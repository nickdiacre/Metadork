#!/bin/sh
# Records a named test capture for the test plan (docs/TEST_PLAN.md).
#   tools/capture.sh <camera> <test-name> [seconds] [extra sdimeta_probe args...]
# Writes captures/<camera>_<test-name>_<YYYY-MM-DD>_raw.txt (every packet of every frame + TC user bits) and prints
# the probe summary. Close the viewer first: only one app can hold the input.
set -e
[ $# -ge 2 ] || { echo "usage: $0 <camera> <test-name> [seconds] [probe args...]"; exit 1; }
cam=$1; name=$2; secs=${3:-30}; [ $# -ge 3 ] && shift 3 || shift $#
dir=$(cd "$(dirname "$0")/.." && pwd)
out="$dir/captures/${cam}_${name}_$(date +%Y-%m-%d)_raw.txt"
echo "Recording $secs s -> $out"
"$dir/build/sdimeta_probe" "$secs" --raw "$@" > "$out" 2>&1
grep -E "Device|Detected|Frames  |HANC|DID |RP188 Any" "$out" | head -20
