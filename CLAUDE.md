# Metadork (formerly SDI Metadata Viewer): project context

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
- Exception for Sony VENICE / VENICE 2: ARRI's Cable CAM (7p) - ENG (12p) (K2.0015759) from an ARRI RIA-1 / cforce RF
  into the VENICE's 12-pin Hirose LENS port injects lens metadata from the ARRI motors' lens map (ARRI RIA-1 cable guide;
  per the vendor listings it needs separate power for the lens functions). Untested here: check whether it then shows
  in RDD 18 (8001 etc.). The Wooden Camera LPL mount + DCS LDT is the other route.
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
  - `MetadataProcessor`: per-frame decode, record state (HANC 52/4D flag (ARRI, VENICE 2) > RED user-bit flag > TC recording trigger >
    Blackmagic transport > Sony UMID > rec-run TC > Sony clip-name events).
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
    ~/Library/Application Support/Metadork/settings.ini (macOS) / %APPDATA%\Metadork (Windows) /
    ~/.config/metadork (Linux). Saved on any output change, port change, splitter release and exit (incl. Ctrl-C); restored and
    applied at launch, so enabled outputs start sending immediately.
  - `UdpSender` (POSIX + Winsock), `OutputManager` (sends on every captured frame; needs the video rate for timing).
  - macOS asks for Local Network permission the first time the app sends; sends fail silently until granted.
- `src/anc/CaptureReplay`: replays a `sdimeta_probe --raw` capture (packets + TC user bits / flags) through a fresh
  MetadataProcessor at the recorded timing, looping; app "Replay a capture file" section, drag-and-drop, `--replay <file>`.
  Replay feeds the live outputs too.
- App `--screenshots <dir> [--screenshot-prefix p]`: default settings (outputs to 127.0.0.1), renders every tab, saves
  TGAs, quits. `tools/screenshots.sh` regenerates docs/images for docs/MANUAL.md from sample captures.
- MIT licence (LICENSE). Release: `third_party/decklink` = SDK 16.0 include folders (EULA clause 0.1 exempts them; default DECKLINK_SDK_DIR).
  macOS builds a .app (packaging/macos/Info.plist.in, NSLocalNetworkUsageDescription); Windows static CRT, no console.
  Relative log paths resolve to ~/Documents (`resolveUserPath`). `.github/workflows/build.yml`: macOS universal +
  Windows build/test/zip on every push; tag v* publishes a GitHub release (zips include packaging/samples.txt captures).
- `src/log/JsonLogger`: JSON Lines, one object per frame, with optional raw ANC hex.
- `src/capture/`: DeckLink enumeration, input callback, platform shims (BSTR/CFStringRef/char*, COM vs dispatch).
- `tools/capture.sh <camera> <test> [secs] [probe args]`: one-command captures for docs/TEST_PLAN.md.
- `src/tools/Probe.cpp` (`sdimeta_probe [secs] [device] [--dump] [--watch] [--raw] [--list] [--hanc 524D,...]`; `--watch` prints every decoded
  change with TC, `--raw` every packet of every frame as hex): headless check of ANC packets, every TC source,
  and raw VANC lines (zero vs black vs ADF), with optional payload hex dump.
- Multiple feeds (`Feed` in main.cpp): each input has its own CaptureSession / CaptureReplay, Shared state, OutputManager,
  JsonLogger and Preview; tiles stacked on the left, click selects the feed shown on the right. Feed name = OSC prefix
  (`FeedSettings::name()` <- osc.prefix). Settings: feed 1 = original unprefixed keys (old files and versions keep
  working), feeds 2+ = `feedN.` keys + `feed_count`. `newFeedSettings` copies a feed with the next camera name, FreeD
  camera ID, OpenTrackIO source number and log file. A port can be captured by only one feed. No Start/Stop: choosing a
  port captures from it (None stops); a per-frame reconcile opens each feed's port at launch / after changes and retries
  failures every 3 s; a running replay pauses the feed's live input.
  Feeds panel: column or grid (2 across, `feed_layout`), sized at launch / on toggle so 4 feeds fit; double-click solos a feed.
- `src/main.cpp`: GLFW + Dear ImGui UI. Right panel tabs: Summary (catalogue), Input (decoded fields of the detected camera,
  TC/record state), Output ("Values the camera doesn't send" section shown only when needed: RED position letter, sensor size; then FreeD | OSC | OpenTrackIO | Log file sub-tabs; changes apply automatically once no widget is active, so typed fields apply on leaving them), Raw (ANC packets, hex, RDD 18
  stream). Preview: 10-bit YUV (v210) frames are uploaded as an RGB10_A2 texture and unpacked +
  colour-converted (601/709/2020 -> 709) in a GLSL 150 shader into an FBO texture, every frame (GL 2+ entry points loaded
  with glfwGetProcAddress). Other formats fall back to IDeckLinkVideoConversion at ~10 fps (CPU; ~40 ms per UHD frame). The SDK 12 GL preview helper was avoided because it is fixed-function and
  incompatible with the GL 3.2 core context macOS requires.
  DPI: on Windows / Linux the font (ProggyClean at 13 px x content scale) and style are scaled by the monitor content
  scale (`applyUiScale`, rebuilt when the window changes monitor); fixed pixel sizes go through `px()`. macOS stays at 1
  (points; Retina via the framebuffer scale).

## Key spec facts (ARRI SDI Metadata white paper, July 2023)
https://www.arri.com/resource/blob/327826/a17ae3b8bf7e8d487d6d01648bdf432e/arri-cameras-sdi-metadata-white-paper-data.pdf
- Metadata key 06 0E 2B 34 02 05 01 0D 0E 17 00 00 00 11 01 01; LUT key has 0xA1 at K14. BER length 83 00 10 00.
- 18 packets x 230-byte payload (DC=233), VANC lines 9-11 by default (user-configurable).
- Record flag: HANC, DID 0x52 / SDID 0x4D, DC=11, word 11 bit 1 (1 = recording).
- Lens offsets: unit 0x374, focus 0x378 (spec: 1/1000 in or mm; in practice always mm, see 1b; 0 invalid, -1 infinite), focal 0x37C (1/1000 mm),
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
   with a phone inclinometer (20 deg clockwise from behind = 205; + = clockwise, same as Sony). Tilt checked 2026-10-22 (`captures/minilf_T8b-tilt-circle_2026-10-22_watch.txt`): phone +-20 deg read +24 / -18 (+ = up), consistent with a ~3 deg offset between the phone's resting surface and the camera's sensor axis. Scene 0x500 / take 0x510 verified (ASCII); circle take 0x4F4 untested. No sensor physical size
   on SDI (IDI fields describe the SDI output image), and the Active Image (IDI06, 0x2C/0x30) does NOT identify the recording
   mode on its own: Open Gate gave 1721x1198 (its own shape) but LF 2.39:1 4.5K gave 3840x2160 (the SDI frame), 2026-10-22.
   `SensorModes` is a table of every ARRI body's modes from ARRI's format data (ALEXA 35, Mini LF, LF, 265, 65, Mini,
   SXT, AMIRA; photosites x pitch; XT / classic, ALEXA 35 SUP 2+ and AMIRA S16 not yet in it). The ARRI adapter
   recognises the mode (sensor_mode + sensor size, derived / inferred) only when the active image is boxed inside the
   SDI frame (IDI02/03) AND its aspect (or aspect / squeeze) matches exactly one mode of that body; otherwise a note
   lists the candidates and the manual setting (Output tab: off / when missing / always, presets) is used. ALEXA 265
   (camera type 11) gave 1744x824 = 6.5K 2.12:1 Open Gate on 2026-10-27, but 1920x1080 on 2026-10-26 (no guess). ALEXA 265
   (SUP 5.02, Panavision UP2 50) sends lens distance unit 0x374 = 0 ("inch") but focus 0x378 in mm: camera HUD 14'6" =
   raw 4420 (2026-11-10); the Mini LF sends unit 1 + mm. So the unit field looks like the display unit and the decoder
   reads ARRI focus as mm always (no body seen sending inches). Not yet confirmed by switching a camera's unit setting. The SDI image doesn't reveal crop modes either (the recorded area is scaled to fill it). Frame line file name e.g.
   "ARRI LFOG 1.78" hints the format but is user-named. UMID 0x27C all FF.
   Pixel aspect / anamorphic squeeze (ICI18, 0x0C4) is an IEEE float: 0x3F800000 = 1.0 with a spherical lens;
   anamorphic values (1.25..2.0) not yet seen on hardware. Output as OSC /squeeze and OpenTrackIO
   static.camera.anamorphicSqueeze. Sony / Blackmagic squeeze not identified yet (toggle-test the desqueeze setting).
   Lens data: a manually selected lens-table entry (e.g. "Canon K35 70") overrides the lens's own data (only name,
   serial, focal; focus/iris invalid). With the table cleared, a ZEISS Supreme Prime 50/T1.5 (same serial 50128435 as
   seen on the VENICE 2) gave focus 0x378 = 1108 mm, iris 0x384 = 3475 -> T2.36, focal 50 mm; LDS lag type 2
   (unknown). Raw LDS/motor encoders stay 0/65535 with /i-type lenses.
   Incomplete payloads exist: 2026-10-28 an on-set feed (DIT confirmed the payload was incomplete) carried a valid
   18-packet block with only camera letter, take (0x510), clips (0x698 / 0x6BB) and HANC rec flag; camera type,
   model, lens, exposure, image fields and camera TC all zero. Tilt / roll are ignored when camera type is 0.
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
   HANC 52/4D (DeckLink 8K Pro, 2026-10-22, `captures/venice2_V2-1-hanc_2026-10-22_raw.txt`): VENICE 2 sends the same
   11-byte record flag as ARRI (01 00 0F 01 00 00 01 00 00 20 0x, last byte bit 1 = REC), so the app uses it first. It
   leads the UMID by ~0.6-0.8 s at REC on and off (button press vs file). Not sent before the first REC press here.
   ND (2026-10-22, `captures/venice2_V2-2-nd_2026-10-22_raw.txt`): 02/8103 = attenuation factor 2^stops (1 clear, 2 = 0.3
   ... 256 = 2.4); nd = 0.3 x log2(value). BURANO sends 6 (variable ND ~0.78), unverified.
   EI (`captures/venice2_V2-3-ei_2026-10-22_raw.txt`): 02/810B and 02/8115 always equal = exposure index (seen 400..3200 in
   1/3-stop steps). Switching base ISO 800 <-> 3200 changed no other tag: base ISO is not sent by the VENICE 2.
   Shutter (`captures/venice2_V2-5-shutter_2026-10-22_raw.txt`, 25 fps): 8108 = angle x60 and 8109 = exposure time (rational)
   are both filled and consistent in angle mode (180/173/144/90 -> 1/50..1/100) and speed mode (1/25..1/100 -> 360..90 deg);
   no tag says which mode is selected.
   Imager modes (`captures/venice2_V2-7-imager_2026-10-22_raw.txt`): 8104/8105 (um) follow the mode: 35.925x23.950 (3:2 FF),
   35.961x19.436, 35.925x15.064 (2.39), 24.330x12.830 (S35 17:9-ish), 22.810x12.830 (S35 16:9). Lens 8004 (35mm-equiv
   focal) scales with the sensor diagonal (99 -> 105 -> 110 -> 156 mm with a 99 mm lens, 8005 unchanged), so it is a
   diagonal crop-factor equivalent. E10F (SDI image area) changes with it. No recording-resolution tag found.
   Squeeze 2.0x confirmed 2026-10-22 (`captures/venice2_V2-6-squeeze_2026-10-22_raw.txt`: E10F 1920x640 on a 3:2 area);
   no separate SDI-output desqueeze display setting was found on this camera.
   Look (`captures/venice2_V2-9-look_2026-10-22_raw.txt`): 7F/E115 = look applied to the SDI output (1/0). 02/3210 and 3219
   follow the SDI picture: BT.709 transfer + primaries with the look on, Sony S-Log3 / S-Gamut3.Cine ULs without; 02/8120
   = 06 / 02 alongside. 7F/E109 (= E113) is the look name ("07_TY_3 STRIP_JP.cube" -> "s709"). No ASC CDL on this camera.
   Stage Precision naming (2026-10-22): VENICE 2 takes named correctly in SP via OSC /camera/recording_name (and /clip).
   Use recording_name: /clip can flip to the next clip number on the REC-off frame in free-run TC.
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
   Both cameras SEND + = tilt up, + = roll clockwise from behind. Stage Precision needs roll the other way, tilt not (checked
   on VENICE 2 and Mini LF over OSC, 2026-10-22), so since 2026-10-28 the adapters negate roll: every output sends roll
   + anticlockwise from behind by default (field label says so). Each output (FreeD / OSC / OpenTrackIO) still has its own
   invert pan / tilt / roll flags (`OrientationSigns`); settings files without roll_convention=2 have invert-roll dropped.
2c. RED V-RAPTOR 8K VV (2026-10-22, 8K Pro, 2160p25, `captures/red_raptor_RD-1-rec_2026-10-22_raw.txt`): no VANC/HANC
   camera metadata packets, only ATC timecode (60/60, LTC line 10 + VITC2 line 571, identical). RED packs clip name +
   record flag into the 32 timecode user bits, alternating two blocks per frame (RED doc 910-0084 "HD-SDI HANC Metadata
   Information", written for RED One / DSMC / DSMC2, matches the V-RAPTOR): `src/anc/RedHanc` decodes camera letter, reel,
   clip, date, 2-char wildcard, camera position and the RS record flag. Verified: R003_C007_102263, C008_1022NE,
   C009_1022RG; RS set exactly during both takes. While stopped the clip fields show the NEXT clip (~1.5 s after REC off),
   like Sony. V-RAPTOR names clips <CamID><reel>_<Camera Position letter><clip> (R003_A012), not RED's documented
   fixed "C" (confirmed: Camera Position B gave R003_B0xx); the position letter is not in the user bits (changing it only regenerates the wildcard,
   `captures/red_raptor_RD-3-position_2026-10-22_raw.txt`), so it is a setting (red_position, default A).
   SP naming via /camera/recording_name verified on the RED 2026-10-22 (after fixing TakeTracker for names known before REC).
   The DeckLink also reports RS as bmdTimecodeRecordingTriggered (generic fallback in MetadataProcessor).
   No lens / ISO / shutter / WB over SDI; RED's network protocol (RCP2) would be the route for those (not investigated).
3. (resolved) ARRI little-endian confirmed on a Mini LF.
4. Hardware so far: UltraStudio Express Recorder 3G on macOS (SDK 16.0) captures VANC (no HANC); DeckLink 8K Pro
   (profile 4 sub-devices half duplex) captures VANC + HANC. Tested on hardware: BURANO, VENICE 2, ALEXA Mini LF,
   URSA, RED V-RAPTOR 8K VV. Builds against SDK 12 (Linux) and SDK 16 (macOS); Windows build not yet checked.

## Next steps
- Lens data for non-smart (vintage) lenses: test ARRI Cable CAM (7p) - ENG (12p) from an RIA-1 / cforce into the VENICE
  LENS port and see which RDD 18 tags move; test ARRI motors + lens file on an ARRI body; ask SP support which focus
  systems (Preston, cmotion, Teradek RT, DCS LDT) SP reads directly; consider DCS LDT + Wooden Camera LPL mount.
- Optional: when a field stops being available (e.g. after switching cameras) clear it in OSC receivers (SP keeps the
  last value).
- RED settings / lens data: RED's network control protocol (RCP2), not investigated.
- Anamorphic: check BURANO / VENICE 1 (E10F), Blackmagic URSA (`--raw` while switching desqueeze), ARRI non-1.0 float.
- ARRI: other bodies (ALEXA 35 uses the new reel/clip fields); Rec Run TC on devices without HANC.
- Unreal tests (FreeD Live Link source, OpenTrackIO); agree FreeD zoom/focus count scaling with the receiver.
- Consider a Live Link source for Unreal and a headless/CLI mode for use alongside SP.
- Decode ZEISS eXtended Data (Sony 7F E201/E215) once the ZEISS / Cooke /i spec is obtained.
- Optional checks: VENICE 2 UMID vs the recorded file; Sony focus against tape marks.

## Build
```bash
cmake -S . -B build          # bundled SDK 16.0 headers; -DDECKLINK_SDK_DIR=... for another SDK
cmake --build build -j
open "build/Metadork.app"   # Windows: build\Release\Metadork.exe (x64 Native Tools prompt, for MIDL)
# Tests only, no SDK needed:
cmake -S . -B build -DSDIMETA_BUILD_APP=OFF && cmake --build build && ctest --test-dir build
```
Only one app can hold a DeckLink input, so stop SP's capture on that port or use an SDI split.
