#pragma once
#include <cstdint>
#include <string>
#include <vector>

// One SMPTE 291 ancillary packet as delivered by the capture card
// (8-bit user data words only; ADF/DID/SDID/DC/CS are stripped).
struct AncPacket {
    uint8_t did = 0;
    uint8_t sdid = 0;
    uint32_t line = 0;    // SDI line number
    uint32_t stream = 0;  // data stream index (0 = Y / link A)
    std::vector<uint8_t> data;
};

inline uint16_t ancKey(uint8_t did, uint8_t sdid) { return (uint16_t(did) << 8) | sdid; }
