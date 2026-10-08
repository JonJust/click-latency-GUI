#pragma once

#include "measurement.h"

#include <string>

struct ImVec2;

namespace click_latency {

// Freeze both the graph history and the test description at the export click.
struct ExportReport {
    Snapshot snapshot;
    std::string label;
    std::string microphone;
    std::string created_at;
};

struct ExportResult {
    bool success = false;
    std::string path;
    std::string error;
};

// Requires the application's OpenGL context to be current on this thread.
// Uses a separate ImGui context and an offscreen image, so the report includes
// every graph even when the application window is small or scrolled. Missing
// directories are created; an existing output file is never replaced.
ExportResult save_graph_png(const ExportReport& report, const std::string& requested_path);

// Shared report layout implemented by the GUI, in an independent ImGui frame.
void draw_export_report(const ExportReport& report, const ImVec2& size);

} // namespace click_latency
