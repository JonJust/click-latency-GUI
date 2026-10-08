#include "graph_export.h"

#include "gui.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace click_latency {
namespace {

constexpr int kReportWidth = 1200;
constexpr int kPressReportHeight = 860;
constexpr int kReleaseReportHeight = 1200;

// The ImGui renderer restores its draw state. Also retain the state touched by
// framebuffer setup, font creation, clearing and pixel readback, including the
// uncommon pixel-store settings that otherwise change the image memory layout.
class GlState {
public:
    GlState() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture_);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler_);
        glActiveTexture(static_cast<GLenum>(active_texture_));
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer_);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer_);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer_);
        glGetIntegerv(GL_VIEWPORT, viewport_.data());
        glGetIntegerv(GL_SCISSOR_BOX, scissor_.data());
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color_.data());
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_.data());
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb_);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha_);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation_rgb_);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_equation_alpha_);
        glGetIntegerv(GL_POLYGON_MODE, polygon_mode_.data());
        for (std::size_t i = 0; i < capabilities_.size(); ++i) {
            enabled_[i] = glIsEnabled(capabilities_[i]);
        }
        for (std::size_t i = 0; i < pixel_store_names_.size(); ++i) {
            glGetIntegerv(pixel_store_names_[i], &pixel_store_[i]);
        }
    }

    ~GlState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_framebuffer_));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_framebuffer_));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_));
        glBindSampler(0, static_cast<GLuint>(sampler_));
        glActiveTexture(static_cast<GLenum>(active_texture_));
        glUseProgram(static_cast<GLuint>(program_));
        glBindVertexArray(static_cast<GLuint>(vertex_array_));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer_));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer_));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer_));
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
        glClearColor(clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]);
        glColorMask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
        glBlendEquationSeparate(static_cast<GLenum>(blend_equation_rgb_),
                                static_cast<GLenum>(blend_equation_alpha_));
        glBlendFuncSeparate(static_cast<GLenum>(blend_src_rgb_),
                           static_cast<GLenum>(blend_dst_rgb_),
                           static_cast<GLenum>(blend_src_alpha_),
                           static_cast<GLenum>(blend_dst_alpha_));
        glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(polygon_mode_[0]));
        for (std::size_t i = 0; i < capabilities_.size(); ++i) {
            if (enabled_[i]) {
                glEnable(capabilities_[i]);
            } else {
                glDisable(capabilities_[i]);
            }
        }
        for (std::size_t i = 0; i < pixel_store_names_.size(); ++i) {
            glPixelStorei(pixel_store_names_[i], pixel_store_[i]);
        }
    }

    GlState(const GlState&) = delete;
    GlState& operator=(const GlState&) = delete;

private:
    static constexpr std::array<GLenum, 8> capabilities_ = {
        GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST,
        GL_SCISSOR_TEST, GL_PRIMITIVE_RESTART, GL_DITHER, GL_FRAMEBUFFER_SRGB
    };
    static constexpr std::array<GLenum, 8> pixel_store_names_ = {
        GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS,
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS
    };
    std::array<GLboolean, 8> enabled_{};
    std::array<GLint, 8> pixel_store_{};
    std::array<GLint, 4> viewport_{};
    std::array<GLint, 4> scissor_{};
    std::array<GLfloat, 4> clear_color_{};
    std::array<GLboolean, 4> color_mask_{};
    std::array<GLint, 2> polygon_mode_{};
    GLint draw_framebuffer_ = 0;
    GLint read_framebuffer_ = 0;
    GLint active_texture_ = GL_TEXTURE0;
    GLint texture_ = 0;
    GLint sampler_ = 0;
    GLint program_ = 0;
    GLint vertex_array_ = 0;
    GLint array_buffer_ = 0;
    GLint pack_buffer_ = 0;
    GLint unpack_buffer_ = 0;
    GLint blend_src_rgb_ = GL_ONE;
    GLint blend_dst_rgb_ = GL_ZERO;
    GLint blend_src_alpha_ = GL_ONE;
    GLint blend_dst_alpha_ = GL_ZERO;
    GLint blend_equation_rgb_ = GL_FUNC_ADD;
    GLint blend_equation_alpha_ = GL_FUNC_ADD;
};

class Framebuffer {
public:
    ~Framebuffer() {
        if (framebuffer_) {
            glDeleteFramebuffers(1, &framebuffer_);
        }
        if (texture_) {
            glDeleteTextures(1, &texture_);
        }
    }

    bool create(int width, int height) {
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               texture_, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        return framebuffer_ && texture_ &&
               glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    Framebuffer() = default;

private:
    GLuint framebuffer_ = 0;
    GLuint texture_ = 0;
};

class ReportContext {
public:
    ReportContext() : original_(ImGui::GetCurrentContext()), context_(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(context_);
    }

    ~ReportContext() {
        ImGui::SetCurrentContext(context_);
        if (ImGui::GetIO().BackendRendererUserData) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(original_);
    }

    ReportContext(const ReportContext&) = delete;
    ReportContext& operator=(const ReportContext&) = delete;

private:
    ImGuiContext* original_ = nullptr;
    ImGuiContext* context_ = nullptr;
};

using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
using IoStream = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>;

std::string system_error_text(int error) {
    return std::error_code(error, std::generic_category()).message();
}

class TemporaryFile {
public:
    TemporaryFile(std::vector<char> name, int descriptor)
        : name_(std::move(name)), descriptor_(descriptor) {}

    ~TemporaryFile() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
        unlink(name_.data());
    }

    int descriptor() const { return descriptor_; }
    const char* name() const { return name_.data(); }

    bool finish() {
        const int descriptor = descriptor_;
        // Linux closes the descriptor even when close reports EINTR.
        descriptor_ = -1;
        return close(descriptor) == 0;
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

private:
    std::vector<char> name_;
    int descriptor_ = -1;
};

bool resolve_path(const std::string& requested, ExportResult& result,
                  std::filesystem::path& path) {
    if (requested.empty() || requested.find('\0') != std::string::npos) {
        result.error = "Enter a filename for the PNG image.";
        return false;
    }
    path = std::filesystem::path(requested);
    if (path.filename().empty() || path.filename() == "." || path.filename() == "..") {
        result.error = "Enter a filename, including .png, rather than a folder.";
        return false;
    }
    if (path.extension().empty()) {
        path += ".png";
    } else {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".png") {
            result.error = "Graph images use PNG. Choose a filename ending in .png.";
            return false;
        }
    }
    std::error_code error;
    path = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        result.error = "Cannot resolve the output path: " + error.message();
        return false;
    }
    result.path = path.string();
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        result.error = "Cannot check the output file: " + error.message();
        return false;
    }
    if (std::filesystem::exists(status)) {
        result.error = "That file already exists. Choose a new filename.";
        return false;
    }
    error.clear();
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        result.error = "Cannot create the output folder: " + error.message();
        return false;
    }
    return true;
}

// Encoding finishes in memory before a file is visible. link() publishes the
// finished temporary file atomically and fails if the chosen name already
// exists, including if another process created it during rendering.
bool publish_png(SDL_IOStream* png, const std::filesystem::path& path, std::string& error) {
    const Sint64 length = SDL_GetIOSize(png);
    const auto* bytes = static_cast<const unsigned char*>(SDL_GetPointerProperty(
        SDL_GetIOProperties(png), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
    if (length <= 0 || !bytes) {
        error = "PNG encoding produced no image data.";
        return false;
    }
    std::string temporary = (path.parent_path() / ".click-latency-XXXXXX").string();
    std::vector<char> template_name(temporary.begin(), temporary.end());
    template_name.push_back('\0');
    const int descriptor = mkstemp(template_name.data());
    if (descriptor < 0) {
        error = "Cannot create the PNG file: " + system_error_text(errno);
        return false;
    }
    TemporaryFile temporary_file(std::move(template_name), descriptor);
    bool written = true;
    std::size_t offset = 0;
    const auto size = static_cast<std::size_t>(length);
    while (offset < size) {
        const ssize_t count = write(descriptor, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error = "Cannot write the PNG file: " + system_error_text(count == 0 ? EIO : errno);
            written = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (written && fsync(descriptor) != 0) {
        error = "Cannot finish writing the PNG file: " + system_error_text(errno);
        written = false;
    }
    if (!temporary_file.finish() && written) {
        error = "Cannot close the PNG file: " + system_error_text(errno);
        written = false;
    }
    if (written && link(temporary_file.name(), path.c_str()) != 0) {
        error = errno == EEXIST ? "That file already exists. Choose a new filename."
                               : "Cannot save the PNG file: " + system_error_text(errno);
        written = false;
    }
    return written;
}

} // namespace

ExportResult save_graph_png(const ExportReport& report, const std::string& requested_path) {
    ExportResult result;
    try {
        std::filesystem::path path;
        if (!resolve_path(requested_path, result, path)) {
            return result;
        }
        if (!SDL_GL_GetCurrentContext()) {
            result.error = "An active OpenGL window is required to export the graphs.";
            return result;
        }

        const int height = report.snapshot.release_enabled ? kReleaseReportHeight : kPressReportHeight;
        GlState restore_state;
        GLint maximum_texture_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
        if (maximum_texture_size < std::max(kReportWidth, height)) {
            result.error = "The graphics device cannot render the PNG image size.";
            return result;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        Framebuffer framebuffer;
        if (!framebuffer.create(kReportWidth, height)) {
            result.error = "Cannot create the offscreen image on this graphics device.";
            return result;
        }

        ReportContext export_context;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(kReportWidth), static_cast<float>(height));
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        ImFontConfig font;
        font.SizePixels = 20;
        io.Fonts->AddFontDefaultVector(&font);
        set_style();
        ImGui::GetStyle().FontScaleDpi = 1;
        if (!ImGui_ImplOpenGL3_Init("#version 330") || !ImGui_ImplOpenGL3_CreateDeviceObjects()) {
            result.error = "Cannot initialize the graph image renderer.";
            return result;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        draw_export_report(report, io.DisplaySize);
        ImGui::Render();

        glViewport(0, 0, kReportWidth, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.055f, 0.071f, 0.09f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        const auto pitch = static_cast<std::size_t>(kReportWidth) * 4;
        std::vector<unsigned char> pixels(pitch * static_cast<std::size_t>(height));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadPixels(0, 0, kReportWidth, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (glGetError() != GL_NO_ERROR) {
            result.error = "The graphics device failed to render or read the graph image.";
            return result;
        }
        for (int y = 0; y < height / 2; ++y) {
            auto* top = pixels.data() + static_cast<std::size_t>(y) * pitch;
            auto* bottom = pixels.data() + static_cast<std::size_t>(height - y - 1) * pitch;
            std::swap_ranges(top, top + pitch, bottom);
        }
        Surface surface(SDL_CreateSurfaceFrom(kReportWidth, height, SDL_PIXELFORMAT_RGBA32,
                                              pixels.data(), kReportWidth * 4), SDL_DestroySurface);
        if (!surface) {
            result.error = std::string("Cannot prepare the PNG image: ") + SDL_GetError();
            return result;
        }
        IoStream png(SDL_IOFromDynamicMem(), SDL_CloseIO);
        if (!png || !SDL_SavePNG_IO(surface.get(), png.get(), false)) {
            result.error = std::string("Cannot encode the PNG image: ") + SDL_GetError();
            return result;
        }
        result.success = publish_png(png.get(), path, result.error);
    } catch (const std::exception& error) {
        result.error = std::string("Cannot export the graph image: ") + error.what();
    }
    return result;
}

} // namespace click_latency
