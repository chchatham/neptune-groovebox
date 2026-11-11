#!/bin/sh
# Take one still of the Launchkey with the Mac's built-in camera.
# Usage: tools/snap.sh [name]   -> .ralph/snaps/<timestamp>-<name>.jpg (path printed)
# -w 2 lets the camera settle exposure before the shot.
DIR="$(cd "$(dirname "$0")/.." && pwd)/.ralph/snaps"
mkdir -p "$DIR"
OUT="$DIR/$(date +%Y%m%d-%H%M%S)-${1:-snap}.jpg"
imagesnap -q -d "FaceTime HD Camera" -w 2 "$OUT" && echo "$OUT"
