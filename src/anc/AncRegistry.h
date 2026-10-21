#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Human-readable name for a DID/SDID pair (SMPTE registered + known vendor packets).
const char* describeAnc(uint8_t did, uint8_t sdid);

std::string toHex(const uint8_t* data, size_t len, size_t maxBytes = SIZE_MAX, char sep = ' ');

// Multi-line "offset: hex  ascii" dump for UI display.
std::string hexDump(const uint8_t* data, size_t len, size_t bytesPerLine = 16);
