# SDI Metadata Viewer: project context

## Goal
Capture per-frame camera metadata (focus/iris/zoom, timecode, record state, reel/clip names) from the
camera's SDI feed on a Blackmagic DeckLink card, for use in Stage Precision (SP) and Unreal Engine.
The SDI feed already goes into SP for lens calibration, so the aim is to get FIZ from that same SDI signal
rather than running extra data cables from the lens control system.

Target cameras: all ARRI (ALEXA 35, Mini LF, LF, Mini, SXT/XT, AMIRA, 65, 265), Sony VENICE, VENICE 2, BURANO,
and Blackmagic URSA.

## Background decisions
- There is no off-the-shelf SDI ANC -> Live Link/FreeD bridge, hence this app.
- SP support for decoding lens data from SDI ANC was not confirmed; worth asking SP support.
- The camera only embeds FIZ it knows about: LDS or /i lenses, or LBUS/LCS motors/encoders. A Preston
  driving a non-smart lens does NOT put FIZ into the SDI.
- Alternatives considered: stYpe RedSpy lens encoders (RedSpy natively reads only Canon/Fujinon digital
  lenses or its own encoders; no Preston input), DCS LDT, the ARRI Live Link plugin (network, not SDI),
  and Unreal's Preston MDR plugin (needs a serial cable).
- A Preston -> Canon-protocol converter was ruled out: the Canon RS-422 protocol is undocumented.

## Architecture
- `src/anc/`: SDK-free parsing, unit-tested (`tests/test_parsers.cpp`).
  - `Rp214`: reassembles KLV from segmented ANC (DID 0x44/0x04, UDW = MID(1) PSC(2) payload).
  - `ArriMetadata`: decodes the 4096-byte ARRI block (ArriFileV3 layout; read little-endian).
  - `Rdd18`: Sony RDD 18 decoder for DID 0x43/0x05. Each packet = sequence byte + slice of a stream of KLV local sets
    (2-byte tag, 2-byte length; set byte 01 lens, 02 camera, 7F Sony user-defined); sets span packets. Verified on a
    BURANO capture (`captures/burano_2026-10-06_probe.txt`, also the test fixture). Sony clip name arrives as a private
    KLV in 44/04 (key prefix 06 0E 2B 34 01 01 01 06 0E 06 01 05 01).
  - `BmdCameraControl`: Blackmagic SDI Camera Control Protocol (51/53) + tally (51/52). The URSA reports its own
    settings on SDI out in this format (operation byte 2). Verified on an URSA 2026-10-15
    (`captures/ursa_tests_2026-10-15_raw.txt`): iris 0.2, ISO, WB, shutter, format, codec, transport mode 10.1
    (0 preview / 2 record: a real record flag), undocumented category 9 (9.4 = BCD timecode). It sent no focus
    (0.0, normalised only, not a distance), zoom (0.7) or clip name; tally byte stayed 0 while recording.
  - `MetadataProcessor`: per-frame decode, record state (ARRI flag > Blackmagic transport > Sony UMID > rec-run TC >
    Sony clip-name events).
- `src/meta/`: the master metadata format (see docs/METADATA.md).
  - `Fields.def`: field registry (id = OSC suffix / JSON key, group, unit, type, Livegrade / OpenTrackIO / FreeD
    equivalents). Add fields here; ids are stable.
  - `MetaRecord`: one value per field with provenance (source camera/derived/manual, confidence verified/documented/
    inferred, origin like `arri.0x045C`, note when missing).
  - `Adapters`: per-manufacturer detect() + fill() (ARRI, Sony RDD 18, Blackmagic) -> MetaRecord; `buildRecord()`.
    New makes (Canon, RED ...) = decoder in src/anc + adapter + capture + coverage column.
  - `sdimeta_coverage` (src/tools/Coverage.cpp) regenerates docs/fields.json and docs/COVERAGE.md from captures.
- `src/out/`: SDK-free outputs. `CameraSample` is now a typed view generated from MetaRecord (refresh()).
  - `CameraSample`: camera-agnostic values + autodetected `CameraKind` (ARRI / Sony / Blackmagic), incl. make/model/
    serial/firmware, FIZ, tilt, sensor size, ISO, shutter, WB, capture rate, clip/reel/UMID, record state.
    `catalogue()` lists every output variable (id = OSC address suffix) with availability; Summary and the OSC
    table use it.
  - `Protocols`: FreeD D1 (configurable focus/zoom count scaling, iris in spare bytes); OSC 1.0 (one message per
    catalogue variable, per-variable on/off); OpenTrackIO 1.0.1 (JSON sample + 16-byte "OTrk" header, Fletcher-16
    over header bytes 0-13 + payload, segmentation, multicast 239.135.1.<source number>:55555, static block every
    2 s). OpenTrackIO output validated against the official schema (2026-10-21). Transform (tilt only) is opt-in
    because zero pan/position would override a real tracker; focal length goes out as pinholeFocalLength (nominal).
  - `Settings`: all UI settings (outputs incl. OSC variable choices, log, device, panel width) as key=value in
    ~/Library/Application Support/SdiMetaViewer/settings.ini (macOS) / %APPDATA%\SdiMetaViewer (Windows) /
    ~/.config/sdimeta (Linux). Saved on Apply, Start capture, splitter release and exit (incl. Ctrl-C); restored and
    applied at launch, so enabled outputs start sending immediately.
  - `UdpSender` (POSIX + Winsock), `OutputManager` (sends on every captured frame; needs the video rate for timing).
  - macOS asks for Local Network permission the first time the app sends; sends fail silently until granted.
- `src/log/JsonLogger`: JSON Lines, one object per frame, with optional raw ANC hex.
- `src/capture/`: DeckLink enumeration, input callback, platform shims (BSTR/CFStringRef/char*, COM vs dispatch).
- `tools/capture.sh <camera> <test> [secs] [probe args]`: one-command captures for docs/TEST_PLAN.md.
- `src/tools/Probe.cpp` (`sdimeta_probe [secs] [device] [--dump] [--watch] [--raw] [--list] [--hanc 524D,...]`; `--watch` prints every decoded
  change with TC, `--raw` every packet of every frame as hex): headless check of ANC packets, every TC source,
  and raw VANC lines (zero vs black vs ADF), with optional payload hex dump.
- `src/main.cpp`: GLFW + Dear ImGui UI. Tabs: Summary (catalogue), Input (decoded fields of the detected camera,
  TC/record state), Output (FreeD | OSC | OpenTrackIO | Log file sub-tabs, one Apply), Raw (ANC packets, hex, RDD 18
  stream). Preview frames are converted to BGRA with IDeckLinkVideoConversion
  and drawn as a GL texture. The SDK 12 GL preview helper was avoided because it is fixed-function and
  incompatible with the GL 3.2 core context macOS requires.

## Key spec facts (ARRI SDI Metadata white paper, July 2023)
https://www.arri.com/resource/blob/327826/a17ae3b8bf7e8d487d6d01648bdf432e/arri-cameras-sdi-metadata-white-paper-data.pdf
- Metadata key 06 0E 2B 34 02 05 01 0D 0E 17 00 00 00 11 01 01; LUT key has 0xA1 at K14. BER length 83 00 10 00.
- 18 packets x 230-byte payload (DC=233), VANC lines 9-11 by default (user-configurable).
- Record flag: HANC, DID 0x52 / SDID 0x4D, DC=11, word 11 bit 1 (1 = recording).
- Lens offsets: unit 0x374, focus 0x378 (1/1000 in or mm; 0 invalid, -1 infinite), focal 0x37C (1/1000 mm),
  iris 0x384 (T = 2^((v/1000 - 1)/2); -1/-2/-3 = invalid/closed/near closed), raw encoders 0x3B8-0x3C2,
  lag 0x3DC-0x3DF, effective focal length 0x3E4 (um), entrance pupil 0x3E8 (um).
- Clip: reel 0x6B0 (11 chars), clip 0x6BB (34 chars); older reel 0x4F8, older clip 0x698.

## Known open issues
1. ARRI record flag (HANC 52/4D): SDK 16 can capture HANC (BMDDeckLinkSupportsHANCInput + up to 4 DID/SDID filters,
   bmdDeckLinkConfigHANCInputFilter1..4, packets then come through the normal iterator). The app requests 0x524D.
   The UltraStudio Express Recorder 3G does NOT support HANC input, so on it ARRI record state is only available
   with the camera in Rec Run TC (rec-run detection). A HANC-capable DeckLink would give the real flag.
1b. ALEXA Mini LF on hardware (SUP 7.03.00, 2026-10-21, `captures/minilf_2026-10-21_probe.txt` = test fixture,
   `captures/minilf_rec_2026-10-21_raw.txt`): 18 x 44/04 on lines 9-11 as documented; little-endian confirmed
   (magic "ARRI", 0x12345678). Differences from the white paper: shutter 0x190 is 1/1000 deg (180000), project
   fps 0x1A4 is fps x1000; new reel/clip fields (0x6B0/0x6BB) are 0xFF, old ones (0x4F8/0x698) used. Tilt 0x45C /
   roll 0x460 = signed 16-bit in the low half (0xFFFFFFFF = n/a, pan always n/a), tenths of a degree: roll verified
   with a phone inclinometer (20 deg clockwise from behind = 205; + = clockwise, same as Sony). No sensor physical size
   on SDI (IDI fields describe the SDI output image), and the Active Image (IDI06, 0x2C/0x30) does NOT identify the recording
   mode: Open Gate gave 1721x1198 (its own shape) but LF 2.39:1 4.5K gave 3840x2160 (the SDI frame), 2026-10-22. So
   there's no auto-derivation: ARRI sensor size comes from the manual setting (Output tab: off / when missing /
   always), with presets in `SensorModes` (photosites x pitch from ARRI format data; LF/Mini LF 8.25 um, ALEXA 35
   6.075 um). The SDI image doesn't reveal crop modes either (the recorded area is scaled to fill it). Frame line file name e.g.
   "ARRI LFOG 1.78" hints the format but is user-named. UMID 0x27C all FF.
   Pixel aspect / anamorphic squeeze (ICI18, 0x0C4) is an IEEE float: 0x3F800000 = 1.0 with a spherical lens;
   anamorphic values (1.25..2.0) not yet seen on hardware. Output as OSC /squeeze and OpenTrackIO
   static.camera.anamorphicSqueeze. Sony / Blackmagic squeeze not identified yet (toggle-test the desqueeze setting).
   Lens data: a manually selected lens-table entry (e.g. "Canon K35 70") overrides the lens's own data (only name,
   serial, focal; focus/iris invalid). With the table cleared, a ZEISS Supreme Prime 50/T1.5 (same serial 50128435 as
   seen on the VENICE 2) gave focus 0x378 = 1108 mm, iris 0x384 = 3475 -> T2.36, focal 50 mm; LDS lag type 2
   (unknown). Raw LDS/motor encoders stay 0/65535 with /i-type lenses.
   REC: clip counter (0x698) increments at REC START (briefly showing reel A000 for ~0.2 s first); nothing changes at
   REC stop; ARRI's clip name is the current / last recorded clip (Sony's is the next clip when stopped). The app
   does not infer record state from this (no stop event).
2. Sony lens values: distances/focal lengths (8001, 8002, 8004, 8005) decode as 4-bit signed power-of-ten exponent +
   12-bit mantissa in metres, iris (8000, 8008) as F = 2^(8(1 - n/65536)). Checked against an 85mm f/1.4 on BURANO
   (85.0 mm, 89.2 mm 35mm-equiv, f/1.40) but focus distance is still to be confirmed against tape-measured marks, and
   infinity / near-limit encodings are unknown. Ring positions (8009-800B) and many 81xx/E1xx/84xx tags are unnamed.
2a. VENICE 2 (MPC-3626, fw 3.00, 2026-10-21, `captures/venice2_2026-10-21_probe.txt`, also a test fixture): same
   RDD 18 layout as BURANO (7 packets on lines 11-12, clip KLV on 18, VITC + LTC). With a ZEISS Supreme Prime 50mm
   T1.5 (lens attributes "Z50128435"; data via the PL mount contacts only, lens's external data port unused) it sent focus 8001 = 9.89 m, focal 50 mm, F 8000 +
   T 8008. A third 7F set carries E201/E215: Cooke /i-format lens data (ZEISS eXtended Data), ending 0A 0D.
   Focus rack 434 mm .. far end (`captures/venice2_focus_rack_2026-10-21_raw.txt`, `--raw` format):
   - E201: bytes are 0x40 + 6-bit groups. Bytes 1-4 = focus distance in mm (matches Sony 8001 exactly on 412/522
     moving frames; diverges only on fast moves at long range). Bytes 15-18 / 19-22 = depth of field near / far (mm,
     far 9999999 = infinity). Bytes 5-6 probably aperture x100 (152 at T1.5). Then "Z50128435" (lens serial) ASCII.
   - E215: /i-encoded header (repeats focus etc.), then 14 plain big-endian int16: two 7-point curves that vary
     smoothly with focus. First starts 1000,1000 and falls (e.g. 863..204 at far, 958..370 at close focus): looks like
     relative illumination (shading) x1000. Second starts 0,0 and goes negative, larger at close focus
     (-155 far, -247 close): looks like radial distortion. Radii / units unknown without the ZEISS eXtended Data spec.
   - Lens at the far end reported 664000 mm (Sony 8001 too), not a special infinity code.
   This confirms the RDD 18 focus decode (open issue 2) against an independent source. Only one of the
   VENICE 2's SDI outputs carried ANC; the other (1080PsF25) had none.
   Anamorphic squeeze: no direct field; 7F/E10F is the desqueezed image area on the SDI output (rational w/h px), so
   squeeze = (E10F aspect) / (8104/8105 sensor aspect), snapped to standard values within 0.01. Verified stepping
   1.0/1.25/1.3/1.5/1.65/1.8 on a 3:2 area (`captures/venice2_desqueeze_2026-10-21_raw.txt`). E101/E102 are the
   frame lines rescaled with it.
2b. Sony record state, VENICE 2 (best signal, `captures/venice2_rec_raw_2026-10-21.txt`): the 44/04 clip packet is
   73 bytes; after the clip-name KLV come 32 bytes that hold a SMPTE basic UMID (06 0A 2B 34 01 01 01 05 01 01 0D 43,
   13, instance 00 00 00, 16-byte material number, new per take) while RECORDING and are all zero while stopped.
   `sonyRecordingFromClipPacket` uses it: a per-frame state, correct from the first frame. Replaying the capture gives
   exactly the 4 REC transitions. The UMID should match the recorded file's UMID (unverified); it is shown and logged.
   Timecode user bits / flags did not change with REC.
   BURANO sends no record flag in RDD 18 (verified over 4 REC cycles,
   `captures/burano_rec_toggle_2026-10-06.txt`). With the camera in rec-run TC, RP188 TC runs only while recording;
   The 44/04 clip name is the clip being recorded while rolling, and the NEXT clip number once stopped; its 2-char
   suffix changes at REC start. VENICE 2 in free-run TC (`captures/venice2_rec_toggle_2026-10-21.txt`): no rec flag
   either, but the clip name changes on the exact frame of REC on (suffix) and REC off (clip number). So
   `MetadataProcessor` uses running TC only after TC has been seen holding for 0.5 s (proves rec-run; free-run
   previously read as "recording"), else clip name events, else Unknown (until the first REC press in free-run).
   Sony 7F/8414 = CAMERA TILT: signed int32, value / 36000 = degrees (+ up, 0 = level, 0.1 deg steps). Verified on
   VENICE 2 2026-10-21 with a phone inclinometer: 20 up = 208.5 / 197.7, 20 down = -205.7, level ~1.4 (x3600), and
   panning 90 deg left it unchanged. It swings +-20 deg during fast whip pans (gravity sensor under acceleration).
   7F/8415 = CAMERA ROLL, same units, + = clockwise seen from behind the camera (right side down). Verified hand-held:
   level 0.0, ~20 deg clockwise +17.7, anticlockwise -16.1 / -15.8, with tilt unchanged. Both go to FreeD, OSC and
   (opt-in) the OpenTrackIO transform. Neither field changes with pan; no pan source exists.
   Receivers differ: Stage Precision needs tilt AND roll inverted (seen on the Mini LF over OSC, 2026-10-22), so each
   output (FreeD / OSC / OpenTrackIO) has its own invert pan / tilt / roll flags (`OrientationSigns`), saved in settings.
3. (resolved) ARRI little-endian confirmed on a Mini LF.
4. Hardware so far: UltraStudio Express Recorder 3G on macOS (SDK 16.0) captures VANC fine; Sony BURANO decoded.
   ARRI is still untested on hardware. Builds against SDK 12 (Linux) and SDK 16 (macOS).

## Next steps
- Anamorphic desqueeze for every camera (parity with ARRI, which already decodes it from 0x0C4):
  - Sony: DONE for VENICE 2 (see 2a). Check BURANO / VENICE 1 the same way, and check what E10F does if the SDI
    output is set not to display desqueezed.
  - Blackmagic URSA: record `sdimeta_probe --raw` while switching its desqueeze setting and diff the commands.
  - ARRI: confirm an anamorphic value (e.g. 2.0x) arrives as the expected float.
  - Consider a manual squeeze override in the Output tab for cameras that don't report it.
- ARRI record state: test Rec Run TC on the Mini LF; test a HANC-capable DeckLink for the 52/4D flag. Test other
  ARRI bodies (ALEXA 35 uses the new reel/clip fields).
- Output tab sends FreeD, OSC and OpenTrackIO; test against Stage Precision and Unreal (FreeD Live Link source,
  OpenTrackIO), and agree the FreeD zoom/focus count scaling with the receiver.
- Consider a Live Link source for Unreal.
- Decode ZEISS eXtended Data (Sony 7F E201/E215) once the ZEISS / Cooke /i spec is obtained.
- Consider a headless/CLI mode for use alongside SP.

## Build
```bash
cmake -S . -B build -DDECKLINK_SDK_DIR="/path/to/Blackmagic DeckLink SDK"   # folder containing Mac/ Win/ Linux/
cmake --build build -j
./build/SdiMetaViewer        # Windows: build\Release\SdiMetaViewer.exe (build from the x64 Native Tools prompt)
# Tests only, no SDK needed:
cmake -S . -B build -DSDIMETA_BUILD_APP=OFF && cmake --build build && ctest --test-dir build
```
Only one app can hold a DeckLink input, so stop SP's capture on that port or use an SDI split.
