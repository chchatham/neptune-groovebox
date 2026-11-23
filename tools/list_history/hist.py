#!/usr/bin/env python3
"""hist.py: dump the named lists at commits of the history and find where they changed.

    hist.py dump <commit> ...      dump these commits (cached)
    hist.py bisect                 find every change point between v0.9 and HEAD (cached dumps reused)
    hist.py report                 print the change points found so far
"""
import concurrent.futures, json, os, subprocess, sys, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.environ.get("NEPTUNE_LIST_HISTORY", WT + "/build/list-history")
DUMPS = WORK + "/dumps"
TREES = WORK + "/trees"
PATHS = ["src/engine", "src/dsp/LFO.h", "src/dsp/filters/FilterSlot.h", "src/dsp/TempoDivisions.h", "src/dsp/Resonator.h"]

def git(*a):
    return subprocess.run(["git", "-C", WT] + list(a), capture_output=True, text=True, check=True).stdout

def order():
    # (the tag v0.9 is the public repository's single commit, not an ancestor of main: its tree is c210682's)
    revs = git("rev-list", "--reverse", "c210682..HEAD", "--", *PATHS).split()
    return [git("rev-parse", "c210682").strip()] + revs

def dump(commit):
    out = f"{DUMPS}/{commit[:10]}.txt"
    bad = out + ".failed"
    if os.path.exists(out) or os.path.exists(bad):
        return out if os.path.exists(out) else None
    tree = f"{TREES}/{commit[:10]}"
    os.makedirs(tree, exist_ok=True)
    tar = subprocess.Popen(["git", "-C", WT, "archive", commit, "src"], stdout=subprocess.PIPE)
    subprocess.run(["tar", "-x", "-C", tree], stdin=tar.stdout, check=True)
    tar.wait()
    r = subprocess.run(["python3", WT + "/tools/gate.py", "run", "--tokens", "1", "--", "python3", HERE + "/dumpat.py", tree, out],
                       capture_output=True, text=True)
    shutil.rmtree(tree, ignore_errors=True)
    if not os.path.exists(out) or os.path.getsize(out) == 0:
        open(bad, "w").write(r.stdout[-3000:] + r.stderr[-6000:])
        if os.path.exists(out):
            os.remove(out)
        return None
    return out

def named(cell):
    """Is this Choice cell one of the lists a file stores a position of and that this work names?"""
    label, choices = cell["label"], cell["choices"]
    if "PTCH" in choices or "LFO1" in choices or "SVF" in choices:
        return True
    return label in ("SHPE", "TRIG", "DIV", "SHP", "FLT1", "FLT2")

def parse(path):
    cells, lists, ids = {}, {}, {}
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if f[0] == "CELL":
            e, page, pname, pid, label, kind, lo, hi, n = f[1:10]
            choices = f[10].split("|") if len(f) > 10 and f[10] else []
            ids.setdefault(e, set()).add(int(pid))
            c = {"label": label, "choices": choices, "page": pname}
            if int(kind) == 3 and named(c):
                cells[(e, int(pid))] = choices
        elif f[0] == "LIST":
            lists[f[1]] = f[3].split("|")
    return cells, lists, ids

def signature(path):
    cells, lists, ids = parse(path)
    # the distinct lists of each engine (which ids show them does not matter here), and the LISTs
    sig = {}
    for (e, pid), choices in cells.items():
        sig.setdefault(e, set()).add("|".join(choices))
    return json.dumps({"cells": {e: sorted(v) for e, v in sorted(sig.items())}, "lists": lists}, sort_keys=True)

def main():
    os.makedirs(DUMPS, exist_ok=True)
    os.makedirs(TREES, exist_ok=True)
    revs = order()
    if sys.argv[1] == "dump":
        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            for c, o in zip(sys.argv[2:], pool.map(dump, [git("rev-parse", c).strip() for c in sys.argv[2:]])):
                print(c, o)
        return
    sigs = {}
    def sig(i):
        if i not in sigs:
            p = dump(revs[i])
            sigs[i] = signature(p) if p else None
        return sigs[i]
    if sys.argv[1] == "bisect":
        # breadth first, four at a time
        todo = [(0, len(revs) - 1)]
        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            list(pool.map(sig, [0, len(revs) - 1]))
            while todo:
                nxt, mids = [], []
                for lo, hi in todo:
                    if hi - lo <= 1 or (sig(lo) is not None and sig(lo) == sig(hi)):
                        continue
                    mid = (lo + hi) // 2
                    mids.append(mid)
                    nxt += [(lo, mid), (mid, hi)]
                list(pool.map(sig, mids))
                # a commit that does not build: its neighbours decide
                todo = nxt
                print("round done:", len(mids), "dumped", flush=True)
    # report
    have = [(i, revs[i]) for i in range(len(revs)) if os.path.exists(f"{DUMPS}/{revs[i][:10]}.txt")]
    prev = None
    for i, c in have:
        s = signature(f"{DUMPS}/{c[:10]}.txt")
        if prev is not None and s != prev[2]:
            gap = [revs[k][:8] for k in range(prev[0] + 1, i)]
            print(f"CHANGE between #{prev[0]} {prev[1][:8]} and #{i} {c[:8]}  (undumped between: {len(gap)}) :: " + git("log", "-1", "--format=%ad %s", "--date=format:%m-%d %H:%M", c).strip()[:110])
        prev = (i, c, s)
    failed = [revs[i][:8] for i in range(len(revs)) if os.path.exists(f"{DUMPS}/{revs[i][:10]}.txt.failed")]
    print("dumped", len(have), "of", len(revs), "failed:", failed)

main()
