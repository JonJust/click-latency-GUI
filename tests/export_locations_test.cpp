#include "export_locations.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using click_latency::ExportLocations;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = base / ("click-latency-locations-" + std::to_string(token) + "-" +
                            std::to_string(attempt));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("could not create temporary export directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class WorkingDirectory {
public:
    explicit WorkingDirectory(const std::filesystem::path& path)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~WorkingDirectory() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
    }

private:
    std::filesystem::path previous_;
};

void memory_history_and_reuse(const std::filesystem::path& directory) {
    ExportLocations locations;
    require(locations.directory() == (directory / "exports").string(),
            "new sessions should suggest cwd/exports");
    require(locations.recent().empty(), "new sessions must not invent recent destinations");

    locations.remember("mouse-a");
    locations.remember("mouse-b");
    require(locations.directory() == (directory / "mouse-b").string(),
            "a selected folder should become the default for the next export");
    require(locations.recent() == std::vector<std::string>{(directory / "mouse-b").string(),
                                                          (directory / "mouse-a").string()},
            "multiple export folders should be retained in most-recent order");

    locations.remember((directory / "mouse-a" / "unused" / "..").string() + "/");
    require(locations.recent() == std::vector<std::string>{(directory / "mouse-a").string(),
                                                          (directory / "mouse-b").string()},
            "reusing a normalized folder should move it to the front without duplicates");
    locations.remember("");
    require(locations.directory() == (directory / "mouse-a").string(),
            "an empty destination should not discard the selected folder");

    for (std::size_t index = 0; index < ExportLocations::kMaxRecent + 2; ++index) {
        locations.remember((directory / ("folder-" + std::to_string(index))).string());
    }
    require(locations.recent().size() == ExportLocations::kMaxRecent,
            "history should stay bounded in RAM");
    require(locations.recent().front() == (directory / "folder-9").string() &&
            locations.recent().back() == (directory / "folder-2").string(),
            "history should evict the oldest destinations");

    ExportLocations next_session;
    require(next_session.recent().empty() &&
            next_session.directory() == (directory / "exports").string(),
            "a separate app session must not inherit another session's locations");
}

void suggestions_and_collisions(const std::filesystem::path& directory) {
    ExportLocations locations;
    const auto first = directory / "first";
    const auto second = directory / "second";
    std::filesystem::create_directory(first);
    std::filesystem::create_directory(second);
    locations.remember(first.string());
    const std::string stamp = "20261007-130000";
    const auto expected = first / ("mouse-a-wired-1000-hz-" + stamp + ".png");
    require(locations.suggest_path("Mouse A - wired, 1000 Hz", stamp) == expected.string(),
            "filename suggestions should preserve the current label sanitization");
    {
        std::ofstream original(expected);
        original << "existing export";
        std::ofstream duplicate(first / ("mouse-a-wired-1000-hz-" + stamp + "-1.png"));
        duplicate << "another export";
    }
    require(locations.suggest_path("Mouse A - wired, 1000 Hz", stamp) ==
                (first / ("mouse-a-wired-1000-hz-" + stamp + "-2.png")).string(),
            "suggestions should skip existing files without overwriting them");
    locations.remember(second.string());
    require(locations.suggest_path("Mouse B / wireless", stamp) ==
                (second / ("mouse-b-wireless-" + stamp + ".png")).string(),
            "changing folder and label should update both parts of the filename");
    require(locations.suggest_path(" !!! ", stamp) ==
                (second / ("click-latency-" + stamp + ".png")).string(),
            "empty sanitized labels should retain the default name");
    require(std::filesystem::path(locations.suggest_path(std::string(100, 'A'), stamp)).filename() ==
                (std::string(72, 'a') + "-" + stamp + ".png"),
            "long labels should be capped at the existing 72-character limit");
    require(std::filesystem::directory_iterator(second) == std::filesystem::directory_iterator{},
            "suggesting filenames must not create files");
}

void picker_start_locations(const std::filesystem::path& directory) {
    ExportLocations locations;
    require(locations.picker_start("") == directory.string(),
            "a missing default exports folder should fall back to cwd");
    const auto selected = directory / "selected";
    const auto typed = directory / "typed";
    std::filesystem::create_directory(selected);
    std::filesystem::create_directory(typed);
    locations.remember(selected.string());
    require(locations.picker_start("") == selected.string(),
            "the picker should reopen in the last selected folder");
    require(locations.picker_start((typed / "absent" / "nested" / "report.png").string()) ==
                typed.string(),
            "a missing output parent should start the picker at its closest existing ancestor");
    require(locations.picker_start("report.png") == directory.string(),
            "a relative output filename should start the picker in cwd");
    require(locations.picker_start(typed.string() + "/") == typed.string(),
            "a folder-only path with a separator should open that folder");
    require(locations.picker_start(directory.root_path().string()) == directory.root_path().string(),
            "a root-only output path should open the root folder");
    require(locations.picker_start((typed / "report").string()) == typed.string(),
            "an extension-less output filename should still use its parent folder");
    {
        std::ofstream blocker(typed / "parent-file");
        blocker << "a file is not a directory";
    }
    require(locations.picker_start((typed / "parent-file" / "report.png").string()) == typed.string(),
            "a file in the output parent path should be skipped for the picker");
    locations.remember((directory / "deleted-folder").string());
    require(locations.picker_start("") == selected.string(),
            "an unavailable current destination should fall back to another remembered folder");
    std::filesystem::remove(selected);
    require(locations.picker_start("") == directory.string(),
            "unavailable history entries should not prevent falling back to cwd");
}

} // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        WorkingDirectory current(temporary.path());
        memory_history_and_reuse(temporary.path());
        suggestions_and_collisions(temporary.path());
        picker_start_locations(temporary.path());
        std::cout << "Export location tests passed: session history, reuse, filenames, collisions, picker start\n";
    } catch (const std::exception& error) {
        std::cerr << "Export location test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
