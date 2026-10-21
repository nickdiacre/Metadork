#pragma once
#include "anc/MetadataProcessor.h"
#include "capture/DeckLinkPlatform.h"
#include "DeckLinkAPIVersion.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// One DeckLink device. Multi-port cards (e.g. DeckLink 8K Pro) appear as one sub-device per port; which ports are
// usable depends on the card's active profile (an inactive sub-device cannot capture).
struct DeckLinkPort {
    IDeckLink* device = nullptr;
    std::string name;          // e.g. "DeckLink 8K Pro (1)"
    std::string model;         // e.g. "DeckLink 8K Pro"
    int subIndex = 0, subCount = 1;
    int64_t persistentId = 0;  // shared by the sub-devices of one card (low bits differ)
    int64_t duplex = 0;        // BMDDuplexMode
    int64_t connections = 0;   // BMDVideoConnection bit field of supported input connectors
    bool hancInput = false;    // can capture HANC (the ARRI record flag)
    bool active() const { return duplex != bmdDuplexInactive; }
    std::string label() const;  // name + port / duplex summary for the UI
};

struct DeckLinkProfileInfo {
    int64_t id = 0;  // BMDProfileID
    std::string name;
    bool active = false;
};

// Enumerates DeckLink devices that have an input interface (including ports inactive in the current profile).
class DeckLinkDevices {
public:
    DeckLinkDevices();
    ~DeckLinkDevices();
    bool driverPresent() const { return m_driver; }
    const std::vector<DeckLinkPort>& ports() const { return m_ports; }
    size_t size() const { return m_ports.size(); }
    IDeckLink* device(size_t i) const { return i < m_ports.size() ? m_ports[i].device : nullptr; }

    // Profiles of the card that port i belongs to. Activating one applies to the whole card (all its ports, and any
    // other application using it); completes asynchronously, so rescan devices afterwards.
    std::vector<DeckLinkProfileInfo> profiles(size_t i) const;
    bool activateProfile(size_t i, int64_t profileId, std::string& err) const;

private:
    bool m_driver = false;
    std::vector<DeckLinkPort> m_ports;
};

const char* videoConnectionName(int64_t connection);  // single BMDVideoConnection bit
const char* profileIdName(int64_t profileId);

// SDK 14.3 moved pixel access off IDeckLinkVideoFrame onto IDeckLinkVideoBuffer.
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
  #define SDIMETA_HAS_VIDEO_BUFFER 1
#else
  #define SDIMETA_HAS_VIDEO_BUFFER 0
#endif

// A CPU-side BGRA frame the SDK's video converter can write into.
class BgraFrame : public IDeckLinkVideoFrame
#if SDIMETA_HAS_VIDEO_BUFFER
    , public IDeckLinkVideoBuffer
#endif
{
public:
    BgraFrame(long w, long h) : m_w(w), m_h(h), m_buf(size_t(w) * h * 4) {}
    long GetWidth() override { return m_w; }
    long GetHeight() override { return m_h; }
    long GetRowBytes() override { return m_w * 4; }
    BMDPixelFormat GetPixelFormat() override { return bmdFormat8BitBGRA; }
    BMDFrameFlags GetFlags() override { return bmdFrameFlagDefault; }
    HRESULT GetBytes(void** buffer) override { *buffer = m_buf.data(); return S_OK; }
#if SDIMETA_HAS_VIDEO_BUFFER
    HRESULT GetSize(uint64_t* size) override { *size = m_buf.size(); return S_OK; }
    HRESULT StartAccess(BMDBufferAccessFlags) override { return S_OK; }
    HRESULT EndAccess(BMDBufferAccessFlags) override { return S_OK; }
#endif
    HRESULT GetTimecode(BMDTimecodeFormat, IDeckLinkTimecode** tc) override { *tc = nullptr; return S_FALSE; }
    HRESULT GetAncillaryData(IDeckLinkVideoFrameAncillary** a) override { *a = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override {
#if SDIMETA_HAS_VIDEO_BUFFER
        if (dlp::iidEquals(iid, IID_IDeckLinkVideoBuffer)) {
            *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
            AddRef();
            return S_OK;
        }
#endif
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --m_ref; if (r == 0) delete this; return r; }
    const uint8_t* data() const { return m_buf.data(); }

private:
    long m_w, m_h;
    std::vector<uint8_t> m_buf;
    std::atomic<ULONG> m_ref{1};
};

struct InputStatus {
    bool running = false;
    bool noSignal = true;
    std::string modeName;
    long width = 0, height = 0;
    double fps = 0;
    int rateNum = 0, rateDen = 1;  // exact video frame rate, e.g. 30000/1001
    bool rgb444 = false;
    std::string error;
};

class CaptureSession : public IDeckLinkInputCallback {
public:
    using FrameHandler = std::function<void(FrameMetadata&&)>;

    static CaptureSession* create(IDeckLink* device, FrameHandler handler);  // ref count 1

    // connection: BMDVideoConnection to select (0 = leave the device's current setting).
    bool start(std::string& err, int64_t connection = bmdVideoConnectionSDI);
    void stop();
    InputStatus status() const;
    void setRecFromTimecode(bool on) { m_processor.setRecFromTimecode(on); }

    // Returns the newest frame for preview (caller must Release), or nullptr if none new.
    IDeckLinkVideoInputFrame* takePreviewFrame();

    // IDeckLinkInputCallback
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode* mode,
                                                      BMDDetectedVideoInputFormatFlags flags) override;
    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket* audio) override;

private:
    CaptureSession(IDeckLink* device, FrameHandler handler);
    ~CaptureSession();
    void setMode(IDeckLinkDisplayMode* mode, bool rgb);

    IDeckLink* m_device = nullptr;
    IDeckLinkInput* m_input = nullptr;
    FrameHandler m_handler;
    MetadataProcessor m_processor;
    std::atomic<ULONG> m_ref{1};

    mutable std::mutex m_mtx;
    InputStatus m_status;
    IDeckLinkVideoInputFrame* m_preview = nullptr;
};
