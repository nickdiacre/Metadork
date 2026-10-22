#pragma once
// RED clip name, camera letter and record flag carried in the SMPTE 12M-2 timecode user bits (ATC, DID/SDID 60/60, LTC
// line 10 and VITC2 line 571, both identical). Source: RED "HD-SDI HANC Metadata Information" 910-0084 Rev C (RED One /
// DSMC / DSMC2); verified on a V-RAPTOR 8K VV 2026-10-22 (captures/red_raptor_RD-1-rec_2026-10-22_raw.txt).
//
// The 32 user bits alternate between two blocks on successive frames. As a uint32 (DeckLink GetTimecodeUserBits),
// nibble n holds binary group n+1:
//   every frame: bit 3 RS (1 = recording), bit 2 VF (valid / new frame), bit 16 MI (block 0 / 1)
//   block 0:     reel = BG4 BG3 BG2 (BCD); month = BG6; day = BG7 + bit 18 (DN) x16; camera letter = BG8 + bit 19 (CL) x16
//   block 1:     clip = BG4 BG3 BG2 (BCD); wildcard 1 = BG8 + bits 23-22 x16, wildcard 2 = BG7 + bits 21-20 x16
//                (0-9 then A-Z); camera position = bits 19-18 (0 centre, 1 left, 2 right)
// Clip name: <letter><reel>_<position><clip>_<MMDD><wildcard 1><wildcard 2>. RED's note shows a fixed "C" in place of
// <position>; the V-RAPTOR uses its Camera Position letter (Slate menu, e.g. R003_A012), which is NOT in the user bits
// (changing it only regenerates the wildcard), so it is a setting (default A).
// While stopped the clip fields show the NEXT clip (updated ~1.5 s after REC off).
#include <cstdint>
#include <string>

struct RedHancInfo {
    bool valid = false;      // both blocks seen recently and well-formed
    bool recording = false;  // RS flag
    char cameraLetter = 0;
    int reel = 0, clip = 0, month = 0, day = 0;
    std::string wildcard;    // 2 chars
    int position = 0;        // 0 centre, 1 left, 2 right (3D rigs)
    char positionLetter = 'A';  // from settings, see above
    std::string clipName() const;
    std::string reelName() const;  // e.g. R003
};

class RedHancDecoder {
public:
    // Feed one frame's timecode user bits; returns the current state.
    const RedHancInfo& update(uint32_t userBits);
    void reset() { *this = RedHancDecoder(); }
    void setPositionLetter(char c) { m_info.positionLetter = c; }

private:
    RedHancInfo m_info;
    bool m_have0 = false, m_have1 = false;
    int m_sinceBlock0 = 99, m_sinceBlock1 = 99;  // frames since each block was last seen valid
};
