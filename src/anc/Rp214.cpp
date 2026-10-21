#include "anc/Rp214.h"
#include <map>

size_t decodeBerLength(const uint8_t* p, size_t avail, size_t& length) {
    if (avail < 1) return 0;
    if (p[0] < 0x80) { length = p[0]; return 1; }
    size_t n = p[0] & 0x7F;
    if (n == 0 || n > 8 || avail < 1 + n) return 0;
    length = 0;
    for (size_t i = 0; i < n; ++i) length = (length << 8) | p[1 + i];
    return 1 + n;
}

std::vector<KlvObject> reassembleRp214(const std::vector<AncPacket>& packets) {
    // (did/sdid, mid) -> psc -> packet. Duplicate inserts (PsF second field) overwrite harmlessly.
    std::map<std::pair<uint16_t, uint8_t>, std::map<uint16_t, const AncPacket*>> groups;
    for (const auto& p : packets) {
        if (p.did != 0x44 || (p.sdid != 0x04 && p.sdid != 0x14)) continue;
        if (p.data.size() < 4) continue;
        uint16_t be = uint16_t(p.data[1] << 8 | p.data[2]);
        uint16_t le = uint16_t(p.data[2] << 8 | p.data[1]);
        uint16_t psc = (be > 255 && le <= 255) ? le : be;  // tolerate either byte order
        groups[{ancKey(p.did, p.sdid), p.data[0]}][psc] = &p;
    }

    std::vector<KlvObject> out;
    for (auto& [id, segs] : groups) {
        KlvObject obj;
        obj.did = uint8_t(id.first >> 8);
        obj.sdid = uint8_t(id.first & 0xFF);
        obj.mid = id.second;

        std::vector<uint8_t> buf;
        uint16_t expect = 1;
        for (auto& [psc, pkt] : segs) {
            if (psc != expect) break;  // gap: stop at first missing segment
            buf.insert(buf.end(), pkt->data.begin() + 3, pkt->data.end());
            ++expect;
        }
        obj.segmentsSeen = uint16_t(expect - 1);
        if (buf.size() < 17) { out.push_back(obj); continue; }

        std::copy(buf.begin(), buf.begin() + 16, obj.key.begin());
        size_t len = 0;
        size_t used = decodeBerLength(buf.data() + 16, buf.size() - 16, len);
        if (!used) { out.push_back(obj); continue; }
        obj.declaredLength = len;
        size_t start = 16 + used;
        size_t avail = buf.size() - start;
        size_t take = avail < len ? avail : len;
        obj.value.assign(buf.begin() + start, buf.begin() + start + take);
        obj.complete = (take == len);
        out.push_back(std::move(obj));
    }
    return out;
}
