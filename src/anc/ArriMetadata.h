#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

// Decoded fields from the ARRI 4 KB SDI metadata block (ArriFileV3 header layout),
// per "ARRI SDI Metadata" white paper (July 2023, incl. ALEXA 35 SUP 1.2).
// Offsets are from that document; multi-byte values are read little-endian.
struct ArriMetadata {
    // Camera
    uint32_t cameraTypeId = 0;
    std::string cameraTypeName;
    std::string cameraModel;
    uint32_t cameraSerial = 0;
    char cameraIndex = 0;
    std::string supName;

    // Image / exposure
    uint32_t exposureIndex = 0;   // ASA
    uint32_t whiteBalanceK = 0;
    double tint = 0;              // IEEE float at 0x060 (Mini LF: 0xC0400000 = -3.0)
    double shutterAngle = 0;      // degrees
    double sensorFps = 0;
    uint32_t exposureTimeRaw = 0; // microseconds (Mini LF: 20000 at 180 deg / 25 fps)
    uint32_t projectFpsCode = 0;
    std::string masterTc;         // HH:MM:SS:FF
    bool mirrorShutterRunning = false;
    bool variFrameDuplicate = false;
    std::string lookFile;
    int imageW = 0, imageH = 0;              // IDI02/03 (0x14/0x18): SDI output image, px
    int activeImageW = 0, activeImageH = 0;  // IDI06 (0x2C/0x30): recording area mapped into the SDI image, px. Its aspect
                                             // is the mode's when letterboxed (Mini LF Open Gate 1721x1198, ALEXA 265
                                             // 1744x824); when it fills the SDI frame it says nothing (LF 2.39:1 -> 3840x2160)
    double pixelAspect = 0;       // ICI18 at 0x0C4, float: 1.0 spherical, 1.25..2.0 anamorphic desqueeze (0 = n/a)

    // Lens (FIZ)
    uint32_t lensDistanceUnit = 0;  // 0 = inch, 1 = metric: the camera's display unit; focus is always mm (see .cpp)
    int32_t focusRaw = 0;
    bool focusValid = false;
    bool focusInfinite = false;
    double focusMm = NAN;
    double focalLengthMm = NAN;
    int32_t irisRaw = 0;
    double tStop = NAN;
    const char* irisState = "";
    uint16_t ndType = 0;
    double ndDensity = 0;
    std::string lensModel;
    uint32_t lensSerial = 0;
    uint16_t rawFocusLds = 0, rawFocusMotor = 0;
    uint16_t rawFocalLds = 0, rawFocalMotor = 0;
    uint16_t rawIrisLds = 0, rawIrisMotor = 0;
    uint8_t ldsLagType = 0, ldsLagFrames = 0;
    uint8_t rawEncoderLagType = 0, rawEncoderLagFrames = 0;
    int32_t effectiveFocalLengthUm = 0;  // ALEXA 35 SUP 1.2+
    int32_t entrancePupilUm = 0;         // ALEXA 35 SUP 1.2+
    std::string lensConverterModel;
    double focalLengthMultiplier = 0;

    // Orientation (VFX08 / VFX09). Mini LF sends a signed 16-bit value in the low half of the 32-bit field
    // (roll -1.8 deg arrived as 0x0000FFEE); 0xFFFFFFFF = not available (as Camera Pan always is).
    // Tenths of a degree; roll verified on a Mini LF with a phone inclinometer (20 deg clockwise = 205), + = clockwise
    // seen from behind, same convention as Sony. Tilt shares the format (+ = up).
    bool hasTilt = false, hasRoll = false;
    double tiltDeg = 0, rollDeg = 0;

    // Clip
    std::string reelName;       // ALEXA 35: A_0001_1234
    std::string clipName;       // ALEXA 35: A_0001C001_YYMMDD_HHMMSS_a1234
    std::string reelNameOld;    // 8 chars, older cameras
    std::string clipNameOld;    // A001C001_YYMMDD_R123
    std::string scene, take;
    bool circleTake = false;

    // Best reel/clip regardless of camera generation
    const std::string& reel() const { return reelName.empty() ? reelNameOld : reelName; }
    const std::string& clip() const { return clipName.empty() ? clipNameOld : clipName; }
};

bool isArriMetadataKey(const uint8_t* key16);
bool isArriLutKey(const uint8_t* key16);

// value must be the 4096-byte block (the KLV "Value").
bool parseArriBlock(const uint8_t* value, size_t size, ArriMetadata& out);
