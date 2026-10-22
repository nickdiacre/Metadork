# Camera metadata: master field list, coverage and gaps

Status as of 2026-10-22. Coverage is from real-camera captures in `captures/` unless marked otherwise.

Legend: ✅ decoded **and output** · 🟡 decoded but **not yet output** (Input tab / log only) · 🔶 derived or partial ·
❔ present in the signal but not decoded · — not available from this camera's SDI metadata

Cameras: **AL** ALEXA Mini LF (ARRI 4 KB block), **V2** Sony VENICE 2 / **BU** BURANO (RDD 18 + clip KLV),
**UR** Blackmagic URSA (SDI camera control protocol).

## 1. Master field list

The `id` is the proposed stable key for the master format (and the OSC address suffix, where output today).

### Identity

| id | Meaning | Unit / type | AL | V2 | BU | UR | Out today |
|---|---|---|---|---|---|---|---|
| `camera_make` | Manufacturer | string | ✅ | ✅ | ✅ | ✅ | OSC `camera`, OTIO |
| `camera_model` | Model (e.g. ALEXA Mini LF, VENICE 2 (MPC-3626)) | string | ✅ | ✅ | ✅ | — | OSC `camera`, OTIO |
| `camera_serial` | Body serial | string | ✅ | ✅ | ✅ | — | OSC, OTIO |
| `camera_firmware` | Software version (SUP / Version) | string | 🟡 | 🟡 | 🟡 | — | OTIO static only |
| `camera_index` | Camera letter (A, B ...) | string | 🟡 | — | — | — | — |
| `lens_model` | Lens name | string | ✅ | — | — | — | OSC `lens`, OTIO |
| `lens_serial` | Lens serial / ID | string | ✅ | ✅ | ✅ | — | OSC `lens`, OTIO |

### Lens (dynamic)

| id | Meaning | Unit / type | AL | V2 | BU | UR | Out today |
|---|---|---|---|---|---|---|---|
| `focus_distance` | Focus distance | m | ✅ | ✅ | ✅ | — | FreeD, OSC, OTIO |
| `focus_position` | Focus position, 0 near - 1 far | 0..1 | — | — | — | 🔶 if sent | OSC, OTIO encoder |
| `iris_t` / `iris_f` | T-stop / f-number | number | ✅ T | ✅ T + f | ✅ f | ✅ f | FreeD spare, OSC, OTIO |
| `focal_length` | Focal length | mm | ✅ | ✅ | ✅ | 🔶 zoom lenses | FreeD, OSC, OTIO |
| `focal_length_35mm` | 35 mm still equivalent | mm | — | 🟡 | 🟡 | — | — |
| `entrance_pupil` | Entrance pupil position | mm | 🟡 ALEXA 35 | — | — | — | — |
| `effective_focal_length` | Axial effective focal length | mm | 🟡 ALEXA 35 | — | — | — | — |
| `dof_near` / `dof_far` | Depth of field limits | m | — | ❔ /i E201 | — | — | — |
| `lens_distortion` | Distortion curve | array | — | ❔ ZEISS XD E215 | — | — | — |
| `lens_shading` | Relative illumination curve | array | — | ❔ ZEISS XD E215 | — | — | — |
| `extender` | Optical extender / converter factor | number | 🟡 | 🟡 | — | — | — |
| `lens_encoders_raw` | Raw LDS / motor encoder values | uint16 x6 | 🟡 | — | — | — | — |
| `lens_data_lag` | Lens data latency | frames | 🟡 | — | — | — | — |

### Exposure / image

| id | Meaning | Unit / type | AL | V2 | BU | UR | Out today |
|---|---|---|---|---|---|---|---|
| `iso` | EI / ISO | int | ✅ | ✅ | ✅ | ✅ | OSC, OTIO |
| `shutter_angle` | Shutter angle | deg | ✅ | ✅ | ✅ | ✅ | OSC, OTIO |
| `exposure_time` | Shutter speed / exposure time | s | 🟡 | 🟡 | 🟡 | — | — |
| `white_balance` | Colour temperature | K | ✅ | ✅ | ✅ | ✅ | OSC |
| `tint` | Green/magenta tint | number | 🟡 | — | — | 🟡 | — |
| `nd_density` | Internal ND | optical density | 🟡 | ❔ (8103 = 8) | ❔ | — | — |
| `gain` | Master gain | dB | — | 🟡 | 🟡 | — | — |
| `capture_fps` | Sensor frame rate | rational | ✅ | ✅ | ✅ | ✅ | OSC, OTIO |
| `project_fps` | Project / base frame rate | rational | 🟡 | — | — | — | — |
| `sensor_size` | Active sensor area | mm x mm | 🔶 derived from mode | ✅ | ✅ | manual | OSC, OTIO |
| `squeeze` | Anamorphic desqueeze | ratio | ✅ (1.0 seen) | ✅ | not tested | not tested | OSC, OTIO |
| `recording_resolution` | Recorded image size | px x px | 🔶 shape only | — | — | 🟡 (1.9) | — |
| `codec` | Recording codec | string | — | — | — | 🟡 | — |
| `capture_gamma` / `colour_primaries` | Encoding (S-Log3 / S-Gamut3.Cine, BT.709 ...) | label | — | 🟡 | 🟡 | 🟡 (dynamic range) | — |
| `look_name` | Look / monitoring LUT name | string | 🟡 look file | 🟡 LUT | 🟡 LUT | 🟡 display LUT | — |
| `asc_cdl` | ASC CDL values | numbers | — | ❔ (8117) | — | — | — |
| `frame_lines` | Frame line name / rectangles | struct | ❔ (FLI 0x850+) | 🟡 (E101/E102) | 🟡 | 🟡 (overlays) | — |

### Orientation

| id | Meaning | Unit / type | AL | V2 | BU | UR | Out today |
|---|---|---|---|---|---|---|---|
| `tilt` | Camera tilt, + up | deg | ✅ | ✅ | 🔶 (same fields, not verified) | — | FreeD, OSC, OTIO (opt-in) |
| `roll` | Camera roll, + clockwise from behind | deg | ✅ | ✅ | 🔶 | — | FreeD, OSC, OTIO (opt-in) |
| `pan` | Pan / heading | deg | — | — | — | — | (field reserved) |

### Timing / take

| id | Meaning | Unit / type | AL | V2 | BU | UR | Out today |
|---|---|---|---|---|---|---|---|
| `timecode` | SDI timecode (RP 188) | HH:MM:SS:FF | ✅ | ✅ | ✅ | ✅ | OSC, OTIO |
| `timecode_camera` | Camera's own master TC | HH:MM:SS:FF | 🟡 | — | — | 🟡 (9.4) | — |
| `recording` | Record state | bool | 🔶 HANC card or Rec Run TC | ✅ UMID | 🔶 Rec Run / clip change | ✅ | OSC, OTIO |
| `clip_name` | Clip name | string | ✅ | ✅ | ✅ | — | OSC, OTIO slate |
| `reel_name` | Reel / magazine | string | ✅ | 🔶 (inside clip name) | 🔶 | — | OSC |
| `umid` | SMPTE UMID of the take | hex | — (FF) | ✅ while recording | — | — | OSC |
| `scene` / `take` / `circle_take` | Slate info | string / bool | 🟡 | — | — | — | — |
| `vari_frame_duplicate` | Duplicate frame flag | bool | 🟡 | — | — | — | — |
| `tally` | Tally state | int | — | — | — | 🟡 | — |

## 2. Comparison with Pomfort Livegrade

Pomfort doesn't publish one field list; this is assembled from its articles on Livegrade's SDI / dynamic metadata
(Livegrade 4 - 7, see sources). Livegrade fields and our status:

| Livegrade field | Ours | Notes |
|---|---|---|
| Timecode | ✅ | |
| Clip Name | ✅ | ARRI, Sony |
| Sensor FPS | ✅ `capture_fps` | |
| Project FPS | 🟡 | ARRI decoded, not output; SDI video rate is known |
| Source image resolution | 🔶 | URSA reports it; ARRI only the shape; Sony not found |
| EI / ISO | ✅ | |
| Shutter (angle) | ✅ | |
| White Balance | ✅ | |
| Tint | 🟡 | ARRI, URSA decoded; not output |
| ND Filter | 🟡 | ARRI decoded; Sony value 8103 not yet understood |
| Lens Model | ✅ | ARRI by name; Sony gives a lens ID only |
| Focal Length | ✅ | |
| Aperture | ✅ | T or f, labelled |
| Focus Distance | ✅ | |
| Applied filters | — | not seen in any capture |
| Camera Tilt / Roll | ✅ | Livegrade documents these for ARRI; we also have Sony |
| Distance to subject | — | not seen in any capture |
| Camera Model | ✅ | |
| Software version | 🟡 | decoded, not output |
| Look (CDL / LUT) | 🟡 names only | Livegrade generates looks itself; cameras only report LUT names |
| Slate (scene / take) | 🟡 ARRI | Livegrade takes slate info manually |

**Where we go beyond Livegrade's documented list:** per-frame record state for Sony (UMID), the take UMID itself,
anamorphic squeeze, active sensor size, raw lens encoders and lag, ZEISS /i depth of field and distortion / shading
data (captured, not decoded), and live output as FreeD / OSC / OpenTrackIO for VP.

**Gaps worth closing** (all small, the data is already decoded): tint, ND, project fps, software version, recording
resolution, look / LUT name, scene / take, camera index, exposure time. Bigger items: Sony ND and gain semantics, ZEISS
eXtended Data (needs the spec), ARRI record flag (needs a HANC-capable device, e.g. DeckLink 8K Pro).

## 3. Proposed master format

Goal: one camera-neutral record, filled by one adapter per manufacturer / body, so new makes (Canon, RED, Panasonic
...) only add an adapter and a coverage column.

1. **Field registry** (one table in code, mirrored as `docs/fields.json`): for each field its `id`, group, type,
   SI-style unit, description, and how it maps to each output (OSC address, OpenTrackIO path, FreeD slot, Livegrade
   name). Outputs, the Summary tab, the OSC variable list, the log and these docs are all generated from it, so a new
   field appears everywhere at once.
2. **Values with provenance**: each value carries its source (`camera`, `derived`, `manual`), a confidence
   (`verified` on hardware, `documented`, `inferred`) and the decoder field it came from (e.g. `sony.rdd18.7F.8414`,
   `arri.0x045C`). This replaces today's ad-hoc `has*` flags and `sensorSource` string.
3. **Adapters**: `detect(packets)` and `fill(frame, record)` per manufacturer (ARRI, Sony RDD 18, Blackmagic today),
   plus per-body quirks (Mini LF shutter units, VENICE 2 UMID, BURANO clip events) kept inside the adapter.
4. **Coverage matrix as data**: each adapter declares which fields it can supply and their verification status; this
   document's tables are generated from that, and tests assert it against the real captures.
5. **Future manufacturers**: Canon Cinema EOS and Panasonic VariCam are documented to send ANC metadata (Livegrade
   supports both via SDI); RED documents metadata over Ethernet rather than SDI. Each would need a capture session
   like the ones in `captures/` before its adapter is written.

Sources: Pomfort, [Dynamic Metadata: Camera and Lens information for each Frame](https://pomfort.com/article/dynamic-metadata-in-silverstack-and-livegrade/),
[Metadata in Livegrade](https://pomfort.com/article/metadata-journey-in-the-pomfort-ecosystem-livegrade/),
[Livegrade Pro v4: camera metadata](https://pomfort.com/article/livegrade-pro-version-4-how-to-manage-camera-metadata/),
[Ways to receive camera metadata at the DIT cart](https://pomfort.com/article/ways-to-receive-camera-metadata-at-the-dit-cart/),
[Livegrade changelog](https://kb.pomfort.com/livegrade/reference/livegrade-installation/changelog-for-livegrade/).
