# Test plan: closing the metadata gaps

For a session with **ARRI ALEXA Mini LF**, **Sony VENICE 2** and **RED V-RAPTOR 8K VV** on a **DeckLink 8K Pro**
(which can capture HANC). Each test names the gap it closes, what to do, how to record it and what "pass" means.
Results feed `docs/COVERAGE.md` and the adapters.

## Before you start

1. Card profile: in the viewer, **Card profile → 4 ports, half duplex** if more than one camera is connected (stop
   Stage Precision's capture on the card first). Note which port each camera is on (`./build/sdimeta_probe --list`).
2. Camera SDI output: **HD (1080p, 3G or 1.5G)**, metadata / ANC enabled, direct cable (no transmitter / converter).
3. Close the viewer before each capture (one app per input). Each test is one command:

   ```bash
   tools/capture.sh <camera> <test-id> <seconds> [probe options, e.g. a port index or --hanc 524D]
   ```

   The port index is the `[n]` from `--list` (default 0). It writes `captures/<camera>_<test-id>_<date>_raw.txt`.
4. **Write down what you did and when** (the probe's clock starts at 0 when the command starts): e.g. "5 s ND 0.6,
   10 s ND 1.2 ...". Hold each setting for ~5 s. Phone level / tape measure for angles and distances.
5. For a live look while setting up: viewer → **Input** tab; values that change turn yellow.

Analysis afterwards: `sdimeta_probe`-format files are replayed through the decoders; the change log shows exactly
which field moved with each action.

---

## Results, 2026-10-22 (DeckLink 8K Pro, port 4)

| Test | Result |
|---|---|
| ML-1 record flag | ✅ HANC 52/4D; SP takes named correctly |
| ML-3 sensor size | ❌ not on SDI (Active Image doesn't identify the mode): manual size / presets |
| ML-4 squeeze | ✅ 1.0-2.0 |
| ML-5 ND, WB, tint, EI, shutter, exposure | ✅ (tint is a float) |
| ML-6 fps | ✅ sensor fps 25 → 50; project fps separate |
| ML-7 slate | ✅ scene / take; circle take not tested (couldn't find it on the camera) |
| ML-8 tilt | ✅ + = up; phone ±20° read +24 / -18 (phone surface ~3° off the sensor axis) |
| V2-1 HANC record flag | ✅ same 52/4D as ARRI, leads the UMID by ~0.7 s; absent before the first REC |
| V2-2 ND | ✅ 8103 = 2^stops (1 clear ... 256 = 2.4) |
| V2-3 EI | ✅ 810B = 8115 = EI; base ISO not sent |
| V2-4 focus accuracy | skipped (production FIZ will come from the focus system) |
| V2-5 shutter | ✅ angle and exposure time consistent in both modes |
| V2-6 squeeze | ✅ 1.25-2.0; no separate SDI desqueeze display setting |
| V2-7 imager modes | ✅ sensor size + crop-aware 35 mm equivalent follow the mode; no recording-resolution tag |
| V2-8 UMID vs file | not done (UMIDs of A003C067 / C068 logged in CLAUDE.md captures) |
| V2-9 look | ✅ look name; 3210/3219 describe the SDI picture (709 with look on); no CDL |
| RED RD-0..RD-3 | ✅ no ANC metadata; clip name + RS flag in the ATC user bits (RED doc 910-0084); Camera Position letter not sent (viewer setting) |
| SP orientation | roll inverted, tilt not (ARRI and Sony) |
| SP naming | ✅ /camera/recording_name on Mini LF, VENICE 2 and RED |

## ALEXA Mini LF (`minilf`)

| ID | Closes | Steps | Pass |
|---|---|---|---|
| ML-1 | **Record flag (HANC 52/4D)** | `tools/capture.sh minilf ML-1-rec 40 <port>`. REC on 10 s, off 10 s, on 10 s, off. | Probe lists `DID 52 SDID 4D ... HANC`; viewer Summary shows record source "SDI rec flag". |
| ML-2 | **Focus / iris accuracy** (electronic lens, lens table cleared) | Supreme Prime. Focus to tape marks **from the sensor plane mark**: 1 m, 2 m, 5 m, infinity; at each, iris T1.5, T2.8, T5.6. 60 s capture. | Focus within the lens's mark accuracy; T-stops match the barrel. Note how infinity is reported. |
| ML-3 | **Sensor size from recording mode** | Switch recording format: LF Open Gate → LF 16:9 UHD → LF 2.39:1 → any S35 / crop modes the camera offers. ~10 s each. 60 s. | Summary sensor size matches each mode, or says "not in table" (then note the mode name and its published size). |
| ML-4 | **Anamorphic squeeze** | Set desqueeze 1.0 → 1.25 → 1.3 → 1.5 → 1.8 → 2.0. 45 s. | `squeeze` follows each value (0x0C4 float). |
| ML-5 | **ND, tint, WB, EI, shutter, exposure time** | ND clear → 0.6 → 1.2 → 2.1; tint −5 → 0 → +5; WB 3200 → 5600; EI 400 → 800 → 1600; shutter 180° → 90° → 1/50 s mode. 90 s. | `nd`, `tint`, `white_balance`, `iso`, `shutter_angle` follow; `exposure_time` unit confirmed (expect 0.020 s at 180° / 25 fps). |
| ML-6 | **Project vs sensor fps, vari-frame** | Project 25. Sensor fps 25 → 50 → 12 (off-speed). 40 s. | `capture_fps` follows sensor fps, `project_fps` stays 25; note the vari-frame flag at 12 fps. |
| ML-7 | **Slate fields** | In the camera, enter scene "12A", take "3", circle the take. 20 s. | `scene`, `take`, `circle_take` appear. |
| ML-8 | **Tilt scale** (roll already verified) | Phone level on the body: level, 20° up, 20° down, hold each 5 s. 30 s. | `tilt` ≈ 0 / +20 / −20. |
| ML-9 | **Lens data lag** (optional) | Fast rack focus near → far while recording a slate clap / timecode on screen. 20 s. | Note `lens_data_lag`; compare focus change frame against picture. |

## VENICE 2 (`venice2`)

| ID | Closes | Steps | Pass |
|---|---|---|---|
| V2-1 | **HANC record flag?** (Livegrade auto-records Sony via HANC) | `tools/capture.sh venice2 V2-1-hanc 40 <port> --hanc 524D,4305,4414,5152`. REC on/off ×2. | Any HANC packet listed; if 52/4D toggles with REC, it's a second record signal next to the UMID. |
| V2-2 | **ND mapping (8103)** | ND clear → 0.3 → 0.6 → 0.9 → 1.2 → 1.5 → 1.8 → 2.1 → 2.4 (and variable ND if fitted). 60 s. | Table of 8103 values per ND step → decode `nd`. |
| V2-3 | **EI / ISO / gain fields** (8115, 8119, 811E, 810A) | Base ISO 800 ↔ 3200; EI 400 → 800 → 1600 → 3200; gain mode if available. 60 s. | Identify which tag is EI vs base ISO vs exposure index. |
| V2-4 | **Focus accuracy & infinity** | Supreme Prime: tape marks from the sensor plane 1 m / 2 m / 5 m / infinity. 40 s. | Within mark accuracy; infinity encoding noted. |
| V2-5 | **Shutter angle vs speed mode** | Shutter angle 180 → 90, then speed mode 1/50 → 1/100. 40 s. | `shutter_angle` and `exposure_time` consistent in both modes. |
| V2-6 | **Squeeze without SDI desqueeze** | Desqueeze 2.0x; toggle the SDI output's desqueeze display off / on. 30 s. | Does E10F still reflect 2.0x? If not, find another field (watch the change log). |
| V2-7 | **Imager mode → sensor size / recording resolution** | 6K 3:2 → 6K 17:9 → 4K 4:3 → 4K 6:5 (or whatever the camera offers). 60 s. | `sensor_width/height` (8104/8105) follow; look for a recording-resolution field. |
| V2-8 | **UMID matches the file** | Record one 10 s clip. Note the clip name. | UMID logged by the app = UMID in the clip's MXF (check with MediaInfo afterwards). |
| V2-9 | **Look / CDL** | Change monitor LUT; enable a CDL if available. 30 s. | `look_name` follows; check 8117 (ASC CDL) content. |
| V2-10 | **Unknown Sony tags** | Covered by the captures above: watch 8103, 8115, 8119, 811E, 811F, 8120, E103, E110, E115, E116, E10A, 8416, 8417. | Each tag explained or noted as constant. |

## RED V-RAPTOR 8K VV (`raptor`): discovery

RED documents "SMPTE Timecode, HANC Metadata" on its SDI outputs but not the format. Nothing is decoded yet, so the
steps are about finding the data first.

| ID | Goal | Steps | Pass / next |
|---|---|---|---|
| RD-0 | Baseline | Monitor SDI out at HD 1080p. `tools/capture.sh raptor RD-0-baseline 10 <port>`. | Lock, timecode source(s), any VANC packets (probe lists them). |
| RD-1 | **Find the HANC packets** | Repeat 10 s captures with different candidate sets (4 per run): `--hanc 524D,4414,4305,4404` · `--hanc 5152,5153,4105,4102` · `--hanc 6060,6061,4108,4109` (and any IDs from RED's SDI documentation if you can get it). | A HANC DID/SDID that appears. Note it; if none appear, RED's metadata may need RED's network API instead. |
| RD-2 | **Decode** (once RD-1 finds packets) | With the found IDs in `--hanc`: change ISO, WB, shutter, fps; rack focus / zoom / iris (RF-mount electronic lens); REC on/off ×2; tilt / roll. ~90 s. | Fields mapped from the change log → write the RED adapter. |
| RD-3 | Record state and TC | REC on/off ×2 in free-run TC, then in rec-run TC. 40 s each. | Record signal identified (HANC flag, clip change or rec-run TC). |

## Outputs (any camera)

| ID | Goal | Steps | Pass |
|---|---|---|---|
| OUT-1 | FreeD into Unreal / SP | Send to the Windows machine; agree focus / zoom count scaling with the receiver. | Lens values move the virtual camera correctly; note any sign / scale changes needed. |
| OUT-2 | OpenTrackIO into Unreal / SP | Multicast `239.135.1.<n>:55555` (receiver on the same subnet). | Receiver shows focus / iris / focal and camera static data. |
| OUT-3 | OSC | Point an OSC monitor (e.g. Protokol) at the Mac's output. | All available variables arrive with the expected values / types. |

## After the session

Send the capture files (they're in `captures/`) and your notes; each test's result goes into the adapters, the
coverage report (`sdimeta_coverage`) and `CLAUDE.md`.
