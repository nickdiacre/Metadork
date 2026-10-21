#include "capture/DeckLinkPlatform.h"
#include <cstdlib>
#include <cstring>

namespace dlp {

#ifdef _WIN32
bool initialise() { return SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
void shutdown() { CoUninitialize(); }

std::string toStd(dl_string s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    SysFreeString(s);
    return out;
}

IDeckLinkIterator* createIterator() {
    IDeckLinkIterator* it = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&it))) return nullptr;
    return it;
}

IDeckLinkVideoConversion* createVideoConversion() {
    IDeckLinkVideoConversion* c = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkVideoConversion, nullptr, CLSCTX_ALL, IID_IDeckLinkVideoConversion, (void**)&c))) return nullptr;
    return c;
}

bool iidEquals(REFIID a, REFIID b) { return IsEqualIID(a, b) != FALSE; }

#else  // macOS + Linux share the dispatch functions

bool initialise() { return true; }
void shutdown() {}

std::string toStd(dl_string s) {
    if (!s) return {};
#ifdef __APPLE__
    char buf[512];
    std::string out;
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) out = buf;
    CFRelease(s);
    return out;
#else
    std::string out(s);
    std::free((void*)s);
    return out;
#endif
}

IDeckLinkIterator* createIterator() { return CreateDeckLinkIteratorInstance(); }
IDeckLinkVideoConversion* createVideoConversion() { return CreateVideoConversionInstance(); }
bool iidEquals(REFIID a, REFIID b) { return std::memcmp(&a, &b, sizeof(REFIID)) == 0; }

#endif

}  // namespace dlp
