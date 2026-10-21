#include "anc/Rdd18.h"
#include "anc/AncRegistry.h"
#include "anc/Rp214.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

const uint8_t kSetPrefix[12] = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x53, 0x01, 0x01, 0x0C, 0x02, 0x01, 0x01};
const uint8_t kUlPrefix[4] = {0x06, 0x0E, 0x2B, 0x34};

enum class Fmt { Auto, Uint, String, Ul, Rational, MicronsToMm, Fps, ShutterAngle, ShutterTime, GainDb, Kelvin,
                 Percent, Readout, Bool, Metres, FNumber, TNumber, TenthDegree };

struct TagInfo {
    const char* name;
    Fmt fmt;
};

const TagInfo* lookup(Rdd18SetKind kind, uint16_t tag) {
    static const std::map<uint16_t, TagInfo> lens = {
        {0x8000, {"Iris F-number", Fmt::FNumber}},
        {0x8001, {"Focus position (image plane)", Fmt::Metres}},
        {0x8002, {"Focus position (front lens vertex)", Fmt::Metres}},
        {0x8003, {"Macro setting", Fmt::Bool}},
        {0x8004, {"Zoom (35mm still equivalent)", Fmt::Metres}},
        {0x8005, {"Zoom (actual focal length)", Fmt::Metres}},
        {0x8006, {"Optical extender magnification", Fmt::Uint}},
        {0x8007, {"Lens attributes", Fmt::String}},
        {0x8008, {"Iris T-number", Fmt::TNumber}},
        {0x8009, {"Iris ring position", Fmt::Uint}},
        {0x800A, {"Focus ring position", Fmt::Uint}},
        {0x800B, {"Zoom ring position", Fmt::Uint}},
    };
    static const std::map<uint16_t, TagInfo> camera = {
        {0x3210, {"Capture gamma equation", Fmt::Ul}},
        {0x3219, {"Colour primaries", Fmt::Ul}},
        {0x321A, {"Coding equations", Fmt::Ul}},
        {0x8100, {"Auto exposure mode", Fmt::Ul}},
        {0x8101, {"Auto focus sensing area", Fmt::Uint}},
        {0x8102, {"Colour correction filter wheel", Fmt::Uint}},
        {0x8103, {"ND filter wheel", Fmt::Uint}},
        {0x8104, {"Sensor effective width", Fmt::MicronsToMm}},
        {0x8105, {"Sensor effective height", Fmt::MicronsToMm}},
        {0x8106, {"Capture frame rate", Fmt::Fps}},
        {0x8107, {"Sensor readout mode", Fmt::Readout}},
        {0x8108, {"Shutter angle", Fmt::ShutterAngle}},
        {0x8109, {"Shutter speed", Fmt::ShutterTime}},
        {0x810A, {"Master gain", Fmt::GainDb}},
        {0x810B, {"ISO sensitivity", Fmt::Uint}},
        {0x810C, {"Electrical extender magnification", Fmt::Percent}},
        {0x810D, {"Auto white balance mode", Fmt::Uint}},
        {0x810E, {"White balance", Fmt::Kelvin}},
        {0x810F, {"Master black level", Fmt::Uint}},
        {0x8110, {"Knee point", Fmt::Uint}},
        {0x8111, {"Knee slope", Fmt::Uint}},
        {0x8112, {"Luminance dynamic range", Fmt::Uint}},
        {0x8113, {"Camera setting file URI", Fmt::String}},
        {0x8114, {"Camera attributes", Fmt::String}},
        {0x8115, {"Exposure index of photo meter", Fmt::Uint}},
        {0x8116, {"Gamma for CDL", Fmt::Uint}},
        {0x8117, {"ASC CDL v1.2", Fmt::Auto}},
        {0x8118, {"Colour matrix", Fmt::Auto}},
    };
    static const std::map<uint16_t, TagInfo> user = {
        {0xE000, {"Set instance UID", Fmt::Auto}},
        {0xE101, {"Effective marker coverage", Fmt::Rational}},
        {0xE102, {"Effective marker aspect ratio", Fmt::Rational}},
        {0xE103, {"Camera process discrimination code", Fmt::Uint}},
        {0xE104, {"Rotary shutter mode", Fmt::Uint}},
        {0xE105, {"Raw black code value", Fmt::Uint}},
        {0xE106, {"Raw gray code value", Fmt::Uint}},
        {0xE107, {"Raw white code value", Fmt::Uint}},
        {0xE109, {"Monitoring descriptions", Fmt::String}},
        // Desqueezed image area on the SDI output (px). Its aspect / sensor aspect = anamorphic squeeze
        // (VENICE 2: 1620x1080 at 1.0x on a 3:2 area, 1920x1024 at 1.25x ... 1920x712 at 1.8x).
        {0xE10F, {"Desqueezed image area (SDI px)", Fmt::Rational}},
        {0xE10B, {"Monitoring base curve", Fmt::Ul}},
        // Verified on VENICE 2 with a phone inclinometer (value / 36000 = degrees): tilt level ~0, 20 deg up ~ +20.3,
        // 20 deg down ~ -20.6; roll level 0.0, ~20 deg clockwise (seen from behind) +17.7, anticlockwise -16.1.
        {0x8414, {"Camera tilt (+ up)", Fmt::TenthDegree}},
        {0x8415, {"Camera roll (+ clockwise from behind)", Fmt::TenthDegree}},
    };
    const auto& m = kind == Rdd18SetKind::Lens ? lens : kind == Rdd18SetKind::Camera ? camera : user;
    auto it = m.find(tag);
    return it == m.end() ? nullptr : &it->second;
}

uint64_t be(const std::vector<uint8_t>& v, size_t off, size_t n) {
    uint64_t r = 0;
    for (size_t i = 0; i < n && off + i < v.size(); ++i) r = (r << 8) | v[off + i];
    return r;
}

bool isUl(const std::vector<uint8_t>& v) { return v.size() == 16 && std::memcmp(v.data(), kUlPrefix, 4) == 0; }

std::string asciiTrim(const std::vector<uint8_t>& v) {
    std::string s(v.begin(), v.end());
    s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
    return s;
}

bool looksLikeString(const std::vector<uint8_t>& v) {
    std::string s = asciiTrim(v);
    if (s.size() < 3) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

std::string ulName(const std::vector<uint8_t>& v) {
    static const std::map<std::string, const char*> known = {
        {"060e2b34040101010401010101020000", "ITU-R BT.709 transfer"},
        {"060e2b34040101010401010102020000", "ITU-R BT.709 coding equations"},
        {"060e2b34040101060401010103030000", "ITU-R BT.709 primaries"},
        {"060e2b340401010b0510010101010000", "Manual exposure"},
    };
    std::string hex = toHex(v.data(), v.size(), SIZE_MAX, 0);
    std::transform(hex.begin(), hex.end(), hex.begin(), ::tolower);
    auto it = known.find(hex);
    if (it != known.end()) return it->second;
    // 06 0E 2B 34 .. .. .. .. 0E 06 = Sony-registered label
    if (v[8] == 0x0E && v[9] == 0x06) return "Sony label " + toHex(v.data() + 8, 8, SIZE_MAX, '.');
    return "UL " + toHex(v.data(), 16, SIZE_MAX, '.');
}

std::string fmtD(const char* f, double d) {
    char b[64];
    std::snprintf(b, sizeof b, f, d);
    return b;
}

}  // namespace

// RDD 18 16-bit distance: top 4 bits = signed power-of-ten exponent, low 12 bits = mantissa, in metres.
// Confirmed on BURANO: C352 = 850e-4 m = 85 mm, C37C = 89.2 mm (its 35mm equivalent), E2BE = 7.02 m.
double rdd18Metres(uint16_t v) {
    int e = v >> 12;
    if (e >= 8) e -= 16;
    return (v & 0x0FFF) * std::pow(10.0, e);
}

// RDD 18 iris: F = 2^(8 * (1 - n / 65536)). F05F -> f/1.40 on an 85mm f/1.4.
double rdd18FNumber(uint16_t v) { return std::pow(2.0, 8.0 * (1.0 - v / 65536.0)); }

// Sony 7F/8414, 8415: signed int32, always a multiple of 3600; value / 36000 = degrees (0.1 deg resolution).
double rdd18TenthDegree(const std::vector<uint8_t>& v) {
    if (v.size() != 4) return 0;
    return int32_t(uint32_t(v[0]) << 24 | uint32_t(v[1]) << 16 | uint32_t(v[2]) << 8 | v[3]) / 36000.0;
}

namespace {

std::string uintText(const std::vector<uint8_t>& v) {
    const uint64_t n = be(v, 0, v.size());
    char b[48];
    std::snprintf(b, sizeof b, "%llu  (0x%0*llX)", (unsigned long long)n, int(v.size() * 2), (unsigned long long)n);
    return b;
}

std::string display(Fmt f, const std::vector<uint8_t>& v) {
    const size_t n = v.size();
    if (n == 0) return "(empty)";
    if (f == Fmt::Auto) {
        if (isUl(v)) f = Fmt::Ul;
        else if (looksLikeString(v)) f = Fmt::String;
        else if (n <= 4) f = Fmt::Uint;
    }
    switch (f) {
        case Fmt::String: return asciiTrim(v);
        case Fmt::Ul: return isUl(v) ? ulName(v) : toHex(v.data(), n);
        case Fmt::Bool: return v[0] ? "on" : "off";
        case Fmt::Metres:
            if (n == 2) {
                const uint16_t raw = uint16_t(be(v, 0, 2));
                if (raw == 0xFFFF) return "n/a (FFFF)";
                const double m = rdd18Metres(raw);
                if (m < 1.0) return fmtD("%.1f mm", m * 1000.0);
                return fmtD("%.3f m", m) + fmtD("  (%.2f ft)", m / 0.3048);
            }
            break;
        case Fmt::FNumber:
            if (n == 2) return fmtD("f/%.2f", rdd18FNumber(uint16_t(be(v, 0, 2))));
            break;
        case Fmt::TenthDegree:
            if (n == 4) return fmtD("%.1f deg", rdd18TenthDegree(v));
            break;
        case Fmt::TNumber:
            if (n == 2) return fmtD("T%.2f", rdd18FNumber(uint16_t(be(v, 0, 2))));
            break;
        case Fmt::Uint: if (n <= 8) return uintText(v); break;
        case Fmt::MicronsToMm: if (n <= 4) return fmtD("%.3f mm", double(be(v, 0, n)) / 1000.0); break;
        case Fmt::ShutterAngle: if (n <= 4) return fmtD("%.2f deg", double(be(v, 0, n)) / 60.0); break;
        case Fmt::GainDb: if (n == 2) return fmtD("%.2f dB", double(int16_t(be(v, 0, 2))) / 100.0); break;
        case Fmt::Kelvin: if (n <= 4) return std::to_string(be(v, 0, n)) + " K"; break;
        case Fmt::Percent: if (n <= 4) return std::to_string(be(v, 0, n)) + " %"; break;
        case Fmt::Readout:
            if (n == 1) {
                static const char* m[] = {"interlaced field", "interlaced frame", "progressive frame"};
                return v[0] < 3 ? m[v[0]] : uintText(v);
            }
            break;
        case Fmt::Rational:
        case Fmt::Fps:
        case Fmt::ShutterTime:
            if (n == 8) {
                const uint32_t num = uint32_t(be(v, 0, 4)), den = uint32_t(be(v, 4, 4));
                if (!den) break;
                if (f == Fmt::Fps) return fmtD("%.3f fps", double(num) / den) + "  (" + std::to_string(num) + "/" + std::to_string(den) + ")";
                if (f == Fmt::ShutterTime) return std::to_string(num) + "/" + std::to_string(den) + " s";
                return std::to_string(num) + "/" + std::to_string(den) + fmtD("  (%.3f)", double(num) / den);
            }
            break;
        default: break;
    }
    return toHex(v.data(), n);
}

Rdd18SetKind kindFor(uint8_t b) {
    switch (b) {
        case 0x01: return Rdd18SetKind::Lens;
        case 0x02: return Rdd18SetKind::Camera;
        case 0x7F: return Rdd18SetKind::UserDefined;
        default: return Rdd18SetKind::Other;
    }
}

}  // namespace

const char* rdd18SetName(Rdd18SetKind k) {
    switch (k) {
        case Rdd18SetKind::Lens: return "Lens unit";
        case Rdd18SetKind::Camera: return "Camera unit";
        case Rdd18SetKind::UserDefined: return "User defined (Sony)";
        default: return "Other";
    }
}

const Rdd18Item* Rdd18Frame::find(Rdd18SetKind kind, uint16_t tag) const {
    for (const auto& s : sets)
        if (s.kind == kind)
            for (const auto& i : s.items)
                if (i.tag == tag) return &i;
    return nullptr;
}

std::string Rdd18Frame::text(Rdd18SetKind kind, uint16_t tag) const {
    const Rdd18Item* i = find(kind, tag);
    return i ? i->display : std::string();
}

Rdd18Frame parseRdd18(const std::vector<AncPacket>& packets) {
    Rdd18Frame f;
    // Order slices by their sequence byte; ignore repeats (e.g. a second field).
    std::map<uint8_t, const AncPacket*> slices;
    for (const auto& p : packets)
        if (p.did == 0x43 && p.sdid == 0x05 && p.data.size() >= 2) {
            ++f.packets;
            slices.emplace(p.data[0], &p);
        }
    for (const auto& [seq, p] : slices) f.stream.insert(f.stream.end(), p->data.begin() + 1, p->data.end());

    const auto& s = f.stream;
    size_t i = 0;
    while (i + 17 <= s.size()) {
        if (std::memcmp(&s[i], kSetPrefix, sizeof kSetPrefix) != 0) { ++i; continue; }  // resync on next set key
        Rdd18Set set;
        set.setByte = s[i + 12];
        set.kind = kindFor(set.setByte);
        for (const auto& prev : f.sets) set.occurrence += prev.setByte == set.setByte;
        size_t len = 0;
        const size_t used = decodeBerLength(&s[i + 16], s.size() - i - 16, len);
        if (!used) { ++i; continue; }
        set.declaredLength = len;
        const size_t start = i + 16 + used;
        const size_t end = std::min(s.size(), start + len);
        size_t j = start;
        while (j + 4 <= end) {
            Rdd18Item it;
            it.tag = uint16_t(s[j] << 8 | s[j + 1]);
            const size_t l = size_t(s[j + 2] << 8 | s[j + 3]);
            if (j + 4 + l > end) break;
            it.value.assign(s.begin() + j + 4, s.begin() + j + 4 + l);
            const TagInfo* ti = lookup(set.kind, it.tag);
            it.name = ti ? ti->name : "";
            it.display = display(ti ? ti->fmt : Fmt::Auto, it.value);
            set.items.push_back(std::move(it));
            j += 4 + l;
        }
        set.complete = (start + len <= s.size()) && j == start + len;
        f.sets.push_back(std::move(set));
        // On a short set (missing slice) the next set's key may sit inside the declared length: rescan from where items stopped.
        i = std::max(f.sets.back().complete ? end : j, i + 1);
    }
    return f;
}

bool isSonyClipKey(const uint8_t* k) {
    static const uint8_t prefix[13] = {0x06, 0x0E, 0x2B, 0x34, 0x01, 0x01, 0x01, 0x06, 0x0E, 0x06, 0x01, 0x05, 0x01};
    return std::memcmp(k, prefix, sizeof prefix) == 0;
}

RecStateHint sonyRecordingFromClipPacket(const AncPacket& p, std::string& umidHex) {
    umidHex.clear();
    const auto& d = p.data;  // MID(1) PSC(2) key(16) BER length value [UMID(32)]
    if (p.did != 0x44 || p.sdid != 0x04 || d.size() < 3 + 17 || !isSonyClipKey(&d[3])) return RecStateHint::Unknown;
    size_t len = 0;
    const size_t used = decodeBerLength(&d[19], d.size() - 19, len);
    if (!used) return RecStateHint::Unknown;
    const size_t after = 19 + used + len;
    if (after + 32 > d.size()) return RecStateHint::Unknown;  // no UMID area (e.g. BURANO)
    static const uint8_t umidLabel[12] = {0x06, 0x0A, 0x2B, 0x34, 0x01, 0x01, 0x01, 0x05, 0x01, 0x01, 0x0D, 0x43};
    if (std::memcmp(&d[after], umidLabel, 12) == 0) {
        umidHex = toHex(&d[after], 32, SIZE_MAX, 0);
        return RecStateHint::Recording;
    }
    for (size_t i = after; i < after + 32; ++i)
        if (d[i]) return RecStateHint::Unknown;
    return RecStateHint::Stopped;
}

std::string sonyClipName(const std::vector<uint8_t>& v) {
    // Value seen: 20 20 00 01 'A002C002_261006HE' 00. Take the longest printable run.
    std::string best, cur;
    for (uint8_t c : v) {
        if (c > 0x20 && c < 0x7F) cur.push_back(char(c));
        else { if (cur.size() > best.size()) best = cur; cur.clear(); }
    }
    return cur.size() > best.size() ? cur : best;
}
