#pragma once
#include <string>

// Name of the take being (or last) recorded, for receivers that name a recording when it STOPS (Stage Precision).
//
// The cameras' own clip names aren't usable directly:
//   - ALEXA Mini LF: shows the LAST recorded clip while stopped; the counter steps to the new clip ~0.2 s after REC
//     start, briefly showing reel "A000" first; nothing changes at REC stop.
//   - VENICE 2: shows the NEXT clip (placeholder suffix) while stopped; the real suffix appears on the REC-start
//     frame; it jumps to the next clip number on the REC-stop frame.
//   - RED: shows the NEXT clip's final name while stopped (nothing changes at REC start); it steps to the next clip
//     ~1.5 s after REC stop. Callers pass clipIsNextTake = true: the name showing at REC start is the take name.
// So (ARRI / Sony) the take name is the last clip name first seen after REC start (minus a small margin) that stayed unchanged for
// `settleS`, ignoring names that appear after REC stop; it is held after the stop until the next take starts.
class TakeTracker {
public:
    explicit TakeTracker(double settleS = 0.3) : m_settle(settleS) {}

    // Call once per frame. recording: 1 / 0 / -1 (unknown). Returns the current take name ("" if none yet).
    const std::string& update(int recording, const std::string& clip, double t, bool clipIsNextTake = false);
    const std::string& name() const { return m_name; }
    bool recording() const { return m_recording; }

private:
    double m_settle;
    bool m_recording = false;
    double m_recStart = -1e9;
    std::string m_name;       // current / last take name
    std::string m_candidate;  // latest clip name seen, and when it first appeared
    double m_candidateSince = -1e9;
};
