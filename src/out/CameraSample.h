#pragma once
#include "anc/MetadataProcessor.h"
#include <string>
#include <vector>

// Which decoder the current feed matched.
enum class CameraKind { None, Arri, Sony, Blackmagic };

// Camera-agnostic per-frame values, pulled from whichever decoder has data. Feeds the Summary tab and all outputs.
struct CameraSample {
    CameraKind kind = CameraKind::None;
    const char* source = "";  // "ARRI", "Sony RDD 18", "Blackmagic" or ""

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
    double iris = 0;      // T-stop (ARRI) or F-number (Sony)
    char irisKind = 0;    // 'T' or 'f'

    bool hasFocusNorm = false;  // lens focus position 0 (near) .. 1 (far), when no distance is available
    double focusNorm = 0;

    // Orientation in degrees, sent when present. Sony supplies tilt (+ up) and roll (+ clockwise seen from behind);
    // nothing supplies pan.
    bool hasPan = false, hasTilt = false, hasRoll = false;
    double pan = 0, tilt = 0, roll = 0;

    // Camera settings
    bool hasSensor = false;  // effective (active) sensor area for the current imager mode, mm
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
    std::string reel;  // ARRI only
    std::string clip;
    std::string umid;  // Sony VENICE 2 while recording
    RecState rec = RecState::Unknown;
    std::string recSource;

    bool anyLens() const { return hasFocus || hasFocusNorm || hasFocal || hasIris; }
};

CameraSample sampleFrom(const FrameMetadata& md);
const char* cameraKindName(CameraKind k);

// ------------------------------------------------------------ variable catalogue
// Every value the app can output, in display order. OSC sends each as <prefix>/<id>; the UI greys out the ones the
// current camera does not supply.
enum class VarType { Float, Int, String };

struct VarValue {
    const char* id;     // also the OSC address suffix
    const char* label;
    const char* unit;   // "" if none
    VarType type;
    bool available = false;
    double num = 0;
    std::string str;
    std::string display() const;
};

enum class FocusUnit { Metres, Millimetres, Feet };
const char* focusUnitName(FocusUnit u);

// infinityMetres: focus value used when the lens reports infinity.
std::vector<VarValue> catalogue(const CameraSample& s, FocusUnit focusUnit = FocusUnit::Metres, double infinityMetres = 1000.0);
