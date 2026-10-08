#pragma once

#include "imgui.h"
#include "graph_export.h"
#include "export_locations.h"

#include <array>
#include <string>

namespace click_latency {

class AudioCapture;
class Measurement;

struct ButtonBounds {
    ImVec2 min{};
    ImVec2 max{};
    bool valid = false;

    bool contains(float x, float y) const {
        return valid && x >= min.x && x < max.x && y >= min.y && y < max.y;
    }
};

struct UiState {
    float threshold = -30;
    float meter_db = -96;
    bool track_release = false;
    bool target_down = false;
    ButtonBounds button;
    std::array<char, 160> export_label{};
    std::array<char, 4096> export_path{};
    ExportLocations export_locations;
    ExportReport export_report;
    std::string export_stamp;
    std::string export_status;
    bool export_failed = false;
    bool export_requested = false;
    bool export_dialog_requested = false;
    bool export_auto_path = true;
    bool export_folder_requested = false;
    bool export_folder_pending = false;
};

void set_style();
void select_export_directory(UiState& ui, const std::string& directory);
void draw_gui(AudioCapture& audio, Measurement& measurement, UiState& ui,
              const std::string& init_error);

} // namespace click_latency
