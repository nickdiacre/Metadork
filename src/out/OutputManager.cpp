#include "out/OutputManager.h"

static void reopen(UdpSender& sock, bool enabled, const std::string& host, int port, bool& open, std::string& err) {
    sock.close();
    err.clear();
    if (enabled) {
        if (port < 1 || port > 65535) err = "Invalid port";
        else sock.open(host, uint16_t(port), err);
    }
    open = sock.isOpen();
}

OutputManager::OutputManager() : m_otioSourceId(randomUuidUrn()) {}

void OutputManager::apply(const OutputSettings& s) {
    std::lock_guard<std::mutex> l(m_mtx);
    const auto& o = m_settings;
    const bool freedNet = s.freedEnabled != o.freedEnabled || s.freedHost != o.freedHost || s.freedPort != o.freedPort ||
                          (s.freedEnabled && !m_freed.isOpen());
    const bool oscNet = s.oscEnabled != o.oscEnabled || s.oscHost != o.oscHost || s.oscPort != o.oscPort ||
                        (s.oscEnabled && !m_osc.isOpen());
    const bool otioNet = s.otioEnabled != o.otioEnabled || s.otioDestination() != o.otioDestination() ||
                         s.otioPort != o.otioPort || (s.otioEnabled && !m_otio.isOpen());
    m_settings = s;
    if (freedNet) reopen(m_freed, s.freedEnabled, s.freedHost, s.freedPort, m_status.freedOpen, m_status.freedError);
    if (oscNet) reopen(m_osc, s.oscEnabled, s.oscHost, s.oscPort, m_status.oscOpen, m_status.oscError);
    if (otioNet) {
        reopen(m_otio, s.otioEnabled, s.otioDestination(), s.otioPort, m_status.otioOpen, m_status.otioError);
        m_otioLastStatic = -1e9;  // send static data straight away
    }
}

void OutputManager::setVideoRate(int num, int den) {
    std::lock_guard<std::mutex> l(m_mtx);
    if (num > 0 && den > 0) { m_rateNum = num; m_rateDen = den; }
}

void OutputManager::onFrame(const FrameMetadata& md) {
    const CameraSample s = sampleFrom(md);
    std::lock_guard<std::mutex> l(m_mtx);
    m_status.last = s;
    m_status.hasLast = true;
    if (md.noSignal || (m_settings.onlyWithLens && !s.anyLens())) return;

    if (m_freed.isOpen()) {
        const auto pkt = buildFreeD(s, m_settings.freed);
        if (m_freed.send(pkt.data(), pkt.size())) ++m_status.freedSent;
        else ++m_status.sendErrors;
    }
    if (m_osc.isOpen()) {
        for (const auto& d : buildOsc(s, m_settings.osc)) {
            if (m_osc.send(d.data(), d.size())) ++m_status.oscSent;
            else ++m_status.sendErrors;
        }
    }
    if (m_otio.isOpen()) {
        OpenTrackIOFrame f;
        f.sampleId = randomUuidUrn();
        f.sourceId = m_otioSourceId;
        f.sequence = m_otioSequence;
        f.rateNum = m_rateNum;
        f.rateDen = m_rateDen;
        f.includeStatic = md.hostTime - m_otioLastStatic >= m_settings.otio.staticIntervalS;
        if (f.includeStatic) m_otioLastStatic = md.hostTime;
        std::string json = buildOpenTrackIOJson(s, m_settings.otio, f);
        for (const auto& pkt : openTrackIOPackets(json, uint16_t(m_otioSequence))) {
            if (m_otio.send(pkt.data(), pkt.size())) ++m_status.otioSent;
            else ++m_status.sendErrors;
        }
        ++m_otioSequence;
        m_status.otioLastJson = std::move(json);
    }
}

OutputManager::Status OutputManager::status() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_status;
}
