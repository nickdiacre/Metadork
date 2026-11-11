#include "out/Protocols.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>

namespace {

void put24(uint8_t* p, int32_t v) {
    p[0] = uint8_t(v >> 16);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v);
}

int32_t clampSigned24(double v) { return int32_t(std::clamp(std::lround(v), -0x800000L, 0x7FFFFFL)); }
int32_t clampUnsigned24(double v) { return int32_t(std::clamp(std::lround(v), 0L, 0xFFFFFFL)); }

void padTo4(std::vector<uint8_t>& b) {
    do b.push_back(0); while (b.size() % 4);
}

void putOscString(std::vector<uint8_t>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    padTo4(b);  // always at least one terminating NUL
}

void putBe32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}

// ---- minimal JSON writer
std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

std::string jnum(double v) {
    if (!std::isfinite(v)) return "0";
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

struct JObj {
    std::string s = "{";
    bool first = true;
    JObj& key(const char* k) { s += first ? "\"" : ",\""; s += k; s += "\":"; first = false; return *this; }
    JObj& num(const char* k, double v) { key(k); s += jnum(v); return *this; }
    JObj& integer(const char* k, long long v) { key(k); s += std::to_string(v); return *this; }
    JObj& boolean(const char* k, bool v) { key(k); s += v ? "true" : "false"; return *this; }
    JObj& str(const char* k, const std::string& v) { key(k); s += "\"" + jsonEsc(v) + "\""; return *this; }
    JObj& strIf(const char* k, const std::string& v) { return v.empty() ? *this : str(k, v.substr(0, 1023)); }
    JObj& raw(const char* k, const std::string& json) { key(k); s += json; return *this; }
    JObj& rational(const char* k, long long n, long long d) { return raw(k, "{\"num\":" + std::to_string(n) + ",\"denom\":" + std::to_string(d) + "}"); }
    bool empty() const { return first; }
    std::string done() const { return s + "}"; }
};

}  // namespace

// ---------------------------------------------------------------- FreeD

uint8_t freedChecksum(const uint8_t* p, size_t n) {
    uint8_t sum = 0x40;
    for (size_t i = 0; i < n; ++i) sum = uint8_t(sum - p[i]);
    return sum;
}

std::array<uint8_t, 29> buildFreeD(const CameraSample& s, const FreeDConfig& c) {
    std::array<uint8_t, 29> p{};
    p[0] = 0xD1;
    p[1] = c.cameraId;
    put24(&p[2], clampSigned24((s.hasPan ? c.signs.pan(s.pan) : 0) * 32768.0));
    put24(&p[5], clampSigned24((s.hasTilt ? c.signs.tilt(s.tilt) : 0) * 32768.0));
    put24(&p[8], clampSigned24((s.hasRoll ? c.signs.roll(s.roll) : 0) * 32768.0));
    // X, Y, Z (bytes 11..19) stay 0: no position source.
    put24(&p[20], s.hasFocal ? clampUnsigned24(s.focalMm * c.zoomCountsPerMm) : 0);
    int32_t focus = 0;
    if (s.hasFocus) focus = s.focusInfinite ? 0xFFFFFF : clampUnsigned24(s.focusM * 1000.0 * c.focusCountsPerMm);
    put24(&p[23], focus);
    if (c.irisInSpare && s.hasIris) {
        const long v = std::clamp(std::lround(s.iris * c.irisSpareScale), 0L, 0xFFFFL);
        p[26] = uint8_t(v >> 8);
        p[27] = uint8_t(v);
    }
    p[28] = freedChecksum(p.data(), 28);
    return p;
}

// ---------------------------------------------------------------- OSC

std::vector<uint8_t> oscMessage(const std::string& address, float value) {
    std::vector<uint8_t> b;
    putOscString(b, address);
    putOscString(b, ",f");
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    putBe32(b, bits);
    return b;
}

std::vector<uint8_t> oscMessage(const std::string& address, const std::string& value) {
    std::vector<uint8_t> b;
    putOscString(b, address);
    putOscString(b, ",s");
    putOscString(b, value);
    return b;
}

std::vector<uint8_t> oscMessage(const std::string& address, int32_t value) {
    std::vector<uint8_t> b;
    putOscString(b, address);
    putOscString(b, ",i");
    putBe32(b, uint32_t(value));
    return b;
}

std::vector<uint8_t> oscBundle(const std::vector<std::vector<uint8_t>>& messages) {
    std::vector<uint8_t> b;
    putOscString(b, "#bundle");
    putBe32(b, 0);  // time tag 1 = "immediately"
    putBe32(b, 1);
    for (const auto& m : messages) {
        putBe32(b, uint32_t(m.size()));
        b.insert(b.end(), m.begin(), m.end());
    }
    return b;
}

std::vector<std::vector<uint8_t>> buildOsc(const CameraSample& s, const OscConfig& c) {
    std::vector<std::vector<uint8_t>> msgs;
    for (const auto& v : catalogue(s, c.focusUnit, c.infinityMetres)) {
        if (!v.available || !c.enabled(v.id)) continue;
        const std::string addr = c.prefix + "/" + v.id;
        double num = v.num;
        if (!std::strcmp(v.id, "pan")) num = c.signs.pan(num);
        else if (!std::strcmp(v.id, "tilt")) num = c.signs.tilt(num);
        else if (!std::strcmp(v.id, "roll")) num = c.signs.roll(num);
        switch (v.type) {
            case VarType::Float: msgs.push_back(oscMessage(addr, float(num))); break;
            case VarType::Int: msgs.push_back(oscMessage(addr, int32_t(std::lround(v.num)))); break;
            case VarType::String: msgs.push_back(oscMessage(addr, v.str)); break;
        }
    }
    if (c.bundle && !msgs.empty()) return {oscBundle(msgs)};
    return msgs;
}

// ---------------------------------------------------------------- OpenTrackIO

uint16_t fletcher16(const uint8_t* data, size_t len, uint8_t sum1, uint8_t sum2) {
    // Reference algorithm from the spec: 8-bit running sums (mod 256).
    for (size_t i = 0; i < len; ++i) {
        sum1 = uint8_t(sum1 + data[i]);
        sum2 = uint8_t(sum2 + sum1);
    }
    return uint16_t(sum2 << 8 | sum1);
}

std::string randomUuidUrn() {
    static std::mt19937_64 rng{std::random_device{}()};
    uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        const uint64_t r = rng();
        std::memcpy(b + i, &r, 8);
    }
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
    char s[64];
    std::snprintf(s, sizeof s, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

std::string openTrackIOMulticast(int sourceNumber) {
    return "239.135.1." + std::to_string(std::clamp(sourceNumber, 1, 200));
}

std::string buildOpenTrackIOJson(const CameraSample& s, const OpenTrackIOConfig& c, const OpenTrackIOFrame& f) {
    JObj root;
    root.raw("protocol", "{\"name\":\"OpenTrackIO\",\"version\":[1,0,1]}");
    root.str("sampleId", f.sampleId).str("sourceId", f.sourceId).integer("sourceNumber", std::clamp(c.sourceNumber, 1, 200));

    // timing
    JObj timing;
    timing.str("mode", "external").rational("sampleRate", f.rateNum, f.rateDen).integer("sequenceNumber", f.sequence);
    timing.raw("synchronization", "{\"locked\":true,\"source\":\"videoIn\"}");
    if (s.tc.valid) {
        // Timecode counts at most 30 frames: 50p / 59.94p TC runs at half the video rate.
        long long tn = f.rateNum, td = f.rateDen;
        if (td && double(tn) / td > 30.5) tn /= 2;
        JObj tc;
        tc.integer("hours", s.tc.h).integer("minutes", s.tc.m).integer("seconds", s.tc.s).integer("frames", s.tc.f);
        tc.rational("frameRate", tn, td).boolean("dropFrame", s.tc.dropFrame);
        timing.raw("timecode", tc.done());
    }
    root.raw("timing", timing.done());

    // tracker
    JObj tracker;
    if (s.rec != RecState::Unknown) tracker.boolean("recording", s.rec == RecState::Recording);
    tracker.strIf("slate", s.clip);
    tracker.str("status", *s.source ? std::string("SDI metadata: ") + s.source : "No camera metadata");
    root.raw("tracker", tracker.done());

    // lens
    JObj lens;
    if (s.hasFocus) lens.num("focusDistance", s.focusInfinite ? c.infinityMetres : std::max(s.focusM, 1e-6));
    if (s.hasIris && s.iris > 0) lens.num(s.irisKind == 'T' ? "tStop" : "fStop", s.iris);
    if (s.hasFocal && c.focalAsPinhole && s.focalMm > 0) lens.num("pinholeFocalLength", s.focalMm);
    if (s.hasFocusNorm)  // OpenTrackIO focus encoder: 0 = infinity, 1 = closest; Blackmagic is 0 near .. 1 far
        lens.raw("encoders", "{\"focus\":" + jnum(std::clamp(1.0 - s.focusNorm, 0.0, 1.0)) + "}");
    if (!lens.empty()) root.raw("lens", lens.done());

    if (c.includeTransform && (s.hasTilt || s.hasRoll || s.hasPan)) {
        root.raw("transforms", "[{\"id\":\"Camera\",\"translation\":{\"x\":0,\"y\":0,\"z\":0},\"rotation\":{\"pan\":" +
                                   jnum(s.hasPan ? c.signs.pan(s.pan) : 0) + ",\"tilt\":" +
                                   jnum(s.hasTilt ? c.signs.tilt(s.tilt) : 0) + ",\"roll\":" +
                                   jnum(s.hasRoll ? c.signs.roll(s.roll) : 0) + "}}]");
    }

    if (f.includeStatic) {
        JObj cam;
        cam.strIf("make", s.cameraMake).strIf("model", s.cameraModel).strIf("serialNumber", s.cameraSerial)
            .strIf("firmwareVersion", s.cameraFirmware);
        if (s.hasSensor)
            cam.raw("activeSensorPhysicalDimensions",
                    "{\"width\":" + jnum(s.sensorWidthMm) + ",\"height\":" + jnum(s.sensorHeightMm) + "}");
        if (s.hasCaptureRate) cam.rational("captureFrameRate", s.captureRateNum, s.captureRateDen);
        if (s.hasSqueeze && s.squeeze > 0) {
            long long n = std::llround(s.squeeze * 1000), d = 1000;  // e.g. 1.333 -> 1333/1000, 2.0 -> 2/1
            for (long long g = std::gcd(n, d); g > 1; g = std::gcd(n, d)) { n /= g; d /= g; }
            cam.rational("anamorphicSqueeze", n, d);
        }
        if (s.hasIso && s.iso >= 1) cam.integer("isoSpeed", s.iso);
        if (s.hasShutter && s.shutterAngle >= 0 && s.shutterAngle <= 360) cam.num("shutterAngle", s.shutterAngle);
        JObj lensStatic;
        lensStatic.strIf("model", s.lensModel).strIf("serialNumber", s.lensSerial);
        JObj st;
        if (!cam.empty()) st.raw("camera", cam.done());
        if (!lensStatic.empty()) st.raw("lens", lensStatic.done());
        st.raw("tracker", "{\"make\":\"Metadork\",\"model\":\"sdimeta\",\"firmwareVersion\":\"1.0\"}");
        root.raw("static", st.done());
    }
    return root.done();
}

std::vector<std::vector<uint8_t>> openTrackIOPackets(const std::string& payload, uint16_t sequence) {
    const size_t maxSeg = 0x7FFF;  // 15-bit payload length
    std::vector<std::vector<uint8_t>> out;
    size_t off = 0;
    do {
        const size_t n = std::min(maxSeg, payload.size() - off);
        const bool last = off + n == payload.size();
        std::vector<uint8_t> p(16 + n);
        std::memcpy(p.data(), "OTrk", 4);
        p[4] = 0;     // reserved
        p[5] = 0x01;  // JSON
        p[6] = uint8_t(sequence >> 8);
        p[7] = uint8_t(sequence);
        p[8] = uint8_t(off >> 24);
        p[9] = uint8_t(off >> 16);
        p[10] = uint8_t(off >> 8);
        p[11] = uint8_t(off);
        const uint16_t lenField = uint16_t((last ? 0x8000 : 0) | n);
        p[12] = uint8_t(lenField >> 8);
        p[13] = uint8_t(lenField);
        std::memcpy(p.data() + 16, payload.data() + off, n);
        // Checksum over the header (excluding the checksum bytes) and the payload.
        const uint16_t h = fletcher16(p.data(), 14);
        const uint16_t ck = fletcher16(p.data() + 16, n, uint8_t(h), uint8_t(h >> 8));
        p[14] = uint8_t(ck >> 8);
        p[15] = uint8_t(ck);
        out.push_back(std::move(p));
        off += n;
    } while (off < payload.size());
    return out;
}
