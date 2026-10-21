# SDI Metadata Viewer

Previews an SDI input on a Blackmagic DeckLink card and shows or logs the ancillary data in every frame:

- **All ANC packets**: DID/SDID, line, stream, size and count, with a hex dump of the last payload for each type.
- **Timecode**: RP 188 via the DeckLink SDK (first valid of HFR TC / VITC1 / LTC / VITC2).
- **ARRI extended metadata**: the RP 214 KLV block (DID 44/04) is reassembled and decoded. That gives focus, iris and focal length, raw LDS and motor encoder values, lens lag, entrance pupil position (ALEXA 35), reel and clip names, EI, shutter, WB, ND, camera ID and more. Offsets follow ARRI's *SDI Metadata* white paper (July 2023), which covers ALEXA 35, Mini LF, LF, Mini, SXT/XT, AMIRA, 65 and 265.
- **ARRI record flag** (DID 52/4D). See the limitations below.
- **Sony VENICE / VENICE 2 / BURANO**: an experimental RDD 18 view (DID 43/05) plus raw capture. See the limitations below.
- **Per-frame JSON Lines log**, optionally with raw ANC payload hex.

## Build

Requirements: CMake 3.20+, a C++17 compiler, Git, the Blackmagic Desktop Video driver, and the **DeckLink SDK** (free download from Blackmagic's support site). Tested against SDK 12.0 headers; anything newer should work.

The GUI uses GLFW and Dear ImGui. CMake fetches them automatically.

```bash
# Parser tests only (no SDK, card or GPU needed)
cmake -S . -B build -DSDIMETA_BUILD_APP=OFF
cmake --build build && ctest --test-dir build

# Full app
cmake -S . -B build -DDECKLINK_SDK_DIR="/path/to/Blackmagic DeckLink SDK 14.x"
cmake --build build --config Release
```

**Windows:** run CMake from an *x64 Native Tools / Developer* prompt so `midl.exe` is on the PATH. It generates the COM headers from `Win/include/DeckLinkAPI.idl`.
**macOS:** the Xcode command line tools are enough.
**Linux:** install the X11 dev packages (`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev`). The build is X11-only by default and runs under XWayland; pass `-DGLFW_BUILD_WAYLAND=ON` for native Wayland.

## Camera setup

- **ARRI:** enable SDI metadata on the output you're feeding (ALEXA 35: SDI settings → *Metadata*; older ALEXA: *SDI → ARRI metadata*). Lens data only appears if the camera knows it, which means an LDS or /i lens, or a LBUS/LCS motor or encoder system feeding the camera. A Preston driving a non-smart lens will not put FIZ into the SDI.
- **Sony VENICE / BURANO:** enable SDI metadata output in the camera menu. Lens data needs Cooke /i or an E-mount lens, or a data source injecting into the lens port (e.g. DCS LDT).
- Wireless video links and some converters strip ANC. Test with a direct BNC first.

## Known limitations

1. **HANC visibility.** The ARRI record flag is sent in HANC. The DeckLink ancillary packet API is primarily VANC, so depending on card and driver you may never see packet 52/4D. If it doesn't appear in the ANC tab, record state shows `UNKNOWN`, and the "TC running" hint is only meaningful if the camera is set to rec-run timecode.
2. **Sony decoding is unverified.** Sony's SDI metadata format isn't documented publicly the way ARRI's is. The RDD 18 tab walks the payload as tag/length/value, and its tag labels are guesses. To make it definitive: enable *Include raw ANC payloads*, log while pulling focus, zoom and iris through known marks, and toggle REC. The diffs will show which bytes move.
3. **Byte order.** ARRI values are read little-endian (the ArriFileV3 header convention). This hasn't been confirmed against live output. If numbers look absurd on a real camera, that is the first thing to check.
4. **Preview.** Frames are converted to BGRA on the CPU with the SDK's converter. At 4K 50/60 on a slow machine, untick *Show video*. Metadata decoding and logging continue either way.
5. **Quad-link / dual-stream** inputs are not handled. Use single-link 12G or 3G/HD.

## Layout

```
src/anc/       ANC parsing, no SDK dependency (unit-tested)
  AncRegistry      DID/SDID names, hex helpers
  Rp214            KLV reassembly from segmented ANC (MID/PSC)
  ArriMetadata     ARRI 4 KB block decoder
  Rdd18            experimental RDD 18 walker
  MetadataProcessor  per-frame decode + record state
src/log/       JSON Lines logger
src/capture/   DeckLink device enumeration, capture callback, platform shims
src/main.cpp   GLFW + ImGui UI
tests/         synthetic ARRI stream built per the white paper
```

## Log format

One JSON object per frame, for example:

```json
{"frame":1204,"host_time":8123.402113,"tc":"10:20:30:04","rec":"RECORDING",
 "arri":{"camera":"Alexa 35","reel":"A_0001_1234","clip":"A_0001C001_260930_101500_a1234",
 "focus_mm":2500.0,"focal_mm":50.0,"t_stop":2.8284,"raw":{"focus_lds":4660,...},
 "entrance_pupil_um":123456,...},
 "anc":[{"did":"44","sdid":"04","line":9,"stream":0,"len":233,"hex":"..."}, ...]}
```

`host_time` is a monotonic clock in seconds, useful for measuring latency against tracking data in Stage Precision.
