# The history of the stored lists

`src/project/StoredListHistory.h` holds, per engine, the lists a saved sound stores positions of AS THEY
WERE at every commit where one changed. It is what lets a project or preset written before format 3
(numbers only) be read. These scripts made it, from the code of each commit itself:

- `dump.cpp` prints every cell of every engine's pages (with the choices of a Choice cell) and the unified
  voice's two destination lists, for the source tree it is compiled against.
- `dumpat.py <tree> <out>` compiles it against an extracted tree (with the flags and the JUCE objects of
  `build/`, so `build/` must hold a built `NeptuneTests`) and runs it.
- `hist.py bisect` dumps the first and the last commit of the history and bisects every stretch where
  the named lists differ (each dump goes through `tools/gate.py run`); `hist.py report` lists the commits.
- `eras.py > eras.txt` reads the dumps: per engine, each change, what kind (grown at its end / in its
  middle), and the ids that came onto the engine's pages at that commit (how a file's layout is told).
- `gen.py` writes the tables (and prints, per list, whether it is a prefix of today's);
  `assemble.py src/project/StoredListHistory.h` puts them into the header with its preamble.
- `cellhist.py <engine> <id>` / `cellhist.py all`: one cell's list through the history; every list that
  ever changed other than by growing at its end.

Work files go to `build/list-history` (or `$NEPTUNE_LIST_HISTORY`).

**When a list has to change again.** Format 3 stores these cells by name, so a list may now grow anywhere
without a new stage: the tables describe files of BEFORE format 3 and are closed. Run this again only if a
list is found that the tables got wrong. What a new list needs instead is its line in
`stored::listOf` (`src/project/StoredLists.h`) or `mod::NamedList::classify` (`src/engine/ModRouter.h`);
the "Format V3" suite fails until it has one.
