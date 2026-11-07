#include "anc/CaptureReplay.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

namespace {
constexpr uint32_t kTcRecordingTriggered = 1 << 4;  // bmdTimecodeRecordingTriggered
constexpr uint32_t kTcDropFrame = 1 << 0;           // bmdTimecodeIsDropFrame

bool parseTc(const std::string& s, TimecodeInfo& tc) {
    if (s.size() != 11 || s[2] != ':' || s[5] != ':' || (s[8] != ':' && s[8] != ';')) return false;
    for (int i : {0, 1, 3, 4, 6, 7, 9, 10})
        if (s[size_t(i)] < '0' || s[size_t(i)] > '9') return false;
    tc.valid = true;
    tc.h = uint8_t(std::stoi(s.substr(0, 2)));
    tc.m = uint8_t(std::stoi(s.substr(3, 2)));
    tc.s = uint8_t(std::stoi(s.substr(6, 2)));
    tc.f = uint8_t(std::stoi(s.substr(9, 2)));
    tc.dropFrame = s[8] == ';';
    return true;
}

bool parseHex(const std::string& h, std::vector<uint8_t>& out) {
    if (h.size() % 2) return false;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        unsigned v = 0;
        for (size_t j = i; j < i + 2; ++j) {
            const char c = h[j];
            v = v * 16 + unsigned(c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99);
        }
        if (v > 255) return false;
        out.push_back(uint8_t(v));
    }
    return true;
}
}  // namespace

bool loadRawCapture(const std::string& path, std::vector<ReplayFrame>& frames, std::string& err) {
    frames.clear();
    std::ifstream in(path);
    if (!in) { err = "Cannot open " + path; return false; }
    std::string line, curT;
    size_t packets = 0;
    auto frameAt = [&](const std::string& t) -> ReplayFrame& {
        if (frames.empty() || t != curT) {
            frames.push_back({});
            frames.back().t = std::stod(t);
            curT = t;
        }
        return frames.back();
    };
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        if (line.rfind("# ", 0) == 0) {
            // "# <t> TC RP188 Any <tc> ub=XXXXXXXX flags=N"
            std::string hash, t, tcWord, rp, which, tcs, ub, fl;
            if (!(ss >> hash >> t >> tcWord >> rp >> which >> tcs >> ub >> fl) || tcWord != "TC" || which != "Any") continue;
            if (frames.empty() || t != curT) continue;  // always follows its frame's packets
            ReplayFrame& f = frames.back();
            TimecodeInfo tc;
            if (!parseTc(tcs, tc)) continue;
            if (ub.rfind("ub=", 0) == 0) tc.userBits = uint32_t(std::stoul(ub.substr(3), nullptr, 16));
            if (fl.rfind("flags=", 0) == 0) tc.flags = uint32_t(std::stoul(fl.substr(6)));
            tc.recTrigger = (tc.flags & kTcRecordingTriggered) != 0;
            tc.dropFrame = tc.dropFrame || (tc.flags & kTcDropFrame) != 0;
            f.tc = tc;
            continue;
        }
        std::string t, tcs, did, sdid, ln, hex;
        if (!(ss >> t >> tcs >> did >> sdid >> ln >> hex) || t.find('.') == std::string::npos || did.size() != 2 ||
            sdid.size() != 2)
            continue;
        AncPacket p;
        std::vector<uint8_t> id;
        if (!parseHex(did + sdid, id) || !parseHex(hex, p.data)) continue;
        p.did = id[0];
        p.sdid = id[1];
        p.line = uint32_t(std::stoul(ln));
        ReplayFrame& f = frameAt(t);
        if (!f.tc.valid) parseTc(tcs, f.tc);
        f.packets.push_back(std::move(p));
        ++packets;
    }
    if (!packets) { err = path + " has no sdimeta_probe --raw packet lines"; frames.clear(); return false; }
    return true;
}

bool CaptureReplay::start(const std::string& path, std::string& err) {
    stop();
    std::vector<ReplayFrame> frames;
    if (!loadRawCapture(path, frames, err)) return false;
    m_frames = std::move(frames);
    m_path = path;
    std::vector<double> gaps;
    for (size_t i = 1; i < m_frames.size(); ++i) gaps.push_back(m_frames[i].t - m_frames[i - 1].t);
    std::sort(gaps.begin(), gaps.end());
    const double gap = gaps.empty() ? 0.04 : gaps[gaps.size() / 2];
    m_fps = gap > 0.001 ? 1.0 / gap : 25;
    m_processor = std::make_unique<MetadataProcessor>();
    m_processor->setRecFromTimecode(m_recFromTc);
    m_processor->setRedPositionLetter(m_redPosition);
    m_pos = 0;
    m_stop = false;
    m_thread = std::thread([this] { run(); });
    return true;
}

void CaptureReplay::stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}

void CaptureReplay::videoRate(int& num, int& den) const {
    // The capture's times are host times rounded to 1 ms, so snap to the nearest standard rate.
    static const int rates[][2] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1},
                                   {50, 1}, {60000, 1001}, {60, 1}};
    num = 25, den = 1;
    double best = 1e9;
    for (const auto& r : rates) {
        const double d = std::fabs(double(r[0]) / r[1] - m_fps);
        if (d < best) { best = d; num = r[0]; den = r[1]; }
    }
}

void CaptureReplay::run() {
    using Clock = std::chrono::steady_clock;
    const auto origin = Clock::now();
    double loopStart = 0;  // seconds since origin at which the current pass started
    const double frameGap = 1.0 / m_fps;
    while (!m_stop) {
        for (size_t i = 0; i < m_frames.size() && !m_stop; ++i) {
            const ReplayFrame& f = m_frames[i];
            const auto due = origin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(loopStart + f.t));
            // Sleep in short steps so stop() stays responsive across long gaps in a capture.
            while (!m_stop && Clock::now() < due)
                std::this_thread::sleep_for(std::min<Clock::duration>(due - Clock::now(), std::chrono::milliseconds(20)));
            if (m_stop) break;
            const double now = std::chrono::duration<double>(Clock::now() - origin).count();
            m_pos = i;
            if (m_handler) m_handler(m_processor->process(std::vector<AncPacket>(f.packets), f.tc, false, now));
        }
        loopStart += duration() + frameGap;
    }
}
