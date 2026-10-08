#include "folder_picker.h"

#include <SDL3/SDL_error.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using click_latency::FolderPicker;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct DialogCall {
    SDL_DialogFileCallback callback = nullptr;
    void* userdata = nullptr;
    SDL_Window* window = nullptr;
    std::string start_directory;
    bool allow_many = true;
    int launches = 0;
};

DialogCall retained;

void SDLCALL retain_dialog(SDL_DialogFileCallback callback, void* userdata,
                           SDL_Window* window, const char* start_directory,
                           bool allow_many) {
    retained.callback = callback;
    retained.userdata = userdata;
    retained.window = window;
    retained.start_directory = start_directory ? start_directory : "";
    retained.allow_many = allow_many;
    ++retained.launches;
}

void SDLCALL fail_immediately(SDL_DialogFileCallback callback, void* userdata,
                              SDL_Window*, const char*, bool) {
    SDL_SetError("The test folder picker is unavailable.");
    callback(userdata, nullptr, -1);
}

void SDLCALL fail_without_message(SDL_DialogFileCallback callback, void* userdata,
                                  SDL_Window*, const char*, bool) {
    SDL_ClearError();
    callback(userdata, nullptr, -1);
}

void selected_directory_is_copied() {
    retained = {};
    FolderPicker picker(retain_dialog);
    require(!picker.pending(), "new picker must not have a pending request");
    require(!picker.poll(), "new picker must not have a result");

    const std::string initial = u8"/tmp/Previous exports/測定";
    picker.open(nullptr, initial);
    require(picker.pending(), "opening must mark the picker pending");
    require(!picker.poll(), "unfinished native dialog must not return a result");
    require(retained.launches == 1 && retained.callback != nullptr,
            "opening must launch the native dialog");
    require(retained.start_directory == initial, "native dialog must start at the requested directory");
    require(retained.window == nullptr, "native dialog must use the supplied parent window");
    require(!retained.allow_many, "output directory picker must allow only one selection");

    picker.open(nullptr, "/tmp/another-directory");
    require(retained.launches == 1, "another click must not open a second pending dialog");

    std::string selected = u8"/tmp/Mouse logs/測定データ";
    const std::string expected = selected;
    const char* files[] = {selected.c_str(), "/tmp/ignored-second-selection", nullptr};
    retained.callback(retained.userdata, files, -1);
    selected.assign("native dialog storage has been released");
    require(picker.pending(), "completed request must stay pending until its result is consumed");
    picker.open(nullptr, "/tmp/another-directory");
    require(retained.launches == 1, "unconsumed completion must not be replaced by a new dialog");

    const auto result = picker.poll();
    require(result.has_value(), "selected directory must produce a result");
    require(result->directory == expected, "selected UTF-8 directory must survive native string disposal");
    require(result->error.empty(), "successful selection must not return an error");
    require(!picker.pending(), "consuming completion must clear the pending request");
    require(!picker.poll(), "completion must be returned only once");

    picker.open(nullptr, result->directory);
    require(retained.launches == 2 && retained.start_directory == expected,
            "consumed picker must accept another request in the remembered directory");
    const char* canceled[] = {nullptr};
    retained.callback(retained.userdata, canceled, -1);
    require(picker.poll().has_value(), "second request must complete independently");
}

void cancellation_keeps_the_directory_unchanged() {
    retained = {};
    FolderPicker picker(retain_dialog);
    picker.open(nullptr, "/tmp/existing-output");
    const char* canceled[] = {nullptr};
    retained.callback(retained.userdata, canceled, -1);
    const auto result = picker.poll();
    require(result.has_value() && result->directory.empty() && result->error.empty(),
            "cancel must complete without changing the directory or reporting an error");
    require(!picker.pending(), "cancel must release the picker for another request");

    picker.open(nullptr, "/tmp/existing-output");
    const char* empty_selection[] = {"", nullptr};
    retained.callback(retained.userdata, empty_selection, -1);
    const auto empty_result = picker.poll();
    require(empty_result.has_value() && empty_result->directory.empty() && empty_result->error.empty(),
            "empty selection from the native backend must be treated as cancellation");
}

void immediate_errors_are_preserved() {
    FolderPicker picker(fail_immediately);
    picker.open(nullptr, "/tmp/existing-output");
    require(picker.pending(), "synchronous failure must still await result consumption");
    SDL_SetError("An unrelated later SDL error.");
    const auto result = picker.poll();
    require(result.has_value() && result->directory.empty(), "native error must not select a directory");
    require(result->error == "The test folder picker is unavailable.",
            "native error text must be copied before later SDL operations replace it");
    require(!picker.pending(), "consuming an error must permit another request");

    FolderPicker without_message(fail_without_message);
    without_message.open(nullptr, "");
    const auto fallback = without_message.poll();
    require(fallback.has_value() && !fallback->error.empty(),
            "native failure without SDL error text must still explain the failure");
}

void completion_can_arrive_from_a_worker_thread() {
    retained = {};
    FolderPicker picker(retain_dialog);
    picker.open(nullptr, "/tmp/start-directory");
    const auto call = retained;
    std::thread worker([call] {
        const std::string selected = u8"/tmp/Thread exports/測定";
        const char* files[] = {selected.c_str(), nullptr};
        call.callback(call.userdata, files, -1);
    });
    const bool pending = picker.pending();
    worker.join();
    require(pending, "main thread must safely observe a pending worker-thread completion");
    const auto result = picker.poll();
    require(result.has_value() && result->directory == u8"/tmp/Thread exports/測定" && result->error.empty(),
            "main thread must receive a copied worker-thread selection");
}

void completion_can_outlive_the_picker() {
    retained = {};
    {
        FolderPicker picker(retain_dialog);
        picker.open(nullptr, "/tmp/start-directory");
        require(picker.pending(), "test request must be active before its owner closes");
    }
    const auto call = retained;
    std::thread worker([call] {
        const char* files[] = {"/tmp/output-after-owner-closes", nullptr};
        call.callback(call.userdata, files, -1);
    });
    worker.join();
}

} // namespace

int main() {
    try {
        selected_directory_is_copied();
        cancellation_keeps_the_directory_unchanged();
        immediate_errors_are_preserved();
        completion_can_arrive_from_a_worker_thread();
        completion_can_outlive_the_picker();
        std::cout << "Folder picker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Folder picker test failed: " << error.what() << '\n';
        return 1;
    }
}
