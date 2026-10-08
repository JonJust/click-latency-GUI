#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace click_latency {

enum class Phase { Listening, Waiting, Measured, TimedOut };
enum class Edge { Press, Release };

struct Result {
    Phase phase = Phase::Listening;
    double latency_ms = 0.0;
    std::uint64_t trigger_ns = 0;
    std::uint64_t trial = 0;
};

struct StatsSnapshot {
    std::uint64_t count = 0;
    double average_ms = 0.0;
    double minimum_ms = 0.0;
    double maximum_ms = 0.0;
    double range_ms = 0.0;
    // Oldest to newest, limited to the most recent kHistoryCapacity successes.
    // Each average is the cumulative average at that sample, since the reset.
    std::vector<float> samples;
    std::vector<float> averages;
};

struct Snapshot {
    // The original fields describe the most recent acoustic trial.
    Phase phase = Phase::Listening;
    float current_db = -96.0f;
    float threshold_db = -30.0f;
    double latency_ms = 0.0;
    std::uint64_t trigger_ns = 0;
    std::uint64_t trial = 0;
    bool armed = true;
    Edge edge = Edge::Press;
    Result press;
    Result release;
    bool release_enabled = false;
    bool button_down = false;
    StatsSnapshot press_stats;
    StatsSnapshot release_stats;
};

// All timestamps use SDL's monotonic nanosecond clock. The detector itself has
// no SDL dependency, so timing and audio analysis can be tested deterministically.
class Measurement {
public:
    static constexpr std::uint64_t kWindowNs = 80'000'000;
    static constexpr std::uint64_t kQuietNs = 20'000'000;
    static constexpr std::size_t kHistoryCapacity = 256;
    static constexpr float kFloorDb = -96.0f;

    void set_threshold(float db);
    void set_release_enabled(bool enabled);
    // Cancel trial and input state while retaining the accumulated statistics.
    void reset();
    // Start a fresh averaging session and cancel any outstanding trials.
    void reset_averages();
    void ingest(const float* interleaved, std::size_t frames, int channels,
                int sample_rate, std::uint64_t block_end_ns);
    void click(std::uint64_t timestamp_ns);
    void release(std::uint64_t timestamp_ns);
    Snapshot snapshot(std::uint64_t now_ns);

private:
    struct InputEvent {
        std::uint64_t timestamp = 0;
        std::uint64_t sequence = 0;
        Edge edge = Edge::Press;
        bool available = false;
    };

    struct Statistics {
        std::uint64_t count = 0;
        double average_ms = 0.0;
        double minimum_ms = 0.0;
        double maximum_ms = 0.0;
        std::array<float, kHistoryCapacity> samples{};
        std::array<float, kHistoryCapacity> averages{};
        std::size_t next = 0;
        std::size_t size = 0;
    };

    struct ButtonState {
        bool down = false;
        std::uint64_t timestamp = 0;
        std::uint64_t sequence = 0;
        Edge edge = Edge::Release;
    };

    void reset_locked(bool keep_button_state = false);
    void expire_locked(std::uint64_t now_ns);
    bool accept_locked(Edge edge, std::uint64_t timestamp_ns);
    void record_event_locked(Edge edge, std::uint64_t timestamp_ns);
    void trigger_locked(Edge edge, std::uint64_t timestamp_ns);
    bool can_rearm_locked(std::uint64_t timestamp_ns, const ButtonState& button) const;
    bool can_trigger_locked(Edge edge, std::uint64_t timestamp_ns,
                            const ButtonState& button) const;
    ButtonState button_at_locked(std::uint64_t timestamp_ns) const;
    Result& result_locked(Edge edge);
    const Result& result_locked(Edge edge) const;
    void add_sample_locked(Edge edge, double latency_ms);
    static StatsSnapshot statistics_snapshot(const Statistics& statistics);

    std::mutex mutex_;
    std::array<Result, 2> results_{};
    std::array<Statistics, 2> statistics_{};
    Edge latest_edge_ = Edge::Press;
    float threshold_db_ = -30.0f;
    float threshold_amplitude_ = 0.0316227766f;
    float quiet_amplitude_ = 0.0158489319f;
    float current_db_ = kFloorDb;
    std::uint64_t trial_ = 0;
    std::uint64_t quiet_start_ns_ = 0;
    std::uint64_t last_observed_ns_ = 0;
    std::uint64_t audio_cutoff_ns_ = 0;
    bool quiet_active_ = false;
    bool armed_ = true;
    bool release_enabled_ = false;
    ButtonState latest_button_{};
    ButtonState history_baseline_{};
    std::array<InputEvent, 128> recent_events_{};
    std::size_t next_event_ = 0;
    std::uint64_t event_sequence_ = 0;
};

} // namespace click_latency
