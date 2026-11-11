#!/usr/bin/env python3
"""Host side of the Neptune firmware debug command (builds NP5 and later).

  call(addr, a0, a1) -> r0     device calls a firmware function and returns its result
  peek32(addr), poke32(addr, value), read_mem(addr, n)

CLI:  nepdbg.py peek ADDR [N_WORDS] | poke ADDR VALUE | call ADDR [A0 [A1]]   (hex)
Addresses are true VMAs (file offset + 0x08008000).
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mido
from lkmidi import find_port

BASE = 0x08008000
PEEK32 = BASE + 0xA190    # ldr r0,[r0]; bx lr
POKE32 = BASE + 0x0196    # str r1,[r0]; bx lr

def _enc(v): return [(v >> s) & 0x7F for s in (28, 21, 14, 7, 0)]
def _dec(b): 
    v = 0
    for x in b: v = (v << 7) | x
    return v & 0xFFFFFFFF

class Dbg:
    def __init__(self):
        self.out = mido.open_output(find_port("MIDI", 'output'))
        self.inp = mido.open_input(find_port("MIDI", 'input'))
    def close(self): self.out.close(); self.inp.close()
    def call(self, addr, a0=0, a1=0, timeout=1.0):
        for _ in self.inp.iter_pending(): pass
        self.out.send(mido.Message('sysex', data=[0, 0x20, 0x29, 0, 0x7D] + _enc(addr | 1) + _enc(a0) + _enc(a1)))
        end = time.time() + timeout
        while time.time() < end:
            for m in self.inp.iter_pending():
                if m.type == 'sysex' and len(m.data) == 10 and list(m.data[:5]) == [0, 0x20, 0x29, 0, 0x7D]:
                    return _dec(m.data[5:10])
            time.sleep(0.001)
        raise TimeoutError(f"no reply to call {addr:#x}")
    def peek32(self, addr): return self.call(PEEK32, addr)
    def poke32(self, addr, value): self.call(POKE32, addr, value)
    def read_mem(self, addr, n):
        out = bytearray()
        for a in range(addr & ~3, addr + n, 4): out += self.peek32(a).to_bytes(4, 'little')
        return bytes(out[addr & 3:(addr & 3) + n])

if __name__ == "__main__":
    d = Dbg(); h = lambda s: int(s, 16)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'peek':
        a = h(args[0])
        for i in range(int(args[1]) if len(args) > 1 else 1): print(f"{a + 4*i:08x}: {d.peek32(a + 4*i):08x}")
    elif cmd == 'poke': d.poke32(h(args[0]), h(args[1])); print("ok")
    elif cmd == 'call': print(f"r0 = {d.call(h(args[0]), *(h(x) for x in args[1:3])):08x}")
    d.close()
