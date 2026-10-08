#pragma once

#include <SDL3/SDL_audio.h>

#include <atomic>
#include <string>
#include <vector>

namespace click_latency {

class Measurement;

struct InputDevice {
    SDL_AudioDeviceID id;
    std::string name;
};

// Control methods are called from the GUI thread. SDL delivers samples on its
// audio thread; stopping or changing devices waits for that callback to finish.
class AudioCapture {
public:
    explicit AudioCapture(Measurement& measurement);
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    void refresh_devices();
    const std::vector<InputDevice>& devices() const noexcept { return devices_; }

    bool open(SDL_AudioDeviceID device);
    void close();

    bool active() const noexcept;
    SDL_AudioDeviceID selected_device() const noexcept { return selected_device_; }
    // Compare removal events with both the requested physical device and this
    // logical device. The system-default entry can migrate between devices.
    SDL_AudioDeviceID logical_device() const noexcept { return logical_device_; }

    const std::string& error() const;
    int sample_rate() const noexcept { return sample_rate_; }
    int buffer_frames() const noexcept { return device_buffer_frames_; }
    const char* driver() const noexcept;

private:
    enum class CallbackError { none, read_failed, invalid_data, processing_failed };

    static void SDLCALL receive_audio(void* userdata, SDL_AudioStream* stream,
                                     int additional_amount, int total_amount) noexcept;
    void consume(SDL_AudioStream* stream);

    Measurement& measurement_;
    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioDeviceID selected_device_ = 0;
    SDL_AudioDeviceID logical_device_ = 0;
    int sample_rate_ = 0;
    int channels_ = 0;
    int device_buffer_frames_ = 0;
    std::vector<float> sample_buffer_;
    std::vector<InputDevice> devices_;
    mutable std::string error_;
    std::atomic<CallbackError> callback_error_{CallbackError::none};
};

} // namespace click_latency
