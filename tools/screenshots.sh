#!/bin/bash
# Regenerates the manual's screenshots (docs/images/*.png) by replaying sample captures through the app's
# --screenshots mode (each tab rendered and saved in turn; no camera or DeckLink needed). macOS: uses sips for PNG.
set -euo pipefail
cd "$(dirname "$0")/.."
APP=${APP:-"./build/Metadork.app/Contents/MacOS/Metadork"}
OUT=docs/images
TMP=$(mktemp -d)
mkdir -p "$OUT"
shoot() {  # <prefix> <capture>
    "$APP" --replay "$2" --screenshots "$TMP" --screenshot-prefix "$1"
}
shoot venice2 captures/venice2_focus_rack_2026-10-21_raw.txt
shoot minilf  captures/minilf_rec_2026-10-21_raw.txt
shoot red     captures/red_raptor_RD-1-rec_2026-10-22_raw.txt
# Only the images docs/MANUAL.md uses
KEEP="venice2-summary venice2-input venice2-output-osc venice2-output-freed venice2-output-opentrackio venice2-output-log
      venice2-raw minilf-summary minilf-output-freed red-summary"
for name in $KEEP; do
    f="$TMP/$name.tga"
    # Retina framebuffer (2x): scale to 1200 px wide to keep the repo small but text readable.
    sips -s format png --resampleWidth 1200 "$f" --out "$OUT/$name.png" >/dev/null
done
rm -rf "$TMP"
ls -1 "$OUT"
