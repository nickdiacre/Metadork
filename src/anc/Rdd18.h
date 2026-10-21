#pragma once
#include "anc/AncPacket.h"
#include <cstdint>
#include <string>
#include <vector>

// SMPTE RDD 18 acquisition metadata (DID 0x43 / SDID 0x05), as sent by Sony BURANO / VENICE.
//
// Layout confirmed on a BURANO (firmware 3.00) capture:
//   - each 43/05 packet UDW = sequence byte (1..N) + a slice of one byte stream;
//   - the stream (slices concatenated in sequence order) is a run of KLV local sets:
//     16-byte key 06 0E 2B 34 02 53 01 01 0C 02 01 01 <set> 01 00 00, BER length, then
//     items of tag(2) + length(2) + value; sets may continue across packets;
//   - set byte 0x01 = lens unit, 0x02 = camera unit, 0x7F = user defined (Sony private E/8xxx tags).
// Tag names follow RDD 18 / Sony conventions. Camera unit values cross-check against the BURANO's
// settings. Lens distances / focal lengths and F-number decode as documented in Rdd18.cpp (checked
// against an 85mm f/1.4 on BURANO); ring positions (8009-800B) are still shown raw.

enum class Rdd18SetKind { Lens, Camera, UserDefined, Other };
enum class RecStateHint { Unknown, Stopped, Recording };

struct Rdd18Item {
    uint16_t tag = 0;
    std::vector<uint8_t> value;
    const char* name = "";  // "" if unknown
    std::string display;    // best-effort human-readable value
};

struct Rdd18Set {
    Rdd18SetKind kind = Rdd18SetKind::Other;
    uint8_t setByte = 0;     // key byte 13
    uint8_t occurrence = 0;  // nth set with this setByte in the frame (BURANO sends two 7F sets)
    size_t declaredLength = 0;
    bool complete = false;  // value fully received and items consumed it exactly
    std::vector<Rdd18Item> items;
};

struct Rdd18Frame {
    size_t packets = 0;
    std::vector<uint8_t> stream;  // reassembled bytes, for display / logging
    std::vector<Rdd18Set> sets;
    bool parsed() const { return !sets.empty(); }
    const Rdd18Item* find(Rdd18SetKind kind, uint16_t tag) const;
    std::string text(Rdd18SetKind kind, uint16_t tag) const;  // display string or ""
};

Rdd18Frame parseRdd18(const std::vector<AncPacket>& packets);

// Stable identity of an item across frames (set byte, occurrence, tag), for change tracking.
inline uint32_t rdd18ItemKey(const Rdd18Set& s, uint16_t tag) {
    return uint32_t(s.setByte) << 24 | uint32_t(s.occurrence) << 16 | tag;
}

// Lens value encodings: 16-bit distance (4-bit signed power-of-ten exponent + 12-bit mantissa, metres)
// and iris number F = 2^(8(1 - n/65536)).
double rdd18Metres(uint16_t v);
double rdd18FNumber(uint16_t v);
// Camera tilt (7F/8414, + up) and roll (7F/8415, + clockwise seen from behind): signed int32 / 36000 = degrees.
double rdd18TenthDegree(const std::vector<uint8_t>& v);
const char* rdd18SetName(Rdd18SetKind k);

// Sony private KLV in RP 214 (44/04) carrying the clip name, e.g. "A002C002_261006HE".
bool isSonyClipKey(const uint8_t* key16);
std::string sonyClipName(const std::vector<uint8_t>& value);

// VENICE 2: the 44/04 clip packet continues after the clip-name KLV with a 32-byte SMPTE basic UMID
// (06 0A 2B 34 01 01 01 05 01 01 0D 43, 13, instance, 16-byte material number) while RECORDING, and 32 zero
// bytes while stopped (verified over 2 REC cycles, captures/venice2_rec_raw_2026-10-21.txt). BURANO's packet
// has no such area. Returns Recording / Stopped / Unknown; umidHex is set while recording.
RecStateHint sonyRecordingFromClipPacket(const AncPacket& p, std::string& umidHex);
