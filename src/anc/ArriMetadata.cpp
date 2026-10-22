#include "anc/ArriMetadata.h"
#include <cstdio>
#include <cmath>
#include <cstring>

// Key prefix: 06 0E 2B 34 02 05 01 0D 0E 17 00 00 00 | K14 K15 | K16(version)
static const uint8_t kArriPrefix[13] = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01,
                                        0x0D, 0x0E, 0x17, 0x00, 0x00, 0x00};

bool isArriMetadataKey(const uint8_t* k) {
    return std::memcmp(k, kArriPrefix, 13) == 0 && k[13] == 0x11 && k[14] == 0x01;
}
bool isArriLutKey(const uint8_t* k) {
    return std::memcmp(k, kArriPrefix, 13) == 0 && k[13] == 0xA1 && k[14] == 0x01;
}

namespace {
struct Reader {
    const uint8_t* p;
    size_t n;
    uint32_t u32(size_t o) const {
        return o + 4 > n ? 0 : uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 3]) << 24;
    }
    int32_t i32(size_t o) const { return int32_t(u32(o)); }
    uint16_t u16(size_t o) const { return o + 2 > n ? 0 : uint16_t(p[o] | p[o + 1] << 8); }
    uint8_t u8(size_t o) const { return o < n ? p[o] : 0; }
    std::string str(size_t o, size_t len) const {
        std::string s;
        for (size_t i = 0; i < len && o + i < n; ++i) {
            uint8_t c = p[o + i];
            if (c == 0 || c == 0xFF) break;
            s.push_back(c >= 32 && c < 127 ? char(c) : '?');
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
};

std::string bcdTimecode(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X", v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return b;
}

const char* cameraTypeName(uint32_t id) {
    switch (id) {
        case 1: return "D-21";
        case 2: return "ALEXA";
        case 3: return "ALEXA 65";
        case 4: return "ALEXA Mini";
        case 5: return "AMIRA";
        case 8: return "ALEXA LF";
        case 9: return "ALEXA Mini LF";
        case 10: return "ALEXA 35";
        default: return "Unknown ARRI";
    }
}
}  // namespace

bool parseArriBlock(const uint8_t* value, size_t size, ArriMetadata& m) {
    if (!value || size < 0x0AA4) return false;
    Reader r{value, size};
    m = ArriMetadata{};

    // Image content info
    m.activeImageW = r.i32(0x002C);
    m.activeImageH = r.i32(0x0030);
    m.whiteBalanceK = r.u32(0x005C);
    {
        // Pixel aspect ratio / anamorphic desqueeze: IEEE float (Mini LF sends 0x3F800000 = 1.0)
        const uint32_t bits = r.u32(0x00C4);
        float f;
        std::memcpy(&f, &bits, 4);
        m.pixelAspect = (std::isfinite(f) && f >= 0.5f && f <= 4.0f) ? double(f) : 0.0;
    }
    {
        // Tint is an IEEE float, not an integer (verified on a Mini LF: -3.0, -2.4, -2.0 while adjusting)
        const uint32_t bits = r.u32(0x0060);
        float f;
        std::memcpy(&f, &bits, 4);
        m.tint = std::isfinite(f) ? double(f) : 0.0;
    }
    m.exposureIndex = r.u32(0x0074);
    m.lookFile = r.str(0x00CC, 32);

    // Camera device info
    m.cameraTypeId = r.u32(0x0164);
    m.cameraTypeName = cameraTypeName(m.cameraTypeId);
    m.cameraSerial = r.u32(0x0170);
    uint32_t idx = r.u32(0x0178);
    m.cameraIndex = (idx >= 'A' && idx <= 'Z') ? char(idx) : 0;
    m.exposureTimeRaw = r.u32(0x018C);
    // White paper: 1/10 deg (50..3580). ALEXA Mini LF SUP 7 sends 1/1000 deg (180000 = 180 deg); accept both.
    const uint32_t shutter = r.u32(0x0190);
    m.shutterAngle = shutter > 3600 ? shutter / 1000.0 : shutter / 10.0;
    m.sensorFps = r.u32(0x01A0) / 1000.0;
    m.projectFpsCode = r.u32(0x01A4);
    m.masterTc = bcdTimecode(r.u32(0x01A8));
    uint32_t flags = r.u32(0x02C4);
    m.mirrorShutterRunning = flags & 0x1;
    m.variFrameDuplicate = flags & 0x2;
    m.supName = r.str(0x02E0, 24);
    m.cameraModel = r.str(0x02F8, 20);

    // Lens data
    m.lensDistanceUnit = r.u32(0x0374);
    m.focusRaw = r.i32(0x0378);
    if (m.focusRaw == -1) {
        m.focusInfinite = true;
        m.focusValid = true;
        m.focusMm = INFINITY;
    } else if (m.focusRaw > 0) {
        m.focusValid = true;
        // Unit 0 = 1/1000 inch, otherwise mm.
        m.focusMm = (m.lensDistanceUnit == 0) ? m.focusRaw * 0.0254 : double(m.focusRaw);
    }
    uint32_t focal = r.u32(0x037C);
    m.focalLengthMm = focal ? focal / 1000.0 : NAN;
    m.lensSerial = r.u32(0x0380);
    m.irisRaw = r.i32(0x0384);
    switch (m.irisRaw) {
        case -1: m.irisState = "invalid"; break;
        case -2: m.irisState = "closed"; break;
        case -3: m.irisState = "near closed"; break;
        default:
            if (m.irisRaw > 0) {
                m.tStop = std::pow(2.0, (m.irisRaw / 1000.0 - 1.0) / 2.0);
                m.irisState = "ok";
            } else {
                m.irisState = "n/a";
            }
    }
    m.ndType = r.u16(0x0388);
    m.ndDensity = r.u16(0x038A) / 1000.0;
    m.lensModel = r.str(0x0398, 32);
    m.rawFocusLds = r.u16(0x03B8);
    m.rawFocusMotor = r.u16(0x03BA);
    m.rawFocalLds = r.u16(0x03BC);
    m.rawFocalMotor = r.u16(0x03BE);
    m.rawIrisLds = r.u16(0x03C0);
    m.rawIrisMotor = r.u16(0x03C2);
    m.ldsLagType = r.u8(0x03DC);
    m.ldsLagFrames = r.u8(0x03DD);
    m.rawEncoderLagType = r.u8(0x03DE);
    m.rawEncoderLagFrames = r.u8(0x03DF);
    m.effectiveFocalLengthUm = r.i32(0x03E4);
    m.entrancePupilUm = r.i32(0x03E8);

    // Clip info
    auto orientation = [&](size_t o, bool& has, double& deg) {
        const uint32_t v = r.u32(o);
        has = v != 0xFFFFFFFFu;
        deg = has ? int16_t(v & 0xFFFF) / 10.0 : 0;
    };
    orientation(0x045C, m.hasTilt, m.tiltDeg);
    orientation(0x0460, m.hasRoll, m.rollDeg);

    m.circleTake = r.u32(0x04F4) == 1;
    m.reelNameOld = r.str(0x04F8, 8);
    m.scene = r.str(0x0500, 16);
    m.take = r.str(0x0510, 8);
    m.clipNameOld = r.str(0x0698, 24);
    m.reelName = r.str(0x06B0, 11);
    m.clipName = r.str(0x06BB, 34);

    // Lens converter
    m.lensConverterModel = r.str(0x0A0C, 64);
    m.focalLengthMultiplier = r.u32(0x0A94) / 1000.0;
    return true;
}
