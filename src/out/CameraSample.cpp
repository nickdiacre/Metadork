#include "out/CameraSample.h"
#include "out/SensorModes.h"
#include <cmath>
#include <cstdio>
#include <sstream>

namespace {

bool u16(const Rdd18Frame& r, uint16_t tag, uint16_t& v) {
    const Rdd18Item* i = r.find(Rdd18SetKind::Lens, tag);
    if (!i || i->value.size() != 2) return false;
    v = uint16_t(i->value[0] << 8 | i->value[1]);
    return v != 0xFFFF;  // 0xFFFF seen as "no value" on a non-electronic lens
}

uint64_t beValue(const uint8_t* p, size_t n) {
    uint64_t r = 0;
    for (size_t i = 0; i < n; ++i) r = (r << 8) | p[i];
    return r;
}
uint64_t beValue(const std::vector<uint8_t>& v) { return beValue(v.data(), v.size()); }

// Sony model numbers as reported in Camera attributes (8114), e.g. "MPC-3626 0010025 Version3.00".
const char* sonyModelName(const std::string& code) {
    if (code == "MPC-3610") return "VENICE";
    if (code == "MPC-3626" || code == "MPC-3628") return "VENICE 2";
    if (code == "MPC-2610") return "BURANO";
    return nullptr;
}

// Standard anamorphic squeeze values; a derived ratio within 0.01 of one snaps to it, otherwise it is kept (2 dp).
double snapSqueeze(double v) {
    for (double std : {1.0, 1.25, 1.3, 1.33, 1.5, 1.65, 1.8, 1.85, 2.0})
        if (std::fabs(v - std) <= 0.01) return std;
    return std::round(v * 100.0) / 100.0;
}

void fpsRational(double fps, int& num, int& den) {
    // Prefer the NTSC form for 23.976 / 29.97 / 59.94 etc.
    const double ntsc = fps * 1.001;
    if (std::fabs(ntsc - std::round(ntsc)) < 0.01 && std::fabs(fps - std::round(fps)) > 0.01) {
        num = int(std::lround(ntsc)) * 1000;
        den = 1001;
    } else {
        num = int(std::lround(fps * 1000));
        den = 1000;
        while (num % 10 == 0 && den % 10 == 0) { num /= 10; den /= 10; }
    }
}

void fillArri(const FrameMetadata& md, CameraSample& s) {
    const auto& a = md.arri;
    s.kind = CameraKind::Arri;
    s.source = "ARRI";
    s.cameraMake = "ARRI";
    s.cameraModel = a.cameraModel.empty() ? a.cameraTypeName : a.cameraModel;
    if (s.cameraModel.rfind("ARRI ", 0) == 0) s.cameraModel.erase(0, 5);  // "ARRI ALEXA Mini LF" -> "ALEXA Mini LF"
    if (a.cameraSerial) s.cameraSerial = std::to_string(a.cameraSerial);
    s.cameraFirmware = a.supName;
    s.lensModel = a.lensModel;
    if (a.lensSerial) s.lensSerial = std::to_string(a.lensSerial);
    s.reel = a.reel();
    s.clip = a.clip();
    if (a.focusInfinite) { s.hasFocus = true; s.focusInfinite = true; }
    else if (a.focusValid) { s.hasFocus = true; s.focusM = a.focusMm / 1000.0; }
    if (!std::isnan(a.focalLengthMm) && a.focalLengthMm > 0) { s.hasFocal = true; s.focalMm = a.focalLengthMm; }
    if (!std::isnan(a.tStop)) { s.hasIris = true; s.iris = a.tStop; s.irisKind = 'T'; }
    if (a.exposureIndex) { s.hasIso = true; s.iso = int(a.exposureIndex); }
    if (a.shutterAngle > 0) { s.hasShutter = true; s.shutterAngle = a.shutterAngle; }
    if (a.whiteBalanceK) { s.hasWhiteBalance = true; s.whiteBalanceK = int(a.whiteBalanceK); }
    if (a.sensorFps > 0) { s.hasCaptureRate = true; fpsRational(a.sensorFps, s.captureRateNum, s.captureRateDen); }
    if (a.pixelAspect > 0) { s.hasSqueeze = true; s.squeeze = a.pixelAspect; }
    // ARRI sends no sensor size: derive it from the camera model and the recording area's aspect.
    if (a.activeImageW > 0 && a.activeImageH > 0) {
        const SensorGuess g = guessSensorMode(s.cameraModel + " " + a.cameraTypeName, double(a.activeImageW) / a.activeImageH);
        if (g.mode) {
            s.hasSensor = true;
            s.sensorWidthMm = g.mode->widthMm();
            s.sensorHeightMm = g.mode->heightMm();
            s.sensorSource = std::string("derived: ") + g.mode->name;
        } else if (g.candidates.size() > 1) {
            s.sensorSource = "ambiguous:";
            for (const auto* m : g.candidates) s.sensorSource += std::string(" ") + m->name + ",";
            s.sensorSource.pop_back();
        } else {
            s.sensorSource = "recording mode not in table";
        }
    }
    if (a.hasTilt) { s.hasTilt = true; s.tilt = a.tiltDeg; }
    if (a.hasRoll) { s.hasRoll = true; s.roll = a.rollDeg; }
}

void fillSony(const FrameMetadata& md, CameraSample& s) {
    using K = Rdd18SetKind;
    const auto& r = md.rdd18;
    s.kind = CameraKind::Sony;
    s.source = "Sony RDD 18";
    s.cameraMake = "Sony";
    s.clip = md.sonyClip;
    s.umid = md.sonyUmid;

    // "MPC-3626 0010025 Version3.00": model code, serial, firmware
    std::istringstream attrs(r.text(K::Camera, 0x8114));
    std::string code, serial, fw;
    attrs >> code >> serial >> fw;
    const char* name = sonyModelName(code);
    s.cameraModel = name ? std::string(name) + " (" + code + ")" : code;
    s.cameraSerial = serial;
    s.cameraFirmware = fw;
    const std::string lensId = r.text(K::Lens, 0x8007);
    if (!lensId.empty() && lensId != "Unknown") s.lensSerial = lensId;

    uint16_t v;
    if (u16(r, 0x8001, v) || u16(r, 0x8002, v)) { s.hasFocus = true; s.focusM = rdd18Metres(v); }
    if (u16(r, 0x8005, v) && v != 0x8000) { s.hasFocal = true; s.focalMm = rdd18Metres(v) * 1000.0; }
    if (u16(r, 0x8008, v)) { s.hasIris = true; s.iris = rdd18FNumber(v); s.irisKind = 'T'; }
    else if (u16(r, 0x8000, v)) { s.hasIris = true; s.iris = rdd18FNumber(v); s.irisKind = 'f'; }

    if (const Rdd18Item *w = r.find(K::Camera, 0x8104), *h = r.find(K::Camera, 0x8105);
        w && h && w->value.size() == 2 && h->value.size() == 2) {
        s.hasSensor = true;
        s.sensorSource = "camera";
        s.sensorWidthMm = beValue(w->value) / 1000.0;  // micrometres
        s.sensorHeightMm = beValue(h->value) / 1000.0;
    }
    if (const Rdd18Item* i = r.find(K::Camera, 0x810B); i && !i->value.empty() && i->value.size() <= 4) {
        s.hasIso = true;
        s.iso = int(beValue(i->value));
    }
    if (const Rdd18Item* i = r.find(K::Camera, 0x8108); i && !i->value.empty() && i->value.size() <= 4) {
        s.hasShutter = true;
        s.shutterAngle = beValue(i->value) / 60.0;
    }
    if (const Rdd18Item* i = r.find(K::Camera, 0x810E); i && !i->value.empty() && i->value.size() <= 4) {
        s.hasWhiteBalance = true;
        s.whiteBalanceK = int(beValue(i->value));
    }
    if (const Rdd18Item* i = r.find(K::Camera, 0x8106); i && i->value.size() == 8) {
        const uint64_t num = beValue(i->value.data(), 4), den = beValue(i->value.data() + 4, 4);
        if (num && den) { s.hasCaptureRate = true; s.captureRateNum = int(num); s.captureRateDen = int(den); }
    }
    // Anamorphic squeeze: aspect of the desqueezed SDI image area (7F/E10F) over the sensor area aspect (8104/8105).
    if (const Rdd18Item* a = r.find(K::UserDefined, 0xE10F); a && a->value.size() == 8 && s.hasSensor && s.sensorHeightMm > 0) {
        const double w = double(beValue(a->value.data(), 4)), h = double(beValue(a->value.data() + 4, 4));
        if (w > 0 && h > 0) {
            s.hasSqueeze = true;
            s.squeeze = snapSqueeze((w / h) / (s.sensorWidthMm / s.sensorHeightMm));
        }
    }
    // Gravity-referenced orientation (verified on VENICE 2); there is no pan.
    if (const Rdd18Item* t = r.find(K::UserDefined, 0x8414); t && t->value.size() == 4) {
        s.hasTilt = true;
        s.tilt = rdd18TenthDegree(t->value);
    }
    if (const Rdd18Item* t = r.find(K::UserDefined, 0x8415); t && t->value.size() == 4) {
        s.hasRoll = true;
        s.roll = rdd18TenthDegree(t->value);
    }
}

long bmdInt(const BmdCommand* c, size_t i, size_t width) {
    if (!c || c->data.size() < (i + 1) * width) return 0;
    uint32_t r = 0;
    for (size_t k = 0; k < width; ++k) r |= uint32_t(c->data[i * width + k]) << (8 * k);  // little-endian
    return width == 2 ? int16_t(r) : width == 4 ? int32_t(r) : long(r);
}

void fillBmd(const FrameMetadata& md, CameraSample& s) {
    const auto& b = md.bmd;
    s.kind = CameraKind::Blackmagic;
    s.source = "Blackmagic";
    s.cameraMake = "Blackmagic Design";
    double v;
    if (b.fNumber(v)) { s.hasIris = true; s.iris = v; s.irisKind = 'f'; }
    if (b.focusNormalised(v)) { s.hasFocusNorm = true; s.focusNorm = v; }
    if (b.zoomMm(v)) { s.hasFocal = true; s.focalMm = v; }
    s.reel = b.text(12, 0);
    if (const BmdCommand* c = b.find(1, 14); c && c->type == 3) { s.hasIso = true; s.iso = int(bmdInt(c, 0, 4)); }
    if (const BmdCommand* c = b.find(1, 11); c && c->type == 3) { s.hasShutter = true; s.shutterAngle = bmdInt(c, 0, 4) / 100.0; }
    if (const BmdCommand* c = b.find(1, 2); c && c->type == 2) { s.hasWhiteBalance = true; s.whiteBalanceK = int(bmdInt(c, 0, 2)); }
    if (const BmdCommand* c = b.find(1, 9); c && c->type == 2 && bmdInt(c, 0, 2) > 0) {
        s.hasCaptureRate = true;
        s.captureRateNum = int(bmdInt(c, 0, 2));
        s.captureRateDen = 1;
    }
}

std::string fmtNum(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

const char* cameraKindName(CameraKind k) {
    switch (k) {
        case CameraKind::Arri: return "ARRI";
        case CameraKind::Sony: return "Sony";
        case CameraKind::Blackmagic: return "Blackmagic";
        default: return "None";
    }
}

CameraSample sampleFrom(const FrameMetadata& md) {
    CameraSample s;
    s.tc = md.tc;
    s.rec = md.rec;
    s.recSource = md.recSource;
    if (md.hasArri) fillArri(md, s);
    else if (md.rdd18.parsed()) fillSony(md, s);
    else if (md.bmd.present) fillBmd(md, s);
    if (s.clip.empty()) s.clip = md.sonyClip;
    return s;
}

const char* focusUnitName(FocusUnit u) {
    switch (u) {
        case FocusUnit::Millimetres: return "mm";
        case FocusUnit::Feet: return "ft";
        default: return "m";
    }
}

std::string VarValue::display() const {
    if (!available) return "n/a";
    const std::string u = *unit ? std::string(" ") + unit : "";
    switch (type) {
        case VarType::String: return str;
        case VarType::Int: return std::to_string(long(num)) + u;
        default: return fmtNum(std::fabs(num) >= 100 ? "%.1f" : "%.3f", num) + u;
    }
}

std::vector<VarValue> catalogue(const CameraSample& s, FocusUnit fu, double infinityMetres) {
    std::vector<VarValue> v;
    auto add = [&](const char* id, const char* label, const char* unit, VarType t, bool has, double x, std::string text = {}) {
        VarValue a{id, label, unit, t};
        a.available = has;
        a.num = x;
        a.str = std::move(text);
        v.push_back(std::move(a));
    };
    auto text = [&](const char* id, const char* label, const std::string& x) { add(id, label, "", VarType::String, !x.empty(), 0, x); };

    const double focusM = s.focusInfinite ? infinityMetres : s.focusM;
    const double focus = fu == FocusUnit::Millimetres ? focusM * 1000.0 : fu == FocusUnit::Feet ? focusM / 0.3048 : focusM;
    add("focus", "Focus distance", focusUnitName(fu), VarType::Float, s.hasFocus, focus);
    add("focus_normalised", "Focus position (0 near - 1 far)", "", VarType::Float, s.hasFocusNorm, s.focusNorm);
    add("iris", s.irisKind == 'T' ? "Iris (T-stop)" : "Iris (f-number)", "", VarType::Float, s.hasIris, s.iris);
    add("zoom", "Focal length", "mm", VarType::Float, s.hasFocal, s.focalMm);
    add("pan", "Pan", "deg", VarType::Float, s.hasPan, s.pan);
    add("tilt", "Tilt (+ up)", "deg", VarType::Float, s.hasTilt, s.tilt);
    add("roll", "Roll (+ clockwise from behind)", "deg", VarType::Float, s.hasRoll, s.roll);
    text("timecode", "Timecode", s.tc.valid ? s.tc.str() : std::string());
    add("recording", "Recording (1/0)", "", VarType::Int, s.rec != RecState::Unknown, s.rec == RecState::Recording ? 1 : 0);
    text("clip", "Clip", s.clip);
    text("reel", "Reel", s.reel);
    text("umid", "UMID (current take)", s.umid);
    add("sensor_width", "Sensor width (active)", "mm", VarType::Float, s.hasSensor, s.sensorWidthMm);
    add("sensor_height", "Sensor height (active)", "mm", VarType::Float, s.hasSensor, s.sensorHeightMm);
    add("iso", "ISO / EI", "", VarType::Int, s.hasIso, s.iso);
    add("shutter_angle", "Shutter angle", "deg", VarType::Float, s.hasShutter, s.shutterAngle);
    add("white_balance", "White balance", "K", VarType::Int, s.hasWhiteBalance, s.whiteBalanceK);
    add("capture_fps", "Capture frame rate", "fps", VarType::Float, s.hasCaptureRate, s.captureFps());
    add("squeeze", "Anamorphic squeeze", "x", VarType::Float, s.hasSqueeze, s.squeeze);
    text("camera", "Camera", s.cameraMake.empty() ? std::string() : s.cameraMake + " " + s.cameraModel);
    text("camera_serial", "Camera serial", s.cameraSerial);
    text("lens", "Lens", s.lensModel.empty() ? s.lensSerial : s.lensModel);
    return v;
}
