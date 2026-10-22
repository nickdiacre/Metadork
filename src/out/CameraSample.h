#pragma once
#include "anc/MetadataProcessor.h"
#include "meta/MetaRecord.h"
#include <string>
#include <vector>

// Typed convenience view of the master MetaRecord (meta/), used by FreeD, OpenTrackIO and the UI.
// The record is the source of truth; these fields are generated from it by sampleFrom() / refresh().
struct CameraSample {
    MetaRecord record;

    CameraKind kind = CameraKind::None;
    const char* source = "";  // adapter name, e.g. "ARRI", "Sony RDD 18", "Blackmagic"; "" if none

    // Camera identity
    std::string cameraMake, cameraModel, cameraSerial, cameraFirmware;
    std::string lensModel, lensSerial;

    // Lens
    bool hasFocus = false;
    bool focusInfinite = false;
    double focusM = 0;  // metres; image plane (Sony) / lens mark (ARRI)

    bool hasFocal = false;
    double focalMm = 0;

    bool hasIris = false;
    double iris = 0;      // T-stop (ARRI, Sony 8008) or F-number (Sony 8000, Blackmagic)
    char irisKind = 0;    // 'T' or 'f'

    bool hasFocusNorm = false;  // lens focus position 0 (near) .. 1 (far), when no distance is available
    double focusNorm = 0;

    // Orientation in degrees: tilt (+ up) and roll (+ clockwise seen from behind); nothing supplies pan.
    bool hasPan = false, hasTilt = false, hasRoll = false;
    double pan = 0, tilt = 0, roll = 0;

    // Camera settings
    bool hasSensor = false;  // effective (active) sensor area for the current recording mode, mm
    double sensorWidthMm = 0, sensorHeightMm = 0;
    std::string sensorSource;  // "camera", "derived: <mode>", "manual"; or why it is missing (e.g. "ambiguous: ...")
    bool hasIso = false;
    int iso = 0;
    bool hasShutter = false;
    double shutterAngle = 0;  // degrees
    bool hasWhiteBalance = false;
    int whiteBalanceK = 0;
    bool hasSqueeze = false;
    double squeeze = 1.0;  // anamorphic desqueeze / pixel aspect ratio (1.0 = spherical)
    bool hasCaptureRate = false;
    int captureRateNum = 0, captureRateDen = 1;  // sensor frame rate as a rational
    double captureFps() const { return captureRateDen ? double(captureRateNum) / captureRateDen : 0; }

    // Timing / take
    TimecodeInfo tc;
    std::string reel;
    std::string clip;
    std::string umid;
    RecState rec = RecState::Unknown;
    std::string recSource;

    bool anyLens() const { return hasFocus || hasFocusNorm || hasFocal || hasIris; }

    // Regenerates the typed fields from `record` (after the record was changed, e.g. by a manual override).
    void refresh();
};

CameraSample sampleFrom(const FrameMetadata& md);

// ------------------------------------------------------------ variable catalogue
// Every registry field, in display order, with its current value. OSC sends each as <prefix>/<id>; the UI greys out
// the ones the current camera does not supply.
enum class VarType { Float, Int, String };

struct VarValue {
    const char* id;     // also the OSC address suffix
    const char* label;
    const char* unit;   // "" if none
    VarType type;
    bool available = false;
    double num = 0;
    std::string str;
    FieldGroup group = FieldGroup::Identity;
    const FieldValue* value = nullptr;  // provenance (into the sample's record)
    std::string display() const;
};

enum class FocusUnit { Metres, Millimetres, Feet };
const char* focusUnitName(FocusUnit u);

// infinityMetres: focus value used when the lens reports infinity.
std::vector<VarValue> catalogue(const CameraSample& s, FocusUnit focusUnit = FocusUnit::Metres, double infinityMetres = 1000.0);
