import os, sys
WT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
S = os.environ.get("NEPTUNE_LIST_HISTORY", WT + "/build/list-history")
pre = r'''#pragma once

// -----------------------------------------------------------------------------
// The lists a stored sound holds positions of, AS THEY WERE: what a file written
// before format 3 (which stores such a cell by the entry's name) meant by a number.
// StoredLists.h reads these tables; nothing else does.
//
// HOW THEY WERE MADE. Not by hand: a small program printed every Choice cell of
// every engine's pages (and the unified voice's two destination lists) from the
// source tree of a commit, and was run on v0.9 (c210682, the tree the tag has), on
// today's tree and on every commit between them at which the output differed (a
// bisection over the 108 commits that touch src/engine; 68 trees built). So each
// list below is what that commit's own code showed.
//
// WHAT THE HISTORY IS (2026-11-11 to 2026-11-22):
//   v0.9 .. 1d0c777   Not one of these lists changed, from the public release through every
//   (11-11 .. 11-21   published build (the last is 66a3f06). Each is a PREFIX of today's list,
//    10:23)           so a number stored by any published build still names the same entry: an
//                     LFO's DEST, a STRIP route, a shape, a trigger mode, a division, a filter
//                     type, the unified voice's LFO and envelope destinations. There were no free
//                     slots yet. ONE list was not kept: the drum kit's TYPE (below).
//   fbbbe40 on        The two days of the unification, never published and never run by the
//   (11-21 10:58 ..)  owner: the free slots came (their DEST list is the engine's table and then
//                     L1RT .. AM16, which every later entry of the table pushed along), engines
//                     were ported to the router one by one, the effect cells were appended, and
//                     twice a list grew in its middle (BDY2 on the macro oscillator and the wave
//                     sequencer, 395b067: eight entries before the effect cells, on the wave
//                     sequencer before ETIM FTIM PM too).
//
// A STAGE is a commit at which one of an engine's lists changed. A file carries no stamp, so its
// stage is told from the ids it holds: `evidence` is the ids that came onto the engine's pages AT
// that commit (a file that holds one was written then or later; a save from the app holds every
// cell of every page). A stage with no evidence cannot be told from the one before it: the file
// is then read as the earlier one, unless the number itself does not fit that list.
//
// A list is written
//     nullptr     as at the stage before (at the first stage: the engine had no such list yet)
//     "=N"        today's first N entries (a prefix of today's list: every number keeps its meaning)
//     "A|B|.."    the entries, in the order they had
// for the families
//     Cell        the DEST of an LFO, of a STRIP route, of an XIN route
//     Slot        a free slot's DEST
//     Source      a free slot's SRC and VIA
//     Type        the drum kit's TYPE. At U2b (871c986) the enum was put in another order, not
//                 appended to: every kit saved by a published build stores the old numbers, and
//                 with them a cymbal page at the ids that are a pad's second page now. No id
//                 came with that commit, so a kit is read by the old numbers until it holds what
//                 the kit's port brought (0dc6685: a pad's WAVE, the XIN page).
// The lists that only ever grew at their end need no table: LFO shape (9, then USR USRS),
// trigger mode (5, then the four X modes), tempo division (17, then five), filter type (6, never
// changed), the unified voice's LFO destinations (75, then the 80 effect cells) and its extra
// envelopes' (75, never changed).
// -----------------------------------------------------------------------------
namespace stored
{
namespace history
{
    enum Family { Cell = 0, Slot, Source, Type, NumFamilies };

    struct Stage
    {
        const char* since;                  // the commit that began it
        const char* evidence;               // ids new to the engine's pages at that commit: "8-9,66-69" ("": none)
        const char* lists[NumFamilies];
    };

    // The stages of an engine type (EngineType as a number), oldest first; nullptr: it has none.
    inline const Stage* stagesOf(int engineType, int& count)
    {
'''
post = r'''
        #define NEPTUNE_STAGES(list) do { count = (int) (sizeof(list) / sizeof(list[0])); return list; } while (false)
        switch (engineType)
        {
            case 0: NEPTUNE_STAGES(unifiedVoice);
            case 1: NEPTUNE_STAGES(fm);
            case 2: NEPTUNE_STAGES(macro);
            case 3: NEPTUNE_STAGES(wavetable);
            case 4: NEPTUNE_STAGES(waveSeq);
            case 5: NEPTUNE_STAGES(sampler);
            case 6: NEPTUNE_STAGES(drum);
            case 8: NEPTUNE_STAGES(resonator);
            default: break;
        }
        #undef NEPTUNE_STAGES
        count = 0;
        return nullptr;
    }
}
}
'''
open(sys.argv[1], "w").write(pre + open(S + "/StoredListHistory.inc").read() + post)
