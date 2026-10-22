#include "meta/Adapters.h"
#include "out/SensorModes.h"
#include <cmath>
#include <sstream>

namespace {

using C = Confidence;
using K = Rdd18SetKind;

std::string hex(size_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "arri.0x%03zX", offset);
    return b;
}

uint64_t beValue(const uint8_t* p, size_t n) {
    uint64_t r = 0;
    for (size_t i = 0; i < n; ++i) r = (r << 8) | p[i];
    return r;
}
uint64_t beValue(const std::vector<uint8_t>& v) { return beValue(v.data(), v.size()); }

// Standard anamorphic squeeze values; a derived ratio within 0.01 of one snaps to it, otherwise it is kept (2 dp).
double snapSqueeze(double v) {
    for (double std : {1.0, 1.25, 1.3, 1.33, 1.5, 1.65, 1.8, 1.85, 2.0})
        if (std::fabs(v - std) <= 0.01) return std;
    return std::round(v * 100.0) / 100.0;
}

// ---------------------------------------------------------------- ARRI (4 KB SDI metadata block)

bool detectArri(const FrameMetadata& md) { return md.hasArri; }

void fillArri(const FrameMetadata& md, MetaRecord& r) {
    const auto& a = md.arri;
    std::string model = a.cameraModel.empty() ? a.cameraTypeName : a.cameraModel;
    if (model.rfind("ARRI ", 0) == 0) model.erase(0, 5);  // "ARRI ALEXA Mini LF" -> "ALEXA Mini LF"
    r.setStr(Field::CameraMake, "ARRI", C::Verified, "arri");
    r.setStr(Field::CameraModel, model, C::Verified, a.cameraModel.empty() ? hex(0x164) : hex(0x2F8));
    if (a.cameraSerial) r.setStr(Field::CameraSerial, std::to_string(a.cameraSerial), C::Verified, hex(0x170));
    r.setStr(Field::CameraFirmware, a.supName, C::Verified, "arri.sup");
    if (a.cameraIndex) r.setStr(Field::CameraIndex, std::string(1, a.cameraIndex), C::Verified, hex(0x178));
    r.setStr(Field::LensModel, a.lensModel, C::Verified, hex(0x398));
    if (a.lensSerial) r.setStr(Field::LensSerial, std::to_string(a.lensSerial), C::Verified, hex(0x380));

    if (a.focusInfinite) {
        r.setNum(Field::FocusDistance, kInfinityFocusM, C::Documented, hex(0x378));
        r.note(Field::FocusDistance, "infinity");
    } else if (a.focusValid) {
        r.setNum(Field::FocusDistance, a.focusMm / 1000.0, C::Verified, hex(0x378));
    } else {
        r.note(Field::FocusDistance, "lens reports no focus (no LDS / /i data or lens-table entry selected)");
    }
    if (!std::isnan(a.tStop)) r.setNum(Field::Iris, a.tStop, C::Verified, hex(0x384));
    if (!std::isnan(a.focalLengthMm) && a.focalLengthMm > 0) r.setNum(Field::FocalLength, a.focalLengthMm, C::Verified, hex(0x37C));
    if (a.entrancePupilUm > 0) r.setNum(Field::EntrancePupil, a.entrancePupilUm / 1000.0, C::Documented, hex(0x3E8));
    if (a.effectiveFocalLengthUm > 0) r.setNum(Field::EffectiveFocal, a.effectiveFocalLengthUm / 1000.0, C::Documented, hex(0x3E4));
    if (a.focalLengthMultiplier > 0) r.setNum(Field::Extender, a.focalLengthMultiplier, C::Documented, hex(0xA94));
    if (a.ldsLagType == 1) r.setNum(Field::LensDataLag, a.ldsLagFrames, C::Documented, hex(0x3DD));

    if (a.exposureIndex) r.setNum(Field::Iso, a.exposureIndex, C::Verified, hex(0x074));
    if (a.shutterAngle > 0) r.setNum(Field::ShutterAngle, a.shutterAngle, C::Verified, hex(0x190));
    if (a.exposureTimeRaw > 0 && a.exposureTimeRaw != 0xFFFFFFFFu)  // microseconds: 20000 at 180 deg / 25 fps
        r.setNum(Field::ExposureTime, a.exposureTimeRaw / 1e6, C::Inferred, hex(0x18C));
    if (a.whiteBalanceK) r.setNum(Field::WhiteBalance, a.whiteBalanceK, C::Verified, hex(0x05C));
    r.setNum(Field::Tint, a.tint, C::Documented, hex(0x060));
    if (a.ndType != 0xFFFF) r.setNum(Field::NdDensity, a.ndDensity, C::Documented, hex(0x38A));

    if (a.sensorFps > 0) r.setNum(Field::CaptureFps, a.sensorFps, C::Verified, hex(0x1A0));
    // White paper: code 0..4 (23.976 / 24 / 25 / 29.97 / 30); Mini LF SUP 7 sends fps x 1000.
    static const double codes[] = {24000.0 / 1001, 24, 25, 30000.0 / 1001, 30};
    if (a.projectFpsCode >= 1000 && a.projectFpsCode < 1000000) r.setNum(Field::ProjectFps, a.projectFpsCode / 1000.0, C::Verified, hex(0x1A4));
    else if (a.projectFpsCode < 5) r.setNum(Field::ProjectFps, codes[a.projectFpsCode], C::Documented, hex(0x1A4));
    if (a.pixelAspect > 0) r.setNum(Field::Squeeze, a.pixelAspect, C::Documented, hex(0x0C4));
    r.setStr(Field::LookName, a.lookFile, C::Verified, hex(0x0CC));

    // ARRI sends no sensor size: derive it from the camera model and the recording area's shape.
    if (a.activeImageW > 0 && a.activeImageH > 0) {
        const SensorGuess g = guessSensorMode(model + " " + a.cameraTypeName, double(a.activeImageW) / a.activeImageH);
        if (g.mode) {
            const std::string origin = std::string("derived: ") + g.mode->name;
            r.setNum(Field::SensorWidth, g.mode->widthMm(), C::Inferred, origin, ValueSource::Derived);
            r.setNum(Field::SensorHeight, g.mode->heightMm(), C::Inferred, origin, ValueSource::Derived);
            r.setNum(Field::RecordingWidth, g.mode->photositesW, C::Inferred, origin, ValueSource::Derived);
            r.setNum(Field::RecordingHeight, g.mode->photositesH, C::Inferred, origin, ValueSource::Derived);
        } else {
            std::string why = "recording mode not in table";
            if (g.candidates.size() > 1) {
                why = "ambiguous:";
                for (const auto* m : g.candidates) why += std::string(" ") + m->name + ",";
                why.pop_back();
            }
            r.note(Field::SensorWidth, why);
            r.note(Field::SensorHeight, why);
        }
    }

    if (a.hasTilt) r.setNum(Field::Tilt, a.tiltDeg, C::Verified, hex(0x45C));
    if (a.hasRoll) r.setNum(Field::Roll, a.rollDeg, C::Verified, hex(0x460));

    r.setStr(Field::CameraTimecode, a.masterTc, C::Verified, hex(0x1A8));
    r.setStr(Field::Clip, a.clip(), C::Verified, a.clipName.empty() ? hex(0x698) : hex(0x6BB));
    r.setStr(Field::Reel, a.reel(), C::Verified, a.reelName.empty() ? hex(0x4F8) : hex(0x6B0));
    r.setStr(Field::Scene, a.scene, C::Documented, hex(0x500));
    r.setStr(Field::TakeName, a.take, C::Documented, hex(0x510));
    if (a.circleTake) r.setNum(Field::CircleTake, 1, C::Documented, hex(0x4F4));
}

// ---------------------------------------------------------------- Sony (RDD 18 + clip KLV)

bool detectSony(const FrameMetadata& md) { return md.rdd18.parsed(); }

const char* sonyModelName(const std::string& code) {
    if (code == "MPC-3610") return "VENICE";
    if (code == "MPC-3626" || code == "MPC-3628") return "VENICE 2";
    if (code == "MPC-2610") return "BURANO";
    return nullptr;
}

// Lens 16-bit values; 0xFFFF = no value (non-electronic lens)
bool lensU16(const Rdd18Frame& f, uint16_t tag, uint16_t& v) {
    const Rdd18Item* i = f.find(K::Lens, tag);
    if (!i || i->value.size() != 2) return false;
    v = uint16_t(i->value[0] << 8 | i->value[1]);
    return v != 0xFFFF;
}

std::string sonyTag(uint8_t set, uint16_t tag) {
    char b[24];
    std::snprintf(b, sizeof b, "sony.%02X.%04X", set, tag);
    return b;
}

void fillSony(const FrameMetadata& md, MetaRecord& r) {
    const auto& f = md.rdd18;
    r.setStr(Field::CameraMake, "Sony", C::Verified, "sony");
    std::istringstream attrs(f.text(K::Camera, 0x8114));  // "MPC-3626 0010025 Version3.00"
    std::string code, serial, fw;
    attrs >> code >> serial >> fw;
    const char* name = sonyModelName(code);
    r.setStr(Field::CameraModel, name ? std::string(name) + " (" + code + ")" : code, C::Verified, sonyTag(0x02, 0x8114));
    r.setStr(Field::CameraSerial, serial, C::Verified, sonyTag(0x02, 0x8114));
    r.setStr(Field::CameraFirmware, fw, C::Verified, sonyTag(0x02, 0x8114));
    const std::string lensId = f.text(K::Lens, 0x8007);
    if (!lensId.empty() && lensId != "Unknown") r.setStr(Field::LensSerial, lensId, C::Verified, sonyTag(0x01, 0x8007));

    uint16_t v;
    if (lensU16(f, 0x8001, v)) r.setNum(Field::FocusDistance, rdd18Metres(v), C::Verified, sonyTag(0x01, 0x8001));
    else if (lensU16(f, 0x8002, v)) r.setNum(Field::FocusDistance, rdd18Metres(v), C::Documented, sonyTag(0x01, 0x8002));
    else r.note(Field::FocusDistance, "lens reports no focus (non-electronic lens)");
    if (lensU16(f, 0x8005, v) && v != 0x8000) r.setNum(Field::FocalLength, rdd18Metres(v) * 1000.0, C::Verified, sonyTag(0x01, 0x8005));
    if (lensU16(f, 0x8004, v) && v != 0x8000) r.setNum(Field::FocalLength35, rdd18Metres(v) * 1000.0, C::Verified, sonyTag(0x01, 0x8004));
    if (lensU16(f, 0x8008, v)) r.setNum(Field::Iris, rdd18FNumber(v), C::Verified, sonyTag(0x01, 0x8008));      // T-stop
    else if (lensU16(f, 0x8000, v)) r.setNum(Field::Iris, rdd18FNumber(v), C::Verified, sonyTag(0x01, 0x8000)); // f-number
    if (lensU16(f, 0x8006, v)) r.setNum(Field::Extender, v / 100.0, C::Inferred, sonyTag(0x01, 0x8006));        // 100 = x1

    auto camU = [&](uint16_t tag, uint64_t& out) {
        const Rdd18Item* i = f.find(K::Camera, tag);
        if (!i || i->value.empty() || i->value.size() > 4) return false;
        out = beValue(i->value);
        return true;
    };
    uint64_t u;
    if (camU(0x810B, u)) r.setNum(Field::Iso, double(u), C::Verified, sonyTag(0x02, 0x810B));
    if (camU(0x8108, u)) r.setNum(Field::ShutterAngle, u / 60.0, C::Verified, sonyTag(0x02, 0x8108));
    if (camU(0x810E, u)) r.setNum(Field::WhiteBalance, double(u), C::Verified, sonyTag(0x02, 0x810E));
    if (const Rdd18Item* i = f.find(K::Camera, 0x810A); i && i->value.size() == 2)
        r.setNum(Field::Gain, int16_t(beValue(i->value)) / 100.0, C::Documented, sonyTag(0x02, 0x810A));
    if (const Rdd18Item* i = f.find(K::Camera, 0x8109); i && i->value.size() == 8) {
        const double n = double(beValue(i->value.data(), 4)), d = double(beValue(i->value.data() + 4, 4));
        if (n > 0 && d > 0) r.setNum(Field::ExposureTime, n / d, C::Verified, sonyTag(0x02, 0x8109));
    }
    if (f.find(K::Camera, 0x8103)) r.note(Field::NdDensity, "ND wheel value (8103) seen, meaning not yet confirmed");
    if (const Rdd18Item* i = f.find(K::Camera, 0x8106); i && i->value.size() == 8) {
        const double n = double(beValue(i->value.data(), 4)), d = double(beValue(i->value.data() + 4, 4));
        if (n > 0 && d > 0) r.setNum(Field::CaptureFps, n / d, C::Verified, sonyTag(0x02, 0x8106));
    }
    const Rdd18Item *w = f.find(K::Camera, 0x8104), *h = f.find(K::Camera, 0x8105);
    if (w && h && w->value.size() == 2 && h->value.size() == 2) {
        r.setNum(Field::SensorWidth, beValue(w->value) / 1000.0, C::Verified, sonyTag(0x02, 0x8104));
        r.setNum(Field::SensorHeight, beValue(h->value) / 1000.0, C::Verified, sonyTag(0x02, 0x8105));
        // Squeeze: aspect of the desqueezed SDI image area (E10F) over the sensor area aspect.
        if (const Rdd18Item* a = f.find(K::UserDefined, 0xE10F); a && a->value.size() == 8) {
            const double aw = double(beValue(a->value.data(), 4)), ah = double(beValue(a->value.data() + 4, 4));
            const double sw = r.num(Field::SensorWidth), sh = r.num(Field::SensorHeight);
            if (aw > 0 && ah > 0 && sw > 0 && sh > 0)
                r.setNum(Field::Squeeze, snapSqueeze((aw / ah) / (sw / sh)), C::Verified, "derived: sony.7F.E10F / sensor aspect",
                         ValueSource::Derived);
        }
    }
    r.setStr(Field::CaptureGamma, f.text(K::Camera, 0x3210), C::Documented, sonyTag(0x02, 0x3210));
    r.setStr(Field::ColourPrimaries, f.text(K::Camera, 0x3219), C::Documented, sonyTag(0x02, 0x3219));
    r.setStr(Field::LookName, f.text(K::UserDefined, 0xE109), C::Verified, sonyTag(0x7F, 0xE109));

    if (const Rdd18Item* t = f.find(K::UserDefined, 0x8414); t && t->value.size() == 4)
        r.setNum(Field::Tilt, rdd18TenthDegree(t->value), C::Verified, sonyTag(0x7F, 0x8414));
    if (const Rdd18Item* t = f.find(K::UserDefined, 0x8415); t && t->value.size() == 4)
        r.setNum(Field::Roll, rdd18TenthDegree(t->value), C::Verified, sonyTag(0x7F, 0x8415));

    r.setStr(Field::Clip, md.sonyClip, C::Verified, "sony.4404.clip");
    if (md.sonyClip.size() >= 4) r.setStr(Field::Reel, md.sonyClip.substr(0, 4), C::Inferred, "derived: clip name prefix", ValueSource::Derived);
    r.setStr(Field::Umid, md.sonyUmid, C::Verified, "sony.4404.umid");
}

// ---------------------------------------------------------------- Blackmagic (SDI camera control)

bool detectBmd(const FrameMetadata& md) { return md.bmd.present; }

long bmdInt(const BmdCommand* c, size_t i, size_t width) {
    if (!c || c->data.size() < (i + 1) * width) return 0;
    uint32_t v = 0;
    for (size_t k = 0; k < width; ++k) v |= uint32_t(c->data[i * width + k]) << (8 * k);  // little-endian
    return width == 2 ? int16_t(v) : width == 4 ? int32_t(v) : long(v);
}

void fillBmd(const FrameMetadata& md, MetaRecord& r) {
    const auto& b = md.bmd;
    r.setStr(Field::CameraMake, "Blackmagic Design", C::Verified, "bmd");
    double v;
    if (b.fNumber(v)) r.setNum(Field::Iris, v, C::Verified, "bmd.0.2");
    if (b.focusNormalised(v)) r.setNum(Field::FocusPosition, v, C::Documented, "bmd.0.0");
    if (b.zoomMm(v)) r.setNum(Field::FocalLength, v, C::Documented, "bmd.0.7");
    if (const BmdCommand* c = b.find(1, 14); c && c->type == 3) r.setNum(Field::Iso, bmdInt(c, 0, 4), C::Verified, "bmd.1.14");
    if (const BmdCommand* c = b.find(1, 11); c && c->type == 3) r.setNum(Field::ShutterAngle, bmdInt(c, 0, 4) / 100.0, C::Verified, "bmd.1.11");
    if (const BmdCommand* c = b.find(1, 2); c && c->type == 2) {
        r.setNum(Field::WhiteBalance, bmdInt(c, 0, 2), C::Verified, "bmd.1.2");
        r.setNum(Field::Tint, bmdInt(c, 1, 2), C::Documented, "bmd.1.2[1]");
    }
    if (const BmdCommand* c = b.find(1, 9); c && c->type == 2 && bmdInt(c, 0, 2) > 0) {
        r.setNum(Field::CaptureFps, bmdInt(c, 0, 2), C::Verified, "bmd.1.9[0]");
        r.setNum(Field::RecordingWidth, bmdInt(c, 2, 2), C::Verified, "bmd.1.9[2]");
        r.setNum(Field::RecordingHeight, bmdInt(c, 3, 2), C::Verified, "bmd.1.9[3]");
    }
    r.setStr(Field::Codec, b.text(10, 0), C::Verified, "bmd.10.0");
    r.setStr(Field::CaptureGamma, b.text(1, 7), C::Documented, "bmd.1.7");
    r.setStr(Field::LookName, b.text(1, 15), C::Documented, "bmd.1.15");
    r.setStr(Field::CameraTimecode, b.text(9, 4), C::Inferred, "bmd.9.4");
    r.setStr(Field::Reel, b.text(12, 0), C::Documented, "bmd.12.0");
    if (b.hasTally && !b.tally.empty()) r.setNum(Field::Tally, b.tally[0], C::Inferred, "bmd.5152");
}

}  // namespace

const std::vector<CameraAdapter>& cameraAdapters() {
    static const std::vector<CameraAdapter> adapters = {
        {"ARRI", CameraKind::Arri, detectArri, fillArri},
        {"Sony RDD 18", CameraKind::Sony, detectSony, fillSony},
        {"Blackmagic", CameraKind::Blackmagic, detectBmd, fillBmd},
    };
    return adapters;
}

MetaRecord buildRecord(const FrameMetadata& md) {
    MetaRecord r;
    for (const auto& a : cameraAdapters())
        if (a.detect(md)) {
            r.kind = a.kind;
            r.adapter = a.name;
            a.fill(md, r);
            break;
        }
    // Camera-independent fields
    if (md.tc.valid) r.setStr(Field::Timecode, md.tc.str(), Confidence::Verified, "sdi.rp188");
    if (md.rec != RecState::Unknown)
        r.setNum(Field::Recording, md.rec == RecState::Recording ? 1 : 0, Confidence::Verified, md.recSource);
    if (!r.has(Field::Clip)) r.setStr(Field::Clip, md.sonyClip, Confidence::Verified, "sony.4404.clip");
    // Composite display fields kept for existing OSC receivers
    if (r.has(Field::CameraMake))
        r.setStr(Field::Camera, r.str(Field::CameraMake) + (r.has(Field::CameraModel) ? " " + r.str(Field::CameraModel) : ""),
                 r.get(Field::CameraMake).confidence, "derived: make + model", ValueSource::Derived);
    if (r.has(Field::LensModel) || r.has(Field::LensSerial))
        r.setStr(Field::Lens, r.has(Field::LensModel) ? r.str(Field::LensModel) : r.str(Field::LensSerial), Confidence::Verified,
                 "derived: lens model or ID", ValueSource::Derived);
    return r;
}
