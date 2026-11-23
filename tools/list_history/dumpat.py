#!/usr/bin/env python3
"""dumpat.py <tree dir> <out file>: compile dump.cpp against an extracted source tree and run it."""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
B = WT + "/build"
tree, out = sys.argv[1], sys.argv[2]
flags = open(B + "/CMakeFiles/NeptuneTests.dir/flags.make").read()
defines = re.search(r"^CXX_DEFINES = (.*)$", flags, re.M).group(1).split()
includes = re.search(r"^CXX_INCLUDES = (.*)$", flags, re.M).group(1).split()
includes = [i for i in includes if i != "-I" + WT + "/src"] + ["-I" + tree + "/src"]
link = open(B + "/CMakeFiles/NeptuneTests.dir/link.txt").read()
import shlex
parts = shlex.split(link)
objs, rest = [], []
for p in parts[1:]:
    if p.endswith(".o"):
        if "juce-src" in p:
            objs.append(os.path.join(B, p))
    elif p == "-o":
        break
# everything after the objects that is a library / framework flag
tail = parts[parts.index("-o") + 2:] if "-o" in parts else []
libs = [p if not p.endswith(".a") or os.path.isabs(p) else os.path.join(B, p) for p in tail if not p.endswith(".o")]
extra = []
if "lfoDestCellLabels" in open(tree + "/src/engine/UnifiedVoice.h").read():
    extra.append("-DDUMP_UV_CELL_LIST=1")
exe = out + ".bin"
srcs = [HERE + "/dump.cpp"]
for cpp in ("engine/VoiceAllocator.cpp", "sequencer/SequencerCore.cpp"):
    if os.path.exists(tree + "/src/" + cpp):
        srcs.append(tree + "/src/" + cpp)
cmd = ["/usr/bin/c++", "-std=gnu++17", "-arch", "arm64", "-O0", "-w"] + defines + extra + includes + srcs + objs + libs + ["-o", exe]
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode != 0:
    sys.stderr.write(r.stderr[-6000:])
    sys.exit(1)
r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
open(out, "w").write(r.stdout)
os.remove(exe)
print(out, len(r.stdout.splitlines()), "lines", "exit", r.returncode, r.stderr[-300:])
