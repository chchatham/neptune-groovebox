#pragma once

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
        // ---- UnifiedVoice --------------------------------------------------------------------------
        static const Stage unifiedVoice[] = {
            // c210682 2026-11-11 13:33  Version 0.9
            { "c210682", "", {
                nullptr,
                nullptr,
                nullptr,
                nullptr } },
            // fbbbe40 2026-11-21 10:58  Mod slots on the seven sound engines, and the custom LFO shape
            { "fbbbe40", "700-772", {
                nullptr,
                "OFF|PTCH|CUT|RESO|MIX|AMP|PW|XMOD|CUTB|PAN|LVL1|LVL2|LVL3|NOIS|FFM|PIT2|RAT1|RAT2|RAT3|RAT4|DPT1|"
                "DPT2|DPT3|DPT4|TUN2|DET2|DRIV|TUN3|NCOL|1A|1B|2A|2B|3A|3B|SPRD|GLID|DRFT|DRAT|ENV|FDRV|MORP|RESB|"
                "BAL|DRVB|TRKB|FATK|FDEC|FSUS|FREL|KEY|FVEL|PMOD|ATK|DEC|SUS|REL|VEL|ATK2|DEC2|SUS2|REL2|ATK3|DEC3|"
                "SUS3|REL3|ATK4|DEC4|SUS4|REL4|ATK5|DEC5|SUS5|REL5|EDPT|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|"
                "AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "OFF|LFO1|LFO2|ENV|FENV|VEL|PRES|STRP|KEY|RAND|IN|LFO3|LFO4",
                nullptr } },
            // 11206c9 2026-11-22 04:29  WIP U4-UV: the unified voice on the modulation router (P01 holds)
            { "11206c9", "158-173", {
                "=80",
                "=104",
                "=43",
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "=160",
                "=184",
                nullptr,
                nullptr } },
        };
        // ---- FM ------------------------------------------------------------------------------------
        static const Stage fm[] = {
            // c210682 2026-11-11 13:33  Version 0.9
            { "c210682", "", {
                "=69",
                nullptr,
                nullptr,
                nullptr } },
            // fbbbe40 2026-11-21 10:58  Mod slots on the seven sound engines, and the custom LFO shape
            { "fbbbe40", "700-772", {
                nullptr,
                "OFF|PTCH|AMP|PAN|CUT|RESO|INDX|FDBK|DETN|LVL1|LVL2|LVL3|LVL4|LVL5|LVL6|RAT1|RAT2|RAT3|RAT4|RAT5|"
                "RAT6|DET1|FDB1|ATK1|DEC1|SUS1|REL1|DET2|FDB2|ATK2|DEC2|SUS2|REL2|DET3|FDB3|ATK3|DEC3|SUS3|REL3|DET4|"
                "FDB4|ATK4|DEC4|SUS4|REL4|DET5|FDB5|ATK5|DEC5|SUS5|REL5|DET6|FDB6|ATK6|DEC6|SUS6|REL6|ENV|DRIV|MORP|"
                "KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|"
                "AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=11",
                nullptr } },
            // ca01eb0 2026-11-21 14:34  U3, the oscillator: dsp/PhaseOsc.h, and the FM engine's operators on it (WAVE, P
            { "ca01eb0", "8-9,18-19,28-29,38-39,48-49,58-59,66-69", {
                "=75",
                "OFF|PTCH|AMP|PAN|CUT|RESO|INDX|FDBK|DETN|LVL1|LVL2|LVL3|LVL4|LVL5|LVL6|RAT1|RAT2|RAT3|RAT4|RAT5|"
                "RAT6|DET1|FDB1|ATK1|DEC1|SUS1|REL1|DET2|FDB2|ATK2|DEC2|SUS2|REL2|DET3|FDB3|ATK3|DEC3|SUS3|REL3|DET4|"
                "FDB4|ATK4|DEC4|SUS4|REL4|DET5|FDB5|ATK5|DEC5|SUS5|REL5|DET6|FDB6|ATK6|DEC6|SUS6|REL6|ENV|DRIV|MORP|"
                "KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|POS1|POS2|POS3|POS4|POS5|POS6|L1RT|L1DP|L2RT|L2DP|AMT1|"
                "AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                nullptr,
                nullptr } },
            // e0a1edd 2026-11-22 00:09  WIP U4 FM: engine on the router, old copy, first tests
            { "e0a1edd", "182-189", {
                "=84",
                "=104",
                "=43",
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "=164",
                "=184",
                nullptr,
                nullptr } },
        };
        // ---- MacroOsc ------------------------------------------------------------------------------
        static const Stage macro[] = {
            // c210682 2026-11-11 13:33  Version 0.9
            { "c210682", "", {
                "=27",
                nullptr,
                nullptr,
                nullptr } },
            // fbbbe40 2026-11-21 10:58  Mod slots on the seven sound engines, and the custom LFO shape
            { "fbbbe40", "700-772", {
                nullptr,
                "OFF|PTCH|AMP|PAN|CUT|RESO|SAWS|SPRD|HARM|MORP|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|FMOR|KTRK|FATK|FDEC|"
                "FSUS|FREL|EVEL|FVEL|LRAT|LDPT|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|"
                "AM11|AM12|AM13|AM14|AM15|AM16",
                "=11",
                nullptr } },
            // 76e77e1 2026-11-21 14:08  U3, the modulation router: one list of slots; the macro oscillator moved onto it
            { "76e77e1", "182-189", {
                nullptr,
                nullptr,
                "=17",
                nullptr } },
            // b7e8463 2026-11-21 23:42  Unify U4-0: the router's shared pieces before the engines are ported
            { "b7e8463", "", {
                "=29",
                "OFF|PTCH|AMP|PAN|CUT|RESO|SAWS|SPRD|HARM|MORP|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|FMOR|KTRK|FATK|FDEC|"
                "FSUS|FREL|EVEL|FVEL|LRAT|LDPT|ETIM|FTIM|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|"
                "AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=38",
                nullptr } },
            // 11a52c8 2026-11-22 04:31  WIP U4-MACRO: BODY, ANLG with SEED, the models' noise on spread::Model, AmpEnvel
            { "11a52c8", "125-140", {
                "=37",
                "OFF|PTCH|AMP|PAN|CUT|RESO|SAWS|SPRD|HARM|MORP|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|FMOR|KTRK|FATK|FDEC|"
                "FSUS|FREL|EVEL|FVEL|LRAT|LDPT|ETIM|FTIM|BMIX|BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|L1RT|L1DP|L2RT|L2DP|"
                "AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                nullptr,
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "OFF|PTCH|AMP|PAN|CUT|RESO|SAWS|SPRD|HARM|MORP|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|FMOR|KTRK|FATK|FDEC|"
                "FSUS|FREL|EVEL|FVEL|LRAT|LDPT|ETIM|FTIM|BMIX|BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|1:C1|1:C2|1:C3|1:C4|"
                "1:C5|1:C6|1:C7|1:C8|1:WT|2:C1|2:C2|2:C3|2:C4|2:C5|2:C6|2:C7|2:C8|2:WT|3:C1|3:C2|3:C3|3:C4|3:C5|3:C6|"
                "3:C7|3:C8|3:WT|4:C1|4:C2|4:C3|4:C4|4:C5|4:C6|4:C7|4:C8|4:WT|5:C1|5:C2|5:C3|5:C4|5:C5|5:C6|5:C7|5:C8|"
                "5:WT|6:C1|6:C2|6:C3|6:C4|6:C5|6:C6|6:C7|6:C8|6:WT|7:C1|7:C2|7:C3|7:C4|7:C5|7:C6|7:C7|7:C8|7:WT|8:C1|"
                "8:C2|8:C3|8:C4|8:C5|8:C6|8:C7|8:C8|8:WT|CH.R|CH.D|FL.R|FL.D|PH.R|PH.D|RO.S|RO.D",
                "OFF|PTCH|AMP|PAN|CUT|RESO|SAWS|SPRD|HARM|MORP|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|FMOR|KTRK|FATK|FDEC|"
                "FSUS|FREL|EVEL|FVEL|LRAT|LDPT|ETIM|FTIM|BMIX|BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|L1RT|L1DP|L2RT|L2DP|"
                "AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16|1:C1|1:C2|1:C3|1:C4|"
                "1:C5|1:C6|1:C7|1:C8|1:WT|2:C1|2:C2|2:C3|2:C4|2:C5|2:C6|2:C7|2:C8|2:WT|3:C1|3:C2|3:C3|3:C4|3:C5|3:C6|"
                "3:C7|3:C8|3:WT|4:C1|4:C2|4:C3|4:C4|4:C5|4:C6|4:C7|4:C8|4:WT|5:C1|5:C2|5:C3|5:C4|5:C5|5:C6|5:C7|5:C8|"
                "5:WT|6:C1|6:C2|6:C3|6:C4|6:C5|6:C6|6:C7|6:C8|6:WT|7:C1|7:C2|7:C3|7:C4|7:C5|7:C6|7:C7|7:C8|7:WT|8:C1|"
                "8:C2|8:C3|8:C4|8:C5|8:C6|8:C7|8:C8|8:WT|CH.R|CH.D|FL.R|FL.D|PH.R|PH.D|RO.S|RO.D",
                nullptr,
                nullptr } },
            // 395b067 2026-11-22 16:36  Widen BODY latent space: BDY2 page (CONT GRAN REED HARD RDAM KDCY DIRE WIRE)
            { "395b067", "141-148", {
                "=125",
                "=145",
                nullptr,
                nullptr } },
        };
        // ---- Wavetable -----------------------------------------------------------------------------
        static const Stage wavetable[] = {
            // c210682 2026-11-11 13:33  Version 0.9
            { "c210682", "", {
                "=34",
                nullptr,
                nullptr,
                nullptr } },
            // fbbbe40 2026-11-21 10:58  Mod slots on the seven sound engines, and the custom LFO shape
            { "fbbbe40", "700-772", {
                nullptr,
                "OFF|PTCH|AMP|PAN|CUT|RESO|POS|SPRD|SUB|MSPD|MDPT|ENVP|VELP|TIME|SPD|GLID|VIBR|ATK|DEC|SUS|REL|VEL|"
                "ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|"
                "AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=11",
                nullptr } },
            // 7fe94b3 2026-11-21 16:52  U3+: one per-voice filter section with a second filter, one seeded per-note spre
            { "7fe94b3", "116-123,125-132", {
                "=39",
                "OFF|PTCH|AMP|PAN|CUT|RESO|POS|SPRD|SUB|MSPD|MDPT|ENVP|VELP|TIME|SPD|GLID|VIBR|ATK|DEC|SUS|REL|VEL|"
                "ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|BAL|FFM|OFSB|RESB|DRVB|L1RT|L1DP|L2RT|"
                "L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                nullptr,
                nullptr } },
            // 4b477ff 2026-11-22 00:19  WIP U4 wavetable: the engine on the router and PhaseOsc, old against new
            { "4b477ff", "182-189", {
                "=42",
                "=62",
                "=40",
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "=122",
                "=142",
                nullptr,
                nullptr } },
        };
        // ---- WaveSeq -------------------------------------------------------------------------------
        static const Stage waveSeq[] = {
            // c210682 2026-11-11 13:33  Version 0.9
            { "c210682", "", {
                "=75",
                nullptr,
                nullptr,
                nullptr } },
            // fbbbe40 2026-11-21 10:58  Mod slots on the seven sound engines, and the custom LFO shape
            { "fbbbe40", "700-772", {
                nullptr,
                "OFF|PTCH|AMP|PAN|CUT|RESO|RATE|XFAD|PRNG|LEVL|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|"
                "FREL|EVEL|FVEL|LRAT|LDPT|P1|P2|P3|P4|P5|P6|P7|P8|P9|P10|P11|P12|P13|P14|P15|P16|L1|L2|L3|L4|L5|L6|"
                "L7|L8|L9|L10|L11|L12|L13|L14|L15|L16|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16|L1RT|"
                "L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=11",
                nullptr } },
            // 7a406c1 2026-11-21 19:26  Unify E5: the master section as an insert chain, one dry / wet mix in the chain,
            { "7a406c1", "182-189", {
                "=83",
                "OFF|PTCH|AMP|PAN|CUT|RESO|RATE|XFAD|PRNG|LEVL|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|"
                "FREL|EVEL|FVEL|LRAT|LDPT|P1|P2|P3|P4|P5|P6|P7|P8|P9|P10|P11|P12|P13|P14|P15|P16|L1|L2|L3|L4|L5|L6|"
                "L7|L8|L9|L10|L11|L12|L13|L14|L15|L16|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16|BMIX|"
                "BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|"
                "AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                nullptr,
                nullptr } },
            // 401f08d 2026-11-22 01:02  Unify U4 wave sequencer: on the modulation router and the shared oscillator's ph
            { "401f08d", "133-140", {
                "OFF|PTCH|AMP|PAN|CUT|RESO|RATE|XFAD|PRNG|LEVL|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|"
                "FREL|EVEL|FVEL|LRAT|LDPT|P1|P2|P3|P4|P5|P6|P7|P8|P9|P10|P11|P12|P13|P14|P15|P16|L1|L2|L3|L4|L5|L6|"
                "L7|L8|L9|L10|L11|L12|L13|L14|L15|L16|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16|BMIX|"
                "BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|ETIM|FTIM|PM",
                "OFF|PTCH|AMP|PAN|CUT|RESO|RATE|XFAD|PRNG|LEVL|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|"
                "FREL|EVEL|FVEL|LRAT|LDPT|P1|P2|P3|P4|P5|P6|P7|P8|P9|P10|P11|P12|P13|P14|P15|P16|L1|L2|L3|L4|L5|L6|"
                "L7|L8|L9|L10|L11|L12|L13|L14|L15|L16|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16|BMIX|"
                "BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|ETIM|FTIM|PM|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|"
                "AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=41",
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "OFF|PTCH|AMP|PAN|CUT|RESO|RATE|XFAD|PRNG|LEVL|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|"
                "FREL|EVEL|FVEL|LRAT|LDPT|P1|P2|P3|P4|P5|P6|P7|P8|P9|P10|P11|P12|P13|P14|P15|P16|L1|L2|L3|L4|L5|L6|"
                "L7|L8|L9|L10|L11|L12|L13|L14|L15|L16|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16|BMIX|"
                "BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|ETIM|FTIM|PM|1:C1|1:C2|1:C3|1:C4|1:C5|1:C6|1:C7|1:C8|1:WT|2:C1|"
                "2:C2|2:C3|2:C4|2:C5|2:C6|2:C7|2:C8|2:WT|3:C1|3:C2|3:C3|3:C4|3:C5|3:C6|3:C7|3:C8|3:WT|4:C1|4:C2|4:C3|"
                "4:C4|4:C5|4:C6|4:C7|4:C8|4:WT|5:C1|5:C2|5:C3|5:C4|5:C5|5:C6|5:C7|5:C8|5:WT|6:C1|6:C2|6:C3|6:C4|6:C5|"
                "6:C6|6:C7|6:C8|6:WT|7:C1|7:C2|7:C3|7:C4|7:C5|7:C6|7:C7|7:C8|7:WT|8:C1|8:C2|8:C3|8:C4|8:C5|8:C6|8:C7|"
                "8:C8|8:WT|CH.R|CH.D|FL.R|FL.D|PH.R|PH.D|RO.S|RO.D",
                "OFF|PTCH|AMP|PAN|CUT|RESO|RATE|XFAD|PRNG|LEVL|ATK|DEC|SUS|REL|VEL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|"
                "FREL|EVEL|FVEL|LRAT|LDPT|P1|P2|P3|P4|P5|P6|P7|P8|P9|P10|P11|P12|P13|P14|P15|P16|L1|L2|L3|L4|L5|L6|"
                "L7|L8|L9|L10|L11|L12|L13|L14|L15|L16|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16|BMIX|"
                "BMRP|BDCY|BCUP|BDMP|BPOS|BTUN|BKTK|ETIM|FTIM|PM|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|"
                "AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16|1:C1|1:C2|1:C3|1:C4|1:C5|1:C6|1:C7|1:C8|1:WT|2:C1|"
                "2:C2|2:C3|2:C4|2:C5|2:C6|2:C7|2:C8|2:WT|3:C1|3:C2|3:C3|3:C4|3:C5|3:C6|3:C7|3:C8|3:WT|4:C1|4:C2|4:C3|"
                "4:C4|4:C5|4:C6|4:C7|4:C8|4:WT|5:C1|5:C2|5:C3|5:C4|5:C5|5:C6|5:C7|5:C8|5:WT|6:C1|6:C2|6:C3|6:C4|6:C5|"
                "6:C6|6:C7|6:C8|6:WT|7:C1|7:C2|7:C3|7:C4|7:C5|7:C6|7:C7|7:C8|7:WT|8:C1|8:C2|8:C3|8:C4|8:C5|8:C6|8:C7|"
                "8:C8|8:WT|CH.R|CH.D|FL.R|FL.D|PH.R|PH.D|RO.S|RO.D",
                nullptr,
                nullptr } },
            // 395b067 2026-11-22 16:36  Widen BODY latent space: BDY2 page (CONT GRAN REED HARD RDAM KDCY DIRE WIRE)
            { "395b067", "141-148", {
                "=174",
                "=194",
                nullptr,
                nullptr } },
        };
        // ---- Sampler -------------------------------------------------------------------------------
        static const Stage sampler[] = {
            // c210682 2026-11-11 13:33  Version 0.9
            { "c210682", "", {
                "=42",
                nullptr,
                nullptr,
                nullptr } },
            // fbbbe40 2026-11-21 10:58  Mod slots on the seven sound engines, and the custom LFO shape
            { "fbbbe40", "700-772", {
                nullptr,
                "OFF|PTCH|AMP|PAN|CUT|RESO|STRT|DRIV|GPOS|SIZE|DENS|SPD|END|LPST|LPEN|SCAN|SPRY|PJIT|SPRD|RVRS|SLIC|"
                "FADE|SLEN|SSIZ|XFAD|JIT|SSPR|BITS|RATE|LEVL|VELF|ENVF|KTRK|FDRV|ATK|DEC|SUS|REL|VEL|RAND|LRAT|LDPT|"
                "L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=11",
                nullptr } },
            // 3ec6fbc 2026-11-22 01:21  Unify U4 sampler: the sampler on the modulation router
            { "3ec6fbc", "182-189", {
                "=43",
                "=63",
                "=40",
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "=123",
                "=143",
                nullptr,
                nullptr } },
        };
        // ---- Drum ----------------------------------------------------------------------------------
        static const Stage drum[] = {
            // c210682 2026-11-11 13:33  Version 0.9
            { "c210682", "", {
                "=18",
                nullptr,
                nullptr,
                "FMKK|KICK|MKIK|SNAR|FMSN|CLAP|HAT|OHAT|CYMB|TOM|RIM|SHKR|CLAV|SUB|KSYN|CSYN" } },
            // fbbbe40 2026-11-21 10:58  Mod slots on the seven sound engines, and the custom LFO shape
            { "fbbbe40", "700-772", {
                nullptr,
                "OFF|PTCH|DEC|TONE|DRIV|SNAP|AMP|PAN|SWEP|SWPT|SCRV|SHAP|KNCK|KTON|HOLD|ACRV|LRAT|LDPT|L1RT|L1DP|"
                "L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=11",
                nullptr } },
            // 871c986 2026-11-21 19:47  WIP U2b: the resonator drum, the oscillator bank, the resonator's second eight l
            { "871c986", "", {
                "=42",
                "OFF|PTCH|DEC|TONE|DRIV|SNAP|AMP|PAN|SWEP|SWPT|SCRV|SHAP|KNCK|KTON|HOLD|ACRV|LRAT|LDPT|MRPH|XMPH|"
                "ENRG|STIF|SHT|SHEL|ODD|DAMP|TILT|HUM|POS|MEMB|DROP|WIRE|MEET|SPRD|PAIR|HAND|DIR|CONT|GRAN|COUP|REED|"
                "HARD|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|"
                "AM16",
                nullptr,
                "=18" } },
            // 0dc6685 2026-11-22 04:48  WIP U4-DRUM: the kit on the router and PhaseOsc, old copies, the suite
            { "0dc6685", "128-143,182-189", {
                "=46",
                "=66",
                "=37",
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "=126",
                "=146",
                nullptr,
                nullptr } },
        };
        // ---- Resonator -----------------------------------------------------------------------------
        static const Stage resonator[] = {
            // 46549f9 2026-11-21 11:03  Resonator engine RSN: six resonators, five exciters, energy from pressure
            { "46549f9", "", {
                "OFF|PTCH|AMP|PAN|CUT|RESO|DAMP|PICK|STIF|DCAY|ENRG|HARD|BRST|NOIS|DIR|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|"
                "MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT",
                nullptr,
                nullptr,
                nullptr } },
            // 2dbff31 2026-11-21 12:21  Track input, step 2: mod slots on the resonator, a guard on the tap, the parts o
            { "2dbff31", "700-772", {
                nullptr,
                "OFF|PTCH|AMP|PAN|CUT|RESO|DAMP|PICK|STIF|DCAY|ENRG|HARD|BRST|NOIS|DIR|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|"
                "MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|"
                "AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                "=11",
                nullptr } },
            // f4b19ba 2026-11-21 14:47  Unify U1: one resonator and one exciter in latent space; RSN rebuilt on them
            { "f4b19ba", "24-31", {
                "=41",
                "OFF|PTCH|AMP|PAN|CUT|RESO|MRPH|XMPH|DCAY|ENRG|STIF|SHT|SHEL|ODD|DAMP|TILT|HUM|POS|CONT|GRAN|COUP|"
                "REED|HARD|IN|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|L1RT|"
                "L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                nullptr,
                nullptr } },
            // 7a406c1 2026-11-21 19:26  Unify E5: the master section as an insert chain, one dry / wet mix in the chain,
            { "7a406c1", "32-39", {
                "=47",
                "OFF|PTCH|AMP|PAN|CUT|RESO|MRPH|XMPH|DCAY|ENRG|STIF|SHT|SHEL|ODD|DAMP|TILT|HUM|POS|CONT|GRAN|COUP|"
                "REED|HARD|IN|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|GPOS|"
                "SCAN|SPRY|GSIZ|GPIT|PJIT|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|"
                "AM12|AM13|AM14|AM15|AM16",
                nullptr,
                nullptr } },
            // 871c986 2026-11-21 19:47  WIP U2b: the resonator drum, the oscillator bank, the resonator's second eight l
            { "871c986", "40-47", {
                "=55",
                "OFF|PTCH|AMP|PAN|CUT|RESO|MRPH|XMPH|DCAY|ENRG|STIF|SHT|SHEL|ODD|DAMP|TILT|HUM|POS|CONT|GRAN|COUP|"
                "REED|HARD|IN|LEVL|ATK|DEC|SUS|REL|ENV|DRIV|MORP|KTRK|FATK|FDEC|FSUS|FREL|EVEL|FVEL|LRAT|LDPT|GPOS|"
                "SCAN|SPRY|GSIZ|GPIT|PJIT|MEMB|DROP|WIRE|MEET|SPRD|PAIR|HAND|DIR|L1RT|L1DP|L2RT|L2DP|AMT1|AMT2|AMT3|"
                "AMT4|AMT5|AMT6|AMT7|AMT8|AMT9|AM10|AM11|AM12|AM13|AM14|AM15|AM16",
                nullptr,
                nullptr } },
            // 840b72f 2026-11-22 04:51  WIP U4 RSN: the engine on the modulation router, old copy, suite
            { "840b72f", "182-189", {
                "=57",
                "=77",
                "=37",
                nullptr } },
            // e208eee 2026-11-22 14:04  E4: the cells of a track's effects are destinations of the track's router
            { "e208eee", "", {
                "=137",
                "=157",
                nullptr,
                nullptr } },
        };

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
