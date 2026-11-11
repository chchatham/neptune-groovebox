#!/usr/bin/env python3
"""Simulate button presses on the Launchkey via the firmware's built-in input
simulation (needs debug-command firmware NP5+).

  nepinput.py hold shift [SECONDS]     press, wait (default 1 s), release
  nepinput.py tap settings             short press
  nepinput.py combo shift scale        hold first, tap the rest, release
  nepinput.py on | off                 enter / leave simulation mode only

While simulation mode is on, the PHYSICAL controls are ignored (they read as
released). Every command here turns it off again on exit unless it is `on`.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nepdbg import Dbg, BASE

SIM_ENABLE = BASE + 0x13660   # (on): sets flag 0x2000F516, clears override table 0x20009170
SIM_SET    = BASE + 0x1362E   # (id, raw12): override raw reading for control id
IDS = {'shift': 0x19, 'settings': 0x1A, 'play': 0x48, 'record': 0x4A, 'oct-': 0x20, 'oct+': 0x21,
       'up': 0x22, 'down': 0x23, 'arp': 0x1E, 'scale': 0x42, 'right': 0x10, 'func': 0x11,
       'bank-up': 0x25, 'bank-down': 0x24}
# All measured with the user pressing each button (2026-11-10). 'right' = button right of the top pad row,
# 'func' = right of the bottom pad row, bank-up/down = the two small buttons right of the encoders.

class Sim:
    def __init__(self, dbg=None): self.d = dbg or Dbg(); self.d.call(SIM_ENABLE, 1)
    def press(self, name):   self.d.call(SIM_SET, IDS[name], 0xFFF)
    def release(self, name): self.d.call(SIM_SET, IDS[name], 0)
    def tap(self, name, t=0.15): self.press(name); time.sleep(t); self.release(name); time.sleep(0.05)
    def close(self): self.d.call(SIM_ENABLE, 0); self.d.close()

if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'off': d = Dbg(); d.call(SIM_ENABLE, 0); d.close(); sys.exit()
    s = Sim()
    if cmd == 'on': s.d.close(); sys.exit()
    try:
        if cmd == 'hold': s.press(args[0]); time.sleep(float(args[1]) if len(args) > 1 else 1.0); s.release(args[0])
        elif cmd == 'tap': s.tap(args[0])
        elif cmd == 'combo':
            s.press(args[0]); time.sleep(0.2)
            for n in args[1:]: s.tap(n)
            s.release(args[0])
    finally:
        time.sleep(0.1); s.close()
