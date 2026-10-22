#include "anc/MetadataProcessor.h"
#include "anc/AncRegistry.h"
#include <cstdio>

std::string TimecodeInfo::str() const {
    if (!valid) return "--:--:--:--";
    char b[16];
    std::snprintf(b, sizeof b, "%02u:%02u:%02u%c%02u", h, m, s, dropFrame ? ';' : ':', f);
    return b;
}

const char* recStateName(RecState s) {
    switch (s) {
        case RecState::Recording: return "RECORDING";
        case RecState::Stopped: return "STOPPED";
        default: return "UNKNOWN";
    }
}

FrameMetadata MetadataProcessor::process(std::vector<AncPacket>&& packets, const TimecodeInfo& tc, bool noSignal, double hostTime) {
    FrameMetadata md;
    md.frameIndex = m_index++;
    md.hostTime = hostTime;
    md.noSignal = noSignal;
    md.tc = tc;
    // A window rather than frame-to-frame: at 50p / 59.94p the TC frame count only moves every other frame.
    if (tc.valid && m_lastTc.valid && !(tc == m_lastTc)) m_lastTcChange = hostTime;
    if (tc.valid && (!m_lastTc.valid || !(tc == m_lastTc))) m_tcValueSince = hostTime;
    md.tcAdvancing = tc.valid && hostTime - m_lastTcChange < 0.25;
    if (tc.valid && !noSignal && hostTime - m_tcValueSince >= 0.5) m_tcRecRun = true;
    m_lastTc = tc;
    md.packets = std::move(packets);

    // RP 214 KLV (ARRI extended metadata + anything else KLV-wrapped)
    for (auto& obj : reassembleRp214(md.packets)) {
        KlvSummary s;
        s.did = obj.did; s.sdid = obj.sdid; s.mid = obj.mid;
        s.keyHex = toHex(obj.key.data(), 16, SIZE_MAX, '.');
        s.declaredLength = obj.declaredLength;
        s.received = obj.value.size();
        s.segments = obj.segmentsSeen;
        s.complete = obj.complete;
        if (isArriMetadataKey(obj.key.data())) {
            s.kind = "ARRI 4K metadata block";
            if (obj.complete && parseArriBlock(obj.value.data(), obj.value.size(), md.arri)) md.hasArri = true;
        } else if (isArriLutKey(obj.key.data())) {
            s.kind = "ARRI tonemap LUT";
        } else if (isSonyClipKey(obj.key.data())) {
            s.kind = "Sony clip name";
            if (obj.complete) md.sonyClip = sonyClipName(obj.value);
        } else {
            s.kind = "Unknown KLV";
        }
        md.klv.push_back(std::move(s));
    }

    for (const auto& p : md.packets) {
        // ARRI HANC record flag: DC=11, word 11 bit 1. Only visible if the card delivers HANC.
        if (p.did == 0x52 && p.sdid == 0x4D && p.data.size() >= 11) {
            md.rec = (p.data[10] & 0x02) ? RecState::Recording : RecState::Stopped;
            md.recSource = "ARRI SDI rec flag (0x52/0x4D)";
        }
    }
    md.bmd = parseBmd(md.packets);
    int transport = 0;
    if (md.rec == RecState::Unknown && md.bmd.transportMode(transport)) {
        md.rec = transport == 2 ? RecState::Recording : RecState::Stopped;
        md.recSource = "Blackmagic transport mode (10.1)";
    }
    // Sony clip name events. Name = <reel+clip number>_<date + 2-char suffix>, e.g. A002C003_261006WH.
    if (!md.sonyClip.empty()) {
        if (!m_lastClip.empty() && md.sonyClip != m_lastClip) {
            const auto stem = [](const std::string& s) { return s.substr(0, s.find('_')); };
            m_clipRec = stem(md.sonyClip) == stem(m_lastClip) ? RecState::Recording : RecState::Stopped;
        }
        m_lastClip = md.sonyClip;
    }
    for (const auto& p : md.packets) {
        const RecStateHint h = sonyRecordingFromClipPacket(p, md.sonyUmid);
        if (h != RecStateHint::Unknown && md.rec == RecState::Unknown) {
            md.rec = h == RecStateHint::Recording ? RecState::Recording : RecState::Stopped;
            md.recSource = "Sony UMID in clip packet";
            break;
        }
    }
    if (md.rec == RecState::Unknown && m_recFromTc && m_tcRecRun && tc.valid && !noSignal) {
        md.rec = md.tcAdvancing ? RecState::Recording : RecState::Stopped;
        md.recSource = "Timecode running (rec-run TC detected)";
    }
    if (md.rec == RecState::Unknown && m_clipRec != RecState::Unknown) {
        md.rec = m_clipRec;
        md.recSource = "Sony clip name change";
    }
    md.rdd18 = parseRdd18(md.packets);
    md.hasRdd18 = md.rdd18.packets > 0;

    const std::string clip = md.hasArri ? md.arri.clip() : md.sonyClip;
    md.recordingName = m_take.update(md.rec == RecState::Recording ? 1 : md.rec == RecState::Stopped ? 0 : -1, clip, hostTime);
    return md;
}
