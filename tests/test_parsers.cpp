// Parser tests that run without a DeckLink card or SDK.
// Builds a synthetic ARRI SDI metadata stream exactly as described in the
// ARRI SDI Metadata white paper and checks it decodes.
#include "anc/MetadataProcessor.h"
#include "log/JsonLogger.h"
#include "out/Protocols.h"
#include "out/SensorModes.h"
#include "out/Settings.h"
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> (8 * i)); }
static void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = uint8_t(v); b[o + 1] = uint8_t(v >> 8); }
static void putStr(std::vector<uint8_t>& b, size_t o, const char* s) { std::memcpy(&b[o], s, std::strlen(s)); }

static std::vector<AncPacket> makeArriPackets(const std::vector<uint8_t>& block, uint8_t mid) {
    std::vector<uint8_t> klv = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01, 0x0D, 0x0E, 0x17, 0x00, 0x00, 0x00, 0x11, 0x01, 0x01,
                                0x83, 0x00, 0x10, 0x00};
    klv.insert(klv.end(), block.begin(), block.end());  // 4116 bytes
    std::vector<AncPacket> pkts;
    for (uint16_t psc = 1; size_t(psc - 1) * 230 < klv.size(); ++psc) {
        AncPacket p;
        p.did = 0x44; p.sdid = 0x04; p.line = psc <= 9 ? 9 : (psc <= 16 ? 10 : 11);
        p.data = {mid, uint8_t(psc >> 8), uint8_t(psc)};
        size_t start = (psc - 1) * 230;
        for (size_t i = 0; i < 230; ++i) p.data.push_back(start + i < klv.size() ? klv[start + i] : 0);
        pkts.push_back(p);
    }
    return pkts;
}

// One real frame from a Sony BURANO (firmware 3.00, 1080p29.97), captured with sdimeta_probe --dump.
// See captures/burano_2026-10-06_probe.txt.
struct HexPacket { uint8_t did, sdid; uint32_t line; const char* hex; };
static const HexPacket kBurano[] = {
        {0x43, 0x05, 11, "01060e2b34025301010c020101010100008300003b80040002800080050002ffff8007002b556e6b6e6f776e000000000000000000000000000000000000000000000000000000000000000000000000"},
        {0x43, 0x05, 11, "02060e2b34025301010c020101020100008300011381000010060e2b340401010b051001010101000081150002032081190004000003208101000100810300020006810400028c66810500024ef98106000800007530000003e98107000102810800040000288c81090008000000010000003e810a00020000810b00020320811e000400000320810c00020064810d000100810e0002196e811f00020000810f0002002832100010060e2b34040101060e0604010101060532190010060e2b34040101060e06040101030105321a0010060e2b340401010104010101020200008120000102"},
        {0x43, 0x05, 11, "038114003f4d50432d3236313020323030303230352056657273696f6e332e30300000000000000000000000000000000000000000000000000000000000000000000000"},
        {0x43, 0x05, 11, "04060e2b34025301010c0201017f01000083000158e0000010966908004678031c2050000cf0c01181e10100080000078000000780e10200080000078000000438e10f00080000078000000438e116000801cc033000000000e11000020101e10300020c01e10a000100e10b0010060e2b34040101010401010101020000e1110010060e2b34040101060e06040101010605e1120010060e2b34040101060e06040101030105e113003f4c55543a534c3353473343746f733730392e637562650000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
        {0x43, 0x05, 11, "05e115000100e1080010060e2b34040101010401010101020000e10d0010060e2b34040101060401010103030000e10e0010060e2b34040101010401010102020000e109003f4c55543a534c3353473343746f733730392e637562650000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
        {0x43, 0x05, 12, "06060e2b34025301010c0201017f0100008300002ee0000010851008004678031c515e1a1933f911e08414000400000000841500040000000084160001008417000100"},
        {0x44, 0x04, 18, "020001060e2b34010101060e06010501000000162020000141303032433030325f323631303036484500"},
};

static std::vector<AncPacket> buranoPackets() {
    std::vector<AncPacket> out;
    for (const auto& h : kBurano) {
        AncPacket p;
        p.did = h.did; p.sdid = h.sdid; p.line = h.line;
        for (const char* c = h.hex; c[0] && c[1]; c += 2) p.data.push_back(uint8_t(std::stoul(std::string(c, 2), nullptr, 16)));
        out.push_back(std::move(p));
    }
    return out;
}

int main() {
    std::vector<uint8_t> blk(4096, 0);
    put32(blk, 0x0164, 10);                 // ALEXA 35
    putStr(blk, 0x02F8, "Alexa 35");
    put32(blk, 0x0170, 1234);
    put32(blk, 0x0178, 'A');
    put32(blk, 0x0074, 800);
    put32(blk, 0x005C, 5600);
    put32(blk, 0x0190, 1800);              // 180.0 deg
    put32(blk, 0x01A0, 25000);             // 25 fps
    put32(blk, 0x01A8, 0x10203004);        // 10:20:30:04 BCD
    put32(blk, 0x0374, 1);                 // metric
    put32(blk, 0x0378, 2500);              // 2.5 m
    put32(blk, 0x037C, 50000);             // 50 mm
    put32(blk, 0x0384, 4000);              // T2.83
    putStr(blk, 0x0398, "Signature Prime 50");
    put16(blk, 0x03B8, 0x1234);
    put16(blk, 0x03BA, 0x5678);
    blk[0x03DD] = 1;
    put32(blk, 0x03E8, 123456);
    putStr(blk, 0x06B0, "A_0001_1234");
    putStr(blk, 0x06BB, "A_0001C001_260930_101500_a1234");

    auto pkts = makeArriPackets(blk, 1);
    CHECK(pkts.size() == 18);

    // Shuffle order and add a duplicate (PsF second-field insert) to prove order independence.
    std::mt19937 rng(42);
    std::shuffle(pkts.begin(), pkts.end(), rng);
    pkts.push_back(pkts[3]);

    // ARRI HANC rec flag, recording.
    AncPacket rec; rec.did = 0x52; rec.sdid = 0x4D; rec.line = 9;
    rec.data = {0x52, 0x4D, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x02};
    pkts.push_back(rec);

    TimecodeInfo tc; tc.valid = true; tc.h = 10; tc.m = 20; tc.s = 30; tc.f = 4;
    MetadataProcessor proc;
    auto md = proc.process(std::vector<AncPacket>(pkts), tc, false, 1.0);

    CHECK(md.hasArri);
    CHECK(md.klv.size() == 1 && md.klv[0].complete && md.klv[0].declaredLength == 4096);
    const auto& a = md.arri;
    CHECK(a.cameraTypeName == "ALEXA 35");
    CHECK(a.cameraModel == "Alexa 35");
    CHECK(a.cameraIndex == 'A');
    CHECK(a.exposureIndex == 800);
    CHECK(std::fabs(a.shutterAngle - 180.0) < 1e-9);
    CHECK(std::fabs(a.sensorFps - 25.0) < 1e-9);
    CHECK(a.masterTc == "10:20:30:04");
    CHECK(a.focusValid && std::fabs(a.focusMm - 2500.0) < 1e-9);
    CHECK(std::fabs(a.focalLengthMm - 50.0) < 1e-9);
    CHECK(std::fabs(a.tStop - 2.8284) < 1e-3);
    CHECK(a.rawFocusLds == 0x1234 && a.rawFocusMotor == 0x5678);
    CHECK(a.entrancePupilUm == 123456);
    CHECK(a.reel() == "A_0001_1234");
    CHECK(a.clip() == "A_0001C001_260930_101500_a1234");
    CHECK(a.lensModel == "Signature Prime 50");
    CHECK(md.rec == RecState::Recording);

    // Stop state
    pkts.back().data[10] = 0x00;
    md = proc.process(std::vector<AncPacket>(pkts), tc, false, 1.04);
    CHECK(md.rec == RecState::Stopped);
    CHECK(!md.tcAdvancing);

    // Imperial focus
    put32(blk, 0x0374, 0);
    put32(blk, 0x0378, 120000);  // 120 inches
    auto p2 = makeArriPackets(blk, 7);
    tc.f = 5;
    md = proc.process(std::move(p2), tc, false, 1.08);
    CHECK(md.hasArri && std::fabs(md.arri.focusMm - 3048.0) < 1e-6);
    CHECK(md.tcAdvancing);

    // Missing segment -> incomplete, no ARRI decode
    auto p3 = makeArriPackets(blk, 1);
    p3.erase(p3.begin() + 5);
    md = proc.process(std::move(p3), tc, false, 1.12);
    CHECK(!md.hasArri);
    CHECK(md.klv.size() == 1 && !md.klv[0].complete);

    // Infinity focus
    put32(blk, 0x0378, uint32_t(-1));
    md = proc.process(makeArriPackets(blk, 1), tc, false, 1.16);
    CHECK(md.hasArri && md.arri.focusInfinite);

    // RDD 18: real BURANO frame
    {
        using K = Rdd18SetKind;
        auto bp = buranoPackets();
        std::reverse(bp.begin(), bp.end());  // arrival order must not matter
        md = proc.process(std::move(bp), tc, false, 1.2);
        const auto& r = md.rdd18;
        CHECK(md.hasRdd18 && r.packets == 6);
        CHECK(r.sets.size() == 4);
        for (const auto& set : r.sets) CHECK(set.complete);
        CHECK(r.sets.size() == 4 && r.sets[0].kind == K::Lens && r.sets[1].kind == K::Camera && r.sets[2].kind == K::UserDefined);
        CHECK(r.text(K::Lens, 0x8007) == "Unknown");
        CHECK(r.text(K::Camera, 0x8114) == "MPC-2610 2000205 Version3.00");
        CHECK(r.text(K::Camera, 0x8106).rfind("29.970 fps", 0) == 0);
        CHECK(r.text(K::Camera, 0x8104) == "35.942 mm");
        CHECK(r.text(K::Camera, 0x8105) == "20.217 mm");
        CHECK(r.text(K::Camera, 0x810B).rfind("800 ", 0) == 0);
        CHECK(r.text(K::Camera, 0x810E) == "6510 K");
        CHECK(r.text(K::Camera, 0x8107) == "progressive frame");
        CHECK(r.text(K::Camera, 0x321A) == "ITU-R BT.709 coding equations");
        CHECK(r.text(K::UserDefined, 0xE109) == "LUT:SL3SG3Ctos709.cube");
        CHECK(r.text(K::UserDefined, 0xE102).rfind("1920/1080", 0) == 0);
        CHECK(md.sonyClip == "A002C002_261006HE");
        CHECK(!md.hasArri);

        // A missing slice leaves the camera set incomplete but the others intact.
        auto partial = buranoPackets();
        partial.erase(partial.begin() + 2);  // seq 03
        md = proc.process(std::move(partial), tc, false, 1.21);
        CHECK(md.rdd18.find(K::Lens, 0x8007) != nullptr);
        CHECK(md.rdd18.find(K::UserDefined, 0xE109) != nullptr);
        bool camIncomplete = false;
        for (const auto& set : md.rdd18.sets) if (set.kind == K::Camera) camIncomplete = !set.complete;
        CHECK(camIncomplete);

        // Lens unit set from the same BURANO with an 85mm f/1.4 attached (captures/burano_lens_2026-10-06_probe.txt).
        AncPacket lens; lens.did = 0x43; lens.sdid = 0x05; lens.line = 11;
        for (const char* c = "01060e2b34025301010c020101010100008300004d80000002f05f80010002e2be800a0002dd8280040002c37c80050002c3528007002b30303036383232363339000000000000000000000000000000000000000000000000000000000000000000"; c[0] && c[1]; c += 2) lens.data.push_back(uint8_t(std::stoul(std::string(c, 2), nullptr, 16)));
        md = proc.process({lens}, tc, false, 1.215);
        CHECK(md.rdd18.text(K::Lens, 0x8005) == "85.0 mm");
        CHECK(md.rdd18.text(K::Lens, 0x8004) == "89.2 mm");
        CHECK(md.rdd18.text(K::Lens, 0x8001).rfind("7.020 m", 0) == 0);
        CHECK(md.rdd18.text(K::Lens, 0x8000) == "f/1.40");
        CHECK(md.rdd18.text(K::Lens, 0x8007) == "0006822639");

        // Output sample from the Sony lens set
        CameraSample cs = sampleFrom(md);
        CHECK(cs.hasFocus && std::fabs(cs.focusM - 7.02) < 1e-9);
        CHECK(cs.hasFocal && std::fabs(cs.focalMm - 85.0) < 1e-9);
        CHECK(cs.hasIris && cs.irisKind == 'f' && std::fabs(cs.iris - 1.40) < 0.005);
        CHECK(!cs.hasPan && !cs.hasTilt && !cs.hasRoll);

        std::string j = JsonLogger::toJson(proc.process(buranoPackets(), tc, false, 1.22), false);
        CHECK(j.find("\"sony_clip\":\"A002C002_261006HE\"") != std::string::npos);
        CHECK(j.find("\"value\":\"MPC-2610 2000205 Version3.00\"") != std::string::npos);
    }

    // Non-electronic lens: no output values (8005 = FFFF, 8004 = 8000)
    {
        CameraSample cs = sampleFrom(proc.process(buranoPackets(), tc, false, 1.23));
        CHECK(!cs.anyLens());
    }

    // Sony VENICE 2 with a 50mm /i-type prime (captures/venice2_2026-10-21_probe.txt)
    {
        using K = Rdd18SetKind;
        static const HexPacket kVenice2[] = {
            {0x43, 0x05, 11, "01060e2b34025301010c020101010100008300003680000002f0da80080002edb880010002e3dd800300010080040002c1f480050002d032800600020064800700095a3530313238343335"},
            {0x43, 0x05, 11, "02060e2b34025301010c020101020100008300012881000010060e2b340401010b05100101010100008115000204e28101000100810300020008810400028c55810500025d8e81060008000000190000000181070001028108000400002a30810900080000000100000032810a00020000810b000204e2810c00020064810d000100810e0002157c811f00020000810f0002001e32100010060e2b3404010101040101010102000032190010060e2b34040101060401010103030000321a0010060e2b3404010101040101010202000081200001068116000105"},
            {0x43, 0x05, 11, "038117001c0000000a000000023c003c003c000000000000003c003c003c003c008114003f4d50432d3336323620303031303032352056657273696f6e332e30300000000000000000000000000000000000000000000000000000000000000000000000"},
            {0x43, 0x05, 11, "04060e2b34025301010c0201017f0100008300018fe0000010966908004678031c20500006f0c01181e10100080000065400000780e102000800000654000002a6e10f00080000065400000438e11000020101e10300020901e10a000100e10b0010060e2b34040101010401010101020000e1110010060e2b34040101060e06040101010605e1120010060e2b34040101060e06040101030105e113003f30375f54595f332053545249505f4a502e63756265000000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
            {0x43, 0x05, 11, "05e114003f6e6f6e650000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000e115000101e1080010060e2b34040101010401010101020000e10d0010060e2b34040101060401010103030000e10e0010060e2b34040101010401010102020000e109003f30375f54595f332053545249505f4a502e63756265000000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
            {0x43, 0x05, 12, "06060e2b34025301010c0201017f01000083000090e0000010966908004678031c204f0001f0c01181e203000100e20100296440425a6a42588e824040406650544042516140426542454841514040535a35303132383433350a0de21500465a40728040425a6a8e8240665054404251614042654245484151404040404072416442564274435603e803e8036102a101d0015300ce00000000fff9ffddffabff89ff650a0d"},
            {0x43, 0x05, 12, "07060e2b34025301010c0201017f01000083000029e0000010851008004678031c515e1a1933f911e0841400040002085084150004fffff1f08416000100"},
            {0x44, 0x04, 18, "020001060e2b34010101060e06010501000000152020000141303033433035305f3232313030365834"},
        };
        std::vector<AncPacket> vp;
        for (const auto& h : kVenice2) {
            AncPacket p; p.did = h.did; p.sdid = h.sdid; p.line = h.line;
            for (const char* c = h.hex; c[0] && c[1]; c += 2) p.data.push_back(uint8_t(std::stoul(std::string(c, 2), nullptr, 16)));
            vp.push_back(std::move(p));
        }
        md = proc.process(std::move(vp), tc, false, 1.3);
        const auto& r = md.rdd18;
        for (const auto& set : r.sets) CHECK(set.complete);
        CHECK(r.text(K::Camera, 0x8114).rfind("MPC-3626", 0) == 0);
        CHECK(r.text(K::Lens, 0x8001).rfind("9.890 m", 0) == 0);
        CHECK(r.text(K::Lens, 0x8005) == "50.0 mm");
        CHECK(r.text(K::Lens, 0x8008) == "T1.49");
        CHECK(md.sonyClip == "A003C050_221006X4");
        CameraSample cs = sampleFrom(md);
        CHECK(cs.hasFocus && std::fabs(cs.focusM - 9.89) < 1e-9);
        CHECK(cs.hasFocal && std::fabs(cs.focalMm - 50.0) < 1e-9);
        CHECK(cs.hasIris && cs.irisKind == 'T' && std::fabs(cs.iris - 1.49) < 0.01);
        CHECK(cs.hasTilt && std::fabs(cs.tilt - 3.7) < 1e-9 && !cs.hasPan);  // 8414 = 133200
        CHECK(cs.hasRoll && std::fabs(cs.roll + 0.1) < 1e-9);                  // 8415 = -3600
        CHECK(cs.hasSqueeze && cs.squeeze == 1.0);  // E10F 1620x1080 on a 3:2 sensor area
        CHECK(cs.hasSensor && std::fabs(cs.sensorWidthMm - 35.925) < 1e-9 && std::fabs(cs.sensorHeightMm - 23.95) < 1e-9);
        OscConfig soc;
        for (const auto& v : catalogue(cs)) if (std::string(v.id).rfind("sensor_", 0) != 0) soc.disabled.insert(v.id);
        auto sm = buildOsc(cs, soc);
        CHECK(sm.size() == 2 && sm[0] == oscMessage("/camera/sensor_width", 35.925f) &&
              sm[1] == oscMessage("/camera/sensor_height", 23.95f));
        CHECK(cs.kind == CameraKind::Sony && cs.cameraModel == "VENICE 2 (MPC-3626)" && cs.cameraSerial == "0010025" &&
              cs.cameraFirmware == "Version3.00" && cs.lensSerial == "Z50128435");
        CHECK(cs.hasIso && cs.iso == 1250 && cs.hasShutter && std::fabs(cs.shutterAngle - 180) < 1e-9);
        CHECK(cs.hasWhiteBalance && cs.whiteBalanceK == 5500 && cs.hasCaptureRate && cs.captureRateNum == 25 && cs.captureRateDen == 1);
        {   // master record: new fields and provenance
            const MetaRecord& r = cs.record;
            CHECK(r.kind == CameraKind::Sony && r.adapter == "Sony RDD 18");
            CHECK(r.has(Field::FocusDistance) && r.get(Field::FocusDistance).origin == "sony.01.8001" &&
                  r.get(Field::FocusDistance).confidence == Confidence::Verified);
            CHECK(std::fabs(r.num(Field::ExposureTime) - 1.0 / 50) < 1e-9);   // 8109 = 1/50
            CHECK(r.has(Field::Gain) && r.num(Field::Gain) == 0.0);
            CHECK(std::fabs(r.num(Field::FocalLength35) - 50.0) < 1e-9 && std::fabs(r.num(Field::Extender) - 1.0) < 1e-9);
            CHECK(r.str(Field::CameraFirmware) == "Version3.00" && r.str(Field::Reel) == "A003");
            CHECK(r.get(Field::Reel).source == ValueSource::Derived);
            CHECK(r.str(Field::LookName) == "07_TY_3 STRIP_JP.cube" && r.has(Field::CaptureGamma));
            CHECK(!r.has(Field::NdDensity) && !r.get(Field::NdDensity).note.empty());   // 8103 seen, meaning unknown
            CHECK(r.str(Field::Camera) == "Sony VENICE 2 (MPC-3626)" && r.str(Field::Lens) == "Z50128435");
        }

        // OpenTrackIO sample from the VENICE 2 frame
        OpenTrackIOConfig oc;
        OpenTrackIOFrame of;
        of.sampleId = "urn:uuid:00000000-0000-4000-8000-000000000001";
        of.sourceId = "urn:uuid:00000000-0000-4000-8000-000000000002";
        of.sequence = 7;
        of.includeStatic = true;
        cs.tc.valid = true; cs.tc.h = 18; cs.tc.m = 24; cs.tc.s = 24; cs.tc.f = 0;
        const std::string j = buildOpenTrackIOJson(cs, oc, of);
        for (const char* want : {"\"protocol\":{\"name\":\"OpenTrackIO\",\"version\":[1,0,1]}", "\"sourceNumber\":1",
                                 "\"focusDistance\":9.89", "\"tStop\":1.4", "\"pinholeFocalLength\":50",
                                 "\"slate\":\"A003C050_221006X4\"", "\"hours\":18", "\"frameRate\":{\"num\":25,\"denom\":1}",
                                 "\"activeSensorPhysicalDimensions\":{\"width\":35.925,\"height\":23.95}",
                                 "\"captureFrameRate\":{\"num\":25,\"denom\":1}", "\"isoSpeed\":1250",
                                 "\"serialNumber\":\"Z50128435\"", "\"model\":\"VENICE 2 (MPC-3626)\""})
            if (j.find(want) == std::string::npos) { std::printf("OpenTrackIO JSON missing %s\n%s\n", want, j.c_str()); CHECK(false); }
        CHECK(j.find("transforms") == std::string::npos);  // off by default
        oc.includeTransform = true;
        CHECK(buildOpenTrackIOJson(cs, oc, of).find("\"rotation\":{\"pan\":0,\"tilt\":3.7,\"roll\":-0.1}") != std::string::npos);
        of.includeStatic = false;
        CHECK(buildOpenTrackIOJson(cs, oc, of).find("\"static\"") == std::string::npos);
        CHECK(md.rdd18.text(K::UserDefined, 0x8415) == "-0.1 deg");
        // Roll readings from the 2026-10-21 test: -16.1 deg anticlockwise
        CHECK(std::fabs(rdd18TenthDegree({0xFF, 0xF7, 0x27, 0xF0}) + 16.1) < 1e-9);
    }

    // VENICE 2 record state from the UMID after the clip name (captures/venice2_rec_raw_2026-10-21.txt)
    {
        auto clipPacket = [](const char* hex) {
            AncPacket p; p.did = 0x44; p.sdid = 0x04; p.line = 18;
            for (const char* c = hex; c[0] && c[1]; c += 2) p.data.push_back(uint8_t(std::stoul(std::string(c, 2), nullptr, 16)));
            return p;
        };
        MetadataProcessor up;
        TimecodeInfo none;
        FrameMetadata m = up.process({clipPacket("020001060E2B34010101060E06010501000000152020000141303033433036335F32323130303657430000000000000000000000000000000000000000000000000000000000000000")}, none, false, 1.0);
        CHECK(m.rec == RecState::Stopped && m.recSource == "Sony UMID in clip packet" && m.sonyUmid.empty());
        CHECK(m.sonyClip == "A003C063_221006WC");
        m = up.process({clipPacket("020001060E2B34010101060E06010501000000152020000141303033433036335F3232313030363151060A2B340101010501010D431300000036044CF5589805D5143FA6FFFE70E4F5")}, none, false, 5.0);
        CHECK(m.rec == RecState::Recording && m.sonyUmid.rfind("060A2B340101010501010D4313", 0) == 0 && m.sonyUmid.size() == 64);
        CHECK(m.sonyClip == "A003C063_2210061Q");
        // BURANO packet (no UMID area) gives no hint, so the fallbacks still apply
        std::string umid;
        CHECK(sonyRecordingFromClipPacket(buranoPackets().back(), umid) == RecStateHint::Unknown);
    }

    // Real ARRI ALEXA Mini LF (SUP 7.03, Canon K35 70 lens file, no encoders) - captures/minilf_2026-10-21_probe.txt
    {
        static const HexPacket kMiniLf[] = {
            {0x44, 0x04, 9, "010100060e2b340205010d0e1700000011010183001000415252491234567800100000030000003c0000008007000038040000420000000100000064000000c6ffffffb9060000ae04000000000000000000008007000038040000fffffffffffffffffffffffffffffffffc000000050000007017000000000000acd4d53f0000803fa058b23f010000002003000000010000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00000000640000000000803f00000000415252492037"},
            {0x44, 0x04, 9, "010200303900000000000000000000000000000000000000000000000002000000ffffffffffffffffffffffff0000803f0000803f0000803f0000803f0000000000000000000000000000803f0000803f0000803fffffffffffffffffffffffff03000000ffffffffffffffff4752424704010201ffffffffffffffffffffffffffffffffffffffffffffffffffffffffb001000009000000ffffffff100000005e830000365950414100000001011320002332090000000000000000204e000020bf0200ffffffffffffffffffffffffa8610000a861000017290300ffffffffa861000000000000"},
            {0x44, 0x04, 9, "010300ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0000000000000000000000000000000000000000000000000000000000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff412d"},
            {0x44, 0x04, 9, "0104004d494e494c46434f44455820436f6d706163742044726976650000000000000000000000000000000000ffffffffffffffffffffffffffffffffffffffffffffffff372e30332e303000000000000000000000000000000000004152524920414c455841204d696e69204c4600ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff74000000010000000000000070110100abb90000"},
            {0x44, 0x04, 9, "010500ffffffff0000000000000000000000000000000043616e6f6e204b333520373020343735333152000000000000000000000000000000ffff0000ffff0000ffffffffffffffffffffffffffffffffffffffffffffffffffff0101000000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff38000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff22000000eeff0000ffffffffffff"},
            {0x44, 0x04, 9, "010600ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc0010000ffffffff41303030415059360000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
            {0x44, 0x04, 9, "0107000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004c6f636174696f6e3a3b4f70657261746f723a3b55736572496e666f313a3b55736572496e666f323a000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
            {0x44, 0x04, 9, "010800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000041303030433030305f3133303130315f4150593600000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffbc000000"},
            {0x44, 0x04, 10, "010900ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0000000000000000000000000000000000000000000000000000000000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff4000000090010000ffffffffffff"},
            {0x44, 0x04, 10, "010a00ffffffffffffffffffffffffffffffffffff99000000ffffffff00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff680100000100000041525249204c464f4720312e3738000000000000000000000000000000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff01000000417370656374526174696f3a312e37383a315f5363616c31303000ffffffffff64003900b906c803ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 10, "010b00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 10, "010c00ffff1800000001000000000000000000204000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff9000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000000000000000000000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 10, "010d00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 10, "010e00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 10, "010f00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 10, "011000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 11, "011100ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
            {0x44, 0x04, 11, "011200ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000000000000000000000000000000000000000000000000"},
        };
        std::vector<AncPacket> lp;
        for (const auto& h : kMiniLf) {
            AncPacket p; p.did = h.did; p.sdid = h.sdid; p.line = h.line;
            for (const char* c = h.hex; c[0] && c[1]; c += 2) p.data.push_back(uint8_t(std::stoul(std::string(c, 2), nullptr, 16)));
            lp.push_back(std::move(p));
        }
        CHECK(lp.size() == 18);
        MetadataProcessor ap;
        FrameMetadata m = ap.process(std::move(lp), tc, false, 2.0);
        CHECK(m.hasArri);
        const auto& a = m.arri;
        CHECK(a.cameraTypeName == "ALEXA Mini LF" && a.cameraModel == "ARRI ALEXA Mini LF" && a.cameraSerial == 33630);
        CHECK(a.cameraIndex == 'A' && a.supName == "7.03.00");
        CHECK(std::fabs(a.shutterAngle - 180.0) < 1e-9);   // sent as 180000 (1/1000 deg)
        CHECK(std::fabs(a.sensorFps - 25.0) < 1e-9 && a.exposureIndex == 800 && a.whiteBalanceK == 6000);
        CHECK(a.lensModel == "Canon K35 70 47531R" && std::fabs(a.focalLengthMm - 70.0) < 1e-9);
        CHECK(!a.focusValid && std::isnan(a.tStop));         // no lens encoders
        CHECK(a.reel() == "A000APY6" && a.clip() == "A000C000_130101_APY6");
        CHECK(a.hasTilt && std::fabs(a.tiltDeg - 3.4) < 1e-9 && a.hasRoll && std::fabs(a.rollDeg + 1.8) < 1e-9);
        CameraSample cs = sampleFrom(m);
        CHECK(cs.kind == CameraKind::Arri && cs.cameraModel == "ALEXA Mini LF" && cs.cameraSerial == "33630");
        CHECK(cs.hasFocal && !cs.hasFocus && !cs.hasIris && cs.hasShutter && cs.hasIso && cs.hasTilt && cs.hasRoll);
        CHECK(cs.hasCaptureRate && cs.captureRateNum == 25 && cs.captureRateDen == 1);
        {   // master record: new fields and provenance
            const MetaRecord& r = cs.record;
            CHECK(r.kind == CameraKind::Arri && r.str(Field::CameraIndex) == "A" && r.str(Field::CameraFirmware) == "7.03.00");
            CHECK(r.num(Field::ProjectFps) == 25.0 && r.get(Field::ProjectFps).origin == "arri.0x1A4");
            CHECK(std::fabs(r.num(Field::ExposureTime) - 0.02) < 1e-12 && r.get(Field::ExposureTime).confidence == Confidence::Inferred);
            CHECK(r.has(Field::Tint) && r.num(Field::Tint) == 0 && r.str(Field::LookName) == "ARRI 709");
            CHECK(r.num(Field::RecordingWidth) == 4448 && r.num(Field::RecordingHeight) == 3096 &&
                  r.get(Field::RecordingWidth).source == ValueSource::Derived);
            CHECK(r.str(Field::CameraTimecode) == "00:03:29:17" && r.str(Field::Reel) == "A000APY6");
            CHECK(!r.has(Field::FocusDistance) && !r.get(Field::FocusDistance).note.empty());
            CHECK(r.str(Field::Camera) == "ARRI ALEXA Mini LF" && r.str(Field::Lens) == "Canon K35 70 47531R");
        }
        CHECK(std::fabs(a.pixelAspect - 1.0) < 1e-9 && cs.hasSqueeze && std::fabs(cs.squeeze - 1.0) < 1e-9);
        // Sensor area derived from the recording area's aspect (1721x1198 = LF Open Gate)
        CHECK(a.activeImageW == 1721 && a.activeImageH == 1198);
        CHECK(cs.hasSensor && std::fabs(cs.sensorWidthMm - 36.696) < 1e-9 && std::fabs(cs.sensorHeightMm - 25.542) < 1e-9);
        CHECK(cs.sensorSource == "derived: LF Open Gate");
        {
            // Manual size: only fills in when missing unless forced
            OutputSettings os;
            os.manualSensorWidthMm = 24.0; os.manualSensorHeightMm = 13.5;
            CameraSample c2 = cs;
            applySensorOverride(c2, os);
            CHECK(c2.sensorSource == "derived: LF Open Gate");
            os.sensorOverride = SensorOverride::Always;
            applySensorOverride(c2, os);
            CHECK(c2.sensorSource == "manual" && c2.sensorWidthMm == 24.0);
        }
        // Ambiguous / unknown modes are not guessed
        CHECK(guessSensorMode("ALEXA 35", 16.0 / 9.0).candidates.size() == 3 && !guessSensorMode("ALEXA 35", 16.0 / 9.0).mode);
        CHECK(guessSensorMode("ALEXA Mini LF", 3840.0 / 2160.0).mode && std::string(guessSensorMode("ALEXA Mini LF", 16.0 / 9.0).mode->name) == "LF 16:9 UHD");
        CHECK(std::fabs(guessSensorMode("ALEXA 35", 4608.0 / 3164.0).mode->widthMm() - 27.9936) < 1e-9);
        CHECK(!guessSensorMode("ALEXA Mini", 1.5).mode);   // no table for S35 ALEV III bodies yet
        OpenTrackIOFrame of;
        of.includeStatic = true;
        cs.squeeze = 1.333;
        CHECK(buildOpenTrackIOJson(cs, OpenTrackIOConfig{}, of).find("\"anamorphicSqueeze\":{\"num\":1333,\"denom\":1000}") != std::string::npos);
        cs.squeeze = 2.0;
        CHECK(buildOpenTrackIOJson(cs, OpenTrackIOConfig{}, of).find("\"anamorphicSqueeze\":{\"num\":2,\"denom\":1}") != std::string::npos);
    }

    // VENICE 2 desqueeze steps from captures/venice2_desqueeze_2026-10-21_raw.txt: E10F area vs 3:2 sensor area
    {
        auto withArea = [&](uint32_t w, uint32_t h) {
            std::vector<AncPacket> vp;
            for (const auto& hp : buranoPackets()) vp.push_back(hp);  // any RDD 18 frame; values are patched below
            MetadataProcessor sp;
            FrameMetadata m = sp.process(std::move(vp), tc, false, 3.0);
            for (auto& set : m.rdd18.sets)
                for (auto& it : set.items) {
                    if (it.tag == 0xE10F) it.value = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                                      uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h)};
                    if (it.tag == 0x8104) it.value = {0x8C, 0x55};  // 35925 um
                    if (it.tag == 0x8105) it.value = {0x5D, 0x8E};  // 23950 um
                }
            return sampleFrom(m).squeeze;
        };
        CHECK(withArea(1620, 1080) == 1.0);
        CHECK(withArea(1920, 1024) == 1.25);
        CHECK(withArea(1920, 984) == 1.3);
        CHECK(withArea(1920, 854) == 1.5);
        CHECK(withArea(1920, 776) == 1.65);
        CHECK(withArea(1920, 712) == 1.8);
    }

    // Blackmagic URSA camera control (51/53), captured 2026-10-15 (captures/ursa_tests_2026-10-15_raw.txt)
    {
        auto bmdPacket = [](const char* hex) {
            AncPacket p; p.did = 0x51; p.sdid = 0x53; p.line = 16;
            for (const char* c = hex; c[0] && c[1]; c += 2) p.data.push_back(uint8_t(std::stoul(std::string(c, 2), nullptr, 16)));
            return p;
        };
        MetadataProcessor bp;
        FrameMetadata m = bp.process({bmdPacket("FF0800000904030208382507FF0800000905010200090103FF0600000906010200010000FF080000010B030228230000FF06000000028002C4070000FF080000010E030220030000FF0800000102020264190000FF0E000001090202180018008007380400000000FF0600000A00010202010000FF0A0000090002020000000002000000FF0C000009070202DE03A00F00000000FF0900000A0101020000400000000000FF0600000901010200010000FF0C0000090202020000C6FD00000000FF0500000107010201000000FF0800000303010201325A00FF0600000300020200000000FF060000010F010202010000")}, tc, false, 20.0);
        const auto& b = m.bmd;
        CHECK(b.present && b.commands.size() == 18);
        double v = 0;
        CHECK(b.fNumber(v) && std::fabs(v - 1.40) < 0.01);
        CHECK(!b.focusNormalised(v) && !b.zoomMm(v));
        CHECK(b.text(1, 14) == "800");
        CHECK(b.text(1, 2) == "6500 K, tint 0");
        CHECK(b.text(1, 11) == "90.00 deg");
        CHECK(b.text(1, 9) == "1920x1080  24 fps");
        CHECK(b.text(10, 0).rfind("ProRes", 0) == 0);
        CHECK(b.text(9, 4).size() == 11);
        int mode = -1;
        CHECK(b.transportMode(mode) && mode == 0);
        CHECK(m.rec == RecState::Stopped && m.recSource.rfind("Blackmagic", 0) == 0);
        CameraSample cs = sampleFrom(m);
        CHECK(std::string(cs.source) == "Blackmagic" && cs.hasIris && cs.irisKind == 'f' && !cs.hasFocus);
        CHECK(cs.record.num(Field::RecordingWidth) == 1920 && cs.record.num(Field::RecordingHeight) == 1080);
        CHECK(cs.record.str(Field::Codec).rfind("ProRes", 0) == 0 && cs.record.has(Field::Tint) && cs.record.str(Field::Camera) == "Blackmagic Design");
        // Registry sanity: every field has a unique id
        std::set<std::string> ids;
        for (const auto& f : allFields()) CHECK(ids.insert(f.id).second);
        CHECK(fieldById("focus") && fieldById("focus")->field == Field::FocusDistance && allFields().size() == kFieldCount);

        m = bp.process({bmdPacket("FF0800000904030208482607FF0800000905010200090103FF0600000906010200010000FF080000010B030228230000FF06000000028002C4070000FF080000010E030220030000FF0800000102020264190000FF0E000001090202180018008007380400000000FF0600000A00010202010000FF0A0000090002020000000002000000FF0C000009070202DE03A00F00000000FF0900000A0101020200400000000000FF0600000901010200010000FF0C0000090202020000C6FD00000000FF0500000107010201000000FF0800000303010201325A00FF0600000300020200000000FF060000010F010202010000")}, tc, false, 20.04);
        CHECK(m.bmd.transportMode(mode) && mode == 2 && m.rec == RecState::Recording);
        CHECK(m.bmd.text(10, 1).rfind("RECORD", 0) == 0);
        CHECK(JsonLogger::toJson(m, false).find("\"cmd\":\"10.1\"") != std::string::npos);
    }

    // Record state from running TC (Sony has no record flag). 50p: TC frame moves every other video frame.
    {
        MetadataProcessor rp;
        TimecodeInfo t; t.valid = true; t.h = 1;
        FrameMetadata m;
        for (int i = 0; i < 10; ++i) m = rp.process({}, t, false, 10.0 + i * 0.02);  // held, but not long enough yet
        CHECK(m.rec == RecState::Unknown);
        for (int i = 10; i < 40; ++i) m = rp.process({}, t, false, 10.0 + i * 0.02);  // held 0.8 s: rec-run proven
        CHECK(m.rec == RecState::Stopped);
        bool allRec = true;
        for (int i = 0; i < 50; ++i) {
            if (i % 2 == 0) t.f = uint8_t((t.f + 1) % 25);
            m = rp.process({}, t, false, 11.0 + i * 0.02);
            if (i > 1) allRec &= m.rec == RecState::Recording;
        }
        CHECK(allRec);
        for (int i = 0; i < 20; ++i) m = rp.process({}, t, false, 12.0 + i * 0.02);  // held again for 0.4 s
        CHECK(m.rec == RecState::Stopped);
        // Free-run TC (VENICE 2): always running, never proves rec-run, so not reported as recording.
        MetadataProcessor fr;
        TimecodeInfo ft; ft.valid = true; ft.h = 18;
        FrameMetadata fm;
        for (int i = 0; i < 100; ++i) { ft.f = uint8_t((ft.f + 1) % 25); fm = fr.process({}, ft, false, 30.0 + i * 0.04); }
        CHECK(fm.rec == RecState::Unknown);
        rp.setRecFromTimecode(false);
        CHECK(rp.process({}, t, false, 13.0).rec == RecState::Unknown);
    }

    // Sony record state from clip name events (BURANO sequence from captures/burano_rec_toggle_2026-10-06.txt)
    {
        MetadataProcessor cp;
        auto clipFrame = [&](const char* name, double t) {
            std::string n = name;
            std::vector<uint8_t> klv = {0x06, 0x0E, 0x2B, 0x34, 0x01, 0x01, 0x01, 0x06, 0x0E, 0x06, 0x01, 0x05, 0x01, 0, 0, 0,
                                        uint8_t(4 + n.size() + 1), 0x20, 0x20, 0x00, 0x01};
            klv.insert(klv.end(), n.begin(), n.end());
            klv.push_back(0);
            AncPacket p; p.did = 0x44; p.sdid = 0x04; p.line = 18;
            p.data = {0x02, 0x00, 0x01};
            p.data.insert(p.data.end(), klv.begin(), klv.end());
            TimecodeInfo none;
            return cp.process({p}, none, false, t);
        };
        CHECK(clipFrame("A002C003_261006NC", 1.0).rec == RecState::Unknown);
        FrameMetadata m = clipFrame("A002C003_261006WH", 1.1);  // REC on: suffix changes
        CHECK(m.sonyClip == "A002C003_261006WH" && m.rec == RecState::Recording && m.recSource == "Sony clip name change");
        CHECK(clipFrame("A002C003_261006WH", 1.2).rec == RecState::Recording);
        CHECK(clipFrame("A002C004_2610069I", 1.3).rec == RecState::Stopped);  // REC off: next clip number
    }

    // FreeD D1
    {
        CameraSample cs;
        cs.hasFocus = true; cs.focusM = 2.5;
        cs.hasFocal = true; cs.focalMm = 50.0;
        cs.hasIris = true; cs.iris = 2.8;
        cs.hasTilt = true; cs.tilt = -10.0;
        cs.hasRoll = true; cs.roll = 1.5;
        FreeDConfig fc;
        fc.cameraId = 7;
        auto pk = buildFreeD(cs, fc);
        auto u24 = [&](int o) { return (pk[o] << 16) | (pk[o + 1] << 8) | pk[o + 2]; };
        CHECK(pk[0] == 0xD1 && pk[1] == 7);
        CHECK(u24(2) == 0);                       // pan
        CHECK(u24(5) == 0xFB0000);                // tilt -10 * 32768, 24-bit two's complement
        CHECK(u24(8) == 49152);                   // roll 1.5 * 32768
        CHECK(u24(11) == 0 && u24(14) == 0 && u24(17) == 0);
        CHECK(u24(20) == 50000);                  // 50 mm in um
        CHECK(u24(23) == 2500);                   // 2500 mm
        CHECK(((pk[26] << 8) | pk[27]) == 280);   // T2.8 x 100
        unsigned sum = 0;
        for (int i = 0; i < 29; ++i) sum += pk[i];
        CHECK(sum % 256 == 0x40);                 // checksum: all 29 bytes sum to 0x40
        cs.focusInfinite = true;
        CHECK(buildFreeD(cs, fc)[23] == 0xFF);
    }

    // OSC
    {
        auto m = oscMessage("/camera/focus", 2.5f);
        const uint8_t expect[] = {'/', 'c', 'a', 'm', 'e', 'r', 'a', '/', 'f', 'o', 'c', 'u', 's', 0, 0, 0,
                                  ',', 'f', 0, 0, 0x40, 0x20, 0x00, 0x00};
        CHECK(m.size() == sizeof expect && std::memcmp(m.data(), expect, sizeof expect) == 0);
        CameraSample cs;
        using C = Confidence;
        cs.record.setNum(Field::FocusDistance, 1.0, C::Verified, "test");
        cs.record.setNum(Field::Iris, 4.0, C::Verified, "test");
        cs.record.setStr(Field::Timecode, "01:00:00:00", C::Verified, "test");
        cs.refresh();
        OscConfig oc;
        oc.focusUnit = FocusUnit::Millimetres;
        auto msgs = buildOsc(cs, oc);
        CHECK(msgs.size() == 3);                  // focus, iris, timecode (no zoom / orientation)
        CHECK(msgs.size() == 3 && msgs[0] == oscMessage("/camera/focus", 1000.0f));
        oc.bundle = true;
        auto b = buildOsc(cs, oc);
        CHECK(b.size() == 1 && b[0].size() > 16 && std::memcmp(b[0].data(), "#bundle", 8) == 0);
        cs.record = MetaRecord{};
        cs.refresh();
        CHECK(buildOsc(cs, oc).empty());
        cs.record.setStr(Field::Clip, "A002C003_261006E7", C::Verified, "test");
        cs.record.setNum(Field::Recording, 1, C::Verified, "test");
        cs.refresh();
        oc.bundle = false;
        auto cr = buildOsc(cs, oc);
        CHECK(cr.size() == 2 && cr[0] == oscMessage("/camera/recording", int32_t(1)) &&
              cr[1] == oscMessage("/camera/clip", std::string("A002C003_261006E7")));
        oc.disabled.insert("recording");
        CHECK(buildOsc(cs, oc).size() == 1);   // switched off by the user
        oc.disabled.clear();
        cs.record.clear(Field::Recording);
        CHECK(buildOsc(cs, oc).size() == 1);   // unknown record state is not sent
    }

    // OpenTrackIO framing: header layout and Fletcher-16 over header bytes 0-13 + payload
    {
        const std::string payload = "{\"protocol\":{\"name\":\"OpenTrackIO\",\"version\":[1,0,1]}}";
        auto pk = openTrackIOPackets(payload, 0x1234);
        CHECK(pk.size() == 1);
        const auto& p = pk[0];
        CHECK(p.size() == 16 + payload.size() && std::memcmp(p.data(), "OTrk", 4) == 0 && p[5] == 0x01);
        CHECK(p[6] == 0x12 && p[7] == 0x34 && p[8] == 0 && p[9] == 0 && p[10] == 0 && p[11] == 0);
        CHECK(((p[12] << 8) | p[13]) == (0x8000 | int(payload.size())));  // last segment + length
        std::vector<uint8_t> cover(p.begin(), p.begin() + 14);
        cover.insert(cover.end(), p.begin() + 16, p.end());
        CHECK(((p[14] << 8) | p[15]) == fletcher16(cover.data(), cover.size()));
        CHECK(std::string(p.begin() + 16, p.end()) == payload);
        // Large payloads are segmented with increasing offsets and only the last flagged
        auto big = openTrackIOPackets(std::string(40000, 'x'), 1);
        CHECK(big.size() == 2 && !(big[0][12] & 0x80) && (big[1][12] & 0x80) && big[1][10] == 0x7F && big[1][11] == 0xFF);
        CHECK(openTrackIOMulticast(7) == "239.135.1.7");
        const std::string u = randomUuidUrn();
        CHECK(u.size() == 45 && u.rfind("urn:uuid:", 0) == 0 && u[23] == '4');
    }

    // Settings round trip (string and file)
    {
        AppSettings a;
        a.deviceName = "UltraStudio Express Recorder 3G";
        a.panelWidth = 742.5f;
        a.showVideo = false;
        a.recFromTc = false;
        a.logPath = "/tmp/some log.jsonl";
        a.outputs.freedEnabled = true;
        a.outputs.freedHost = "192.168.10.2";
        a.outputs.freedPort = 40001;
        a.outputs.freed.cameraId = 3;
        a.outputs.freed.zoomCountsPerMm = 123.25;
        a.outputs.oscEnabled = true;
        a.outputs.oscHost = "192.168.10.3";
        a.outputs.osc.prefix = "/cam/a";
        a.outputs.osc.focusUnit = FocusUnit::Feet;
        a.outputs.osc.disabled = {"umid", "camera_serial"};
        a.outputs.otioEnabled = true;
        a.outputs.otioMulticast = false;
        a.outputs.otioHost = "10.0.0.9";
        a.outputs.otio.sourceNumber = 42;
        a.outputs.otio.includeTransform = true;
        AppSettings b;
        settingsFromString(settingsToString(a), b);
        CHECK(settingsToString(a) == settingsToString(b));
        CHECK(b.deviceName == a.deviceName && b.panelWidth == 742.5f && !b.showVideo && !b.recFromTc && b.logPath == a.logPath);
        CHECK(b.outputs.freedHost == "192.168.10.2" && b.outputs.freedPort == 40001 && b.outputs.freed.cameraId == 3);
        CHECK(b.outputs.osc.prefix == "/cam/a" && b.outputs.osc.focusUnit == FocusUnit::Feet && b.outputs.osc.disabled.size() == 2);
        CHECK(!b.outputs.otioMulticast && b.outputs.otioHost == "10.0.0.9" && b.outputs.otio.sourceNumber == 42);
        // Missing / garbage values keep the defaults
        AppSettings c;
        settingsFromString("freed_port=abc\notio_source_number=999\nunknown=1\n", c);
        CHECK(c.outputs.freedPort == 40000 && c.outputs.otio.sourceNumber == 200);
        const std::string path = (std::filesystem::temp_directory_path() / "sdimeta_test" / "settings.ini").string();
        CHECK(saveSettings(path, a));
        AppSettings d;
        CHECK(loadSettings(path, d) && settingsToString(d) == settingsToString(a));
        std::filesystem::remove_all(std::filesystem::path(path).parent_path());
        AppSettings e;
        CHECK(!loadSettings(path, e));
    }

    // JSON sanity
    md = proc.process(makeArriPackets(blk, 1), tc, false, 1.24);
    std::string j = JsonLogger::toJson(md, true);
    CHECK(j.find("\"reel\":\"A_0001_1234\"") != std::string::npos);
    CHECK(j.find("\"focus_mm\":\"inf\"") != std::string::npos);
    std::printf("%s\n", j.substr(0, 400).c_str());

    std::printf(g_fail ? "%d FAILED\n" : "All parser tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
