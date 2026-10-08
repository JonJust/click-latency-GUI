#include "gui.h"

#include "audio_capture.h"
#include "measurement.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>

namespace click_latency {
namespace {

const ImVec4 kMuted(0.56f, 0.62f, 0.68f, 1.0f);
const ImVec4 kGreen(0.32f, 0.87f, 0.67f, 1.0f);
const ImVec4 kBlue(0.43f, 0.70f, 0.98f, 1.0f);
const ImVec4 kAmber(0.98f, 0.74f, 0.36f, 1.0f);
const ImVec4 kError(1.0f, 0.47f, 0.47f, 1.0f);

void gap(float height = 4.0f) {
    ImGui::Dummy(ImVec2(0, height * ImGui::GetStyle().FontScaleDpi));
}

void draw_result(const char* label, const Result& result, const ImVec4& color,
                 const char* timeout) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    char text[64];
    switch (result.phase) {
    case Phase::Measured:
        std::snprintf(text, sizeof(text), "%.3f ms", result.latency_ms);
        break;
    case Phase::TimedOut:
        std::snprintf(text, sizeof(text), "%s", timeout);
        break;
    case Phase::Waiting:
        std::snprintf(text, sizeof(text), "waiting...");
        break;
    case Phase::Listening:
        std::snprintf(text, sizeof(text), "--");
        break;
    }
    ImGui::PushFont(nullptr, 22);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * scale, 8 * scale));
    ImGui::PushStyleColor(ImGuiCol_Text, result.phase == Phase::Measured ? color
                                          : result.phase == Phase::TimedOut ? kAmber
                                          : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##result", text, sizeof(text), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::PopID();
}

float graph_ceiling(const StatsSnapshot& stats) {
    float maximum = 0;
    for (float value : stats.samples) {
        maximum = std::max(maximum, value);
    }
    for (float value : stats.averages) {
        maximum = std::max(maximum, value);
    }
    return std::min(80.0f, std::max(1.0f, std::ceil(maximum * 12.0f) / 10.0f));
}

void draw_graph(const StatsSnapshot& stats, const ImVec4& sample_color,
                float graph_height = 130, float shared_ceiling = 0) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(120.0f * scale, ImGui::GetContentRegionAvail().x), graph_height * scale);
    ImGui::InvisibleButton("##latency_graph", size);
    const bool hovered = ImGui::IsItemHovered();
    if (!ImGui::IsItemVisible()) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    draw->PushClipRect(origin, end, true);
    draw->AddRectFilled(origin, end, IM_COL32(22, 30, 38, 255), 6 * scale);
    draw->AddRect(origin, end, IM_COL32(48, 59, 69, 255), 6 * scale);
    const ImVec2 plot_min(origin.x + 46 * scale, origin.y + 14 * scale);
    const ImVec2 plot_max(end.x - 14 * scale, end.y - 27 * scale);
    const float width = plot_max.x - plot_min.x;
    const float height = plot_max.y - plot_min.y;
    const float ceiling = shared_ceiling > 0 ? shared_ceiling : graph_ceiling(stats);
    for (int grid = 0; grid < 3; ++grid) {
        const float ratio = static_cast<float>(grid) / 2;
        const float y = plot_max.y - ratio * height;
        draw->AddLine(ImVec2(plot_min.x, y), ImVec2(plot_max.x, y), IM_COL32(52, 64, 76, 255));
        char label[24];
        std::snprintf(label, sizeof(label), "%.1f", ratio * ceiling);
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(plot_min.x - text_size.x - 8 * scale, y - text_size.y / 2),
                      ImGui::GetColorU32(kMuted), label);
    }

    const std::size_t count = stats.samples.size();
    if (count == 0) {
        const char* text = "No successful samples yet";
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(plot_min.x + (width - text_size.x) / 2,
                            plot_min.y + (height - text_size.y) / 2),
                      ImGui::GetColorU32(kMuted), text);
    } else {
        const auto point = [&](std::size_t index, float value) {
            const float x = count == 1 ? plot_min.x + width / 2
                                      : plot_min.x + width * static_cast<float>(index) /
                                                                static_cast<float>(count - 1);
            return ImVec2(x, plot_max.y - std::clamp(value / ceiling, 0.0f, 1.0f) * height);
        };
        for (std::size_t index = 0; index < count; ++index) {
            const ImVec2 sample = point(index, stats.samples[index]);
            if (index > 0) {
                draw->AddLine(point(index - 1, stats.samples[index - 1]), sample,
                              ImGui::GetColorU32(sample_color), 1.5f * scale);
            }
            if (count < 65) {
                draw->AddCircleFilled(sample, 2.5f * scale, ImGui::GetColorU32(sample_color));
            }
        }
        const std::size_t average_count = std::min(count, stats.averages.size());
        for (std::size_t index = 0; index < average_count; ++index) {
            const ImVec2 average = point(index, stats.averages[index]);
            if (index > 0) {
                draw->AddLine(point(index - 1, stats.averages[index - 1]), average,
                              ImGui::GetColorU32(kAmber), 2 * scale);
            } else if (average_count == 1) {
                draw->AddCircleFilled(average, 2 * scale, ImGui::GetColorU32(kAmber));
            }
        }
        char first[32];
        char last[32];
        const auto first_sample = stats.count - static_cast<std::uint64_t>(count) + 1;
        std::snprintf(first, sizeof(first), "#%llu", static_cast<unsigned long long>(first_sample));
        std::snprintf(last, sizeof(last), "#%llu", static_cast<unsigned long long>(stats.count));
        draw->AddText(ImVec2(plot_min.x, plot_max.y + 7 * scale), ImGui::GetColorU32(kMuted), first);
        if (count > 1) {
            draw->AddText(ImVec2(plot_max.x - ImGui::CalcTextSize(last).x, plot_max.y + 7 * scale),
                          ImGui::GetColorU32(kMuted), last);
        }
        if (hovered && width > 0) {
            const float fraction = std::clamp((ImGui::GetIO().MousePos.x - plot_min.x) / width,
                                              0.0f, 1.0f);
            const auto index = static_cast<std::size_t>(std::lround(fraction *
                                                        static_cast<float>(count - 1)));
            draw->AddLine(ImVec2(point(index, 0).x, plot_min.y),
                          ImVec2(point(index, 0).x, plot_max.y), IM_COL32(142, 159, 177, 170));
            ImGui::BeginTooltip();
            ImGui::Text("Sample #%llu", static_cast<unsigned long long>(first_sample + index));
            ImGui::Text("Latency: %.3f ms", static_cast<double>(stats.samples[index]));
            if (index < average_count) {
                ImGui::Text("Running average: %.3f ms", static_cast<double>(stats.averages[index]));
            }
            ImGui::EndTooltip();
        }
    }
    draw->PopClipRect();
}

void draw_series(const char* label, const StatsSnapshot& stats, const ImVec4& color,
                 float graph_height = 130, float shared_ceiling = 0) {
    ImGui::PushID(label);
    ImGui::PushFont(nullptr, 21);
    ImGui::TextColored(color, "%s", label);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "  %llu samples", static_cast<unsigned long long>(stats.count));
    if (ImGui::BeginTable("##statistics", 4, ImGuiTableFlags_SizingStretchSame)) {
        const char* labels[] = {"Average (ms)", "Minimum (ms)", "Maximum (ms)", "Range (ms)"};
        const double values[] = {stats.average_ms, stats.minimum_ms, stats.maximum_ms, stats.range_ms};
        for (int column = 0; column < 4; ++column) {
            ImGui::TableNextColumn();
            ImGui::TextColored(kMuted, "%s", labels[column]);
            ImGui::PushFont(nullptr, 20);
            if (stats.count > 0) {
                ImGui::Text("%.3f", values[column]);
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    draw_graph(stats, color, graph_height, shared_ceiling);
    ImGui::TextColored(color, "Samples");
    ImGui::SameLine();
    ImGui::TextColored(kAmber, "Running average");
    ImGui::PopID();
}

void draw_controls(AudioCapture& audio, Measurement& measurement, UiState& ui,
                   const std::string& init_error, Snapshot& snapshot) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::TextUnformatted("Microphone");
    const char* device_name = "Select an input device";
    for (const auto& device : audio.devices()) {
        if (device.id == audio.selected_device()) {
            device_name = device.name.c_str();
            break;
        }
    }
    ImGui::SetNextItemWidth(std::max(120.0f * scale, ImGui::GetContentRegionAvail().x - 100 * scale));
    if (ImGui::BeginCombo("##microphone", device_name)) {
        for (const auto& device : audio.devices()) {
            ImGui::PushID(static_cast<int>(device.id));
            const bool selected = device.id == audio.selected_device();
            if (ImGui::Selectable(device.name.c_str(), selected)) {
                audio.open(device.id);
                ui.meter_db = Measurement::kFloorDb;
                ui.target_down = false;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(88 * scale, 0))) {
        audio.refresh_devices();
    }
    if (!init_error.empty() || !audio.error().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kError);
        ImGui::TextWrapped("%s", init_error.empty() ? audio.error().c_str() : init_error.c_str());
        ImGui::PopStyleColor();
    }
    gap();
    ImGui::TextUnformatted("Sound threshold");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##threshold", &ui.threshold, -96.0f, 0.0f, "%.1f dBFS",
                           ImGuiSliderFlags_AlwaysClamp)) {
        measurement.set_threshold(ui.threshold);
        ui.target_down = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Set this above room noise and below the peak of a mouse click.");
    }
    const auto now = SDL_GetTicksNS();
    snapshot = measurement.snapshot(now);
    const bool active = audio.active();
    char level[48];
    if (active) {
        std::snprintf(level, sizeof(level), "%.1f dBFS", snapshot.current_db);
    } else {
        std::snprintf(level, sizeof(level), "Microphone unavailable");
    }
    ImGui::TextColored(kMuted, "Current microphone level");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##current_db", level, sizeof(level), ImGuiInputTextFlags_ReadOnly);
    ui.meter_db = active ? std::max(snapshot.current_db,
                                   ui.meter_db - ImGui::GetIO().DeltaTime * 90.0f)
                        : Measurement::kFloorDb;
    const float fraction = std::clamp((ui.meter_db + 96.0f) / 96.0f, 0.0f, 1.0f);
    const ImVec2 meter_start = ImGui::GetCursorScreenPos();
    const float meter_width = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui.meter_db >= ui.threshold ? kAmber : kGreen);
    ImGui::ProgressBar(fraction, ImVec2(-1, 8 * scale), "");
    ImGui::PopStyleColor();
    const float marker_x = meter_start.x + meter_width * (ui.threshold + 96.0f) / 96.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(marker_x, meter_start.y - 2 * scale),
                                      ImVec2(marker_x, meter_start.y + 10 * scale),
                                      ImGui::GetColorU32(kAmber), 2 * scale);
    gap(2);
    if (ImGui::Checkbox("Measure release latency", &ui.track_release)) {
        measurement.set_release_enabled(ui.track_release);
        snapshot = measurement.snapshot(now);
    }

    ImVec4 status_color = kMuted;
    std::string status;
    if (!active) {
        status = "Select an available microphone to begin.";
    } else if (snapshot.phase == Phase::Waiting) {
        const auto elapsed = now >= snapshot.trigger_ns ? now - snapshot.trigger_ns : 0;
        const double left = static_cast<double>(Measurement::kWindowNs -
                                 std::min(elapsed, Measurement::kWindowNs)) / 1'000'000.0;
        char text[80];
        std::snprintf(text, sizeof(text), "%s sound detected  /  %.0f ms left",
                      snapshot.edge == Edge::Release ? "Release" : "Press", left);
        status = text;
        status_color = kAmber;
    } else if (snapshot.button_down && !ui.track_release) {
        status = "Release the button for the next press test";
    } else if (snapshot.armed) {
        status = snapshot.button_down ? "Listening for release sound" : "Listening for press sound";
        status_color = snapshot.button_down ? kBlue : kGreen;
    } else {
        status = "Waiting for quiet before the next test";
    }
    ImGui::TextColored(status_color, "%s", status.c_str());
    ImGui::BeginDisabled(!active);
    ImGui::PushStyleColor(ImGuiCol_Button, snapshot.phase == Phase::Waiting
                             ? ImVec4(0.55f, 0.34f, 0.09f, 1)
                             : ImVec4(0.08f, 0.34f, 0.25f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.11f, 0.44f, 0.32f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.28f, 0.21f, 1));
    ImGui::PushFont(nullptr, 26);
    ImGui::Button(ui.track_release ? "PRESS / RELEASE HERE" : "CLICK HERE", ImVec2(-1, 104 * scale));
    const ImVec2 item_min = ImGui::GetItemRectMin();
    const ImVec2 item_max = ImGui::GetItemRectMax();
    const ImVec2 clip_min = ImGui::GetWindowDrawList()->GetClipRectMin();
    const ImVec2 clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
    ui.button.min = ImVec2(std::max(item_min.x, clip_min.x), std::max(item_min.y, clip_min.y));
    ui.button.max = ImVec2(std::min(item_max.x, clip_max.x), std::min(item_max.y, clip_max.y));
    ui.button.valid = active && ImGui::IsItemVisible() &&
                      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Keep the pointer here, then press the left mouse button.\n"
                          "With release measurement enabled, hold briefly and release it too.");
    }
    gap();
    draw_result("Down press latency", snapshot.press, kGreen, "no click detected");
    if (ui.track_release) {
        draw_result("Release latency", snapshot.release, kBlue, "no release detected");
    }
    gap();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Place the microphone close to the mouse. Hold briefly between pressing and releasing.");
    ImGui::PopStyleColor();
}

bool set_export_path(UiState& ui, const std::string& path) {
    if (path.size() >= ui.export_path.size()) {
        ui.export_failed = true;
        ui.export_status = "The output path is too long. Choose a shorter folder or filename.";
        return false;
    }
    std::snprintf(ui.export_path.data(), ui.export_path.size(), "%s", path.c_str());
    return true;
}

void suggest_export_path(UiState& ui) {
    set_export_path(ui, ui.export_locations.suggest_path(ui.export_label.data(), ui.export_stamp));
}

void prepare_export(AudioCapture& audio, UiState& ui, const Snapshot& snapshot) {
    ui.export_report.snapshot = snapshot;
    ui.export_report.microphone = "System default microphone";
    for (const auto& device : audio.devices()) {
        if (device.id == audio.selected_device()) {
            ui.export_report.microphone = device.name;
            break;
        }
    }
    const auto time = std::time(nullptr);
    std::tm local{};
    localtime_r(&time, &local);
    char timestamp[96];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %Z", &local);
    ui.export_report.created_at = timestamp;
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local);
    ui.export_stamp = timestamp;
    ui.export_auto_path = true;
    ui.export_status.clear();
    ui.export_failed = false;
    suggest_export_path(ui);
    ui.export_dialog_requested = true;
}

void draw_export_dialog(UiState& ui) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(650 * scale, viewport->WorkSize.x - 20 * scale), 0));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x / 2,
                                 viewport->WorkPos.y + viewport->WorkSize.y / 2),
                           ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Export graphs", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::BeginDisabled(ui.export_folder_pending);
    ImGui::TextUnformatted("Mouse / graph label");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##export_label", "Mouse model - connection / polling rate",
                                  ui.export_label.data(), ui.export_label.size()) && ui.export_auto_path) {
        suggest_export_path(ui);
    }
    ImGui::TextUnformatted("Save PNG to");
    const float browse_width = ImGui::CalcTextSize("Browse...").x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SetNextItemWidth(std::max(100 * scale, ImGui::GetContentRegionAvail().x -
                                              browse_width - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputText("##export_path", ui.export_path.data(), ui.export_path.size())) {
        ui.export_auto_path = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(browse_width, 0))) {
        ui.export_folder_requested = true;
        ui.export_folder_pending = true;
        ui.export_status.clear();
        ui.export_failed = false;
    }
    if (!ui.export_locations.recent().empty()) {
        ImGui::SetNextItemWidth(-1);
        std::string selected_directory;
        if (ImGui::BeginCombo("##recent_export_folders", "Recent folders")) {
            for (const auto& directory : ui.export_locations.recent()) {
                if (ImGui::Selectable(directory.c_str(), directory == ui.export_locations.directory())) {
                    selected_directory = directory;
                }
            }
            ImGui::EndCombo();
        }
        if (!selected_directory.empty()) {
            select_export_directory(ui, selected_directory);
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("A .png extension is added if needed. Existing files are kept.");
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ui.export_folder_pending) {
        ImGui::TextColored(kMuted, "Choose an output folder in the system file explorer...");
    }
    gap();
    ImGui::Text("Snapshot: %llu down samples", static_cast<unsigned long long>(ui.export_report.snapshot.press_stats.count));
    if (ui.export_report.snapshot.release_enabled) {
        ImGui::SameLine();
        ImGui::Text("/ %llu release samples", static_cast<unsigned long long>(ui.export_report.snapshot.release_stats.count));
    }
    ImGui::TextWrapped("The image includes this snapshot, its statistics, microphone, threshold, and date.");
    if (ui.export_failed && !ui.export_status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kError);
        ImGui::TextWrapped("%s", ui.export_status.c_str());
        ImGui::PopStyleColor();
    }
    gap();
    ImGui::BeginDisabled(ui.export_requested || ui.export_folder_pending || ui.export_path[0] == '\0');
    if (ImGui::Button("Save PNG")) {
        ui.export_report.label = ui.export_label[0] ? ui.export_label.data() : "Click latency";
        ui.export_requested = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.export_folder_pending);
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (!ui.export_failed && !ui.export_status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::TextWrapped("%s", ui.export_status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
}

void draw_statistics(AudioCapture& audio, Measurement& measurement, UiState& ui, const Snapshot& snapshot) {
    ImGui::TextUnformatted("Sample averages");
    ImGui::SameLine();
    if (ImGui::Button("Reset averages")) {
        measurement.reset_averages();
    }
    ImGui::SameLine();
    const bool has_samples = snapshot.press_stats.count > 0 ||
                             (snapshot.release_enabled && snapshot.release_stats.count > 0);
    ImGui::BeginDisabled(!has_samples);
    if (ImGui::Button("Export PNG")) {
        prepare_export(audio, ui, snapshot);
    }
    ImGui::EndDisabled();
    if (!has_samples && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Collect a successful sample before exporting the graphs.");
    }
    gap(2);
    draw_series("Down press", snapshot.press_stats, kGreen);
    if (ui.track_release) {
        gap();
        ImGui::Separator();
        gap();
        draw_series("Release", snapshot.release_stats, kBlue);
    }
    gap();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Successful samples only. Range = maximum - minimum.\n"
                      "Graphs: latest 256 samples. Statistics: all samples since reset.");
    gap();
    ImGui::TextWrapped("dBFS is digital signal level. Audio buffering affects the measured latency.");
    ImGui::PopStyleColor();
}

} // namespace

void select_export_directory(UiState& ui, const std::string& directory) {
    if (directory.empty()) {
        return;
    }
    // Keep a manually edited filename when moving it to another folder.
    // Automatic filenames continue to follow the label and avoid collisions.
    ExportLocations locations = ui.export_locations;
    locations.remember(directory);
    const auto filename = std::filesystem::path(ui.export_path.data()).filename();
    const bool automatic = ui.export_auto_path || filename.empty();
    const auto path = automatic
        ? locations.suggest_path(ui.export_label.data(), ui.export_stamp)
        : (std::filesystem::path(locations.directory()) / filename).string();
    if (set_export_path(ui, path)) {
        ui.export_locations = std::move(locations);
        ui.export_auto_path = automatic;
        ui.export_failed = false;
        ui.export_status.clear();
    }
}

void set_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(28, 24);
    style.FramePadding = ImVec2(12, 9);
    style.ItemSpacing = ImVec2(12, 8);
    style.ItemInnerSpacing = ImVec2(8, 8);
    style.FrameRounding = 6;
    style.GrabRounding = 5;
    style.PopupRounding = 7;
    style.WindowBorderSize = 0;
    style.FrameBorderSize = 1;
    style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.94f, 0.97f, 1);
    style.Colors[ImGuiCol_TextDisabled] = kMuted;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.071f, 0.09f, 1);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.13f, 1);
    style.Colors[ImGuiCol_Border] = ImVec4(0.19f, 0.23f, 0.27f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.12f, 0.15f, 1);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.18f, 0.21f, 1);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.21f, 0.25f, 1);
    style.Colors[ImGuiCol_SliderGrab] = kGreen;
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.49f, 0.97f, 0.78f, 1);
    style.Colors[ImGuiCol_CheckMark] = kGreen;
    style.Colors[ImGuiCol_Button] = ImVec4(0.13f, 0.18f, 0.22f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.28f, 0.32f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.33f, 0.37f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.29f, 0.25f, 1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.36f, 0.29f, 1);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.40f, 0.32f, 1);
}

void draw_gui(AudioCapture& audio, Measurement& measurement, UiState& ui,
              const std::string& init_error) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Click latency", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(nullptr, 30);
    ImGui::TextUnformatted("Click latency");
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 80 * scale);
    if (ImGui::SmallButton("Reset")) {
        measurement.reset();
        ui.target_down = false;
        ui.meter_db = Measurement::kFloorDb;
    }
    ImGui::TextColored(kMuted, "Acoustic onset to mouse event  /  80 ms window");
    gap();
    ImGui::Separator();
    gap();
    Snapshot snapshot{};
    const bool columns = ImGui::GetContentRegionAvail().x >= 900 * scale;
    if (columns && ImGui::BeginTable("##layout", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthFixed, 390 * scale);
        ImGui::TableSetupColumn("Graphs", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        draw_controls(audio, measurement, ui, init_error, snapshot);
        ImGui::TableNextColumn();
        draw_statistics(audio, measurement, ui, snapshot);
        ImGui::EndTable();
    } else {
        draw_controls(audio, measurement, ui, init_error, snapshot);
        gap();
        ImGui::Separator();
        gap();
        draw_statistics(audio, measurement, ui, snapshot);
    }
    // The button is inside a table's ID scope. Open the popup in the same
    // window scope used by BeginPopupModal, after leaving that table.
    if (ui.export_dialog_requested) {
        ImGui::OpenPopup("Export graphs");
        ui.export_dialog_requested = false;
        ui.button.valid = false;
    }
    draw_export_dialog(ui);
    ImGui::End();
}

void draw_export_report(const ExportReport& report, const ImVec2& size) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36, 28));
    ImGui::Begin("##graph_report", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(nullptr, 30);
    ImGui::TextUnformatted("Mouse click latency");
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 28);
    ImGui::TextWrapped("%s", report.label.empty() ? "Click latency" : report.label.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Captured: %s", report.created_at.c_str());
    ImGui::TextWrapped("Microphone: %s", report.microphone.c_str());
    ImGui::Text("Threshold: %.1f dBFS  /  Detection window: 80 ms", static_cast<double>(report.snapshot.threshold_db));
    ImGui::PopStyleColor();
    gap();
    ImGui::Separator();
    gap();
    float ceiling = graph_ceiling(report.snapshot.press_stats);
    if (report.snapshot.release_enabled) {
        ceiling = std::max(ceiling, graph_ceiling(report.snapshot.release_stats));
    }
    draw_series("Down press", report.snapshot.press_stats, kGreen, 210, ceiling);
    if (report.snapshot.release_enabled) {
        gap();
        ImGui::Separator();
        gap();
        draw_series("Release", report.snapshot.release_stats, kBlue, 210, ceiling);
    }
    gap();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Y: latency in milliseconds. X: successful sample number. Orange: cumulative running average.");
    ImGui::TextWrapped("Plots show the latest 256 samples. Statistics include every successful sample since Reset averages. "
                      "Timeouts are excluded. Range = maximum - minimum.");
    ImGui::TextWrapped("Software timing estimate. Microphone and audio buffering affect absolute latency; "
                      "use consistent settings when comparing mice.");
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace click_latency
