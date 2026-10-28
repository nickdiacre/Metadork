#pragma once
#include "out/CameraSample.h"
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Values go out as tilt + up and roll + anticlockwise seen from behind (the cameras send roll + clockwise; Stage
// Precision expects anticlockwise, checked on VENICE 2 and Mini LF). Receivers that differ get per-output invert flags.
struct OrientationSigns {
    bool invertPan = false, invertTilt = false, invertRoll = false;
    double pan(double v) const { return invertPan ? -v : v; }
    double tilt(double v) const { return invertTilt ? -v : v; }
    double roll(double v) const { return invertRoll ? -v : v; }
};

// ---------------------------------------------------------------- FreeD (D1 message, 29 bytes)
// Layout: D1, camera ID, pan, tilt, roll (signed 24-bit, deg * 32768), X, Y, Z (signed 24-bit, mm * 64),
// zoom, focus (unsigned 24-bit encoder counts), spare (16-bit), checksum = 0x40 - sum(bytes 0..27).
// FreeD has no units for zoom / focus, so they are scaled here; the receiver must use the same scale.
struct FreeDConfig {
    uint8_t cameraId = 1;
    double focusCountsPerMm = 1.0;     // focus distance mm -> counts (infinity = max count)
    double zoomCountsPerMm = 1000.0;   // focal length mm -> counts (1000 = micrometres)
    bool irisInSpare = true;           // put the iris number in the spare bytes
    double irisSpareScale = 100.0;     // e.g. T2.8 -> 280
    OrientationSigns signs;
};

std::array<uint8_t, 29> buildFreeD(const CameraSample& s, const FreeDConfig& c);
uint8_t freedChecksum(const uint8_t* p, size_t n);

// ---------------------------------------------------------------- OSC 1.0 over UDP
// One message per catalogue variable: <prefix>/<id>, float32 / int32 / string. Variables the camera does not
// supply are never sent; `disabled` lists ids the user switched off.
struct OscConfig {
    std::string prefix = "/camera";
    FocusUnit focusUnit = FocusUnit::Metres;
    double infinityMetres = 1000.0;  // focus value sent when the lens reports infinity
    bool bundle = false;             // one bundle per frame instead of separate messages
    OrientationSigns signs;
    std::set<std::string> disabled;
    bool enabled(const std::string& id) const { return !disabled.count(id); }
};

std::vector<uint8_t> oscMessage(const std::string& address, float value);
std::vector<uint8_t> oscMessage(const std::string& address, const std::string& value);
std::vector<uint8_t> oscMessage(const std::string& address, int32_t value);
std::vector<uint8_t> oscBundle(const std::vector<std::vector<uint8_t>>& messages);
std::vector<std::vector<uint8_t>> buildOsc(const CameraSample& s, const OscConfig& c);  // UDP datagrams

// ---------------------------------------------------------------- OpenTrackIO 1.0.1 (SMPTE RIS OSVP)
// https://ris-pub.smpte.org/ris-osvp-metadata-camdkit/
// JSON sample in a 16-byte header: "OTrk", reserved, encoding (0x01 JSON), sequence (16), segment offset (32),
// last-segment flag (1) + payload length (15), Fletcher-16 (mod 256) over header bytes 0-13 and the payload.
// Default destination: multicast 239.135.1.<source number>, UDP 55555.
struct OpenTrackIOConfig {
    int sourceNumber = 1;          // 1-200, also the last octet of the multicast address
    bool focalAsPinhole = true;    // send focal length as lens.pinholeFocalLength (nominal, not calibrated)
    bool includeTransform = false; // transforms[0] with tilt + roll (pan, position = 0): off unless the
                                   // receiver should take orientation from here
    double infinityMetres = 1000.0;
    double staticIntervalS = 2.0;  // the spec asks for static data about every 2 s
    OrientationSigns signs;
};

struct OpenTrackIOFrame {
    std::string sampleId, sourceId;  // urn:uuid:...
    uint32_t sequence = 0;
    int rateNum = 25, rateDen = 1;   // video sample rate
    bool includeStatic = false;
};

std::string buildOpenTrackIOJson(const CameraSample& s, const OpenTrackIOConfig& c, const OpenTrackIOFrame& f);
// Wraps a payload in OTrk headers, segmenting if needed (15-bit payload length per packet).
std::vector<std::vector<uint8_t>> openTrackIOPackets(const std::string& payload, uint16_t sequence);
uint16_t fletcher16(const uint8_t* data, size_t len, uint8_t sum1 = 0, uint8_t sum2 = 0);
std::string randomUuidUrn();
std::string openTrackIOMulticast(int sourceNumber);
