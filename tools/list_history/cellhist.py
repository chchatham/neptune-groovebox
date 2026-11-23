#!/usr/bin/env python3
"""cellhist.py <engine> <id> [...ids]: the choices of a cell at every dump where they changed.
   cellhist.py all: every Choice cell of every engine whose list ever changed other than by growing at its end."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
src = open(HERE + "/hist.py").read().replace("\nmain()\n", "\n")
hist = {"__file__": HERE + "/hist.py"}
exec(compile(src, "hist.py", "exec"), hist)
revs = hist["order"]()
have = [(i, c) for i, c in enumerate(revs) if os.path.exists(f"{hist['DUMPS']}/{c[:10]}.txt")]

def cells(c):
    out = {}
    for line in open(f"{hist['DUMPS']}/{c[:10]}.txt"):
        f = line.rstrip("\n").split("\t")
        if f[0] == "CELL" and int(f[6]) == 3:
            out[(f[1], int(f[4]))] = (f[5], f[10] if len(f) > 10 else "", f[3])
    return out

if sys.argv[1] == "all":
    last, flagged = {}, {}
    for i, c in have:
        now = cells(c)
        for k, (label, ch, page) in now.items():
            if k in last and last[k][1] != ch:
                o, n = last[k][1].split("|"), ch.split("|")
                if n[:len(o)] != o:
                    flagged.setdefault(k, []).append((i, c[:8], last[k][0], label, page, len(o), len(n), last[k][1][:150], ch[:150]))
            last[k] = (label, ch, page)
    for k in sorted(flagged):
        for row in flagged[k]:
            print(k, row)
else:
    e, ids = sys.argv[1], [int(x) for x in sys.argv[2:]]
    prev = {}
    for i, c in have:
        now = cells(c)
        for pid in ids:
            v = now.get((e, pid))
            if v != prev.get(pid):
                print(f"#{i} {c[:8]} id {pid}: {v}")
                prev[pid] = v
