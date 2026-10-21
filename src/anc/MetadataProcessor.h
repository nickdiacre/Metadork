#pragma once
#include "anc/AncPacket.h"
#include "anc/ArriMetadata.h"
#include "anc/BmdCameraControl.h"
#include "anc/Rdd18.h"
#include "anc/Rp214.h"
#include <atomic>
#include <string>
#include <vector>

struct TimecodeInfo {
    bool valid = false;
    uint8_t h = 0, m = 0, s = 0, f = 0;
    bool dropFrame = false;
    uint32_t userBits = 0;
    std::string str() const;
    bool operator==(const TimecodeInfo& o) const { return valid == o.valid && h == o.h && m == o.m && s == o.s && f == o.f; }
};

enum class RecState { Unknown, Stopped, Recording };
const char* recStateName(RecState s);

struct KlvSummary {
    uint8_t did = 0, sdid = 0, mid = 0;
    std::string keyHex;
    std::string kind;
    size_t declaredLength = 0;
    size_t received = 0;
    uint16_t segments = 0;
    bool complete = false;
};

struct FrameMetadata {
    uint64_t frameIndex = 0;
    double hostTime = 0;  // seconds, steady clock
    bool noSignal = false;

    TimecodeInfo tc;
    bool tcAdvancing = false;  // TC changed within the last 0.25 s (in rec-run TC mode: recording)

    std::vector<AncPacket> packets;
    std::vector<KlvSummary> klv;

    bool hasArri = false;
    ArriMetadata arri;

    RecState rec = RecState::Unknown;
    std::string recSource;

    bool hasRdd18 = false;  // any 43/05 packets this frame
    Rdd18Frame rdd18;
    std::string sonyClip;   // from Sony private KLV (44/04)
    std::string sonyUmid;   // SMPTE UMID of the take being recorded (VENICE 2), hex; empty when stopped

    BmdFrame bmd;           // Blackmagic camera control (51/53) and tally (51/52)
};

// Stateless per-frame decode plus a little cross-frame state (frame count, TC advance).
class MetadataProcessor {
public:
    FrameMetadata process(std::vector<AncPacket>&& packets, const TimecodeInfo& tc, bool noSignal, double hostTime);

    // Record state when the camera sends no record flag (Sony), in priority order:
    //  0. VENICE 2: a UMID present (recording) or zeroed (stopped) after the clip name in the 44/04 packet;
    //  1. running timecode, but only once TC has been seen holding still for 0.5 s, which proves rec-run TC
    //     (in free-run TC it never holds, so it is never used; VENICE 2 in free run read as "recording" before this);
    //  2. Sony clip name events: suffix changes with the same clip number = REC started, clip number changes =
    //     REC stopped (seen on BURANO);
    //  3. otherwise Unknown.
    void setRecFromTimecode(bool on) { m_recFromTc = on; }

private:
    uint64_t m_index = 0;
    TimecodeInfo m_lastTc;
    double m_lastTcChange = -1e9;
    double m_tcValueSince = 0;  // when the current TC value first appeared
    bool m_tcRecRun = false;  // TC seen holding: camera is in rec-run
    std::string m_lastClip;
    RecState m_clipRec = RecState::Unknown;
    std::atomic<bool> m_recFromTc{true};
};
