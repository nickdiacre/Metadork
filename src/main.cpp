// Metadork (formerly SDI Metadata Viewer)
// Preview an SDI input on a Blackmagic DeckLink and show / log every ancillary
// packet, with decoders for ARRI extended metadata (FIZ, reel, clip, ...),
// the ARRI record flag, RP 188 timecode, Sony RDD 18 acquisition metadata and Blackmagic camera control.

#include "anc/AncRegistry.h"
#include "anc/CaptureReplay.h"
#include "capture/Capture.h"
#include "log/JsonLogger.h"
#include "out/OutputManager.h"
#include "out/SensorModes.h"
#include "out/Settings.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#include <algorithm>
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include <atomic>
#include <csignal>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

struct PacketStats {
    uint64_t count = 0;
    uint32_t line = 0;
    uint32_t stream = 0;
    std::vector<uint8_t> last;
};

// Per RDD 18 item (set byte << 16 | tag): when its value last changed, to highlight live values.
struct ItemHistory {
    std::vector<uint8_t> value;
    double changedAt = -1e9;
    uint64_t changes = 0;
};
using RddHistory = std::map<uint32_t, ItemHistory>;

struct Shared {
    std::mutex mtx;
    FrameMetadata latest;
    bool hasFrame = false;
    std::map<uint16_t, PacketStats> stats;
    uint64_t frames = 0;
    double fps = 0;
    double fpsWindowStart = 0;
    uint64_t fpsWindowFrames = 0;
    RddHistory rddHistory;
    RddHistory bmdHistory;  // keyed category << 8 | parameter

    void reset() {
        std::lock_guard<std::mutex> l(mtx);
        latest = {};
        hasFrame = false;
        stats.clear();
        frames = 0;
        fps = 0;
        fpsWindowFrames = 0;
        rddHistory.clear();
        bmdHistory.clear();
    }
};

std::string fmt(const char* f, double v) {
    if (std::isnan(v)) return "--";
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

void row(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value.c_str());
}

// "Invert  [ ] pan  [ ] tilt  [ ] roll" for one output; returns true if changed.
bool orientationSigns(const char* id, OrientationSigns& sg) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TextUnformatted("Invert");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("pan", &sg.invertPan);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("tilt", &sg.invertTilt);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("roll", &sg.invertRoll);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sent as tilt + up and roll + anticlockwise seen from behind (what Stage Precision expects).\n"
                          "Invert for receivers that use the opposite convention.");
    ImGui::PopID();
    return changed;
}

// Label / value tables share one fixed label width, so sections line up with each other and a value changing length
// (a clip name, a timecode) never resizes the columns.
bool beginKV(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("Focus position (0 near - 1 far)    ").x);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

std::string focusString(const ArriMetadata& a) {
    if (a.focusInfinite) return "Infinity";
    if (!a.focusValid) return "--";
    char b[64];
    double ft = a.focusMm / 304.8;
    int feet = int(ft);
    double inches = (ft - feet) * 12.0;
    std::snprintf(b, sizeof b, "%.3f m  (%d' %.1f\")", a.focusMm / 1000.0, feet, inches);
    return b;
}

std::string irisString(const ArriMetadata& a) {
    if (std::isnan(a.tStop)) return std::string("-- (") + a.irisState + ")";
    char b[32];
    std::snprintf(b, sizeof b, "T%.1f", a.tStop);
    return b;
}

// ------------------------------------------------------------ preview texture

struct Preview {
    GLuint tex = 0;  // RGBA8 texture drawn by ImGui (the GPU path renders into it, the SDK path uploads into it)
    long w = 0, h = 0;
    BgraFrame* bgra = nullptr;
    IDeckLinkVideoConversion* conv = nullptr;
    bool enabled = true;
    bool gpu = false;        // the last frame went through the shader
    double lastConvert = -1;

    // GPU path for 10-bit YUV (v210), the format the capture asks for: the packed words are uploaded as an RGB10_A2 texture
    // (one texel per 32-bit word, r/g/b = its three 10-bit samples) and a shader unpacks + colour-converts every frame.
    // The SDK's CPU converter needed ~40 ms for a UHD frame (one core), too slow for full frame rate.
    struct Gl {
        GLuint (*CreateShader)(GLenum);
        void (*ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
        void (*CompileShader)(GLuint);
        void (*GetShaderiv)(GLuint, GLenum, GLint*);
        GLuint (*CreateProgram)();
        void (*AttachShader)(GLuint, GLuint);
        void (*LinkProgram)(GLuint);
        void (*GetProgramiv)(GLuint, GLenum, GLint*);
        void (*UseProgram)(GLuint);
        GLint (*GetUniformLocation)(GLuint, const char*);
        void (*Uniform1i)(GLint, GLint);
        void (*Uniform2i)(GLint, GLint, GLint);
        void (*GenVertexArrays)(GLsizei, GLuint*);
        void (*BindVertexArray)(GLuint);
        void (*GenFramebuffers)(GLsizei, GLuint*);
        void (*BindFramebuffer)(GLenum, GLuint);
        void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
        GLenum (*CheckFramebufferStatus)(GLenum);
        void (*DeleteFramebuffers)(GLsizei, const GLuint*);
        void (*DeleteProgram)(GLuint);
        void (*ActiveTexture)(GLenum);
    } gl{};
    bool glOk = false;
    GLuint prog = 0, vao = 0, fbo = 0, srcTex = 0;
    GLint uSrc = -1, uSize = -1, uMatrix = -1;
    long srcW = 0, srcH = 0;

    static constexpr GLenum kRgb10A2 = 0x8059, kUint2101010Rev = 0x8368, kFramebuffer = 0x8D40, kColor0 = 0x8CE0,
                            kComplete = 0x8CD5, kVertex = 0x8B31, kFragment = 0x8B30, kCompiled = 0x8B81,
                            kLinked = 0x8B82, kTexture0 = 0x84C0, kClampToEdge = 0x812F;

    void initGpu() {
        auto load = [](const char* n) { return glfwGetProcAddress(n); };
#define SDIMETA_GL(name) gl.name = reinterpret_cast<decltype(gl.name)>(load("gl" #name)); if (!gl.name) return;
        SDIMETA_GL(CreateShader) SDIMETA_GL(ShaderSource) SDIMETA_GL(CompileShader) SDIMETA_GL(GetShaderiv)
        SDIMETA_GL(CreateProgram) SDIMETA_GL(AttachShader) SDIMETA_GL(LinkProgram) SDIMETA_GL(GetProgramiv)
        SDIMETA_GL(UseProgram) SDIMETA_GL(GetUniformLocation) SDIMETA_GL(Uniform1i) SDIMETA_GL(Uniform2i)
        SDIMETA_GL(GenVertexArrays) SDIMETA_GL(BindVertexArray) SDIMETA_GL(GenFramebuffers) SDIMETA_GL(BindFramebuffer)
        SDIMETA_GL(FramebufferTexture2D) SDIMETA_GL(CheckFramebufferStatus) SDIMETA_GL(DeleteFramebuffers)
        SDIMETA_GL(DeleteProgram) SDIMETA_GL(ActiveTexture)
#undef SDIMETA_GL
        static const char* vs = R"(#version 150
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);  // one triangle covering the viewport
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
        // v210: 4 words carry 6 pixels.  w0 = Cb0 Y0 Cr0 | w1 = Y1 Cb2 Y2 | w2 = Cr2 Y3 Cb4 | w3 = Y4 Cr4 Y5
        static const char* fs = R"(#version 150
uniform sampler2D src;
uniform ivec2 size;   // output width, height
uniform int matrix;   // 0 = Rec.601, 1 = Rec.709, 2 = Rec.2020 (shown as Rec.709)
out vec4 color;
vec3 w(int i, int y) { return texelFetch(src, ivec2(i, y), 0).rgb * 1023.0; }
vec3 toLinear(vec3 v) { return mix(v / 4.5, pow((v + 0.099) / 1.099, vec3(1.0 / 0.45)), step(0.081, v)); }
vec3 toGamma(vec3 v) { return mix(v * 4.5, 1.099 * pow(v, vec3(0.45)) - 0.099, step(0.018, v)); }
void main() {
    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);  // row 0 = top of the picture, as ImGui samples it
    int g = (x / 6) * 4, i = x - (x / 6) * 6;
    vec3 a = w(g, y), b = w(g + 1, y), c = w(g + 2, y), d = w(g + 3, y);
    float Y = i == 0 ? a.g : i == 1 ? b.r : i == 2 ? b.b : i == 3 ? c.g : i == 4 ? d.r : d.b;
    int p = i / 2;
    float Cb = p == 0 ? a.r : p == 1 ? b.g : c.b;
    float Cr = p == 0 ? a.b : p == 1 ? c.r : d.g;
    float yy = (Y - 64.0) / 876.0, cb = (Cb - 512.0) / 896.0, cr = (Cr - 512.0) / 896.0;
    vec3 rgb;
    if (matrix == 0)      rgb = vec3(yy + 1.402 * cr,  yy - 0.344136 * cb - 0.714136 * cr, yy + 1.772 * cb);
    else if (matrix == 1) rgb = vec3(yy + 1.5748 * cr, yy - 0.187324 * cb - 0.468124 * cr, yy + 1.8556 * cb);
    else {
        rgb = vec3(yy + 1.4746 * cr, yy - 0.164553 * cb - 0.571353 * cr, yy + 1.8814 * cb);
        vec3 l = toLinear(clamp(rgb, 0.0, 1.0));
        l = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187) * l;  // BT.2020 -> BT.709
        rgb = toGamma(clamp(l, 0.0, 1.0));
    }
    color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
})";
        auto compile = [&](GLenum type, const char* src) -> GLuint {
            GLuint s = gl.CreateShader(type);
            gl.ShaderSource(s, 1, &src, nullptr);
            gl.CompileShader(s);
            GLint ok = 0;
            gl.GetShaderiv(s, kCompiled, &ok);
            return ok ? s : 0;
        };
        GLuint v = compile(kVertex, vs), f = compile(kFragment, fs);
        if (!v || !f) return;
        prog = gl.CreateProgram();
        gl.AttachShader(prog, v);
        gl.AttachShader(prog, f);
        gl.LinkProgram(prog);
        GLint ok = 0;
        gl.GetProgramiv(prog, kLinked, &ok);
        if (!ok) return;
        uSrc = gl.GetUniformLocation(prog, "src");
        uSize = gl.GetUniformLocation(prog, "size");
        uMatrix = gl.GetUniformLocation(prog, "matrix");
        gl.GenVertexArrays(1, &vao);
        gl.GenFramebuffers(1, &fbo);
        glOk = true;
    }

    void ensureTex(long fw, long fh) {
        if (!tex) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        if (fw != w || fh != h) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            w = fw;
            h = fh;
        }
    }

    // Returns false when the frame can't go through the shader (then the SDK converter is used).
    bool convertGpu(IDeckLinkVideoInputFrame* f) {
        if (!glOk || f->GetPixelFormat() != bmdFormat10BitYUV) return false;
        const long fw = f->GetWidth(), fh = f->GetHeight(), rb = f->GetRowBytes();
        if (rb % 4 || rb / 4 < (fw + 5) / 6 * 4) return false;
        int matrix = fh < 720 ? 0 : 1;
        IDeckLinkVideoFrameMetadataExtensions* meta = nullptr;
        if (f->QueryInterface(IID_IDeckLinkVideoFrameMetadataExtensions, (void**)&meta) == S_OK && meta) {
            int64_t cs = 0;
            if (meta->GetInt(bmdDeckLinkFrameMetadataColorspace, &cs) == S_OK)
                matrix = cs == bmdColorspaceRec2020 ? 2 : cs == bmdColorspaceRec601 ? 0 : 1;
            meta->Release();
        }
        void* bytes = nullptr;
#if SDIMETA_HAS_VIDEO_BUFFER
        IDeckLinkVideoBuffer* buf = nullptr;
        if (f->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&buf) != S_OK || !buf) return false;
        if (buf->StartAccess(bmdBufferAccessRead) != S_OK) { buf->Release(); return false; }
        buf->GetBytes(&bytes);
#else
        f->GetBytes(&bytes);
#endif
        bool ok = bytes != nullptr;
        if (ok) {
            if (!srcTex) {
                glGenTextures(1, &srcTex);
                glBindTexture(GL_TEXTURE_2D, srcTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
            }
            gl.ActiveTexture(kTexture0);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            if (rb / 4 != srcW || fh != srcH) {
                glTexImage2D(GL_TEXTURE_2D, 0, kRgb10A2, rb / 4, fh, 0, GL_RGBA, kUint2101010Rev, bytes);
                srcW = rb / 4;
                srcH = fh;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rb / 4, fh, GL_RGBA, kUint2101010Rev, bytes);
            }
        }
#if SDIMETA_HAS_VIDEO_BUFFER
        buf->EndAccess(bmdBufferAccessRead);
        buf->Release();
#endif
        if (!ok) return false;

        ensureTex(fw, fh);
        gl.BindFramebuffer(kFramebuffer, fbo);
        gl.FramebufferTexture2D(kFramebuffer, kColor0, GL_TEXTURE_2D, tex, 0);
        if (gl.CheckFramebufferStatus(kFramebuffer) != kComplete) {
            gl.BindFramebuffer(kFramebuffer, 0);
            glOk = false;  // fall back to the SDK converter from now on
            return false;
        }
        glViewport(0, 0, GLsizei(fw), GLsizei(fh));
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        gl.UseProgram(prog);
        gl.Uniform1i(uSrc, 0);
        gl.Uniform2i(uSize, GLint(fw), GLint(fh));
        gl.Uniform1i(uMatrix, matrix);
        gl.BindVertexArray(vao);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        gl.BindVertexArray(0);
        gl.UseProgram(0);
        gl.BindFramebuffer(kFramebuffer, 0);
        return true;
    }

    void update(CaptureSession* s) {
        if (!s) return;
        IDeckLinkVideoInputFrame* f = s->takePreviewFrame();
        if (!f) return;
        if (!enabled || (f->GetFlags() & bmdFrameHasNoInputSource)) { f->Release(); return; }
        if (convertGpu(f)) { gpu = true; f->Release(); return; }
        gpu = false;
        // SDK converter (CPU, slow for UHD): limited to ~10 fps so it doesn't stall the UI.
        const double now = glfwGetTime();
        if (!conv || (lastConvert >= 0 && now - lastConvert < 0.1)) { f->Release(); return; }
        lastConvert = now;
        long fw = f->GetWidth(), fh = f->GetHeight();
        if (!bgra || bgra->GetWidth() != fw || bgra->GetHeight() != fh) {
            if (bgra) bgra->Release();
            bgra = new BgraFrame(fw, fh);
        }
        HRESULT hr = conv->ConvertFrame(f, bgra);
        f->Release();
        if (hr != S_OK) return;
        ensureTex(fw, fh);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_BGRA, GL_UNSIGNED_BYTE, bgra->data());
    }

    void release() {
        if (tex) glDeleteTextures(1, &tex);
        if (srcTex) glDeleteTextures(1, &srcTex);
        tex = srcTex = 0;
        if (glOk) { gl.DeleteFramebuffers(1, &fbo); gl.DeleteProgram(prog); }
        if (bgra) bgra->Release();
        bgra = nullptr;
        if (conv) conv->Release();
        conv = nullptr;
    }
};

// ------------------------------------------------------------ UI panels

// --screenshots <dir>: renders each tab in turn and saves it (for docs/MANUAL.md), then quits. Tabs are picked by name.
const char* g_wantTab = nullptr;
const char* g_wantSubTab = nullptr;
ImGuiTabItemFlags tabFlags(const char* name, const char* want) {
    return want && std::strcmp(name, want) == 0 ? ImGuiTabItemFlags_SetSelected : 0;
}


// UI scale (see applyUiScale): fixed pixel sizes go through px().
float g_uiScale = 1.0f;
bool g_rescaleUi = true;
float px(float v) { return v * g_uiScale; }

// Hex dump in a bordered child, with as many bytes per line as fit the width (multiple of 4), so it never scrolls
// sideways. A line is "0000: " + 3 chars per byte + 2 + 1 char per byte.
void drawHex(const char* id, const uint8_t* data, size_t len, float height) {
    ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders);
    const float charW = ImGui::CalcTextSize("0").x;
    const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    size_t perLine = size_t(std::max(1.0f, (avail / charW - 8.0f) / 4.0f));
    perLine = std::clamp<size_t>(perLine / 4 * 4, 4, 32);
    const std::string dump = hexDump(data, len, perLine);
    ImGui::TextUnformatted(dump.c_str());
    ImGui::EndChild();
}

std::string orDash(const std::string& s) { return s.empty() ? "--" : s; }

const ImVec4 kRed(1, 0.4f, 0.4f, 1), kAmber(0.9f, 0.7f, 0.2f, 1), kYellow(1, 0.85f, 0.2f, 1);

void drawCameraBanner(const CameraSample& cs) {
    if (cs.kind == CameraKind::None) {
        ImGui::TextColored(kAmber, "No camera metadata detected");
        return;
    }
    ImGui::Text("%s %s", cs.cameraMake.c_str(), cs.cameraModel.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s%s%s)", cs.source, cs.cameraFirmware.empty() ? "" : ", ", cs.cameraFirmware.c_str());
}

void drawSummary(const FrameMetadata& md, bool hasFrame, const CameraSample& cs) {
    if (!hasFrame) { ImGui::TextDisabled("No frames yet. Choose an input port (capture starts straight away)."); return; }
    drawCameraBanner(cs);

    ImGui::SetWindowFontScale(2.0f);
    ImGui::TextUnformatted(md.tc.str().c_str());
    ImGui::SameLine(0, 30);
    ImVec4 recCol = md.rec == RecState::Recording ? ImVec4(1, 0.2f, 0.2f, 1)
                  : md.rec == RecState::Stopped   ? ImVec4(0.6f, 0.6f, 0.6f, 1)
                                                  : kAmber;
    ImGui::TextColored(recCol, "%s", recStateName(md.rec));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Record state: %s", md.rec == RecState::Unknown ? "no indicator seen yet" : md.recSource.c_str());

    ImGui::Separator();
    if (cs.kind == CameraKind::None) {
        ImGui::TextWrapped("No ARRI, Sony or Blackmagic metadata in this frame. Check the Raw tab. If it lists no "
                           "packets, make sure SDI metadata output is enabled on the camera and that nothing in the "
                           "chain (wireless TX, converters, mixers) strips ANC.");
        return;
    }
    // Grouped by the master registry; hover a value for where it came from
    const auto vars = catalogue(cs);
    int group = -1;
    bool open = false;
    for (const auto& v : vars) {
        if (!std::strcmp(v.id, "timecode") || !std::strcmp(v.id, "recording") || !std::strcmp(v.id, "camera") ||
            !std::strcmp(v.id, "lens"))
            continue;  // shown above / composites of other fields
        if (int(v.group) != group) {
            if (open) ImGui::EndTable();
            group = int(v.group);
            ImGui::SeparatorText(fieldGroupName(v.group));
            open = beginKV(fieldGroupName(v.group));
        }
        if (!open) continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", v.label);
        ImGui::TableSetColumnIndex(1);
        if (v.available) ImGui::TextUnformatted(v.display().c_str());
        else ImGui::TextDisabled("n/a");
        if (v.value && ImGui::IsItemHovered()) {
            if (v.available)
                ImGui::SetTooltip("%s, %s\nfrom %s", valueSourceName(v.value->source), confidenceName(v.value->confidence),
                                  v.value->origin.c_str());
            else if (!v.value->note.empty())
                ImGui::SetTooltip("%s", v.value->note.c_str());
        }
        if (v.available && v.value && v.value->source != ValueSource::Camera) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", v.value->source == ValueSource::Manual ? "manual" : "derived");
        } else if (!v.available && v.value && !v.value->note.empty() && !std::strcmp(v.id, "sensor_height")) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", v.value->note.c_str());
        }
    }
    if (open) ImGui::EndTable();
}

void drawArri(const FrameMetadata& md) {
    const auto& a = md.arri;
    if (beginKV("arri")) {
        row("Camera type", a.cameraTypeName + " (id " + std::to_string(a.cameraTypeId) + ")");
        row("Model / SUP", a.cameraModel + "  " + a.supName);
        row("Serial", std::to_string(a.cameraSerial));
        row("Look file", a.lookFile);
        row("Pixel aspect / squeeze", a.pixelAspect > 0 ? fmt("%.3gx", a.pixelAspect) : std::string("--"));
        row("Reel (new / old)", a.reelName + "  /  " + a.reelNameOld);
        row("Clip (new / old)", a.clipName + "  /  " + a.clipNameOld);
        row("Scene / Take", a.scene + "  /  " + a.take + (a.circleTake ? "  (circled)" : ""));
        row("Focus raw", std::to_string(a.focusRaw) + " mm  (camera display unit: " + (a.lensDistanceUnit == 0 ? "imperial)" : "metric)"));
        row("Focal length", fmt("%.3f mm", a.focalLengthMm));
        row("Iris raw", std::to_string(a.irisRaw) + "  -> " + irisString(a));
        row("Enc focus  LDS / motor", std::to_string(a.rawFocusLds) + "  /  " + std::to_string(a.rawFocusMotor));
        row("Enc zoom   LDS / motor", std::to_string(a.rawFocalLds) + "  /  " + std::to_string(a.rawFocalMotor));
        row("Enc iris   LDS / motor", std::to_string(a.rawIrisLds) + "  /  " + std::to_string(a.rawIrisMotor));
        row("LDS lag (type / frames)", std::to_string(a.ldsLagType) + "  /  " + std::to_string(a.ldsLagFrames));
        row("Raw enc lag (type / frames)", std::to_string(a.rawEncoderLagType) + "  /  " + std::to_string(a.rawEncoderLagFrames));
        row("Effective focal length", a.effectiveFocalLengthUm ? fmt("%.3f mm", a.effectiveFocalLengthUm / 1000.0) : "--");
        row("Entrance pupil position", a.entrancePupilUm ? fmt("%.3f mm", a.entrancePupilUm / 1000.0) : "--");
        row("Lens converter", a.lensConverterModel.empty() ? "--" : a.lensConverterModel + fmt("  x%.3f", a.focalLengthMultiplier));
        row("Exposure time (raw)", std::to_string(a.exposureTimeRaw));
        row("Tilt / roll", (a.hasTilt ? fmt("%.1f", a.tiltDeg) : std::string("--")) + "  /  " +
                               (a.hasRoll ? fmt("%.1f deg", a.rollDeg) : std::string("-- deg")));
        row("Mirror shutter running", a.mirrorShutterRunning ? "yes" : "no");
        row("Vari-frame duplicate", a.variFrameDuplicate ? "YES (duplicate frame)" : "no");
        ImGui::EndTable();
    }
}

void drawAnc(const FrameMetadata& md, const std::map<uint16_t, PacketStats>& stats, int& selected) {
    ImGui::Text("%zu packets in latest frame, %zu packet types seen", md.packets.size(), stats.size());
    if (ImGui::BeginTable("anc", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          ImVec2(0, px(240)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Fixed widths (fitted to the header / typical value) so wrapped text never squeezes the description.
        auto fixed = [](const char* name, const char* sample) {
            ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(sample).x);
        };
        fixed("DID/SDID", "DID/SDID");
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        fixed("Line", "Line");
        fixed("Str", "Str");
        fixed("Bytes", "Bytes");
        fixed("Count", "000000");
        ImGui::TableHeadersRow();
        for (const auto& [key, s] : stats) {
            uint8_t did = key >> 8, sdid = key & 0xFF;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char id[16];
            std::snprintf(id, sizeof id, "%02X / %02X", did, sdid);
            if (ImGui::Selectable(id, selected == key, ImGuiSelectableFlags_SpanAllColumns)) selected = key;
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(describeAnc(did, sdid));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", s.line);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u", s.stream);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%zu", s.last.size());
            ImGui::TableSetColumnIndex(5); ImGui::Text("%llu", (unsigned long long)s.count);
        }
        ImGui::EndTable();
    }

    if (!md.klv.empty() && ImGui::CollapsingHeader("RP 214 KLV objects", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& k : md.klv)
            ImGui::BulletText("MID %u  %s  %zu/%zu bytes  %u segs  %s\n  key %s", k.mid, k.kind.c_str(), k.received,
                              k.declaredLength, k.segments, k.complete ? "complete" : "INCOMPLETE", k.keyHex.c_str());
    }

    auto it = stats.find(uint16_t(selected));
    if (it != stats.end()) {
        ImGui::SeparatorText("Last payload (8-bit UDW)");
        drawHex("hex", it->second.last.data(), it->second.last.size(), 0);
    }
}

void drawRdd18(const FrameMetadata& md, const RddHistory& hist) {
    const auto& r = md.rdd18;
    ImGui::Text("%zu packets, %zu bytes, %zu sets", r.packets, r.stream.size(), r.sets.size());
    if (!md.sonyClip.empty()) { ImGui::SameLine(); ImGui::Text("  |  clip %s", md.sonyClip.c_str()); }
    ImGui::TextDisabled("Yellow = changed in the last 2 s. Hover a value for its raw bytes.");

    for (size_t si = 0; si < r.sets.size(); ++si) {
        const auto& set = r.sets[si];
        char hdr[96];
        std::snprintf(hdr, sizeof hdr, "%s  (set %02X, %zu items%s)##set%zu", rdd18SetName(set.kind), set.setByte,
                      set.items.size(), set.complete ? "" : ", INCOMPLETE", si);
        if (!ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        char tid[16];
        std::snprintf(tid, sizeof tid, "rdd%zu", si);
        if (!ImGui::BeginTable(tid, 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
            continue;
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000").x);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Chg", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000").x);
        ImGui::TableHeadersRow();
        for (const auto& it : set.items) {
            auto h = hist.find(rdd18ItemKey(set, it.tag));
            const bool recent = h != hist.end() && md.hostTime - h->second.changedAt < 2.0;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%04X", it.tag);
            ImGui::TableSetColumnIndex(1);
            if (*it.name) ImGui::TextUnformatted(it.name); else ImGui::TextDisabled("?");
            ImGui::TableSetColumnIndex(2);
            if (recent) ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1), "%s", it.display.c_str());
            else ImGui::TextUnformatted(it.display.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", toHex(it.value.data(), it.value.size()).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", (unsigned long long)(h == hist.end() ? 0 : h->second.changes));
        }
        ImGui::EndTable();
    }
}

void drawBmd(const FrameMetadata& md, const RddHistory& hist) {
    const auto& b = md.bmd;
    ImGui::Text("%zu commands", b.commands.size());
    if (b.hasTally) { ImGui::SameLine(); ImGui::Text("  |  tally packet: %s", toHex(b.tally.data(), b.tally.size()).c_str()); }
    ImGui::TextDisabled("Yellow = changed in the last 2 s. Hover a value for its raw bytes.");
    if (!ImGui::BeginTable("bmdcmds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("Cmd", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00.00").x);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Chg", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000").x);
    ImGui::TableHeadersRow();
    for (const auto& c : b.commands) {
        auto h = hist.find(uint32_t(c.category) << 8 | c.parameter);
        const bool recent = h != hist.end() && md.hostTime - h->second.changedAt < 2.0;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%u.%u", c.category, c.parameter);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bmdCategoryName(c.category));
        ImGui::TableSetColumnIndex(1);
        if (*c.name) ImGui::TextUnformatted(c.name); else ImGui::TextDisabled("? (%s)", bmdCategoryName(c.category));
        ImGui::TableSetColumnIndex(2);
        if (recent) ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1), "%s", c.display.c_str());
        else ImGui::TextUnformatted(c.display.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("type %u, op %u: %s", c.type, c.operation, toHex(c.data.data(), c.data.size()).c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%llu", (unsigned long long)(h == hist.end() ? 0 : h->second.changes));
    }
    ImGui::EndTable();
}

void drawLogging(JsonLogger& logger, char* path, size_t pathSize, bool& raw) {
    ImGui::TextWrapped("Writes one JSON object per captured frame (JSON Lines): timecode, record state, decoded "
                       "ARRI fields and a list of every ANC packet. Enable raw ANC to include payload hex, which is "
                       "what you need for reverse-engineering Sony metadata.");
    bool open = logger.isOpen();
    ImGui::BeginDisabled(open);
    ImGui::InputText("File", path, pathSize);
    if (!open && !std::filesystem::path(path).is_absolute()) ImGui::TextDisabled("Saved as %s", resolveUserPath(path).c_str());
    ImGui::Checkbox("Include raw ANC payloads", &raw);
    ImGui::EndDisabled();
    if (!open) {
        if (ImGui::Button("Start logging") && !logger.open(resolveUserPath(path), raw))
            ImGui::OpenPopup("logerr");
    } else {
        if (ImGui::Button("Stop logging")) logger.close();
        ImGui::SameLine();
        ImGui::Text("%llu frames written to %s", (unsigned long long)logger.linesWritten(), logger.path().c_str());
    }
    if (ImGui::BeginPopup("logerr")) {
        ImGui::Text("Could not open %s", path);
        ImGui::EndPopup();
    }
}


void drawInput(const FrameMetadata& md, bool hasFrame, const CameraSample& cs, const RddHistory& rddHist,
               const RddHistory& bmdHist) {
    if (!hasFrame) { ImGui::TextDisabled("No frames yet."); return; }
    drawCameraBanner(cs);
    ImGui::TextDisabled("Every field decoded from this feed. Yellow = changed in the last 2 s.");

    if (ImGui::CollapsingHeader("Timecode and record state", ImGuiTreeNodeFlags_DefaultOpen) && beginKV("tcrec")) {
        row("Timecode (RP 188)", md.tc.valid ? md.tc.str() + (md.tc.dropFrame ? "  drop frame" : "") : "--");
        if (md.tc.valid) {
            char ub[12];
            std::snprintf(ub, sizeof ub, "%08X", unsigned(md.tc.userBits));
            row("User bits", ub);
        }
        row("Timecode running", md.tcAdvancing ? "yes" : "no");
        row("Record state", std::string(recStateName(md.rec)) + (md.recSource.empty() ? "" : "  (" + md.recSource + ")"));
        if (!md.sonyClip.empty()) row("Clip", md.sonyClip);
        if (!md.sonyUmid.empty()) row("UMID (this take)", md.sonyUmid);
        ImGui::EndTable();
    }
    if (md.hasArri && ImGui::CollapsingHeader("ARRI metadata", ImGuiTreeNodeFlags_DefaultOpen)) drawArri(md);
    if (md.rdd18.parsed() && ImGui::CollapsingHeader("Sony RDD 18", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        drawRdd18(md, rddHist);
        ImGui::Unindent();
    }
    if ((md.bmd.present || md.bmd.hasTally) && ImGui::CollapsingHeader("Blackmagic camera control", ImGuiTreeNodeFlags_DefaultOpen))
        drawBmd(md, bmdHist);
    if (cs.kind == CameraKind::None)
        ImGui::TextColored(kAmber, "No decodable camera metadata. See the Raw tab for the packets that are present.");
}

void drawRaw(const FrameMetadata& md, const std::map<uint16_t, PacketStats>& stats, int& selected) {
    drawAnc(md, stats, selected);
    if (md.rdd18.packets && ImGui::CollapsingHeader("Sony RDD 18 reassembled stream")) {
        drawHex("rddhex", md.rdd18.stream.data(), md.rdd18.stream.size(), px(300));
    }
}

// ------------------------------------------------------------ Output tab

// UI-side copy of the output settings; changes are pushed to the OutputManager automatically (see drawOutput).
struct OutputsUi {
    OutputSettings pending;
    char freedHost[128] = "127.0.0.1";
    char oscHost[128] = "127.0.0.1";
    char otioHost[128] = "127.0.0.1";
    bool dirty = false;
};

void connectionStatus(bool open, const std::string& err, const std::string& dest, int port, uint64_t sent, const char* unit) {
    if (!err.empty()) ImGui::TextColored(kRed, "%s", err.c_str());
    else if (open) ImGui::Text("Sending to %s:%d  |  %llu %s", dest.c_str(), port, (unsigned long long)sent, unit);
    else ImGui::TextDisabled("Not sending");
}

void drawFreeD(OutputsUi& ui, const OutputManager::Status& st, const OutputManager& out) {
    auto& p = ui.pending;
    ui.dirty |= ImGui::Checkbox("Enable FreeD", &p.freedEnabled);
    ui.dirty |= ImGui::InputText("Host##freed", ui.freedHost, sizeof ui.freedHost);
    ui.dirty |= ImGui::InputInt("Port##freed", &p.freedPort, 0);
    int camId = p.freed.cameraId;
    if (ImGui::InputInt("Camera ID", &camId, 1)) { p.freed.cameraId = uint8_t(std::clamp(camId, 0, 255)); ui.dirty = true; }
    ui.dirty |= ImGui::InputDouble("Focus counts per mm", &p.freed.focusCountsPerMm, 0, 0, "%.4f");
    ui.dirty |= ImGui::InputDouble("Zoom counts per mm", &p.freed.zoomCountsPerMm, 0, 0, "%.4f");
    ui.dirty |= ImGui::Checkbox("Iris in spare bytes", &p.freed.irisInSpare);
    if (p.freed.irisInSpare) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ui.dirty |= ImGui::InputDouble("x scale", &p.freed.irisSpareScale, 0, 0, "%.1f");
    }
    if (st.hasLast) {
        const auto pkt = buildFreeD(st.last, p.freed);
        auto u24 = [&](int o) { return (pkt[o] << 16) | (pkt[o + 1] << 8) | pkt[o + 2]; };
        auto s24 = [&](int o) { int v = u24(o); return v & 0x800000 ? v - 0x1000000 : v; };
        ImGui::TextDisabled("Now: focus %d, zoom %d, spare %d, tilt %.2f deg, roll %.2f deg", u24(23), u24(20),
                            (pkt[26] << 8) | pkt[27], s24(5) / 32768.0, s24(8) / 32768.0);
    }
    ui.dirty |= orientationSigns("freedsigns", p.freed.signs);
    ImGui::TextDisabled("Pan/tilt/roll = deg x 32768 (tilt and roll when the camera sends them), X/Y/Z = 0. Infinity focus = 16777215.");
    connectionStatus(st.freedOpen, st.freedError, out.settings().freedHost, out.settings().freedPort, st.freedSent, "packets");
}

void drawOsc(OutputsUi& ui, const OutputManager::Status& st, const OutputManager& out) {
    auto& p = ui.pending;
    ui.dirty |= ImGui::Checkbox("Enable OSC", &p.oscEnabled);
    ui.dirty |= ImGui::InputText("Host##osc", ui.oscHost, sizeof ui.oscHost);
    ui.dirty |= ImGui::InputInt("Port##osc", &p.oscPort, 0);
    ImGui::Text("Address prefix: %s", p.osc.prefix.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(the feed name, set at the top of this panel)");
    int unit = int(p.osc.focusUnit);
    if (ImGui::Combo("Focus unit", &unit, "metres\0millimetres\0feet\0")) { p.osc.focusUnit = FocusUnit(unit); ui.dirty = true; }
    ui.dirty |= ImGui::InputDouble("Infinity sends (m)", &p.osc.infinityMetres, 0, 0, "%.1f");
    ui.dirty |= ImGui::Checkbox("Send as one bundle per frame", &p.osc.bundle);
    ui.dirty |= orientationSigns("oscsigns", p.osc.signs);
    connectionStatus(st.oscOpen, st.oscError, out.settings().oscHost, out.settings().oscPort, st.oscSent, "datagrams");

    ImGui::SeparatorText("Variables");
    const auto vars = catalogue(st.last, p.osc.focusUnit, p.osc.infinityMetres);
    if (ImGui::SmallButton("All")) { p.osc.disabled.clear(); ui.dirty = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) { for (const auto& v : vars) p.osc.disabled.insert(v.id); ui.dirty = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Only available")) {
        p.osc.disabled.clear();
        for (const auto& v : vars) if (!v.available) p.osc.disabled.insert(v.id);
        ui.dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Grey = not supplied by this camera (never sent)");
    if (ImGui::BeginTable("oscvars", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Send", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("string").x);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        const std::string pre = p.osc.prefix;
        for (const auto& v : vars) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool on = p.osc.enabled(v.id);
            ImGui::PushID(v.id);
            if (ImGui::Checkbox("##send", &on)) {
                if (on) p.osc.disabled.erase(v.id); else p.osc.disabled.insert(v.id);
                ui.dirty = true;
            }
            ImGui::PopID();
            ImGui::BeginDisabled(!v.available);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s/%s", pre.c_str(), v.id);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", v.label);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(v.type == VarType::Float ? "float" : v.type == VarType::Int ? "int" : "string");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(v.display().c_str());
            ImGui::EndDisabled();
        }
        ImGui::EndTable();
    }
}

void drawOpenTrackIO(OutputsUi& ui, const OutputManager::Status& st, const OutputManager& out) {
    auto& p = ui.pending;
    ImGui::TextDisabled("SMPTE RIS OSVP OpenTrackIO 1.0.1: JSON samples, one per frame, static data every ~2 s.");
    ui.dirty |= ImGui::Checkbox("Enable OpenTrackIO", &p.otioEnabled);
    if (ImGui::InputInt("Source number (1-200)", &p.otio.sourceNumber, 1)) {
        p.otio.sourceNumber = std::clamp(p.otio.sourceNumber, 1, 200);
        ui.dirty = true;
    }
    int dest = p.otioMulticast ? 0 : 1;
    if (ImGui::RadioButton("Multicast", &dest, 0)) { p.otioMulticast = true; ui.dirty = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", openTrackIOMulticast(p.otio.sourceNumber).c_str());
    if (ImGui::RadioButton("Unicast", &dest, 1)) { p.otioMulticast = false; ui.dirty = true; }
    if (!p.otioMulticast) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ui.dirty |= ImGui::InputText("Host##otio", ui.otioHost, sizeof ui.otioHost);
    }
    ui.dirty |= ImGui::InputInt("Port##otio", &p.otioPort, 0);
    ui.dirty |= ImGui::Checkbox("Send focal length as pinholeFocalLength", &p.otio.focalAsPinhole);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The camera reports the nominal focal length. OpenTrackIO's pinholeFocalLength is meant to be\n"
                          "a calibrated value; turn off if a lens calibration elsewhere supplies it.");
    ui.dirty |= ImGui::Checkbox("Include camera transform (tilt + roll; pan, position = 0)", &p.otio.includeTransform);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off by default: a transform with zero pan and position would move a tracked camera to the\n"
                          "origin in the receiver. Only enable if nothing else supplies the camera transform.");
    ui.dirty |= ImGui::InputDouble("Infinity focus sends (m)", &p.otio.infinityMetres, 0, 0, "%.1f");
    ui.dirty |= orientationSigns("otiosigns", p.otio.signs);
    connectionStatus(st.otioOpen, st.otioError, out.settings().otioDestination(), out.settings().otioPort, st.otioSent, "packets");
    ImGui::TextDisabled("Sends: lens focusDistance, fStop/tStop, pinholeFocalLength; timing incl. timecode; tracker "
                        "recording + slate (clip); static camera make/model/serial, sensor size, frame rate, ISO, shutter.");
    if (!st.otioLastJson.empty() && ImGui::CollapsingHeader("Last sample (JSON)")) {
        ImGui::BeginChild("otiojson", ImVec2(0, px(220)), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", st.otioLastJson.c_str());
        ImGui::EndChild();
    }
}

// Returns true when changed settings were applied (the caller saves settings).
// Values the camera doesn't send over SDI and the user has to supply. Each item shows only when the current camera
// needs it (or a manual value is in force).
struct ManualInputs {
    CameraKind kind = CameraKind::None;
    bool cameraHasSensor = false;   // the camera sends its own sensor size (not derived from the mode table)
    std::string cameraModel;        // for listing the camera's own modes first
    std::string sensorMode;         // mode recognised from the SDI active image (ARRI), or ""
    std::string sensorModeNote;     // why it wasn't recognised
    char* redPosition = nullptr;    // 2-char buffer
    bool redPositionChanged = false;
};

bool drawOutput(OutputManager& out, OutputsUi& ui, JsonLogger& logger, char* logPath, size_t logPathSize, bool& logRaw,
                ManualInputs& manual) {
    const OutputManager::Status st = out.status();
    auto& p = ui.pending;
    if (st.sendErrors) {
        ImGui::TextColored(kRed, "%llu send errors, last: %s", (unsigned long long)st.sendErrors, st.lastSendError.c_str());
    }

    const bool needRed = manual.kind == CameraKind::Red;
    const bool needSensor = (manual.kind != CameraKind::None && !manual.cameraHasSensor) || p.sensorOverride == SensorOverride::Always;
    if ((needRed || needSensor) && ImGui::CollapsingHeader("Values the camera doesn't send", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (needRed) {
            ImGui::SeparatorText("RED clip name");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 2.0f);
            if (ImGui::InputText("Camera position letter", manual.redPosition, 2,
                                 ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank))
                manual.redPositionChanged = true;
            ImGui::TextDisabled("RED clip names use the Camera Position letter (Slate menu), e.g. R003_A012. The camera "
                                "doesn't send it over SDI, so set it to match the camera.");
        }
        if (needSensor) {
            ImGui::SeparatorText("Sensor size");
            const CameraSample& last = st.last;
            ImGui::TextDisabled("Camera / derived: %s",
                                last.sensorSource.empty() ? "none" : (last.hasSensor ? fmt("%.2f", last.sensorWidthMm) + " x " +
                                    fmt("%.2f mm  ", last.sensorHeightMm) + "(" + last.sensorSource + ")" : last.sensorSource).c_str());
            if (!manual.sensorMode.empty())
                ImGui::Text("Recognised mode: %s", manual.sensorMode.c_str());
            else if (!manual.sensorModeNote.empty())
                ImGui::TextWrapped("Mode: %s", manual.sensorModeNote.c_str());
            int mode = int(p.sensorOverride);
            if (ImGui::Combo("Manual size", &mode, "Off\0Use when the camera has none\0Always use\0")) {
                p.sensorOverride = SensorOverride(mode);
                ui.dirty = true;
            }
            std::string presetLabel = p.manualSensorWidthMm > 0 && p.manualSensorHeightMm > 0 ? "Custom size" : "Choose a recording mode...";
            // Show the mode matching the current size (this camera's modes first, as several bodies share sizes)
            const std::string body = sensorBody(manual.cameraModel);
            auto matches = [&](const SensorMode& m) {
                return std::fabs(m.widthMm() - p.manualSensorWidthMm) < 0.005 && std::fabs(m.heightMm() - p.manualSensorHeightMm) < 0.005;
            };
            for (int pass = 0; pass < 2 && presetLabel.rfind("Custom", 0) == 0; ++pass)
                for (const auto& m : sensorModes())
                    if ((body == m.camera) == (pass == 0) && matches(m)) { presetLabel = std::string(m.camera) + "  " + m.name; break; }
            if (ImGui::BeginCombo("Preset", presetLabel.c_str())) {
                // This camera's modes first, then every other body.
                for (int pass = 0; pass < 2; ++pass) {
                    if (pass == 1 && !body.empty()) ImGui::SeparatorText("Other cameras");
                    for (const auto& m : sensorModes()) {
                        if ((body == m.camera) != (pass == 0)) continue;
                        const std::string label = std::string(m.camera) + "  " + m.name + "  " + fmt("%.2f", m.widthMm()) +
                                                  " x " + fmt("%.2f mm", m.heightMm());
                        if (ImGui::Selectable(label.c_str())) {
                            p.manualSensorWidthMm = m.widthMm();
                            p.manualSensorHeightMm = m.heightMm();
                            ui.dirty = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            ui.dirty |= ImGui::InputDouble("Width (mm)", &p.manualSensorWidthMm, 0, 0, "%.3f");
            ui.dirty |= ImGui::InputDouble("Height (mm)", &p.manualSensorHeightMm, 0, 0, "%.3f");
            ImGui::TextDisabled("This camera doesn't send its sensor size (ARRI never does). It is recognised from the SDI "
                                "image when that shows the mode's shape; otherwise pick the recording mode's preset or type "
                                "the active area.");
        }
    }

    if (ImGui::BeginTabBar("outputs")) {
        if (ImGui::BeginTabItem("FreeD", nullptr, tabFlags("FreeD", g_wantSubTab))) { drawFreeD(ui, st, out); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("OSC", nullptr, tabFlags("OSC", g_wantSubTab))) { drawOsc(ui, st, out); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("OpenTrackIO", nullptr, tabFlags("OpenTrackIO", g_wantSubTab))) { drawOpenTrackIO(ui, st, out); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log file", nullptr, tabFlags("Log file", g_wantSubTab))) { drawLogging(logger, logPath, logPathSize, logRaw); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    // Apply changes automatically once nothing is being edited: checkboxes and lists at once, typed fields (hosts,
    // ports, scales) when the field is left, so a half-typed address or port is never used.
    if (ui.dirty && !ImGui::IsAnyItemActive()) {
        p.freedHost = ui.freedHost;
        p.oscHost = ui.oscHost;
        p.otioHost = ui.otioHost;
        out.apply(p);
        ui.dirty = false;
        return true;
    }
    return false;
}

#ifdef __APPLE__
// Keep macOS from App-Napping the viewer (throttling it when its window is hidden or idle), which stalled live
// output. Equivalent to [[NSProcessInfo processInfo] beginActivityWithOptions:UserInitiated|LatencyCritical ...].
void preventAppNap() {
    using MsgId = id (*)(id, SEL);
    using MsgStr = id (*)(id, SEL, const char*);
    using MsgBegin = id (*)(id, SEL, unsigned long long, id);
    id info = reinterpret_cast<MsgId>(objc_msgSend)(reinterpret_cast<id>(objc_getClass("NSProcessInfo")),
                                                   sel_registerName("processInfo"));
    id reason = reinterpret_cast<MsgStr>(objc_msgSend)(reinterpret_cast<id>(objc_getClass("NSString")),
                                                      sel_registerName("stringWithUTF8String:"), "Live SDI metadata output");
    const unsigned long long userInitiated = 0x00FFFFFFULL | (1ULL << 20);  // NSActivityUserInitiated
    const unsigned long long latencyCritical = 0xFF00000000ULL;             // NSActivityLatencyCritical
    id activity = reinterpret_cast<MsgBegin>(objc_msgSend)(info, sel_registerName("beginActivityWithOptions:reason:"),
                                                          userInitiated | latencyCritical, reason);
    reinterpret_cast<MsgId>(objc_msgSend)(activity, sel_registerName("retain"));  // held for the app's lifetime
}
#endif

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }  // Ctrl-C in the terminal: leave the loop normally so settings are saved

// Writes the current framebuffer as an uncompressed 32-bit TGA (bottom-up rows, as glReadPixels returns them).
bool saveFramebufferTga(const std::string& path, int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 8};
    std::fwrite(hdr, 1, sizeof hdr, f);
    std::fwrite(px.data(), 1, px.size(), f);
    std::fclose(f);
    return true;
}

std::mutex g_dropMtx;
std::string g_dropped;  // a file dropped on the window: replayed as a capture
void onDrop(GLFWwindow*, int count, const char** paths) {
    if (count < 1) return;
    std::lock_guard<std::mutex> l(g_dropMtx);
    g_dropped = paths[0];
}

// ------------------------------------------------------------ feeds

// One input: its own capture (or replay), decode state, outputs, log file and preview. Feeds never move once created
// (held by unique_ptr): the capture and replay threads call handle() through a pointer.
struct Feed {
    Shared shared;
    JsonLogger logger;
    OutputManager outputs;
    OutputsUi outputsUi;
    Preview preview;
    CaptureSession* session = nullptr;
    std::unique_ptr<CaptureReplay> replay;
    int deviceIndex = -1;          // input port, -1 = none
    int64_t inputConnection = 1;
    double retryAt = 0;            // next attempt to open the port after a failure
    std::string lastError, replayError, nameError;
    char name[64] = "camera";
    char logPath[512] = "";
    char replayPath[1024] = "";
    bool logRaw = true, recFromTc = true;
    char redPosition[2] = {'A', 0};
    int selectedPacket = -1;

    Feed() {
        preview.conv = dlp::createVideoConversion();
        preview.initGpu();
        replay = std::make_unique<CaptureReplay>([this](FrameMetadata&& md) { handle(std::move(md)); });
    }
    ~Feed() {
        replay->stop();
        stopCapture();
        logger.close();
        preview.release();
    }
    char redLetter() const { return redPosition[0] ? redPosition[0] : 'A'; }
    void stopCapture() {
        if (!session) return;
        session->stop();
        session->Release();
        session = nullptr;
    }

    // Capture / replay thread.
    void handle(FrameMetadata&& md) {
        logger.write(md);
        outputs.onFrame(md);
        std::lock_guard<std::mutex> l(shared.mtx);
        for (const auto& p : md.packets) {
            auto& s = shared.stats[ancKey(p.did, p.sdid)];
            ++s.count;
            s.line = p.line;
            s.stream = p.stream;
            s.last = p.data;
        }
        for (const auto& c : md.bmd.commands) {
            auto& h = shared.bmdHistory[uint32_t(c.category) << 8 | c.parameter];
            if (h.value != c.data) {
                if (!h.value.empty()) { ++h.changes; h.changedAt = md.hostTime; }
                h.value = c.data;
            }
        }
        for (const auto& set : md.rdd18.sets)
            for (const auto& it : set.items) {
                auto& h = shared.rddHistory[rdd18ItemKey(set, it.tag)];
                if (h.value != it.value) {
                    if (!h.value.empty()) { ++h.changes; h.changedAt = md.hostTime; }
                    h.value = it.value;
                }
            }
        ++shared.frames;
        if (md.hostTime - shared.fpsWindowStart >= 1.0) {
            shared.fps = shared.fpsWindowFrames / (md.hostTime - shared.fpsWindowStart);
            shared.fpsWindowStart = md.hostTime;
            shared.fpsWindowFrames = 0;
        }
        ++shared.fpsWindowFrames;
        shared.latest = std::move(md);
        shared.hasFrame = true;
    }

    void load(const FeedSettings& fs) {
        outputs.apply(fs.outputs);
        outputsUi.pending = fs.outputs;
        outputsUi.dirty = false;
        std::snprintf(outputsUi.freedHost, sizeof outputsUi.freedHost, "%s", fs.outputs.freedHost.c_str());
        std::snprintf(outputsUi.oscHost, sizeof outputsUi.oscHost, "%s", fs.outputs.oscHost.c_str());
        std::snprintf(outputsUi.otioHost, sizeof outputsUi.otioHost, "%s", fs.outputs.otioHost.c_str());
        std::snprintf(name, sizeof name, "%s", fs.name().c_str());
        std::snprintf(logPath, sizeof logPath, "%s", fs.logPath.c_str());
        std::snprintf(replayPath, sizeof replayPath, "%s", fs.replayPath.c_str());
        logRaw = fs.logRaw;
        recFromTc = fs.recFromTc;
        redPosition[0] = fs.redPosition;
        inputConnection = fs.inputConnection;
    }

    FeedSettings settings(const std::string& deviceName) const {
        FeedSettings fs;
        fs.outputs = outputs.settings();
        fs.logPath = logPath;
        fs.replayPath = replayPath;
        fs.logRaw = logRaw;
        fs.recFromTc = recFromTc;
        fs.redPosition = redLetter();
        fs.deviceName = deviceName;
        fs.inputConnection = inputConnection;
        return fs;
    }

    // Renames the feed: the OSC prefix follows at once (applied and pending), other unapplied edits stay pending.
    void rename(const std::string& n) {
        OutputSettings s = outputs.settings();
        s.osc.prefix = "/" + n;
        outputs.apply(s);
        outputsUi.pending.osc.prefix = s.osc.prefix;
    }
};

// UI scale for the display's DPI. macOS already works in points (Retina handled by the framebuffer scale), so 1 there;
// Windows / Linux report physical pixels, so the font and style are scaled by the monitor's content scale (150% = 1.5).

void onContentScale(GLFWwindow*, float, float) { g_rescaleUi = true; }

void applyUiScale(GLFWwindow* window, bool rebuildTexture) {
    float scale = 1.0f;
#ifndef __APPLE__
    float sy = 1.0f;
    glfwGetWindowContentScale(window, &scale, &sy);
    if (scale <= 0.0f) scale = 1.0f;
#else
    (void)window;
#endif
    g_uiScale = scale;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = std::round(13.0f * scale);
    io.Fonts->AddFontDefault(&cfg);
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.ScaleAllSizes(scale);
    if (rebuildTexture) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }
}

// Characters allowed in a feed name (it becomes an OSC address part): letters, digits, _ - . and / for sub-levels.
int feedNameFilter(ImGuiInputTextCallbackData* d) {
    const ImWchar c = d->EventChar;
    return (c < 128 && std::isalnum(int(c))) || c == '_' || c == '-' || c == '.' || c == '/' ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    dlp::initialise();
#ifdef __APPLE__
    preventAppNap();
#endif
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);  // Windows / Linux: window size follows the display scale
    // Screenshot mode uses a narrow window: replayed captures have no video, so the preview is kept small.
    bool shotMode = false;
    for (int i = 1; i < argc; ++i) shotMode |= std::strcmp(argv[i], "--screenshots") == 0;
    GLFWwindow* window = glfwCreateWindow(shotMode ? 1000 : 1600, shotMode ? 1000 : 900, "Metadork " SDIMETA_VERSION, nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
#ifndef __APPLE__
    // Scaled to the monitor (e.g. 2400 x 1350 at 150%), the default size can exceed a laptop screen: fit the work area.
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        int mx, my, mw, mh, ww, wh;
        glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
        glfwGetWindowSize(window, &ww, &wh);
        if (mw > 0 && mh > 0 && (ww > mw * 9 / 10 || wh > mh * 9 / 10)) {
            const double k = std::min(mw * 0.9 / ww, mh * 0.9 / wh);
            ww = int(ww * k);
            wh = int(wh * k);
            glfwSetWindowSize(window, ww, wh);
            glfwSetWindowPos(window, mx + (mw - ww) / 2, my + (mh - wh) / 2);
        }
    }
#endif
    glfwMakeContextCurrent(window);
    glfwSetDropCallback(window, onDrop);
    glfwSetWindowContentScaleCallback(window, onContentScale);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");
    applyUiScale(window, false);
    g_rescaleUi = false;

    auto devices = std::make_unique<DeckLinkDevices>();
    std::vector<std::unique_ptr<Feed>> feeds;
    size_t sel = 0;  // feed shown in the right-hand panel

    // Restore the previous session's settings and start any outputs that were enabled.
    const std::string settingsPath = defaultSettingsPath();
    AppSettings app;
    const bool screenshots = shotMode;
    if (screenshots) {
        // Defaults, sending to this machine only: keeps the user's hosts out of the images and off their network.
        auto& o = app.feeds[0].outputs;
        o.freedEnabled = o.oscEnabled = o.otioEnabled = true;
        o.otioMulticast = false;
    } else {
        if (!loadSettings(settingsPath, app)) loadSettings(legacySettingsPath(), app);  // first run after the rename
    }
    auto indexByName = [&](const std::string& name, int fallback) {
        for (size_t i = 0; i < devices->size(); ++i)
            if (devices->ports()[i].name == name) return int(i);
        return fallback;
    };
    auto deviceName = [&](const Feed& f) {
        return f.deviceIndex >= 0 && f.deviceIndex < int(devices->size()) ? devices->ports()[size_t(f.deviceIndex)].name
                                                                             : std::string();
    };
    auto addFeed = [&](const FeedSettings& fs) {
        feeds.push_back(std::make_unique<Feed>());
        Feed& f = *feeds.back();
        f.load(fs);
        f.deviceIndex = fs.deviceName.empty() ? -1 : indexByName(fs.deviceName, -1);
        f.preview.enabled = app.showVideo;
    };
    for (const auto& fs : app.feeds) addFeed(fs);

    // Rescan: re-enumerate and keep every feed on its port (by name). Running sessions hold their own device reference.
    auto rescan = [&] {
        std::vector<std::string> keep;
        for (const auto& f : feeds) keep.push_back(deviceName(*f));
        devices = std::make_unique<DeckLinkDevices>();
        for (size_t i = 0; i < feeds.size(); ++i) feeds[i]->deviceIndex = keep[i].empty() ? -1 : indexByName(keep[i], -1);
    };

    std::string profileMessage;
    double rescanAt = 0;  // after a profile change the card re-enumerates asynchronously
    bool replayAtLaunch = false;
    std::string shotDir, shotPrefix = "shot";
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--replay") {
            std::snprintf(feeds[0]->replayPath, sizeof feeds[0]->replayPath, "%s", argv[++i]);
            replayAtLaunch = true;
        }
        else if (a == "--screenshots") shotDir = argv[++i];
        else if (a == "--screenshot-prefix") shotPrefix = argv[++i];
    }
    struct Shot { const char* tab; const char* sub; const char* name; };
    static const Shot kShots[] = {{"Summary", nullptr, "summary"}, {"Input", nullptr, "input"},
                                  {"Output", "FreeD", "output-freed"}, {"Output", "OSC", "output-osc"},
                                  {"Output", "OpenTrackIO", "output-opentrackio"}, {"Output", "Log file", "output-log"},
                                  {"Raw", nullptr, "raw"}};
    size_t shotIndex = 0;
    double shotAt = 0;
    float panelW = app.panelWidth;
    bool gridView = app.gridView;  // feeds 2 across (quad / side by side) instead of one column
    bool fitFeeds = true;
    int solo = -1;                 // feed shown alone (double-click a tile), -1 = all          // size the feeds panel to fit 4 feeds (at launch and when the layout changes)
    bool showVideo = app.showVideo;

    if (screenshots) panelW = px(760);
    auto save = [&] {
        if (screenshots) return;  // don't overwrite the user's settings
        app.feeds.clear();
        for (const auto& f : feeds) app.feeds.push_back(f->settings(deviceName(*f)));
        app.panelWidth = panelW;
        app.showVideo = showVideo;
        app.gridView = gridView;
        saveSettings(settingsPath, app);
    };

    auto startReplay = [&](Feed& f) {
        f.stopCapture();
        f.shared.reset();
        f.replay->setRecFromTimecode(f.recFromTc);
        f.replay->setRedPositionLetter(f.redLetter());
        if (f.replay->start(f.replayPath, f.replayError)) f.replayError.clear();
        save();
    };
    if (replayAtLaunch) startReplay(*feeds[0]);

    // Another feed capturing from this port, or nullptr.
    auto portUser = [&](int port, const Feed* except) -> const Feed* {
        for (const auto& f : feeds)
            if (f.get() != except && f->session && f->deviceIndex == port) return f.get();
        return nullptr;
    };

    // Opens the feed's chosen port. Failures are shown and retried (e.g. Stage Precision holding the port).
    auto startCapture = [&](Feed& f) {
        f.retryAt = glfwGetTime() + 3.0;
        if (f.deviceIndex < 0 || f.deviceIndex >= int(devices->size())) return;
        const DeckLinkPort& port = devices->ports()[size_t(f.deviceIndex)];
        if (!port.active()) { f.lastError = "Port inactive in the card's current profile."; return; }
        if (const Feed* user = portUser(f.deviceIndex, &f)) { f.lastError = std::string("Port in use by /") + user->name + "."; return; }
        if (!(port.connections & f.inputConnection))
            f.inputConnection = (port.connections & bmdVideoConnectionSDI) ? int64_t(bmdVideoConnectionSDI) : int64_t(port.connections & -port.connections);
        f.shared.reset();
        f.session = CaptureSession::create(devices->device(size_t(f.deviceIndex)), [p = &f](FrameMetadata&& m) {
            p->handle(std::move(m));
        });
        f.session->setRecFromTimecode(f.recFromTc);
        f.session->setRedPositionLetter(f.redLetter());
        if (!f.session->start(f.lastError, f.inputConnection)) { f.session->Release(); f.session = nullptr; }
        else f.lastError.clear();
    };

    while (!glfwWindowShouldClose(window) && !g_quit) {
        glfwPollEvents();
        if (sel >= feeds.size()) sel = feeds.size() - 1;
        Feed& F = *feeds[sel];
        {
            std::lock_guard<std::mutex> l(g_dropMtx);
            if (!g_dropped.empty()) {
                std::snprintf(F.replayPath, sizeof F.replayPath, "%s", g_dropped.c_str());
                g_dropped.clear();
                startReplay(F);
            }
        }
        for (auto& f : feeds) {
            // Keep each feed capturing from its chosen port (at launch, after a port / connector / profile change,
            // and retrying after a failure), except while it replays a capture file.
            if (!screenshots && f->deviceIndex >= 0 && !f->session && !f->replay->running() && rescanAt == 0 &&
                glfwGetTime() >= f->retryAt)
                startCapture(*f);
            f->preview.enabled = showVideo;
            f->preview.update(f->session);
            const InputStatus fst = f->session ? f->session->status() : InputStatus{};
            if (fst.rateNum > 0) f->outputs.setVideoRate(fst.rateNum, fst.rateDen);
            if (f->replay->running()) {
                int num, den;
                f->replay->videoRate(num, den);
                f->outputs.setVideoRate(num, den);
            }
        }

        if (g_rescaleUi) {  // window moved to a monitor with a different scale
            g_rescaleUi = false;
            applyUiScale(window, true);
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Snapshot the selected feed's shared state for this UI frame.
        FrameMetadata md;
        std::map<uint16_t, PacketStats> stats;
        RddHistory rddHist, bmdHist;
        bool hasFrame;
        uint64_t frames;
        {
            std::lock_guard<std::mutex> l(F.shared.mtx);
            md = F.shared.latest;
            stats = F.shared.stats;
            rddHist = F.shared.rddHistory;
            bmdHist = F.shared.bmdHistory;
            hasFrame = F.shared.hasFrame;
            frames = F.shared.frames;
        }
        CaptureSession*& session = F.session;
        CaptureReplay& replay = *F.replay;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float headerH = ImGui::GetTextLineHeightWithSpacing();  // feed tile header line
        if (fitFeeds && !screenshots) {
            // Feeds panel width at which 4 feeds fill its height without scrolling: 4 rows in a column, 2 x 2 in grid.
            const int cols = gridView ? 2 : 1, rows = 4 / cols;
            const float h = vp->WorkSize.y - style.WindowPadding.y * 2 - ImGui::GetFrameHeightWithSpacing();
            const float tileH = (h - style.ItemSpacing.y * float(rows - 1)) / float(rows);
            const float picH = std::floor(tileH - style.WindowPadding.y * 2 - headerH) - 1.0f;
            const float tileW = picH * 16.0f / 9.0f + style.WindowPadding.x * 2;
            const float leftW = tileW * float(cols) + style.ItemSpacing.x * float(cols - 1) + style.WindowPadding.x * 2 + 2.0f;
            panelW = vp->WorkSize.x - leftW;
            fitFeeds = false;
        }
        panelW = std::clamp(panelW, px(320), std::max(px(320), vp->WorkSize.x - px(240)));
        // NoBringToFrontOnFocus keeps the splitter (drawn last) on top of both panels.
        const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

        // Feeds, stacked vertically; click one to show it in the right-hand panel
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - panelW, vp->WorkSize.y));
        ImGui::Begin("Feeds", nullptr, fixed);  // scrolls when the tiles don't fit
        if (ImGui::Checkbox("Show video", &showVideo)) save();
        ImGui::SameLine();
        if (ImGui::Button("+ Add feed")) {
            std::vector<FeedSettings> existing;
            for (const auto& f : feeds) existing.push_back(f->settings(deviceName(*f)));
            FeedSettings fs = newFeedSettings(existing, F.settings(""));
            addFeed(fs);  // no input port yet: the user picks one
            sel = feeds.size() - 1;
            save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds another input. It copies this feed's output settings with a new name (OSC prefix),\n"
                              "the next free FreeD camera ID and OpenTrackIO source number, and its own log file.");
        ImGui::SameLine();
        if (solo >= int(feeds.size())) solo = -1;
        if (solo >= 0) {
            if (ImGui::Button("Show all feeds")) solo = -1;
            ImGui::SameLine();
            ImGui::TextDisabled("(or double-click the feed)");
        } else if (ImGui::Button(gridView ? "Column view" : "Grid view")) {
            gridView = !gridView;
            fitFeeds = true;  // resize the panel to suit the new layout from the next frame
            save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(gridView ? "Stack the feeds in one column" : "Show the feeds two across (side by side / quad view)");
        {
            // Tiles: a header line plus a 16:9 picture area, in one column or two across
            const int cols = gridView && solo < 0 ? 2 : 1;
            const ImVec2 area = ImGui::GetContentRegionAvail();
            const float tileW = (area.x - style.ItemSpacing.x * float(cols - 1)) / float(cols);
            for (size_t i = 0, shown = 0; i < feeds.size(); ++i) {
                if (solo >= 0 && int(i) != solo) continue;  // soloed: only that feed, filling the panel
                if (shown++ % size_t(cols)) ImGui::SameLine();
                Feed& f = *feeds[i];
                const bool isSel = i == sel && feeds.size() > 1;
                ImGui::PushID(int(i));
                if (isSel) {
                    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
                }
                const float tileH = solo >= 0 ? area.y
                                              : style.WindowPadding.y * 2 + headerH + (tileW - style.WindowPadding.x * 2) * 9.0f / 16.0f;
                ImGui::BeginChild("tile", ImVec2(tileW, tileH), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                const InputStatus fst = f.session ? f.session->status() : InputStatus{};
                RecState rec;
                std::string tc;
                double fps;
                bool fHas;
                {
                    std::lock_guard<std::mutex> l(f.shared.mtx);
                    rec = f.shared.latest.rec;
                    tc = f.shared.latest.tc.str();
                    fps = f.shared.fps;
                    fHas = f.shared.hasFrame;
                }
                ImGui::TextColored(kYellow, "/%s", f.name);
                ImGui::SameLine();
                if (f.session) {
                    ImGui::TextDisabled("%s", deviceName(f).c_str());
                    ImGui::SameLine();
                    if (fst.running && !fst.noSignal)
                        ImGui::Text("%s  %ldx%ld  %s  %.2f fps", fst.modeName.c_str(), fst.width, fst.height,
                                    fst.rgb444 ? "RGB 4:4:4" : "YUV 4:2:2", fps);
                } else if (f.replay->running()) {
                    ImGui::TextDisabled("Replaying %s", std::filesystem::path(f.replay->path()).filename().string().c_str());
                } else if (f.deviceIndex >= 0 && !f.lastError.empty()) {
                    ImGui::TextColored(kAmber, "%s: %s", deviceName(f).c_str(), f.lastError.c_str());
                } else {
                    ImGui::TextDisabled(f.deviceIndex >= 0 ? "Connecting..." : "No input selected");
                }
                if (fHas && (f.session || f.replay->running())) {
                    ImGui::SameLine();
                    ImGui::TextUnformatted(tc.c_str());
                    if (rec == RecState::Recording) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "REC"); }
                }
                ImVec2 avail = ImGui::GetContentRegionAvail();
                if (f.session && fst.running && fst.noSignal) {
                    ImGui::TextColored(kRed, "NO SIGNAL");
                } else if (f.session && f.preview.tex && showVideo && f.preview.w > 0) {
                    const float aspect = float(f.preview.w) / float(f.preview.h);
                    ImVec2 size(avail.x, avail.x / aspect);
                    if (size.y > avail.y) size = ImVec2(avail.y * aspect, avail.y);
                    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f, ImGui::GetCursorPosY()));
                    ImGui::Image((ImTextureID)(intptr_t)f.preview.tex, size);
                } else if (f.replay->running()) {
                    ImGui::TextDisabled("Replaying a capture: metadata only (capture files hold no video).");
                } else if (f.session && showVideo) {
                    ImGui::TextDisabled(f.preview.conv ? "No video" : "Video converter unavailable (DeckLink driver missing?)");
                }
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) sel = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) solo = solo >= 0 ? -1 : int(i);  // solo / unsolo
                }
                ImGui::EndChild();
                if (isSel) { ImGui::PopStyleVar(); ImGui::PopStyleColor(); }
                ImGui::PopID();
            }
        }
        ImGui::End();

        // Metadata panel (selected feed)
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - panelW, vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(panelW, vp->WorkSize.y));
        ImGui::Begin("Metadata", nullptr, fixed);
        ImGui::PushTextWrapPos(0.0f);  // wrap all text at the window / table-column edge instead of overflowing

        // Feed name = OSC address prefix
        {
            ImGui::PushID(int(sel));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            ImGui::InputText("Feed name", F.name, sizeof F.name, ImGuiInputTextFlags_CallbackCharFilter, feedNameFilter);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string n = F.name;
                while (!n.empty() && n.front() == '/') n.erase(n.begin());
                while (!n.empty() && n.back() == '/') n.pop_back();
                bool dup = false;
                for (const auto& f : feeds) dup |= f.get() != &F && std::string(f->name) == n;
                const std::string current = F.outputs.settings().osc.prefix.substr(1);
                if (n.empty() || dup) {
                    F.nameError = n.empty() ? "The name can't be empty." : "Another feed already uses /" + n + ".";
                    std::snprintf(F.name, sizeof F.name, "%s", current.c_str());
                } else {
                    F.nameError.clear();
                    std::snprintf(F.name, sizeof F.name, "%s", n.c_str());
                    if (n != current) { F.rename(n); save(); }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Advertised name: OSC addresses are /<name>/... (e.g. /camera/focus_distance).");
            ImGui::SameLine();
            ImGui::TextDisabled("OSC /%s/...", F.name);
            if (feeds.size() > 1) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove feed")) ImGui::OpenPopup("removefeed");
                if (ImGui::BeginPopup("removefeed")) {
                    ImGui::Text("Remove /%s? Its capture and outputs stop.", F.name);
                    if (ImGui::Button("Remove")) {
                        feeds.erase(feeds.begin() + std::ptrdiff_t(sel));
                        sel = std::min(sel, feeds.size() - 1);
                        save();
                        ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                        ImGui::PopID();
                        ImGui::PopTextWrapPos();
                        ImGui::End();
                        ImGui::EndFrame();
                        continue;  // the selected feed is gone: start the frame again
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            if (!F.nameError.empty()) ImGui::TextColored(kRed, "%s", F.nameError.c_str());
            // IDs a receiver tells cameras apart by must differ between feeds sending to it
            if (feeds.size() > 1) {
                const OutputSettings& me = F.outputs.settings();
                for (const auto& f : feeds) {
                    if (f.get() == &F) continue;
                    const OutputSettings& o = f->outputs.settings();
                    if (me.freedEnabled && o.freedEnabled && me.freed.cameraId == o.freed.cameraId)
                        ImGui::TextColored(kAmber, "FreeD camera ID %d is also used by /%s.", int(me.freed.cameraId), f->name);
                    if (me.otioEnabled && o.otioEnabled && me.otio.sourceNumber == o.otio.sourceNumber)
                        ImGui::TextColored(kAmber, "OpenTrackIO source number %d is also used by /%s.", me.otio.sourceNumber, f->name);
                }
            }
            ImGui::PopID();
        }
        ImGui::Separator();

        if (rescanAt > 0 && glfwGetTime() >= rescanAt) {
            rescan();
            rescanAt = 0;
        }
        int& deviceIndex = F.deviceIndex;
        int64_t& inputConnection = F.inputConnection;
        if (!devices->driverPresent()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Blackmagic Desktop Video driver not found.");
        } else if (devices->size() == 0) {
            ImGui::TextDisabled("No DeckLink inputs found.");
            if (ImGui::Button("Rescan")) rescan();
        } else {
            const auto& ports = devices->ports();
            if (deviceIndex >= int(ports.size())) deviceIndex = -1;

            // Port (sub-device) list. Choosing a port captures from it straight away; None stops. Ports inactive in the
            // card's current profile, or captured by another feed, can't be chosen.
            const std::string current = deviceIndex >= 0 ? ports[size_t(deviceIndex)].label() : "None (not capturing)";
            if (ImGui::BeginCombo("Input port", current.c_str())) {
                if (ImGui::Selectable("None (not capturing)", deviceIndex < 0) && deviceIndex >= 0) {
                    deviceIndex = -1;
                    F.stopCapture();
                    F.lastError.clear();
                    save();
                }
                for (int i = 0; i < int(ports.size()); ++i) {
                    const Feed* user = portUser(i, &F);
                    ImGui::BeginDisabled(!ports[size_t(i)].active() || user);
                    std::string label = ports[size_t(i)].label();
                    if (user) label += std::string("  (used by /") + user->name + ")";
                    if (ImGui::Selectable((label + "##port" + std::to_string(i)).c_str(), i == deviceIndex) && i != deviceIndex) {
                        deviceIndex = i;
                        F.stopCapture();  // the new port is opened below (reconnect)
                        F.retryAt = 0;
                        save();
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Rescan")) rescan();

            if (deviceIndex >= 0) {
                const DeckLinkPort& port = devices->ports()[size_t(deviceIndex)];

                // Connector, from what this port advertises; changing it reopens the input
                std::vector<int64_t> connectors;
                for (int64_t bit = 1; bit <= (int64_t(1) << 8); bit <<= 1)
                    if (port.connections & bit) connectors.push_back(bit);
                if (!(port.connections & inputConnection))
                    inputConnection = (port.connections & bmdVideoConnectionSDI) ? bmdVideoConnectionSDI : connectors.front();
                if (connectors.size() > 1) {
                    if (ImGui::BeginCombo("Connector", videoConnectionName(inputConnection))) {
                        for (int64_t c : connectors)
                            if (ImGui::Selectable(videoConnectionName(c), c == inputConnection) && c != inputConnection) {
                                inputConnection = c;
                                F.stopCapture();
                                F.retryAt = 0;
                                save();
                            }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::TextDisabled("Connector: %s", videoConnectionName(inputConnection));
                }
                ImGui::TextDisabled("%s  |  ARRI record flag (HANC): %s", port.model.empty() ? "DeckLink" : port.model.c_str(),
                                    port.hancInput ? "can capture" : "not supported by this device");

                // Card profile (multi-port cards): decides how many ports exist and whether they are full or half duplex
                const auto profiles = devices->profiles(size_t(deviceIndex));
                if (profiles.size() > 1) {
                    static int64_t pending = 0;
                    int64_t activeId = 0;
                    for (const auto& pr : profiles) if (pr.active) activeId = pr.id;
                    if (!pending) pending = activeId;
                    ImGui::BeginDisabled(rescanAt > 0);
                    if (ImGui::BeginCombo("Card profile", profileIdName(pending))) {
                        for (const auto& pr : profiles)
                            if (ImGui::Selectable((pr.name + (pr.active ? "  (active)" : "")).c_str(), pr.id == pending)) pending = pr.id;
                        ImGui::EndCombo();
                    }
                    if (pending != activeId) {
                        ImGui::SameLine();
                        if (ImGui::Button("Activate")) {
                            F.stopCapture();  // reconnects after the card has re-enumerated
                            std::string err;
                            if (devices->activateProfile(size_t(deviceIndex), pending, err)) {
                                profileMessage = std::string("Switching card to: ") + profileIdName(pending);
                                rescanAt = glfwGetTime() + 1.5;
                            } else {
                                profileMessage = err;
                            }
                            pending = 0;
                        }
                        ImGui::TextColored(kAmber, "Changes the whole card, including other feeds and any other app using it "
                                                   "(e.g. Stage Precision).");
                    }
                    ImGui::EndDisabled();
                    if (!profileMessage.empty()) ImGui::TextDisabled("%s", profileMessage.c_str());
                }
                if (!port.active())
                    ImGui::TextColored(kAmber, "This port is inactive in the card's current profile. Pick an active port, or "
                                               "a profile with more ports.");
            }
            if (replay.running() && deviceIndex >= 0)
                ImGui::TextColored(kAmber, "Paused while a capture file replays; stop the replay to resume.");
            else if (!F.lastError.empty())
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s  (retrying)", F.lastError.c_str());
            if (session) ImGui::Text("Frames: %llu", (unsigned long long)frames);
            if (ImGui::Checkbox("Record state from running timecode", &F.recFromTc)) {
                if (session) session->setRecFromTimecode(F.recFromTc);
                replay.setRecFromTimecode(F.recFromTc);
                save();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Used when the camera sends no record flag (Sony), and only after timecode has been\n"
                                  "seen stopped (proving Rec Run TC). In Free Run it is never used.");
        }

        // Replay a recorded capture (sdimeta_probe --raw) instead of the live input
        if (ImGui::CollapsingHeader("Replay a capture file", replay.running() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::TextDisabled("Plays back a capture recorded with sdimeta_probe --raw (e.g. the sample captures)\n"
                                "through the same decoding and outputs, in a loop, pausing this feed's live input.\n"
                                "You can also drop a file on the window (it replays into the selected feed).");
            ImGui::BeginDisabled(replay.running());
            ImGui::InputText("Capture file", F.replayPath, sizeof F.replayPath);
            ImGui::EndDisabled();
            if (!replay.running()) {
                if (ImGui::Button("Replay")) startReplay(F);
            } else {
                if (ImGui::Button("Stop replay")) { replay.stop(); F.shared.reset(); F.retryAt = 0; }
                ImGui::SameLine();
                ImGui::Text("Frame %zu / %zu  (%.0f s at %.2f fps, looping)", replay.position() + 1, replay.frameCount(),
                            replay.duration(), replay.fps());
            }
            if (!F.replayError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", F.replayError.c_str());
        }

        ImGui::Separator();
        CameraSample cs = sampleFrom(md);
        ManualInputs manual;
        manual.kind = cs.kind;
        manual.cameraHasSensor = cs.hasSensor && cs.record.get(Field::SensorWidth).source == ValueSource::Camera;
        manual.cameraModel = cs.record.str(Field::CameraModel);
        manual.sensorMode = cs.record.str(Field::SensorMode);
        manual.sensorModeNote = cs.record.get(Field::SensorMode).note;
        manual.redPosition = F.redPosition;
        applySensorOverride(cs, F.outputs.settings());
        ImGui::PushID(int(sel));  // per-feed widget state (tables, collapsing headers)
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Summary", nullptr, tabFlags("Summary", g_wantTab))) { drawSummary(md, hasFrame, cs); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Input", nullptr, tabFlags("Input", g_wantTab))) { drawInput(md, hasFrame, cs, rddHist, bmdHist); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Output", nullptr, tabFlags("Output", g_wantTab))) {
                if (drawOutput(F.outputs, F.outputsUi, F.logger, F.logPath, sizeof F.logPath, F.logRaw, manual)) save();
                if (manual.redPositionChanged) {
                    if (session) session->setRedPositionLetter(F.redLetter());
                    replay.setRedPositionLetter(F.redLetter());
                    save();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Raw", nullptr, tabFlags("Raw", g_wantTab))) { drawRaw(md, stats, F.selectedPacket); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::PopID();
        ImGui::PopTextWrapPos();
        ImGui::End();

        // Drag handle on the panel's left edge
        const float grip = px(8);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - panelW - grip * 0.5f, vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(grip, vp->WorkSize.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        // Without this ImGui widens the window to its 32 px minimum, and the invisible splitter (on top of everything)
        // swallowed clicks on the checkboxes and arrows along the right panel's left edge.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
        ImGui::Begin("##splitter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::InvisibleButton("##drag", ImVec2(grip, vp->WorkSize.y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) panelW -= ImGui::GetIO().MouseDelta.x;
        if (ImGui::IsItemDeactivated()) save();
        ImGui::End();
        ImGui::PopStyleVar(3);

        ImGui::Render();
        int fbw, fbh;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.08f, 0.08f, 0.09f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (screenshots) {
            // Each tab is shown for 3 s (replayed data settles, tab switch takes a frame), then saved.
            const double now = glfwGetTime();
            if (shotAt == 0) shotAt = now + 3.0;
            g_wantTab = kShots[shotIndex].tab;
            g_wantSubTab = kShots[shotIndex].sub;
            if (now >= shotAt) {
                const std::string file = shotDir + "/" + shotPrefix + "-" + kShots[shotIndex].name + ".tga";
                std::printf("%s %s\n", saveFramebufferTga(file, fbw, fbh) ? "saved" : "FAILED", file.c_str());
                shotAt = now + 1.5;
                if (++shotIndex == std::size(kShots)) g_quit = true;
            }
        }
        glfwSwapBuffers(window);
    }

    save();
    feeds.clear();  // stops captures / replays, closes logs, frees GL textures (context still current)
    devices.reset();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    dlp::shutdown();
    return 0;
}
