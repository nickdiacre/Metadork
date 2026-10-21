#include "capture/Capture.h"
#include <numeric>
#include <chrono>

static double steadySeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- devices

const char* videoConnectionName(int64_t c) {
    switch (c) {
        case bmdVideoConnectionSDI: return "SDI";
        case bmdVideoConnectionHDMI: return "HDMI";
        case bmdVideoConnectionOpticalSDI: return "Optical SDI";
        case bmdVideoConnectionComponent: return "Component";
        case bmdVideoConnectionComposite: return "Composite";
        case bmdVideoConnectionSVideo: return "S-Video";
        case bmdVideoConnectionEthernet: return "Ethernet";
        case bmdVideoConnectionOpticalEthernet: return "Optical Ethernet";
        default: return "Other";
    }
}

const char* profileIdName(int64_t p) {
    switch (p) {
        case bmdProfileOneSubDeviceFullDuplex: return "1 port, full duplex";
        case bmdProfileOneSubDeviceHalfDuplex: return "1 port, half duplex";
        case bmdProfileTwoSubDevicesFullDuplex: return "2 ports, full duplex";
        case bmdProfileTwoSubDevicesHalfDuplex: return "2 ports, half duplex";
        case bmdProfileFourSubDevicesHalfDuplex: return "4 ports, half duplex";
        default: return "Unknown profile";
    }
}

std::string DeckLinkPort::label() const {
    std::string s = name;
    if (subCount > 1) s += "  -  port " + std::to_string(subIndex + 1) + "/" + std::to_string(subCount);
    if (!active()) s += "  (inactive in current profile)";
    else if (duplex == bmdDuplexFull) s += ", full duplex";
    else if (duplex == bmdDuplexHalf) s += ", half duplex";
    return s;
}

DeckLinkDevices::DeckLinkDevices() {
    IDeckLinkIterator* it = dlp::createIterator();
    if (!it) return;
    m_driver = true;
    IDeckLink* dl = nullptr;
    while (it->Next(&dl) == S_OK) {
        IDeckLinkInput* in = nullptr;
        if (dl->QueryInterface(IID_IDeckLinkInput, (void**)&in) != S_OK || !in) { dl->Release(); continue; }
        in->Release();
        DeckLinkPort p;
        p.device = dl;
        dl_string name = nullptr;
        p.name = (dl->GetDisplayName(&name) == S_OK) ? dlp::toStd(name) : "DeckLink";
        p.duplex = bmdDuplexFull;
        p.connections = bmdVideoConnectionSDI;
        IDeckLinkProfileAttributes* a = nullptr;
        if (dl->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&a) == S_OK && a) {
            int64_t v = 0;
            dl_string str = nullptr;
            if (a->GetString(BMDDeckLinkModelName, &str) == S_OK) p.model = dlp::toStd(str);
            if (a->GetInt(BMDDeckLinkSubDeviceIndex, &v) == S_OK) p.subIndex = int(v);
            if (a->GetInt(BMDDeckLinkNumberOfSubDevices, &v) == S_OK) p.subCount = int(v);
            a->GetInt(BMDDeckLinkPersistentID, &p.persistentId);
            if (a->GetInt(BMDDeckLinkDuplex, &v) == S_OK) p.duplex = v;
            if (a->GetInt(BMDDeckLinkVideoInputConnections, &v) == S_OK && v) p.connections = v;
            dl_bool f = false;
            if (a->GetFlag(BMDDeckLinkSupportsHANCInput, &f) == S_OK) p.hancInput = f;
            a->Release();
        }
        m_ports.push_back(std::move(p));
    }
    it->Release();
}

DeckLinkDevices::~DeckLinkDevices() {
    for (auto& p : m_ports) p.device->Release();
}

std::vector<DeckLinkProfileInfo> DeckLinkDevices::profiles(size_t i) const {
    std::vector<DeckLinkProfileInfo> out;
    IDeckLink* d = device(i);
    IDeckLinkProfileManager* pm = nullptr;
    if (!d || d->QueryInterface(IID_IDeckLinkProfileManager, (void**)&pm) != S_OK || !pm) return out;
    IDeckLinkProfileIterator* pi = nullptr;
    if (pm->GetProfiles(&pi) == S_OK && pi) {
        IDeckLinkProfile* prof = nullptr;
        while (pi->Next(&prof) == S_OK && prof) {
            DeckLinkProfileInfo info;
            dl_bool active = false;
            prof->IsActive(&active);
            info.active = active;
            IDeckLinkProfileAttributes* pa = nullptr;
            if (prof->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&pa) == S_OK && pa) {
                pa->GetInt(BMDDeckLinkProfileID, &info.id);
                pa->Release();
            }
            info.name = profileIdName(info.id);
            out.push_back(std::move(info));
            prof->Release();
        }
        pi->Release();
    }
    pm->Release();
    return out;
}

bool DeckLinkDevices::activateProfile(size_t i, int64_t profileId, std::string& err) const {
    IDeckLink* d = device(i);
    IDeckLinkProfileManager* pm = nullptr;
    if (!d || d->QueryInterface(IID_IDeckLinkProfileManager, (void**)&pm) != S_OK || !pm) {
        err = "This device has no selectable profiles";
        return false;
    }
    IDeckLinkProfile* prof = nullptr;
    bool ok = pm->GetProfile(BMDProfileID(profileId), &prof) == S_OK && prof && prof->SetActive() == S_OK;
    if (!ok) err = "Profile change was refused (is another application using the card?)";
    if (prof) prof->Release();
    pm->Release();
    return ok;
}

// ---------------------------------------------------------------- session

CaptureSession* CaptureSession::create(IDeckLink* device, FrameHandler handler) {
    return new CaptureSession(device, std::move(handler));
}

CaptureSession::CaptureSession(IDeckLink* device, FrameHandler handler) : m_device(device), m_handler(std::move(handler)) {
    if (m_device) m_device->AddRef();
}

CaptureSession::~CaptureSession() {
    if (m_preview) m_preview->Release();
    if (m_input) m_input->Release();
    if (m_device) m_device->Release();
}

HRESULT CaptureSession::QueryInterface(REFIID iid, LPVOID* ppv) {
    if (dlp::iidEquals(iid, IID_IDeckLinkInputCallback)) {
        *ppv = static_cast<IDeckLinkInputCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG CaptureSession::AddRef() { return ++m_ref; }
ULONG CaptureSession::Release() {
    ULONG r = --m_ref;
    if (r == 0) delete this;
    return r;
}

bool CaptureSession::start(std::string& err, int64_t connection) {
    if (!m_device) { err = "No device"; return false; }
    if (m_device->QueryInterface(IID_IDeckLinkInput, (void**)&m_input) != S_OK || !m_input) {
        err = "Device has no input interface";
        return false;
    }

    IDeckLinkProfileAttributes* attr = nullptr;
    dl_bool canDetect = false;
    if (m_device->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attr) == S_OK && attr) {
        attr->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &canDetect);
        attr->Release();
    }
    if (!canDetect) {
        err = "This DeckLink does not support input format detection";
        return false;
    }

    // Select the input connector where the device has several (SDI, HDMI, optical SDI ...).
    IDeckLinkConfiguration* cfg = nullptr;
    if (m_device->QueryInterface(IID_IDeckLinkConfiguration, (void**)&cfg) == S_OK && cfg) {
        if (connection) cfg->SetInt(bmdDeckLinkConfigVideoInputConnection, connection);
        // Ask for the ARRI record start/stop flag, which is a HANC packet (DID 0x52 / SDID 0x4D). Devices without
        // HANC input ignore this; the packets then arrive through the normal ancillary packet iterator.
        cfg->SetInt(bmdDeckLinkConfigHANCInputFilter1, 0x524D);
        cfg->Release();
    }

    m_input->SetCallback(this);
    // Any starting mode works; format detection switches to the real one on the first frame.
    if (m_input->EnableVideoInput(bmdModeHD1080p25, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection) != S_OK) {
        err = "EnableVideoInput failed (device busy in another app?)";
        m_input->SetCallback(nullptr);
        return false;
    }
    if (m_input->StartStreams() != S_OK) {
        err = "StartStreams failed";
        m_input->DisableVideoInput();
        m_input->SetCallback(nullptr);
        return false;
    }
    std::lock_guard<std::mutex> l(m_mtx);
    m_status.running = true;
    m_status.error.clear();
    return true;
}

void CaptureSession::stop() {
    if (m_input) {
        m_input->StopStreams();
        m_input->DisableVideoInput();
        m_input->SetCallback(nullptr);
    }
    std::lock_guard<std::mutex> l(m_mtx);
    m_status.running = false;
    if (m_preview) { m_preview->Release(); m_preview = nullptr; }
}

InputStatus CaptureSession::status() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_status;
}

IDeckLinkVideoInputFrame* CaptureSession::takePreviewFrame() {
    std::lock_guard<std::mutex> l(m_mtx);
    IDeckLinkVideoInputFrame* f = m_preview;
    m_preview = nullptr;
    return f;
}

void CaptureSession::setMode(IDeckLinkDisplayMode* mode, bool rgb) {
    dl_string name = nullptr;
    std::string n = (mode->GetName(&name) == S_OK) ? dlp::toStd(name) : "";
    BMDTimeValue dur = 0;
    BMDTimeScale scale = 0;
    mode->GetFrameRate(&dur, &scale);
    std::lock_guard<std::mutex> l(m_mtx);
    m_status.modeName = n;
    m_status.width = mode->GetWidth();
    m_status.height = mode->GetHeight();
    m_status.fps = dur ? double(scale) / double(dur) : 0;
    const long long g = (scale && dur) ? std::gcd((long long)scale, (long long)dur) : 1;
    m_status.rateNum = int(scale / g);
    m_status.rateDen = int(dur ? dur / g : 1);
    m_status.rgb444 = rgb;
}

HRESULT CaptureSession::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode* mode,
                                                BMDDetectedVideoInputFormatFlags flags) {
    if (!mode || !m_input) return S_OK;
    bool rgb = (flags & bmdDetectedVideoInputRGB444) != 0;
    BMDPixelFormat pf = rgb ? bmdFormat10BitRGB : bmdFormat10BitYUV;
    if (events & (bmdVideoInputDisplayModeChanged | bmdVideoInputColorspaceChanged)) {
        m_input->PauseStreams();
        m_input->EnableVideoInput(mode->GetDisplayMode(), pf, bmdVideoInputEnableFormatDetection);
        m_input->FlushStreams();
        m_input->StartStreams();
    }
    setMode(mode, rgb);
    return S_OK;
}

HRESULT CaptureSession::VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket*) {
    if (!frame) return S_OK;
    const double t = steadySeconds();
    const bool noSignal = (frame->GetFlags() & bmdFrameHasNoInputSource) != 0;

    // Keep the newest frame for the UI thread to convert and draw.
    {
        std::lock_guard<std::mutex> l(m_mtx);
        m_status.noSignal = noSignal;
        frame->AddRef();
        if (m_preview) m_preview->Release();
        m_preview = frame;
    }

    // Ancillary packets
    std::vector<AncPacket> packets;
    IDeckLinkVideoFrameAncillaryPackets* anc = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoFrameAncillaryPackets, (void**)&anc) == S_OK && anc) {
        IDeckLinkAncillaryPacketIterator* it = nullptr;
        if (anc->GetPacketIterator(&it) == S_OK && it) {
            IDeckLinkAncillaryPacket* p = nullptr;
            while (it->Next(&p) == S_OK && p) {
                AncPacket ap;
                ap.did = p->GetDID();
                ap.sdid = p->GetSDID();
                ap.line = p->GetLineNumber();
                ap.stream = p->GetDataStreamIndex();
                const void* data = nullptr;
                uint32_t size = 0;
                if (p->GetBytes(bmdAncillaryPacketFormatUInt8, &data, &size) == S_OK && data && size)
                    ap.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
                packets.push_back(std::move(ap));
                p->Release();
                p = nullptr;
            }
            it->Release();
        }
        anc->Release();
    }

    // Timecode (first valid of HFR TC / VITC1 / LTC / VITC2)
    TimecodeInfo tc;
    IDeckLinkTimecode* dtc = nullptr;
    if (frame->GetTimecode(bmdTimecodeRP188Any, &dtc) == S_OK && dtc) {
        if (dtc->GetComponents(&tc.h, &tc.m, &tc.s, &tc.f) == S_OK) tc.valid = true;
        tc.dropFrame = (dtc->GetFlags() & bmdTimecodeIsDropFrame) != 0;
        BMDTimecodeUserBits ub = 0;
        if (dtc->GetTimecodeUserBits(&ub) == S_OK) tc.userBits = ub;
        dtc->Release();
    }

    if (m_handler) m_handler(m_processor.process(std::move(packets), tc, noSignal, t));
    return S_OK;
}
