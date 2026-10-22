#include "anc/RedHanc.h"
#include <cstdio>

namespace {
unsigned nib(uint32_t ub, int group) { return (ub >> (4 * (group - 1))) & 0xF; }  // binary group 1..8
bool bit(uint32_t ub, int b) { return (ub >> b) & 1; }
bool bcd3(uint32_t ub, int& out) {
    const unsigned h = nib(ub, 4), t = nib(ub, 3), o = nib(ub, 2);
    if (h > 9 || t > 9 || o > 9) return false;
    out = int(h * 100 + t * 10 + o);
    return true;
}
char wildChar(unsigned v) { return v < 10 ? char('0' + v) : char('A' + v - 10); }
}  // namespace

std::string RedHancInfo::reelName() const {
    char b[8];
    std::snprintf(b, sizeof b, "%c%03d", cameraLetter, reel);
    return b;
}

std::string RedHancInfo::clipName() const {
    char b[32];
    std::snprintf(b, sizeof b, "%c%03d_%c%03d_%02d%02d%s", cameraLetter, reel, positionLetter, clip, month, day, wildcard.c_str());
    return b;
}

const RedHancInfo& RedHancDecoder::update(uint32_t ub) {
    ++m_sinceBlock0;
    ++m_sinceBlock1;
    if (!bit(ub, 16)) {  // block 0: reel, date, camera letter
        int reel;
        const unsigned month = nib(ub, 6), day = nib(ub, 7) + (bit(ub, 18) ? 16 : 0),
                       letter = nib(ub, 8) + (bit(ub, 19) ? 16 : 0);
        if (bcd3(ub, reel) && month >= 1 && month <= 12 && day >= 1 && day <= 31 && letter <= 25) {
            m_info.reel = reel;
            m_info.month = int(month);
            m_info.day = int(day);
            m_info.cameraLetter = char('A' + letter);
            m_info.recording = bit(ub, 3);
            m_have0 = true;
            m_sinceBlock0 = 0;
        }
    } else {  // block 1: clip number, wildcard, camera position
        int clip;
        const unsigned w1 = nib(ub, 8) + ((ub >> 22) & 3) * 16, w2 = nib(ub, 7) + ((ub >> 20) & 3) * 16;
        if (bcd3(ub, clip) && w1 <= 35 && w2 <= 35) {
            m_info.clip = clip;
            m_info.wildcard = {wildChar(w1), wildChar(w2)};
            m_info.position = int((ub >> 18) & 3);
            m_info.recording = bit(ub, 3);
            m_have1 = true;
            m_sinceBlock1 = 0;
        }
    }
    // Both blocks must keep arriving (they alternate every frame); a few frames of slack for dropped frames.
    m_info.valid = m_have0 && m_have1 && m_sinceBlock0 <= 4 && m_sinceBlock1 <= 4;
    return m_info;
}
