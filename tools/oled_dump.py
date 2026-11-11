#!/usr/bin/env python3
"""Read the firmware's host-bitmap page buffer back over the debug SysEx (NP5+) and
show it as text art / save a PNG-free PBM. This is what the firmware holds for
display layer 0 (and layer 1); with the display lock it is what the OLED shows.

Buffer: pointer at 0x20000028 -> 8 pages x 128 bytes; byte = 8 vertical pixels, bit 0 = top.
Usage: oled_dump.py [out.pbm]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nepdbg import Dbg

def read_pixels(d):
    buf = d.peek32(0x20000028)
    raw = d.read_mem(buf, 1024)
    return [[(raw[(y >> 3) * 128 + x] >> (y & 7)) & 1 for x in range(128)] for y in range(64)]

def art(px):
    out = []
    for y in range(0, 64, 2):
        out.append(''.join(' ▀▄█'[px[y][x] + 2 * px[y + 1][x]] for x in range(128)))
    return '\n'.join(out)

if __name__ == "__main__":
    d = Dbg(); px = read_pixels(d); d.close()
    print(art(px))
    if len(sys.argv) > 1:
        open(sys.argv[1], 'w').write("P1\n128 64\n" + '\n'.join(' '.join(map(str, r)) for r in px) + '\n')
