#!/usr/bin/env python3
"""The machine gate: parallel agents share one Mac, so builds and test runs queue for its cores here
instead of all compiling at once (each then runs slower than alone, and nobody finishes sooner).

The machine has TOKENS cores' worth of tokens. Every command takes some before it runs and gives
them back when it ends (also when it is killed: the locks are the kernel's):
    build   cmake --build <dir> --target <t> -j <TOKENS_BUILD>     takes TOKENS_BUILD tokens
    suite   the named suites side by side, each killed and reported  takes 2 tokens each
            HUNG after SUITE_LIMIT s; output in <dir>/suite-logs/
    full    the whole suite: ctest -j (TOKENS - 2) on <dir>         takes all but two tokens
    merge   <dir> <base commit>: the groups every change must pass  takes all but two tokens
            plus the sound groups of the files changed since <base>
            (MERGE_ALWAYS / MERGE_RULES below); the whole suite
            when a piece under every engine changed
    run     any other command                                       takes --tokens N (default 1)
A request that has waited WAIT_RESERVE seconds reserves the machine: smaller ones wait behind it,
so a full run is not starved by a stream of small ones.

    python3 tools/gate.py build build NeptuneTests
    python3 tools/gate.py build build NeptuneGroovebox
    python3 tools/gate.py suite build "U4 FM" "Mod Slots"
    python3 tools/gate.py full build
    python3 tools/gate.py merge build <commit before the merge>
    python3 tools/gate.py run --tokens 4 -- <command ...>

Every request appends one line to the log (who, what, seconds waited, seconds run), so the time an
agent spends waiting for the machine can be counted. `python3 tools/gate.py stats` sums it.
"""
import fcntl, json, os, subprocess, sys, threading, time

TOKENS = int(os.environ.get("NEPTUNE_GATE_TOKENS", os.cpu_count() or 8))
TOKENS_BUILD = int(os.environ.get("NEPTUNE_GATE_BUILD", max(2, TOKENS // 3)))
# A whole-suite or merge run leaves two cores to the agents, so one suite of theirs never waits
# behind it (2026-11-22: a 12 s suite waited two minutes behind the lead's full run).
TOKENS_RUN = max(1, TOKENS - 2)
WAIT_RESERVE = 90.0
# No suite takes three minutes by itself (the slowest, "Effect Destinations", about 100 s on a busy machine).
CTEST_LIMIT = int(os.environ.get("NEPTUNE_CTEST_LIMIT", 300))      # a ctest group (ctest's own default is 25 minutes)
SUITE_LIMIT = int(os.environ.get("NEPTUNE_SUITE_LIMIT", 240))
DIR = os.environ.get("NEPTUNE_GATE_DIR", "/tmp/neptune-gate")
LOG = os.path.join(DIR, "log.jsonl")

# A sanitizer's first report is symbolized by /usr/bin/atos -p <pid>, which on a Mac without
# developer mode (DevToolsSecurity -status) waits for a dialog nobody answers: the test process then
# sleeps for ever and keeps its tokens (2026-11-22: every queue behind it stood for 70 minutes; the
# report that sets it off is JUCE's own FLAC decoder, in "Robustness"). So everything started here
# is told to symbolize from the files instead, or not at all where llvm-symbolizer is missing.
_SYMBOLIZER = next((p for p in ("/opt/homebrew/opt/llvm/bin/llvm-symbolizer", "/usr/local/opt/llvm/bin/llvm-symbolizer")
                    if os.path.exists(p)), None)
for _var in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS"):
    _old = os.environ.get(_var, "")
    if "symbolize" not in _old and "external_symbolizer_path" not in _old:
        _add = "external_symbolizer_path=" + _SYMBOLIZER if _SYMBOLIZER else "symbolize=0"
        os.environ[_var] = (_old + ":" if _old else "") + _add


def _open(name):
    return os.open(os.path.join(DIR, name), os.O_RDWR | os.O_CREAT, 0o666)


class Gate:
    def __init__(self, need, label):
        os.makedirs(DIR, exist_ok=True)
        self.need = max(1, min(need, TOKENS))
        self.label = label
        self.held = []
        self.reserving = False

    def _try(self):
        mutex = _open("mutex")
        fcntl.flock(mutex, fcntl.LOCK_EX)
        try:
            # Someone else reserved the machine: wait behind them.
            res = _open("reserve")
            try:
                fcntl.flock(res, fcntl.LOCK_EX | fcntl.LOCK_NB)
                fcntl.flock(res, fcntl.LOCK_UN)
                reserved_by_other = False
            except BlockingIOError:
                reserved_by_other = not self.reserving
            finally:
                os.close(res)
            if reserved_by_other:
                return False
            got = []
            for i in range(TOKENS):
                fd = _open("token.%d" % i)
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    got.append(fd)
                    if len(got) == self.need:
                        break
                except BlockingIOError:
                    os.close(fd)
            if len(got) == self.need:
                self.held = got
                return True
            for fd in got:
                fcntl.flock(fd, fcntl.LOCK_UN)
                os.close(fd)
            return False
        finally:
            fcntl.flock(mutex, fcntl.LOCK_UN)
            os.close(mutex)

    def __enter__(self):
        self.start = time.time()
        self.resfd = None
        while not self._try():
            if not self.reserving and time.time() - self.start > WAIT_RESERVE:
                fd = _open("reserve")
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    self.resfd, self.reserving = fd, True
                except BlockingIOError:
                    os.close(fd)
            time.sleep(1.0)
        if self.resfd is not None:
            fcntl.flock(self.resfd, fcntl.LOCK_UN)
            os.close(self.resfd)
        self.waited = time.time() - self.start
        if self.waited > 5:
            print("[gate] waited %.0f s for %d of %d cores" % (self.waited, self.need, TOKENS), file=sys.stderr, flush=True)
        self.began = time.time()
        return self

    def __exit__(self, *exc):
        for fd in self.held:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)
        try:
            with open(LOG, "a") as f:
                f.write(json.dumps({"who": os.getcwd(), "what": self.label, "tokens": self.need,
                                    "waited": round(self.waited, 1), "ran": round(time.time() - self.began, 1),
                                    "at": round(self.began)}) + "\n")
        except OSError:
            pass


# ---- The merge run -------------------------------------------------------------------------------
# ctest names are "NeptuneTests-<group>" (CMakeLists.txt, NEPTUNE_TEST_GROUPS); the texts below are
# parts of those names. MERGE_ALWAYS is what every change must pass: the pin, the crash and thread
# guards, the screens / grammar / projects (the catch-all group), the click audit.
MERGE_ALWAYS = ["P01-Keyboard", "Robustness", "everything-else", "Click-Audit"]
# What a changed file adds: (part of its path, groups). First match wins; None = the whole suite
# (a piece every engine is built on). A source file no rule knows also means the whole suite.
_FX = ["One-Network", "Effect-Cores", "Send-Reverb", "Send-Chains", "Effect-Destinations", "Perf-FX", "Vocoder-XIN", "Effects-More"]
MERGE_RULES = [
    ("src/engine/ModRouter", None), ("src/engine/EngineBase", None), ("src/engine/ParamSpec", None),
    ("src/engine/VoiceAllocator", None), ("src/engine/VoiceFilterSection", None), ("src/engine/VoiceSpread", None),
    ("src/engine/EngineLfoSection", None), ("src/engine/EngineModSection", None), ("src/engine/AftertouchRouter", None),
    ("src/dsp/LFO", None), ("src/dsp/Envelope", None), ("src/dsp/PhaseOsc", None), ("src/dsp/DSPUtils", None),
    ("src/dsp/Saturation", None), ("src/dsp/filters/", None), ("src/dsp/TempoDivisions", None),
    ("src/engine/FM", ["U4-FM"]),
    ("Wavetable", ["U4-FM-U4-Wavetable", "Wavetable-Embed", "Wavetable-Keyframes"]), ("src/dsp/Wave", ["U4-FM-U4-Wavetable", "Wavetable-Embed", "Wavetable-Keyframes"]),
    ("WaveSeq", ["U4-Wave-Sequencer"]),
    ("Sampler", ["U4-Sampler", "Sampler-Stretch", "Sampler-Release", "Sampler-Grain"]), ("SampleBank", ["U4-Sampler", "Sampler-Release", "U4-Drum"]),
    ("Resampler", ["U4-Sampler"]), ("GrainScheduler", ["U4-Sampler", "Resonator", "U4-RSN"]),
    ("Drum", ["Kick-Points", "U4-Drum", "Cymbal-Machine", "Dead-Controls"]), ("Cymbal", ["U4-Drum", "Cymbal-Machine"]), ("OscBank", ["U4-Drum", "Cymbal-Machine"]),
    ("Resonator", ["Resonator", "U4-RSN", "U4-Drum"]), ("ModeBank", ["Resonator", "U4-RSN", "U4-Drum", "Vocoder-XIN"]),
    ("VoiceBody", ["Voice-Body", "U4-Macro"]),
    ("UnifiedVoice", ["U4-Unified", "Preset-Audition"]), ("ArchitecturePreset", ["U4-Unified", "Preset-Audition"]),
    ("MacroOsc", ["U4-Macro", "Macro-Osc-Extended"]),
    ("src/dsp/effects/", _FX), ("src/engine/PerfFx", _FX), ("src/engine/AudioEngine", _FX + ["Master-Chain", "Track-Input"]),
    ("src/engine/Metronome", []), ("src/engine/MIDITrack", []), ("src/engine/ModStripFollower", ["Mod-Wheel"]),
    ("Presets", ["Preset-Audition", "Mod-Wheel-Strip-Routes"]), ("MultiPreset", ["Preset-Audition"]), ("FactoryMultis", ["Preset-Audition"]),
    ("src/project/", []), ("src/ui/", []), ("src/controller/", []), ("src/sequencer/", []), ("src/arpeggiator/", []),
    ("src/sync/", []), ("src/plugin/", []), ("src/main.cpp", []),
]
# A changed test file runs its own group (the file name holds the suite's words).
MERGE_TEST_FILES = [("click_audit", "Click-Audit"), ("dead_controls", "Dead-Controls"), ("mod_wheel", "Mod-Wheel"),
                    ("strip_routes", "Mod-Wheel"), ("send_reverb", "Send-Reverb"), ("engine_lfo", "Engine-LFOs"),
                    ("sampler_stretch", "Sampler-Stretch"), ("preset_audition", "Preset-Audition"), ("cymbal", "Cymbal-Machine"),
                    ("lfo_destinations", "LFO-Destinations"), ("undo_history", "Undo-History"), ("perf_fx", "Perf-FX"),
                    ("u4_sampler", "U4-Sampler"), ("u4_fm", "U4-FM"), ("u4_wavetable", "U4-Wavetable"), ("u4_waveseq", "U4-Wave-Sequencer"),
                    ("resonator", "Resonator"), ("effect_cores", "Effect-Cores"), ("vocoder", "Vocoder"), ("xin_", "XIN"),
                    ("mod_slots", "Mod-Slots"), ("mod_router", "Mod-Router"), ("send_chain", "Send-Chains"),
                    ("u4_unified", "U4-Unified"), ("u4_rsn", "U4-RSN"), ("u4_drum", "U4-Drum"), ("kick_points", "Kick-Points"), ("saturation", "Saturation"), ("one_network", "One-Network"), ("pinned_on_cores", "One-Network"), ("track_input", "Track-Input"),
                    ("u4_macro", "U4-Macro"), ("effect_destinations", "Effect-Destinations")]


def merge_groups(paths):
    """The ctest name parts to run for these changed files, and why; (None, reason) = everything."""
    groups, why = list(MERGE_ALWAYS), []
    for p in paths:
        if p.startswith("tests/"):
            for part, g in MERGE_TEST_FILES:
                if part in p and g not in groups:
                    groups.append(g); why.append(g)
            continue
        if not p.startswith("src/"):
            continue                                   # notes, docs, tools, the build file
        for part, add in MERGE_RULES:
            if part in p:
                if add is None:
                    return None, p + " is under every engine"
                for g in add:
                    if g not in groups:
                        groups.append(g); why.append(g)
                break
        else:
            return None, p + " is in no rule of tools/gate.py"
    return groups, why


def reap():
    """Kill every NeptuneTests process on the machine that has run longer than any test may.

    Whatever started it (an older copy of this file in a worktree, a bare run, a tool call that timed
    out and left it behind), a test binary older than the group limit is looping: on 2026-11-22 four
    such processes spun for up to 33 minutes behind an agent that kept starting new ones."""
    try:
        out = subprocess.run(["ps", "-eo", "pid=,etime=,command="], capture_output=True, text=True).stdout
    except OSError:
        return
    for line in out.splitlines():
        try:
            pid, etime, command = line.split(None, 2)
            if "NeptuneTests_artefacts" not in command.split()[0]:
                continue                               # the test binary itself, not a shell that names it
            days, _, clock = etime.rpartition("-")     # macOS: [[dd-]hh:]mm:ss (it has no "etimes")
            seconds = 0
            for part in clock.split(":"):
                seconds = seconds * 60 + int(part)
            seconds += int(days or 0) * 86400
            if seconds > CTEST_LIMIT + 60:
                os.kill(int(pid), 9)
                print("[gate] killed a test process that had run %d s: pid %s" % (seconds, pid), file=sys.stderr, flush=True)
        except (ValueError, IndexError, OSError):
            continue                                   # a line that is not what was expected: never fail the gate for it


def run(cmd, need, label, env=None):
    with Gate(need, label):
        return subprocess.call(cmd, env=env)


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    verb, rest = argv[0], argv[1:]
    reap()
    if verb == "build":
        d, targets = rest[0], rest[1:] or ["NeptuneTests"]
        code = 0
        for t in targets:
            code = code or run(["cmake", "--build", d, "--target", t, "-j", str(TOKENS_BUILD)], TOKENS_BUILD, "build " + t)
        return code
    if verb == "suite":
        # The named suites run SIDE BY SIDE (two tokens each), each with a time limit: a suite that
        # passes it is killed and reported HUNG with the test it was in. On 2026-11-22 an agent waited
        # 24 minutes on a test that looped for ever, and eight suites one after the other took ten.
        # Every suite's whole output is kept in <dir>/suite-logs/<name>.log: read that, do not run again.
        d, names = rest[0], rest[1:]
        binary = None
        for cfg in ("Debug", "Release"):
            p = os.path.join(d, "NeptuneTests_artefacts", cfg, "NeptuneTests")
            if os.path.exists(p):
                binary = p
                break
        if binary is None:
            print("[gate] no NeptuneTests binary under " + d, file=sys.stderr)
            return 2
        logs = os.path.join(d, "suite-logs")
        os.makedirs(logs, exist_ok=True)
        results = {}

        def one(n):
            env = dict(os.environ, NEPTUNE_TEST_ONLY=n)
            log = os.path.join(logs, "".join(c if c.isalnum() else "-" for c in n) + ".log")
            with Gate(2, "suite " + n):
                began = time.time()
                with open(log, "w") as f:
                    proc = subprocess.Popen([binary], env=env, stdout=f, stderr=subprocess.STDOUT)
                    try:
                        code = proc.wait(timeout=SUITE_LIMIT)
                    except subprocess.TimeoutExpired:
                        proc.kill(); proc.wait(); code = "HUNG"
                took = time.time() - began
            out = open(log, errors="replace").read().splitlines()
            failures = [l for l in out if l.startswith("!!!")]
            last = ([l[19:] for l in out if l.startswith("Starting tests in:")] or ["?"])[-1]
            results[n] = (code, failures, last, took, log)

        threads = [threading.Thread(target=one, args=(n,)) for n in names]
        for t in threads: t.start()
        for t in threads: t.join()
        code = 0
        for n in names:
            c, failures, last, took, log = results[n]
            for l in failures[:40]:
                print(l)
            if len(failures) > 40:
                print("    ... %d more in %s" % (len(failures) - 40, log))
            if c == "HUNG":
                print("[suite] %s: HUNG, killed after %d s inside: %s" % (n, SUITE_LIMIT, last[:160]))
            else:
                print("[suite] %s: %d failures%s (%.0f s; log %s)" % (n, len(failures), "" if c == 0 else " (exit %s)" % c, took, log))
            code = code or (1 if failures or c != 0 else 0)
        return code
    if verb == "full":
        d = rest[0]
        return run(["ctest", "--test-dir", d, "-j", str(TOKENS_RUN), "--output-on-failure", "--timeout", str(CTEST_LIMIT)], TOKENS_RUN, "full ctest")
    if verb == "merge":
        # The run after a merge: the groups every change must pass, and the sound groups of whatever
        # the merged commits touched (base..HEAD of the checkout that holds <dir>).
        d, base = rest[0], rest[1]
        diff = subprocess.run(["git", "-C", os.path.dirname(os.path.abspath(d)), "diff", "--name-only", base, "HEAD"],
                              capture_output=True, text=True)
        # No answer from git (a wrong commit name) must never read as "nothing changed".
        groups, why = merge_groups(diff.stdout.split()) if diff.returncode == 0 else (None, "git could not compare with '%s'" % base)
        if groups is None:
            print("[merge] %s: the whole suite" % why, flush=True)
            return run(["ctest", "--test-dir", d, "-j", str(TOKENS_RUN), "--output-on-failure", "--timeout", str(CTEST_LIMIT)], TOKENS_RUN, "merge ctest (whole)")
        print("[merge] always: %s\n[merge] for this change: %s" % (" ".join(MERGE_ALWAYS), " ".join(why) or "nothing more"), flush=True)
        return run(["ctest", "--test-dir", d, "-j", str(TOKENS_RUN), "--output-on-failure", "--timeout", str(CTEST_LIMIT), "-R", "|".join(groups)],
                   TOKENS_RUN, "merge ctest")
    if verb == "run":
        need = 1
        if rest[:1] == ["--tokens"]:
            need, rest = int(rest[1]), rest[2:]
        if rest[:1] == ["--"]:
            rest = rest[1:]
        return run(rest, need, "run " + " ".join(rest)[:80])
    if verb == "stats":
        rows = [json.loads(l) for l in open(LOG)] if os.path.exists(LOG) else []
        by = {}
        for r in rows:
            k = os.path.basename(r["who"].rstrip("/"))
            w = by.setdefault(k, [0, 0.0, 0.0])
            w[0] += 1; w[1] += r["waited"]; w[2] += r["ran"]
        for k, (n, waited, ran) in sorted(by.items()):
            print("%-40s %4d requests  waited %7.0f s  ran %7.0f s" % (k, n, waited, ran))
        return 0
    print("unknown verb " + verb, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
