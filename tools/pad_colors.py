#!/usr/bin/env python3
"""Set pad LEDs to known RGB colours (DAW mode). Usage: pad_colors.py pattern|off
pattern: top row  = red green blue white yellow cyan magenta orange (full brightness)
         bottom   = same colours at 1/4 brightness
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mido
from lkmidi import find_port
COLS = [(127,0,0),(0,127,0),(0,0,127),(127,127,127),(127,127,0),(0,127,127),(127,0,127),(127,48,0)]
NAMES = "red green blue white yellow cyan magenta orange".split()
def rgb(out, pad, c): out.send(mido.Message('sysex', data=[0x00,0x20,0x29,0x02,0x13,0x01,0x43,pad,*c]))
with mido.open_output(find_port("DAW", 'output')) as out:
    out.send(mido.Message('note_on', channel=15, note=0x0C, velocity=0x7F)); time.sleep(0.3)
    off = len(sys.argv) > 1 and sys.argv[1] == 'off'
    for i, c in enumerate(COLS):
        rgb(out, 0x60 + i, (0,0,0) if off else c)
        rgb(out, 0x70 + i, (0,0,0) if off else tuple(v // 4 for v in c))
        time.sleep(0.01)
print("pads off" if off else "top row: " + " ".join(NAMES) + " | bottom row: same at 1/4 brightness")
