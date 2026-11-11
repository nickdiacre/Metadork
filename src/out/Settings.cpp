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

std::string FeedSettings::name() const {
    const std::string& p = outputs.osc.prefix;
    return !p.empty() && p[0] == '/' ? p.substr(1) : p;
}

void FeedSettings::setName(const std::string& n) { outputs.osc.prefix = "/" + n; }

FeedSettings newFeedSettings(const std::vector<FeedSettings>& existing, const FeedSettings& from) {
    FeedSettings f = from;
    f.deviceName.clear();
    f.replayPath.clear();
    auto taken = [&](const std::string& n) {
        for (const auto& e : existing) if (e.name() == n) return true;
        return false;
    };
    int n = int(existing.size()) + 1;
    while (taken("camera" + std::to_string(n))) ++n;
    f.setName("camera" + std::to_string(n));
    int camId = 0, source = 0;
    for (const auto& e : existing) {
        camId = std::max(camId, int(e.outputs.freed.cameraId));
        source = std::max(source, e.outputs.otio.sourceNumber);
    }
    f.outputs.freed.cameraId = uint8_t(std::min(camId + 1, 255));
    f.outputs.otio.sourceNumber = std::min(source + 1, 200);
    f.logPath = "sdi_metadata_log_" + f.name() + ".jsonl";
    return f;
}

namespace {

void writeFeed(std::ostringstream& w, const std::string& k, const FeedSettings& f) {
    const auto& o = f.outputs;
    std::string disabled;
    for (const auto& id : o.osc.disabled) disabled += (disabled.empty() ? "" : ",") + id;
    w << k << "device=" << f.deviceName << "\n"
      << k << "input_connection=" << f.inputConnection << "\n"
      << k << "rec_from_timecode=" << b(f.recFromTc) << "\n"
      << k << "red_position=" << f.redPosition << "\n"
      << k << "log_path=" << f.logPath << "\n"
      << k << "log_raw=" << b(f.logRaw) << "\n"
      << k << "replay_path=" << f.replayPath << "\n"
      << k << "sensor_override=" << int(o.sensorOverride) << "\n"
      << k << "manual_sensor_width_mm=" << d(o.manualSensorWidthMm) << "\n"
      << k << "manual_sensor_height_mm=" << d(o.manualSensorHeightMm) << "\n"
      << k << "freed_enabled=" << b(o.freedEnabled) << "\n"
      << k << "freed_host=" << o.freedHost << "\n"
      << k << "freed_port=" << o.freedPort << "\n"
      << k << "freed_camera_id=" << int(o.freed.cameraId) << "\n"
      << k << "freed_focus_counts_per_mm=" << d(o.freed.focusCountsPerMm) << "\n"
      << k << "freed_zoom_counts_per_mm=" << d(o.freed.zoomCountsPerMm) << "\n"
      << k << "freed_iris_in_spare=" << b(o.freed.irisInSpare) << "\n"
      << k << "freed_iris_scale=" << d(o.freed.irisSpareScale) << "\n"
      << k << "osc_enabled=" << b(o.oscEnabled) << "\n"
      << k << "osc_host=" << o.oscHost << "\n"
      << k << "osc_port=" << o.oscPort << "\n"
      << k << "osc_prefix=" << o.osc.prefix << "\n"
      << k << "osc_focus_unit=" << int(o.osc.focusUnit) << "\n"
      << k << "osc_infinity_m=" << d(o.osc.infinityMetres) << "\n"
      << k << "osc_bundle=" << b(o.osc.bundle) << "\n"
      << k << "osc_disabled=" << disabled << "\n"
      << k << "otio_enabled=" << b(o.otioEnabled) << "\n"
      << k << "otio_multicast=" << b(o.otioMulticast) << "\n"
      << k << "otio_host=" << o.otioHost << "\n"
      << k << "otio_port=" << o.otioPort << "\n"
      << k << "otio_source_number=" << o.otio.sourceNumber << "\n"
      << k << "otio_focal_as_pinhole=" << b(o.otio.focalAsPinhole) << "\n"
      << k << "otio_include_transform=" << b(o.otio.includeTransform) << "\n"
      << k << "otio_infinity_m=" << d(o.otio.infinityMetres) << "\n"
      << k << "freed_invert=" << signs(o.freed.signs) << "\n"
      << k << "osc_invert=" << signs(o.osc.signs) << "\n"
      << k << "otio_invert=" << signs(o.otio.signs) << "\n";
}

void readFeed(const std::map<std::string, std::string>& kv, const std::string& k, FeedSettings& f) {
    auto find = [&](const char* key) { return kv.find(k + key); };
    auto str = [&](const char* key, std::string& v) { if (auto it = find(key); it != kv.end()) v = it->second; };
    auto boolean = [&](const char* key, bool& v) { if (auto it = find(key); it != kv.end()) v = it->second == "1"; };
    auto integer = [&](const char* key, int& v) {
        if (auto it = find(key); it != kv.end()) try { v = std::stoi(it->second); } catch (...) {}
    };
    auto real = [&](const char* key, double& v) {
        if (auto it = find(key); it != kv.end()) try { v = std::stod(it->second); } catch (...) {}
    };
    auto& o = f.outputs;
    str("device", f.deviceName);
    if (auto it = find("input_connection"); it != kv.end()) try { f.inputConnection = std::stoll(it->second); } catch (...) {}
    boolean("rec_from_timecode", f.recFromTc);
    if (auto it = find("red_position"); it != kv.end() && it->second.size() == 1 && std::isalpha((unsigned char)it->second[0]))
        f.redPosition = char(std::toupper((unsigned char)it->second[0]));
    str("log_path", f.logPath);
    str("replay_path", f.replayPath);
    boolean("log_raw", f.logRaw);
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
    if (auto it = find("osc_disabled"); it != kv.end()) {
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
    auto readSigns = [&](const char* key, OrientationSigns& sg) {
        if (auto it = find(key); it != kv.end()) {
            sg.invertPan = it->second.find('p') != std::string::npos;
            sg.invertTilt = it->second.find('t') != std::string::npos;
            sg.invertRoll = it->second.find('r') != std::string::npos;
        }
    };
    readSigns("freed_invert", o.freed.signs);
    readSigns("osc_invert", o.osc.signs);
    readSigns("otio_invert", o.otio.signs);
}

}  // namespace

std::string settingsToString(const AppSettings& s) {
    std::ostringstream w;
    w << "# SDI Metadata Viewer settings\n"
      << "panel_width=" << d(s.panelWidth) << "\n"
      << "show_video=" << b(s.showVideo) << "\n"
      << "roll_convention=2\n"
      << "feed_count=" << s.feeds.size() << "\n";
    for (size_t i = 0; i < s.feeds.size(); ++i) {
        if (i) w << "# Feed " << i + 1 << "\n";
        writeFeed(w, i ? "feed" + std::to_string(i + 1) + "." : "", s.feeds[i]);
    }
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
    if (auto it = kv.find("panel_width"); it != kv.end()) try { s.panelWidth = std::stof(it->second); } catch (...) {}
    if (auto it = kv.find("show_video"); it != kv.end()) s.showVideo = it->second == "1";
    int count = 1;
    if (auto it = kv.find("feed_count"); it != kv.end()) try { count = std::clamp(std::stoi(it->second), 1, 16); } catch (...) {}
    if (s.feeds.empty()) s.feeds.resize(1);
    s.feeds.resize(size_t(count));
    for (int i = 0; i < count; ++i) readFeed(kv, i ? "feed" + std::to_string(i + 1) + "." : "", s.feeds[size_t(i)]);
    // Before roll_convention=2 roll went out + clockwise and Stage Precision needed "invert roll"; it is now sent
    // + anticlockwise by default, so an invert-roll flag from an older file would flip it back: drop it.
    if (kv.find("roll_convention") == kv.end() && kv.find("osc_invert") != kv.end()) {
        auto& o = s.feeds[0].outputs;
        o.freed.signs.invertRoll = o.osc.signs.invertRoll = o.otio.signs.invertRoll = false;
    }
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
