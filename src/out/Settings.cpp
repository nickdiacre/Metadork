#include "out/Settings.h"
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string b(bool v) { return v ? "1" : "0"; }

// Inverted axes as letters: "p" pan, "t" tilt, "r" roll (e.g. "tr")
std::string signs(const OrientationSigns& s) {
    return std::string(s.invertPan ? "p" : "") + (s.invertTilt ? "t" : "") + (s.invertRoll ? "r" : "");
}

std::string d(double v) {
    std::ostringstream o;
    o.precision(10);
    o << v;
    return o.str();
}

}  // namespace

std::string defaultSettingsPath() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    fs::path dir = base ? fs::path(base) / "SdiMetaViewer" : fs::path("SdiMetaViewer");
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    fs::path dir = fs::path(home ? home : ".") / "Library" / "Application Support" / "SdiMetaViewer";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path dir = xdg ? fs::path(xdg) / "sdimeta" : fs::path(home ? home : ".") / ".config" / "sdimeta";
#endif
    return (dir / "settings.ini").string();
}

std::string resolveUserPath(const std::string& path) {
    if (path.empty() || fs::path(path).is_absolute()) return path;
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    const fs::path dir = home ? fs::path(home) / "Documents" : fs::path(".");
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    const fs::path dir = home ? fs::path(home) / "Documents" : fs::path(".");
#else
    const char* home = std::getenv("HOME");
    const fs::path dir = home ? fs::path(home) : fs::path(".");
#endif
    return (dir / path).string();
}

std::string settingsToString(const AppSettings& s) {
    const auto& o = s.outputs;
    std::string disabled;
    for (const auto& id : o.osc.disabled) disabled += (disabled.empty() ? "" : ",") + id;
    std::ostringstream w;
    w << "# SDI Metadata Viewer settings\n"
      << "device=" << s.deviceName << "\n"
      << "input_connection=" << s.inputConnection << "\n"
      << "panel_width=" << d(s.panelWidth) << "\n"
      << "show_video=" << b(s.showVideo) << "\n"
      << "rec_from_timecode=" << b(s.recFromTc) << "\n"
      << "red_position=" << s.redPosition << "\n"
      << "log_path=" << s.logPath << "\n"
      << "log_raw=" << b(s.logRaw) << "\n"
      << "replay_path=" << s.replayPath << "\n"
      << "sensor_override=" << int(o.sensorOverride) << "\n"
      << "manual_sensor_width_mm=" << d(o.manualSensorWidthMm) << "\n"
      << "manual_sensor_height_mm=" << d(o.manualSensorHeightMm) << "\n"
      << "freed_enabled=" << b(o.freedEnabled) << "\n"
      << "freed_host=" << o.freedHost << "\n"
      << "freed_port=" << o.freedPort << "\n"
      << "freed_camera_id=" << int(o.freed.cameraId) << "\n"
      << "freed_focus_counts_per_mm=" << d(o.freed.focusCountsPerMm) << "\n"
      << "freed_zoom_counts_per_mm=" << d(o.freed.zoomCountsPerMm) << "\n"
      << "freed_iris_in_spare=" << b(o.freed.irisInSpare) << "\n"
      << "freed_iris_scale=" << d(o.freed.irisSpareScale) << "\n"
      << "osc_enabled=" << b(o.oscEnabled) << "\n"
      << "osc_host=" << o.oscHost << "\n"
      << "osc_port=" << o.oscPort << "\n"
      << "osc_prefix=" << o.osc.prefix << "\n"
      << "osc_focus_unit=" << int(o.osc.focusUnit) << "\n"
      << "osc_infinity_m=" << d(o.osc.infinityMetres) << "\n"
      << "osc_bundle=" << b(o.osc.bundle) << "\n"
      << "osc_disabled=" << disabled << "\n"
      << "otio_enabled=" << b(o.otioEnabled) << "\n"
      << "otio_multicast=" << b(o.otioMulticast) << "\n"
      << "otio_host=" << o.otioHost << "\n"
      << "otio_port=" << o.otioPort << "\n"
      << "otio_source_number=" << o.otio.sourceNumber << "\n"
      << "otio_focal_as_pinhole=" << b(o.otio.focalAsPinhole) << "\n"
      << "otio_include_transform=" << b(o.otio.includeTransform) << "\n"
      << "otio_infinity_m=" << d(o.otio.infinityMetres) << "\n"
      << "freed_invert=" << signs(o.freed.signs) << "\n"
      << "osc_invert=" << signs(o.osc.signs) << "\n"
      << "otio_invert=" << signs(o.otio.signs) << "\n"
      << "roll_convention=2\n";
    return w.str();
}

void settingsFromString(const std::string& text, AppSettings& s) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto str = [&](const char* k, std::string& v) { if (auto it = kv.find(k); it != kv.end()) v = it->second; };
    auto boolean = [&](const char* k, bool& v) { if (auto it = kv.find(k); it != kv.end()) v = it->second == "1"; };
    auto integer = [&](const char* k, int& v) {
        if (auto it = kv.find(k); it != kv.end()) try { v = std::stoi(it->second); } catch (...) {}
    };
    auto real = [&](const char* k, double& v) {
        if (auto it = kv.find(k); it != kv.end()) try { v = std::stod(it->second); } catch (...) {}
    };
    auto& o = s.outputs;
    str("device", s.deviceName);
    if (auto it = kv.find("input_connection"); it != kv.end()) try { s.inputConnection = std::stoll(it->second); } catch (...) {}
    double pw = s.panelWidth;
    real("panel_width", pw);
    s.panelWidth = float(pw);
    boolean("show_video", s.showVideo);
    boolean("rec_from_timecode", s.recFromTc);
    if (auto it = kv.find("red_position"); it != kv.end() && it->second.size() == 1 && std::isalpha((unsigned char)it->second[0]))
        s.redPosition = char(std::toupper((unsigned char)it->second[0]));
    str("log_path", s.logPath);
    str("replay_path", s.replayPath);
    boolean("log_raw", s.logRaw);
    int so = int(o.sensorOverride);
    integer("sensor_override", so);
    o.sensorOverride = SensorOverride(std::clamp(so, 0, 2));
    real("manual_sensor_width_mm", o.manualSensorWidthMm);
    real("manual_sensor_height_mm", o.manualSensorHeightMm);
    boolean("freed_enabled", o.freedEnabled);
    str("freed_host", o.freedHost);
    integer("freed_port", o.freedPort);
    int camId = o.freed.cameraId;
    integer("freed_camera_id", camId);
    o.freed.cameraId = uint8_t(std::clamp(camId, 0, 255));
    real("freed_focus_counts_per_mm", o.freed.focusCountsPerMm);
    real("freed_zoom_counts_per_mm", o.freed.zoomCountsPerMm);
    boolean("freed_iris_in_spare", o.freed.irisInSpare);
    real("freed_iris_scale", o.freed.irisSpareScale);
    boolean("osc_enabled", o.oscEnabled);
    str("osc_host", o.oscHost);
    integer("osc_port", o.oscPort);
    str("osc_prefix", o.osc.prefix);
    int unit = int(o.osc.focusUnit);
    integer("osc_focus_unit", unit);
    o.osc.focusUnit = FocusUnit(std::clamp(unit, 0, 2));
    real("osc_infinity_m", o.osc.infinityMetres);
    boolean("osc_bundle", o.osc.bundle);
    if (auto it = kv.find("osc_disabled"); it != kv.end()) {
        o.osc.disabled.clear();
        std::istringstream ids(it->second);
        for (std::string id; std::getline(ids, id, ',');)
            if (!id.empty()) o.osc.disabled.insert(id);
    }
    boolean("otio_enabled", o.otioEnabled);
    boolean("otio_multicast", o.otioMulticast);
    str("otio_host", o.otioHost);
    integer("otio_port", o.otioPort);
    integer("otio_source_number", o.otio.sourceNumber);
    o.otio.sourceNumber = std::clamp(o.otio.sourceNumber, 1, 200);
    boolean("otio_focal_as_pinhole", o.otio.focalAsPinhole);
    boolean("otio_include_transform", o.otio.includeTransform);
    real("otio_infinity_m", o.otio.infinityMetres);
    auto readSigns = [&](const char* k, OrientationSigns& sg) {
        if (auto it = kv.find(k); it != kv.end()) {
            sg.invertPan = it->second.find('p') != std::string::npos;
            sg.invertTilt = it->second.find('t') != std::string::npos;
            sg.invertRoll = it->second.find('r') != std::string::npos;
        }
    };
    readSigns("freed_invert", o.freed.signs);
    readSigns("osc_invert", o.osc.signs);
    readSigns("otio_invert", o.otio.signs);
    // Before roll_convention=2 roll went out + clockwise and Stage Precision needed "invert roll"; it is now sent
    // + anticlockwise by default, so an invert-roll flag from an older file would flip it back: drop it.
    if (kv.find("roll_convention") == kv.end() && kv.find("osc_invert") != kv.end())
        o.freed.signs.invertRoll = o.osc.signs.invertRoll = o.otio.signs.invertRoll = false;
}

bool loadSettings(const std::string& path, AppSettings& s) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    settingsFromString(ss.str(), s);
    return true;
}

bool saveSettings(const std::string& path, const AppSettings& s) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << settingsToString(s);
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);  // atomic replace, so a crash mid-write cannot corrupt the settings
    return !ec;
}
