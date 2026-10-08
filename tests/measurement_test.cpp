#include "measurement.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using click_latency::Measurement;
using click_latency::Phase;
using click_latency::Edge;

constexpr uint64_t kStart = 1'000'000'000;
constexpr uint64_t ms(uint64_t value) { return value * 1'000'000; }

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 0.0001) {
        throw std::runtime_error(std::string(message) + ": got " +
                                 std::to_string(actual) + ", expected " +
                                 std::to_string(expected));
    }
}

void samples(Measurement& measurement, const std::vector<float>& values,
             uint64_t end_ns, int channels = 1) {
    measurement.ingest(values.data(), values.size() / channels, channels,
                       1000, end_ns);
}

void constant(Measurement& measurement, float value, size_t frame_count,
              uint64_t end_ns) {
    samples(measurement, std::vector<float>(frame_count, value), end_ns);
}

uint64_t trigger(Measurement& measurement) {
    // With a 1 kHz sample rate, this sample spans [kStart, kStart + 1 ms].
    constant(measurement, 0.5f, 1, kStart + ms(1));
    const auto state = measurement.snapshot(kStart + ms(1));
    require(state.phase == Phase::Waiting, "loud sample should begin a trial");
    return state.trigger_ns;
}

void defaults_and_silence() {
    Measurement measurement;
    auto state = measurement.snapshot(kStart);
    require(state.phase == Phase::Listening, "initial phase should listen");
    require(state.armed, "initial trial should be armed");
    near(state.threshold_db, -30.0, "default threshold");
    near(state.current_db, -96.0, "initial meter floor");
    require(state.trial == 0, "no initial trial");

    constant(measurement, 0.0f, 100, kStart + ms(100));
    state = measurement.snapshot(kStart + ms(100));
    require(state.phase == Phase::Listening, "silence must not create a trial");
    near(state.current_db, -96.0, "silence should use the meter floor");
}

void below_and_exact_threshold() {
    Measurement measurement;
    measurement.set_threshold(0.0f);
    constant(measurement, 0.99f, 1, kStart + ms(1));
    require(measurement.snapshot(kStart + ms(1)).phase == Phase::Listening,
            "signal below threshold must not trigger");

    constant(measurement, 1.0f, 1, kStart + ms(2));
    const auto state = measurement.snapshot(kStart + ms(2));
    require(state.phase == Phase::Waiting, "exact threshold must trigger");
    require(state.trigger_ns == kStart + ms(2), "exact crossing timestamp");
}

void crossing_uses_sample_position() {
    Measurement measurement;
    measurement.set_threshold(-20.0f);
    samples(measurement, {0.0f, 0.0f, 0.5f, 0.0f, 0.0f}, kStart + ms(5));
    const auto state = measurement.snapshot(kStart + ms(5));
    require(state.phase == Phase::Waiting, "short transient should trigger");
    require(state.trigger_ns == kStart + ms(3),
            "onset must use first loud sample, not callback end");
    measurement.click(kStart + ms(13));
    near(measurement.snapshot(kStart + ms(13)).latency_ms, 10.0,
         "latency should start at the transient sample");
}

void click_measures_elapsed_time() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(12));
    const auto state = measurement.snapshot(onset + ms(12));
    require(state.phase == Phase::Measured, "valid click should be measured");
    near(state.latency_ms, 12.0, "measured click latency");
    require(!state.armed, "measured trial must wait for rearm");
}

void first_accepted_press_persists() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    measurement.click(onset + ms(10));
    const auto state = measurement.snapshot(onset + ms(15));
    require(state.phase == Phase::Measured, "further presses should retain result");
    near(state.latency_ms, 5.0, "first accepted press should determine result");
}

void press_before_sound_is_ignored() {
    Measurement measurement;
    measurement.click(kStart - ms(30));
    measurement.release(kStart - ms(25));
    const uint64_t onset = trigger(measurement);
    require(measurement.snapshot(onset).phase == Phase::Waiting,
            "press before onset must not produce a negative latency");
    measurement.click(onset + ms(3));
    const auto state = measurement.snapshot(onset + ms(3));
    require(state.phase == Phase::Measured, "later valid press should measure");
    near(state.latency_ms, 3.0, "earlier press must not poison the trial");
}

void exact_80_ms_is_valid() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + Measurement::kWindowNs);
    const auto state = measurement.snapshot(onset + Measurement::kWindowNs);
    require(state.phase == Phase::Measured, "80 ms boundary must be accepted");
    near(state.latency_ms, 80.0, "80 ms boundary latency");
}

void timeout_accepts_delayed_in_window_event() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    require(measurement.snapshot(onset + ms(90)).phase == Phase::TimedOut,
            "pending trial must expire after 80 ms");
    // The event can be delivered late, but its timestamp is still valid.
    measurement.click(onset + Measurement::kWindowNs);
    const auto state = measurement.snapshot(onset + ms(100));
    require(state.phase == Phase::Measured,
            "late delivery must preserve an in-window event timestamp");
    near(state.latency_ms, 80.0, "late-delivered boundary latency");
}

void after_80_ms_is_timeout() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + Measurement::kWindowNs + 1);
    require(measurement.snapshot(onset + ms(81)).phase == Phase::TimedOut,
            "press one nanosecond after the window must not measure");
}

void click_history_handles_delayed_audio() {
    Measurement measurement;
    // Main-thread input may arrive before a delayed capture callback.
    measurement.click(kStart + ms(12));
    samples(measurement, {0.0f, 0.5f, 0.0f, 0.0f, 0.0f}, kStart + ms(5));
    const auto state = measurement.snapshot(kStart + ms(12));
    require(state.phase == Phase::Measured,
            "recorded press should match an audio onset delivered later");
    near(state.latency_ms, 10.0, "history matched to delayed audio onset");
}

void history_uses_earliest_valid_press() {
    Measurement measurement;
    // Arrival order must not select a later timestamp from the history.
    measurement.click(kStart + ms(21));
    measurement.click(kStart + ms(11));
    measurement.click(kStart + ms(31));
    constant(measurement, 0.5f, 1, kStart + ms(1));
    const auto state = measurement.snapshot(kStart + ms(31));
    require(state.phase == Phase::Measured, "history should produce a result");
    near(state.latency_ms, 10.0, "history should use earliest valid timestamp");
}

void stereo_peak_does_not_cancel() {
    Measurement measurement;
    measurement.set_threshold(-10.0f);
    samples(measurement, {0.5f, -0.5f}, kStart + ms(1), 2);
    const auto state = measurement.snapshot(kStart + ms(1));
    require(state.phase == Phase::Waiting,
            "opposite stereo channels must not cancel a transient");
    near(state.current_db, 20.0 * std::log10(0.5), "stereo peak meter");
}

void waiting_trial_is_not_overwritten() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    constant(measurement, 0.8f, 30, onset + ms(30));
    auto state = measurement.snapshot(onset + ms(30));
    require(state.phase == Phase::Waiting, "same trial should remain pending");
    require(state.trigger_ns == onset, "later sound must not move the onset");
    require(state.trial == 1, "loud continuation must not create another trial");
    measurement.click(onset + ms(40));
    state = measurement.snapshot(onset + ms(40));
    near(state.latency_ms, 40.0, "original onset should determine latency");
}

void result_persists_without_quiet_rearm() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    constant(measurement, 0.8f, 120, onset + ms(120));
    const auto state = measurement.snapshot(onset + ms(120));
    require(state.phase == Phase::Measured, "result should persist in noise");
    require(state.trial == 1, "continuous noise should not retrigger");
    require(!state.armed, "continuous noise must not rearm");
    near(state.latency_ms, 5.0, "persistent result latency");
}

void quiet_cannot_rearm_inside_window() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    constant(measurement, 0.0f, 25, onset + ms(25));
    const auto state = measurement.snapshot(onset + ms(25));
    require(!state.armed, "quiet must also wait for previous 80 ms window");
    require(state.phase == Phase::Measured, "quiet should preserve the result");
}

void hysteresis_and_quiet_duration_allow_next_trial() {
    Measurement measurement;
    measurement.set_threshold(-20.0f);
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    measurement.release(onset + ms(6));
    constant(measurement, 0.5f, 80, onset + ms(80));
    // 0.08 is below the trigger threshold (0.1), but above the quiet
    // hysteresis level (~0.05). It must not count as quiet.
    constant(measurement, 0.08f, 40, onset + ms(120));
    require(!measurement.snapshot(onset + ms(120)).armed,
            "signal within hysteresis band must not rearm");

    constant(measurement, 0.0f, 19, onset + ms(139));
    require(!measurement.snapshot(onset + ms(139)).armed,
            "less than 20 ms quiet must not rearm");
    constant(measurement, 0.0f, 2, onset + ms(141));
    auto state = measurement.snapshot(onset + ms(141));
    require(state.armed, "sufficient quiet after window must rearm");
    require(state.phase == Phase::Measured, "rearm should retain old result");

    constant(measurement, 0.5f, 1, onset + ms(142));
    state = measurement.snapshot(onset + ms(142));
    require(state.phase == Phase::Waiting, "next sound should begin a new trial");
    require(state.trial == 2, "next onset should increment the trial");
    require(state.trigger_ns == onset + ms(142), "new trial onset");
}

void reset_clears_results_and_history() {
    Measurement measurement;
    measurement.set_threshold(-20.0f);
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    measurement.reset();
    auto state = measurement.snapshot(onset + ms(10));
    require(state.phase == Phase::Listening && state.armed,
            "reset should listen immediately");
    near(state.threshold_db, -20.0, "reset should preserve threshold");
    near(state.current_db, -96.0, "reset should clear meter");
    require(state.trial == 0 && state.trigger_ns == 0,
            "reset should clear trial state");

    // This onset precedes the old click; cleared history must not match it.
    constant(measurement, 0.5f, 1, onset + ms(2));
    state = measurement.snapshot(onset + ms(10));
    require(state.phase == Phase::Waiting, "reset must clear recent click history");
}

void threshold_change_resets_pending_trial_and_history() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(20));
    measurement.set_threshold(0.0f);
    auto state = measurement.snapshot(onset + ms(30));
    require(state.phase == Phase::Listening && state.armed,
            "threshold change should reset trial");
    near(state.threshold_db, 0.0, "updated threshold");
    constant(measurement, 1.0f, 1, onset + ms(10));
    state = measurement.snapshot(onset + ms(30));
    require(state.phase == Phase::Waiting,
            "threshold change must discard recent click history");
}

void invalid_arguments_and_nonfinite_audio_are_safe() {
    Measurement measurement;
    const float loud = 1.0f;
    measurement.ingest(nullptr, 1, 1, 1000, kStart);
    measurement.ingest(&loud, 0, 1, 1000, kStart);
    measurement.ingest(&loud, 1, 0, 1000, kStart);
    measurement.ingest(&loud, 1, 1, 0, kStart);
    measurement.set_threshold(std::numeric_limits<float>::quiet_NaN());
    measurement.set_threshold(std::numeric_limits<float>::infinity());
    auto state = measurement.snapshot(kStart);
    require(state.phase == Phase::Listening && state.armed,
            "invalid arguments must not trigger a trial");
    near(state.threshold_db, -30.0, "invalid threshold should be ignored");
    near(state.current_db, -96.0, "invalid capture must preserve meter");

    samples(measurement,
            {std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity()}, kStart + ms(3));
    state = measurement.snapshot(kStart + ms(3));
    require(state.phase == Phase::Listening,
            "nonfinite audio samples must not trigger");
    near(state.current_db, -96.0, "nonfinite samples should not poison meter");

    samples(measurement, {std::numeric_limits<float>::quiet_NaN(), 0.5f},
            kStart + ms(4), 2);
    require(measurement.snapshot(kStart + ms(4)).phase == Phase::Waiting,
            "valid stereo channel should still trigger beside a NaN");
}

void measured_press(Measurement& measurement, uint64_t base,
                    uint64_t latency_ns) {
    constant(measurement, 0.5f, 1, base + ms(1));
    measurement.click(base + ms(1) + latency_ns);
    measurement.release(base + ms(90));
    constant(measurement, 0.0f, 30, base + ms(130));
}

void measured_cycle(Measurement& measurement, uint64_t base,
                    uint64_t press_ms, uint64_t release_ms) {
    constant(measurement, 0.5f, 1, base + ms(1));
    measurement.click(base + ms(1 + press_ms));
    constant(measurement, 0.0f, 25, base + ms(32));
    constant(measurement, 0.5f, 1, base + ms(40));
    measurement.release(base + ms(40 + release_ms));
    constant(measurement, 0.0f, 30, base + ms(150));
}

void release_is_disabled_by_default() {
    Measurement measurement;
    const auto state = measurement.snapshot(kStart);
    require(!state.release_enabled, "release measurement should be opt-in");
    require(!state.button_down, "initial mouse state should be released");
    require(state.press.phase == Phase::Listening &&
                state.release.phase == Phase::Listening,
            "both result fields should begin empty");
    require(state.press_stats.count == 0 && state.release_stats.count == 0,
            "both averages should begin empty");
}

void press_and_release_have_separate_results() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(7));
    constant(measurement, 0.0f, 25, onset + ms(31));
    constant(measurement, 0.5f, 1, onset + ms(39));
    auto state = measurement.snapshot(onset + ms(39));
    require(state.edge == Edge::Release && state.release.phase == Phase::Waiting,
            "held button should classify next quiet-separated sound as release");
    require(state.press.phase == Phase::Measured,
            "waiting release must preserve the measured press");
    near(state.press.latency_ms, 7.0, "separate press result");

    measurement.release(onset + ms(48));
    state = measurement.snapshot(onset + ms(48));
    require(state.release.phase == Phase::Measured && !state.button_down,
            "mouse-up should complete the release trial");
    near(state.release.latency_ms, 9.0, "release latency starts at release sound");
    require(state.press_stats.count == 1 && state.release_stats.count == 1,
            "each edge should contribute one independent sample");
}

void disabled_release_sound_does_not_report_a_missed_press() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    constant(measurement, 0.0f, 100, onset + ms(110));
    constant(measurement, 0.5f, 1, onset + ms(120));
    measurement.release(onset + ms(125));
    auto state = measurement.snapshot(onset + ms(220));
    require(state.press.phase == Phase::Measured,
            "release noise must not replace measured press with timeout");
    near(state.press.latency_ms, 5.0, "release-off should preserve press latency");
    require(state.trial == 1 && state.press_stats.count == 1 &&
                state.release_stats.count == 0,
            "release-off should not create a trial or sample for release noise");

    constant(measurement, 0.0f, 30, onset + ms(160));
    constant(measurement, 0.5f, 1, onset + ms(170));
    measurement.click(onset + ms(177));
    state = measurement.snapshot(onset + ms(177));
    require(state.press.phase == Phase::Measured && state.press_stats.count == 2,
            "next physical press should still be measured after release suppression");
    near(state.press.latency_ms, 7.0, "next physical press latency");
}

void disabled_release_is_suppressed_with_delayed_audio() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    constant(measurement, 0.0f, 100, onset + ms(110));
    // The UI knows the button is up before this callback delivers its sound.
    measurement.release(onset + ms(125));
    constant(measurement, 0.5f, 1, onset + ms(120));
    const auto state = measurement.snapshot(onset + ms(220));
    require(state.press.phase == Phase::Measured && state.trial == 1,
            "timestamped held state should suppress delayed release sound");
    require(state.press_stats.count == 1,
            "suppressed delayed release must not add a press sample");
}

void release_after_mouse_up_is_suppressed() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    constant(measurement, 0.0f, 100, onset + ms(110));
    measurement.release(onset + ms(119));
    constant(measurement, 0.5f, 1, onset + ms(120));
    const auto state = measurement.snapshot(onset + ms(220));
    require(state.press.phase == Phase::Measured && state.trial == 1,
            "release sound shortly after mouse-up should not become a missed press");
}

void delayed_callbacks_use_button_state_at_each_sound() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    measurement.click(kStart + ms(8));
    measurement.release(kStart + ms(49));
    // Both mouse events arrived before either acoustic callback.
    constant(measurement, 0.5f, 1, kStart + ms(1));
    constant(measurement, 0.0f, 25, kStart + ms(32));
    constant(measurement, 0.5f, 1, kStart + ms(40));
    const auto state = measurement.snapshot(kStart + ms(50));
    require(state.press.phase == Phase::Measured &&
                state.release.phase == Phase::Measured,
            "delayed callbacks should match both typed input events");
    near(state.press.latency_ms, 7.0, "historical press classification");
    near(state.release.latency_ms, 9.0, "historical release classification");
    require(state.press_stats.count == 1 && state.release_stats.count == 1,
            "historical cycle should add each sample once");
}

void release_has_its_own_exact_80_ms_window() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    measured_cycle(measurement, kStart, 5, 80);
    const auto state = measurement.snapshot(kStart + ms(150));
    require(state.press.phase == Phase::Measured &&
                state.release.phase == Phase::Measured,
            "release window should be independent of earlier press window");
    near(state.release.latency_ms, 80.0, "release exact 80 ms boundary");
    require(state.release_stats.count == 1,
            "release boundary should contribute a valid sample");
}

void equal_sound_and_input_timestamps_measure_zero() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    // Historical classification must use the state before an equal-time event.
    measurement.click(kStart + ms(1));
    measurement.release(kStart + ms(40));
    constant(measurement, 0.5f, 1, kStart + ms(1));
    constant(measurement, 0.0f, 25, kStart + ms(32));
    constant(measurement, 0.5f, 1, kStart + ms(40));
    const auto state = measurement.snapshot(kStart + ms(41));
    require(state.press.phase == Phase::Measured &&
                state.release.phase == Phase::Measured,
            "equal timestamps should produce valid zero-latency edges");
    near(state.press.latency_ms, 0.0, "zero-latency press");
    near(state.release.latency_ms, 0.0, "zero-latency release");
    require(state.press_stats.count == 1 && state.release_stats.count == 1,
            "zero latency should count as a successful measurement");
}

void release_requires_a_quiet_interval() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(5));
    constant(measurement, 0.0f, 19, onset + ms(24));
    constant(measurement, 0.5f, 1, onset + ms(29));
    auto state = measurement.snapshot(onset + ms(29));
    require(state.release.phase == Phase::Listening,
            "press ring-down before 20 ms quiet should not become a release trial");
    constant(measurement, 0.0f, 21, onset + ms(51));
    constant(measurement, 0.5f, 1, onset + ms(54));
    measurement.release(onset + ms(59));
    state = measurement.snapshot(onset + ms(59));
    require(state.release.phase == Phase::Measured,
            "release sound should be accepted after enough quiet");
    near(state.release.latency_ms, 5.0, "quiet-separated release latency");
}

void wrong_input_edge_cannot_complete_a_trial() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    const uint64_t onset = trigger(measurement);
    measurement.release(onset + ms(3));
    auto state = measurement.snapshot(onset + ms(3));
    require(state.press.phase == Phase::Waiting && state.press_stats.count == 0,
            "mouse-up must not complete a press trial");
    measurement.click(onset + ms(5));
    constant(measurement, 0.0f, 25, onset + ms(31));
    constant(measurement, 0.5f, 1, onset + ms(39));
    measurement.click(onset + ms(43));
    state = measurement.snapshot(onset + ms(43));
    require(state.release.phase == Phase::Waiting && state.release_stats.count == 0,
            "mouse-down must not complete a release trial");
    measurement.release(onset + ms(48));
    state = measurement.snapshot(onset + ms(48));
    require(state.release.phase == Phase::Measured,
            "matching mouse-up should complete the release trial");
}

void missed_press_can_have_a_valid_release() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + ms(90));
    constant(measurement, 0.0f, 30, onset + ms(115));
    constant(measurement, 0.5f, 1, onset + ms(120));
    measurement.release(onset + ms(130));
    const auto state = measurement.snapshot(onset + ms(130));
    require(state.press.phase == Phase::TimedOut &&
                state.release.phase == Phase::Measured,
            "release should get an independent trial after a missed press");
    near(state.release.latency_ms, 10.0, "valid release following missed press");
    require(state.press_stats.count == 0 && state.release_stats.count == 1,
            "only valid release should contribute to statistics");
}

void release_timeout_excludes_missed_sample() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    constant(measurement, 0.5f, 1, kStart + ms(1));
    measurement.click(kStart + ms(6));
    constant(measurement, 0.0f, 25, kStart + ms(32));
    constant(measurement, 0.5f, 1, kStart + ms(40));
    measurement.release(kStart + ms(120) + 1);
    const auto state = measurement.snapshot(kStart + ms(121));
    require(state.release.phase == Phase::TimedOut,
            "release after its own 80 ms window should time out");
    require(state.press.phase == Phase::Measured,
            "missed release should preserve valid press result");
    require(state.press_stats.count == 1 && state.release_stats.count == 0,
            "missed release should be excluded from averages");
}

void release_timeout_accepts_delayed_valid_timestamp() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    constant(measurement, 0.5f, 1, kStart + ms(1));
    measurement.click(kStart + ms(6));
    constant(measurement, 0.0f, 25, kStart + ms(32));
    constant(measurement, 0.5f, 1, kStart + ms(40));
    require(measurement.snapshot(kStart + ms(130)).release.phase == Phase::TimedOut,
            "pending release should expire independently");
    measurement.release(kStart + ms(120));
    const auto state = measurement.snapshot(kStart + ms(140));
    require(state.release.phase == Phase::Measured,
            "delayed in-window release timestamp should recover result");
    near(state.release.latency_ms, 80.0, "delayed release boundary latency");
    require(state.release_stats.count == 1,
            "recovered release result should add a sample once");
}

void successful_samples_have_separate_statistics() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    measured_cycle(measurement, kStart, 4, 3);
    measured_cycle(measurement, kStart + ms(200), 8, 7);
    measured_cycle(measurement, kStart + ms(400), 12, 11);
    const auto state = measurement.snapshot(kStart + ms(600));
    require(state.press_stats.count == 3 && state.release_stats.count == 3,
            "series should count successful samples independently");
    near(state.press_stats.average_ms, 8.0, "press average");
    near(state.press_stats.minimum_ms, 4.0, "press minimum");
    near(state.press_stats.maximum_ms, 12.0, "press maximum");
    near(state.press_stats.range_ms, 8.0, "press range is maximum minus minimum");
    near(state.release_stats.average_ms, 7.0, "release average");
    near(state.release_stats.minimum_ms, 3.0, "release minimum");
    near(state.release_stats.maximum_ms, 11.0, "release maximum");
    near(state.release_stats.range_ms, 8.0, "release range");
    require(state.press_stats.samples.size() == 3 &&
                state.press_stats.averages.size() == 3,
            "graph should retain sample and average values together");
    near(state.press_stats.samples[0], 4.0, "first graph sample");
    near(state.press_stats.samples[2], 12.0, "last graph sample");
    near(state.press_stats.averages[0], 4.0, "first cumulative average");
    near(state.press_stats.averages[1], 6.0, "second cumulative average");
    near(state.press_stats.averages[2], 8.0, "third cumulative average");
}

void repeated_input_and_snapshot_do_not_duplicate_samples() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    measured_cycle(measurement, kStart, 5, 9);
    measurement.click(kStart + ms(6));
    measurement.release(kStart + ms(49));
    measurement.release(kStart + ms(55));
    for (int i = 0; i < 10; ++i) {
        const auto state = measurement.snapshot(kStart + ms(200 + i));
        require(state.press_stats.count == 1 && state.release_stats.count == 1,
                "completed trial should contribute at most one sample per edge");
    }
}

void graph_history_is_bounded_but_averages_are_cumulative() {
    Measurement measurement;
    constexpr size_t total = Measurement::kHistoryCapacity + 44;
    for (size_t i = 0; i < total; ++i) {
        // 0.1 through 30 ms, all inside the timeout window.
        measured_press(measurement, kStart + ms(i * 200), (i + 1) * 100'000);
    }
    const auto state = measurement.snapshot(kStart + ms(total * 200));
    const auto& stats = state.press_stats;
    require(stats.count == total, "cumulative count must exceed graph capacity");
    require(stats.samples.size() == Measurement::kHistoryCapacity &&
                stats.averages.size() == Measurement::kHistoryCapacity,
            "both graph series must be bounded to 256 values");
    const double first = static_cast<double>(total - Measurement::kHistoryCapacity + 1) / 10.0;
    near(stats.samples.front(), first, "oldest retained sample chronology");
    near(stats.samples.back(), static_cast<double>(total) / 10.0,
         "newest retained sample chronology");
    near(stats.averages.front(), (first + 0.1) / 2.0,
         "retained average should include samples older than the graph");
    near(stats.average_ms, static_cast<double>(total + 1) / 20.0,
         "average should include every sample since reset");
    near(stats.minimum_ms, 0.1, "minimum should survive graph eviction");
    near(stats.maximum_ms, static_cast<double>(total) / 10.0,
         "maximum across cumulative sample history");
}

void reset_averages_clears_pending_history_and_preserves_settings() {
    Measurement measurement;
    measurement.set_threshold(-20.0f);
    measurement.set_release_enabled(true);
    measured_cycle(measurement, kStart, 5, 9);
    measurement.click(kStart + ms(210));
    measurement.reset_averages();
    auto state = measurement.snapshot(kStart + ms(220));
    require(state.press_stats.count == 0 && state.release_stats.count == 0 &&
                state.press_stats.samples.empty() && state.release_stats.samples.empty(),
            "reset averages should clear both cumulative statistics and graphs");
    require(state.release_enabled, "reset averages should preserve release setting");
    near(state.threshold_db, -20.0, "reset averages should preserve threshold");
    require(state.phase == Phase::Listening && state.armed && state.button_down,
            "reset averages should clear trials while preserving physical button state");

    // Without cleared input history, the old 210 ms press would match this onset.
    constant(measurement, 0.5f, 1, kStart + ms(205));
    state = measurement.snapshot(kStart + ms(220));
    require(state.phase == Phase::Listening && state.press_stats.count == 0,
            "reset averages should exclude pre-reset queued audio and input history");
    constant(measurement, 0.5f, 1, kStart + ms(230));
    measurement.release(kStart + ms(237));
    state = measurement.snapshot(kStart + ms(237));
    require(state.press_stats.count == 0 && state.release_stats.count == 1,
            "fresh release after averages reset should start a new independent count");
    near(state.release_stats.average_ms, 7.0, "fresh average after reset");
    near(state.release_stats.range_ms, 0.0, "single sample has zero range");
}

void result_reset_threshold_and_toggle_preserve_averages() {
    Measurement measurement;
    measurement.set_release_enabled(true);
    measured_cycle(measurement, kStart, 5, 9);
    measurement.reset();
    auto state = measurement.snapshot(kStart + ms(160));
    require(state.press_stats.count == 1 && state.release_stats.count == 1,
            "result reset should preserve cumulative samples");
    require(state.release_enabled && state.press.phase == Phase::Listening &&
                state.release.phase == Phase::Listening,
            "result reset should preserve option but clear both results");

    measurement.set_threshold(-18.0f);
    state = measurement.snapshot(kStart + ms(170));
    require(state.press_stats.count == 1 && state.release_stats.count == 1,
            "threshold change should preserve samples");
    near(state.press_stats.average_ms, 5.0, "preserved press average");
    near(state.release_stats.average_ms, 9.0, "preserved release average");
    measurement.click(kStart + ms(180));
    measurement.set_release_enabled(false);
    state = measurement.snapshot(kStart + ms(185));
    require(!state.release_enabled && state.button_down,
            "release toggle should preserve physical state while resetting trials");
    require(state.press_stats.count == 1 && state.release_stats.count == 1,
            "release toggle should retain both cumulative series");
    measurement.release(kStart + ms(190));
    constant(measurement, 0.0f, 21, kStart + ms(212));
    constant(measurement, 0.5f, 1, kStart + ms(215));
    state = measurement.snapshot(kStart + ms(215));
    require(state.phase == Phase::Waiting,
            "release toggle should allow a new physical press after quiet");
}

void reset_averages_cancels_an_outstanding_acoustic_trial() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.reset_averages();
    measurement.click(onset + ms(5));
    auto state = measurement.snapshot(onset + ms(6));
    require(state.press.phase == Phase::Listening && state.press_stats.count == 0,
            "old pending trial must not add a sample after averages reset");
    constant(measurement, 0.5f, 1, onset);
    state = measurement.snapshot(onset + ms(10));
    require(state.press.phase == Phase::Listening && state.press_stats.count == 0,
            "queued old sound must not recreate a canceled pending trial");
}

void press_timeout_is_excluded_from_statistics() {
    Measurement measurement;
    const uint64_t onset = trigger(measurement);
    measurement.click(onset + Measurement::kWindowNs + 1);
    const auto state = measurement.snapshot(onset + ms(100));
    require(state.press.phase == Phase::TimedOut && state.press_stats.count == 0,
            "missed press should not become a latency sample");
    require(state.press_stats.samples.empty() && state.press_stats.averages.empty(),
            "missed press should not appear on graphs");
}

}  // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"defaults and silence", defaults_and_silence},
        {"below and exact threshold", below_and_exact_threshold},
        {"sample position onset", crossing_uses_sample_position},
        {"click elapsed time", click_measures_elapsed_time},
        {"first accepted press", first_accepted_press_persists},
        {"press before sound", press_before_sound_is_ignored},
        {"exact 80 ms boundary", exact_80_ms_is_valid},
        {"late event delivery", timeout_accepts_delayed_in_window_event},
        {"after 80 ms timeout", after_80_ms_is_timeout},
        {"delayed audio callback", click_history_handles_delayed_audio},
        {"earliest historical press", history_uses_earliest_valid_press},
        {"stereo peak", stereo_peak_does_not_cancel},
        {"pending trial stability", waiting_trial_is_not_overwritten},
        {"persistent result", result_persists_without_quiet_rearm},
        {"window gates rearm", quiet_cannot_rearm_inside_window},
        {"hysteresis and quiet rearm", hysteresis_and_quiet_duration_allow_next_trial},
        {"reset", reset_clears_results_and_history},
        {"threshold reset", threshold_change_resets_pending_trial_and_history},
        {"invalid capture and nonfinite samples", invalid_arguments_and_nonfinite_audio_are_safe},
        {"release opt-in default", release_is_disabled_by_default},
        {"separate press and release results", press_and_release_have_separate_results},
        {"release-off sound suppression", disabled_release_sound_does_not_report_a_missed_press},
        {"release-off delayed audio suppression", disabled_release_is_suppressed_with_delayed_audio},
        {"release sound after mouse-up", release_after_mouse_up_is_suppressed},
        {"historical button state for delayed callbacks", delayed_callbacks_use_button_state_at_each_sound},
        {"independent release 80 ms boundary", release_has_its_own_exact_80_ms_window},
        {"zero-latency typed edges", equal_sound_and_input_timestamps_measure_zero},
        {"quiet interval before release", release_requires_a_quiet_interval},
        {"typed input matches matching edge", wrong_input_edge_cannot_complete_a_trial},
        {"missed press and valid release", missed_press_can_have_a_valid_release},
        {"release timeout excludes sample", release_timeout_excludes_missed_sample},
        {"delayed in-window release", release_timeout_accepts_delayed_valid_timestamp},
        {"separate sample statistics", successful_samples_have_separate_statistics},
        {"no duplicate completed samples", repeated_input_and_snapshot_do_not_duplicate_samples},
        {"bounded graph with cumulative averages", graph_history_is_bounded_but_averages_are_cumulative},
        {"averages reset clears pending history", reset_averages_clears_pending_history_and_preserves_settings},
        {"result and settings changes retain averages", result_reset_threshold_and_toggle_preserve_averages},
        {"averages reset cancels pending trial", reset_averages_cancels_an_outstanding_acoustic_trial},
        {"press timeout excludes sample", press_timeout_is_excluded_from_statistics},
    };

    size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << (std::size(tests) - failures) << '/' << std::size(tests)
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
