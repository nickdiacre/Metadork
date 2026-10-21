#include "anc/BmdCameraControl.h"
#include "anc/AncRegistry.h"
#include <cmath>
#include <cstdio>
#include <map>

namespace {

enum class Fmt { Raw, Fixed, FStop, Kelvin, ShutterAngle, Iso, RecFormat, Codec, Transport, DynRange, DisplayLut, Overlays,
                 Timecode, Bool, Text };

struct Info {
    const char* name;
    Fmt fmt;
};

const Info* lookup(uint8_t c, uint8_t p) {
    static const std::map<uint16_t, Info> m = {
        {0x0000, {"Focus (normalised, 0 near - 1 far)", Fmt::Fixed}},
        {0x0001, {"Instantaneous autofocus", Fmt::Raw}},
        {0x0002, {"Aperture (f-stop)", Fmt::FStop}},
        {0x0003, {"Aperture (normalised)", Fmt::Fixed}},
        {0x0004, {"Aperture (ordinal)", Fmt::Raw}},
        {0x0005, {"Instantaneous auto aperture", Fmt::Raw}},
        {0x0006, {"Optical image stabilisation", Fmt::Bool}},
        {0x0007, {"Zoom (focal length mm)", Fmt::Raw}},
        {0x0008, {"Zoom (normalised)", Fmt::Fixed}},
        {0x0009, {"Continuous zoom speed", Fmt::Fixed}},
        {0x0100, {"Video mode", Fmt::Raw}},
        {0x0101, {"Gain (legacy)", Fmt::Raw}},
        {0x0102, {"White balance / tint", Fmt::Kelvin}},
        {0x0105, {"Exposure (us)", Fmt::Raw}},
        {0x0106, {"Exposure (ordinal)", Fmt::Raw}},
        {0x0107, {"Dynamic range mode", Fmt::DynRange}},
        {0x0108, {"Sharpening level", Fmt::Raw}},
        {0x0109, {"Recording format", Fmt::RecFormat}},
        {0x010A, {"Auto exposure mode", Fmt::Raw}},
        {0x010B, {"Shutter angle", Fmt::ShutterAngle}},
        {0x010C, {"Shutter speed (1/x s)", Fmt::Raw}},
        {0x010D, {"Gain (dB)", Fmt::Raw}},
        {0x010E, {"ISO", Fmt::Iso}},
        {0x010F, {"Display LUT", Fmt::DisplayLut}},
        {0x0110, {"ND filter (stops)", Fmt::Fixed}},
        {0x0300, {"Overlay enables", Fmt::Raw}},
        {0x0303, {"Overlays", Fmt::Overlays}},
        {0x0400, {"Display brightness", Fmt::Fixed}},
        {0x0401, {"Exposure and focus tools", Fmt::Raw}},
        {0x0402, {"Zebra level", Fmt::Fixed}},
        {0x0403, {"Peaking level", Fmt::Fixed}},
        {0x0404, {"Colour bars", Fmt::Raw}},
        {0x0405, {"Focus assist", Fmt::Raw}},
        {0x0406, {"Program return feed", Fmt::Raw}},
        {0x0500, {"Tally brightness", Fmt::Fixed}},
        {0x0501, {"Front tally brightness", Fmt::Fixed}},
        {0x0502, {"Rear tally brightness", Fmt::Fixed}},
        {0x0600, {"Reference source", Fmt::Raw}},
        {0x0601, {"Reference offset", Fmt::Raw}},
        {0x0800, {"Lift", Fmt::Fixed}},
        {0x0801, {"Gamma", Fmt::Fixed}},
        {0x0802, {"Gain", Fmt::Fixed}},
        {0x0803, {"Offset", Fmt::Fixed}},
        {0x0804, {"Contrast", Fmt::Fixed}},
        {0x0805, {"Luma mix", Fmt::Fixed}},
        {0x0806, {"Colour adjust (hue, sat)", Fmt::Fixed}},
        {0x0904, {"Timecode", Fmt::Timecode}},
        {0x0A00, {"Codec", Fmt::Codec}},
        {0x0A01, {"Transport mode", Fmt::Transport}},
        {0x0C00, {"Reel", Fmt::Text}},
        {0x0C02, {"Scene", Fmt::Text}},
        {0x0C03, {"Take", Fmt::Raw}},
        {0x0C05, {"Camera ID", Fmt::Text}},
        {0x0C06, {"Camera operator", Fmt::Text}},
        {0x0C07, {"Director", Fmt::Text}},
        {0x0C08, {"Project name", Fmt::Text}},
        {0x0C09, {"Lens type", Fmt::Text}},
        {0x0C0A, {"Lens iris", Fmt::Text}},
        {0x0C0B, {"Lens focal length", Fmt::Text}},
        {0x0C0C, {"Lens distance", Fmt::Text}},
        {0x0C0D, {"Lens filter", Fmt::Text}},
    };
    auto it = m.find(uint16_t(c << 8 | p));
    return it == m.end() ? nullptr : &it->second;
}

size_t elemSize(uint8_t type) {
    switch (type) {
        case 1: case 0: return 1;
        case 2: case 128: return 2;
        case 3: return 4;
        case 4: return 8;
        default: return 1;
    }
}

int64_t elem(const BmdCommand& c, size_t i) {
    const size_t n = elemSize(c.type), o = i * n;
    if (o + n > c.data.size()) return 0;
    uint64_t v = 0;
    for (size_t k = 0; k < n; ++k) v |= uint64_t(c.data[o + k]) << (8 * k);
    switch (n) {  // sign-extend
        case 1: return int8_t(v);
        case 2: return int16_t(v);
        case 4: return int32_t(v);
        default: return int64_t(v);
    }
}

size_t count(const BmdCommand& c) { return c.data.size() / elemSize(c.type); }

double fixed16(int64_t v) { return double(v) / 2048.0; }

std::string f(const char* fmt, double v) {
    char b[64];
    std::snprintf(b, sizeof b, fmt, v);
    return b;
}

std::string list(const BmdCommand& c) {
    if (c.type == 5) return std::string(c.data.begin(), c.data.end());
    std::string s;
    for (size_t i = 0; i < count(c); ++i) {
        if (i) s += ", ";
        s += c.type == 128 ? f("%.4f", fixed16(elem(c, i))) : std::to_string(elem(c, i));
    }
    return s.empty() ? "(none)" : s;
}

std::string display(Fmt fmt, const BmdCommand& c) {
    const size_t n = count(c);
    switch (fmt) {
        case Fmt::FStop:
            if (c.type == 128 && n >= 1) return f("f/%.2f", std::sqrt(std::pow(2.0, fixed16(elem(c, 0)))));
            break;
        case Fmt::Kelvin:
            if (n >= 2) return std::to_string(elem(c, 0)) + " K, tint " + std::to_string(elem(c, 1));
            break;
        case Fmt::ShutterAngle:
            if (n >= 1) return f("%.2f deg", elem(c, 0) / 100.0);
            break;
        case Fmt::Iso:
            if (n >= 1) return std::to_string(elem(c, 0));
            break;
        case Fmt::RecFormat:
            if (n >= 4) {
                std::string s = std::to_string(elem(c, 2)) + "x" + std::to_string(elem(c, 3)) + "  " +
                                std::to_string(elem(c, 0)) + " fps";
                if (elem(c, 1) && elem(c, 1) != elem(c, 0)) s += ", off-speed " + std::to_string(elem(c, 1)) + " fps";
                if (n >= 5 && elem(c, 4)) s += ", flags " + std::to_string(elem(c, 4));
                return s;
            }
            break;
        case Fmt::Codec:
            if (n >= 2) {
                static const char* codecs[] = {"CinemaDNG", "DNxHD", "ProRes", "Blackmagic RAW"};
                const int64_t b = elem(c, 0);
                return std::string(b >= 0 && b < 4 ? codecs[b] : "codec " + std::to_string(b)) + ", variant " +
                       std::to_string(elem(c, 1));
            }
            break;
        case Fmt::Transport:
            if (n >= 1) {
                static const char* modes[] = {"Preview", "Play", "RECORD"};
                const int64_t m = elem(c, 0);
                std::string s = m >= 0 && m < 3 ? modes[m] : "mode " + std::to_string(m);
                if (n >= 3) s += "  (speed " + std::to_string(elem(c, 1)) + ", flags " + std::to_string(elem(c, 2) & 0xFF) + ")";
                return s;
            }
            break;
        case Fmt::DynRange:
            if (n >= 1) {
                static const char* m[] = {"Film", "Video", "Extended video"};
                const int64_t v = elem(c, 0);
                return v >= 0 && v < 3 ? m[v] : std::to_string(v);
            }
            break;
        case Fmt::DisplayLut:
            if (n >= 2) {
                static const char* m[] = {"None", "Custom", "Film to video", "Film to extended video"};
                const int64_t v = elem(c, 0);
                return std::string(v >= 0 && v < 4 ? m[v] : std::to_string(v)) + (elem(c, 1) ? ", enabled" : ", disabled");
            }
            break;
        case Fmt::Overlays:
            if (n >= 4)
                return "guide style " + std::to_string(elem(c, 0)) + ", opacity " + std::to_string(elem(c, 1)) +
                       "%, safe area " + std::to_string(elem(c, 2)) + "%, grid " + std::to_string(elem(c, 3));
            break;
        case Fmt::Timecode:
            if (c.data.size() >= 4) {
                char b[16];
                std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X", c.data[3], c.data[2], c.data[1], c.data[0]);
                return b;
            }
            break;
        case Fmt::Bool:
            if (n >= 1) return elem(c, 0) ? "on" : "off";
            break;
        default: break;
    }
    return list(c);
}

}  // namespace

const char* bmdCategoryName(uint8_t c) {
    static const char* names[] = {"Lens", "Video", "Audio", "Output", "Display", "Tally", "Reference",
                                  "Configuration", "Colour correction", "Status (undocumented)", "Media", "PTZ",
                                  "Metadata"};
    return c < 13 ? names[c] : "Unknown";
}

std::vector<BmdCommand> parseBmdCommands(const std::vector<uint8_t>& d) {
    std::vector<BmdCommand> out;
    size_t i = 0;
    while (i + 8 <= d.size()) {
        const size_t len = d[i + 1];
        if (len < 4 || i + 4 + len > d.size()) break;  // padding / end of packet
        const uint8_t* b = &d[i + 4];
        BmdCommand c;
        c.category = b[0];
        c.parameter = b[1];
        c.type = b[2];
        c.operation = b[3];
        c.data.assign(b + 4, b + len);
        const Info* info = lookup(c.category, c.parameter);
        c.name = info ? info->name : "";
        c.display = display(info ? info->fmt : Fmt::Raw, c);
        out.push_back(std::move(c));
        i += 4 + ((len + 3) & ~size_t(3));
    }
    return out;
}

BmdFrame parseBmd(const std::vector<AncPacket>& packets) {
    BmdFrame f;
    for (const auto& p : packets) {
        if (p.did != 0x51) continue;
        if (p.sdid == 0x52) { f.hasTally = true; f.tally = p.data; }
        if (p.sdid == 0x53) {
            f.present = true;
            for (auto& c : parseBmdCommands(p.data)) f.commands.push_back(std::move(c));
        }
    }
    return f;
}

const BmdCommand* BmdFrame::find(uint8_t c, uint8_t p) const {
    for (const auto& x : commands)
        if (x.category == c && x.parameter == p) return &x;
    return nullptr;
}

std::string BmdFrame::text(uint8_t c, uint8_t p) const {
    const BmdCommand* x = find(c, p);
    return x ? x->display : std::string();
}

bool BmdFrame::fNumber(double& v) const {
    const BmdCommand* c = find(0, 2);
    if (!c || c->type != 128 || c->data.size() < 2) return false;
    v = std::sqrt(std::pow(2.0, fixed16(elem(*c, 0))));
    return true;
}

bool BmdFrame::focusNormalised(double& v) const {
    const BmdCommand* c = find(0, 0);
    if (!c || c->type != 128 || c->data.size() < 2) return false;
    v = fixed16(elem(*c, 0));
    return true;
}

bool BmdFrame::zoomMm(double& mm) const {
    const BmdCommand* c = find(0, 7);
    if (!c || c->data.empty()) return false;
    mm = double(elem(*c, 0));
    return mm > 0;
}

bool BmdFrame::transportMode(int& mode) const {
    const BmdCommand* c = find(10, 1);
    if (!c || c->data.empty()) return false;
    mode = int(elem(*c, 0));
    return true;
}
