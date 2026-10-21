#include "anc/AncRegistry.h"
#include <cstdio>

struct AncName { uint8_t did, sdid; const char* name; };

static const AncName kNames[] = {
    {0x41, 0x01, "ST 352 Payload ID"},
    {0x41, 0x05, "ST 2016-3 AFD / Bar data"},
    {0x41, 0x06, "ST 2016-4 Pan-Scan"},
    {0x41, 0x07, "ST 2010 SCTE-104"},
    {0x41, 0x08, "ST 2031 DVB/SCTE VBI"},
    {0x41, 0x0C, "ST 2108-1 HDR/WCG metadata"},
    {0x43, 0x01, "BT.1685 inter-station control"},
    {0x43, 0x02, "RDD 8 / OP-47 subtitling"},
    {0x43, 0x03, "RDD 8 / OP-47 multipacket"},
    {0x43, 0x05, "RDD 18 camera acquisition metadata"},
    {0x44, 0x04, "RP 214 KLV metadata (VANC)"},
    {0x44, 0x14, "RP 214 KLV metadata (HANC)"},
    {0x44, 0x44, "RP 223 UMID / program ID"},
    {0x45, 0x01, "ST 2020 audio metadata"},
    {0x50, 0x01, "RDD 8 WSS"},
    {0x51, 0x01, "RP 215 film codes"},
    {0x51, 0x52, "Blackmagic tally"},
    {0x51, 0x53, "Blackmagic camera control"},
    {0x52, 0x4D, "ARRI record start/stop flag"},
    {0x60, 0x60, "ST 12-2 ancillary timecode (ATC)"},
    {0x61, 0x01, "ST 334 CEA-708 captions (CDP)"},
    {0x61, 0x02, "ST 334 CEA-608 captions"},
    {0x62, 0x01, "RP 207 program description"},
    {0x62, 0x02, "ST 334 data broadcast"},
    {0x62, 0x03, "RP 208 VBI data"},
    {0x64, 0x64, "RP 196 LTC (HANC)"},
    {0x64, 0x7F, "RP 196 VITC (HANC)"},
};

const char* describeAnc(uint8_t did, uint8_t sdid) {
    for (const auto& n : kNames)
        if (n.did == did && n.sdid == sdid) return n.name;
    if (did >= 0x50 && did <= 0x5F) return "User application (unregistered)";
    if (did >= 0xC0 && did <= 0xCF) return "User application (type 1)";
    return "Unknown";
}

std::string toHex(const uint8_t* data, size_t len, size_t maxBytes, char sep) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    size_t n = len < maxBytes ? len : maxBytes;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        if (i && sep) s.push_back(sep);
        s.push_back(d[data[i] >> 4]);
        s.push_back(d[data[i] & 0xF]);
    }
    if (n < len) s += " ...";
    return s;
}

std::string hexDump(const uint8_t* data, size_t len, size_t perLine) {
    std::string out;
    char buf[32];
    for (size_t off = 0; off < len; off += perLine) {
        std::snprintf(buf, sizeof buf, "%04zX: ", off);
        out += buf;
        size_t n = (len - off < perLine) ? len - off : perLine;
        out += toHex(data + off, n);
        for (size_t i = n; i < perLine; ++i) out += "   ";
        out += "  ";
        for (size_t i = 0; i < n; ++i) {
            uint8_t c = data[off + i];
            out.push_back(c >= 32 && c < 127 ? char(c) : '.');
        }
        out.push_back('\n');
    }
    return out;
}
