// SDI Metadata Viewer
// Preview an SDI input on a Blackmagic DeckLink and show / log every ancillary
// packet, with decoders for ARRI extended metadata (FIZ, reel, clip, ...),
// the ARRI record flag, RP 188 timecode, Sony RDD 18 acquisition metadata and Blackmagic camera control.

#include "anc/AncRegistry.h"
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
#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstring>
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

bool beginKV(const char* id) {
    return ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp);
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
    GLuint tex = 0;
    long w = 0, h = 0;
    BgraFrame* bgra = nullptr;
    IDeckLinkVideoConversion* conv = nullptr;
    bool enabled = true;

    void update(CaptureSession* s) {
        if (!s) return;
        IDeckLinkVideoInputFrame* f = s->takePreviewFrame();
        if (!f) return;
        if (!enabled || !conv || (f->GetFlags() & bmdFrameHasNoInputSource)) { f->Release(); return; }
        long fw = f->GetWidth(), fh = f->GetHeight();
        if (!bgra || fw != w || fh != h) {
            if (bgra) bgra->Release();
            bgra = new BgraFrame(fw, fh);
        }
        HRESULT hr = conv->ConvertFrame(f, bgra);
        f->Release();
        if (hr != S_OK) return;

        if (!tex) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (fw != w || fh != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra->data());
            w = fw;
            h = fh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_BGRA, GL_UNSIGNED_BYTE, bgra->data());
        }
    }

    void release() {
        if (tex) glDeleteTextures(1, &tex);
        tex = 0;
        if (bgra) bgra->Release();
        bgra = nullptr;
        if (conv) conv->Release();
        conv = nullptr;
    }
};

// ------------------------------------------------------------ UI panels

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
    if (!hasFrame) { ImGui::TextDisabled("No frames yet. Choose an input and press Start capture."); return; }
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
    if (ImGui::BeginTable("summary", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& v : catalogue(cs)) {
            if (!std::strcmp(v.id, "timecode") || !std::strcmp(v.id, "recording") || !std::strcmp(v.id, "camera")) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", v.label);
            ImGui::TableSetColumnIndex(1);
            const bool sensorRow = !std::strcmp(v.id, "sensor_width") || !std::strcmp(v.id, "sensor_height");
            if (v.available) ImGui::TextUnformatted(v.display().c_str());
            else ImGui::TextDisabled("n/a");
            if (sensorRow && !cs.sensorSource.empty() && !std::strcmp(v.id, "sensor_height")) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", cs.sensorSource.c_str());
            }
        }
        ImGui::EndTable();
    }
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
        row("Focus raw / unit", std::to_string(a.focusRaw) + (a.lensDistanceUnit == 0 ? "  (1/1000 in)" : "  (mm)"));
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
                          ImVec2(0, 240))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("DID/SDID");
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Line");
        ImGui::TableSetupColumn("Str");
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupColumn("Count");
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
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Chg", ImGuiTableColumnFlags_WidthFixed);
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
    ImGui::TableSetupColumn("Cmd", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Chg", ImGuiTableColumnFlags_WidthFixed);
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
    ImGui::Checkbox("Include raw ANC payloads", &raw);
    ImGui::EndDisabled();
    if (!open) {
        if (ImGui::Button("Start logging") && !logger.open(path, raw))
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
        drawHex("rddhex", md.rdd18.stream.data(), md.rdd18.stream.size(), 300);
    }
}

// ------------------------------------------------------------ Output tab

// UI-side copy of the output settings; Apply pushes it to the OutputManager.
struct OutputsUi {
    OutputSettings pending;
    char freedHost[128] = "127.0.0.1";
    char oscHost[128] = "127.0.0.1";
    char oscPrefix[64] = "/camera";
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
    ImGui::TextDisabled("Pan/tilt/roll = deg x 32768 (tilt and roll from Sony when present), X/Y/Z = 0. Infinity focus = 16777215.");
    connectionStatus(st.freedOpen, st.freedError, out.settings().freedHost, out.settings().freedPort, st.freedSent, "packets");
}

void drawOsc(OutputsUi& ui, const OutputManager::Status& st, const OutputManager& out) {
    auto& p = ui.pending;
    ui.dirty |= ImGui::Checkbox("Enable OSC", &p.oscEnabled);
    ui.dirty |= ImGui::InputText("Host##osc", ui.oscHost, sizeof ui.oscHost);
    ui.dirty |= ImGui::InputInt("Port##osc", &p.oscPort, 0);
    ui.dirty |= ImGui::InputText("Address prefix", ui.oscPrefix, sizeof ui.oscPrefix);
    int unit = int(p.osc.focusUnit);
    if (ImGui::Combo("Focus unit", &unit, "metres\0millimetres\0feet\0")) { p.osc.focusUnit = FocusUnit(unit); ui.dirty = true; }
    ui.dirty |= ImGui::InputDouble("Infinity sends (m)", &p.osc.infinityMetres, 0, 0, "%.1f");
    ui.dirty |= ImGui::Checkbox("Send as one bundle per frame", &p.osc.bundle);
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
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        const std::string pre = ui.oscPrefix;
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
    connectionStatus(st.otioOpen, st.otioError, out.settings().otioDestination(), out.settings().otioPort, st.otioSent, "packets");
    ImGui::TextDisabled("Sends: lens focusDistance, fStop/tStop, pinholeFocalLength; timing incl. timecode; tracker "
                        "recording + slate (clip); static camera make/model/serial, sensor size, frame rate, ISO, shutter.");
    if (!st.otioLastJson.empty() && ImGui::CollapsingHeader("Last sample (JSON)")) {
        ImGui::BeginChild("otiojson", ImVec2(0, 220), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", st.otioLastJson.c_str());
        ImGui::EndChild();
    }
}

// Returns true when Apply was pressed (the caller saves settings).
bool drawOutput(OutputManager& out, OutputsUi& ui, JsonLogger& logger, char* logPath, size_t logPathSize, bool& logRaw) {
    bool applied = false;
    const OutputManager::Status st = out.status();
    auto& p = ui.pending;
    ui.dirty |= ImGui::Checkbox("Only send when lens data is present", &p.onlyWithLens);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Applies to FreeD, OSC and OpenTrackIO.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui.dirty);
    if (ImGui::Button("Apply")) {
        p.freedHost = ui.freedHost;
        p.oscHost = ui.oscHost;
        p.osc.prefix = ui.oscPrefix;
        p.otioHost = ui.otioHost;
        out.apply(p);
        ui.dirty = false;
        applied = true;
    }
    ImGui::EndDisabled();
    if (ui.dirty) { ImGui::SameLine(); ImGui::TextColored(kAmber, "Unapplied changes"); }
    if (st.sendErrors) { ImGui::SameLine(); ImGui::TextDisabled("%llu send errors", (unsigned long long)st.sendErrors); }

    if (ImGui::CollapsingHeader("Sensor size")) {
        const CameraSample& last = st.last;
        ImGui::TextDisabled("Camera / derived: %s",
                            last.sensorSource.empty() ? "none" : (last.hasSensor ? fmt("%.2f", last.sensorWidthMm) + " x " +
                                fmt("%.2f mm  ", last.sensorHeightMm) + "(" + last.sensorSource + ")" : last.sensorSource).c_str());
        int mode = int(p.sensorOverride);
        if (ImGui::Combo("Manual size", &mode, "Off\0Use when the camera has none\0Always use\0")) {
            p.sensorOverride = SensorOverride(mode);
            ui.dirty = true;
        }
        if (ImGui::BeginCombo("Preset", "Choose a recording mode...")) {
            for (const auto& m : sensorModes()) {
                const std::string label = std::string(m.camera) + "  " + m.name + "  " + fmt("%.2f", m.widthMm()) + " x " +
                                          fmt("%.2f mm", m.heightMm());
                if (ImGui::Selectable(label.c_str())) {
                    p.manualSensorWidthMm = m.widthMm();
                    p.manualSensorHeightMm = m.heightMm();
                    ui.dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        ui.dirty |= ImGui::InputDouble("Width (mm)", &p.manualSensorWidthMm, 0, 0, "%.3f");
        ui.dirty |= ImGui::InputDouble("Height (mm)", &p.manualSensorHeightMm, 0, 0, "%.3f");
        ImGui::TextDisabled("ARRI doesn't send sensor size: it is derived from the camera model and the recording area's "
                            "shape where that is unambiguous. Set a manual size for other modes and cameras.");
    }

    if (ImGui::BeginTabBar("outputs")) {
        if (ImGui::BeginTabItem("FreeD")) { drawFreeD(ui, st, out); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("OSC")) { drawOsc(ui, st, out); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("OpenTrackIO")) { drawOpenTrackIO(ui, st, out); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log file")) { drawLogging(logger, logPath, logPathSize, logRaw); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    return applied;
}

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }  // Ctrl-C in the terminal: leave the loop normally so settings are saved

}  // namespace

int main() {
    dlp::initialise();
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "SDI Metadata Viewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    auto devices = std::make_unique<DeckLinkDevices>();
    Shared shared;
    JsonLogger logger;
    OutputManager outputs;
    OutputsUi outputsUi;
    Preview preview;
    preview.conv = dlp::createVideoConversion();

    // Restore the previous session's settings and start any outputs that were enabled.
    const std::string settingsPath = defaultSettingsPath();
    AppSettings app;
    loadSettings(settingsPath, app);
    outputs.apply(app.outputs);
    outputsUi.pending = app.outputs;
    std::snprintf(outputsUi.freedHost, sizeof outputsUi.freedHost, "%s", app.outputs.freedHost.c_str());
    std::snprintf(outputsUi.oscHost, sizeof outputsUi.oscHost, "%s", app.outputs.oscHost.c_str());
    std::snprintf(outputsUi.oscPrefix, sizeof outputsUi.oscPrefix, "%s", app.outputs.osc.prefix.c_str());
    std::snprintf(outputsUi.otioHost, sizeof outputsUi.otioHost, "%s", app.outputs.otioHost.c_str());

    CaptureSession* session = nullptr;
    int deviceIndex = 0;
    auto selectByName = [&](const std::string& name) {
        for (size_t i = 0; i < devices->size(); ++i)
            if (devices->ports()[i].name == name) deviceIndex = int(i);
    };
    selectByName(app.deviceName);
    int64_t inputConnection = app.inputConnection;
    std::string profileMessage;
    double rescanAt = 0;  // after a profile change the card re-enumerates asynchronously
    int selectedPacket = -1;
    std::string lastError;
    char logPath[512];
    std::snprintf(logPath, sizeof logPath, "%s", app.logPath.c_str());
    bool logRaw = app.logRaw;
    bool recFromTc = app.recFromTc;
    float panelW = app.panelWidth;
    preview.enabled = app.showVideo;

    auto save = [&] {
        app.outputs = outputs.settings();
        app.logPath = logPath;
        app.logRaw = logRaw;
        app.recFromTc = recFromTc;
        if (deviceIndex < int(devices->size())) app.deviceName = devices->ports()[size_t(deviceIndex)].name;
        app.inputConnection = inputConnection;
        app.panelWidth = panelW;
        app.showVideo = preview.enabled;
        saveSettings(settingsPath, app);
    };

    auto handler = [&shared, &logger, &outputs](FrameMetadata&& md) {
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
    };

    while (!glfwWindowShouldClose(window) && !g_quit) {
        glfwPollEvents();
        preview.update(session);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Snapshot shared state for this UI frame.
        FrameMetadata md;
        std::map<uint16_t, PacketStats> stats;
        RddHistory rddHist, bmdHist;
        bool hasFrame;
        uint64_t frames;
        double fps;
        {
            std::lock_guard<std::mutex> l(shared.mtx);
            md = shared.latest;
            stats = shared.stats;
            rddHist = shared.rddHistory;
            bmdHist = shared.bmdHistory;
            hasFrame = shared.hasFrame;
            frames = shared.frames;
            fps = shared.fps;
        }
        InputStatus st = session ? session->status() : InputStatus{};
        if (st.rateNum > 0) outputs.setVideoRate(st.rateNum, st.rateDen);

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        panelW = std::clamp(panelW, 320.0f, std::max(320.0f, vp->WorkSize.x - 240.0f));
        // NoBringToFrontOnFocus keeps the splitter (drawn last) on top of both panels.
        const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

        // Preview
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - panelW, vp->WorkSize.y));
        ImGui::Begin("Preview", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Checkbox("Show video", &preview.enabled);
        ImGui::SameLine();
        if (st.running)
            ImGui::Text("%s  %ldx%ld  %s  |  %.2f fps measured", st.modeName.c_str(), st.width, st.height,
                        st.rgb444 ? "RGB 4:4:4" : "YUV 4:2:2", fps);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (st.running && st.noSignal) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "NO SIGNAL");
        } else if (preview.tex && preview.enabled && preview.w > 0) {
            float aspect = float(preview.w) / float(preview.h);
            ImVec2 size(avail.x, avail.x / aspect);
            if (size.y > avail.y) size = ImVec2(avail.y * aspect, avail.y);
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f, ImGui::GetCursorPosY()));
            ImGui::Image((ImTextureID)(intptr_t)preview.tex, size);
        } else {
            ImGui::TextDisabled(preview.conv ? "No video" : "Video converter unavailable (DeckLink driver missing?)");
        }
        ImGui::PopTextWrapPos();
        ImGui::End();

        // Metadata panel
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - panelW, vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(panelW, vp->WorkSize.y));
        ImGui::Begin("Metadata", nullptr, fixed);
        ImGui::PushTextWrapPos(0.0f);  // wrap all text at the window / table-column edge instead of overflowing

        if (rescanAt > 0 && glfwGetTime() >= rescanAt && !session) {
            const std::string keep = deviceIndex < int(devices->size()) ? devices->ports()[size_t(deviceIndex)].name : "";
            devices = std::make_unique<DeckLinkDevices>();
            deviceIndex = 0;
            selectByName(keep);
            rescanAt = 0;
        }
        if (!devices->driverPresent()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Blackmagic Desktop Video driver not found.");
        } else if (devices->size() == 0) {
            ImGui::TextDisabled("No DeckLink inputs found.");
            if (ImGui::Button("Rescan")) devices = std::make_unique<DeckLinkDevices>();
        } else {
            const auto& ports = devices->ports();
            if (deviceIndex >= int(ports.size())) deviceIndex = 0;
            ImGui::BeginDisabled(session != nullptr);

            // Port (sub-device) list; ports inactive in the card's current profile can't be selected
            if (ImGui::BeginCombo("Input port", ports[size_t(deviceIndex)].label().c_str())) {
                for (int i = 0; i < int(ports.size()); ++i) {
                    ImGui::BeginDisabled(!ports[size_t(i)].active());
                    if (ImGui::Selectable((ports[size_t(i)].label() + "##port" + std::to_string(i)).c_str(), i == deviceIndex))
                        deviceIndex = i;
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Rescan")) {
                const std::string keep = ports[size_t(deviceIndex)].name;
                devices = std::make_unique<DeckLinkDevices>();
                deviceIndex = 0;
                selectByName(keep);
            }
            const DeckLinkPort& port = devices->ports()[size_t(deviceIndex)];

            // Connector, from what this port advertises
            std::vector<int64_t> connectors;
            for (int64_t bit = 1; bit <= (int64_t(1) << 8); bit <<= 1)
                if (port.connections & bit) connectors.push_back(bit);
            if (!(port.connections & inputConnection))
                inputConnection = (port.connections & bmdVideoConnectionSDI) ? bmdVideoConnectionSDI : connectors.front();
            if (connectors.size() > 1) {
                if (ImGui::BeginCombo("Connector", videoConnectionName(inputConnection))) {
                    for (int64_t c : connectors)
                        if (ImGui::Selectable(videoConnectionName(c), c == inputConnection)) inputConnection = c;
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("Connector: %s", videoConnectionName(inputConnection));
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s  |  ARRI record flag (HANC): %s", port.model.empty() ? "DeckLink" : port.model.c_str(),
                                port.hancInput ? "can capture" : "not supported by this device");

            // Card profile (multi-port cards): decides how many ports exist and whether they are full or half duplex
            const auto profiles = devices->profiles(size_t(deviceIndex));
            if (profiles.size() > 1) {
                static int64_t pending = 0;
                int64_t activeId = 0;
                for (const auto& pr : profiles) if (pr.active) activeId = pr.id;
                if (!pending) pending = activeId;
                ImGui::BeginDisabled(session != nullptr || rescanAt > 0);
                if (ImGui::BeginCombo("Card profile", profileIdName(pending))) {
                    for (const auto& pr : profiles)
                        if (ImGui::Selectable((pr.name + (pr.active ? "  (active)" : "")).c_str(), pr.id == pending)) pending = pr.id;
                    ImGui::EndCombo();
                }
                if (pending != activeId) {
                    ImGui::SameLine();
                    if (ImGui::Button("Activate")) {
                        std::string err;
                        if (devices->activateProfile(size_t(deviceIndex), pending, err)) {
                            profileMessage = std::string("Switching card to: ") + profileIdName(pending);
                            rescanAt = glfwGetTime() + 1.5;
                        } else {
                            profileMessage = err;
                        }
                        pending = 0;
                    }
                    ImGui::TextColored(kAmber, "Changes the whole card, including any other app using it (e.g. Stage Precision).");
                }
                ImGui::EndDisabled();
                if (session) ImGui::TextDisabled("Stop capture to change the card profile.");
                if (!profileMessage.empty()) ImGui::TextDisabled("%s", profileMessage.c_str());
            }
            if (!port.active())
                ImGui::TextColored(kAmber, "This port is inactive in the card's current profile. Pick an active port, or "
                                           "a profile with more ports.");

            if (!session) {
                ImGui::BeginDisabled(!port.active());
                if (ImGui::Button("Start capture")) {
                    shared.reset();
                    session = CaptureSession::create(devices->device(size_t(deviceIndex)), handler);
                    session->setRecFromTimecode(recFromTc);
                    if (!session->start(lastError, inputConnection)) { session->Release(); session = nullptr; }
                    else lastError.clear();
                    save();
                }
                ImGui::EndDisabled();
            } else if (ImGui::Button("Stop capture")) {
                session->stop();
                session->Release();
                session = nullptr;
            }
            if (!lastError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", lastError.c_str());
            if (session) ImGui::Text("Frames: %llu", (unsigned long long)frames);
            if (ImGui::Checkbox("Record state from running timecode", &recFromTc) && session)
                session->setRecFromTimecode(recFromTc);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Used when the camera sends no record flag (Sony), and only after timecode has been\n"
                                  "seen stopped (proving Rec Run TC). In Free Run it is never used.");
        }

        ImGui::Separator();
        CameraSample cs = sampleFrom(md);
        applySensorOverride(cs, outputs.settings());
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Summary")) { drawSummary(md, hasFrame, cs); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Input")) { drawInput(md, hasFrame, cs, rddHist, bmdHist); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Output")) {
                if (drawOutput(outputs, outputsUi, logger, logPath, sizeof logPath, logRaw)) save();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Raw")) { drawRaw(md, stats, selectedPacket); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::PopTextWrapPos();
        ImGui::End();

        // Drag handle on the panel's left edge
        const float grip = 8.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - panelW - grip * 0.5f, vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(grip, vp->WorkSize.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##splitter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::InvisibleButton("##drag", ImVec2(grip, vp->WorkSize.y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) panelW -= ImGui::GetIO().MouseDelta.x;
        if (ImGui::IsItemDeactivated()) save();
        ImGui::End();
        ImGui::PopStyleVar(2);

        ImGui::Render();
        int fbw, fbh;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.08f, 0.08f, 0.09f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    save();
    if (session) { session->stop(); session->Release(); }
    logger.close();
    preview.release();
    devices.reset();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    dlp::shutdown();
    return 0;
}
