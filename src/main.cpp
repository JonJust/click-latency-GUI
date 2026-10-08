#include "audio_capture.h"
#include "measurement.h"
#include "gui.h"
#include "folder_picker.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using click_latency::AudioCapture;
using click_latency::Measurement;
using click_latency::UiState;
using click_latency::draw_gui;
using click_latency::set_style;
using click_latency::save_graph_png;

bool save_screenshot(const char* path, SDL_Window* window) {
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0) {
        return false;
    }
    const auto pitch = static_cast<std::size_t>(width) * 4;
    std::vector<unsigned char> pixels(pitch * static_cast<std::size_t>(height));
    std::vector<unsigned char> flipped(pixels.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    for (int y = 0; y < height; ++y) {
        std::memcpy(flipped.data() + static_cast<std::size_t>(y) * pitch,
                    pixels.data() + static_cast<std::size_t>(height - y - 1) * pitch, pitch);
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32,
                                                 flipped.data(), width * 4);
    if (!surface) {
        return false;
    }
    const bool saved = SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    return saved;
}

} // namespace

int main(int argc, char** argv) {
    bool smoke_test = false;
    const char* screenshot = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--smoke-test") == 0) {
            smoke_test = true;
        } else if (std::strcmp(argv[index], "--screenshot") == 0 && index + 1 < argc) {
            screenshot = argv[++index];
        } else {
            std::fprintf(stderr, "Usage: %s [--smoke-test] [--screenshot path.bmp]\n", argv[0]);
            return 2;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "Cannot initialize video: %s\n", SDL_GetError());
        return 1;
    }
    std::string audio_init_error;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        audio_init_error = std::string("Cannot initialize audio: ") + SDL_GetError();
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    const SDL_DisplayID primary_display = SDL_GetPrimaryDisplay();
    const float initial_scale = std::max(1.0f, SDL_GetDisplayContentScale(primary_display));
    int initial_width = static_cast<int>(1080 * initial_scale);
    int initial_height = static_cast<int>(860 * initial_scale);
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(primary_display, &usable)) {
        initial_width = std::min(initial_width, usable.w);
        initial_height = std::min(initial_height, usable.h - static_cast<int>(40 * initial_scale));
    }
    SDL_Window* window = SDL_CreateWindow("Click latency", initial_width, initial_height,
                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "Cannot create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(560 * initial_scale),
                                    static_cast<int>(620 * initial_scale));
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context || !SDL_GL_MakeCurrent(window, context)) {
        std::fprintf(stderr, "Cannot create OpenGL context: %s\n", SDL_GetError());
        if (context) {
            SDL_GL_DestroyContext(context);
        }
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // Event timestamps drive measurements. Rendering at 60 Hz keeps the app
    // inexpensive without quantizing the measurement to a display frame.
    SDL_GL_SetSwapInterval(0);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImFontConfig font_config;
    font_config.SizePixels = 17;
    ImGui::GetIO().Fonts->AddFontDefaultVector(&font_config);
    set_style();
    const float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale > 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(display_scale);
        ImGui::GetStyle().FontScaleDpi = display_scale;
    }
    if (!ImGui_ImplSDL3_InitForOpenGL(window, context) || !ImGui_ImplOpenGL3_Init("#version 330")) {
        std::fprintf(stderr, "Cannot initialize Dear ImGui backends\n");
        ImGui::DestroyContext();
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int exit_code = 0;
    {
        Measurement measurement;
        AudioCapture audio(measurement);
        if (audio_init_error.empty()) {
            audio.open(SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
        }
        if (smoke_test) {
            std::printf("Video: %s; audio: %s; inputs: %zu; capture: %s\n",
                        SDL_GetCurrentVideoDriver(), audio.driver(), audio.devices().size(),
                        audio.active() ? "active" : "unavailable");
            if (!audio.active()) {
                std::printf("Audio status: %s\n", audio.error().c_str());
            }
        }
        UiState ui;
        click_latency::FolderPicker folder_picker;
        bool running = true;
        int frames = 0;
        const SDL_WindowID window_id = SDL_GetWindowID(window);
        while (running) {
            const auto frame_start = SDL_GetTicksNS();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     event.window.windowID == window_id)) {
                    running = false;
                }
                if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                    event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                    event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                    event.type == SDL_EVENT_MOUSE_WHEEL) {
                    ui.button.valid = false;
                }
                if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                     event.type == SDL_EVENT_WINDOW_MINIMIZED) &&
                    event.window.windowID == window_id) {
                    ui.target_down = false;
                    measurement.reset();
                }
                if (event.type == SDL_EVENT_AUDIO_DEVICE_ADDED ||
                    event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
                    if (!event.adevice.recording) {
                        continue;
                    }
                    ui.button.valid = false;
                    if (event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED &&
                        (event.adevice.which == audio.selected_device() ||
                         event.adevice.which == audio.logical_device())) {
                        audio.close();
                        ui.target_down = false;
                    }
                    audio.refresh_devices();
                    // A default recording stream can survive unplugging the
                    // last microphone as a silent SDL "zombie" device.
                    if (audio.selected_device() == SDL_AUDIO_DEVICE_DEFAULT_RECORDING &&
                        audio.devices().size() <= 1) {
                        audio.close();
                        ui.target_down = false;
                    }
                }
                if (event.type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED &&
                    event.adevice.recording && audio.active() &&
                    (event.adevice.which == audio.logical_device() ||
                     event.adevice.which == audio.selected_device())) {
                    // Default-device migration can change channel count.
                    // Reopen to preserve independent channel peaks, and avoid
                    // carrying a pending trial across different microphones.
                    const auto selected = audio.selected_device();
                    audio.open(selected);
                    ui.button.valid = false;
                    ui.target_down = false;
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    event.button.windowID == window_id &&
                    event.button.button == SDL_BUTTON_LEFT &&
                    event.button.which != SDL_TOUCH_MOUSEID && audio.active() &&
                    ui.button.contains(event.button.x, event.button.y)) {
                    measurement.click(event.button.timestamp ? event.button.timestamp : SDL_GetTicksNS());
                    ui.target_down = true;
                }
                // Track the matching UP even if the pointer left the target.
                // UP events still reach the detector with release measurement
                // disabled so its sound can be suppressed instead of timing out.
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                    event.button.windowID == window_id &&
                    event.button.button == SDL_BUTTON_LEFT &&
                    event.button.which != SDL_TOUCH_MOUSEID && ui.target_down) {
                    if (audio.active()) {
                        measurement.release(event.button.timestamp ? event.button.timestamp : SDL_GetTicksNS());
                    }
                    ui.target_down = false;
                }
            }
            if (!running) {
                break;
            }
            if (auto result = folder_picker.poll()) {
                ui.export_folder_pending = false;
                if (!result->error.empty()) {
                    ui.export_failed = true;
                    ui.export_status = "Cannot open the folder picker: " + result->error +
                                       " You can still enter the output path manually.";
                } else if (!result->directory.empty()) {
                    click_latency::select_export_directory(ui, result->directory);
                }
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(20);
                continue;
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            draw_gui(audio, measurement, ui, audio_init_error);
            ImGui::Render();
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.055f, 0.071f, 0.09f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (ui.export_folder_requested) {
                ui.export_folder_requested = false;
                folder_picker.open(window, ui.export_locations.picker_start(ui.export_path.data()));
            }
            if (ui.export_requested) {
                const auto result = save_graph_png(ui.export_report, ui.export_path.data());
                ui.export_requested = false;
                ui.export_failed = !result.success;
                ui.export_status = result.success ? "Saved: " + result.path : result.error;
                if (result.success) {
                    ui.export_locations.remember(std::filesystem::path(result.path).parent_path().string());
                }
            }
            if (screenshot && frames == 8 && !save_screenshot(screenshot, window)) {
                std::fprintf(stderr, "Cannot save screenshot: %s\n", SDL_GetError());
                exit_code = 1;
            }
            SDL_GL_SwapWindow(window);
            ++frames;
            if (smoke_test && frames >= 20) {
                running = false;
            }
            constexpr Uint64 kFrameNs = 16'666'667;
            const auto elapsed = SDL_GetTicksNS() - frame_start;
            if (elapsed < kFrameNs) {
                SDL_DelayNS(kFrameNs - elapsed);
            }
        }
    } // Close the microphone before shutting down SDL.

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
