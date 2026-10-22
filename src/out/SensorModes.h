#pragma once
#include <string>
#include <vector>

// Recording modes with their active sensor area: presets for the manual sensor size, for cameras that don't send it
// over SDI (ARRI). Not used to auto-detect: ARRI's Active Image fields don't identify the mode (a Mini LF in LF 2.39:1
// reported 3840x2160).
// Area = photosites x photosite pitch, from ARRI's published format data:
//   ALEXA LF / Mini LF (ALEV III, 8.25 um): LF Open Gate 4448x3096, LF 16:9 UHD 3840x2160, LF 2.39:1 4448x1856
//   ALEXA 35 (ALEV 4, 6.075 um): 4.6K 3:2 Open Gate 4608x3164, 4.6K 16:9 4608x2592, 4K 16:9 4096x2304,
//                                 4K 2:1 4096x2048, 2K 16:9 S16 2048x1152
// Add bodies / modes only from ARRI's format documentation.
struct SensorMode {
    const char* camera;  // matched case-insensitively as a substring of the camera model / type name
    const char* name;    // e.g. "LF Open Gate"
    int photositesW, photositesH;
    double pitchUm;
    double widthMm() const { return photositesW * pitchUm / 1000.0; }
    double heightMm() const { return photositesH * pitchUm / 1000.0; }
    double aspect() const { return double(photositesW) / photositesH; }
};

const std::vector<SensorMode>& sensorModes();

struct SensorGuess {
    const SensorMode* mode = nullptr;      // set when exactly one mode matches
    std::vector<const SensorMode*> candidates;  // all modes for this camera with a matching aspect
};

// Modes of `camera` whose aspect is within 0.5% of `aspect` (the recording area's aspect).
SensorGuess guessSensorMode(const std::string& camera, double aspect);
