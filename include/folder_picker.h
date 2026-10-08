#pragma once

#include <SDL3/SDL_dialog.h>

#include <memory>
#include <optional>
#include <string>

namespace click_latency {

struct FolderPickerResult {
    std::string directory;
    std::string error;
};

// The native dialog may finish on a worker thread or after the owner closes.
// Its callback owns the shared result storage and never accesses the GUI.
class FolderPicker {
public:
    using ShowDialog = decltype(&SDL_ShowOpenFolderDialog);

    explicit FolderPicker(ShowDialog show_dialog = SDL_ShowOpenFolderDialog);
    bool pending() const;
    void open(SDL_Window* window, const std::string& start_directory);
    std::optional<FolderPickerResult> poll();

private:
    struct State;
    static void SDLCALL completed(void* userdata, const char* const* files, int filter);

    std::shared_ptr<State> state_;
    ShowDialog show_dialog_;
};

} // namespace click_latency
