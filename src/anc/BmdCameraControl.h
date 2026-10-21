#pragma once
#include "anc/AncPacket.h"
#include <cstdint>
#include <string>
#include <vector>

// Blackmagic SDI Camera Control Protocol (DID 0x51 / SDID 0x53) and tally (0x51 / 0x52).
//
// URSA cameras report their own settings on SDI out using the same command format ATEM uses to control them.
// Each command: destination(1) length(1) command id(1) reserved(1), then `length` bytes of
// category(1) parameter(1) data type(1) operation(1) data(...), padded to a 4-byte boundary.
// Data types: 0 void/bool, 1 int8, 2 int16, 3 int32, 4 int64, 5 string, 128 fixed16 (signed 5.11); little-endian.
// Operation 2 is what the camera uses when reporting status (0 = assign, 1 = offset in the control direction).
// Category 9 is not in Blackmagic's public documentation; 9.4 is evidently the timecode (BCD, matches RP 188).

struct BmdCommand {
    uint8_t category = 0, parameter = 0, type = 0, operation = 0;
    std::vector<uint8_t> data;
    const char* name = "";  // "" if unknown
    std::string display;
};

struct BmdFrame {
    bool present = false;   // any 51/53 packet this frame
    bool hasTally = false;
    std::vector<uint8_t> tally;  // 51/52 payload
    std::vector<BmdCommand> commands;

    const BmdCommand* find(uint8_t category, uint8_t parameter) const;
    std::string text(uint8_t category, uint8_t parameter) const;  // display string or ""

    // Typed accessors; return false when the camera did not send the value.
    bool fNumber(double& f) const;           // 0.2 aperture (f-stop)
    bool focusNormalised(double& v) const;   // 0.0, 0 = near .. 1 = far (not a distance)
    bool zoomMm(double& mm) const;           // 0.7 absolute zoom (focal length)
    bool transportMode(int& mode) const;     // 10.1 [0]: 0 preview, 1 play, 2 record
};

BmdFrame parseBmd(const std::vector<AncPacket>& packets);
std::vector<BmdCommand> parseBmdCommands(const std::vector<uint8_t>& udw);
const char* bmdCategoryName(uint8_t category);
