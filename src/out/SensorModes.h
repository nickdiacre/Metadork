#pragma once
#include <string>
#include <vector>

// ARRI sensor modes with their active sensor area. ARRI sends no sensor size or mode over SDI, so this table is used
// for the manual sensor-size presets and to recognise the mode from the Active Image shape (IDI06).
// Area = photosites x photosite pitch. Sources (ARRI's own format data; add bodies / modes only from those):
//   ALEXA 35 (ALEV 4, 6.075 um): "ALEXA 35 Recording Formats" poster, SUP 1.1 (later SUPs add modes)
//   ALEXA Mini LF (ALEV III LF, 8.25 um): "ALEXA Mini LF Recording Formats" sheet, SUP 7.1
//   ALEXA LF: ALEXA LF FAQ (LF Open Gate / LF 16:9 / LF 2.39:1)
//   ALEXA 265 (A3X Rev.B, 8.25 um): ARRI Rental technical data
//   ALEXA 65 (A3X, 8.25 um): ARRI Rental technical data (Open Gate, crop modes)
//   ALEXA Mini (ALEV III, 8.25 um): "ALEXA Mini Recording Areas, Surround Views and Framelines" SUP 4.0
//   ALEXA SXT: ALEXA SXT technical data;  AMIRA: arri.com/amira
// Not yet tabulated: ALEXA XT / classic, ALEXA 35 SUP 2+ modes, AMIRA S16.
struct SensorMode {
    const char* camera;  // body, e.g. "ALEXA Mini LF"; matched against the camera model (longest match wins)
    const char* name;    // mode as ARRI names it, e.g. "4.5K LF 3:2 Open Gate"
    int photositesW, photositesH;
    double pitchUm;
    double widthMm() const { return photositesW * pitchUm / 1000.0; }
    double heightMm() const { return photositesH * pitchUm / 1000.0; }
    double aspect() const { return double(photositesW) / photositesH; }
};

const std::vector<SensorMode>& sensorModes();

// The table's body name for a camera model string ("ARRI ALEXA 265" -> "ALEXA 265"), or "" if none.
std::string sensorBody(const std::string& cameraModel);

struct SensorGuess {
    const SensorMode* mode = nullptr;           // set when exactly one mode matches
    std::vector<const SensorMode*> candidates;  // all modes of this body with a matching aspect
};

// Modes of the camera's body whose aspect is within 0.5% of `aspect` (the recording area's aspect).
SensorGuess guessSensorMode(const std::string& cameraModel, double aspect);
