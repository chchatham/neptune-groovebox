#!/usr/bin/env python3
"""Flash with the on-device confirmation step.

Sequence: (caller sends 43 00 trigger) -> send 0x71 + 0x7C header -> device shows
"Entering Upgrade" with the right-arrow blinking -> USER PRESSES RIGHT ARROW ->
"Receiving App..." -> send data packets + final 0x73. Logs every message the
device sends, with timestamps.

Usage: flash_confirmed.py <firmware.syx> [--wait SECONDS] [--delay MS]
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mido
from lkmidi import find_port, read_syx_messages

def arg(name, default):
    return float(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default

def main():
    msgs = read_syx_messages(sys.argv[1])
    wait, delay = arg('--wait', 25.0), arg('--delay', 2.0) / 1000.0
    out_name, in_name = find_port("MIDI", 'output'), find_port("MIDI", 'input')
    daw_in = find_port("DAW", 'input')
    t0 = time.time(); log = []
    def cb(tag):
        def f(m):
            if m.type in ('clock', 'active_sensing'): return
            log.append((time.time() - t0, tag, m))
            d = ' '.join(f'{b:02X}' for b in m.data[:10]) if m.type == 'sysex' else str(m)
            if len(log) <= 40: print(f"  [{time.time()-t0:6.2f}s] {tag} <- {d}", flush=True)
        return f
    ins = [mido.open_input(in_name, callback=cb('MIDI'))]
    if daw_in: ins.append(mido.open_input(daw_in, callback=cb('DAW')))
    with mido.open_output(out_name) as out:
        retries = int(arg('--retries', 1)); go = False; seen = len(log)
        for attempt in range(retries):
            for m in msgs[:2]:
                out.send(m); time.sleep(0.05)
            print(f"header sent (attempt {attempt+1}/{retries}). PRESS THE BLINKING RIGHT ARROW (unless this build auto-confirms)...", flush=True)
            deadline = time.time() + wait
            while time.time() < deadline and not go:
                while seen < len(log):
                    _, _, m = log[seen]; seen += 1
                    if m.type != 'sysex': continue
                    if len(m.data) > 5 and m.data[4] == 0x73 and m.data[5] in (0x10, 0x11):
                        print(f"  device cancelled ({m.data[5]:#04x}: 0x11 = no button press, 0x10 = a control was touched too early)", flush=True); deadline = 0
                    elif len(m.data) > 4 and m.data[4] == 0x72:
                        print("  device sent 72 ready — starting data", flush=True); go = True
                time.sleep(0.02)
            if go: break
            time.sleep(1.0)
        if not go:
            print("no start signal from device within the wait; NOT sending data this run", flush=True)
            time.sleep(3)
            for i in ins: i.close()
            return
        time.sleep(0.3)
        print(f"sending {len(msgs)-2} data/final messages at {delay*1000:.1f} ms spacing...", flush=True)
        n1 = len(log); ts = time.time()
        for j, m in enumerate(msgs[2:]):
            out.send(m); time.sleep(delay)
            if (j + 1) % 1000 == 0: print(f"  sent {j+1}  device replies during data: {len(log)-n1}", flush=True)
        print(f"data sent in {time.time()-ts:.1f}s; listening 8s more", flush=True)
        time.sleep(8)
    for i in ins: i.close()
    sx = [m for _, _, m in log[n1:] if m.type == 'sysex']
    print(f"replies during/after data: {len(sx)} sysex, {len(log)-n1-len(sx)} other")
    for m in sx[-3:]: print("  last:", ' '.join(f'{b:02X}' for b in m.data[:10]))

if __name__ == "__main__":
    main()
