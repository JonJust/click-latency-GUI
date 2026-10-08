#include "export_locations.h"

#include <algorithm>
#include <filesystem>

namespace click_latency {
namespace {

std::filesystem::path normalized_directory(const std::filesystem::path& directory) {
    std::error_code error;
    auto normalized = std::filesystem::absolute(directory, error);
    if (error) {
        normalized = directory;
    }
    normalized = normalized.lexically_normal();
    // A trailing separator must not give the same folder another history entry.
    if (normalized.filename().empty() && normalized != normalized.root_path()) {
        normalized = normalized.parent_path();
    }
    return normalized;
}

bool existing_directory(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

std::string filename_label(const std::string& label) {
    std::string result;
    for (const unsigned char ch : label) {
        if (result.size() >= 72) {
            break;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            result += static_cast<char>(ch);
        } else if (ch >= 'A' && ch <= 'Z') {
            result += static_cast<char>(ch - 'A' + 'a');
        } else if (!result.empty() && result.back() != '-') {
            result += '-';
        }
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    return result.empty() ? "click-latency" : result;
}

} // namespace

ExportLocations::ExportLocations() {
    std::error_code error;
    auto directory = std::filesystem::current_path(error) / "exports";
    if (error) {
        directory = "exports";
    }
    directory_ = directory.string();
}

void ExportLocations::remember(const std::string& directory) {
    if (directory.empty()) {
        return;
    }
    directory_ = normalized_directory(directory).string();
    recent_.erase(std::remove(recent_.begin(), recent_.end(), directory_), recent_.end());
    recent_.insert(recent_.begin(), directory_);
    if (recent_.size() > kMaxRecent) {
        recent_.resize(kMaxRecent);
    }
}

std::string ExportLocations::picker_start(const std::string& output_path) const {
    if (!output_path.empty()) {
        auto parent = std::filesystem::path(output_path).parent_path();
        parent = normalized_directory(parent.empty() ? std::filesystem::path(".") : parent);
        while (!parent.empty()) {
            if (existing_directory(parent)) {
                return parent.string();
            }
            const auto next = parent.parent_path();
            if (next == parent) {
                break;
            }
            parent = next;
        }
    }
    if (existing_directory(directory_)) {
        return directory_;
    }
    for (const auto& directory : recent_) {
        if (existing_directory(directory)) {
            return directory;
        }
    }
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    return error ? std::string{} : current.string();
}

std::string ExportLocations::suggest_path(const std::string& label,
                                          const std::string& stamp) const {
    const std::filesystem::path directory(directory_);
    const std::string stem = filename_label(label) + "-" + stamp;
    auto path = directory / (stem + ".png");
    std::error_code error;
    for (std::size_t suffix = 1; std::filesystem::exists(path, error); ++suffix) {
        path = directory / (stem + "-" + std::to_string(suffix) + ".png");
    }
    return path.string();
}

} // namespace click_latency
