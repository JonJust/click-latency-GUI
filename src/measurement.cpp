#include "measurement.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace click_latency {
namespace {

constexpr std::size_t index(Edge edge) {
    return edge == Edge::Press ? 0 : 1;
}

} // namespace

Result& Measurement::result_locked(Edge edge) {
    return results_[index(edge)];
}

const Result& Measurement::result_locked(Edge edge) const {
    return results_[index(edge)];
}

void Measurement::reset_locked(bool keep_button_state) {
    results_ = {};
    latest_edge_ = Edge::Press;
    current_db_ = kFloorDb;
    trial_ = 0;
    quiet_active_ = false;
    quiet_start_ns_ = 0;
    armed_ = true;
    recent_events_ = {};
    next_event_ = 0;
    if (!keep_button_state) {
        latest_button_ = {};
        event_sequence_ = 0;
    }
    history_baseline_ = latest_button_;
    // Baseline state is retained for classification, but only recent_events_
    // contains input edges available for matching a new acoustic onset.
}

void Measurement::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_locked();
}

void Measurement::reset_averages() {
    std::lock_guard<std::mutex> lock(mutex_);
    statistics_ = {};
    // An audio block can be queued before the reset and delivered afterward.
    // Do not let its old onset add a sample to the new averaging session.
    audio_cutoff_ns_ = last_observed_ns_;
    reset_locked(true);
}

void Measurement::set_release_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (release_enabled_ != enabled) {
        release_enabled_ = enabled;
        reset_locked(true);
    }
}

void Measurement::set_threshold(float db) {
    if (!std::isfinite(db)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    threshold_db_ = std::clamp(db, kFloorDb, 0.0f);
    threshold_amplitude_ = std::pow(10.0f, threshold_db_ / 20.0f);
    quiet_amplitude_ = std::pow(10.0f, (threshold_db_ - 6.0f) / 20.0f);
    reset_locked();
}

void Measurement::expire_locked(std::uint64_t now_ns) {
    for (auto& result : results_) {
        if (result.phase == Phase::Waiting && now_ns >= result.trigger_ns &&
            now_ns - result.trigger_ns >= kWindowNs) {
            result.phase = Phase::TimedOut;
        }
    }
}

void Measurement::add_sample_locked(Edge edge, double latency_ms) {
    auto& statistics = statistics_[index(edge)];
    ++statistics.count;
    statistics.average_ms += (latency_ms - statistics.average_ms) /
                             static_cast<double>(statistics.count);
    if (statistics.count == 1) {
        statistics.minimum_ms = statistics.maximum_ms = latency_ms;
    } else {
        statistics.minimum_ms = std::min(statistics.minimum_ms, latency_ms);
        statistics.maximum_ms = std::max(statistics.maximum_ms, latency_ms);
    }
    statistics.samples[statistics.next] = static_cast<float>(latency_ms);
    statistics.averages[statistics.next] = static_cast<float>(statistics.average_ms);
    statistics.next = (statistics.next + 1) % kHistoryCapacity;
    statistics.size = std::min(statistics.size + 1, kHistoryCapacity);
}

bool Measurement::accept_locked(Edge edge, std::uint64_t timestamp_ns) {
    auto& result = result_locked(edge);
    if ((result.phase != Phase::Waiting && result.phase != Phase::TimedOut) ||
        timestamp_ns < result.trigger_ns || timestamp_ns - result.trigger_ns > kWindowNs) {
        return false;
    }
    // The event timestamp remains valid even when the UI processes it late.
    result.latency_ms = static_cast<double>(timestamp_ns - result.trigger_ns) / 1'000'000.0;
    result.phase = Phase::Measured;
    add_sample_locked(edge, result.latency_ms);
    return true;
}

Measurement::ButtonState Measurement::button_at_locked(std::uint64_t timestamp_ns) const {
    ButtonState state = history_baseline_;
    for (const auto& event : recent_events_) {
        // At equal timestamps use the state immediately BEFORE that event so
        // an acoustic onset can legitimately measure a zero-millisecond edge.
        if (event.sequence != 0 && event.timestamp < timestamp_ns &&
            (event.timestamp > state.timestamp ||
             (event.timestamp == state.timestamp && event.sequence > state.sequence))) {
            state = {event.edge == Edge::Press, event.timestamp, event.sequence, event.edge};
        }
    }
    return state;
}

void Measurement::record_event_locked(Edge edge, std::uint64_t timestamp_ns) {
    last_observed_ns_ = std::max(last_observed_ns_, timestamp_ns);
    const bool consumed = accept_locked(edge, timestamp_ns);
    const std::uint64_t sequence = ++event_sequence_;
    const auto& evicted = recent_events_[next_event_];
    if (evicted.sequence != 0 &&
        (evicted.timestamp > history_baseline_.timestamp ||
         (evicted.timestamp == history_baseline_.timestamp &&
          evicted.sequence > history_baseline_.sequence))) {
        history_baseline_ = {evicted.edge == Edge::Press, evicted.timestamp,
                             evicted.sequence, evicted.edge};
    }
    recent_events_[next_event_] = {timestamp_ns, sequence, edge, !consumed};
    next_event_ = (next_event_ + 1) % recent_events_.size();
    if (timestamp_ns > latest_button_.timestamp ||
        (timestamp_ns == latest_button_.timestamp && sequence > latest_button_.sequence)) {
        latest_button_ = {edge == Edge::Press, timestamp_ns, sequence, edge};
    }
}

void Measurement::click(std::uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_event_locked(Edge::Press, timestamp_ns);
}

void Measurement::release(std::uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_event_locked(Edge::Release, timestamp_ns);
}

bool Measurement::can_rearm_locked(std::uint64_t timestamp_ns,
                                    const ButtonState& button) const {
    if (!release_enabled_ && button.down) {
        return false;
    }
    const auto& latest = result_locked(latest_edge_);
    if (latest.phase == Phase::Listening) {
        return true;
    }
    if (latest.phase == Phase::Waiting || timestamp_ns < latest.trigger_ns) {
        return false;
    }
    // A successful edge can be followed by the opposite physical edge before
    // its old 80 ms deadline. An uncompleted trial retains its entire window.
    return timestamp_ns - latest.trigger_ns >= kWindowNs ||
           (latest.phase == Phase::Measured &&
            (release_enabled_ || button.timestamp > latest.trigger_ns));
}

bool Measurement::can_trigger_locked(Edge edge, std::uint64_t timestamp_ns,
                                      const ButtonState& button) const {
    if (edge == Edge::Release && !release_enabled_) {
        return false;
    }
    // A release sound arriving just after mouse UP must settle before a new
    // down trial. Historical release events never suppress an older press
    // onset delivered by a delayed audio callback.
    if (edge == Edge::Press && button.edge == Edge::Release && button.sequence != 0 &&
        timestamp_ns >= button.timestamp && timestamp_ns - button.timestamp < kQuietNs) {
        return false;
    }
    const auto& latest = result_locked(latest_edge_);
    if (latest.phase == Phase::Waiting) {
        return false;
    }
    if (latest.phase != Phase::Listening && timestamp_ns >= latest.trigger_ns &&
        timestamp_ns - latest.trigger_ns < kWindowNs && edge == latest_edge_ &&
        button.timestamp <= latest.trigger_ns) {
        return false;
    }
    return true;
}

void Measurement::trigger_locked(Edge edge, std::uint64_t timestamp_ns) {
    auto& result = result_locked(edge);
    result = {Phase::Waiting, 0.0, timestamp_ns, ++trial_};
    latest_edge_ = edge;
    armed_ = false;
    // SDL events may be processed before the audio block containing the sound.
    // Match only the same physical edge and never reuse a consumed input event.
    std::size_t earliest = recent_events_.size();
    std::uint64_t earliest_time = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t earliest_sequence = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < recent_events_.size(); ++i) {
        const auto& event = recent_events_[i];
        if (event.available && event.edge == edge && event.timestamp >= timestamp_ns &&
            event.timestamp - timestamp_ns <= kWindowNs &&
            (event.timestamp < earliest_time ||
             (event.timestamp == earliest_time && event.sequence < earliest_sequence))) {
            earliest = i;
            earliest_time = event.timestamp;
            earliest_sequence = event.sequence;
        }
    }
    if (earliest != recent_events_.size() && accept_locked(edge, earliest_time)) {
        recent_events_[earliest].available = false;
    }
}

void Measurement::ingest(const float* interleaved, std::size_t frames, int channels,
                         int sample_rate, std::uint64_t block_end_ns) {
    if (!interleaved || frames == 0 || channels <= 0 || sample_rate <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    last_observed_ns_ = std::max(last_observed_ns_, block_end_ns);
    float block_peak = 0.0f;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float amplitude = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            const float sample = interleaved[frame * static_cast<std::size_t>(channels) +
                                             static_cast<std::size_t>(channel)];
            if (std::isfinite(sample)) {
                amplitude = std::max(amplitude, std::abs(sample));
            }
        }
        block_peak = std::max(block_peak, amplitude);
        const auto remaining_ns = static_cast<std::uint64_t>(frames - frame - 1) *
                                  1'000'000'000ULL / static_cast<std::uint64_t>(sample_rate);
        const std::uint64_t sample_ns = remaining_ns > block_end_ns
                                            ? 0 : block_end_ns - remaining_ns;
        if ((audio_cutoff_ns_ != 0 && sample_ns <= audio_cutoff_ns_) ||
            sample_ns < history_baseline_.timestamp) {
            continue;
        }
        expire_locked(sample_ns);
        const ButtonState button = button_at_locked(sample_ns);
        if (amplitude <= quiet_amplitude_) {
            if (!quiet_active_) {
                quiet_active_ = true;
                quiet_start_ns_ = sample_ns;
            }
            // After UP, require quiet following the event, rather than using
            // quiet audio that occurred while the mouse was still held down.
            const std::uint64_t quiet_start = !button.down && button.sequence != 0
                                                 ? std::max(quiet_start_ns_, button.timestamp)
                                                 : quiet_start_ns_;
            if (!armed_ && sample_ns >= quiet_start &&
                sample_ns - quiet_start >= kQuietNs && can_rearm_locked(sample_ns, button)) {
                armed_ = true;
            }
        } else {
            quiet_active_ = false;
        }
        if (armed_ && amplitude >= threshold_amplitude_) {
            const Edge edge = button.down ? Edge::Release : Edge::Press;
            if (can_trigger_locked(edge, sample_ns, button)) {
                trigger_locked(edge, sample_ns);
            } else {
                // Suppressed release sounds still require fresh quiet before
                // another onset, but never replace the displayed press result.
                armed_ = false;
            }
        }
    }
    current_db_ = block_peak > 0.0f
                      ? std::clamp(20.0f * std::log10(block_peak), kFloorDb, 0.0f)
                      : kFloorDb;
}

StatsSnapshot Measurement::statistics_snapshot(const Statistics& statistics) {
    StatsSnapshot snapshot;
    snapshot.count = statistics.count;
    snapshot.average_ms = statistics.average_ms;
    snapshot.minimum_ms = statistics.minimum_ms;
    snapshot.maximum_ms = statistics.maximum_ms;
    snapshot.range_ms = statistics.maximum_ms - statistics.minimum_ms;
    snapshot.samples.reserve(statistics.size);
    snapshot.averages.reserve(statistics.size);
    const std::size_t first = (statistics.next + kHistoryCapacity - statistics.size) %
                              kHistoryCapacity;
    for (std::size_t i = 0; i < statistics.size; ++i) {
        const std::size_t position = (first + i) % kHistoryCapacity;
        snapshot.samples.push_back(statistics.samples[position]);
        snapshot.averages.push_back(statistics.averages[position]);
    }
    return snapshot;
}

Snapshot Measurement::snapshot(std::uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_observed_ns_ = std::max(last_observed_ns_, now_ns);
    expire_locked(now_ns);
    const auto& latest = result_locked(latest_edge_);
    Snapshot snapshot;
    snapshot.phase = latest.phase;
    snapshot.current_db = current_db_;
    snapshot.threshold_db = threshold_db_;
    snapshot.latency_ms = latest.latency_ms;
    snapshot.trigger_ns = latest.trigger_ns;
    snapshot.trial = trial_;
    snapshot.armed = armed_;
    snapshot.edge = latest_edge_;
    snapshot.press = result_locked(Edge::Press);
    snapshot.release = result_locked(Edge::Release);
    snapshot.release_enabled = release_enabled_;
    snapshot.button_down = latest_button_.down;
    snapshot.press_stats = statistics_snapshot(statistics_[index(Edge::Press)]);
    snapshot.release_stats = statistics_snapshot(statistics_[index(Edge::Release)]);
    return snapshot;
}

} // namespace click_latency
