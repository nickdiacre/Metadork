#pragma once
#include "anc/AncPacket.h"
#include <array>

// A KLV object reassembled from SMPTE RP 214 segmented ANC packets
// (DID 0x44 / SDID 0x04 in VANC, 0x44 / 0x14 in HANC).
// Each packet UDW = MID(1) PSC(2) payload(...); packets with the same MID
// are concatenated in PSC order starting at 1.
struct KlvObject {
    uint8_t did = 0, sdid = 0, mid = 0;
    std::array<uint8_t, 16> key{};
    size_t declaredLength = 0;
    std::vector<uint8_t> value;
    uint16_t segmentsSeen = 0;
    bool complete = false;  // all segments present and value fully received
};

std::vector<KlvObject> reassembleRp214(const std::vector<AncPacket>& packets);

// SMPTE 336 BER length decode. Returns bytes consumed (0 on error).
size_t decodeBerLength(const uint8_t* p, size_t avail, size_t& length);
