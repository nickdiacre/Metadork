// sdimeta_probe: headless check of what ancillary data actually reaches the DeckLink API.
//
// Captures for a few seconds and reports, independently of the app's parsers:
//   1. device capabilities and signal lock status,
//   2. packets returned by IDeckLinkVideoFrameAncillaryPackets (the path the viewer uses),
//   3. every timecode source queried individually,
//   4. the raw VANC lines (legacy IDeckLinkVideoFrameAncillary), scanned for ANC data flags
//      (000 3FF 3FF). If the raw lines are blank, the data is not in the signal the device sees.
//
// Usage: sdimeta_probe [seconds=5] [device index=0] [--dump] [--watch]
//   --dump   also print the hex payload of every ANC packet in the last frame that had any
//   --hanc 524D,6060,...  HANC DID/SDID pairs to capture (up to 4; default 524D = ARRI record flag)
//   --list   list every DeckLink device / port: sub-device, duplex, profiles, connectors, HANC support; then exit
//   --raw    print every ANC packet of every frame as hex, with time and TC (for offline analysis)
//   --watch  print every decoded metadata change (RDD 18 items, clip, TC running/stopped) as it happens,
//            e.g. while toggling REC or racking a lens

#include "anc/AncRegistry.h"
#include "anc/MetadataProcessor.h"
#include "out/CameraSample.h"
#include "capture/DeckLinkPlatform.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct LineStats {
    uint64_t framesCaptured = 0;  // frames where the driver returned a buffer for this line
    uint64_t framesZero = 0;      // every sample 0: driver likely never filled the buffer
    uint64_t framesBlack = 0;     // Y=0x040 / C=0x200: genuine empty blanking from the wire
    uint64_t framesWithAdf = 0;   // at least one ANC data flag found
    std::set<uint16_t> didSdid;   // DID<<8 | SDID seen via the ADF scan
};

bool gDump = false;
bool gWatch = false;
bool gRaw = false;
std::vector<uint16_t> gHanc = {0x524D};  // HANC DID/SDID filters (DeckLink allows 4)

struct TcStats {
    uint64_t valid = 0;
    std::string last;
};

struct Stats {
    std::mutex mtx;
    uint64_t frames = 0, noSignalFrames = 0, framesWithPackets = 0;
    std::string mode;
    std::string vancFormat;
    bool legacyAncAvailable = false;
    std::map<uint16_t, uint64_t> packets;  // DID<<8 | SDID -> count
    std::map<uint16_t, std::set<uint32_t>> packetLines;
    std::vector<std::string> lastDump;  // per packet, filled only with --dump
    std::set<uint16_t> hancKeys;        // packet types that arrived in HANC
    std::map<std::string, TcStats> tc;
    std::map<uint32_t, LineStats> lines;
};

const std::pair<BMDTimecodeFormat, const char*> kTcFormats[] = {
    {bmdTimecodeRP188VITC1, "RP188 VITC1"},
    {bmdTimecodeRP188VITC2, "RP188 VITC2"},
    {bmdTimecodeRP188LTC, "RP188 LTC"},
    {bmdTimecodeRP188HighFrameRate, "RP188 HFR"},
    {bmdTimecodeRP188Any, "RP188 Any"},
    {bmdTimecodeVITC, "VITC (SD)"},
    {bmdTimecodeVITCField2, "VITC field 2 (SD)"},
};

// Unpacks one v210 line into interleaved 10-bit samples (Cb Y Cr Y ...).
std::vector<uint16_t> unpackV210(const void* buf, long width) {
    const long groups = (width + 5) / 6;  // 6 pixels per 16-byte group
    const auto* w = static_cast<const uint32_t*>(buf);
    std::vector<uint16_t> s;
    s.reserve(size_t(groups) * 12);
    for (long i = 0; i < groups * 4; ++i) {
        s.push_back(uint16_t(w[i] & 0x3FF));
        s.push_back(uint16_t((w[i] >> 10) & 0x3FF));
        s.push_back(uint16_t((w[i] >> 20) & 0x3FF));
    }
    s.resize(size_t(width) * 2);
    return s;
}

// Scans one sample stream (luma or chroma) for ANC data flags; returns DID<<8|SDID for each hit.
void scanAdf(const std::vector<uint16_t>& s, size_t start, std::set<uint16_t>& out, bool& found) {
    for (size_t i = start; i + 4 * 2 < s.size(); i += 2) {
        if (s[i] == 0x000 && s[i + 2] == 0x3FF && s[i + 4] == 0x3FF) {
            found = true;
            out.insert(uint16_t(((s[i + 6] & 0xFF) << 8) | (s[i + 8] & 0xFF)));
        }
    }
}

bool isZero(const std::vector<uint16_t>& s) {
    for (uint16_t v : s)
        if (v) return false;
    return true;
}

bool isBlack(const std::vector<uint16_t>& s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (i % 2 ? s[i] != 0x040 : s[i] != 0x200) return false;  // odd = Y, even = C
    return true;
}

std::string hexPacket(IDeckLinkAncillaryPacket* p) {
    const void* data = nullptr;
    uint32_t size = 0;
    char head[64];
    std::snprintf(head, sizeof head, "DID %02X SDID %02X line %u, %u bytes", p->GetDID(), p->GetSDID(),
                  p->GetLineNumber(), 0u);
    if (p->GetBytes(bmdAncillaryPacketFormatUInt8, &data, &size) != S_OK || !data) return std::string(head) + " (no data)";
    std::snprintf(head, sizeof head, "DID %02X SDID %02X line %u, %u bytes", p->GetDID(), p->GetSDID(),
                  p->GetLineNumber(), size);
    std::string out = head;
    const auto* b = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < size; ++i) {
        char h[8];
        std::snprintf(h, sizeof h, "%s%02X", i % 32 ? " " : "\n    ", b[i]);
        out += h;
    }
    return out;
}

std::string fourcc(uint32_t v) {
    char c[5] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v), 0};
    return c;
}

class Probe : public IDeckLinkInputCallback {
public:
    Probe(IDeckLinkInput* in, Stats& st) : m_in(in), m_st(st) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID* ppv) override { *ppv = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode* mode,
                                                      BMDDetectedVideoInputFormatFlags flags) override {
        if (!mode) return S_OK;
        const bool rgb = (flags & bmdDetectedVideoInputRGB444) != 0;
        if (events & (bmdVideoInputDisplayModeChanged | bmdVideoInputColorspaceChanged)) {
            m_in->PauseStreams();
            m_in->EnableVideoInput(mode->GetDisplayMode(), rgb ? bmdFormat10BitRGB : bmdFormat10BitYUV,
                                   bmdVideoInputEnableFormatDetection);
            m_in->FlushStreams();
            m_in->StartStreams();
        }
        dl_string name = nullptr;
        std::string n = mode->GetName(&name) == S_OK ? dlp::toStd(name) : "?";
        std::lock_guard<std::mutex> l(m_st.mtx);
        m_st.mode = n + (rgb ? " (RGB 4:4:4)" : " (YUV 4:2:2)");
        m_width = mode->GetWidth();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* f, IDeckLinkAudioInputPacket*) override {
        if (!f) return S_OK;
        std::lock_guard<std::mutex> l(m_st.mtx);
        ++m_st.frames;
        if (f->GetFlags() & bmdFrameHasNoInputSource) {
            ++m_st.noSignalFrames;
            if (gWatch && !m_noSignal) { std::printf("  NO SIGNAL (frame flagged)\n"); std::fflush(stdout); }
            m_noSignal = true;
            return S_OK;
        }
        if (gWatch && m_noSignal) { std::printf("  signal back\n"); std::fflush(stdout); }
        m_noSignal = false;

        // 1. Packet API
        IDeckLinkVideoFrameAncillaryPackets* anc = nullptr;
        if (f->QueryInterface(IID_IDeckLinkVideoFrameAncillaryPackets, (void**)&anc) == S_OK && anc) {
            IDeckLinkAncillaryPacketIterator* it = nullptr;
            bool any = false;
            std::vector<std::string> dump;
            m_frame.clear();
            if (anc->GetPacketIterator(&it) == S_OK && it) {
                IDeckLinkAncillaryPacket* p = nullptr;
                while (it->Next(&p) == S_OK && p) {
                    any = true;
                    const uint16_t key = uint16_t((p->GetDID() << 8) | p->GetSDID());
                    ++m_st.packets[key];
                    m_st.packetLines[key].insert(p->GetLineNumber());
                    if (p->GetDataSpace() == bmdAncillaryDataSpaceHANC) m_st.hancKeys.insert(key);
                    if (gDump) dump.push_back(hexPacket(p));
                    if (gWatch || gRaw) m_frame.push_back(toAncPacket(p));
                    p->Release();
                    p = nullptr;
                }
                it->Release();
            }
            if (any) {
                ++m_st.framesWithPackets;
                if (gDump) m_st.lastDump = std::move(dump);
            }
            anc->Release();
        }

        // 2. Every timecode source
        for (const auto& [fmt, label] : kTcFormats) {
            IDeckLinkTimecode* tc = nullptr;
            if (f->GetTimecode(fmt, &tc) == S_OK && tc) {
                uint8_t h, m, s, fr;
                if (tc->GetComponents(&h, &m, &s, &fr) == S_OK) {
                    char b[32];
                    std::snprintf(b, sizeof b, "%02u:%02u:%02u:%02u", h, m, s, fr);
                    auto& t = m_st.tc[label];
                    ++t.valid;
                    t.last = b;
                    if (gRaw) {
                        BMDTimecodeUserBits ub = 0;
                        tc->GetTimecodeUserBits(&ub);
                        char line[96];
                        std::snprintf(line, sizeof line, "%s %s ub=%08X flags=%X", label, b, unsigned(ub), unsigned(tc->GetFlags()));
                        m_tcRaw.push_back(line);
                    }
                    if (fmt == bmdTimecodeRP188Any) {
                        m_tc.valid = true;
                        m_tc.h = h; m_tc.m = m; m_tc.s = s; m_tc.f = fr;
                        m_tc.recTrigger = (tc->GetFlags() & bmdTimecodeRecordingTriggered) != 0;
                        BMDTimecodeUserBits ubAny = 0;
                        if (tc->GetTimecodeUserBits(&ubAny) == S_OK) m_tc.userBits = ubAny;
                    }
                }
                tc->Release();
            }
        }

        if (gRaw) printRaw();
        if (gWatch) watch();

        // 3. Raw VANC lines
        IDeckLinkVideoFrameAncillary* raw = nullptr;
        if (f->GetAncillaryData(&raw) == S_OK && raw) {
            m_st.legacyAncAvailable = true;
            const BMDPixelFormat pf = raw->GetPixelFormat();
            m_st.vancFormat = fourcc(pf);
            // UHD/DCI VANC buffers are HD width; see the SDK header note.
            const long width = m_width > 2048 ? (m_width >= 4096 ? 2048 : 1920) : m_width;
            if (pf == bmdFormat10BitYUV && width > 0) {
                for (uint32_t line : candidateLines()) {
                    void* buf = nullptr;
                    if (raw->GetBufferForVerticalBlankingLine(line, &buf) != S_OK || !buf) continue;
                    auto& ls = m_st.lines[line];
                    ++ls.framesCaptured;
                    const auto s = unpackV210(buf, width);
                    if (isZero(s)) { ++ls.framesZero; continue; }
                    if (isBlack(s)) { ++ls.framesBlack; continue; }
                    bool found = false;
                    scanAdf(s, 1, ls.didSdid, found);  // luma
                    scanAdf(s, 0, ls.didSdid, found);  // chroma
                    if (found) ++ls.framesWithAdf;
                }
            }
            raw->Release();
        }
        return S_OK;
    }

private:
    static AncPacket toAncPacket(IDeckLinkAncillaryPacket* p) {
        AncPacket a;
        a.did = p->GetDID();
        a.sdid = p->GetSDID();
        a.line = p->GetLineNumber();
        const void* data = nullptr;
        uint32_t size = 0;
        if (p->GetBytes(bmdAncillaryPacketFormatUInt8, &data, &size) == S_OK && data)
            a.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        return a;
    }

    void printRaw() {
        using Clock = std::chrono::steady_clock;
        if (m_rawStart == Clock::time_point{}) m_rawStart = Clock::now();
        const double t = std::chrono::duration<double>(Clock::now() - m_rawStart).count();
        for (const auto& a : m_frame)
            std::printf("%.3f %s %02X %02X %u %s\n", t, m_tc.str().c_str(), a.did, a.sdid, a.line,
                        toHex(a.data.data(), a.data.size(), SIZE_MAX, 0).c_str());
        for (const auto& l : m_tcRaw) std::printf("# %.3f TC %s\n", t, l.c_str());  // '#': skipped by packet parsers
        m_tcRaw.clear();
        if (!gWatch) { m_frame.clear(); m_tc = {}; }
        std::fflush(stdout);
    }

    // Prints what changed since the previous frame.
    void watch() {
        using Clock = std::chrono::steady_clock;
        if (m_start == Clock::time_point{}) m_start = Clock::now();
        const double t = std::chrono::duration<double>(Clock::now() - m_start).count();
        const TimecodeInfo tc = m_tc;
        m_tc = {};
        FrameMetadata md = m_proc.process(std::move(m_frame), tc, false, t);
        m_frame.clear();
        const std::string when = [&] { char b[48]; std::snprintf(b, sizeof b, "%7.2fs  %s", t, tc.str().c_str()); return std::string(b); }();

        if (md.tcAdvancing != m_tcRunning && m_frames > 1)
            std::printf("%s  TIMECODE %s\n", when.c_str(), md.tcAdvancing ? "started running" : "stopped");
        m_tcRunning = md.tcAdvancing;
        if (md.sonyClip != m_clip) {
            std::printf("%s  CLIP  '%s' -> '%s'\n", when.c_str(), m_clip.c_str(), md.sonyClip.c_str());
            m_clip = md.sonyClip;
        }
        // Output gating diagnostics: would the viewer send this frame?
        {
            const CameraSample cs = sampleFrom(md);
            const bool lens = cs.anyLens();
            const bool arriComplete = md.klv.empty() || md.klv[0].complete;
            if (lens != m_lens || md.hasArri != m_hasArri || arriComplete != m_arriComplete || m_frames == 0)
                std::printf("%s  STATE arri=%d klv_complete=%d (%zu klv, %zu packets) lens_data=%d -> viewer %s\n", when.c_str(),
                            md.hasArri, arriComplete, md.klv.size(), md.packets.size(), lens,
                            lens ? "SENDS" : "would NOT send (only-with-lens)");
            m_lens = lens;
            m_hasArri = md.hasArri;
            m_arriComplete = arriComplete;
            if (m_lastFrameT > 0 && t - m_lastFrameT > 0.2)
                std::printf("%s  FRAME GAP %.2f s\n", when.c_str(), t - m_lastFrameT);
            m_lastFrameT = t;
        }
        // Every master-record field that changed (except ones that move every frame)
        {
            const MetaRecord rec = sampleFrom(md).record;
            for (const auto& f : allFields()) {
                if (f.field == Field::Timecode || f.field == Field::CameraTimecode) continue;
                const std::string v = rec.display(f.field);
                auto& prev = m_fields[size_t(f.field)];
                if (prev != v) {
                    if (m_frames > 0) std::printf("%s  FIELD %-22s %s -> %s\n", when.c_str(), f.id, prev.c_str(), v.c_str());
                    prev = v;
                }
            }
        }
        if (md.recordingName != m_recName) {
            std::printf("%s  RECORDING NAME -> '%s'\n", when.c_str(), md.recordingName.c_str());
            m_recName = md.recordingName;
        }
        if (md.hasArri && md.arri.clip() != m_arriClip) {
            std::printf("%s  ARRI CLIP '%s' -> '%s'\n", when.c_str(), m_arriClip.c_str(), md.arri.clip().c_str());
            m_arriClip = md.arri.clip();
        }
        if (md.rec != RecState::Unknown && md.rec != m_rec) {
            std::printf("%s  REC FLAG %s\n", when.c_str(), recStateName(md.rec));
            m_rec = md.rec;
        }
        for (const auto& c : md.bmd.commands) {
            if (c.category == 9 && c.parameter == 4) continue;  // camera timecode: changes every frame
            auto& prev = m_prev[0xB0000000u | uint32_t(c.category) << 8 | c.parameter];
            if (prev.first == c.data) continue;
            if (m_frames > 0 && !prev.first.empty())
                std::printf("%s  BMD %u.%u %-32s %s -> %s\n", when.c_str(), c.category, c.parameter,
                            *c.name ? c.name : "?", prev.second.c_str(), c.display.c_str());
            prev = {c.data, c.display};
        }
        for (const auto& set : md.rdd18.sets)
            for (const auto& it : set.items) {
                auto& prev = m_prev[rdd18ItemKey(set, it.tag)];
                if (prev.first == it.value) continue;
                if (m_frames > 0 && !prev.first.empty())
                    std::printf("%s  set %02X tag %04X %-32s %s -> %s\n", when.c_str(), set.setByte, it.tag,
                                *it.name ? it.name : "?", prev.second.c_str(), it.display.c_str());
                prev = {it.value, it.display};
            }
        ++m_frames;
        std::fflush(stdout);
    }

    std::vector<AncPacket> m_frame;
    std::vector<std::string> m_tcRaw;
    TimecodeInfo m_tc;
    MetadataProcessor m_proc;
    std::map<uint32_t, std::pair<std::vector<uint8_t>, std::string>> m_prev;
    std::string m_clip;
    std::string m_recName, m_arriClip;
    std::vector<std::string> m_fields = std::vector<std::string>(kFieldCount);
    bool m_lens = false, m_hasArri = false, m_arriComplete = true, m_noSignal = false;
    double m_lastFrameT = -1;
    RecState m_rec = RecState::Unknown;
    bool m_tcRunning = false;
    uint64_t m_frames = 0;
    std::chrono::steady_clock::time_point m_start{}, m_rawStart{};

    static const std::vector<uint32_t>& candidateLines() {
        static const std::vector<uint32_t> v = [] {
            std::vector<uint32_t> r;
            for (uint32_t i = 1; i <= 45; ++i) r.push_back(i);      // progressive / field 1
            for (uint32_t i = 560; i <= 590; ++i) r.push_back(i);  // 1080i field 2
            return r;
        }();
        return v;
    }

    IDeckLinkInput* m_in;
    Stats& m_st;
    long m_width = 1920;
};

void printFlagAttr(IDeckLinkProfileAttributes* a, BMDDeckLinkAttributeID id, const char* label) {
    dl_bool v = false;
    if (a && a->GetFlag(id, &v) == S_OK) std::printf("  %-40s %s\n", label, v ? "yes" : "no");
    else std::printf("  %-40s n/a\n", label);
}

void printStatusFlag(IDeckLinkStatus* st, BMDDeckLinkStatusID id, const char* label) {
    dl_bool v = false;
    if (st && st->GetFlag(id, &v) == S_OK) std::printf("  %-40s %s\n", label, v ? "yes" : "no");
    else std::printf("  %-40s n/a (not reported by this device)\n", label);
}

}  // namespace

const char* duplexName(int64_t d) {
    switch (d) {
        case bmdDuplexFull: return "full duplex";
        case bmdDuplexHalf: return "half duplex";
        case bmdDuplexSimplex: return "simplex";
        case bmdDuplexInactive: return "INACTIVE in this profile";
        default: return "?";
    }
}

const char* profileName(int64_t p) {
    switch (p) {
        case bmdProfileOneSubDeviceFullDuplex: return "1 sub-device, full duplex";
        case bmdProfileOneSubDeviceHalfDuplex: return "1 sub-device, half duplex";
        case bmdProfileTwoSubDevicesFullDuplex: return "2 sub-devices, full duplex";
        case bmdProfileTwoSubDevicesHalfDuplex: return "2 sub-devices, half duplex";
        case bmdProfileFourSubDevicesHalfDuplex: return "4 sub-devices, half duplex";
        default: return "?";
    }
}

std::string connections(int64_t c) {
    std::string s;
    const std::pair<int64_t, const char*> names[] = {{bmdVideoConnectionSDI, "SDI"}, {bmdVideoConnectionHDMI, "HDMI"},
        {bmdVideoConnectionOpticalSDI, "optical SDI"}, {bmdVideoConnectionComponent, "component"},
        {bmdVideoConnectionComposite, "composite"}, {bmdVideoConnectionSVideo, "S-video"},
        {bmdVideoConnectionEthernet, "Ethernet"}, {bmdVideoConnectionOpticalEthernet, "optical Ethernet"}};
    for (const auto& [bit, n] : names)
        if (c & bit) s += (s.empty() ? "" : ", ") + std::string(n);
    return s.empty() ? "none" : s;
}

int listDevices() {
    IDeckLinkIterator* iter = dlp::createIterator();
    if (!iter) { std::fprintf(stderr, "DeckLink driver not found\n"); return 1; }
    IDeckLink* d = nullptr;
    int index = 0;
    while (iter->Next(&d) == S_OK && d) {
        dl_string nm = nullptr;
        const std::string name = d->GetDisplayName(&nm) == S_OK ? dlp::toStd(nm) : "?";
        IDeckLinkInput* in = nullptr;
        const bool hasInput = d->QueryInterface(IID_IDeckLinkInput, (void**)&in) == S_OK && in;
        if (in) in->Release();
        std::printf("[%d] %s%s\n", index++, name.c_str(), hasInput ? "" : "  (no input interface)");
        IDeckLinkProfileAttributes* a = nullptr;
        if (d->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&a) == S_OK && a) {
            int64_t v = 0;
            dl_string str = nullptr;
            if (a->GetString(BMDDeckLinkModelName, &str) == S_OK) std::printf("    model            %s\n", dlp::toStd(str).c_str());
            if (a->GetInt(BMDDeckLinkSubDeviceIndex, &v) == S_OK) {
                int64_t n = 0;
                a->GetInt(BMDDeckLinkNumberOfSubDevices, &n);
                std::printf("    sub-device       %lld of %lld\n", (long long)v + 1, (long long)n);
            }
            if (a->GetInt(BMDDeckLinkPersistentID, &v) == S_OK) std::printf("    persistent ID    %llx\n", (unsigned long long)v);
            if (a->GetInt(BMDDeckLinkDuplex, &v) == S_OK) std::printf("    duplex           %s\n", duplexName(v));
            if (a->GetInt(BMDDeckLinkProfileID, &v) == S_OK) std::printf("    active profile   %s\n", profileName(v));
            if (a->GetInt(BMDDeckLinkVideoInputConnections, &v) == S_OK) std::printf("    input connectors %s\n", connections(v).c_str());
            if (a->GetInt(BMDDeckLinkDeviceInterface, &v) == S_OK)
                std::printf("    interface        %s\n", v == bmdDeviceInterfacePCI ? "PCIe" : v == bmdDeviceInterfaceUSB ? "USB" : "Thunderbolt");
            dl_bool f = false;
            if (a->GetFlag(BMDDeckLinkSupportsHANCInput, &f) == S_OK) std::printf("    HANC input       %s\n", f ? "yes" : "no");
            if (a->GetFlag(BMDDeckLinkSupportsDualLinkSDI, &f) == S_OK) std::printf("    dual link SDI    %s\n", f ? "yes" : "no");
            if (a->GetFlag(BMDDeckLinkSupportsQuadLinkSDI, &f) == S_OK) std::printf("    quad link SDI    %s\n", f ? "yes" : "no");
            a->Release();
        }
        IDeckLinkProfileManager* pm = nullptr;
        if (d->QueryInterface(IID_IDeckLinkProfileManager, (void**)&pm) == S_OK && pm) {
            IDeckLinkProfileIterator* pi = nullptr;
            if (pm->GetProfiles(&pi) == S_OK && pi) {
                IDeckLinkProfile* prof = nullptr;
                std::printf("    profiles        ");
                while (pi->Next(&prof) == S_OK && prof) {
                    IDeckLinkProfileAttributes* pa = nullptr;
                    int64_t id = 0;
                    dl_bool active = false;
                    prof->IsActive(&active);
                    if (prof->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&pa) == S_OK && pa) {
                        pa->GetInt(BMDDeckLinkProfileID, &id);
                        pa->Release();
                    }
                    std::printf(" [%s%s]", profileName(id), active ? " *" : "");
                    prof->Release();
                }
                std::printf("\n");
                pi->Release();
            }
            pm->Release();
        }
        d->Release();
    }
    iter->Release();
    return 0;
}

int main(int argc, char** argv) {
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dump") gDump = true;
        else if (std::string(argv[i]) == "--watch") gWatch = true;
        else if (std::string(argv[i]) == "--raw") gRaw = true;
        else if (std::string(argv[i]) == "--hanc" && i + 1 < argc) {
            gHanc.clear();
            std::istringstream ids(argv[++i]);
            for (std::string id; std::getline(ids, id, ',') && gHanc.size() < 4;)
                if (!id.empty()) gHanc.push_back(uint16_t(std::stoul(id, nullptr, 16)));
        }
        else if (std::string(argv[i]) == "--list") { dlp::initialise(); return listDevices(); }
        else pos.push_back(argv[i]);
    }
    const int seconds = pos.size() > 0 ? std::max(1, std::atoi(pos[0])) : 5;
    const int want = pos.size() > 1 ? std::atoi(pos[1]) : 0;

    if (!dlp::initialise()) { std::fprintf(stderr, "Failed to initialise\n"); return 1; }
    IDeckLinkIterator* iter = dlp::createIterator();
    if (!iter) { std::fprintf(stderr, "DeckLink driver not found (is Desktop Video installed?)\n"); return 1; }

    IDeckLink* dev = nullptr;
    IDeckLinkInput* in = nullptr;
    int idx = 0;
    IDeckLink* d = nullptr;
    while (iter->Next(&d) == S_OK && d) {
        IDeckLinkInput* i = nullptr;
        if (d->QueryInterface(IID_IDeckLinkInput, (void**)&i) == S_OK && i) {
            if (idx++ == want) { dev = d; in = i; break; }
            i->Release();
        }
        d->Release();
    }
    iter->Release();
    if (!dev) { std::fprintf(stderr, "No DeckLink input device #%d\n", want); return 1; }

    IDeckLinkProfileAttributes* attr = nullptr;
    dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attr);
    IDeckLinkStatus* status = nullptr;
    dev->QueryInterface(IID_IDeckLinkStatus, (void**)&status);

    dl_string nm = nullptr;
    std::printf("Device: %s\n", dev->GetDisplayName(&nm) == S_OK ? dlp::toStd(nm).c_str() : "?");
    printFlagAttr(attr, BMDDeckLinkVANCRequires10BitYUVVideoFrames, "VANC requires 10-bit YUV frames");

    printFlagAttr(attr, BMDDeckLinkSupportsHANCInput, "HANC capture supported");
    printFlagAttr(attr, BMDDeckLinkHANCRequiresInputFilterConfiguration, "HANC needs DID/SDID filters");

    IDeckLinkConfiguration* cfg = nullptr;
    if (dev->QueryInterface(IID_IDeckLinkConfiguration, (void**)&cfg) == S_OK && cfg) {
        cfg->SetInt(bmdDeckLinkConfigVideoInputConnection, bmdVideoConnectionSDI);
        // HANC filters, e.g. the ARRI record start/stop flag (DID 0x52 / SDID 0x4D)
        const BMDDeckLinkConfigurationID filters[] = {bmdDeckLinkConfigHANCInputFilter1, bmdDeckLinkConfigHANCInputFilter2,
                                                      bmdDeckLinkConfigHANCInputFilter3, bmdDeckLinkConfigHANCInputFilter4};
        for (size_t k = 0; k < gHanc.size(); ++k) {
            const HRESULT hr = cfg->SetInt(filters[k], gHanc[k]);
            char label[48];
            std::snprintf(label, sizeof label, "HANC filter %zu = %02X/%02X", k + 1, gHanc[k] >> 8, gHanc[k] & 0xFF);
            std::printf("  %-40s %s\n", label, hr == S_OK ? "set" : "not accepted");
        }
        cfg->Release();
    }

    Stats st;
    Probe probe(in, st);
    in->SetCallback(&probe);
    if (in->EnableVideoInput(bmdModeHD1080p25, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection) != S_OK) {
        std::fprintf(stderr, "EnableVideoInput failed: is the SdiMetaViewer (or another app) holding the input?\n");
        return 1;
    }
    in->StartStreams();
    std::printf("Capturing for %d s...%s\n\n", seconds, gWatch ? " (watching for changes)" : "");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    in->StopStreams();
    in->DisableVideoInput();
    in->SetCallback(nullptr);

    std::lock_guard<std::mutex> l(st.mtx);
    std::printf("Signal\n");
    std::printf("  %-40s %s\n", "Detected mode", st.mode.empty() ? "(no format change event)" : st.mode.c_str());
    std::printf("  %-40s %llu (%llu flagged no-signal)\n", "Frames", (unsigned long long)st.frames,
                (unsigned long long)st.noSignalFrames);
    printStatusFlag(status, bmdDeckLinkStatusVideoInputSignalLocked, "Video input locked");
    printStatusFlag(status, bmdDeckLinkStatusAncillaryInputSignalLocked, "Ancillary input locked");

    std::printf("\nANC packets via IDeckLinkVideoFrameAncillaryPackets (what the viewer reads)\n");
    std::printf("  frames with >=1 packet: %llu\n", (unsigned long long)st.framesWithPackets);
    for (const auto& [key, n] : st.packets) {
        std::printf("  DID %02X SDID %02X  x%-6llu %s lines:", key >> 8, key & 0xFF, (unsigned long long)n,
                    st.hancKeys.count(key) ? "HANC" : "VANC");
        for (auto ln : st.packetLines[key]) std::printf(" %u", ln);
        std::printf("\n");
    }
    if (st.packets.empty()) std::printf("  (none)\n");

    if (gDump && !st.lastDump.empty()) {
        std::printf("\nPayloads from the last frame with packets\n");
        for (const auto& d : st.lastDump) std::printf("  %s\n", d.c_str());
    }

    std::printf("\nTimecode, each source queried separately\n");
    for (const auto& [fmt, label] : kTcFormats) {
        auto it = st.tc.find(label);
        if (it == st.tc.end()) std::printf("  %-20s -\n", label);
        else std::printf("  %-20s %llu frames, last %s\n", label, (unsigned long long)it->second.valid, it->second.last.c_str());
    }

    std::printf("\nRaw VANC lines via IDeckLinkVideoFrameAncillary\n");
    if (!st.legacyAncAvailable) {
        std::printf("  Driver returned no VANC buffer at all: this device/mode does not capture VANC.\n");
    } else if (st.vancFormat != "v210") {
        std::printf("  VANC buffer format is '%s', not v210; cannot scan.\n", st.vancFormat.c_str());
    } else if (st.lines.empty()) {
        std::printf("  Driver exposes no VANC lines for this mode.\n");
    } else {
        std::printf("  zero = buffer all zeros (driver likely not capturing VANC); black = real empty blanking\n");
        std::printf("  line  captured   zero  black  with-ADF  DID/SDID found\n");
        for (const auto& [line, ls] : st.lines) {
            std::printf("  %4u  %8llu  %5llu  %5llu  %8llu ", line, (unsigned long long)ls.framesCaptured,
                        (unsigned long long)ls.framesZero, (unsigned long long)ls.framesBlack,
                        (unsigned long long)ls.framesWithAdf);
            for (auto k : ls.didSdid) std::printf(" %02X/%02X", k >> 8, k & 0xFF);
            std::printf("\n");
        }
    }

    if (status) status->Release();
    if (attr) attr->Release();
    in->Release();
    dev->Release();
    dlp::shutdown();
    return 0;
}
