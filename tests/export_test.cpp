#include "graph_export.h"
#include "gui.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL_opengl.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<char> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void verify_png(const std::string& path, int height) {
    const auto bytes = read_file(path);
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    require(bytes.size() > 8, "export must produce a nonempty PNG");
    for (std::size_t i = 0; i < 8; ++i) {
        require(static_cast<unsigned char>(bytes[i]) == signature[i], "PNG signature");
    }
    SDL_Surface* image = SDL_LoadPNG(path.c_str());
    require(image != nullptr, "PNG must decode with SDL");
    const bool size_matches = image->w == 1200 && image->h == height;
    SDL_DestroySurface(image);
    require(size_matches, "report dimensions must be independent of the small app window");
}

} // namespace

int main() {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    ImGuiContext* gui_context = nullptr;
    std::filesystem::path directory;
    int result = 0;
    try {
        require(SDL_Init(SDL_INIT_VIDEO), "SDL video initialization");
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        window = SDL_CreateWindow("PNG export test", 320, 240, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        require(window != nullptr, "OpenGL test window");
        gl_context = SDL_GL_CreateContext(window);
        require(gl_context != nullptr && SDL_GL_MakeCurrent(window, gl_context), "OpenGL test context");
        gui_context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        ImGui::GetIO().DisplaySize = ImVec2(320, 240);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        require(ImGui_ImplOpenGL3_Init("#version 330"), "main renderer initialization");

        char temporary[] = "/tmp/click-latency-export-test-XXXXXX";
        require(mkdtemp(temporary) != nullptr, "temporary export directory");
        directory = temporary;
        {
            click_latency::UiState ui;
            ui.export_stamp = "20261007-120000";
            std::snprintf(ui.export_label.data(), ui.export_label.size(), "Mouse A");
            click_latency::select_export_directory(ui, (directory / "first").string());
            require(std::filesystem::path(ui.export_path.data()).parent_path() == directory / "first",
                    "folder selection must move the automatic filename to the selected destination");
            require(std::filesystem::path(ui.export_path.data()).filename() == "mouse-a-20261007-120000.png",
                    "folder selection must preserve automatic filename generation");
            ui.export_auto_path = false;
            std::snprintf(ui.export_path.data(), ui.export_path.size(), "%s",
                          (directory / "first" / "custom mouse report.png").c_str());
            click_latency::select_export_directory(ui, (directory / "second").string());
            require(std::filesystem::path(ui.export_path.data()) == directory / "second" / "custom mouse report.png",
                    "choosing a new folder must retain a manually edited filename");
            require(ui.export_locations.recent().size() == 2 && !ui.export_auto_path,
                    "folder selections must be cached while preserving manual filename mode");
            const std::string before = ui.export_path.data();
            const auto history_before = ui.export_locations.recent();
            click_latency::select_export_directory(ui, std::string(ui.export_path.size(), 'x'));
            require(ui.export_failed && ui.export_path.data() == before &&
                    ui.export_locations.recent() == history_before,
                    "overlong paths must report an error without truncating the output or changing history");
        }
        click_latency::ExportReport report;
        report.label = "Mouse A - wired, 1000 Hz, 100% test";
        report.microphone = "Test microphone";
        report.created_at = "2026-10-07 12:00:00 CDT";
        report.snapshot.threshold_db = -30;
        report.snapshot.press_stats = {3, 3, 2, 4, 2, {2, 3, 4}, {2, 2.5f, 3}};
        report.snapshot.release_stats = {3, 5, 3, 7, 4, {3, 5, 7}, {3, 4, 5}};
        report.snapshot.release_enabled = true;

        // Begin a real caller frame. Exporting must leave it usable afterward.
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        glViewport(7, 9, 123, 137);
        glPixelStorei(GL_PACK_ALIGNMENT, 8);
        glPixelStorei(GL_PACK_ROW_LENGTH, 23);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 31);
        glEnable(GL_SCISSOR_TEST);
        glScissor(11, 13, 17, 19);
        glClearColor(0.1f, 0.2f, 0.3f, 0.4f);
        const auto dual = click_latency::save_graph_png(report, (directory / "nested" / "dual").string());
        require(dual.success, dual.error.c_str());
        require(std::filesystem::path(dual.path).extension() == ".png", "missing extension becomes PNG");
        require(ImGui::GetCurrentContext() == gui_context, "original ImGui context must be restored");
        std::array<GLint, 4> viewport{};
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        require(viewport == std::array<GLint, 4>{7, 9, 123, 137}, "caller viewport restoration");
        GLint value = 0;
        glGetIntegerv(GL_PACK_ALIGNMENT, &value);
        require(value == 8, "pack alignment restoration");
        glGetIntegerv(GL_PACK_ROW_LENGTH, &value);
        require(value == 23, "pack row length restoration");
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &value);
        require(value == 31, "unpack row length restoration");
        require(glIsEnabled(GL_SCISSOR_TEST), "caller scissor enable restoration");
        verify_png(dual.path, 1200);
        const auto original_bytes = read_file(dual.path);
        const auto duplicate = click_latency::save_graph_png(report, dual.path);
        require(!duplicate.success && !duplicate.error.empty(), "existing image must produce a useful error");
        require(read_file(dual.path) == original_bytes, "existing image must remain unchanged");
        require(!click_latency::save_graph_png(report, (directory / "wrong.jpg").string()).success,
                "wrong file format must be rejected");
        require(!click_latency::save_graph_png(report, "").success, "empty output path must fail");
        require(!click_latency::save_graph_png(report, (directory / "nested" / "").string()).success,
                "folder-only output path must fail");
        require(!click_latency::save_graph_png(report, dual.path + "/child.png").success,
                "invalid parent path must fail");
        {
            std::ofstream blocker(directory / "parent-file");
            blocker << "keep this";
        }
        require(!click_latency::save_graph_png(report, (directory / "parent-file" / "child.png").string()).success,
                "file cannot become a parent directory");

        report.snapshot.release_enabled = false;
        const auto press = click_latency::save_graph_png(report, (directory / "press.PNG").string());
        require(press.success, press.error.c_str());
        verify_png(press.path, 860);
        // Exercise a wrapping label with the largest value allowed by the GUI.
        report.label.assign(159, 'W');
        const auto long_label = click_latency::save_graph_png(report, (directory / "long-label.png").string());
        require(long_label.success, long_label.error.c_str());
        verify_png(long_label.path, 860);

        // Render the interrupted frame after several exports and failures.
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        ImGui::Begin("Still running");
        ImGui::TextUnformatted("Export leaves the measurement GUI usable.");
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, 320, 240);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        require(ImGui::GetCurrentContext() == gui_context, "caller GUI still active after rendering");
        require(report.snapshot.press_stats.count == 3 && report.snapshot.release_stats.count == 3,
                "export must retain the frozen statistics");
        std::cout << "PNG export tests passed: format, both reports, errors, no overwrite, context/state restoration\n";
    } catch (const std::exception& error) {
        std::cerr << "PNG export test failed: " << error.what() << ": " << SDL_GetError() << '\n';
        result = 1;
    }
    if (gui_context) {
        ImGui::SetCurrentContext(gui_context);
        if (ImGui::GetIO().BackendRendererUserData) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        ImGui::DestroyContext(gui_context);
    }
    if (gl_context) {
        SDL_GL_DestroyContext(gl_context);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    if (!directory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    return result;
}
