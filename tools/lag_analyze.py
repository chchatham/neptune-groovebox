#!/usr/bin/env python3
"""Analyse one display-lag recording (lagrec + lag_drive): writes results.csv and summary.md into the folder.

    lag_analyze.py DIR [--note "text for the top of the summary"]

All times are on the host clock (ns). Per trial:
  t_inj   just before the call that completes a simulated encoder step
  t_cc    the encoder message the Launchkey sent for it (what the app reacts to)
  ready   the display's "ready for the next frame" replies after t_cc (one per frame the app sent)
  glass   camera: first frame in which the display differs from the picture before the trial ("first"),
          and the first frame from which it no longer differs from the final picture ("settled")
Camera times are the time stamp of the first frame that SHOWS the change: the change happened somewhere in the
frame interval before it (33 ms at 30 fps), so they read late by 0 to one frame interval, about half on average.
Standard library only.
"""
import csv, math, os, sys

W, H = 128, 64
BANDS = {"top": (0, 16), "mid": (16, 46), "bot": (46, 64)}
READY = bytes.fromhex("F0002029021309F7")
MS = 1e6


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, math.ceil(p * len(v)) - 1))] if v else None


def stat(v):
    v = [x for x in v if x is not None]
    if not v: return "–"
    return f"{pct(v, 0.5):.0f} / {pct(v, 0.9):.0f} / {max(v):.0f}"


def load_midi(d):
    """-> (encoder messages [(t, enc 1-8, delta)], ready times, debug reply times)."""
    daw = 1
    for line in open(os.path.join(d, "meta.txt")):
        p = line.split()
        if p and p[0] == "midi_source" and "DAW" in line: daw = int(p[1])
    enc, ready, dbg = [], [], []
    sysex = {}                                      # per source: bytes of an unfinished SysEx
    status = {}
    pending = {}
    for row in csv.DictReader(open(os.path.join(d, "midi.csv"))):
        t, src = int(row["host_ns"]), int(row["source"])
        for b in bytes.fromhex(row["hex"]):
            if b >= 0xF8: continue                  # clock and other real-time bytes
            if b == 0xF0: sysex[src] = bytearray([b]); continue
            if src in sysex:
                sysex[src].append(b)
                if b == 0xF7:
                    m = bytes(sysex.pop(src))
                    if src == daw and m == READY: ready.append(t)
                    elif m[:6] == bytes.fromhex("F0002029007D"): dbg.append(t)
                continue
            if b & 0x80: status[src] = b; pending[src] = []; continue
            if src not in status: continue
            pending[src].append(b)
            need = 1 if (status[src] & 0xF0) in (0xC0, 0xD0) else 2
            if len(pending[src]) == need:
                if src == daw and status[src] == 0xBF and 0x55 <= pending[src][0] <= 0x5C:
                    enc.append((t, pending[src][0] - 0x54, pending[src][1] - 64))
                pending[src] = []
    return enc, ready, dbg


class Frames:
    def __init__(self, d):
        self.pts = []
        path = os.path.join(d, "frames.csv")
        self.ok = os.path.exists(path) and os.path.getsize(path) > 40
        if not self.ok: return
        for row in csv.DictReader(open(path)): self.pts.append(int(row["pts_ns"]))
        self.f = open(os.path.join(d, "frames.bin"), "rb")
        self.cache = {}
        dt = sorted(b - a for a, b in zip(self.pts, self.pts[1:]))
        self.interval = dt[len(dt) // 2] / MS if dt else 0
        self.max_gap = dt[-1] / MS if dt else 0

    def get(self, i):
        if i not in self.cache:
            self.f.seek(i * W * H); self.cache[i] = self.f.read(W * H)
            if len(self.cache) > 600: self.cache.pop(next(iter(self.cache)))
        return self.cache[i]

    def span(self, t0, t1):
        import bisect
        return range(bisect.bisect_left(self.pts, t0), bisect.bisect_right(self.pts, t1))


def mean_image(fr, idx):
    n = len(idx)
    imgs = [fr.get(i) for i in idx]
    return [sum(im[k] for im in imgs) / n for k in range(W * H)]


BW = 4                                              # block size: the picture is compared in 4 x 4 blocks


def block_resid(img, ref):
    """Compare a frame with a reference picture, robust against the camera's flicker.
    The display is scanned and the camera's exposure is short, so every frame has dark bands that move from
    frame to frame: whole rows are dimmer or brighter by up to a third. Each row of the frame is therefore
    fitted to the same row of the reference with one gain (least squares), and what is left over is averaged
    over 4 x 4 blocks. Returns the 32 x 16 block means (row-major)."""
    out = [0.0] * ((W // BW) * (H // BW))
    for y in range(H):
        o = y * W
        f = img[o:o + W]; r = ref[o:o + W]
        rr = sum(v * v for v in r)
        g = (sum(x * v for x, v in zip(f, r)) / rr) if rr > 0 else 1.0
        ob = (y // BW) * (W // BW)
        for x in range(W):
            out[ob + x // BW] += f[x] - g * r[x]
    return [v / (BW * BW) for v in out]


def changed(res, thr):
    """Blocks whose mean differs by more than thr: whole picture and per band."""
    out = {}
    per_row = W // BW
    total = 0
    for name, (y0, y1) in BANDS.items():
        c = sum(1 for k in range((y0 // BW) * per_row, (y1 // BW) * per_row) if abs(res[k]) > thr)
        out[name] = c; total += c
    out["all"] = total
    return out


def confirmed(counts, j, key="all"):
    """A change counts when it is in two frames running, or is large (3 blocks or more)."""
    c = counts[j][key]
    if c == 0: return False
    if c >= 3: return True
    return (j + 1 < len(counts) and counts[j + 1][key] > 0) or (j > 0 and counts[j - 1][key] > 0)


def glass(fr, t_inj, t_end, final_from):
    """Camera view of one trial. Baseline = the frames in the 200 ms before t_inj; final = the frames in
    [final_from, t_end]. Returns times (ns) or None."""
    r = {"first": None, "settled": None, "first_top": None, "first_mid": None, "first_bot": None,
         "changed_px": 0, "runs": 0, "noise": 0}
    if not fr.ok: return r
    base_idx = list(fr.span(t_inj - 200 * MS, t_inj - 8 * MS))
    idx = list(fr.span(t_inj - 8 * MS, t_end))
    fin_idx = [i for i in idx if fr.pts[i] >= final_from]
    if len(base_idx) < 4 or len(idx) < 4 or len(fin_idx) < 3: return r
    base = mean_image(fr, base_idx)
    final = mean_image(fr, fin_idx)
    # Noise: how far single still frames stray from the mean of the still frames (before, and at the end).
    noise = max(max(abs(v) for v in block_resid(fr.get(i), base)) for i in base_idx)
    noise = max(noise, max(max(abs(v) for v in block_resid(fr.get(i), final)) for i in fin_idx))
    thr = max(6.0, 2.0 * noise)
    r["noise"] = round(noise, 1)
    counts = [changed(block_resid(fr.get(i), base), thr) for i in idx]
    for j in range(len(idx)):
        if r["first"] is None and confirmed(counts, j): r["first"] = fr.pts[idx[j]]
        for b in BANDS:
            if r["first_" + b] is None and confirmed(counts, j, b): r["first_" + b] = fr.pts[idx[j]]
    r["changed_px"] = changed(block_resid(final, base), thr)["all"]
    if r["first"] is None: return r
    fc = [changed(block_resid(fr.get(i), final), thr) for i in idx]
    last_bad = max((j for j in range(len(idx)) if confirmed(fc, j)), default=-1)
    if last_bad + 1 < len(idx) and fr.pts[idx[last_bad + 1]] < final_from: r["settled"] = fr.pts[idx[last_bad + 1]]
    # Runs of frame-to-frame change: a rough count of separate updates of the glass (two updates closer
    # together than two camera frames merge into one run, so this is a lower bound).
    prev, inrun = None, False
    for i in idx:
        cur = fr.get(i)
        if prev is not None:
            ch = changed(block_resid(cur, prev), 1.7 * thr)["all"] >= 2
            if ch and not inrun: r["runs"] += 1
            inrun = ch
        prev = cur
    return r


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    d = sys.argv[1]
    note = sys.argv[sys.argv.index("--note") + 1] if "--note" in sys.argv else ""
    enc, ready, dbg = load_midi(d)
    fr = Frames(d)
    steps = list(csv.DictReader(open(os.path.join(d, "trials.csv"))))
    for s in steps:
        s["t0"], s["t1"] = int(s["t_before_ns"]), int(s["t_after_ns"])
    all_inj = sorted(s["t0"] for s in steps)
    trials = {}
    for s in steps:
        if s["pattern"] in ("park", "preroll"): continue
        trials.setdefault((s["cell"], s["pattern"], int(s["rep"])), []).append(s)

    import bisect
    ready.sort(); enc.sort()
    rows = []
    step_delays = []                                 # injection -> message, every measured step
    for (cell, pattern, rep), ss in trials.items():
        ss.sort(key=lambda s: s["t0"])
        e = int(ss[0]["enc"])
        t_first, t_last = ss[0]["t0"], ss[-1]["t0"]
        nxt = bisect.bisect_right(all_inj, t_last)
        t_next = all_inj[nxt] if nxt < len(all_inj) else t_last + 5000 * MS
        ccs = [(t, dv) for t, en, dv in enc if en == e and t_first - 1 * MS <= t <= t_last + 300 * MS and t < t_next - 6 * MS]
        row = {"cell": cell, "pattern": pattern, "rep": rep, "enc": e, "steps": len(ss), "cc_count": len(ccs),
               "cc_units": sum(abs(dv) for _, dv in ccs), "flags": []}
        rows.append(row)
        row["call_ms"] = max((s["t1"] - s["t0"]) / MS for s in ss)
        if not ccs:
            row["flags"].append("NO_CC"); continue
        if len(ccs) != len(ss): row["flags"].append("CC_COUNT")
        else:
            for s, (t, _) in zip(ss, ccs): step_delays.append((t - s["t0"]) / MS)
        t_cc, t_cc_last = ccs[0][0], ccs[-1][0]
        row["inj_to_cc_ms"] = (t_cc - t_first) / MS
        single = pattern == "single"
        # How long the trial's own window lasts: to the next injected step, at most 0.52 s after the last message
        # (the screens redraw again 0.6 to 1 s after the last turn, when the momentary readout or picture goes
        # away: that is a second change, not lag).
        t_end = min(t_next - 30 * MS, t_cc_last + 520 * MS)
        rd = [t for t in ready if t_cc < t <= t_end]
        before = [t for t in ready if t_cc - 100 * MS <= t <= t_cc + 60 * MS]
        if before: row["flags"].append("BUSY")       # a frame was already in flight (the 5 s re-send, or earlier work)
        own = [t for t in rd if t > t_cc + 60 * MS] if single else rd
        row["ready_count"] = len(own)
        if own:
            row["cc_to_first_ready_ms"] = (own[0] - t_cc) / MS
            row["cc_to_last_ready_ms"] = (own[-1] - t_cc) / MS
            row["lastcc_to_last_ready_ms"] = (own[-1] - t_cc_last) / MS
            gaps = [(b - a) / MS for a, b in zip(own, own[1:])]
            if gaps: row["ready_gap_max_ms"] = max(gaps); row["ready_gap_med_ms"] = pct(gaps, 0.5)
        else:
            row["flags"].append("NO_READY")
        if not single:
            dur = (t_cc_last - t_cc) / 1e9
            during = [t for t in rd if t <= t_cc_last + 80 * MS]
            row["burst_s"] = dur
            row["frames_during"] = len(during)
            row["frames_per_s"] = len(during) / (dur + 0.08) if dur > 0 else None
        g = glass(fr, t_first, t_end, t_end - 170 * MS)
        row["noise"] = g["noise"]; row["changed_blocks"] = g["changed_px"]; row["glass_runs"] = g["runs"]
        if fr.ok and g["first"] is None: row["flags"].append("NO_GLASS_CHANGE")
        for k in ("first", "first_top", "first_mid", "first_bot", "settled"):
            if g[k] is not None: row["glass_" + k + "_ms"] = (g[k] - t_cc) / MS
        if g["settled"] is not None:
            row["lastcc_to_settled_ms"] = (g["settled"] - t_cc_last) / MS
            if g["first"] is not None: row["first_to_settled_ms"] = (g["settled"] - g["first"]) / MS

    cols = ["cell", "pattern", "rep", "enc", "steps", "cc_count", "cc_units", "inj_to_cc_ms", "call_ms", "ready_count",
            "cc_to_first_ready_ms", "cc_to_last_ready_ms", "lastcc_to_last_ready_ms", "ready_gap_med_ms", "ready_gap_max_ms",
            "burst_s", "frames_during", "frames_per_s", "glass_first_ms", "glass_first_top_ms", "glass_first_mid_ms",
            "glass_first_bot_ms", "glass_settled_ms", "first_to_settled_ms", "lastcc_to_settled_ms", "glass_runs",
            "changed_blocks", "noise", "flags"]
    with open(os.path.join(d, "results.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(cols)
        for r in rows:
            w.writerow([("|".join(r[c]) if c == "flags" else (f"{r[c]:.1f}" if isinstance(r.get(c), float) else r.get(c, ""))) for c in cols])

    def col(rs, c): return [r[c] for r in rs if r.get(c) is not None]
    out = ["# Display lag: " + os.path.basename(os.path.abspath(d)), ""]
    if note: out += [note, ""]
    for p in ("build.txt",):
        if os.path.exists(os.path.join(d, p)): out += ["Release binary (modified, size): " + open(os.path.join(d, p)).read().strip(), ""]
    if fr.ok:
        out += [f"Camera: {len(fr.pts)} frames, {fr.interval:.1f} ms apart (longest gap {fr.max_gap:.1f} ms). A camera time is the first "
                f"frame that shows the change, so it reads late by 0 to {fr.interval:.0f} ms (about {fr.interval / 2:.0f} on average) "
                "plus the camera's own exposure; compare rows with each other rather than reading them as absolutes.", ""]
    else:
        out += ["No camera frames: MIDI-only measurement.", ""]
    ev = os.path.join(d, "events.csv")
    if os.path.exists(ev):
        out += ["Debug round trips and events:", ""] + ["- " + l.strip().split(",", 1)[1] for l in open(ev)
                                                         if "rtt" in l or "simulation" in l or "fast burst" in l] + [""]
    if step_delays:
        out += [f"Injection → encoder message, all {len(step_delays)} measured steps: median {pct(step_delays, 0.5):.1f} ms, "
                f"p90 {pct(step_delays, 0.9):.1f}, p99 {pct(step_delays, 0.99):.1f}, worst {max(step_delays):.1f} ms. "
                f"Steps later than 15 ms: {sum(1 for x in step_delays if x > 15)}.", ""]
    all_gaps = sorted((b - a) / MS for a, b in zip(ready, ready[1:]) if (b - a) / MS < 400)
    close = [g for g in all_gaps if g < 130]
    if close:
        out += [f"Ready replies: {len(ready)} in the recording. Back-to-back frames (gap under 130 ms, n = {len(close)}): "
                f"median {pct(close, 0.5):.1f} ms, p90 {pct(close, 0.9):.1f}, longest {max(close):.1f}; "
                f"gaps of 130 to 400 ms: {len(all_gaps) - len(close)}.", ""]

    log = os.path.join(d, "app.log")
    if os.path.exists(log):
        disp = [l.strip() for l in open(log, errors="replace") if l.startswith("[Display]")]
        if disp:
            out += [f"App log: first `{disp[0]}`; last `{disp[-1]}` (the time-out count is a running total: "
                    "a frame the device did not acknowledge within 200 ms).", ""]
    cells = sorted({r["cell"] for r in rows})
    out += ["## Single steps (median / p90 / worst, ms, from the encoder message)", "",
            "| cell | n | no msg | busy | frames per step | → first ready | → last ready | → glass first | top / mid / bottom band first (median) | → glass settled | first → settled | no glass change |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for c in cells:
        rs = [r for r in rows if r["cell"] == c and r["pattern"] == "single"]
        if not rs: continue
        good = [r for r in rs if "NO_CC" not in r["flags"]]
        clean = [r for r in good if "BUSY" not in r["flags"]]
        rc = col(clean, "ready_count")
        bands = " / ".join((f"{pct(col(clean, 'glass_first_' + b + '_ms'), 0.5):.0f}" if col(clean, 'glass_first_' + b + '_ms') else "–") for b in BANDS)
        out.append(f"| {c} | {len(rs)} | {len(rs) - len(good)} | {len(good) - len(clean)} | "
                   f"{(sum(rc) / len(rc)) if rc else 0:.2f} (max {max(rc) if rc else 0}) | {stat(col(clean, 'cc_to_first_ready_ms'))} | "
                   f"{stat(col(clean, 'cc_to_last_ready_ms'))} | {stat(col(clean, 'glass_first_ms'))} | {bands} | "
                   f"{stat(col(clean, 'glass_settled_ms'))} | {stat(col(clean, 'first_to_settled_ms'))} | "
                   f"{sum(1 for r in clean if 'NO_GLASS_CHANGE' in r['flags'])} |")
    out += ["", "Rows marked busy (a frame was already on its way when the step arrived: the 5 s re-send, or earlier work) are left out of the statistics.", ""]
    out += ["## Bursts of steps (median / worst over the repetitions; ms from the LAST encoder message unless said)", "",
            "| cell | rate asked | n | steps → messages (units) | burst length s | frames during burst | frames / s | ready gap median / longest | first msg → first ready | first msg → glass first | last msg → last ready | last msg → glass settled |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|"]

    def mw(v, fmt="{:.0f}"):
        v = [x for x in v if x is not None]
        return (fmt.format(pct(v, 0.5)) + " / " + fmt.format(max(v))) if v else "–"
    for c in cells:
        for pat in sorted({r["pattern"] for r in rows if r["cell"] == c and r["pattern"] != "single"}, key=lambda p: (int(p[5:].split("load")[0]), "load" in p)):
            rs = [r for r in rows if r["cell"] == c and r["pattern"] == pat and "NO_CC" not in r["flags"]]
            if not rs: continue
            out.append(f"| {c} | {pat[5:].replace('load', '/s + do-nothing calls ')}/s | {len(rs)} | {sum(r['steps'] for r in rs)} → {sum(r['cc_count'] for r in rs)} ({sum(r['cc_units'] for r in rs)}) | "
                       f"{mw(col(rs, 'burst_s'), '{:.2f}')} | {mw(col(rs, 'frames_during'))} | {mw(col(rs, 'frames_per_s'), '{:.1f}')} | "
                       f"{mw(col(rs, 'ready_gap_med_ms'))[:mw(col(rs, 'ready_gap_med_ms')).find(' /')]} / {max(col(rs, 'ready_gap_max_ms'), default=0):.0f} | "
                       f"{mw(col(rs, 'cc_to_first_ready_ms'))} | {mw(col(rs, 'glass_first_ms'))} | "
                       f"{mw(col(rs, 'lastcc_to_last_ready_ms'))} | {mw(col(rs, 'lastcc_to_settled_ms'))} |")
    flagged = [r for r in rows if r["flags"]]
    out += ["", f"## Flags ({len(flagged)} of {len(rows)} trials)", ""]
    out += [f"- {r['cell']} {r['pattern']} rep {r['rep']}: {', '.join(r['flags'])}" for r in flagged] or ["- none"]
    out += ["", "Flags: NO_CC = the device sent no encoder message for the step; CC_COUNT = messages ≠ steps in a burst; "
            "BUSY = a ready reply within 100 ms before to 60 ms after the message (a frame was in flight); "
            "NO_READY = no frame followed; NO_GLASS_CHANGE = the camera saw nothing change."]
    open(os.path.join(d, "summary.md"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
