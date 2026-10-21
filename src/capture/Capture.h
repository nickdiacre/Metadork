#pragma once
#include "anc/MetadataProcessor.h"
#include "capture/DeckLinkPlatform.h"
#include "DeckLinkAPIVersion.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Enumerates DeckLink devices that have an input.
class DeckLinkDevices {
public:
    DeckLinkDevices();
    ~DeckLinkDevices();
    bool driverPresent() const { return m_driver; }
    const std::vector<std::string>& names() const { return m_names; }
    IDeckLink* device(size_t i) const { return i < m_devices.size() ? m_devices[i] : nullptr; }

private:
    bool m_driver = false;
    std::vector<IDeckLink*> m_devices;
    std::vector<std::string> m_names;
};

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

    bool start(std::string& err);
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
