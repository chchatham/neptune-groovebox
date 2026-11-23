#!/usr/bin/env python3
"""eras.py: from the dumps, per engine, the eras of its named lists and the ids each era brought."""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("hist", HERE + "/hist.py")
src = open(HERE + "/hist.py").read().replace("\nmain()\n", "\n")
hist = {"__file__": HERE + "/hist.py"}
exec(compile(src, "hist.py", "exec"), hist)
revs = hist["order"]()
DUMPS = hist["DUMPS"]
have = [(i, c) for i, c in enumerate(revs) if os.path.exists(f"{DUMPS}/{c[:10]}.txt")]
ENG = {"0": "UV", "1": "FM", "2": "MACRO", "3": "WT", "4": "WSEQ", "5": "SMP", "6": "DRUM", "7": "MIDI", "8": "RSN"}

def load(c):
    cells, lists, ids = hist["parse"](f"{DUMPS}/{c[:10]}.txt")
    per = {}
    for (e, pid), choices in cells.items():
        per.setdefault(e, {}).setdefault("|".join(choices), []).append(pid)
    return per, lists, ids

def describe(old, new):
    o, n = old.split("|"), new.split("|")
    if n[:len(o)] == o:
        return f"+{len(n)-len(o)} at the end ({' '.join(n[len(o):][:12])}{' ..' if len(n)-len(o) > 12 else ''})"
    # first difference
    k = 0
    while k < min(len(o), len(n)) and o[k] == n[k]:
        k += 1
    return f"{len(o)} -> {len(n)}, first difference at {k}: old {' '.join(o[k:k+6])} / new {' '.join(n[k:k+6])}"

prev = None
seen_ids = {}
out = {}
for i, c in have:
    per, lists, ids = load(c)
    when = subprocess.run(["git", "-C", hist["WT"], "log", "-1", "--format=%ad %s", "--date=format:%m-%d %H:%M", c], capture_output=True, text=True).stdout.strip()[:90]
    for e in sorted(set(per) | (set(prev[0]) if prev else set())):
        now = per.get(e, {})
        before = prev[0].get(e, {}) if prev else None
        key = json.dumps({k: sorted(v) for k, v in sorted(now.items())})
        bkey = json.dumps({k: sorted(v) for k, v in sorted(before.items())}) if before is not None else None
        # compare by list content only (which ids show a list may change without the list changing)
        if before is None or set(now) != set(before):
            new_ids = sorted(ids.get(e, set()) - seen_ids.get(e, set()))
            print(f"\n=== {ENG.get(e, e)} era at #{i} {c[:8]} {when}")
            print(f"    ids new to the engine's pages: {new_ids if len(new_ids) < 60 else str(new_ids[:60]) + ' ...'}")
            gone = sorted((seen_ids.get(e, set())) - ids.get(e, set())) if before is not None else []
            if before is not None:
                last_ids = prev[2].get(e, set())
                print(f"    ids gone since the dump before: {sorted(last_ids - ids.get(e, set()))}")
            for lst, pids in sorted(now.items(), key=lambda kv: min(kv[1])):
                tag = ""
                if before is not None and lst not in before:
                    # which old list did these ids show?
                    olds = [ol for ol, op in before.items() if set(op) & set(pids)]
                    tag = " CHANGED: " + (describe(olds[0], lst) if olds else "new list")
                    print(f"    ids {sorted(pids)[:14]}{'..' if len(pids) > 14 else ''} n={len(lst.split('|'))}{tag}")
                    print(f"        {lst}")
            if before is not None:
                for lst, pids in before.items():
                    if lst not in now and not any(set(pids) & set(p2) for p2 in now.values()):
                        print(f"    list gone with its ids {sorted(pids)[:10]}: {lst[:80]}")
            out.setdefault(e, []).append({"index": i, "commit": c, "lists": {k: sorted(v) for k, v in now.items()}, "new_ids": new_ids})
        seen_ids.setdefault(e, set()).update(ids.get(e, set()))
    if prev is not None:
        for name in sorted(set(lists) | set(prev[1])):
            if lists.get(name) != prev[1].get(name):
                print(f"\n=== LIST {name} changed at #{i} {c[:8]} {when}: " + describe("|".join(prev[1].get(name, [])), "|".join(lists.get(name, []))))
                print("        " + "|".join(lists.get(name, [])))
    prev = (per, lists, ids)
json.dump(out, open(hist["WORK"] + "/eras.json", "w"))
