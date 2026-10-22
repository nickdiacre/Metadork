#include "anc/TakeTracker.h"

const std::string& TakeTracker::update(int recording, const std::string& clip, double t) {
    // A record start: forget the previous take's name only once the new one is known (it stays valid until then).
    const bool nowRecording = recording == 1;
    if (nowRecording && !m_recording) m_recStart = t;

    if (!clip.empty() && clip != m_candidate) {
        // On the REC-stop frame Sony already shows the next clip: don't let that become this take's name.
        if (!(m_recording && !nowRecording)) {
            m_candidate = clip;
            m_candidateSince = t;
        }
    }

    // While rolling: accept the candidate once it first appeared at / after REC start (with a one-frame margin, since
    // the record state can lag the name by a frame) and has been stable for the settle time.
    const double margin = 0.1;
    if (nowRecording && !m_candidate.empty() && m_candidateSince >= m_recStart - margin && t - m_candidateSince >= m_settle)
        m_name = m_candidate;

    // At REC stop: a take shorter than the settle time still gets the newest name seen while rolling.
    if (m_recording && !nowRecording && !m_candidate.empty() && m_candidateSince >= m_recStart - margin)
        m_name = m_candidate;

    if (recording != -1) m_recording = nowRecording;
    return m_name;
}
