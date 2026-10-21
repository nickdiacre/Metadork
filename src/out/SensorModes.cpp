#include "out/SensorModes.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
}  // namespace

const std::vector<SensorMode>& sensorModes() {
    // Order matters only for display. "LF" also matches "Mini LF".
    static const std::vector<SensorMode> modes = {
        {"LF", "LF Open Gate", 4448, 3096, 8.25},
        {"LF", "LF 16:9 UHD", 3840, 2160, 8.25},
        {"LF", "LF 2.39:1", 4448, 1856, 8.25},
        {"ALEXA 35", "4.6K 3:2 Open Gate", 4608, 3164, 6.075},
        {"ALEXA 35", "4.6K 16:9", 4608, 2592, 6.075},
        {"ALEXA 35", "4K 16:9", 4096, 2304, 6.075},
        {"ALEXA 35", "4K 2:1", 4096, 2048, 6.075},
        {"ALEXA 35", "2K 16:9 S16", 2048, 1152, 6.075},
    };
    return modes;
}

SensorGuess guessSensorMode(const std::string& camera, double aspect) {
    SensorGuess g;
    if (aspect <= 0) return g;
    const std::string cam = lower(camera);
    for (const auto& m : sensorModes()) {
        if (cam.find(lower(m.camera)) == std::string::npos) continue;
        if (std::fabs(m.aspect() / aspect - 1.0) <= 0.005) g.candidates.push_back(&m);
    }
    if (g.candidates.size() == 1) g.mode = g.candidates.front();
    return g;
}
