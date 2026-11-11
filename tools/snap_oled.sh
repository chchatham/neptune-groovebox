#!/bin/sh
# Photo of the device plus an enlarged crop of the OLED (camera framing as of 2026-11-10).
# Usage: tools/snap_oled.sh [name]  -> prints full photo path and crop path
F=$("$(dirname "$0")/snap.sh" "${1:-snap}" 2>/dev/null) || exit 1
C="${F%.jpg}-oled.jpg"
sips -c 170 270 --cropOffset 30 395 "$F" --out "$C" >/dev/null && sips -z 680 1080 "$C" >/dev/null
echo "$F"; echo "$C"
