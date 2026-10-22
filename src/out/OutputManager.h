#pragma once
#include "out/Protocols.h"
#include "out/UdpSender.h"
#include <mutex>

enum class SensorOverride { Off, WhenMissing, Always };

struct OutputSettings {
    // Manual sensor area, for cameras / modes that don't report or can't be derived (or to force a value).
    SensorOverride sensorOverride = SensorOverride::WhenMissing;
    double manualSensorWidthMm = 0, manualSensorHeightMm = 0;  // 0 = not set

    bool freedEnabled = false;
    std::string freedHost = "127.0.0.1";
    int freedPort = 40000;
    FreeDConfig freed;

    bool oscEnabled = false;
    std::string oscHost = "127.0.0.1";
    int oscPort = 9000;
    OscConfig osc;

    bool otioEnabled = false;
    bool otioMulticast = true;          // 239.135.1.<source number>; otherwise otioHost
    std::string otioHost = "127.0.0.1";
    int otioPort = 55555;
    OpenTrackIOConfig otio;

    std::string otioDestination() const { return otioMulticast ? openTrackIOMulticast(otio.sourceNumber) : otioHost; }
};

// Applies the manual sensor area to a sample according to the settings.
void applySensorOverride(CameraSample& s, const OutputSettings& o);

// Sends FreeD, OSC and / or OpenTrackIO for every captured frame. onFrame runs on the capture thread;
// apply / setVideoRate / status are called from the UI thread.
class OutputManager {
public:
    struct Status {
        CameraSample last;
        bool hasLast = false;
        uint64_t freedSent = 0, oscSent = 0, otioSent = 0, sendErrors = 0;
        std::string lastSendError;     // e.g. "OSC: sendto failed: No route to host"
        double lastSendErrorTime = 0;  // capture clock (s) of that error
        std::string freedError, oscError, otioError;
        bool freedOpen = false, oscOpen = false, otioOpen = false;
        std::string otioLastJson;  // most recent OpenTrackIO payload, for the UI
    };

    OutputManager();
    void apply(const OutputSettings& s);  // (re)opens sockets as needed
    const OutputSettings& settings() const { return m_settings; }  // UI thread only
    void setVideoRate(int num, int den);
    void onFrame(const FrameMetadata& md);
    Status status() const;

private:
    mutable std::mutex m_mtx;
    OutputSettings m_settings;
    UdpSender m_freed, m_osc, m_otio;
    Status m_status;
    std::string m_otioSourceId;
    uint32_t m_otioSequence = 0;
    double m_otioLastStatic = -1e9;
    int m_rateNum = 25, m_rateDen = 1;
    void noteError(const char* what, const UdpSender& sock, double t);
};
