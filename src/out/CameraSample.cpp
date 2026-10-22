#include "out/CameraSample.h"
#include "meta/Adapters.h"
#include <cmath>
#include <cstdio>

namespace {

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

std::string fmtNum(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

void CameraSample::refresh() {
    const MetaRecord& r = record;
    kind = r.kind;
    auto str = [&](Field f) { return r.str(f); };
    cameraMake = str(Field::CameraMake);
    cameraModel = str(Field::CameraModel);
    cameraSerial = str(Field::CameraSerial);
    cameraFirmware = str(Field::CameraFirmware);
    lensModel = str(Field::LensModel);
    lensSerial = str(Field::LensSerial);

    hasFocus = r.has(Field::FocusDistance);
    focusInfinite = hasFocus && r.get(Field::FocusDistance).note == "infinity";
    focusM = r.num(Field::FocusDistance);
    hasFocusNorm = r.has(Field::FocusPosition);
    focusNorm = r.num(Field::FocusPosition);
    hasFocal = r.has(Field::FocalLength);
    focalMm = r.num(Field::FocalLength);
    hasIris = r.has(Field::Iris);
    iris = r.num(Field::Iris);
    // T-stop from ARRI 0x384 and Sony 8008; f-number from Sony 8000 and Blackmagic 0.2
    const std::string& irisOrigin = r.get(Field::Iris).origin;
    irisKind = !hasIris ? 0 : (irisOrigin.find("8000") != std::string::npos || irisOrigin.rfind("bmd", 0) == 0) ? 'f' : 'T';

    hasPan = r.has(Field::Pan);
    pan = r.num(Field::Pan);
    hasTilt = r.has(Field::Tilt);
    tilt = r.num(Field::Tilt);
    hasRoll = r.has(Field::Roll);
    roll = r.num(Field::Roll);

    hasSensor = r.has(Field::SensorWidth) && r.has(Field::SensorHeight);
    sensorWidthMm = r.num(Field::SensorWidth);
    sensorHeightMm = r.num(Field::SensorHeight);
    const FieldValue& sw = r.get(Field::SensorWidth);
    sensorSource = !sw.set ? sw.note : sw.source == ValueSource::Camera ? "camera" : sw.source == ValueSource::Manual ? "manual" : sw.origin;
    hasIso = r.has(Field::Iso);
    iso = int(std::lround(r.num(Field::Iso)));
    hasShutter = r.has(Field::ShutterAngle);
    shutterAngle = r.num(Field::ShutterAngle);
    hasWhiteBalance = r.has(Field::WhiteBalance);
    whiteBalanceK = int(std::lround(r.num(Field::WhiteBalance)));
    hasSqueeze = r.has(Field::Squeeze);
    squeeze = r.num(Field::Squeeze, 1.0);
    hasCaptureRate = r.has(Field::CaptureFps);
    captureRateNum = 0;
    captureRateDen = 1;
    if (hasCaptureRate) fpsRational(r.num(Field::CaptureFps), captureRateNum, captureRateDen);

    reel = str(Field::Reel);
    clip = str(Field::Clip);
    umid = str(Field::Umid);
}

CameraSample sampleFrom(const FrameMetadata& md) {
    CameraSample s;
    s.record = buildRecord(md);
    s.tc = md.tc;
    s.rec = md.rec;
    s.recSource = md.recSource;
    for (const auto& a : cameraAdapters())
        if (a.kind == s.record.kind) s.source = a.name;
    s.refresh();
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
        case VarType::Int: return std::to_string(long(std::lround(num))) + u;
        default: return fmtNum(std::fabs(num) >= 100 ? "%.1f" : "%.3f", num) + u;
    }
}

std::vector<VarValue> catalogue(const CameraSample& s, FocusUnit fu, double infinityMetres) {
    std::vector<VarValue> out;
    out.reserve(kFieldCount);
    for (const auto& f : allFields()) {
        const FieldValue& v = s.record.get(f.field);
        VarValue a{f.id, f.label, f.unit,
                   f.type == FieldType::Float ? VarType::Float : f.type == FieldType::Int ? VarType::Int : VarType::String};
        a.group = f.group;
        a.value = &v;
        a.available = v.set;
        a.num = v.num;
        a.str = v.str;
        if (f.field == Field::FocusDistance) {
            const double m = s.focusInfinite ? infinityMetres : v.num;
            a.num = fu == FocusUnit::Millimetres ? m * 1000.0 : fu == FocusUnit::Feet ? m / 0.3048 : m;
            a.unit = focusUnitName(fu);
        }
        if (f.field == Field::Iris && v.set) a.label = s.irisKind == 'T' ? "Iris (T-stop)" : "Iris (f-number)";
        out.push_back(std::move(a));
    }
    return out;
}
