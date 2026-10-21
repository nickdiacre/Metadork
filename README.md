# SDI Metadata Viewer

Reads per-frame camera metadata from an SDI feed on a Blackmagic DeckLink / UltraStudio device and sends it to
Stage Precision, Unreal Engine or anything else that speaks **FreeD**, **OSC** or **OpenTrackIO**. The aim is to get
lens data (focus / iris / zoom), camera orientation, timecode and record state from the same SDI cable that already
carries the picture, instead of running extra data cables from the lens system.

The camera is detected automatically and only the data it actually supplies is shown and sent.

## What each camera provides

Verified on real hardware with an UltraStudio Express Recorder 3G (DeckLink SDK 16, macOS).

| | Sony VENICE 2 | Sony BURANO | ARRI ALEXA Mini LF | Blackmagic URSA |
|---|---|---|---|---|
| Format | RDD 18 (43/05) + clip KLV (44/04) | same | ARRI 4 KB block (44/04, lines 9-11) | Camera control (51/53) |
| Focus distance | ✅ metres | ✅ (with electronic lens) | ✅ (LDS / /i lens, or motors + lens file) | position 0-1 only, if sent |
| Iris | ✅ T and f | ✅ | ✅ T | ✅ f |
| Focal length | ✅ | ✅ | ✅ | ✅ only with electronic zoom |
| Tilt / roll | ✅ 0.1° | same fields, not separately verified | ✅ 0.1° | — |
| Record state | ✅ (take UMID) | Rec Run TC or clip-name changes | Rec Run TC, or HANC-capable DeckLink | ✅ (transport mode) |
| Clip / reel | ✅ | ✅ | ✅ | — |
| Sensor size (active) | ✅ | ✅ | — (not on ARRI SDI) | — |
| Anamorphic squeeze | ✅ | not yet tested | ✅ | not yet tested |
| ISO, shutter, WB, fps, model, serial | ✅ | ✅ | ✅ | ✅ (no model) |

Other ARRI bodies (ALEXA 35, LF, Mini, SXT/XT, AMIRA, 65, 265) use the same documented block; firmware differences
(for example shutter units) are handled where known. Timecode (RP 188 VITC / LTC) is read from every camera.

Lens data only exists if the camera knows it: an electronic lens (ARRI LDS, Cooke /i, ZEISS eXtended Data, Sony
E-mount) or lens motors / encoders connected to the camera. A follow-focus system that isn't connected to the camera
does not put FIZ into the SDI.

## Outputs

All outputs send once per captured frame and can run at the same time. Settings are saved between sessions.

- **FreeD** (UDP, default port 40000): standard 29-byte D1 packet. Zoom and focus are encoder counts, so their scale
  is configurable (default: focus in mm, focal length in micrometres) and must match the receiver. Iris goes in the
  spare bytes (×100). Tilt and roll fill the pan/tilt/roll fields when the camera supplies them.
- **OSC** (UDP, default port 9000): one message per variable, `<prefix>/<name>` (default prefix `/camera`), as
  float, int or string. The Output tab lists every possible variable with a checkbox; ones the current camera doesn't
  supply are greyed out and never sent. Variables: `focus`, `focus_normalised`, `iris`, `zoom`, `pan`, `tilt`,
  `roll`, `timecode`, `recording`, `clip`, `reel`, `umid`, `sensor_width`, `sensor_height`, `iso`, `shutter_angle`,
  `white_balance`, `capture_fps`, `squeeze`, `camera`, `camera_serial`, `lens`.
- **OpenTrackIO 1.0.1** ([SMPTE RIS OSVP](https://ris-pub.smpte.org/ris-osvp-metadata-camdkit/)): JSON samples with
  the standard 16-byte `OTrk` header and Fletcher-16 checksum, multicast to `239.135.1.<source number>:55555` or
  unicast. Sends lens focus distance, f/T-stop and focal length, timing with timecode, recording and slate (clip),
  plus static camera data (make, model, serial, sensor size, frame rate, ISO, shutter, squeeze) about every 2 s.
  Validated against the official schema. The camera transform (tilt and roll only) is off by default, because a
  transform with zero pan and position would override a real tracking system.

## Using it

1. Build (below) and run `SdiMetaViewer`.
2. Pick the input and press **Start capture**.
3. Tabs:
   - **Summary**: detected camera, timecode, record state and every value in one table.
   - **Input**: every decoded field for the detected camera; values that changed in the last 2 s turn yellow.
   - **Output**: FreeD / OSC / OpenTrackIO / Log file. Set hosts and options, then **Apply**.
   - **Raw**: every ANC packet with hex dumps.
4. Settings (outputs, OSC variable choices, device, panel width, log path) are stored in
   `~/Library/Application Support/SdiMetaViewer/settings.ini` (macOS), `%APPDATA%\SdiMetaViewer\settings.ini`
   (Windows) or `~/.config/sdimeta/settings.ini` (Linux), and restored at launch.

On macOS, allow **Local Network** access when prompted; without it network output fails silently.

### `sdimeta_probe`: is the metadata actually arriving?

A command-line check that bypasses the decoders. Only one app can hold the input, so close the viewer first.

```bash
./build/sdimeta_probe 5            # signal, ANC packets, every timecode source, raw VANC lines
./build/sdimeta_probe 5 --dump     # plus the hex of every packet in one frame
./build/sdimeta_probe 60 --watch   # print every decoded change as it happens (REC, focus, settings...)
./build/sdimeta_probe 60 --raw     # every packet of every frame, for offline analysis
```

The raw VANC report tells "no ANC in the signal" (lines captured, blank) apart from "device not capturing VANC".

## Camera setup tips

- **Sony VENICE / VENICE 2 / BURANO:** use an SDI output with metadata enabled; on the VENICE 2 only one of the two
  outputs tested carried it. The VENICE 2 reports record state in any timecode mode (via the take's UMID); on BURANO
  use Rec Run TC (clip-name changes are a fallback).
- **ARRI:** enable SDI metadata. A manually selected lens-table entry overrides an electronic lens's own data (you get
  name and focal length but no focus or iris); clear the lens table to use the lens's data. For record state through a
  device without HANC capture, set timecode to **Rec Run**.
- **Blackmagic URSA:** iris, settings and record state are sent; focus and zoom only with lenses that report them.
- **SDI format:** a 3G device (like the UltraStudio Express Recorder 3G) needs HD / 3G-SDI, not 6G/12G UHD.
- **Signal chain:** vision mixers, some routers' processing, wireless links and converters often strip ANC. Test with
  a direct cable first.
- Set the camera clock: Sony and ARRI clip names include the camera date.

## Build

Requirements: CMake 3.20+, a C++17 compiler, Git, the Blackmagic Desktop Video driver, and the **DeckLink SDK**
(free from Blackmagic's support site). Builds against SDK 12 through 16. GLFW and Dear ImGui are fetched by CMake.

```bash
cmake -S . -B build -DDECKLINK_SDK_DIR="/path/to/Blackmagic DeckLink SDK 16.0"   # folder with Mac/ Win/ Linux/
cmake --build build --config Release

# Parser tests only (no SDK, device or GPU needed)
cmake -S . -B build -DSDIMETA_BUILD_APP=OFF
cmake --build build && ctest --test-dir build
```

- **macOS:** Xcode command line tools. Output: `build/SdiMetaViewer`, `build/sdimeta_probe`.
- **Windows:** Visual Studio 2022 with *Desktop development with C++*; run CMake from the *x64 Native Tools Command
  Prompt* so `midl.exe` (which generates the DeckLink COM headers) is on the PATH. Output: `build\Release\`.
  Not yet built on Windows; please report any compile errors.
- **Linux:** X11 dev packages (`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev`);
  `-DGLFW_BUILD_WAYLAND=ON` for native Wayland.

## Known limitations

- **ARRI record flag** is a HANC packet (52/4D). The app requests it, but only devices that support HANC capture
  deliver it (the UltraStudio Express Recorder 3G does not); use Rec Run TC instead.
- **Pan** is not available from any camera; it needs a tracking system or an encoded head.
- **Tilt / roll** come from a gravity sensor: accurate when still (about 0.1-1°), disturbed by fast moves.
- **ZEISS eXtended Data** (distortion / shading curves passed through by the VENICE 2) is captured but not yet
  decoded; the ZEISS / Cooke /i spec is needed to label it.
- **Quad-link / dual-stream** inputs are not handled. Use single-link 12G or 3G/HD.
- **Preview** converts frames to BGRA on the CPU; untick *Show video* on slow machines. Decoding continues either way.

## Layout

```
src/anc/       ANC decoders, no SDK dependency (unit-tested)
  Rp214              KLV reassembly from segmented ANC (MID/PSC)
  ArriMetadata       ARRI 4 KB block
  Rdd18              Sony RDD 18 sets, lens value encodings, clip KLV + UMID
  BmdCameraControl   Blackmagic SDI camera control protocol
  MetadataProcessor  per-frame decode and record state
src/out/       camera-agnostic sample + variable catalogue, FreeD / OSC / OpenTrackIO, UDP, settings
src/log/       JSON Lines logger
src/capture/   DeckLink enumeration, capture callback, platform shims
src/tools/     sdimeta_probe
src/main.cpp   GLFW + Dear ImGui UI
tests/         parser tests, incl. fixtures from real Mini LF, VENICE 2, BURANO and URSA frames
captures/      probe captures from real cameras (evidence for the decoders)
```

`CLAUDE.md` holds the detailed reverse-engineering notes: field offsets, encodings, how each finding was verified,
and the open issues.

## Log format

One JSON object per frame (Output → Log file), for example:

```json
{"frame":1204,"host_time":8123.402113,"tc":"18:24:24:00","rec":"STOPPED","rec_source":"Sony UMID in clip packet",
 "sony_clip":"A003C050_221006X4",
 "rdd18":[{"set":"01","complete":true,"items":[{"tag":"8001","name":"Focus position (image plane)",
   "value":"9.890 m  (32.45 ft)","hex":"E3DD"}, ...]}, ...],
 "anc":[{"did":"43","sdid":"05","line":11,"stream":0,"len":75,"hex":"..."}, ...]}
```

`host_time` is a monotonic clock in seconds, useful for measuring latency against tracking data.
