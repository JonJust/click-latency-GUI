#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace click_latency {

// Export destinations belong to this app session and are never written to disk.
class ExportLocations {
public:
    static constexpr std::size_t kMaxRecent = 8;

    ExportLocations();

    const std::string& directory() const { return directory_; }
    const std::vector<std::string>& recent() const { return recent_; }

    void remember(const std::string& directory);
    std::string picker_start(const std::string& output_path) const;
    std::string suggest_path(const std::string& label, const std::string& stamp) const;

private:
    std::string directory_;
    std::vector<std::string> recent_;
};

} // namespace click_latency
