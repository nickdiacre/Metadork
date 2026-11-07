#pragma once
// Replays a recorded capture (`sdimeta_probe --raw` output, as in captures/*_raw.txt) through the same decode path as a
// live DeckLink input, at the recorded frame timing and looping. Lets the app be tried, demonstrated and screenshotted
// without a camera. No video: the capture files hold ANC packets and timecode only.
//
// --raw lines:  "<t> <tc> <did> <sdid> <line> <hex>"  one per packet, t = seconds since start (one value per frame)
//               "# <t> TC RP188 Any <hh:mm:ss:ff> ub=<8 hex> flags=<n>"  timecode user bits / flags (newer captures)
#include "anc/MetadataProcessor.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ReplayFrame {
    double t = 0;  // seconds since the start of the capture
    std::vector<AncPacket> packets;
    TimecodeInfo tc;
};

// Parses a --raw capture. Fails (with err) when the file is missing or has no --raw packet lines.
bool loadRawCapture(const std::string& path, std::vector<ReplayFrame>& frames, std::string& err);

class CaptureReplay {
public:
    using FrameHandler = std::function<void(FrameMetadata&&)>;
    explicit CaptureReplay(FrameHandler handler) : m_handler(std::move(handler)) {}
    ~CaptureReplay() { stop(); }

    bool start(const std::string& path, std::string& err);
    void stop();
    bool running() const { return m_thread.joinable(); }
    void setRecFromTimecode(bool on) { m_recFromTc = on; if (m_processor) m_processor->setRecFromTimecode(on); }
    void setRedPositionLetter(char c) { m_redPosition = c; if (m_processor) m_processor->setRedPositionLetter(c); }

    const std::string& path() const { return m_path; }
    size_t frameCount() const { return m_frames.size(); }
    size_t position() const { return m_pos; }      // index of the last frame sent
    double duration() const { return m_frames.empty() ? 0 : m_frames.back().t; }
    double fps() const { return m_fps; }           // from the median frame spacing
    void videoRate(int& num, int& den) const;      // nearest standard rate, for the outputs' timing

private:
    void run();

    FrameHandler m_handler;
    std::unique_ptr<MetadataProcessor> m_processor;  // fresh per start (holds record / take state)
    bool m_recFromTc = true;
    char m_redPosition = 'A';
    std::vector<ReplayFrame> m_frames;
    std::string m_path;
    double m_fps = 25;
    std::atomic<size_t> m_pos{0};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
