#!/usr/bin/env python3
"""gen.py: from eras.json (the dumps of the history), the stages of each engine's stored lists as a C++ header.
Prints what it checked; writes StoredListHistory.inc (the tables) next to itself."""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.environ.get("NEPTUNE_LIST_HISTORY", WT + "/build/list-history")
eras = json.load(open(WORK + "/eras.json"))
NAMES = {"0": "UnifiedVoice", "1": "FM", "2": "MacroOsc", "3": "Wavetable", "4": "WaveSeq", "5": "Sampler", "6": "Drum", "8": "Resonator"}

def when(c):
    return subprocess.run(["git", "-C", WT, "log", "-1", "--format=%h %ad", "--date=format:%Y-%m-%d %H:%M", c], capture_output=True, text=True).stdout.strip()

def subject(c):
    return subprocess.run(["git", "-C", WT, "log", "-1", "--format=%s", c], capture_output=True, text=True).stdout.strip()

def ranges(ids):
    ids = sorted(ids)
    out, i = [], 0
    while i < len(ids):
        j = i
        while j + 1 < len(ids) and ids[j + 1] == ids[j] + 1:
            j += 1
        out.append((ids[i], ids[j]))
        i = j + 1
    return out

def family_lists(e, era):
    """cell / slot / source lists of an era, by the ids that show them."""
    by_id = {}
    for content, ids in era["lists"].items():
        for i in ids:
            by_id[i] = content
    cell = by_id.get(165) if e != "0" else by_id.get(158)
    return {"Cell": cell, "Slot": by_id.get(701), "Source": by_id.get(700)}

out = []
report = []
for e in sorted(eras, key=int):
    if e not in NAMES:
        continue
    stages = eras[e]
    today = family_lists(e, stages[-1])
    last = {"Cell": None, "Slot": None, "Source": None}
    rows = []
    for k, era in enumerate(stages):
        fam = family_lists(e, era)
        changed = {f: (fam[f] if fam[f] != last[f] else None) for f in fam}
        if k > 0 and not any(changed.values()) and not (e == "6" and era["commit"].startswith("871c986")):
            continue        # (a stage of other lists only: shapes, triggers, divisions - they grew at their ends)
        ev = ranges(era["new_ids"]) if k > 0 else []
        # the slots' block is known by any of its ids
        if any(a >= 700 for a, b in ev):
            ev = [(a, b) for a, b in ev if a < 700] + [(700, 772)]
        for fm in changed:
            if changed[fm] is not None:
                o, t = changed[fm].split("|"), today[fm].split("|")
                if t[:len(o)] == o:
                    changed[fm] = "=" + str(len(o))
        # by hand: what the dumps cannot say
        if e == "4" and era["commit"].startswith("401f08d"):
            ev = [(133, 140)]       # BODY's ids of 80 minutes before (b7e8463): the nearest thing a file can show
        if e == "6":
            changed["Type"] = ("FMKK|KICK|MKIK|SNAR|FMSN|CLAP|HAT|OHAT|CYMB|TOM|RIM|SHKR|CLAV|SUB|KSYN|CSYN" if k == 0
                               else ("=18" if era["commit"].startswith("871c986") else None))
        rows.append((era["commit"], ev, changed))
        for f in fam:
            if fam[f] is not None:
                # checks against today's list
                o, t = fam[f].split("|"), today[f].split("|")
                missing = [x for x in o if x not in t]
                dup = sorted({x for x in o if o.count(x) > 1})
                prefix = t[:len(o)] == o
                moved = [i for i, x in enumerate(o) if x in t and t.index(x) != i]
                if changed[f] is not None:
                    report.append(f"{NAMES[e]:12} {f:6} {when(era['commit'])} n={len(o):3} "
                                  + ("prefix of today's" if prefix else f"NOT a prefix: {len(moved)} entries elsewhere today (first at {moved[0] if moved else '-'})")
                                  + (f"; gone today: {' '.join(missing)}" if missing else "") + (f"; DUPLICATE {dup}" if dup else "")
                                  + (f"; evidence {ev}" if ev else "; no id tells this stage"))
            last[f] = fam[f]
    out.append((e, rows))

print("\n".join(report))

VAR = {"0": "unifiedVoice", "1": "fm", "2": "macro", "3": "wavetable", "4": "waveSeq", "5": "sampler", "6": "drum", "8": "resonator"}

def literal(v, indent):
    """A long list as adjacent string literals of about 100 characters, cut at a '|'."""
    if v is None:
        return "nullptr"
    parts, line = [], ""
    for label in v.split("|"):
        piece = label + "|"
        if len(line) + len(piece) > 100:
            parts.append(line)
            line = ""
        line += piece
    parts.append(line[:-1])
    return ("\n" + indent).join('"' + x + '"' for x in parts)

with open(WORK + "/StoredListHistory.inc", "w") as f:
    for e, rows in out:
        f.write(f"        // ---- {NAMES[e]} " + "-" * (86 - len(NAMES[e])) + "\n")
        f.write(f"        static const Stage {VAR[e]}[] = {{\n")
        for commit, ev, changed in rows:
            evs = ",".join(f"{a}-{b}" if a != b else str(a) for a, b in ev)
            f.write(f"            // {when(commit)}  {subject(commit)[:80]}\n")
            f.write(f"            {{ \"{when(commit).split()[0]}\", \"{evs}\", {{\n")
            for fam in ("Cell", "Slot", "Source", "Type"):
                f.write("                " + literal(changed.get(fam), "                ") + ("," if fam != "Type" else " } },") + "\n")
        f.write("        };\n")
print("written", WORK + "/StoredListHistory.inc")
