#include "audio_capture.h"

#include "measurement.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace click_latency {
namespace {

constexpr int kCallbackFrames = 256;
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

std::string sdl_error(const char* action) {
    const char* detail = SDL_GetError();
    return std::string(action) + (detail && *detail ? ": " : "") +
           (detail ? detail : "");
}

} // namespace

AudioCapture::AudioCapture(Measurement& measurement) : measurement_(measurement) {
    refresh_devices();
}

AudioCapture::~AudioCapture() {
    close();
}

void AudioCapture::refresh_devices() {
    std::vector<InputDevice> refreshed;
    refreshed.push_back({SDL_AUDIO_DEVICE_DEFAULT_RECORDING, "System default microphone"});

    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    if (!ids) {
        error_ = sdl_error("Cannot list recording devices");
    } else {
        for (int index = 0; index < count; ++index) {
            const char* name = SDL_GetAudioDeviceName(ids[index]);
            refreshed.push_back({ids[index], name ? name : "Unnamed recording device"});
        }
        SDL_free(ids);
    }
    devices_ = std::move(refreshed);
}

bool AudioCapture::open(SDL_AudioDeviceID device) {
    close();
    error_.clear();

    SDL_AudioSpec preferred{};
    int preferred_frames = 0;
    // Prefer the source's channel count and rate. Downmixing to mono can cancel
    // a click when stereo microphone channels have opposite polarity.
    if (!SDL_GetAudioDeviceFormat(device, &preferred, &preferred_frames)) {
        error_ = sdl_error("Cannot query microphone format");
        return false;
    }
    if (preferred.channels <= 0 || preferred.channels > 64 || preferred.freq <= 0) {
        error_ = "Microphone reported an unsupported channel count or sample rate";
        return false;
    }

    channels_ = preferred.channels;
    sample_rate_ = preferred.freq;
    sample_buffer_.resize(static_cast<std::size_t>(channels_) * kCallbackFrames);

    SDL_AudioSpec requested{};
    requested.format = SDL_AUDIO_F32;
    requested.channels = channels_;
    requested.freq = sample_rate_;

    // A backend may choose a different buffer size. SDL's environment override
    // remains available to users through SDL_AUDIO_DEVICE_SAMPLE_FRAMES.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256", SDL_HINT_DEFAULT);
    stream_ = SDL_OpenAudioDeviceStream(device, &requested, nullptr, nullptr);
    if (!stream_) {
        error_ = sdl_error("Cannot open microphone");
        close();
        return false;
    }

    selected_device_ = device;
    logical_device_ = SDL_GetAudioStreamDevice(stream_);
    SDL_AudioSpec actual{};
    if (!SDL_GetAudioDeviceFormat(logical_device_, &actual, &device_buffer_frames_)) {
        error_ = sdl_error("Cannot query opened microphone");
        close();
        return false;
    }

    if (!SDL_SetAudioStreamPutCallback(stream_, &AudioCapture::receive_audio, this)) {
        error_ = sdl_error("Cannot configure microphone callback");
        close();
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        error_ = sdl_error("Cannot start microphone capture");
        close();
        return false;
    }
    return true;
}

void AudioCapture::close() {
    if (stream_) {
        // Setting the callback waits for any callback already in progress.
        // Destroy then closes the logical device owned by this convenience stream.
        SDL_SetAudioStreamPutCallback(stream_, nullptr, nullptr);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    selected_device_ = 0;
    logical_device_ = 0;
    sample_rate_ = 0;
    channels_ = 0;
    device_buffer_frames_ = 0;
    callback_error_.store(CallbackError::none, std::memory_order_release);
    sample_buffer_.clear();
    measurement_.reset();
}

bool AudioCapture::active() const noexcept {
    return stream_ && callback_error_.load(std::memory_order_acquire) == CallbackError::none;
}

const std::string& AudioCapture::error() const {
    // Keep allocations and error strings off the audio thread. All control
    // methods, including this accessor, are only used from the GUI thread.
    switch (callback_error_.load(std::memory_order_acquire)) {
    case CallbackError::read_failed:
        error_ = "Microphone capture failed while reading audio; reselect the input device";
        break;
    case CallbackError::invalid_data:
        error_ = "Microphone returned incomplete audio frames; reselect the input device";
        break;
    case CallbackError::processing_failed:
        error_ = "Microphone sample processing failed; reselect the input device";
        break;
    case CallbackError::none:
        break;
    }
    return error_;
}

const char* AudioCapture::driver() const noexcept {
    const char* name = SDL_GetCurrentAudioDriver();
    return name ? name : "unavailable";
}

void SDLCALL AudioCapture::receive_audio(void* userdata, SDL_AudioStream* stream,
                                       int, int) noexcept {
    auto* self = static_cast<AudioCapture*>(userdata);
    if (self->callback_error_.load(std::memory_order_relaxed) != CallbackError::none) {
        // Continue draining after an error instead of letting the stream's
        // internal queue grow while the GUI displays the failure.
        SDL_ClearAudioStream(stream);
        return;
    }
    try {
        self->consume(stream);
    } catch (...) {
        // No C++ exception may cross SDL's C callback boundary.
        self->callback_error_.store(CallbackError::processing_failed, std::memory_order_release);
        SDL_ClearAudioStream(stream);
    }
}

void AudioCapture::consume(SDL_AudioStream* stream) {
    const std::uint64_t callback_time_ns = SDL_GetTicksNS();
    const int frame_bytes = channels_ * static_cast<int>(sizeof(float));
    int available = SDL_GetAudioStreamAvailable(stream);
    if (available < 0) {
        callback_error_.store(CallbackError::read_failed, std::memory_order_release);
        return;
    }

    while (available >= frame_bytes) {
        const int requested_frames = std::min(available / frame_bytes, kCallbackFrames);
        const int bytes = SDL_GetAudioStreamData(stream, sample_buffer_.data(),
                                                requested_frames * frame_bytes);
        if (bytes < 0) {
            callback_error_.store(CallbackError::read_failed, std::memory_order_release);
            return;
        }
        if (bytes == 0) {
            return;
        }
        if (bytes % frame_bytes != 0) {
            callback_error_.store(CallbackError::invalid_data, std::memory_order_release);
            return;
        }

        available -= bytes;
        const std::uint64_t remaining_frames = static_cast<std::uint64_t>(available / frame_bytes);
        const std::uint64_t queued_duration_ns =
            remaining_frames * kNanosecondsPerSecond / static_cast<std::uint64_t>(sample_rate_);
        const std::uint64_t block_end_ns = callback_time_ns > queued_duration_ns
                                             ? callback_time_ns - queued_duration_ns
                                             : 0;

        // SDL supplies no ADC hardware timestamps here. Estimate each block's
        // end from callback arrival and queued sample duration, in the same
        // SDL_GetTicksNS clock as mouse-event timestamps. Driver/ADC buffering
        // before callback delivery remains an uncalibrated measurement offset.
        measurement_.ingest(sample_buffer_.data(), static_cast<std::size_t>(bytes / frame_bytes),
                            channels_, sample_rate_, block_end_ns);
    }
}

} // namespace click_latency
