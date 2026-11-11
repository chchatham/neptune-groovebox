#!/bin/sh
# Record SECONDS (default 2) from the default microphone and print level and rough pitch.
# Usage: tools/listen.sh [seconds] [name]  -> .ralph/snaps/<timestamp>-<name>.wav
# Used to confirm the synth is audibly sounding (speakers -> built-in mic). Level is RMS amplitude, 0..1.
DIR="$(cd "$(dirname "$0")/.." && pwd)/.ralph/snaps"; mkdir -p "$DIR"
OUT="$DIR/$(date +%Y%m%d-%H%M%S)-${2:-listen}.wav"
sox -q -d -c 1 -r 44100 "$OUT" trim 0 "${1:-2}" 2>/dev/null || { echo "record failed (microphone permission?)"; exit 1; }
sox "$OUT" -n stat 2>&1 | grep -E "RMS +amplitude|Maximum amplitude|Rough +frequency" | tr -s ' ' | tr '\n' ';'
echo " $OUT"
