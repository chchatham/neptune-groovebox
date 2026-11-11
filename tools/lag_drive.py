#!/usr/bin/env python3
"""Drive the display-lag measurement: simulated encoder steps through the firmware's input simulation.

    lag_drive.py rtt                                   debug round-trip time (controls stay alive)
    lag_drive.py --out DIR goto tracks|fltr|param      Shift + pad (checks the pad's note came back)
    lag_drive.py --out DIR single --enc N --reps R --gap S [--cell NAME]
    lag_drive.py --out DIR burst  --enc N --rate HZ --steps K --reps R [--cell NAME]
    lag_drive.py --out DIR check                       short dead window: the three screens + 6 steps (look at the snaps)
    lag_drive.py --out DIR matrix [--reps 15] [--burst-reps 3] [--max-seconds 420]
    lag_drive.py off                                   simulation off (safe to run at any time)

Run `lagrec` (tools/lagrec.swift) first with the same DIR; this script appends to DIR/trials.csv (one line per
injected step, times from time.monotonic_ns(): the same clock lagrec uses) and DIR/events.csv. Analyse with
lag_analyze.py.

WHILE SIMULATION IS ON THE PHYSICAL CONTROLS ARE DEAD. Every command switches it off again when it ends, also
when it fails or is interrupted. Simulated encoder direction is not trustworthy: nothing here depends on it
(every cell is first turned to one end of its range and a counted number of reported steps back).
Only two firmware functions are ever called (simulation on/off, set raw reading) and only for whitelisted ids.
"""
import argparse, os, random, signal, statistics, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mido
from lkmidi import find_port
from nepdbg import Dbg
from nepinput import SIM_ENABLE, SIM_SET, IDS

SHIFT = IDS['shift']
ENC_IDS = list(range(0x38, 0x40))          # encoders 1-8: raw = quadrature phase 0-3
PAD_IDS = list(range(0x00, 0x10))          # pads: raw = 12-bit pressure
ALLOWED = set(ENC_IDS) | set(PAD_IDS) | {SHIFT}
# Shift + pad -> screen (docs/launchkey-mk4-control-map.md). Pad ids 0-7 = top row L->R (notes 96-103),
# 8-15 = bottom row (notes 112-119); `goto` checks the note that comes back and stops if it is another one.
SCREENS = {'param': (0x00, 96), 'tracks': (0x01, 97), 'fltr': (0x0D, 117)}
APP_PATTERN = "MacOS/Neptune Groovebox"


def app_running():
    return subprocess.run(["pgrep", "-f", APP_PATTERN], stdout=subprocess.DEVNULL).returncode == 0


class Driver:
    def __init__(self, out):
        self.out = out
        self.d = None
        self.sim = False
        self.phase = {i: 0 for i in ENC_IDS}
        self.daw = None
        self.trials = self.events = None
        self.t_on = None
        if out:
            os.makedirs(out, exist_ok=True)
            new = not os.path.exists(os.path.join(out, "trials.csv")) or os.path.getsize(os.path.join(out, "trials.csv")) == 0
            self.trials = open(os.path.join(out, "trials.csv"), "a")
            self.events = open(os.path.join(out, "events.csv"), "a")
            if new:
                self.trials.write("cell,screen,enc,pattern,rate,rep,step,dir,phase,t_before_ns,t_after_ns\n")
                self.events.write("t_ns,event\n")
            self.trials.flush(); self.events.flush()

    def event(self, text):
        print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)
        if self.events:
            self.events.write(f"{time.monotonic_ns()},{text}\n"); self.events.flush()

    def open(self):
        """Open the ports. Opening has hung once (2026-11-11, 14 s after the app was started), silently and for
        minutes: so a watchdog ends the process if it takes more than 15 s. Simulation is not on yet at this
        point, so ending here leaves the controls alive. Every debug call after this has its own 1 s time-out."""
        import threading
        done = threading.Event()

        def watchdog():
            if not done.wait(15):
                print("FAILED: opening the MIDI ports took more than 15 s (simulation was never switched on). "
                      "Check `pgrep -f \"ython.*-I\"` for a stuck tool and run again.", flush=True)
                os._exit(3)
        threading.Thread(target=watchdog, daemon=True).start()
        self.event("opening MIDI ports")
        self.d = Dbg()
        self.daw = mido.open_input(find_port("DAW", 'input'))
        done.set()
        self.event("ports open")

    def set(self, cid, raw):
        if cid not in ALLOWED or not 0 <= raw <= 0xFFF:
            raise ValueError(f"control id {cid:#x} / raw {raw:#x} not allowed")
        self.d.call(SIM_SET, cid, raw)

    def on(self):
        self.d.call(SIM_ENABLE, 1)
        self.sim = True
        self.t_on = time.monotonic()
        self.phase = {i: 0 for i in ENC_IDS}
        self.event("simulation ON (physical controls dead)")
        time.sleep(0.5)                               # entry jump: pitch bend, CC 1, all eight encoders

    def off(self):
        """Simulation off, whatever state we are in. Tries a fresh port if the open one fails."""
        ok = False
        for attempt in range(4):
            try:
                if self.d is None:
                    self.d = Dbg()
                self.d.call(SIM_ENABLE, 0)
                ok = True
                break
            except Exception as e:
                print("simulation off failed, retrying:", e, flush=True)
                try: self.d.close()
                except Exception: pass
                self.d = None
                time.sleep(0.5)
        if self.sim or self.t_on:
            dead = time.monotonic() - self.t_on if self.t_on else 0
            self.event(f"simulation OFF ok={ok} (controls were dead for {dead:.1f} s)")
        self.sim = False
        return ok

    def rtt(self, n=100, harmless_off=True):
        """Round trips of a call that changes nothing: 'simulation off' while it is off, or
        'release Shift' while simulation is on."""
        times = []
        for _ in range(n):
            t = time.monotonic_ns()
            if harmless_off: self.d.call(SIM_ENABLE, 0)
            else: self.set(SHIFT, 0)
            times.append((time.monotonic_ns() - t) / 1e6)
        times.sort()
        med, p99, worst = statistics.median(times), times[int(len(times) * 0.99) - 1], times[-1]
        self.event(f"rtt n={n} median_ms={med:.2f} p99_ms={p99:.2f} worst_ms={worst:.2f}")
        return med, p99, worst

    def snap(self):
        subprocess.run(["pkill", "-USR1", "-x", "lagrec"], stdout=subprocess.DEVNULL)

    def goto(self, screen):
        pad, note = SCREENS[screen]
        for _ in self.daw.iter_pending(): pass
        self.set(SHIFT, 0xFFF); time.sleep(0.2)
        self.set(pad, 0xE00); time.sleep(0.12)
        self.set(pad, 0); time.sleep(0.15)
        self.set(SHIFT, 0); time.sleep(0.35)
        notes = [m.note for m in self.daw.iter_pending() if m.type == 'note_on' and m.velocity > 0]
        self.event(f"goto {screen} pad_id={pad:#x} notes_seen={notes}")
        if notes != [note]:
            raise RuntimeError(f"goto {screen}: expected note {note} from pad id {pad:#x}, saw {notes}: wrong pad, stopping")
        self.snap(); time.sleep(0.15)

    def read_cc(self, enc):
        """Sum of the relative encoder steps (value - 64) the device has reported for this encoder since the last call."""
        return sum(m.value - 64 for m in self.daw.iter_pending()
                   if m.type == 'control_change' and m.channel == 15 and m.control == 0x54 + enc)

    def step(self, enc, direction, row):
        """One detent. Measured on NPD (2026-11-11) with this simulation:
        - the firmware reports a step when the raw phase arrives at 0 or 3;
        - by way of phase 1 to 3, or of phase 2 to 0, it has one sign (direction +1 here); by way of 2 to 3, or
          of 1 to 0, the other (direction -1). That is two calls, and is used only to park a value;
        - straight there (0 <-> 3, one call: direction 0 here) it carries on in the direction of the step
          before. Everything that is measured is direction 0, after parking has ended with direction -1;
        - the first step after a change of direction is swallowed (no message);
        - steps closer together than roughly 50 ms are reported accelerated, as several messages of 64 -+ 2 or 4
          about 5 ms apart: as a real fast turn is."""
        cid = ENC_IDS[enc - 1]
        target = 3 if self.phase[cid] == 0 else 0
        if direction > 0:
            self.set(cid, 1 if target == 3 else 2)
        elif direction < 0:
            self.set(cid, 2 if target == 3 else 1)
        t0 = time.monotonic_ns()
        self.set(cid, target)
        t1 = time.monotonic_ns()
        self.phase[cid] = target
        self.trials.write(f"{row},{direction},{target},{t0},{t1}\n")

    def run_steps(self, cell, screen, enc, pattern, rate, rep, n, direction, load=0):
        """n steps at `rate` per second. load > 0: also that many do-nothing debug calls per second in between
        (the control for 'is it the debug traffic that slows the display?')."""
        t0 = time.monotonic()
        next_load = t0 + 0.5 / load if load else None
        for k in range(n):
            due = t0 + k / rate
            while load and next_load < due - 0.008:
                wait = next_load - time.monotonic()
                if wait > 0: time.sleep(wait)
                self.set(SHIFT, 0)
                next_load += 1.0 / load
            wait = due - time.monotonic()
            if wait > 0: time.sleep(wait)
            self.step(enc, direction, f"{cell},{screen},{enc},{pattern},{rate},{rep},{k}")
        self.trials.flush()

    def move(self, cell, screen, enc, direction, units, rate=50, cap=260):
        """Step one way until the device has reported at least `units` steps (counts the reported steps,
        so acceleration does not matter)."""
        self.read_cc(enc)
        total, k, t0 = 0, 0, time.monotonic()
        while abs(total) < units and k < cap:
            wait = t0 + k / rate - time.monotonic()
            if wait > 0: time.sleep(wait)
            self.step(enc, direction, f"{cell},{screen},{enc},park,{rate},0,{k}")
            k += 1
            total += self.read_cc(enc)
            if k == 12 and total == 0:
                raise RuntimeError(f"encoder {enc}: 12 simulated steps and the device reported none")
        time.sleep(0.03)
        total += self.read_cc(enc)
        self.trials.flush()
        return total

    def park(self, cell, screen, enc, back):
        """To one end of the range (whichever end '+' is), then `back` steps away from it."""
        a = self.move(cell, screen, enc, +1, 115)
        time.sleep(0.1)
        b = self.move(cell, screen, enc, -1, back)
        if a * b >= 0:
            raise RuntimeError(f"encoder {enc}: both directions reported the same sign ({a}, {b})")
        return a, b

    def singles(self, cell, screen, enc, reps, gap):
        """Single steps, all the same way as the end of the parking (a change of direction would swallow one),
        starting 30 from one end."""
        a, b = self.park(cell, screen, enc, 30)
        self.event(f"singles cell={cell} screen={screen} enc={enc} reps={reps} gap={gap} parked={a}/{b}")
        time.sleep(1.5)
        for r in range(reps):
            time.sleep(random.uniform(0, 0.120))
            self.step(enc, 0, f"{cell},{screen},{enc},single,0,{r},0")
            self.trials.flush()
            time.sleep(gap)

    def bursts(self, cell, screen, enc, rate, steps, reps, pause=1.5, settle=1.3, load=0):
        """Each burst starts 10 steps from one end and turns away from it, so it cannot run into the other end
        (20 steps, at most 4 per step when accelerated). `settle`: the screens redraw once more 0.6 to 0.8 s after
        the last turn (momentary readouts going away), and that must be over before the burst starts."""
        name = f"burst{rate}" + (f"load{load}" if load else "")
        self.event(f"bursts cell={cell} screen={screen} enc={enc} rate={rate} steps={steps} reps={reps} load={load}")
        for r in range(reps):
            self.park(cell, screen, enc, 10)
            time.sleep(settle + random.uniform(0, 0.120))
            self.run_steps(cell, screen, enc, name, rate, r, steps, 0, load)
            time.sleep(pause)

    def close(self):
        for f in (self.trials, self.events):
            if f: f.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("rtt"); sub.add_parser("off"); sub.add_parser("check")
    g = sub.add_parser("goto"); g.add_argument("screen", choices=sorted(SCREENS))
    s = sub.add_parser("single")
    s.add_argument("--enc", type=int, required=True); s.add_argument("--reps", type=int, default=15)
    s.add_argument("--gap", type=float, default=0.5); s.add_argument("--cell", default="adhoc")
    b = sub.add_parser("burst")
    b.add_argument("--enc", type=int, required=True); b.add_argument("--rate", type=int, default=10)
    b.add_argument("--steps", type=int, default=20); b.add_argument("--reps", type=int, default=4)
    b.add_argument("--cell", default="adhoc"); b.add_argument("--load", type=int, default=0)
    m = sub.add_parser("matrix")
    m.add_argument("--reps", type=int, default=15); m.add_argument("--burst-reps", type=int, default=3)
    m.add_argument("--fast-rate", type=int, default=0, help="steps/s of the fast burst (default: from the round trip, at most 40)")
    m.add_argument("--max-seconds", type=float, default=420)
    a = ap.parse_args()
    if a.cmd in ("single", "burst") and not 1 <= a.enc <= 8:
        sys.exit("--enc is 1 to 8")

    if a.cmd == "off":
        ok = Driver(None).off()
        print("simulation off:", "ok" if ok else "FAILED", flush=True)
        os._exit(0 if ok else 1)
    if a.cmd != "rtt":
        if not a.out: sys.exit("--out DIR is needed")
        if not app_running(): sys.exit("The app is not running: start it first.")

    drv = Driver(a.out)

    def stop(signum, frame): raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGHUP, stop)
    code = 0
    try:
        drv.open()
        if a.cmd == "rtt":
            drv.rtt(100)
        elif a.cmd == "goto":
            drv.on(); drv.goto(a.screen)
        elif a.cmd == "single":
            drv.on(); drv.singles(a.cell, "?", a.enc, a.reps, a.gap)
        elif a.cmd == "burst":
            drv.on(); drv.bursts(a.cell, "?", a.enc, a.rate, a.steps, a.reps, load=a.load)
        elif a.cmd == "check":
            drv.on()
            for scr in ("param", "fltr", "tracks"):
                drv.goto(scr); time.sleep(0.6)
            drv.singles("check", "tracks", 1, 4, 1.0)
            drv.bursts("check", "tracks", 1, 10, 20, 1)
            drv.bursts("check", "tracks", 1, 10, 20, 1, load=40)
            drv.bursts("check", "tracks", 1, 40, 20, 1)
            drv.snap(); time.sleep(0.2)
        elif a.cmd == "matrix":
            med, p99, worst = drv.rtt(100)
            fast = a.fast_rate or max(25, min(40, int(0.6 * 1000 / max(p99, 1))))
            drv.event(f"fast burst rate {fast} steps/s")
            drv.on()
            deadline = time.monotonic() + a.max_seconds
            drv.rtt(50, harmless_off=False)
            # cell, encoder, gap between single steps, burst rates, do-nothing-call control
            plan = [("tracks", [("0-TRACKS-vol1", 1, 1.0, (10, 20, fast), True)]),
                    ("fltr",   [("1-FLTR-cut", 1, 1.0, (10, 20, fast), False), ("2-FLTR-reso", 2, 1.0, (), False)]),
                    ("param",  [("3-PARAM-cut", 1, 1.6, (10, fast), False), ("4-PARAM-atk", 5, 1.6, (10, fast), False)])]
            for screen, cells in plan:
                drv.goto(screen); time.sleep(0.5)
                for cell, enc, gap, rates, control in cells:
                    if time.monotonic() > deadline: raise RuntimeError("time budget used up: stopping")
                    drv.singles(cell, screen, enc, a.reps, gap)
                    time.sleep(0.5)
                    for rate in rates:
                        drv.bursts(cell, screen, enc, rate, 20, a.burst_reps)
                    if control:
                        drv.bursts(cell, screen, enc, 10, 20, a.burst_reps, load=40)
                    drv.snap(); time.sleep(0.3)
    except KeyboardInterrupt:
        print("interrupted", flush=True); code = 130
    except Exception as e:
        print("FAILED:", repr(e), flush=True); code = 1
    finally:
        try:
            if drv.sim and drv.d is not None:
                try: drv.set(SHIFT, 0)
                except Exception: pass
            if not drv.off(): code = code or 2
        finally:
            drv.close()
            sys.stdout.flush()
            os._exit(code)                            # mido inputs can hang a normal exit


if __name__ == "__main__":
    main()
