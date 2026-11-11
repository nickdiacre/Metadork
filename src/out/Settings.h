#pragma once
#include "out/OutputManager.h"
#include <string>
#include <vector>

// One input (camera feed). Its name is the OSC address prefix without the leading '/' ("camera" -> /camera/...).
struct FeedSettings {
    OutputSettings outputs;
    std::string logPath = "sdi_metadata_log.jsonl";
    bool logRaw = true;
    std::string replayPath;  // last capture file replayed
    bool recFromTc = true;
    char redPosition = 'A';  // RED Camera Position letter used in clip names (not sent over SDI)
    std::string deviceName;   // last used input; matched by name on the next launch
    long long inputConnection = 1;  // BMDVideoConnection (1 = SDI)

    std::string name() const;              // from outputs.osc.prefix
    void setName(const std::string& name); // sets outputs.osc.prefix = "/" + name
};

// Everything the user sets up in the UI, persisted between sessions as key=value lines. Feed 1 uses the original
// (unprefixed) keys, so files stay readable by single-feed versions; feed N >= 2 uses "feedN." keys.
struct AppSettings {
    std::vector<FeedSettings> feeds{1};  // always at least one
    float panelWidth = 560.0f;
    bool showVideo = true;
    bool gridView = false;  // feeds two across instead of one column
};

// A new feed's settings: copies `from` (hosts, ports, options) with a unique name, FreeD camera ID, OpenTrackIO source
// number and log file, so a second camera never collides with the first in a receiver.
FeedSettings newFeedSettings(const std::vector<FeedSettings>& existing, const FeedSettings& from);

// ~/Library/Application Support/SdiMetaViewer/settings.ini (macOS), %APPDATA%\SdiMetaViewer\settings.ini (Windows),
// $XDG_CONFIG_HOME or ~/.config/sdimeta/settings.ini (Linux). Creates the directory when saving.
std::string defaultSettingsPath();

// A relative path (e.g. the default log file name) resolved against the user's Documents folder (home on Linux), so it
// lands somewhere sensible however the app was launched (a macOS .app starts with the working directory at /).
std::string resolveUserPath(const std::string& path);

bool loadSettings(const std::string& path, AppSettings& s);  // false if the file is missing; unknown keys ignored
bool saveSettings(const std::string& path, const AppSettings& s);

std::string settingsToString(const AppSettings& s);
void settingsFromString(const std::string& text, AppSettings& s);
