// sdimeta_coverage: generates the field registry (JSON) and a per-camera coverage table (Markdown) by running real
// captures through the adapters. No SDK or hardware needed.
//
// Usage: sdimeta_coverage [--json out.json] [--md out.md] "Label=captures/file.txt" ...
// Capture files are `sdimeta_probe --dump` output (one frame) or `--raw` output (first complete frame is used).

#include "meta/Adapters.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace {

struct Capture {
    std::string label, path;
    MetaRecord record;
};

std::vector<uint8_t> hexBytes(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
}

// Reads one frame of packets (+ timecode) from a probe --dump or --raw file.
bool readFrame(const std::string& path, std::vector<AncPacket>& packets, TimecodeInfo& tc) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    const std::regex dumpHead(R"(\s+DID (\w\w) SDID (\w\w) line (\d+), (\d+) bytes)");
    const std::regex tcLine(R"(\s+RP188 Any\s+\d+ frames, last (\d\d):(\d\d):(\d\d):(\d\d))");
    std::smatch m;
    AncPacket* cur = nullptr;
    std::string rawTime;
    bool raw = false;
    while (std::getline(in, line)) {
        if (std::regex_match(line, m, tcLine)) {
            tc.valid = true;
            tc.h = uint8_t(std::stoi(m[1])); tc.m = uint8_t(std::stoi(m[2]));
            tc.s = uint8_t(std::stoi(m[3])); tc.f = uint8_t(std::stoi(m[4]));
            continue;
        }
        if (std::regex_match(line, m, dumpHead)) {
            packets.push_back({});
            cur = &packets.back();
            cur->did = uint8_t(std::stoul(m[1], nullptr, 16));
            cur->sdid = uint8_t(std::stoul(m[2], nullptr, 16));
            cur->line = std::stoul(m[3]);
            continue;
        }
        if (cur && line.rfind("    ", 0) == 0 && line.find_first_not_of(" 0123456789ABCDEF") == std::string::npos) {
            for (auto& b : hexBytes([&] { std::string h; for (char c : line) if (c != ' ') h += c; return h; }())) cur->data.push_back(b);
            continue;
        }
        cur = nullptr;
        // --raw: "<t> <tc> <did> <sdid> <line> <hex>"
        std::istringstream ss(line);
        std::string t, tcs, did, sdid, ln, h;
        if (ss >> t >> tcs >> did >> sdid >> ln >> h && t.find('.') != std::string::npos && tcs.find(':') != std::string::npos) {
            if (raw && t != rawTime) {
                // first frame done; keep going only if it was incomplete (e.g. ARRI needs all 18 packets)
                FrameMetadata md = MetadataProcessor().process(std::vector<AncPacket>(packets), tc, false, 0);
                if (buildRecord(md).kind != CameraKind::None) return true;
                packets.clear();
            }
            raw = true;
            rawTime = t;
            AncPacket p;
            p.did = uint8_t(std::stoul(did, nullptr, 16));
            p.sdid = uint8_t(std::stoul(sdid, nullptr, 16));
            p.line = std::stoul(ln);
            p.data = hexBytes(h);
            packets.push_back(std::move(p));
            if (tcs != "--:--:--:--" && tcs.size() == 11) {
                tc.valid = true;
                tc.h = uint8_t(std::stoi(tcs.substr(0, 2))); tc.m = uint8_t(std::stoi(tcs.substr(3, 2)));
                tc.s = uint8_t(std::stoi(tcs.substr(6, 2))); tc.f = uint8_t(std::stoi(tcs.substr(9, 2)));
            }
        }
    }
    return !packets.empty();
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

const char* typeName(FieldType t) { return t == FieldType::Float ? "float" : t == FieldType::Int ? "int" : "string"; }

std::string cell(const FieldValue& v, const FieldInfo& f, const MetaRecord& r) {
    if (!v.set) return "—";
    std::string val = r.display(f.field);
    if (val.size() > 28) val = val.substr(0, 25) + "...";
    for (auto& c : val) if (c == '|') c = '/';
    const char* mark = v.source != ValueSource::Camera ? "🔶" : v.confidence == Confidence::Verified ? "✅"
                       : v.confidence == Confidence::Documented ? "📄" : "🔶";
    return std::string(mark) + " " + val;
}

}  // namespace

int main(int argc, char** argv) {
    std::string jsonOut, mdOut;
    std::vector<Capture> caps;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json" && i + 1 < argc) jsonOut = argv[++i];
        else if (a == "--md" && i + 1 < argc) mdOut = argv[++i];
        else {
            const auto eq = a.find('=');
            Capture c;
            c.label = eq == std::string::npos ? a : a.substr(0, eq);
            c.path = eq == std::string::npos ? a : a.substr(eq + 1);
            std::vector<AncPacket> packets;
            TimecodeInfo tc;
            if (!readFrame(c.path, packets, tc)) { std::fprintf(stderr, "Cannot read %s\n", c.path.c_str()); return 1; }
            c.record = buildRecord(MetadataProcessor().process(std::move(packets), tc, false, 0));
            caps.push_back(std::move(c));
        }
    }

    if (!jsonOut.empty()) {
        std::ofstream o(jsonOut);
        o << "{\n  \"description\": \"SDI Metadata Viewer master field registry (generated from src/meta/Fields.def)\",\n"
             "  \"fields\": [\n";
        const auto& fs = allFields();
        for (size_t i = 0; i < fs.size(); ++i) {
            const auto& f = fs[i];
            o << "    {\"id\": \"" << f.id << "\", \"group\": \"" << fieldGroupName(f.group) << "\", \"label\": \""
              << jsonEsc(f.label) << "\", \"unit\": \"" << f.unit << "\", \"type\": \"" << typeName(f.type)
              << "\", \"livegrade\": \"" << f.livegrade << "\", \"opentrackio\": \"" << f.otio << "\", \"freed\": \""
              << f.freed << "\", \"osc\": \"<prefix>/" << f.id << "\"}" << (i + 1 < fs.size() ? ",\n" : "\n");
        }
        o << "  ]\n}\n";
    }

    std::ostringstream md;
    md << "# Field coverage by camera (generated)\n\n"
          "Generated by `sdimeta_coverage` from real captures; do not edit by hand. Regenerate with:\n\n"
          "```bash\n./build/sdimeta_coverage --json docs/fields.json --md docs/COVERAGE.md";
    for (const auto& c : caps) md << " \\\n  \"" << c.label << "=" << c.path << "\"";
    md << "\n```\n\n✅ verified on hardware · 📄 per spec / documentation · 🔶 derived or inferred · — not supplied in "
          "this capture\n\n| Group | Field (`id`) |";
    for (const auto& c : caps) md << " " << c.label << " |";
    md << " Livegrade |\n|---|---|";
    for (size_t i = 0; i < caps.size(); ++i) md << "---|";
    md << "---|\n";
    for (const auto& f : allFields()) {
        md << "| " << fieldGroupName(f.group) << " | " << f.label << " (`" << f.id << "`) |";
        for (const auto& c : caps) md << " " << cell(c.record.get(f.field), f, c.record) << " |";
        md << " " << (*f.livegrade ? f.livegrade : "") << " |\n";
    }
    md << "\nCaptures:\n\n";
    for (const auto& c : caps)
        md << "- **" << c.label << "**: `" << c.path << "` (" << (c.record.adapter.empty() ? "no adapter matched" : c.record.adapter)
           << ")\n";
    if (!mdOut.empty()) std::ofstream(mdOut) << md.str();
    else std::fputs(md.str().c_str(), stdout);
    return 0;
}
