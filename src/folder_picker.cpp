#include "folder_picker.h"

#include <mutex>
#include <utility>

namespace click_latency {

struct FolderPicker::State {
    std::mutex mutex;
    bool pending = false;
    std::optional<FolderPickerResult> result;
};

FolderPicker::FolderPicker(ShowDialog show_dialog)
    : state_(std::make_shared<State>()), show_dialog_(show_dialog) {}

bool FolderPicker::pending() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->pending;
}

void FolderPicker::open(SDL_Window* window, const std::string& start_directory) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->pending) {
            return;
        }
        state_->pending = true;
        state_->result.reset();
    }
    auto* request = new std::shared_ptr<State>(state_);
    show_dialog_(completed, request, window, start_directory.c_str(), false);
}

void SDLCALL FolderPicker::completed(void* userdata, const char* const* files, int) {
    const std::unique_ptr<std::shared_ptr<State>> request(
        static_cast<std::shared_ptr<State>*>(userdata));
    FolderPickerResult result;
    if (!files) {
        result.error = SDL_GetError();
        if (result.error.empty()) {
            result.error = "The system folder picker is unavailable.";
        }
    } else if (files[0] && files[0][0]) {
        result.directory = files[0];
    }
    std::lock_guard<std::mutex> lock((*request)->mutex);
    (*request)->result = std::move(result);
}

std::optional<FolderPickerResult> FolderPicker::poll() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->result) {
        return std::nullopt;
    }
    auto result = std::move(state_->result);
    state_->result.reset();
    state_->pending = false;
    return result;
}

} // namespace click_latency
