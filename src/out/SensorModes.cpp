#include "out/SensorModes.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
}  // namespace

const std::vector<SensorMode>& sensorModes() {
    // Grouped by body; order is the display order.
    static const std::vector<SensorMode> modes = {
        {"ALEXA 265", "6.5K 2.12:1 Open Gate", 6560, 3100, 8.25},
        {"ALEXA 265", "5.1K 1.65:1", 5120, 3100, 8.25},
        {"ALEXA 265", "4.5K LF 3:2", 4448, 3096, 8.25},

        {"ALEXA 65", "Open Gate", 6560, 3100, 8.25},
        {"ALEXA 65", "1.78 Crop Mode", 5120, 2880, 8.25},
        {"ALEXA 65", "1.50 Crop Mode", 4320, 2880, 8.25},
        {"ALEXA 65", "LF Open Gate", 4448, 3096, 8.25},
        {"ALEXA 65", "4K UHD", 3840, 2160, 8.25},

        {"ALEXA 35", "4.6K 3:2 Open Gate", 4608, 3164, 6.075},
        {"ALEXA 35", "4.6K 16:9", 4608, 2592, 6.075},
        {"ALEXA 35", "4K 16:9", 4096, 2304, 6.075},
        {"ALEXA 35", "4K 2:1", 4096, 2048, 6.075},
        {"ALEXA 35", "3.8K 16:9", 3840, 2160, 6.075},
        {"ALEXA 35", "3.3K 6:5", 3328, 2790, 6.075},
        {"ALEXA 35", "3K 1:1", 3072, 3072, 6.075},
        {"ALEXA 35", "2.7K 8:9", 2743, 3086, 6.075},
        {"ALEXA 35", "2K 16:9 S16", 2048, 1152, 6.075},

        {"ALEXA Mini LF", "4.5K LF 3:2 Open Gate", 4448, 3096, 8.25},
        {"ALEXA Mini LF", "4.5K LF 2.39:1", 4448, 1856, 8.25},
        {"ALEXA Mini LF", "4.3K LF 16:9", 4320, 2430, 8.25},
        {"ALEXA Mini LF", "3.8K LF 16:9", 3840, 2160, 8.25},
        {"ALEXA Mini LF", "2.8K LF 1:1", 2880, 2880, 8.25},
        {"ALEXA Mini LF", "3.4K S35 3:2", 3424, 2202, 8.25},
        {"ALEXA Mini LF", "3.2K S35 16:9", 3200, 1800, 8.25},
        {"ALEXA Mini LF", "2.8K S35 4:3", 2880, 2160, 8.25},
        {"ALEXA Mini LF", "2.8K S35 16:9", 2880, 1620, 8.25},

        {"ALEXA LF", "LF Open Gate", 4448, 3096, 8.25},
        {"ALEXA LF", "LF 16:9", 3840, 2160, 8.25},
        {"ALEXA LF", "LF 2.39:1", 4448, 1856, 8.25},

        {"ALEXA Mini", "Open Gate 3.4K", 3424, 2202, 8.25},
        {"ALEXA Mini", "16:9 3.2K / 4K UHD", 3200, 1800, 8.25},
        {"ALEXA Mini", "16:9 2.8K / HD", 2880, 1620, 8.25},
        {"ALEXA Mini", "16:9 2K", 2868, 1612, 8.25},
        {"ALEXA Mini", "4:3 2.8K", 2880, 2160, 8.25},
        {"ALEXA Mini", "2.39:1 2K Ana.", 2560, 2145, 8.25},
        {"ALEXA Mini", "16:9 HD Ana.", 1920, 2160, 8.25},
        {"ALEXA Mini", "S16 HD", 1600, 900, 8.25},

        {"ALEXA SXT", "Open Gate", 3424, 2202, 8.25},
        {"ALEXA SXT", "16:9 3.2K / 4K UHD", 3200, 1800, 8.25},
        {"ALEXA SXT", "16:9 2.8K / HD", 2880, 1620, 8.25},
        {"ALEXA SXT", "4:3 2.8K", 2880, 2160, 8.25},

        {"AMIRA", "16:9 3.2K / 4K UHD", 3200, 1800, 8.25},
        {"AMIRA", "16:9 2.8K / HD", 2880, 1620, 8.25},
    };
    return modes;
}

std::string sensorBody(const std::string& cameraModel) {
    // Longest body name contained in the model wins ("ALEXA Mini LF" over "ALEXA Mini", "ALEXA 265" over nothing).
    const std::string cam = lower(cameraModel);
    std::string best;
    for (const auto& m : sensorModes())
        if (std::strlen(m.camera) > best.size() && cam.find(lower(m.camera)) != std::string::npos) best = m.camera;
    return best;
}

SensorGuess guessSensorMode(const std::string& cameraModel, double aspect) {
    SensorGuess g;
    const std::string body = sensorBody(cameraModel);
    if (aspect <= 0 || body.empty()) return g;
    for (const auto& m : sensorModes()) {
        if (body != m.camera) continue;
        if (std::fabs(m.aspect() / aspect - 1.0) <= 0.005) g.candidates.push_back(&m);
    }
    if (g.candidates.size() == 1) g.mode = g.candidates.front();
    return g;
}
