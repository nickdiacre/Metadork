#pragma once
#include "out/OutputManager.h"
#include <string>

// Everything the user sets up in the UI, persisted between sessions as key=value lines.
struct AppSettings {
    OutputSettings outputs;
    std::string logPath = "sdi_metadata_log.jsonl";
    bool logRaw = true;
    bool recFromTc = true;
    std::string deviceName;   // last used input; matched by name on the next launch
    long long inputConnection = 1;  // BMDVideoConnection (1 = SDI)
    float panelWidth = 560.0f;
    bool showVideo = true;
};

// ~/Library/Application Support/SdiMetaViewer/settings.ini (macOS), %APPDATA%\SdiMetaViewer\settings.ini (Windows),
// $XDG_CONFIG_HOME or ~/.config/sdimeta/settings.ini (Linux). Creates the directory when saving.
std::string defaultSettingsPath();

bool loadSettings(const std::string& path, AppSettings& s);  // false if the file is missing; unknown keys ignored
bool saveSettings(const std::string& path, const AppSettings& s);

std::string settingsToString(const AppSettings& s);
void settingsFromString(const std::string& text, AppSettings& s);
