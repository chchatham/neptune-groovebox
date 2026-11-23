#pragma once

#include "ParamSpec.h"
#include "AftertouchRouter.h"
#include "VoiceSpread.h"
#include "../dsp/LFO.h"
#include "../dsp/effects/Detector.h"
#include "../dsp/effects/Vocoder.h"
#include "../dsp/DSPUtils.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
// The modulation router: every way a sound is moved, as one list of slots.
//
// A slot is a source, a destination, an amount and a via. The LFOs' DEST / DPTH cells, the mod
// strip's routes, the pad-pressure routes and the track input at audio rate are slots whose source
// is fixed; the sixteen free slots choose theirs. All of them are summed per destination, in the
// destination's own scale, and the voice reads the result.
//
//   mod::DestTable   what an engine can have moved: one table, built once (static storage)
//   mod::SourceTable what an engine's slots can read: the SRC / VIA list, built once; the engine's
//                    own sources are published into it
//   mod::Router      one per engine: the cells (ids, pages, true values), the resolved slots,
//                    the track input for the block (its samples, its onsets, its sixteen bands),
//                    the scratch the voices fill
//   mod::Voice       one per voice: its LFOs, its sources, where each destination was last
//
// IDS: ONE RULE FOR EVERY ENGINE. Engine ids are four blocks (EngineParamRange): 0-149, 158-189,
// 400-655, 700-784.
//   158-189  THE ROUTER'S, all of it: 158-165 LFO1, 166-173 LFO2, 174-181 STRIP, 182-189 XIN.
//   700-784  THE ROUTER'S, all of it: 700-763 the sixteen free slots, 764-771 the custom LFO
//            shape, 772 the slots shown, 773-784 the four pressure slots (PressureSlotParam).
//   0-149    the engine's own cells, and every other shared section at a base the engine gives it
//            (the sections take a base): the voice filter 100-115, its second filter 116-124,
//            the per-note spread ANLG 125-132, BODY 133-140 (as the wavetable engine and the
//            wave sequencer have them). Use those numbers where the engine has them free, another
//            base where it has not. No shared section has a fixed place outside this block.
//   400-655  stage curves 400-423 (StageCurveParam; 424-431 reserved) on every engine but the
//            drum kit, whose kick and cymbal pages fill the block.
// So no engine and no section puts a cell at 158-189 or 700-784, and the router puts none
// anywhere else. Two engines are built differently and say so:
//   the drum kit       uses all of 0-127 and 400-655 for its pads; its router ids are the rule's.
//   the unified voice  its LFO cells are stored at 40-89, 130-145 and 174-189 (WHL / VEL / KTRK /
//                      PRES), and the P01 pin loads them by those ids: until format 3 has a
//                      loader they stay. It sets the router's LFOs with setLfo() and shows the
//                      router's STRIP and XIN pages at ids of its own (appendPages with PageIds,
//                      handing id - ids.strip + STRIP_BASE and so on to set / get).
// (The wave sequencer's BODY page was at 182-189, on top of XIN: it is at 133-140 now. Tests:
// "Router Sources" -> Ids.)
//
// HOW TO PORT AN ENGINE (the macro oscillator, MacroOscEngine.h, is the model: whoever reads this
// block and that file can port any engine)
//  1. The table. A static function builds a DestTable: input() for what the voice takes as pure
//     modulation (PTCH, AMP, PAN, CUT, RESO ...: a base, a span, limits), add() for page cells
//     (range and law come from the ParamSpec: a linear cell moves by its whole range at full
//     depth, a logarithmic one by half its range in octaves). Flag PerSample on everything the
//     voice can take sample by sample (pitch or phase, cutoff, level, pan, table or morph
//     position): only those are reached at audio rate; the rest moves at control rate. Flag
//     Phase as well on what is added to an oscillator's phase (PM), with .period(2 pi) where it
//     is in radians: the router then brings it in and lets it go without a step, and the voice
//     needs no let-go of its own (NOTHING IS JUMPED TO). group()
//     starts a Func + turn group, addCross() adds LRAT / LDPT, envTimes() adds ETIM / FTIM (7),
//     stripRoute() / stripVibrato() are the strip's defaults, hidden() adds what only a fixed
//     slot may reach, finish() closes it. Keep an enum of the indices the voice reads by name.
//     A DEST cell stores an index of this table: add at the end of the visible entries (a new
//     group) where saved sounds matter.
//  2. The engine owns a Router (table, number of LFOs) and each voice a mod::Voice. Route the
//     ids: Router::owns(id) -> set / get / name; every other cell that is in the table also goes
//     to setBase(id, value) when it is set (the router keeps the set value of a routed cell).
//     Pages: appendPages() adds XIN, SLT1.., LSHP, STRIP, LFO1.. at the end of the engine's own.
//  3. processBlock: beginBlock(inLeft, inRight, inFrames, follower, strip, sampleRate), then sub-blocks:
//     n = beginChunk(offset, numSamples); for every voice either voice.idle(router, n) (not
//     sounding: its LFOs keep time; if it returns true, restore() the cells it still holds) or
//     its render for n samples. Voices are rendered one after the other inside a sub-block, each
//     reading its buffers before the next one ticks: they are scratch shared by all voices.
//     Sub-blocks outside, voices inside: an engine that renders a voice for the whole block and
//     then the next must be turned round first.
//     (An engine whose voices are not copies of one sound - a drum kit: one set of LFO, strip and
//     slot cells for the kit, each drum its own values of the cells that are destinations - keeps
//     ONE router and gives each voice its own set values: Voice::useBase.)
//  4. The voice, per sub-block: tick(router, n, ampEnv, filterEnv, pressure). False = nothing
//     moves this voice: run the plain path, which must cost what the engine cost without any
//     modulation. True: forEachControl(fn(dest, value)) hands over every control-rate destination
//     that moved (and, once, each one that was let go, with its set value: push them into the
//     voice's setters); buffer(dest) is the per-sample value of a PerSample destination for these
//     n samples, or nullptr when nothing moves it (read the set value then). The values are final:
//     a cell's own value with the modulation applied and held to its range, a frequency ratio for
//     pitch, a gain for level, octaves for cutoff. No slot or destination is looked up per sample.
//  5. noteOn: voice.noteOn(router, note, velocity) before the first sample of the note; with the
//     voice's spread generator where the engine has the ANLG page (6).
//     (Something read once at the note - a sample's start point - is read at the note's first
//     sub-block, after its tick(): that is where a note-on's values exist.)
//  6. THE RANDOMS. RAND and RND2 .. RND4 are four independent values a note, -1 .. 1, draws of
//     the one per-note generator (spread::Generator, VoiceSpread.h: stream Free, index 0 .. 3).
//     An engine with the ANLG page: in the note-on callback, after VoiceSpread::next(generator),
//     voice.noteOn(router, note, velocity, generator): SEED then repeats the randoms together
//     with the spread. An engine without it: voice.seed(0, voiceIndex) for every voice in its
//     constructor, and the three-argument noteOn. Either way the same notes in the same order on
//     a new engine give the same values. Any other per-note draw of the engine goes through the
//     same generator (a stream of its own, appended to spread::Stream; draws made while a note
//     sounds: spread::Sequence): no engine keeps a random of its own for what a note is.
//  7. ENVELOPE TIME. Envelope::setTimeScale(factor) multiplies an envelope's stage times and
//     leaves its params as set; under a sounding note the stage goes on from where it is. So:
//     t.envTimes() in the table adds the destinations ETIM (the amp envelope) and FTIM (the
//     filter's) - envTimes(false) for ETIM alone -, a factor of 1/8 .. 8 at full depth; in the
//     voice's control hand-over, dest == table.ampTime(): every amp-like envelope of the voice
//     .setTimeScale(value x the note's EVAR factor), dest == table.filterTime():
//     VoiceFilter::setEnvTimeScale(value x EVAR) (which holds the product to 1/8 .. 8; the
//     Envelope itself to 1/64 .. 64). An engine with more envelopes than two decides
//     what ETIM means (FM: every operator's) and may add inputs of its own for the others.
//     This replaces every "envelope as set plus a scaled copy" (spread::scaleTimes and a second
//     Params member): delete them.
//  8. THE ENGINE'S OWN SOURCES. What only this engine has and another cell could use - an FM
//     operator's output, a wave sequence's lanes, a timed scan's position - is published:
//         static const mod::SourceTable& sources()     // static storage, like the DestTable
//         { mod::SourceTable t(numLfos, envelopes);  t.publish("OP1", mod::AudioRate); ...
//           t.publish("LANE", mod::Unipolar); ...  then registerGroups() where it stays }
//         mod::Router router { table, numLfos, envelopes, &sources() };
//     The k-th publish() call is source k (keep an enum). It appears at the end of every SRC and
//     VIA list of the engine, as a group. In the voice, per sub-block:
//         if (router.wants(k))            // false: nobody reads it, work nothing out
//             control rate:   mods.publish(k, value)  BEFORE tick(), -1 .. 1 (0 .. 1 if Unipolar)
//             audio rate:     AFTER rendering the sub-block, mods.publishAudio(k, buffer, n) with
//                             the n samples the voice made of it (the router copies them: the
//                             buffer may be the voice's scratch); when wants(k) turns false,
//                             publishAudio(k, nullptr, 0) once.
//     An audio-rate source is heard by the slots exactly one whole sub-block late (16 samples at
//     48 kHz), at any host block size: the voice keeps the last MAX_CHUNK samples it made and
//     tick() hands over what was made chunkSize() samples before each sample. For a steady tone
//     that is a turn of its phase; an engine that needs its own signal on its own phase within
//     the sample (an FM algorithm) keeps that path and publishes beside it. A source whose
//     samples exist before the voice renders (the track input as the engine filters it: the
//     wavetable's XFLT / XRCT) is published with AudioRate | Immediate and handed over BEFORE
//     tick() for the sub-block itself: no delay. At most sixteen sources an engine
//     (Src::MAX_PUBLISHED). The list is append-only per engine: a later source, the engine's or
//     the router's, is added by a call after the existing ones.
//     The input itself, for an engine that works something out from it: readsInput(true) before
//     beginChunk(), inputNow() after it (faded where it ends, as the slots read it).
//  9. THE TRACK INPUT'S ONSETS AND BANDS need nothing from the engine: ONST and BND1 .. BN16 are
//     sources on every router, and an LFO's TRIG at XKEY / XONE / XHLF / XHLD restarts on a hit
//     in the input. The router works them out from the input it is given in beginBlock(), only
//     while a slot or an LFO reads them. An engine that restarts something of its own on a hit
//     (an envelope: by a cell of the engine's) calls router.needOnsets(true) while that cell is
//     on and reads router.onsetHeard() after each beginChunk(): true = a sound began in the
//     input within this sub-block; retrigger the envelopes of the sounding voices then (they
//     start from their present level: no click).
// 10. WHAT A PORT DELETES from the engine: its use of VoiceLfos / EngineLfoSection (the LFO and
//     STRIP pages, LfoDestList and its computeModulation block) and of EngineModSection /
//     EngineModSlots (SLT pages, LSHP) ; its XIN cells and their page (they are slots: step 2's
//     XIN page, or a free slot with source XIN / XLP / XHP / XBP) ; its private vibrato
//     oscillator (the source VIB and the STRIP page's WVIB / VRAT) ; its own strip smoothing
//     where it only fed routes (EngineBase::smoothWheel once a block is what beginBlock takes;
//     the filter's WHL amount stays until it is a default slot) ; its private per-note random
//     and any scaled copy of an envelope (6, 7). When the last engine is ported those headers go.
//  For an engine that is built differently: more than two LFOs or LFO pages of its own - setLfo()
//  with Phase / Delay / KeyTrack, DestTable::lfoCell() for "LFO n's rate" as an entry an LFO page
//  can aim at, appendPages(pages, PageIds) to show STRIP and XIN at other ids and leave the LFO
//  pages out; further envelopes - Router(table, lfos, envelopes) and Voice::setEnvelope() before
//  tick(): the sources ENV2 .. ENV5.
//
// PER ENGINE, as the engines on the router stand (U3-router-oscillator.md, U4-*.md):
//   FM              ported (U4). 84 entries: the old 75 at their places (LRAT / LDPT at 67, POS1
//                   .. POS6 at 69), ETIM FTIM, then PM1 .. PM6 and PMAL: Phase destinations in
//                   radians (.period(2 pi)), the old XIN cells (64 / 65 still accepted on set) as
//                   the XIN page's DST1 / AMT1. PTCH AMP PAN CUT INDX FDBK POSn PMn per sample;
//                   stripScale INDX x 3. Publishes OP1 .. OP6 at audio rate, handed over after each
//                   sub-block (8: heard one whole sub-block late at any host block). ETIM is every
//                   operator's envelope. Seeds its voices (0, voice); KTRK / VEL / EVEL / WHL stay
//                   filter cells.
//   wavetable       ported (U4). 42 entries: the old 39, ETIM FTIM, PM (a Phase in cycles, span 2,
//                   amount squared: the voice has no let-go of its own). POS is an INPUT (an offset
//                   around the glided position), not the Position cell; with an audio-rate slot on
//                   it whose input is heard, the whole of POS is added after the glide and folded
//                   at the ends. Publishes SCAN (control rate), XRCT and XFLT (AudioRate |
//                   Immediate: the input through the engine's own two-pole filters, read with
//                   readsInput / inputNow, handed over before tick(), no delay); the XIN page's
//                   LPF / HPF default open on it. VIBR is a cell fed by Router::vibrato(). The RAND
//                   scan draws from spread::Sequence (stream Scan); ETIM through setTimeScale.
//   wave sequencer  ported (U4). 86 entries: 75 to T16, BODY's eight (133-140), ETIM FTIM, PM (a
//                   Phase in cycles): ten from DestTable::MAX. Presets hold out-of-range PRNG
//                   values: only routed cells are held to their range, by construction. Publishes
//                   its lanes PLAN SLAN LLAN TLAN at control rate, before tick(); RATE / XFAD /
//                   PRNG are moved for the sub-block and given back.
//   sampler         ported (U4). Chooses a PressureMap (VOL CUT PAN, TBR on DRIV, WAV on GPOS;
//                   a sampler track is made with no route); STRT, END, the loop points and the
//                   slice are read once, at the note's first sub-block after tick(); the strip's
//                   own routes to DRIV are drive with the level taken back (processBlock), a free
//                   slot from STRP is drive as asked for. ETIM only (envTimes(false)). Publishes
//                   POS, GRN, SLEV at control rate; grains and slices draw from spread::Sequence.
//   drum kit        one Router for the kit, Voice::useBase per drum for the kick-page cells; the
//                   strip is read at the strike through the note-on's snap. (Slots per pad wait
//                   for format 3.)
//   RSN             the exciter's energy is a hidden destination with mod::Steps, as the macro's
//                   BOW. ChoiceGroups::MAX_LISTS (96, ParamSpec.h) must grow first: 30 tables x 2
//                   lists, and a source list an engine.
//   unified voice   setLfo with Phase / Delay / KeyTrack, lfoCell, PageIds, ENV2 .. ENV5. XIN PM
//                   and RING are PerSample destinations; SYNC is a PerSample destination whose
//                   buffer (amount x input) the voice thresholds itself. Still missing here: its
//                   additive LFO depth cells (WHEEL / VEL / PRES). Under the P01 pin: its three
//                   envelope copies become setTimeScale, which is the same product
//                   (time x factor, then the 1 ms floor) and the old class bit for bit at 1.
//   macro           done. PLCK, DUST and BOW go for BODY and the exciter's grain axis; it has no
//                   ANLG page yet (its randoms are seeded 0).
//
// SOURCES (mod::Src). A SRC or VIA cell stores a PLACE in the engine's list (SourceTable), which
// only grows at its end; Router::sourceIndex(mod::Src::X) is the place of a source.
//   LFO1 LFO2       the LFO's shape with its fade-in (the slot's amount is the depth)
//   ENV FENV        the amp envelope and the filter's, 0..1
//   VEL KEY RAND    velocity 0..1; (note - 60) / 60; one value a note, -1..1
//   PRES            this voice's own pad pressure, 0..1
//   STRP IN         the mod strip and the follower of the track input, 0..1, the same for all voices
//   XIN XLP XHP XBP THE TRACK INPUT AT AUDIO RATE (EngineBase::audioInput, the mean of its sides):
//                   as it is, through a low-pass (the XIN page's LPF), a high-pass (HPF), or both
//   VIB             the engine's vibrato sine (STRIP page, VRAT), the same phase in every voice
//   ONE             a constant 1: an offset, or something for a VIA to open and close
//   LFO3 LFO4       on an engine with that many
//   ENV2 .. ENV5    an engine's further envelopes, 0..1 (only on a router made for them)
//   RND2 RND3 RND4  three more values a note, -1..1, independent of RAND and of one another
//   ONST            a sound begins in the track input (dyn::Onset: out of silence, or a jump of
//                   4 dB over the floor it was at): 1 at the hit, falling to 1 / e in 0.1 s. The
//                   same for all voices. For a shape and a length of one's own: an LFO at XONE.
//   BND1 .. BN16    the level of the track input in the vocoder's sixteen bands (voc::Analysis:
//                   100 Hz .. 8 kHz, five semitones apiece; up in 2 ms, down in 60), 0..1:
//                   another track's spectrum as sixteen control signals. The same for all voices.
//   (the engine's)  what it publishes, at control rate or as samples (HOW TO PORT, 8)
// VIA is one of the same list: the amount is multiplied by it taken as 0..1 (a source that
// swings both ways is moved up: (v + 1) / 2).
// The list is grouped for Func + turn: OFF .. | VEL .. | IN .. | VIB ONE | (LFO3 ..) | RND2 .. |
// ONST BND1 .. | the engine's own.
//
// SCALE. A slot gives  source x amount x via  (-1..1); the destination's span says what 1 is:
// 12 semitones for pitch with the AMOUNT SQUARED (10 % = 12 cents, 50 % = 3 semitones), 4 octaves
// for cutoff, a gain of 1 + sum for level (0..4), +-1 for pan, a cell's whole range (linear) or
// half of it in octaves (logarithmic). From the strip's own routes a destination may count more
// (DestTable::stripScale). The audio-rate sources follow the same laws per sample: XIN on PTCH
// is exponential FM of up to an octave either way, on CUT filter FM, on AMP amplitude modulation.
// On a destination with an audio floor (DestTable::audioFloor: the level) the audio-rate part is
// added after the control-rate sum has been held to its range and may take the value below it:
// ONE > AMP at -100 takes the 1 away and XIN > AMP is ring modulation (ONE at -d and XIN at d:
// the cross-fade from the plain sound to the ring).
//
// MODULATION OF MODULATION. The last entries of a free slot's DEST are not in the table: L1RT
// L1DP ... (one LFO's rate, +-2 octaves, and depth, x 0..2) and AMT1 .. AM16 (the amount of a free
// slot, added and held to -1..1). LRAT / LDPT in the table are "the other LFOs" for an LFO and
// "every LFO" for any other source. All of these act one sub-block late.
//
// THE SLOTS (Router::FREE0 ...): 16 free (SRC DEST AMT VIA, ids 700-763), one per LFO (source the
// LFO, amount its DPTH, destination its DEST), three for the STRIP page (source STRP) and one for
// its WVIB (VIB via STRP on the pitch), two for the XIN page (source XIN / XLP / XHP / XBP by FLT), four
// for the track's pressure routes (source PRES; cells at PressureSlotParam ids 773-784: THE PRESSURE
// SLOTS below). The pages LFO1 / LFO2 / STRIP / XIN and the AT ROUTE screen are views of these:
// nothing but the fixed source sets them apart.
// An LFO's TRIG is dsp/LFO.h's FREE KEY ONE HALF HOLD, then XKEY XONE XHLF XHLD: the same four
// with a hit in the track input as the trigger instead of the note (Router::triggerLabels).
//
// THE PRESSURE SLOTS (AT ROUTE). Four slots a router, three cells each (PressureSlotParam, ids
// 773-784: DEST, AMT, BIPOLAR), stored with the engine's parameters: they are part of the sound (a
// preset, a project, an undo step carry them) and an engine is made with its defaults in them
// (DestTable::pressureRoute: the level on the synths, EXC on the resonator, DEC on a drum kit,
// nothing on the sampler), so a fresh, an auditioned and a parked engine are alike.
//   DEST    a value of the pressure list (DestTable::pressureLabels): 0 OFF; an index of the table,
//           the hidden entries too (EXC, BOW); then VIB (the value size()): the vibrato sine brought
//           in by the pad, +-AMT semitones at full pressure (VIB via the pad on the pitch, as the
//           old route did it); then the cells of the track's effects (TrackDest), summed by the
//           voice that leads them. AT ROUTE shows the list in another order (pressureMenu: OFF, what
//           only pressure reaches, the table, the effects); the stored value does not change with it.
//   AMT     -8 .. 8 (the screen offers -1 .. 1): the slot gives pressure x AMT in the destination's
//           own scale, never squared (1 = its whole span: a gain of 1 + pressure on the level, four
//           octaves of cutoff, an octave of pitch).
//   BIPOLAR the pad read as 2 p - 1 (a pad at rest is still no pressure at all).
// A slot counts only for a voice whose pad is pressed. An engine that plays some destinations on a
// path of its own (the unified voice's six classic ones, which P01 pins; a drum kit's seven) says
// so once with setOwnPressure(): resolve() leaves those slots out and the engine reads the cells
// (pressureSlot, pressureIsOwn). Old projects store AftertouchRouter routes: each engine says what
// a Destination is here (EngineBase::pressureDestOf, what its PressureMap said before the slots
// had cells) and EngineBase::setAftertouchRouter loads them into the cells.
//
// COST. Control-rate work is done once per sub-block (16 samples at 48 kHz, 32 at 96, 64 at 192)
// for the slots in use only; PerSample destinations get a linear ramp between sub-blocks, or the
// audio-rate sum where a slot with an audio source is aimed at them: the per-sample path exists
// only for those slots. A voice with nothing routed returns from tick() after a few compares.
// The strip's routes count only while the strip is up and the pressure routes only for a voice
// whose pad is pressed, so an engine's defaults cost nothing at rest.
// The input's onsets (a detector a sample) and bands (32 ringing modes and 16 followers a sample,
// once per engine, not per voice) run only while a slot or an LFO reads them, and an engine's own
// source is worked out by the voice only while Router::wants() says so: unread, each costs one
// test a sub-block; read at audio rate, a copy of its samples into the voice's ring and out
// (4 KB a voice for the sixteen rings). The input's fade where it ends is worked out only while
// something reads the input.
// Time the port (tests/mod_router_tests.h, NEPTUNE_ROUTER_BENCH): the plain path is the engine's
// own loop and what the compiler makes of it follows what is inlined around it.
//
// NOTHING IS JUMPED TO. LFO and envelope sources move no faster than their whole swing in 4 ms
// (pitch takes them as they are: a pitch step is no click), the once-a-block sources (PRES, STRP,
// IN) over 5 ms; a slot that is changed or taken away under a sounding note is carried off its
// destination over 4 ms (the difference it leaves is added back and faded out), and a control-rate
// cell that is let go is given its set value again. An audio-rate slot cannot be carried off as a
// sum, so its gain is what is led: to a new amount over at most 4 ms, and to nothing, where it
// was, when the slot goes, is aimed elsewhere or reads another filter (the input is kept running
// for that long after its last slot). A note-on takes everything as it stands.
// A PHASE destination (mod::Phase) is the same at every whole period: carried off, it would run
// the oscillator fast or slow for 4 ms (two cycles held: a blip of 500 Hz). So where a slot comes,
// goes, is re-aimed or changed under a note, the whole periods of the step are dropped at once (no ramp crosses them) and the rest, half a
// period at most, is carried off at half a period in 4 ms (or as any destination is, where that
// is slower); a source that such a slot begins to
// read starts where it stands. The slots on it are summed as they are meanwhile and exactly once
// the carry has gone: an audio-rate slot on a phase is never slowed or bent. (Exact where the
// voice adds the phase as it is; FM scales it by INDX, so there a step is a whole period only at
// an INDX of 1.)
// The TRACK INPUT that ends - taken away, or shorter than the block - is let go of in 4 ms from
// its last sample, and let in again as fast where it comes back (beginChunk): the router never
// reads silence from the sample it ends.
//
// THE TRACK'S EFFECTS (E4, mod::TrackDest). After everything a DEST cell showed before come the
// cells of the effects on the engine's track: the eight insert slots, each with its eight cells
// (1:C1 .. 1:C8: FX and FX2 as the TRK FX screen shows them) and its WET (1:WT), then MODFX's eight
// (CH.R .. RO.D). A group a slot for Func + turn. They are appended to BOTH lists - the LFO / STRIP
// / XIN pages' (DestTable::cellLabels: the visible entries, then these) and a free slot's (after
// AMT1 .. AM16) - so no index stored before E4 moves. (A table that later grows by a visible entry
// moves them, as it moves L1RT .. and AMT1 .. in a free slot today: format 3 has to store a DEST by
// name before a table grows again.)
// They are NOT destinations of the voices: no mask bit, no buffer, no entry of the table. An effect
// is the track's and wants one value a block, so a slot aimed at one (Kind ToTrack) is summed by
// ONE voice, the leader, into Router::trackOffset(d): a share of the cell's range, -1 .. 1, which
// the AudioEngine hands to EffectBase::modulateCell before the track's effects run
// (AudioEngine::moveTrackEffects), 0 once when the last slot on a cell goes. The effects glide to
// what they are given, so nothing is carried off here.
//   The leader: the voice whose note began last among those that sound (as of the sub-block
//   before); when none sounds, the one that led last goes on from Voice::idle() - its LFOs run on,
//   its envelopes are at rest, its VEL / KEY / RAND stay as its last note left them - so a free LFO
//   on a delay time moves in silence; before the first note, the first voice the engine looks at.
//   (A drum kit: the drum struck last.) When the lead passes to another voice the offset may step;
//   the effect glides.
// Every source reaches them; an audio-rate one (XIN, an operator) is read as it stands at the end
// of each sub-block, as on any control-rate destination. A slot's amount can be moved (AMTn) and
// VIA works as everywhere. Cost: a track with no such slot pays one test a sub-block and one per
// block in the AudioEngine; with one, every voice that sounds runs the slots' sources in every
// sub-block (tick() no longer returns at once for a voice nothing else moves) and the leader the
// sum; the strip's routes still count only while the strip is up.
//
// THREADS. Cells are set from the message thread (as every engine parameter is, with no lock) and
// only counted there; the slots are resolved from them on the audio thread, at the next
// beginBlock(), where every cell is read once and every index made valid. So no voice ever runs
// on slots that are half made, and a burst of changes (a preset) is resolved once. routed() and
// idle() are as of that beginBlock(). Nothing here allocates or locks after construction.
// -----------------------------------------------------------------------------

namespace mod
{

namespace Src
{
    static constexpr int MAX_LFOS = 4;
    static constexpr int NUM_AUDIO = 4;
    static constexpr int MAX_ENVELOPES = 4;     // Env2 .. Env5
    static constexpr int NUM_RANDOM = 4;        // Random, Random2 .. Random4
    static constexpr int NUM_BANDS = voc::NUM_BANDS;
    static constexpr int MAX_PUBLISHED = 16;    // an engine's own sources (SourceTable::publish)

    // What a slot can read. These numbers are the router's own and are NOT stored: a SRC or VIA
    // cell holds a place in the engine's list (SourceTable), which only grows at its end. For the
    // sources up to One - and LFO3 .. ENV5 on an engine that has them - the two are the same.
    enum
    {
        Off = 0, Lfo1, Lfo2, Env, FilterEnv, Velocity, Pressure, Strip, Key, Random, Input,
        Xin, XinLow, XinHigh, XinBand, Vib, One,
        Lfo3, Lfo4,
        Env2, Env3, Env4, Env5,     // an engine's further envelopes (Voice::setEnvelope)
        Random2, Random3, Random4,  // further values drawn at the note-on, independent of RAND
        Onset,                      // 1 when a sound begins in the track input, then falling
        Band1,                      // the level of the track input in sixteen bands, 100 Hz .. 8 kHz
        BandLast = Band1 + NUM_BANDS - 1,
        Published1,                 // the engine's own (Voice::publish / publishAudio)
        PublishedLast = Published1 + MAX_PUBLISHED - 1,
        COUNT,
        PressureBipolar = COUNT,    // a bipolar pressure route's reading of the pad (not offered)
        TOTAL
    };
    static_assert(TOTAL <= 64, "the sources a slot reads are a 64-bit set");

    static constexpr uint64_t bit(int source) { return uint64_t (1) << source; }
    static constexpr uint64_t BAND_BITS = ((uint64_t (1) << NUM_BANDS) - 1) << Band1;
    static constexpr int ofLfo(int lfo) { return lfo < 2 ? Lfo1 + lfo : Lfo3 + lfo - 2; }
    static constexpr int lfoIndex(int source)
    {
        return source == Lfo1 ? 0 : (source == Lfo2 ? 1 : (source >= Lfo3 && source <= Lfo4 ? source - Lfo3 + 2 : -1));
    }
    static constexpr int ofRandom(int which) { return which <= 0 ? (int) Random : (int) Random2 + which - 1; }
    static constexpr bool isAudio(int source) { return source >= Xin && source <= XinBand; }
    static constexpr bool isPublished(int source) { return source >= Published1 && source <= PublishedLast; }
    // Where the list of an engine with this many LFOs and further envelopes ended before the
    // sources after ENV5 existed (with further envelopes it runs through LFO4): the places up to
    // there are the same numbers as the enum.
    static constexpr int offered(int numLfos, int envelopes = 0)
    {
        return envelopes > 0 ? (int) Env2 + (envelopes < MAX_ENVELOPES ? envelopes : MAX_ENVELOPES)
                             : (numLfos <= 2 ? (int) Lfo3 : (int) Lfo3 + numLfos - 2);
    }

    // By the enum (an engine's own sources carry the names it published them under).
    inline const char* const* labels()
    {
        static const char* const names[COUNT] = {
            "OFF", "LFO1", "LFO2", "ENV", "FENV", "VEL", "PRES", "STRP", "KEY", "RAND", "IN",
            "XIN", "XLP", "XHP", "XBP", "VIB", "ONE", "LFO3", "LFO4", "ENV2", "ENV3", "ENV4", "ENV5",
            "RND2", "RND3", "RND4", "ONST",
            "BND1", "BND2", "BND3", "BND4", "BND5", "BND6", "BND7", "BND8",
            "BND9", "BN10", "BN11", "BN12", "BN13", "BN14", "BN15", "BN16",
            "OWN1", "OWN2", "OWN3", "OWN4", "OWN5", "OWN6", "OWN7", "OWN8",
            "OWN9", "OW10", "OW11", "OW12", "OW13", "OW14", "OW15", "OW16" };
        return names;
    }
}

// What an engine's slots can read: the list its SRC and VIA cells show. A cell stores a PLACE in
// this list, so the list only ever grows at its end, engine by engine:
//   the list as it was (OFF .. ONE, then LFO3 LFO4 ENV2 .. where the engine has them),
//   RND2 RND3 RND4, ONST, BND1 .. BN16,
//   the engine's own sources, in the order of its publish() calls.
// Whatever comes later - a source of the router's or another of the engine's - is added by a call
// after those. An engine with nothing of its own needs no table: Router takes standard().
// Built once and kept in static storage (the cells point at its names), like a DestTable.
enum SourceFlags
{
    Bipolar = 0,        // swings both ways, -1 .. 1 (as a VIA it is moved up: (v + 1) / 2)
    Unipolar = 1,       // 0 .. 1
    AudioRate = 2,      // handed over as samples (Voice::publishAudio), read per sample by PerSample destinations
    Immediate = 4       // (with AudioRate) the samples exist before the voice renders - the track input as the
                        // engine filters it - and are handed over before tick() for the sub-block itself, heard
                        // with no delay. Without it a voice hands over what it has just MADE, after rendering,
                        // and the slots hear it exactly one whole sub-block later (Voice::publishAudio)
};

class SourceTable
{
public:
    static constexpr int MAX = Src::COUNT;

    explicit SourceTable(int numLfos = 2, int envelopes = 0)
    {
        where.fill(-1);
        group();
        const int first = Src::offered(numLfos, envelopes);
        for (int source = 0; source < first; ++source)
        {
            if (source == Src::Velocity || source == Src::Input || source == Src::Vib || source == Src::Lfo3)
                group();
            push(source, Src::labels()[source]);
        }
        group();
        for (int source = Src::Random2; source <= Src::Random4; ++source)
            push(source, Src::labels()[source]);
        group();
        for (int source = Src::Onset; source <= Src::BandLast; ++source)
            push(source, Src::labels()[source]);
    }

    // The entries added from here on are a new group (Func + turn on SRC / VIA stops at its start).
    SourceTable& group()
    {
        if (numStarts < MAX && (numStarts == 0 || starts[(size_t) (numStarts - 1)] != count))
            starts[(size_t) numStarts++] = count;
        return *this;
    }

    // One of the engine's own per-voice sources, the next in its list. Its number k (0, 1, ..: the
    // order of these calls) is what Router::wants(k) and Voice::publish(k, ..) /
    // publishAudio(k, ..) take. The first one starts a group.
    SourceTable& publish(const char* label, int flags = Bipolar)
    {
        if (numPublished >= Src::MAX_PUBLISHED || count >= MAX || indexOfLabel(label) >= 0)
        {
            jassertfalse;
            return *this;
        }
        if (numPublished == 0)
            group();
        if (flags & AudioRate)      audioSet |= 1u << numPublished;
        if ((flags & AudioRate) && (flags & Immediate))  immediateSet |= 1u << numPublished;
        if (! (flags & Unipolar))   bipolarSet |= 1u << numPublished;
        push(Src::Published1 + numPublished++, label);
        return *this;
    }

    // Call where the table stays: Func + turn on a SRC or VIA cell then moves by group.
    const SourceTable& registerGroups() const
    {
        ChoiceGroups::add(names.data(), starts.data(), numStarts);
        return *this;
    }

    int size() const { return count; }                          // what SRC and VIA offer
    const char* const* labels() const { return names.data(); }
    // The source at a place of the list, and the place of a source (-1: not on this engine).
    int source(int place) const { return place >= 0 && place < count ? (int) sources[(size_t) place] : (int) Src::Off; }
    int indexOf(int source) const { return source >= 0 && source < (int) Src::COUNT ? (int) where[(size_t) source] : -1; }
    int indexOfLabel(const char* label) const
    {
        for (int i = 0; i < count; ++i)
            if (std::strcmp(names[(size_t) i], label) == 0)
                return i;
        return -1;
    }
    int published() const { return numPublished; }
    int placeOfPublished(int k) const { return indexOf(Src::Published1 + k); }
    bool audio(int k) const { return (audioSet >> k) & 1u; }
    bool bipolar(int k) const { return (bipolarSet >> k) & 1u; }
    bool immediate(int k) const { return (immediateSet >> k) & 1u; }
    uint32_t audioPublished() const { return audioSet; }
    int numGroups() const { return numStarts; }
    int groupStart(int g) const { return starts[(size_t) std::clamp(g, 0, std::max(0, numStarts - 1))]; }

    // The list of an engine that publishes nothing.
    static const SourceTable& standard(int numLfos = 2, int envelopes = 0)
    {
        static const std::array<SourceTable, (size_t) (Src::MAX_LFOS * (Src::MAX_ENVELOPES + 1))> tables = []
        {
            std::array<SourceTable, (size_t) (Src::MAX_LFOS * (Src::MAX_ENVELOPES + 1))> all;
            for (int l = 0; l < Src::MAX_LFOS; ++l)
                for (int e = 0; e <= Src::MAX_ENVELOPES; ++e)
                    all[(size_t) (l * (Src::MAX_ENVELOPES + 1) + e)] = SourceTable(l + 1, e);
            return all;
        }();
        const auto& t = tables[(size_t) ((std::clamp(numLfos, 1, (int) Src::MAX_LFOS) - 1) * (Src::MAX_ENVELOPES + 1)
                                         + std::clamp(envelopes, 0, (int) Src::MAX_ENVELOPES))];
        t.registerGroups();     // (only the lists in use take a place among the grouped lists)
        return t;
    }

private:
    void push(int source, const char* label)
    {
        where[(size_t) source] = (int8_t) count;
        sources[(size_t) count] = (uint8_t) source;
        names[(size_t) count++] = label;
    }

    std::array<const char*, MAX> names {};
    std::array<uint8_t, MAX> sources {};
    std::array<int8_t, Src::COUNT> where {};
    std::array<int, MAX> starts {};
    int count = 0, numStarts = 0, numPublished = 0;
    uint32_t audioSet = 0, bipolarSet = 0, immediateSet = 0;
};

// The cells of the effects on the engine's track, as destinations (see THE TRACK'S EFFECTS above).
// A number d: insert slot d / PER_SLOT, cell d % PER_SLOT (0 .. 7 the eight cells, WET_CELL the
// slot's mix: EffectBase::modulateCell's numbers), then MODFX's cells (ModFxBlock::Cell). Stored in
// DEST cells behind everything else: this list only grows at its end.
namespace TrackDest
{
    static constexpr int SLOTS = 8;             // AudioEngine::MAX_INSERTS_PER_TRACK
    static constexpr int CELLS = 8;             // the four of FX and the four of FX2
    static constexpr int WET_CELL = CELLS;      // EffectBase::WET_CELL
    static constexpr int PER_SLOT = CELLS + 1;
    static constexpr int MODFX = SLOTS * PER_SLOT;
    static constexpr int MODFX_CELLS = 8;       // ModFxBlock::NumCells
    static constexpr int COUNT = MODFX + MODFX_CELLS;
    static constexpr int GROUPS = SLOTS + 1;

    static constexpr bool isInsert(int d) { return d >= 0 && d < MODFX; }
    static constexpr bool isModFx(int d) { return d >= MODFX && d < COUNT; }
    static constexpr int slot(int d) { return d / PER_SLOT; }
    static constexpr int cell(int d) { return d % PER_SLOT; }
    static constexpr int ofInsert(int slot, int cell) { return slot * PER_SLOT + cell; }
    static constexpr int ofModFx(int cell) { return MODFX + cell; }
    static constexpr int groupStart(int g) { return g < SLOTS ? g * PER_SLOT : MODFX; }

    inline const char* const* labels();
    // Where these begin in a DEST cell's list of `count` choices (they are its last entries);
    // `count` when the list has none.
    inline int firstIn(const char* const* choices, int count)
    {
        return choices != nullptr && count >= COUNT && choices[count - COUNT] == labels()[0] ? count - COUNT : count;
    }

    inline const char* const* labels()
    {
        static const char* const names[COUNT] = {
            "1:C1", "1:C2", "1:C3", "1:C4", "1:C5", "1:C6", "1:C7", "1:C8", "1:WT",
            "2:C1", "2:C2", "2:C3", "2:C4", "2:C5", "2:C6", "2:C7", "2:C8", "2:WT",
            "3:C1", "3:C2", "3:C3", "3:C4", "3:C5", "3:C6", "3:C7", "3:C8", "3:WT",
            "4:C1", "4:C2", "4:C3", "4:C4", "4:C5", "4:C6", "4:C7", "4:C8", "4:WT",
            "5:C1", "5:C2", "5:C3", "5:C4", "5:C5", "5:C6", "5:C7", "5:C8", "5:WT",
            "6:C1", "6:C2", "6:C3", "6:C4", "6:C5", "6:C6", "6:C7", "6:C8", "6:WT",
            "7:C1", "7:C2", "7:C3", "7:C4", "7:C5", "7:C6", "7:C7", "7:C8", "7:WT",
            "8:C1", "8:C2", "8:C3", "8:C4", "8:C5", "8:C6", "8:C7", "8:C8", "8:WT",
            "CH.R", "CH.D", "FL.R", "FL.D", "PH.R", "PH.D", "RO.S", "RO.D" };
        return names;
    }
}

// A set of destinations.
struct Mask
{
    uint64_t w[2] { 0, 0 };

    void set(int i)             { w[i >> 6] |= uint64_t (1) << (i & 63); }
    void clear(int i)           { w[i >> 6] &= ~(uint64_t (1) << (i & 63)); }
    bool test(int i) const      { return (w[i >> 6] >> (i & 63)) & 1u; }
    bool any() const            { return (w[0] | w[1]) != 0; }
    Mask operator| (const Mask& o) const { return { { w[0] | o.w[0], w[1] | o.w[1] } }; }
    Mask operator& (const Mask& o) const { return { { w[0] & o.w[0], w[1] & o.w[1] } }; }
    Mask without(const Mask& o) const    { return { { w[0] & ~o.w[0], w[1] & ~o.w[1] } }; }
    bool operator== (const Mask& o) const { return w[0] == o.w[0] && w[1] == o.w[1]; }

    template <typename Fn>
    void forEach(Fn&& fn) const
    {
        for (int half = 0; half < 2; ++half)
            for (uint64_t bits = w[half]; bits != 0; bits &= bits - 1)
                fn(half * 64 + (int) __builtin_ctzll(bits));
    }
};

enum DestFlags
{
    Control = 0,
    PerSample = 1,      // the voice takes it sample by sample: ramps, and the audio-rate sources
    Squared = 2,        // the amount counts squared (pitch)
    Steps = 4,          // may step: reads the sources as they are and is never carried off (pitch)
    Exp = 8,            // base x 2 ^ (sum x span) instead of base + sum x span
    Pitch = PerSample | Squared | Steps | Exp,
    Phase = 16          // added to an oscillator's PHASE (with PerSample, never Exp): a whole period of it is no
                        // change of the sound. The router brings it in, carries it off and lets it go without a
                        // step (see NOTHING IS JUMPED TO). Its period is 1 in the destination's own units
                        // (cycles) unless DestTable::period() says otherwise (2 pi for radians)
};

struct Dest
{
    const char* label = "OFF";
    int id = -1;                        // the page cell (its set value is the base); -1: an input of the voice
    float base = 0.0f;                  // an input's value with nothing on it
    float span = 1.0f;                  // what a sum of 1 is (octaves under Exp)
    float lo = 0.0f, hi = 1.0f;         // the result is held to this
    float floor = 0.0f;                 // ... and to this at the bottom once an audio-rate slot has added to it
    float stripScale = 1.0f;
    bool exp = false, perSample = false, squared = false, steps = false;
    float period = 0.0f;                // a phase (mod::Phase): its period in the destination's units; 0: not a phase
    float cycle = 0.0f;                 // ... the same as a sum (period / span)
    uint8_t cross = 0;                  // 1: an LFO's rate, 2: its depth ...
    int8_t lfo = -1;                    // ... of this LFO; -1: of the others
    int16_t buffer = -1;                // which per-sample buffer
};

// What an engine can have moved. Index 0 is OFF; the visible entries (the DEST cells of the LFO,
// STRIP and XIN pages show these; a free slot's DEST shows them and then L1RT .., AMT1 ..) are
// followed by the hidden ones. Built once, kept in static storage.
class DestTable
{
public:
    static constexpr int MAX = 96;                  // visible and hidden together
    static constexpr int NUM_FREE = 16;             // free slots (AMT1 .. AM16)
    static constexpr int MAX_NAMES = MAX + 2 * Src::MAX_LFOS + NUM_FREE + TrackDest::COUNT;
    static constexpr int MAX_CELL_NAMES = MAX + TrackDest::COUNT;
    static constexpr int MAX_PRESSURE_NAMES = MAX + 1 + TrackDest::COUNT;      // the table, VIB, the track's effects
    static constexpr int STRIP_ROUTES = 3;
    struct StripDefault { int dest = 0; float amount = 0.0f; };
    static constexpr int PRESSURE_ROUTES = AftertouchRouter::MAX_ROUTES;
    static constexpr float PRESSURE_AMOUNT = 0.5f;      // a slot with no destination yet: what choosing one starts from
    struct PressureDefault { int dest = 0; float amount = PRESSURE_AMOUNT; };

    DestTable()
    {
        names[0] = items[0].label;
        count = 1;
        starts[(size_t) numStarts++] = 0;
    }

    // The entries added from here on are a new group (Func + turn on DEST stops at its start).
    DestTable& group()
    {
        if (numStarts < MAX && starts[(size_t) (numStarts - 1)] != count)
            starts[(size_t) numStarts++] = count;
        return *this;
    }

    // Something the voice takes as modulation only: `base` with nothing on it, a sum of 1 = span.
    DestTable& input(const char* label, float base, float span, float lo, float hi, int flags = Control)
    {
        Dest d;
        d.label = label;
        d.base = base;
        d.span = span;
        d.lo = lo;
        d.hi = hi;
        return push(d, flags);
    }
    // The entry just added may be taken as low as this by an audio-rate slot (a level: a gain
    // below zero is the other half of a ring modulator).
    DestTable& audioFloor(float lowest)
    {
        items[(size_t) (count - 1)].floor = lowest;
        return *this;
    }

    // The entry just added is a phase (mod::Phase) whose period is this, in its own units (2 pi for
    // radians; the flag alone means 1, a cycle).
    DestTable& period(float units)
    {
        auto& d = items[(size_t) (count - 1)];
        jassert(units > 0.0f && ! d.exp && d.span != 0.0f);
        if (units > 0.0f && ! d.exp && d.span != 0.0f)
        {
            d.period = units;
            d.cycle = units / std::abs(d.span);
        }
        return *this;
    }

    // A page cell. label: another name where the cell's own is taken.
    DestTable& add(const ParamSpec& spec, const char* label = nullptr, int flags = Control)
    {
        if (! spec.isValid() || spec.kind == ParamKind::Choice || spec.maxVal <= spec.minVal)
            return *this;
        Dest d;
        d.label = label != nullptr ? label : spec.label;
        d.id = spec.id;
        d.lo = spec.minVal;
        d.hi = spec.maxVal;
        d.base = spec.defaultVal;
        const bool log = spec.kind == ParamKind::Log && spec.minVal > 0.0f;
        d.span = log ? 0.5f * std::log2(spec.maxVal / spec.minVal) : spec.maxVal - spec.minVal;
        return push(d, flags | (log ? (int) Exp : 0));
    }
    DestTable& add(const ParamPages& pages, int id, const char* label = nullptr, int flags = Control)
    {
        for (auto& page : pages)
            if (const auto* spec = page.find(id))
                return add(*spec, label, flags);
        jassertfalse;
        return *this;
    }

    // LRAT and LDPT: the rate and depth of the other LFOs (of every LFO, for another source).
    DestTable& addCross()
    {
        group();
        static const char* const labels[2] = { "LRAT", "LDPT" };
        for (int i = 0; i < 2; ++i)
        {
            Dest d;
            d.label = labels[i];
            d.cross = (uint8_t) (i + 1);
            push(d, Control);
        }
        return *this;
    }

    // One LFO's rate (+-2 octaves) or depth (x 0..2) as an entry of its own: for an engine whose
    // LFO pages aim at one another by name (a free slot has L1RT .. after the table anyway).
    DestTable& lfoCell(const char* label, int lfo, bool depth)
    {
        Dest d;
        d.label = label;
        d.cross = (uint8_t) (depth ? 2 : 1);
        d.lfo = (int8_t) std::clamp(lfo, 0, (int) Src::MAX_LFOS - 1);
        return push(d, Control);
    }

    // ETIM and FTIM: the time of the amp envelope and of the filter's (every stage of it), as a
    // factor: 1 with nothing on it, three octaves either way at full depth (eight times as long,
    // or an eighth), held to 1/32 .. 32. The voice gives the value to Envelope::setTimeScale
    // (times the note's EVAR factor): forEachControl hands it over like any control-rate
    // destination, and hands 1 back when the slot goes. ampTime() / filterTime() are the indices.
    static constexpr float ENV_TIME_OCTAVES = 3.0f, ENV_TIME_MAX = 32.0f;
    DestTable& envTimes(bool filterToo = true)
    {
        input("ETIM", 1.0f, ENV_TIME_OCTAVES, 1.0f / ENV_TIME_MAX, ENV_TIME_MAX, Exp);
        etim = indexOfLabel("ETIM");
        if (filterToo)
        {
            input("FTIM", 1.0f, ENV_TIME_OCTAVES, 1.0f / ENV_TIME_MAX, ENV_TIME_MAX, Exp);
            ftim = indexOfLabel("FTIM");
        }
        return *this;
    }
    int ampTime() const { return etim; }            // -1: the table has none
    int filterTime() const { return ftim; }

    // What only a fixed slot reaches (the pressure adapter's BOW ...): after the visible entries.
    DestTable& hidden(const char* label, float base, float span, float lo, float hi, int flags = Control)
    {
        if (shown < 0)
            shown = count;
        return input(label, base, span, lo, hi, flags);
    }

    // ---- What the mod strip does until the STRIP page is edited ----------------------------
    DestTable& stripRoute(int route, int dest, float amount)
    {
        if (route >= 0 && route < STRIP_ROUTES && dest >= 0 && dest < count)
            stripDefaults[(size_t) route] = { dest, std::clamp(amount, -1.0f, 1.0f) };
        else
            jassertfalse;
        return *this;
    }
    DestTable& stripRoute(int route, const char* label, float amount) { return stripRoute(route, indexOfLabel(label), amount); }
    DestTable& stripVibrato(float semitones) { stripVibratoDefault = semitones; return *this; }
    // From the strip's own routes the amount on this destination counts `scale` times.
    DestTable& stripScale(int dest, float scale)
    {
        if (dest >= 0 && dest < count)
            items[(size_t) dest].stripScale = scale;
        return *this;
    }

    // ---- What pad pressure does until AT ROUTE is edited (the pressure slots' cells as an
    // engine is made with them). The destination may be a hidden one (EXC).
    DestTable& pressureRoute(int route, int dest, float amount)
    {
        if (route >= 0 && route < PRESSURE_ROUTES && dest > 0 && dest < count && std::isfinite(amount))
            pressureDefaults[(size_t) route] = { dest, amount };
        else
            jassertfalse;
        return *this;
    }
    DestTable& pressureRoute(int route, const char* label, float amount) { return pressureRoute(route, indexOfLabel(label), amount); }

    // Closes the table (where it will stay): the names a free slot's DEST shows, the id lookup,
    // the groups for Func + turn.
    DestTable& finish(int numLfos = 2)
    {
        if (shown < 0)
            shown = count;
        lfos = std::clamp(numLfos, 1, (int) Src::MAX_LFOS);
        static const char* const lfoNames[2 * Src::MAX_LFOS] = { "L1RT", "L1DP", "L2RT", "L2DP", "L3RT", "L3DP", "L4RT", "L4DP" };
        static const char* const amounts[NUM_FREE] = { "AMT1", "AMT2", "AMT3", "AMT4", "AMT5", "AMT6", "AMT7", "AMT8",
                                                       "AMT9", "AM10", "AM11", "AM12", "AM13", "AM14", "AM15", "AM16" };
        slotCount = 0;
        numSlotStarts = 0;
        for (int g = 0; g < numStarts; ++g)
            if (starts[(size_t) g] < shown)
                slotStarts[(size_t) numSlotStarts++] = starts[(size_t) g];
        for (int i = 0; i < shown; ++i)
            slotText[(size_t) slotCount++] = items[(size_t) i].label;
        slotStarts[(size_t) numSlotStarts++] = slotCount;
        for (int i = 0; i < 2 * lfos; ++i)
            slotText[(size_t) slotCount++] = lfoNames[i];
        slotStarts[(size_t) numSlotStarts++] = slotCount;
        for (int i = 0; i < NUM_FREE; ++i)
            slotText[(size_t) slotCount++] = amounts[i];
        // The track's effect cells: behind everything else, in both lists (a group a slot).
        slotTrack = slotCount;
        cellCount = 0;
        numCellStarts = 0;
        for (int g = 0; g < numStarts; ++g)
            if (starts[(size_t) g] < shown)
                cellStarts[(size_t) numCellStarts++] = starts[(size_t) g];
        for (int i = 0; i < shown; ++i)
            cellText[(size_t) cellCount++] = items[(size_t) i].label;
        for (int g = 0; g < TrackDest::GROUPS; ++g)
        {
            slotStarts[(size_t) numSlotStarts++] = slotCount + TrackDest::groupStart(g);
            cellStarts[(size_t) numCellStarts++] = cellCount + TrackDest::groupStart(g);
        }
        for (int i = 0; i < TrackDest::COUNT; ++i)
        {
            slotText[(size_t) slotCount++] = TrackDest::labels()[i];
            cellText[(size_t) cellCount++] = TrackDest::labels()[i];
        }

        // The pressure slots' DEST (see THE PRESSURE SLOTS). By VALUE: the table (hidden entries
        // too), VIB, the track's effects. By PLACE, as AT ROUTE shows them: OFF, what only pressure
        // reaches (the hidden entries, VIB), the table's visible entries, the track's effects.
        jassert(indexOfLabel("VIB") < 0);
        presCount = 0;
        for (int i = 0; i < count; ++i)
            presText[(size_t) presCount++] = items[(size_t) i].label;
        presText[(size_t) presCount++] = "VIB";
        for (int i = 0; i < TrackDest::COUNT; ++i)
            presText[(size_t) presCount++] = TrackDest::labels()[i];
        int place = 0;
        numMenuStarts = 0;
        const auto put = [&](int value)
        {
            menuValue[(size_t) place] = (int16_t) value;
            menuPlace[(size_t) value] = (int16_t) place;
            menuText[(size_t) place++] = presText[(size_t) value];
        };
        menuStarts[(size_t) numMenuStarts++] = 0;
        put(0);
        for (int i = shown; i < count; ++i)
            put(i);
        put(count);
        for (int g = 0, i = 1; i < shown; ++i)
        {
            // (the table's own groups; its first one begins after what only pressure reaches)
            while (g + 1 < numStarts && starts[(size_t) (g + 1)] <= i)
                ++g;
            if (i == 1 || starts[(size_t) g] == i)
                menuStarts[(size_t) numMenuStarts++] = place;
            put(i);
        }
        for (int g = 0; g < TrackDest::GROUPS; ++g)
            menuStarts[(size_t) numMenuStarts++] = place + TrackDest::groupStart(g);
        for (int i = 0; i < TrackDest::COUNT; ++i)
            put(count + 1 + i);
        jassert(place == presCount);

        idToDest.fill(-1);
        numBuffers = 0;
        for (int i = 1; i < count; ++i)
        {
            auto& d = items[(size_t) i];
            if (d.id >= 0 && d.id < (int) idToDest.size())
                idToDest[(size_t) d.id] = (int16_t) i;
            if (d.perSample)
                d.buffer = (int16_t) numBuffers++;
            if (d.steps && pitchDest < 0)
                pitchDest = i;
        }
        return *this;
    }
    // Call where the table stays: Func + turn on its DEST cells then moves by group.
    const DestTable& registerGroups() const
    {
        int visibleStarts = 0;
        while (visibleStarts < numStarts && starts[(size_t) visibleStarts] < shown)
            ++visibleStarts;
        ChoiceGroups::add(names.data(), starts.data(), visibleStarts);
        ChoiceGroups::add(cellText.data(), cellStarts.data(), numCellStarts);
        ChoiceGroups::add(slotText.data(), slotStarts.data(), numSlotStarts);
        ChoiceGroups::add(menuText.data(), menuStarts.data(), numMenuStarts);
        return *this;
    }

    int size() const { return count; }                          // with the hidden entries
    int visible() const { return shown < 0 ? count : shown; }   // the entries of the table a DEST cell shows
    int slotChoices() const { return slotCount; }               // what a free slot's DEST offers
    // What the DEST cell of an LFO, of the STRIP page or of the XIN page offers: the visible
    // entries, then the track's effect cells (TrackDest); and where those begin in a free slot's.
    int cellChoices() const { return cellCount; }
    const char* const* cellLabels() const { return cellText.data(); }
    int trackCell() const { return visible(); }
    int trackSlotChoice() const { return slotTrack; }
    // What a pressure slot's DEST holds (THE PRESSURE SLOTS): the list by value, the value of VIB
    // and of the track's first effect cell; and the same list in the order AT ROUTE shows it
    // (a place of the menu <-> a value of the cell).
    int pressureChoices() const { return presCount; }
    const char* const* pressureLabels() const { return presText.data(); }
    int pressureVibrato() const { return count; }
    int pressureTrack() const { return count + 1; }
    const char* const* pressureMenu() const { return menuText.data(); }
    int pressureValueAt(int place) const { return menuValue[(size_t) std::clamp(place, 0, std::max(1, presCount) - 1)]; }
    int pressurePlaceOf(int value) const { return menuPlace[(size_t) std::clamp(value, 0, std::max(1, presCount) - 1)]; }
    const PressureDefault& pressureDefault(int route) const { return pressureDefaults[(size_t) std::clamp(route, 0, PRESSURE_ROUTES - 1)]; }
    int numLfos() const { return lfos; }
    int buffers() const { return numBuffers; }
    int pitch() const { return pitchDest; }
    const Dest& operator[](int i) const { return items[(size_t) std::clamp(i, 0, count - 1)]; }
    const char* const* labels() const { return names.data(); }
    const char* const* slotLabels() const { return slotText.data(); }
    const StripDefault& stripDefault(int route) const { return stripDefaults[(size_t) std::clamp(route, 0, STRIP_ROUTES - 1)]; }
    float stripVibratoDefaultValue() const { return stripVibratoDefault; }
    int numGroups() const { return numStarts; }
    int groupStart(int g) const { return starts[(size_t) std::clamp(g, 0, std::max(0, numStarts - 1))]; }

    int indexOfId(int id) const { return id >= 0 && id < (int) idToDest.size() ? idToDest[(size_t) id] : -1; }
    int indexOfLabel(const char* label) const
    {
        for (int i = 0; i < count; ++i)
            if (std::strcmp(items[(size_t) i].label, label) == 0)
                return i;
        return -1;
    }

private:
    DestTable& push(Dest d, int flags)
    {
        if (count >= MAX)
        {
            jassertfalse;
            return *this;
        }
        jassert(indexOfLabel(d.label) < 0);
        d.perSample = (flags & PerSample) != 0;
        d.squared = (flags & Squared) != 0;
        d.steps = (flags & Steps) != 0;
        d.exp = (flags & Exp) != 0;
        d.floor = d.lo;
        if ((flags & Phase) && ! d.exp && d.span != 0.0f)
        {
            d.period = 1.0f;
            d.cycle = 1.0f / std::abs(d.span);
        }
        items[(size_t) count] = d;
        names[(size_t) count++] = d.label;
        return *this;
    }

    std::array<Dest, MAX> items {};
    std::array<const char*, MAX> names {};
    std::array<int, MAX> starts {};
    std::array<const char*, MAX_NAMES> slotText {};
    std::array<int, MAX + 2 + TrackDest::GROUPS> slotStarts {};
    std::array<const char*, MAX_CELL_NAMES> cellText {};
    std::array<int, MAX + TrackDest::GROUPS> cellStarts {};
    int cellCount = 0, numCellStarts = 0, slotTrack = 0;
    std::array<int16_t, EngineParamRange::END> idToDest {};
    std::array<StripDefault, STRIP_ROUTES> stripDefaults {};
    std::array<PressureDefault, PRESSURE_ROUTES> pressureDefaults {};
    std::array<const char*, MAX_PRESSURE_NAMES> presText {}, menuText {};
    std::array<int16_t, MAX_PRESSURE_NAMES> menuValue {}, menuPlace {};
    std::array<int, MAX + 2 + TrackDest::GROUPS> menuStarts {};
    int presCount = 0, numMenuStarts = 0;
    float stripVibratoDefault = 0.0f;
    int count = 0, shown = -1, numStarts = 0, slotCount = 0, numSlotStarts = 0, lfos = 2, numBuffers = 0, pitchDest = -1;
    int etim = -1, ftim = -1;
};

class Voice;

// One engine's router.
class Router
{
public:
    static constexpr int NUM_FREE = DestTable::NUM_FREE;
    static constexpr int MAX_LFOS = Src::MAX_LFOS;
    static constexpr int NUM_STRIP = DestTable::STRIP_ROUTES;
    static constexpr int NUM_XIN = 2;
    static constexpr int NUM_PRESSURE = AftertouchRouter::MAX_ROUTES;
    static constexpr float MAX_PRESSURE_AMOUNT = 8.0f;     // a pressure slot's AMT (what a route's depth could always be)
    // How an engine handed its track's routes over before the slots had cells, once a block:
    // destOf[d] is the table index for AftertouchRouter::Destination d (-1: none here; Vibrato is
    // always VIB). Kept for the reference engines of tests/old/, and it does what it did: the
    // routes are written into the cells (which only count as changed when they differ).
    using PressureMap = std::array<int8_t, AftertouchRouter::DEST_COUNT>;
    void setPressureRoutes(const AftertouchRouter& routes, const PressureMap& destOf)
    {
        for (int i = 0; i < NUM_PRESSURE; ++i)
        {
            const auto r = routes.effectiveRoute(i);
            const int d = (int) r.dest;
            int dest = r.dest == AftertouchRouter::Destination::Vibrato ? table->pressureVibrato()
                     : (d > 0 && d < (int) destOf.size() ? (int) destOf[(size_t) d] : 0);
            float amount = std::isfinite(r.depth) ? std::clamp(r.depth, -MAX_PRESSURE_AMOUNT, MAX_PRESSURE_AMOUNT) : 0.0f;
            if (dest <= 0 || amount == 0.0f)
            {
                dest = 0;
                amount = DestTable::PRESSURE_AMOUNT;
            }
            set(pressureSlotId(i, PressureSlotParam::DEST_OFFSET), (float) dest);
            set(pressureSlotId(i, PressureSlotParam::AMT_OFFSET), amount);
            set(pressureSlotId(i, PressureSlotParam::BIPOLAR_OFFSET), r.bipolar && dest > 0 ? 1.0f : 0.0f);
        }
    }

    // The slots, all alike: the free ones, then those whose source is fixed.
    enum
    {
        FREE0 = 0,
        LFO0 = FREE0 + NUM_FREE,            // an LFO's DEST and DPTH
        STRIP0 = LFO0 + MAX_LFOS,           // the STRIP page's routes
        VIBRATO = STRIP0 + NUM_STRIP,       // WVIB: VIB via STRP on the pitch
        XIN0,                               // the XIN page's routes
        PRES0 = XIN0 + NUM_XIN,             // the pressure routes
        NUM_SLOTS = PRES0 + NUM_PRESSURE
    };
    static constexpr int MAX_CHUNK = 64;

    // ---- Ids -----------------------------------------------------------------------------------
    //   158-165  LFO1: RATE DPTH SHPE TRIG | SYNC DIV FADE DEST     166-173  LFO2
    //   174-181  STRIP: DST1 AMT1 DST2 AMT2 | DST3 AMT3 WVIB VRAT
    //   182-189  XIN:   DST1 AMT1 FLT1 LPF  | DST2 AMT2 FLT2 HPF
    //   700-763  the free slots, four cells each (ModSlotParam); 764-771 the custom LFO shape
    //   (LfoShapeParam); 772 the least number of slots shown
    // (An engine with more than two LFOs, or pages of its own for them, uses setLfo().)
    static constexpr int LFO_BASE = 158, PER_LFO = 8, LFO_CELLS = 2;
    static constexpr int STRIP_BASE = LFO_BASE + PER_LFO * LFO_CELLS;       // 174
    static constexpr int STRIP_VIBRATO = STRIP_BASE + NUM_STRIP * 2;        // 180
    static constexpr int STRIP_VIBRATO_RATE = STRIP_VIBRATO + 1;            // 181
    static constexpr int XIN_BASE = STRIP_VIBRATO_RATE + 1;                 // 182
    static constexpr int XIN_END = XIN_BASE + 8;                            // 190
    enum LfoCell { Rate = 0, Depth, Shape, Trigger, Sync, Division, Fade, DestCell, Phase, Delay, KeyTrack };

    // An LFO's TRIG: dsp/LFO.h's five (FREE KEY ONE HALF HOLD: the note is the trigger), then the
    // same four with A SOUND THAT BEGINS IN THE TRACK INPUT as the trigger instead of the note
    // (XKEY XONE XHLF XHLD: a hit on the track this one listens to restarts the LFO, in every
    // voice at once; a note-on does not). An X one-shot rests at the end of its cycle until the
    // first hit. Stored by index: the list only grows at the end.
    static constexpr int LFO_TRIGGERS = (int) LFO::TriggerMode::COUNT;
    static constexpr int NUM_TRIGGERS = LFO_TRIGGERS + 4;
    static_assert(LFO_TRIGGERS == 5, "the names below are dsp/LFO.h's five and the four that follow");
    static const char* const* triggerLabels()
    {
        static const char* const names[NUM_TRIGGERS] = { "FREE", "KEY", "ONE", "HALF", "HOLD", "XKEY", "XONE", "XHLF", "XHLD" };
        return names;
    }
    static constexpr bool triggeredByInput(int trigger) { return trigger >= LFO_TRIGGERS; }
    static constexpr int lfoMode(int trigger) { return triggeredByInput(trigger) ? trigger - LFO_TRIGGERS + 1 : trigger; }
    enum XinCell { XinDest1 = 0, XinAmount1, XinFilter1, XinLowPass, XinDest2, XinAmount2, XinFilter2, XinHighPass };

    static constexpr int lfoId(int lfo, int cell) { return LFO_BASE + lfo * PER_LFO + cell; }
    static constexpr int stripDestId(int route) { return STRIP_BASE + route * 2; }
    static constexpr int stripAmountId(int route) { return STRIP_BASE + route * 2 + 1; }
    static constexpr int xinId(int cell) { return XIN_BASE + cell; }
    static constexpr bool ownsSlotBlock(int id) { return id >= ModSlotParam::BASE && id <= ModSlotParam::COUNT; }
    static constexpr bool owns(int id) { return (id >= LFO_BASE && id < XIN_END) || ownsSlotBlock(id) || ownsPressureSlot(id); }

    static constexpr float MIN_RATE = 0.02f, MAX_RATE = 40.0f, MAX_FADE = 5.0f;
    static constexpr float MAX_STRIP_VIBRATO = 1.0f;        // semitones
    static constexpr float MIN_VIBRATO_RATE = 0.5f, MAX_VIBRATO_RATE = 12.0f, DEFAULT_VIBRATO_RATE = 5.5f;
    static constexpr float RATE_OCTAVES = 2.0f;             // what a sum of 1 does to an LFO's rate
    static constexpr float MIN_XIN_HZ = 20.0f, MAX_XIN_HZ = 20000.0f;
    static constexpr int PER_PAGE = 2, MAX_SLOT_PAGES = NUM_FREE / PER_PAGE, DEFAULT_SHOWN = 2;
    static constexpr float ONSET_FALL_S = 0.1f;             // ONST falls to 1 / e in this
    static constexpr float BAND_RELEASE_MS = 60.0f;         // how fast a band's level falls (it rises in 2 ms)

    struct LfoCells
    {
        float rate = 2.0f, depth = 0.0f, fade = 0.0f, phase = 0.0f, delay = 0.0f, keyTrack = 0.0f;
        int shape = 0, trigger = 0, division = LFO::DEFAULT_DIVISION, dest = 0;
        bool sync = false;
    };
    struct FreeCells { int src = 0, dest = 0, via = 0; float amount = 0.0f; };

    // envelopes: how many further envelopes the engine's voices hand over (sources ENV2 ..).
    // sourceTable: the engine's list of sources, made for the same numLfos and envelopes, when it
    // publishes sources of its own (nullptr: SourceTable::standard).
    explicit Router(const DestTable& destTable, int numLfos = 2, int envelopes = 0, const SourceTable* sourceTable = nullptr)
        : table(&destTable), lfoCount(std::clamp(numLfos, 1, MAX_LFOS)),
          sourceList(sourceTable != nullptr ? sourceTable : &SourceTable::standard(lfoCount, envelopes)),
          sourceCount(sourceList->size())
    {
        jassert(destTable.numLfos() == lfoCount);
        jassert(sourceList->source(Src::offered(lfoCount, envelopes) - 1) == Src::offered(lfoCount, envelopes) - 1);
        bipolarSources = Src::bit(Src::Key) | Src::bit(Src::Random) | Src::bit(Src::Vib) | Src::bit(Src::Random2)
                         | Src::bit(Src::Random3) | Src::bit(Src::Random4);
        for (int k = 0; k < Src::NUM_AUDIO; ++k)
            bipolarSources |= Src::bit(Src::Xin + k);
        for (int k = 0; k < sourceList->published(); ++k)
            if (sourceList->bipolar(k))
                bipolarSources |= Src::bit(Src::Published1 + k);
        for (int i = 0; i < DestTable::MAX; ++i)
            base[(size_t) i] = destTable[i].base;
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            stripDest[(size_t) r] = destTable.stripDefault(r).dest;
            stripAmount[(size_t) r] = destTable.stripDefault(r).amount;
        }
        vibratoDepth = std::clamp(destTable.stripVibratoDefaultValue(), 0.0f, MAX_STRIP_VIBRATO);
        for (int p = 0; p < NUM_PRESSURE; ++p)
            pressureSlots[(size_t) p] = { destTable.pressureDefault(p).dest, destTable.pressureDefault(p).amount, false };
        for (int i = 0; i < LFO::USER_POINTS; ++i)
            levels[(size_t) i] = LFO::defaultUserLevel(i);
        scratch.assign((size_t) (std::max(1, destTable.buffers()) * MAX_CHUNK), 0.0f);
        prepare(44100.0);
        resolve();
    }

    // Another table of the same shape (an engine whose cells are named by a model): the labels.
    void setTable(const DestTable& other)
    {
        jassert(other.size() == table->size() && other.visible() == table->visible() && other.buffers() == table->buffers());
        table = &other;
    }
    const DestTable& dests() const { return *table; }
    const SourceTable& sources() const { return *sourceList; }
    int numLfos() const { return lfoCount; }
    int numSources() const { return sourceCount; }     // what SRC and VIA offer
    // The value a SRC or VIA cell takes for a source of mod::Src (-1: not on this engine).
    int sourceIndex(int source) const { return sourceList->indexOf(source); }

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        chunk = std::clamp(16 * (int) std::lround(sampleRate / 48000.0), 16, MAX_CHUNK);
        slewStep = (float) (2.0 / (0.004 * sampleRate));
        blockStep = (float) (1.0 / (0.005 * sampleRate));
        phaseStep = (float) (0.5 / (0.004 * sampleRate));
        fadeStep = (float) (1.0 / (0.004 * sampleRate));
        inGain = 1.0f;
        inTail = inHeld = 0.0f;
        updateInputFilters();
        lowState = highState = bandState = 0.0f;
        tailLength = (int) (0.004 * sampleRate) + MAX_CHUNK;
        audioTail = 0;
        publishedKept = 0;
        publishedTail = 0;
        onsets.prepare(sampleRate);
        onsetRate = (float) (1.0 / ((double) ONSET_FALL_S * sampleRate));
        onsetValue = 0.0f;
        onsetNow = false;
        analysis.tune(sampleRate);
        analysis.setTimes(sampleRate, voc::ATTACK_MS, BAND_RELEASE_MS);
        analysis.clear(true);
        bandNow.fill(0.0f);
        onsetsRan = bandsRan = hearing = false;
    }

    // ---- Cells ---------------------------------------------------------------------------------
    float get(int id) const
    {
        if (id >= LFO_BASE && id < STRIP_BASE)
            return getLfo((id - LFO_BASE) / PER_LFO, (id - LFO_BASE) % PER_LFO);
        if (id >= STRIP_BASE && id < XIN_BASE)
        {
            if (id == STRIP_VIBRATO)        return vibratoDepth;
            if (id == STRIP_VIBRATO_RATE)   return vibratoRate;
            const auto r = (size_t) ((id - STRIP_BASE) / 2);
            return (id - STRIP_BASE) % 2 == 0 ? (float) stripDest[r] : stripAmount[r];
        }
        if (id >= XIN_BASE && id < XIN_END)
        {
            const int cell = id - XIN_BASE;
            if (cell == XinLowPass)     return lowHz;
            if (cell == XinHighPass)    return highHz;
            const auto x = (size_t) (cell / 4);
            return cell % 4 == 0 ? (float) xinDest[x] : (cell % 4 == 1 ? xinAmount[x] : (float) xinFilter[x]);
        }
        if (id == ModSlotParam::COUNT)
            return (float) shown;
        if (id >= LfoShapeParam::BASE && id < LfoShapeParam::END)
            return levels[(size_t) (id - LfoShapeParam::BASE)];
        if (id >= ModSlotParam::BASE && id < ModSlotParam::END)
        {
            const FreeCells& s = freeSlots[(size_t) ((id - ModSlotParam::BASE) / ModSlotParam::STRIDE)];
            switch ((id - ModSlotParam::BASE) % ModSlotParam::STRIDE)
            {
                case ModSlotParam::SRC:   return (float) s.src;
                case ModSlotParam::DEST:  return (float) s.dest;
                case ModSlotParam::AMT:   return s.amount;
                default:                  return (float) s.via;
            }
        }
        if (ownsPressureSlot(id))
            return getPressure((id - PressureSlotParam::BASE) / PressureSlotParam::STRIDE,
                               (id - PressureSlotParam::BASE) % PressureSlotParam::STRIDE);
        return 0.0f;
    }

    // Stores the (range-checked) value and applies it.
    void set(int id, float value)
    {
        if (! std::isfinite(value))
            value = 0.0f;
        value = std::clamp(value, -1.0e6f, 1.0e6f);
        if (get(id) == value && id != ModSlotParam::COUNT)
            return;
        const int visible = table->cellChoices();      // (a DEST cell: the table's entries, then the track's effects)
        if (id >= LFO_BASE && id < STRIP_BASE)
        {
            setLfo((id - LFO_BASE) / PER_LFO, (id - LFO_BASE) % PER_LFO, value);
            return;
        }
        if (id >= STRIP_BASE && id < XIN_BASE)
        {
            if (id == STRIP_VIBRATO)            vibratoDepth = std::clamp(value, 0.0f, MAX_STRIP_VIBRATO);
            else if (id == STRIP_VIBRATO_RATE)  vibratoRate = std::clamp(value, MIN_VIBRATO_RATE, MAX_VIBRATO_RATE);
            else if ((id - STRIP_BASE) % 2 == 0) stripDest[(size_t) ((id - STRIP_BASE) / 2)] = index(value, visible);
            else                                stripAmount[(size_t) ((id - STRIP_BASE) / 2)] = std::clamp(value, -1.0f, 1.0f);
        }
        else if (id >= XIN_BASE && id < XIN_END)
        {
            const int cell = id - XIN_BASE;
            const auto x = (size_t) (cell / 4);
            if (cell == XinLowPass)         lowHz = std::clamp(value, MIN_XIN_HZ, MAX_XIN_HZ);
            else if (cell == XinHighPass)   highHz = std::clamp(value, MIN_XIN_HZ, MAX_XIN_HZ);
            else if (cell % 4 == 0)         xinDest[x] = index(value, visible);
            else if (cell % 4 == 1)         xinAmount[x] = std::clamp(value, -1.0f, 1.0f);
            else                            xinFilter[x] = index(value, Src::NUM_AUDIO);
            updateInputFilters();
        }
        else if (id == ModSlotParam::COUNT)
        {
            shown = index(value, NUM_FREE + 1);
            return;
        }
        else if (id >= LfoShapeParam::BASE && id < LfoShapeParam::END)
        {
            levels[(size_t) (id - LfoShapeParam::BASE)] = std::clamp(value, -1.0f, 1.0f);
            return;         // (the LFOs read the levels where they are)
        }
        else if (id >= ModSlotParam::BASE && id < ModSlotParam::END)
        {
            FreeCells& s = freeSlots[(size_t) ((id - ModSlotParam::BASE) / ModSlotParam::STRIDE)];
            switch ((id - ModSlotParam::BASE) % ModSlotParam::STRIDE)
            {
                case ModSlotParam::SRC:   s.src = index(value, sourceCount); break;
                case ModSlotParam::DEST:  s.dest = index(value, table->slotChoices()); break;
                case ModSlotParam::AMT:   s.amount = std::clamp(value, -1.0f, 1.0f); break;
                default:                  s.via = index(value, sourceCount); break;
            }
        }
        else if (ownsPressureSlot(id))
        {
            setPressure((id - PressureSlotParam::BASE) / PressureSlotParam::STRIDE,
                        (id - PressureSlotParam::BASE) % PressureSlotParam::STRIDE, value);
            return;     // setPressure calls changed() itself
        }
        else
            return;
        changed();
    }

    // An LFO's cells by name (the ids 158-173 come here; an engine with pages of its own calls it).
    void setLfo(int lfo, int cell, float value)
    {
        if (lfo < 0 || lfo >= lfoCount || ! std::isfinite(value))
            return;
        auto& c = lfoCells[(size_t) lfo];
        bool routing = false;
        switch (cell)
        {
            case Rate:      c.rate = std::clamp(value, MIN_RATE, MAX_RATE); break;
            case Depth:     c.depth = std::clamp(value, -1.0f, 1.0f); routing = true; break;
            case Shape:     c.shape = index(value, (int) LFO::Shape::COUNT); break;
            case Trigger:
            {
                const int t = index(value, NUM_TRIGGERS);
                if (t != c.trigger)
                    ++lfoRestart[(size_t) lfo];     // every voice's LFO begins again, in step
                routing = triggeredByInput(t) != triggeredByInput(c.trigger);   // (who listens for onsets)
                c.trigger = t;
                break;
            }
            case Sync:      c.sync = value > 0.5f; break;
            case Division:  c.division = index(value, LFO::NUM_DIVISIONS); break;
            case Fade:      c.fade = std::clamp(value, 0.0f, MAX_FADE); break;
            case DestCell:  c.dest = index(value, table->cellChoices()); routing = true; break;
            case Phase:     c.phase = std::clamp(value, 0.0f, 1.0f); break;
            case Delay:     c.delay = std::clamp(value, 0.0f, 10.0f); break;
            case KeyTrack:  c.keyTrack = std::clamp(value, -1.0f, 1.0f); break;
            default:        return;
        }
        ++lfoGen[(size_t) lfo];
        if (routing)
            changed();
    }
    float getLfo(int lfo, int cell) const
    {
        if (lfo < 0 || lfo >= lfoCount)
            return 0.0f;
        const auto& c = lfoCells[(size_t) lfo];
        switch (cell)
        {
            case Rate:      return c.rate;
            case Depth:     return c.depth;
            case Shape:     return (float) c.shape;
            case Trigger:   return (float) c.trigger;
            case Sync:      return c.sync ? 1.0f : 0.0f;
            case Division:  return (float) c.division;
            case Fade:      return c.fade;
            case DestCell:  return (float) c.dest;
            case Phase:     return c.phase;
            case Delay:     return c.delay;
            case KeyTrack:  return c.keyTrack;
            default:        return 0.0f;
        }
    }
    const LfoCells& lfo(int i) const { return lfoCells[(size_t) std::clamp(i, 0, MAX_LFOS - 1)]; }
    const FreeCells& slot(int i) const { return freeSlots[(size_t) std::clamp(i, 0, NUM_FREE - 1)]; }
    const float* shape() const { return levels.data(); }

    void setTempo(float bpm)
    {
        // (Held to what a tempo can be: an infinite one would run a synced LFO's phase off to
        // not-a-number.)
        bpm = std::isfinite(bpm) ? std::clamp(bpm, 1.0f, 1000.0f) : tempo;
        if (bpm == tempo)
            return;
        tempo = bpm;
        for (auto& g : lfoGen)
            ++g;
    }

    // The set value of a cell that is a destination: call whenever the engine's cell is set.
    void setBase(int id, float value)
    {
        const int d = table->indexOfId(id);
        if (d > 0 && std::isfinite(value))
            base[(size_t) d] = value;
    }
    float baseOf(int dest) const { return base[(size_t) std::clamp(dest, 0, DestTable::MAX - 1)]; }

    // ---- The pressure routes (AT ROUTE) as router slots -----------------------------------------
    // Four slots, source fixed at PRES (per voice: polyphonic aftertouch reaches individual
    // voices); the cells are at PressureSlotParam ids 773-784, each slot DEST, AMT, BIPOLAR: see
    // THE PRESSURE SLOTS at the top. The AT ROUTE screen edits them through setTrackParameter.
    struct PressureSlotCells { int dest = 0; float amount = DestTable::PRESSURE_AMOUNT; bool bipolar = false; };

    static constexpr int pressureSlotId(int slot, int offset) { return PressureSlotParam::id(slot, offset); }
    static constexpr bool ownsPressureSlot(int id) { return PressureSlotParam::contains(id); }

    float getPressure(int slot, int offset) const
    {
        if (slot < 0 || slot >= NUM_PRESSURE) return 0.0f;
        const auto& c = pressureSlots[(size_t) slot];
        switch (offset)
        {
            case PressureSlotParam::DEST_OFFSET:    return (float) c.dest;
            case PressureSlotParam::AMT_OFFSET:     return c.amount;
            case PressureSlotParam::BIPOLAR_OFFSET: return c.bipolar ? 1.0f : 0.0f;
            default: return 0.0f;
        }
    }

    void setPressure(int slot, int offset, float value)
    {
        if (slot < 0 || slot >= NUM_PRESSURE || ! std::isfinite(value)) return;
        auto& c = pressureSlots[(size_t) slot];
        switch (offset)
        {
            case PressureSlotParam::DEST_OFFSET:    c.dest = index(value, table->pressureChoices()); break;
            case PressureSlotParam::AMT_OFFSET:     c.amount = std::clamp(value, -MAX_PRESSURE_AMOUNT, MAX_PRESSURE_AMOUNT); break;
            case PressureSlotParam::BIPOLAR_OFFSET: c.bipolar = value > 0.5f; break;
            default: return;
        }
        changed();
    }

    const PressureSlotCells& pressureSlot(int i) const { return pressureSlots[(size_t) std::clamp(i, 0, NUM_PRESSURE - 1)]; }

    // An engine that plays some pressure destinations on a path of its own (the unified voice:
    // P01 pins the arithmetic; a drum kit: each drum's own) says which, once, before it plays:
    // the first `slots` slots are the engine's own while they are aimed at one of `dests` (table
    // indices), or at VIB when `vibrato`. resolve() leaves those out; the engine reads the cells
    // (pressureSlot) and asks pressureIsOwn(slot) which are its to play.
    void setOwnPressure(const Mask& dests, bool vibrato, int slots = NUM_PRESSURE)
    {
        ownDests = dests;
        ownVibrato = vibrato;
        ownSlots = std::clamp(slots, 0, NUM_PRESSURE);
        changed();
    }
    bool pressureIsOwn(int slot) const
    {
        if (slot < 0 || slot >= ownSlots)
            return false;
        const int dest = pressureSlots[(size_t) slot].dest;
        return dest > 0 && (dest < table->size() ? ownDests.test(dest) : (dest == table->pressureVibrato() && ownVibrato));
    }

    // ---- Pages ---------------------------------------------------------------------------------
    int slotPagesShown() const
    {
        int last = -1;
        for (int s = NUM_FREE - 1; s >= 0 && last < 0; --s)
            if (freeSlots[(size_t) s].src != 0 || freeSlots[(size_t) s].dest != 0 || freeSlots[(size_t) s].via != 0 || freeSlots[(size_t) s].amount != 0.0f)
                last = s;
        const int inUse = last < 0 ? 0 : last / PER_PAGE + 1;
        return std::clamp(std::max(inUse + 1, (shown + PER_PAGE - 1) / PER_PAGE), 1, MAX_SLOT_PAGES);
    }

    // XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2: an engine's last pages. ids: for an engine whose
    // own cells are at the STRIP or XIN ids, or that has LFO pages of its own: where the two pages'
    // cells are shown (the engine then hands id - ids.strip + STRIP_BASE and so on to set / get).
    struct PageIds { int strip = STRIP_BASE, xin = XIN_BASE; bool lfoPages = true; };
    void appendPages(ParamPages& pages) const { appendPages(pages, PageIds()); }
    void appendPages(ParamPages& pages, const PageIds& ids) const
    {
        const auto xinId = [&ids](int cell) { return ids.xin + cell; };
        const auto stripDestId = [&ids](int route) { return ids.strip + route * 2; };
        const auto stripAmountId = [&ids](int route) { return ids.strip + route * 2 + 1; };
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto C = ParamKind::Choice;
        const char* const* dests = table->cellLabels();
        const int visible = table->cellChoices();
        const float lastDest = (float) (visible - 1);

        static const char* const filters[Src::NUM_AUDIO] = { "RAW", "LP", "HP", "BP" };
        ParamPage xin;
        xin.name = "XIN";
        xin.section = "XIN";
        xin.params = {{
            { "DST1", xinId(XinDest1), 0, lastDest, 0, C, "", dests, visible },
            { "AMT1", xinId(XinAmount1), -1, 1, 0, L },
            { "FLT1", xinId(XinFilter1), 0, Src::NUM_AUDIO - 1, 0, C, "", filters, Src::NUM_AUDIO },
            { "LPF",  xinId(XinLowPass), MIN_XIN_HZ, MAX_XIN_HZ, 2000.0f, G, "Hz" },
            { "DST2", xinId(XinDest2), 0, lastDest, 0, C, "", dests, visible },
            { "AMT2", xinId(XinAmount2), -1, 1, 0, L },
            { "FLT2", xinId(XinFilter2), 0, Src::NUM_AUDIO - 1, 0, C, "", filters, Src::NUM_AUDIO },
            { "HPF",  xinId(XinHighPass), MIN_XIN_HZ, MAX_XIN_HZ, 200.0f, G, "Hz" } }};
        pages.push_back(xin);

        static const char* const slotPages[MAX_SLOT_PAGES] = { "SLT1", "SLT2", "SLT3", "SLT4", "SLT5", "SLT6", "SLT7", "SLT8" };
        const int sources = sourceCount;
        for (int p = 0; p < slotPagesShown(); ++p)
        {
            ParamPage page;
            page.name = slotPages[p];
            page.section = "SLOT";
            for (int h = 0; h < PER_PAGE; ++h)
            {
                const int s = p * PER_PAGE + h;
                const auto c = (size_t) (h * ModSlotParam::STRIDE);
                page.params[c + 0] = { "SRC", ModSlotParam::id(s, ModSlotParam::SRC), 0, (float) (sources - 1), 0, C, "", sourceList->labels(), sources };
                page.params[c + 1] = { "DEST", ModSlotParam::id(s, ModSlotParam::DEST), 0, (float) (table->slotChoices() - 1), 0, C, "",
                                       table->slotLabels(), table->slotChoices() };
                page.params[c + 2] = { "AMT", ModSlotParam::id(s, ModSlotParam::AMT), -1, 1, 0, L };
                page.params[c + 3] = { "VIA", ModSlotParam::id(s, ModSlotParam::VIA), 0, (float) (sources - 1), 0, C, "", sourceList->labels(), sources };
            }
            pages.push_back(page);
        }
        static const char* const levelNames[LFO::USER_POINTS] = { "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8" };
        ParamPage shapePage;
        shapePage.name = "LSHP";
        shapePage.section = "SLOT";
        for (int i = 0; i < LFO::USER_POINTS; ++i)
            shapePage.params[(size_t) i] = { levelNames[i], LfoShapeParam::BASE + i, -1, 1, LFO::defaultUserLevel(i), L };
        pages.push_back(shapePage);

        static const char* const destNames[NUM_STRIP] = { "DST1", "DST2", "DST3" };
        static const char* const amountNames[NUM_STRIP] = { "AMT1", "AMT2", "AMT3" };
        ParamPage stripPage;
        stripPage.name = "STRIP";
        stripPage.section = "MOD";
        for (int r = 0; r < NUM_STRIP; ++r)
        {
            const auto d = table->stripDefault(r);
            stripPage.params[(size_t) (r * 2)] = { destNames[r], stripDestId(r), 0, lastDest, (float) d.dest, C, "", dests, visible };
            stripPage.params[(size_t) (r * 2 + 1)] = { amountNames[r], stripAmountId(r), -1, 1, d.amount, L };
        }
        stripPage.params[6] = { "WVIB", ids.strip + (STRIP_VIBRATO - STRIP_BASE), 0, MAX_STRIP_VIBRATO, table->stripVibratoDefaultValue(), L, "st", nullptr, 0, 0.01f };
        stripPage.params[7] = { "VRAT", ids.strip + (STRIP_VIBRATO_RATE - STRIP_BASE), MIN_VIBRATO_RATE, MAX_VIBRATO_RATE, DEFAULT_VIBRATO_RATE, G, "Hz" };
        pages.push_back(stripPage);

        static const char* const lfoPages[LFO_CELLS] = { "LFO1", "LFO2" };
        for (int i = 0; ids.lfoPages && i < std::min(lfoCount, (int) LFO_CELLS); ++i)
        {
            const int b = LFO_BASE + i * PER_LFO;
            ParamPage page;
            page.name = lfoPages[i];
            page.section = "MOD";
            page.params = {{
                { "RATE", b + Rate, MIN_RATE, MAX_RATE, 2.0f, G, "Hz" },
                { "DPTH", b + Depth, -1, 1, 0, L },
                { "SHPE", b + Shape, 0, (float) ((int) LFO::Shape::COUNT - 1), 0, C, "", LFO::shapeLabels(), (int) LFO::Shape::COUNT },
                { "TRIG", b + Trigger, 0, (float) (NUM_TRIGGERS - 1), 0, C, "", triggerLabels(), NUM_TRIGGERS },
                { "SYNC", b + Sync, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
                { "DIV",  b + Division, 0, (float) (LFO::NUM_DIVISIONS - 1), (float) LFO::DEFAULT_DIVISION, C, "", LFO::divisionLabels(), LFO::NUM_DIVISIONS },
                { "FADE", b + Fade, 0, MAX_FADE, 0, L, "s" },
                { "DEST", b + DestCell, 0, lastDest, 0, C, "", dests, visible } }};
            pages.push_back(page);
        }
    }

    static juce::String name(int id)
    {
        static const char* const fixed[XIN_END - LFO_BASE] = {
            "LFO1 Rate", "LFO1 Depth", "LFO1 Shape", "LFO1 Trigger", "LFO1 Sync", "LFO1 Division", "LFO1 Fade", "LFO1 Dest",
            "LFO2 Rate", "LFO2 Depth", "LFO2 Shape", "LFO2 Trigger", "LFO2 Sync", "LFO2 Division", "LFO2 Fade", "LFO2 Dest",
            "Strip 1 Dest", "Strip 1 Amount", "Strip 2 Dest", "Strip 2 Amount", "Strip 3 Dest", "Strip 3 Amount",
            "Strip Vibrato", "Strip Vibrato Rate",
            "Input 1 Dest", "Input 1 Amount", "Input 1 Filter", "Input Low-pass",
            "Input 2 Dest", "Input 2 Amount", "Input 2 Filter", "Input High-pass" };
        if (id >= LFO_BASE && id < XIN_END)
            return fixed[id - LFO_BASE];
        if (id == ModSlotParam::COUNT)
            return "Mod Slots";
        if (id >= LfoShapeParam::BASE && id < LfoShapeParam::END)
            return "LFO Shape " + juce::String(id - LfoShapeParam::BASE + 1);
        if (id >= ModSlotParam::BASE && id < ModSlotParam::END)
        {
            static const char* const cells[ModSlotParam::STRIDE] = { " Source", " Dest", " Amount", " Via" };
            return "Slot " + juce::String((id - ModSlotParam::BASE) / ModSlotParam::STRIDE + 1) + cells[(id - ModSlotParam::BASE) % ModSlotParam::STRIDE];
        }
        if (ownsPressureSlot(id))
        {
            static const char* const cells[PressureSlotParam::STRIDE] = { " Dest", " Amount", " Bipolar" };
            return "Pressure " + juce::String((id - PressureSlotParam::BASE) / PressureSlotParam::STRIDE + 1)
                   + cells[(id - PressureSlotParam::BASE) % PressureSlotParam::STRIDE];
        }
        return {};
    }

    // ---- The block -----------------------------------------------------------------------------
    // Once per processBlock, before the voices: the track input (left / right / frames, the
    // follower), the mod strip (0..1, as smooth as the engine has it).
    void beginBlock(const float* inLeft, const float* inRight, int inFrames, float inputFollower, float stripPosition, double sr)
    {
        if (sr != sampleRate && sr > 0.0)
            prepare(sr);
        if (const uint32_t e = edits.load(std::memory_order_acquire); e != resolvedAt)
        {
            resolvedAt = e;
            resolve();
        }
        inL = inFrames > 0 ? inLeft : nullptr;
        inR = inRight != nullptr ? inRight : inLeft;
        frames = inL != nullptr ? inFrames : 0;
        follower = std::isfinite(inputFollower) ? std::clamp(inputFollower, 0.0f, 1.0f) : 0.0f;
        strip = std::isfinite(stripPosition) ? std::clamp(stripPosition, 0.0f, 1.0f) : 0.0f;
    }

    // The next sub-block, starting `offset` samples into a block of `numSamples`: how long it is.
    int beginChunk(int offset, int numSamples)
    {
        const int n = std::min(chunk, numSamples - offset);
        vibratoPhase += (double) vibratoRate * n / sampleRate;
        vibratoPhase -= std::floor(vibratoPhase);
        if (state.usesVibrato)
            vibratoNow = std::sin((float) vibratoPhase * 6.2831853f);
        const bool filtered = state.audioSources != 0 || audioTail > 0;
        if (filtered || inputRead)
        {
            audioTail = std::max(0, audioTail - n);
            // The input for these samples: the mean of its sides, held to +-1 (anything else, or
            // not a number, must not reach a pitch), and the two filtered copies. Where it ends -
            // taken away, or shorter than the block - what it last gave is let go of in 4 ms, and
            // where it comes back it is let in as fast: never dropped, never jumped to (a phase or
            // a position hanging on it would step with it).
            for (int i = 0; i < n; ++i)
            {
                const int at = offset + i;
                const bool have = at < frames;
                if (! have && inGain != 0.0f)
                {
                    inTail = std::clamp(inTail + inHeld * inGain, -1.0f, 1.0f);
                    inGain = 0.0f;
                }
                if (inTail != 0.0f)
                    inTail = dsp::towards(inTail, 0.0f, fadeStep);
                float x = 0.0f;
                if (have)
                {
                    x = 0.5f * (inL[at] + inR[at]);
                    x = std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f;
                    inHeld = x;
                    if (inGain != 1.0f)
                    {
                        inGain = dsp::towards(inGain, 1.0f, fadeStep);
                        x *= inGain;
                    }
                }
                if (inTail != 0.0f)
                    x = std::clamp(x + inTail, -1.0f, 1.0f);
                audio[0][(size_t) i] = x;
                if (! filtered)
                    continue;
                lowState += (x - lowState) * lowK;
                audio[1][(size_t) i] = lowState;
                highState += (x - highState) * highK;
                audio[2][(size_t) i] = x - highState;
                bandState += (x - highState - bandState) * lowK;
                audio[3][(size_t) i] = bandState;
            }
            if (std::abs(lowState) < 1.0e-20f)  lowState = 0.0f;
            if (std::abs(highState) < 1.0e-20f) highState = 0.0f;
            if (std::abs(bandState) < 1.0e-20f) bandState = 0.0f;
        }
        else
        {
            // Nobody reads it: the fade follows the input as it stands (an input that is there
            // when a slot begins to read it is not let in again: the slot's own gain is led).
            inGain = offset + n <= frames ? 1.0f : 0.0f;
            inTail = inHeld = 0.0f;
        }
        if (publishedTail > 0 && (publishedTail -= n) <= 0)
            publishedKept = 0;
        // The track's effect cells: the voice whose note began last among those that sounded in
        // the sub-block before leads this one; if none sounded, the leader goes on; if nobody led
        // (its voice is gone), the first voice that is looked at takes over.
        if (state.numTrack != 0)
        {
            if (claim != nullptr)   leader = claim;
            else if (! led)         leader = nullptr;
            claim = nullptr;
            claimSerial = 0;
            led = false;
        }
        // The track input's onsets and its sixteen band levels: only while somebody reads them.
        if (state.hears || hearing || onsetsWanted.load(std::memory_order_relaxed))
            hear(offset, n);
        return n;
    }

    // As of the last beginBlock():
    // true when a slot is aimed at this destination (whether or not its source moves right now);
    bool routed(int dest) const { return (state.used[0] | state.used[1] | state.used[2]).test(dest); }
    // true when no slot is in use at all (the strip's and the pads' included).
    bool idle() const { return state.count == 0; }
    float stripPosition() const { return strip; }
    // The vibrato sine (the source VIB: STRIP page, VRAT) as of the last beginChunk(), -1 .. 1, for
    // an engine that has a constant-vibrato cell of its own (VIBR) instead of an oscillator for it.
    float vibrato() const { return std::sin((float) vibratoPhase * 6.2831853f); }
    // true when a slot reads the engine's own source k (as its source or its via), or did until a
    // few milliseconds ago: only then need a voice work it out and hand it over. A source nobody
    // reads costs the voice this one test a sub-block.
    bool wants(int k) const { return (((uint32_t) state.published | publishedKept) >> k) & 1u; }
    // The length of a whole sub-block at this sample rate (beginChunk gives less at a block's end).
    int chunkSize() const { return chunk; }

    // ---- The track's effect cells (mod::TrackDest) ---------------------------------------------
    // As of the last beginBlock(): the cells a slot is aimed at; and after the block's last
    // sub-block, what the slots on cell d come to: a share of the cell's range, -1 .. 1, for
    // EffectBase::modulateCell (the AudioEngine hands it over before the track's effects run, and
    // 0 once when a cell is no longer in the set).
    const Mask& trackDests() const { return state.trackUsed; }
    float trackOffset(int d) const { return d >= 0 && d < TrackDest::COUNT ? trackValue[(size_t) d] : 0.0f; }
    // The value a DEST cell takes for track destination d: of an LFO / STRIP / XIN page, of a free slot.
    int trackCellIndex(int d) const { return table->trackCell() + d; }
    int trackSlotIndex(int d) const { return table->trackSlotChoice() + d; }

    // ---- The track input, for the engine itself ------------------------------------------------
    // An engine that works something out from the input on its own (the wavetable's XFLT / XRCT)
    // says so before each beginChunk() and reads inputNow() after it: the n samples of the input
    // as the slots read it (the mean of its sides, held to +-1, faded where it ends or comes back).
    void readsInput(bool wanted) { inputRead = wanted; }
    const float* inputNow() const { return audio[0].data(); }

    // ---- The track input's onsets, for the engine itself ---------------------------------------
    // The router listens for onsets while a slot reads ONST or an LFO's TRIG is one of the X
    // modes. An engine that restarts something of its own on a hit (an envelope: the engine's
    // cell says so) asks for them with needOnsets(true) and reads onsetHeard() after each
    // beginChunk(): true when a sound began in the input within that sub-block.
    void needOnsets(bool wanted) { onsetsWanted.store(wanted, std::memory_order_relaxed); }
    bool onsetHeard() const { return onsetNow; }
    // As of the last beginChunk() (0 while nobody reads them): ONST, and the level of band b.
    float onsetLevel() const { return onsetValue; }
    float bandLevel(int b) const { return bandNow[(size_t) std::clamp(b, 0, (int) Src::NUM_BANDS - 1)]; }

private:
    friend class Voice;

    enum Gate { Always = 0, PressureGate, StripGate, NUM_GATES };
    enum Kind { None = 0, ToDest, ToAmount, ToRate, ToDepth, ToTrack };
    enum ActiveFlags { Modulated = 1, SquaredAmount = 2, Raw = 4, Audio = 8 };

    struct Active
    {
        uint8_t slot = 0, src = 0, via = 0, kind = None, flags = 0, dest = 0;
        int8_t ownLfo = -1;         // this is LFO n's own slot (its depth can be moved; LRAT / LDPT skip it)
        int8_t target = -1;         // ToAmount: the slot; ToRate / ToDepth: the LFO, -1 = every other
        float amount = 0.0f, scale = 1.0f;
    };

    struct State
    {
        std::array<Active, NUM_SLOTS> active {};
        int count = 0;
        int end[NUM_GATES] { 0, 0, 0 };         // active[] is ordered by gate: one past the last of each
        Mask used[NUM_GATES];                   // the destinations of each gate's slots
        Mask audioDests;                        // destinations with an audio-rate slot on them
        int numAudio = 0;
        uint8_t lfoMask = 0;                    // the LFOs a slot reads
        uint8_t onsetLfos = 0;                  // of those, the ones an onset in the track input restarts
        uint64_t sources = 0;                   // bit n: source n is read (as a source or a via)
        uint8_t audioSources = 0;               // bit n: Src::Xin + n is read
        uint16_t published = 0;                 // bit k: the engine's own source k is read
        bool onset = false, bands = false;      // somebody reads the input's onsets / its band levels
        bool hears = false;                     // ... either
        bool usesVibrato = false, anyAmount = false, anyRate = false, anyDepth = false;
        uint64_t phaseSources = 0;              // the sources (and vias) of the slots aimed at a phase
        Mask trackUsed;                         // the track's effect cells (TrackDest) a slot is aimed at
        std::array<uint8_t, NUM_SLOTS> trackList {};    // ... one entry a slot
        int numTrack = 0;
        uint32_t generation = 0;
    };

    static int index(float value, int count)
    {
        return std::clamp((int) std::lround(std::clamp(value, -1.0e6f, 1.0e6f)), 0, std::max(1, count) - 1);
    }

    void updateInputFilters()
    {
        lowK = 1.0f - std::exp(-6.2831853f * std::min(lowHz, 0.45f * (float) sampleRate) / (float) sampleRate);
        highK = 1.0f - std::exp(-6.2831853f * std::min(highHz, 0.45f * (float) sampleRate) / (float) sampleRate);
    }

    // A cell that decides the slots was set: they are resolved again at the next beginBlock().
    void changed() { edits.fetch_add(1, std::memory_order_release); }

    // The track input for the n samples from `offset`: does a sound begin in them (and ONST, which
    // is 1 then and falls), and the sixteen band levels. What is no longer read is put away once.
    // (Out of line: an engine nobody listens through pays one test a sub-block.)
   #if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
   #endif
    void hear(int offset, int n)
    {
        onsetNow = false;
        if (state.onset || onsetsWanted.load(std::memory_order_relaxed))
        {
            bool begun = false;
            for (int i = 0; i < n; ++i)
            {
                const int at = offset + i;
                const float l = at < frames ? inL[at] : 0.0f, r = at < frames ? inR[at] : 0.0f;
                begun = onsets.onset.hears(onsets.detector.level(l, r)) || begun;
            }
            onsetValue *= std::exp(-(float) n * onsetRate);
            if (onsetValue < 1.0e-4f)
                onsetValue = 0.0f;
            if (begun)
            {
                onsetNow = true;
                onsetValue = 1.0f;
            }
            onsetsRan = true;
        }
        else if (onsetsRan)
        {
            onsets.reset();
            onsetValue = 0.0f;
            onsetsRan = false;
        }
        if (state.bands)
        {
            for (int i = 0; i < n; ++i)
            {
                const int at = offset + i;
                const float x = at < frames ? 0.5f * (inL[at] + inR[at]) : 0.0f;
                // (As the vocoder takes its modulator: a sample under -200 dB, or no number, is none.)
                analysis.tick(std::abs(x) >= 1.0e-10f && std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f);
            }
            if ((++bandLooks & 3u) == 0)
                analysis.rest();
            for (int b = 0; b < Src::NUM_BANDS; ++b)
                bandNow[(size_t) b] = std::min(analysis.level(b), 1.0f);
            bandsRan = true;
        }
        else if (bandsRan)
        {
            analysis.clear(true);
            bandNow.fill(0.0f);
            bandsRan = false;
        }
        hearing = onsetsRan || bandsRan;
    }

    // The slots as the voices run them, from the cells as they stand. On the audio thread only
    // (beginBlock), so no voice ever reads them half made. Every cell is read once and every index
    // made valid here, whatever another thread is setting meanwhile.
    void resolve()
    {
        State& s = state;
        if (s.audioSources != 0)
            audioTail = tailLength;         // (a voice may still be fading what a slot left)
        if (s.published != 0)
        {
            publishedKept |= s.published;   // (the same for the engine's own: its voices go on handing them over)
            publishedTail = tailLength;
        }
        const Mask trackBefore = s.trackUsed;
        s = State();
        s.generation = ++generation;
        const DestTable& t = *table;
        const int visible = t.visible();

        std::array<Active, NUM_SLOTS> found {};
        std::array<uint8_t, NUM_SLOTS> gates {};
        int numFound = 0;
        bool modulated[NUM_FREE] = {};
        for (int k = 0; k < NUM_FREE; ++k)
        {
            const int other = freeSlots[(size_t) k].dest - visible - 2 * lfoCount;
            if (sourceList->source(freeSlots[(size_t) k].src) != Src::Off && other >= 0 && other < NUM_FREE && other != k)
                modulated[other] = true;
        }

        // destCell: an index of the table, or beyond its visible part (free slots only).
        auto add = [&](int slot, int src, int via, int destCell, float amount, int gate, bool linear, bool fromStrip, int ownLfo)
        {
            Active a;
            a.slot = (uint8_t) slot;
            a.src = (uint8_t) src;
            a.via = (uint8_t) via;
            a.amount = amount;
            a.ownLfo = (int8_t) ownLfo;
            const bool isModulated = slot < NUM_FREE && modulated[slot];
            if (src == Src::Off || destCell <= 0 || (amount == 0.0f && ! isModulated))
                return;
            if (isModulated)
                a.flags |= Modulated;
            // A cell of an effect on the track: behind everything else in the slot's list (the
            // pressure slots are handed table indices, hidden ones among them: never these).
            if (const int first = slot < NUM_FREE ? t.trackSlotChoice() : t.trackCell(); slot < PRES0 && destCell >= first)
            {
                if (destCell - first >= TrackDest::COUNT)
                    return;
                a.kind = ToTrack;
                a.dest = (uint8_t) (destCell - first);
                gates[(size_t) numFound] = (uint8_t) gate;
                found[(size_t) numFound++] = a;
                return;
            }
            if (destCell < t.size() && (destCell < visible || slot >= NUM_FREE))
            {
                const Dest& d = t[destCell];
                if (d.cross != 0)
                {
                    if (d.lfo >= lfoCount)
                        return;
                    a.kind = d.cross == 1 ? ToRate : ToDepth;
                    a.target = d.lfo;
                }
                else
                {
                    a.kind = ToDest;
                    a.dest = (uint8_t) destCell;
                    if (d.squared && ! linear)  a.flags |= SquaredAmount;
                    if (d.steps)                a.flags |= Raw;
                    if (d.perSample && audioRate(src)) a.flags |= Audio;
                    if (fromStrip)              a.scale = d.stripScale;
                }
            }
            else
            {
                const int extra = destCell - visible;
                if (extra < 2 * lfoCount)
                {
                    a.kind = extra % 2 == 0 ? ToRate : ToDepth;
                    a.target = (int8_t) (extra / 2);
                    a.ownLfo = -1;
                }
                else if (extra - 2 * lfoCount < NUM_FREE && extra - 2 * lfoCount != slot)
                {
                    a.kind = ToAmount;
                    a.target = (int8_t) (extra - 2 * lfoCount);
                }
                else
                    return;
            }
            gates[(size_t) numFound] = (uint8_t) gate;
            found[(size_t) numFound++] = a;
        };

        // (A free slot's SRC and VIA are places in the engine's list.)
        for (int k = 0; k < NUM_FREE; ++k)
            add(FREE0 + k, sourceList->source(freeSlots[(size_t) k].src), sourceList->source(freeSlots[(size_t) k].via),
                freeSlots[(size_t) k].dest, freeSlots[(size_t) k].amount, Always, false, false, -1);
        for (int i = 0; i < lfoCount; ++i)
            add(LFO0 + i, Src::ofLfo(i), Src::Off, lfoCells[(size_t) i].dest, lfoCells[(size_t) i].depth, Always, false, false, i);
        for (int r = 0; r < NUM_STRIP; ++r)
            add(STRIP0 + r, Src::Strip, Src::Off, stripDest[(size_t) r], stripAmount[(size_t) r], StripGate, false, true, -1);
        if (t.pitch() > 0)
            add(VIBRATO, Src::Vib, Src::Strip, t.pitch(), vibratoDepth / (12.0f * t[t.pitch()].span), StripGate, true, false, -1);
        for (int x = 0; x < NUM_XIN; ++x)
            add(XIN0 + x, Src::Xin + xinFilter[(size_t) x], Src::Off, xinDest[(size_t) x], xinAmount[(size_t) x], Always, false, false, -1);
        for (int p = 0; p < NUM_PRESSURE; ++p)
        {
            // (Every cell read once: another thread may be setting them.)
            const PressureSlotCells c = pressureSlots[(size_t) p];
            const int reading = c.bipolar ? (int) Src::PressureBipolar : (int) Src::Pressure;
            if (c.dest <= 0 || c.amount == 0.0f)
                continue;
            if (p < ownSlots && (c.dest < t.size() ? ownDests.test(c.dest) : (c.dest == t.pressureVibrato() && ownVibrato)))
                continue;       // the engine plays this slot on its own path (setOwnPressure)
            if (c.dest < t.size())
                add(PRES0 + p, reading, Src::Off, c.dest, c.amount, PressureGate, true, false, -1);
            else if (c.dest == t.pressureVibrato())
            {
                // VIB: the vibrato sine through the pad on the pitch, +-amount semitones at full pressure.
                if (t.pitch() > 0)
                    add(PRES0 + p, Src::Vib, reading, t.pitch(), c.amount / (12.0f * t[t.pitch()].span), PressureGate, true, false, -1);
            }
            else if (const int d = c.dest - t.pressureTrack(); d < TrackDest::COUNT && numFound < NUM_SLOTS)
            {
                // A cell of an effect on the track: summed by the voice that leads (its pad).
                Active a;
                a.slot = (uint8_t) (PRES0 + p);
                a.src = (uint8_t) reading;
                a.amount = c.amount;
                a.kind = ToTrack;
                a.dest = (uint8_t) d;
                gates[(size_t) numFound] = (uint8_t) PressureGate;
                found[(size_t) numFound++] = a;
            }
        }

        for (int gate = 0; gate < NUM_GATES; ++gate)
        {
            for (int i = 0; i < numFound; ++i)
            {
                if (gates[(size_t) i] != gate)
                    continue;
                const Active& a = found[(size_t) i];
                s.active[(size_t) s.count++] = a;
                for (int source : { (int) a.src, (int) a.via })
                {
                    s.sources |= Src::bit(source);
                    if (const int l = Src::lfoIndex(source); l >= 0)
                        s.lfoMask |= (uint8_t) (1u << l);
                    if (Src::isAudio(source))
                        s.audioSources |= (uint8_t) (1u << (source - Src::Xin));
                    if (Src::isPublished(source))
                        s.published |= (uint16_t) (1u << (source - Src::Published1));
                    s.usesVibrato = s.usesVibrato || source == Src::Vib;
                }
                if (a.kind == ToDest)
                {
                    if (t[a.dest].cycle > 0.0f)
                        s.phaseSources |= Src::bit(a.src) | Src::bit(a.via);
                    s.used[gate].set(a.dest);
                    if (a.flags & Audio)
                    {
                        s.audioDests.set(a.dest);
                        ++s.numAudio;
                    }
                }
                if (a.kind == ToTrack)
                {
                    s.trackUsed.set(a.dest);
                    s.trackList[(size_t) s.numTrack++] = a.dest;
                }
                s.anyAmount = s.anyAmount || a.kind == ToAmount;
                s.anyRate = s.anyRate || a.kind == ToRate;
                s.anyDepth = s.anyDepth || a.kind == ToDepth;
            }
            s.end[gate] = s.count;
        }
        for (int i = 0; i < lfoCount; ++i)
            if (triggeredByInput(lfoCells[(size_t) i].trigger) && (s.lfoMask & (1u << i)))
                s.onsetLfos |= (uint8_t) (1u << i);
        s.onset = (s.sources & Src::bit(Src::Onset)) != 0 || s.onsetLfos != 0;
        s.bands = (s.sources & Src::BAND_BITS) != 0;
        s.hears = s.onset || s.bands;
        // A cell of the track's effects that comes in starts from nothing.
        s.trackUsed.without(trackBefore).forEach([this](int d) { trackValue[(size_t) d] = 0.0f; });
    }

    // An audio-rate source: the track input's four, and what the engine publishes as samples.
    bool audioRate(int source) const
    {
        return Src::isAudio(source) || (Src::isPublished(source) && sourceList->audio(source - Src::Published1));
    }
    // The samples of an audio-rate source for the sub-block (the engine's own: of the voice that
    // is being worked on, Voice::takePublished).
    const float* audioOf(int source) const
    {
        return Src::isPublished(source) ? publishedAudio[(size_t) (source - Src::Published1)].data()
                                        : audio[(size_t) (source - Src::Xin)].data();
    }

    float* bufferOf(int dest) const { return scratch.data() + (size_t) ((*table)[dest].buffer * MAX_CHUNK); }

    const DestTable* table;
    int lfoCount;
    const SourceTable* sourceList;
    int sourceCount;
    uint64_t bipolarSources = 0;            // the sources that swing both ways (the LFOs say by their shape)

    // The cells
    std::array<LfoCells, MAX_LFOS> lfoCells {};
    std::array<FreeCells, NUM_FREE> freeSlots {};
    std::array<int, NUM_STRIP> stripDest {};
    std::array<float, NUM_STRIP> stripAmount {};
    float vibratoDepth = 0.0f, vibratoRate = DEFAULT_VIBRATO_RATE;
    std::array<int, NUM_XIN> xinDest {}, xinFilter {};
    std::array<float, NUM_XIN> xinAmount {};
    float lowHz = 2000.0f, highHz = 200.0f;
    std::array<PressureSlotCells, NUM_PRESSURE> pressureSlots {};
    Mask ownDests;                      // the destinations the engine plays from pressure by itself (setOwnPressure)
    bool ownVibrato = false;
    int ownSlots = 0;
    std::array<float, LFO::USER_POINTS> levels {};
    int shown = DEFAULT_SHOWN;
    float tempo = 120.0f;
    std::array<float, DestTable::MAX> base {};
    std::array<uint32_t, MAX_LFOS> lfoGen {}, lfoRestart {};

    // The resolved slots (the audio thread's)
    State state;
    std::atomic<uint32_t> edits { 0 };      // counts the changes of the cells behind them
    uint32_t resolvedAt = 0, generation = 0;

    // The block
    double sampleRate = 44100.0;
    int chunk = 16;
    float slewStep = 0.01f, blockStep = 0.005f;
    float phaseStep = 0.0026f;              // a phase's carry: half a period in 4 ms (per sample, in periods)
    const float* inL = nullptr;
    const float* inR = nullptr;
    int frames = 0;
    int audioTail = 0, tailLength = 256;    // the input is kept running this long after its last slot went
    float follower = 0.0f, strip = 0.0f;
    double vibratoPhase = 0.0;
    float vibratoNow = 0.0f;
    float lowK = 0.2f, highK = 0.02f, lowState = 0.0f, highState = 0.0f, bandState = 0.0f;
    std::array<std::array<float, MAX_CHUNK>, Src::NUM_AUDIO> audio {};
    // The input's fade where it ends or comes back (beginChunk), and who reads it
    float inGain = 1.0f, inTail = 0.0f, inHeld = 0.0f, fadeStep = 0.005f;
    bool inputRead = false;

    // The track input's onsets and band levels (hear())
    dyn::Trigger onsets;
    voc::Analysis analysis;
    std::array<float, Src::NUM_BANDS> bandNow {};
    float onsetValue = 0.0f, onsetRate = 0.0002f;
    bool onsetNow = false, onsetsRan = false, bandsRan = false, hearing = false;
    unsigned bandLooks = 0;
    std::atomic<bool> onsetsWanted { false };       // the engine itself listens (needOnsets)

    // The engine's own sources: those kept wanted for a while after their last slot went
    uint32_t publishedKept = 0;
    int publishedTail = 0;
    mutable std::array<std::array<float, MAX_CHUNK>, Src::MAX_PUBLISHED> publishedAudio {};    // of the voice being worked on

    // Scratch the voices fill, one after the other
    mutable std::vector<float> scratch;
    mutable std::array<float, DestTable::MAX> sum {};
    mutable std::array<float, NUM_SLOTS> gain {};
    struct ControlValue { int dest; float value; };
    mutable std::array<ControlValue, DestTable::MAX> control {};
    mutable int numControl = 0;

    // The track's effect cells: the leader's sums and what they come to; who leads (compared,
    // never read through), who asked to in this sub-block, whether anybody led it
    mutable std::array<float, TrackDest::COUNT> trackSum {}, trackValue {};
    mutable const Voice* leader = nullptr;
    mutable const Voice* claim = nullptr;
    mutable uint32_t claimSerial = 0;
    mutable bool led = false;
    mutable std::atomic<uint32_t> notes { 0 };      // counts the note-ons: a voice's is its serial
};

// One voice's side of the router.
class Voice
{
public:
    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        for (auto& l : lfos)
            l.setSampleRate(sampleRate);
    }

    // This voice's own set values, indexed like the table (at least DestTable::size() of them,
    // staying where they are), instead of the router's: for an engine whose voices are not copies
    // of one sound (a drum kit: one router for the kit, each drum its own cells). nullptr: the router's.
    void useBase(const float* values) { own = values; }
    // A further envelope of the voice as it stands (0..1), before tick(): the source ENV2 + which.
    void setEnvelope(int which, float level)
    {
        if (which >= 0 && which < Src::MAX_ENVELOPES)
            target[(size_t) (FirstEnvelope + which)] = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
    }

    // An engine's own source k (SourceTable::publish, in the order published), at control rate:
    // its value as it stands, -1 .. 1, before tick(). Led like an envelope (4 ms); a note-on takes
    // it as it stands. Ask Router::wants(k) first: a source nobody reads need not be worked out.
    void publish(int k, float value)
    {
        if (k >= 0 && k < Src::MAX_PUBLISHED)
            publishedValue[(size_t) k] = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    }
    // An engine's own source k at audio rate (held to -1 .. 1 when read). nullptr or 0: silence
    // from now on (the source is taken back: call it once when Router::wants(k) turns false).
    //  - What the voice MAKES (an operator's output): the `count` samples it makes in the
    //    sub-block it renders, handed over after tick() - before rendering, the buffer filled as
    //    it renders, or after rendering. The samples must stay where they are until the next
    //    tick(), which keeps them: the voice keeps the last Router::MAX_CHUNK it made, and the
    //    slots hear sample i of a sub-block as what was made exactly Router::chunkSize() samples
    //    before it - one whole sub-block late, 16 samples at 48 kHz, whatever the host's block
    //    size (a host block that is not a whole number of sub-blocks ends on a short one: the
    //    delay does not change with it). For a steady tone that is a turn of its phase. A source
    //    that begins to be handed over, and every note-on, begins from silence.
    //  - A source published with mod::Immediate (samples that exist before the voice renders: the
    //    track input as the engine filters it): before tick(), the n samples of the sub-block
    //    itself, heard with no delay. They must stay where they are until the next call.
    void publishAudio(int k, const float* samples, int count)
    {
        if (k < 0 || k >= Src::MAX_PUBLISHED)
            return;
        const auto at = (size_t) k;
        publishedSamples[at] = count > 0 ? samples : nullptr;
        publishedCount[at] = samples != nullptr ? std::max(0, count) : 0;
        if (publishedCount[at] > 0)
            handed |= 1u << k;
        else
        {
            handed &= ~(1u << k);
            made &= ~(1u << k);
        }
    }
    // True while the voice has samples of source k handed over (a test's question).
    bool publishing(int k) const { return k >= 0 && k < Src::MAX_PUBLISHED && (publishedCount[(size_t) k] > 0 || ((made >> k) & 1u)); }

    // The per-note randoms (RAND, RND2 .. RND4) are draws of a spread::Generator: the voice's own
    // (VoiceSpread.h) where the engine hands it to noteOn, else this one, which the engine seeds
    // once with its seed and the voice's number so that a render repeats. (Never seeded, every
    // voice still draws its own sequence, but not the same one from run to run.)
    void seed(uint32_t engineSeed, int voice) { random.seed(engineSeed, voice); }

    // The note that starts with the next tick(): the LFOs are triggered (those that wait for the
    // track input are not), the randoms are drawn, and everything is taken as it stands at the
    // note's first sample.
    void noteOn(const Router& r, int note, float velocity)
    {
        random.advance();
        noteOn(r, note, velocity, random);
    }
    // ... with the randoms taken from the voice's spread generator, as it stands for this note
    // (after VoiceSpread::next): SEED then repeats them with the spread.
    void noteOn(const Router& r, int note, float velocity, const spread::Generator& generator)
    {
        for (int i = 0; i < r.lfoCount; ++i)
        {
            syncLfo(r, i);
            lfos[(size_t) i].setKeyNote((float) note);
            if (! Router::triggeredByInput(r.lfoCells[(size_t) i].trigger))
                lfos[(size_t) i].trigger();
        }
        raw[Src::Velocity] = src[Src::Velocity] = std::clamp(velocity, 0.0f, 1.0f);
        raw[Src::Key] = src[Src::Key] = std::clamp((float) (note - 60) / 60.0f, -1.0f, 1.0f);
        for (int k = 0; k < Src::NUM_RANDOM; ++k)
            raw[(size_t) Src::ofRandom(k)] = src[(size_t) Src::ofRandom(k)] = generator.bipolar(spread::Free, k);
        publishedCount.fill(0);         // (what the last note made is not this note's)
        made = handed = 0;
        fresh = true;
        serial = r.notes.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    // One sub-block of n samples for a voice that sounds. ampEnv / filterEnv: the envelopes as
    // they stand (0..1); pressure: this voice's pad. False: nothing moves this voice.
    bool tick(const Router& r, int n, float ampEnv, float filterEnv, float padPressure)
    {
        const Router::State& s = r.state;
        router = &r;
        padPressure = std::isfinite(padPressure) ? std::clamp(padPressure, 0.0f, 1.0f) : 0.0f;
        const bool pressed = s.end[Router::PressureGate] > s.end[Router::Always]
                             && (padPressure > 0.0f || src[Src::Pressure] != 0.0f || src[Src::PressureBipolar] != 0.0f);
        const bool stripUp = s.end[Router::StripGate] > s.end[Router::PressureGate] && (r.strip > 0.0f || src[Src::Strip] != 0.0f);
        const bool structural = seen != s.generation;
        // (A strip route on a cell of the track's effects is closed with the strip down like any
        // other: the last thing the leader gave the cell was the strip at 0, which is nothing.)
        if (s.end[Router::Always] == 0 && ! pressed && ! stripUp && ! structural && ! busy)
        {
            fresh = false;
            readBefore = 0;             // (no source is moved meanwhile)
            return false;
        }
        return work(r, s, n, ampEnv, filterEnv, padPressure, pressed, stripUp, structural);
    }

private:
    // tick() for a voice that something moves. (Kept out of line: the few compares above are all
    // a voice at rest pays, and the engine's render loop around them stays as small as it was -
    // inlined here, this body cost the macro oscillator's plain path a tenth of its speed.)
   #if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
   #endif
    bool work(const Router& r, const Router::State& s, int n, float ampEnv, float filterEnv, float padPressure,
              bool pressed, bool stripUp, bool structural)
    {
        seen = s.generation;
        leading = s.numTrack != 0 && leads(r, true);
        ledAtRest = false;
        const float* base = baseOf(r);
        // A sound has begun in the track input: the LFOs that wait for one begin again.
        if (r.onsetNow && s.onsetLfos != 0)
            for (int i = 0; i < r.lfoCount; ++i)
                if (s.onsetLfos & (1u << i))
                {
                    syncLfo(r, i);
                    lfos[(size_t) i].trigger();
                }
        if ((s.published | r.publishedKept) != 0)
            takePublished(r, s, n);
        if (fresh)
        {
            dropFading();                   // (a note-on takes everything as it stands:
            heard.fill(0.0f);               // nothing of the last note's audio-rate slots is left)
        }
        else if (structural)
            retireAudio(s);

        Mask mask = s.used[Router::Always] | carrying | fadingDests;
        if (pressed) mask = mask | s.used[Router::PressureGate];
        if (stripUp) mask = mask | s.used[Router::StripGate];
        if (structural && ! fresh)
            mask = mask | now;          // what a slot has just left is carried off, not dropped
        // A destination that comes in starts from its set value.
        mask.without(now).forEach([&](int d)
        {
            lastSum[(size_t) d] = 0.0f;
            carry[(size_t) d] = 0.0f;
            lastValue[(size_t) d] = base[(size_t) d];
        });
        // A source that a slot on a phase begins to read starts where it stands, not from where
        // it was last read (its lead would run the phase off at the speed of the lead).
        const uint64_t starting = structural && ! fresh ? s.phaseSources & ~readBefore : 0;
        readBefore = s.sources;
        if (structural)
        {
            if (! s.anyAmount)  amountMod.fill(0.0f);
            if (! s.anyRate)    for (int i = 0; i < r.lfoCount; ++i) { rateMod[(size_t) i] = 0.0f; lfos[(size_t) i].setRateMod(0.0f); }
            if (! s.anyDepth)   depthMod.fill(0.0f);
        }

        target[0] = std::isfinite(ampEnv) ? std::clamp(ampEnv, 0.0f, 1.0f) : 0.0f;
        target[1] = std::isfinite(filterEnv) ? std::clamp(filterEnv, 0.0f, 1.0f) : 0.0f;
        target[2] = padPressure;
        target[3] = padPressure > 0.0f ? 2.0f * padPressure - 1.0f : 0.0f;     // (a pad at rest is no pressure at all)

        if (fresh)
        {
            run(r, s, base, mask, pressed, stripUp, 0, 1, true, false, 0, true);
            if (n > 1)
                run(r, s, base, mask, pressed, stripUp, 1, n - 1, false, false, 0, true);
            fresh = false;
        }
        else
            run(r, s, base, mask, pressed, stripUp, 0, n, false, structural, starting, true);

        // The control-rate destinations that moved, and those let go (their set value, once).
        r.numControl = 0;
        mask.forEach([&](int d)
        {
            if (! r.dests()[d].perSample)
            {
                r.control[(size_t) r.numControl++] = { d, lastValue[(size_t) d] };
                // (At its set value it is not held: nothing to give back later.)
                if (lastValue[(size_t) d] != base[(size_t) d]) pushed.set(d); else pushed.clear(d);
            }
        });
        pushed.without(mask).forEach([&](int d)
        {
            r.control[(size_t) r.numControl++] = { d, base[(size_t) d] };
            pushed.clear(d);
        });
        now = mask;
        busy = mask.any() || pushed.any();
        return mask.any() || r.numControl > 0;
    }

public:
    // After tick(): fn(dest, value) for each control-rate destination to set on the voice.
    template <typename Fn>
    void forEachControl(Fn&& fn) const
    {
        for (int i = 0; router != nullptr && i < router->numControl; ++i)
            fn(router->control[(size_t) i].dest, router->control[(size_t) i].value);
    }

    // After tick(): the per-sample values of a PerSample destination for the sub-block, or
    // nullptr when nothing moves it (the voice reads its set value).
    const float* buffer(int dest) const { return now.test(dest) && router->dests()[dest].buffer >= 0 ? router->bufferOf(dest) : nullptr; }
    bool moved(int dest) const { return now.test(dest); }
    // A control-rate destination as of the last tick (meaningful while moved(dest)).
    float value(int dest) const { return lastValue[(size_t) dest]; }

    // A sub-block of n samples for a voice that does not sound: its LFOs keep time with the
    // others'. True: it still holds control-rate cells off their set values: restore() them.
    bool idle(const Router& r, int n)
    {
        const Router::State& s = r.state;
        // The voice that leads the track's effect cells goes on doing so at rest: its sources and
        // the slots are run as for a voice that sounds (its LFOs by run(), not here).
        leading = s.numTrack != 0 && leads(r, false);
        if (s.lfoMask != 0)
            for (int i = 0; i < r.lfoCount; ++i)
                if (s.lfoMask & (1u << i))
                {
                    syncLfo(r, i);
                    if (r.onsetNow && (s.onsetLfos & (1u << i)))
                        lfos[(size_t) i].trigger();     // (in step with the voices that sound)
                    if (! leading)
                        lfos[(size_t) i].advanceIdle(n);
                }
        const bool letGo = busy;
        if (busy)
        {
            // Nobody is listening: everything is let go of at once.
            carrying = Mask();
            now = Mask();
            for (int k : { (int) Src::Env, (int) Src::FilterEnv, (int) Src::Pressure, (int) Src::Strip, (int) Src::Input, (int) Src::PressureBipolar,
                           (int) Src::Env2, (int) Src::Env3, (int) Src::Env4, (int) Src::Env5 })
                src[(size_t) k] = raw[(size_t) k] = 0.0f;
            for (int k = Src::Onset; k <= Src::PublishedLast; ++k)
                src[(size_t) k] = raw[(size_t) k] = 0.0f;
            heard.fill(0.0f);
            dropFading();
            busy = pushed.any();
        }
        // (Its sources are taken as they stand in the first sub-block it leads at rest: what it
        // last read of the strip or the input may be seconds old, or was put away just now.)
        if (leading)
            leadAtRest(r, s, n, letGo || ! ledAtRest);
        ledAtRest = leading;
        return busy;
    }
    template <typename Fn>
    void restore(const Router& r, Fn&& fn)
    {
        const float* base = baseOf(r);
        pushed.forEach([&](int d) { fn(d, base[(size_t) d]); });
        pushed = Mask();
        busy = false;
    }

    LFO& getLfo(int i) { return lfos[(size_t) std::clamp(i, 0, Router::MAX_LFOS - 1)]; }
    float source(int s) const { return src[(size_t) std::clamp(s, 0, (int) Src::TOTAL - 1)]; }
    // A source as a VIA (0..1) and a destination's law, by the router's own rules: for what still
    // evaluates a slot of its own beside the router (the drum kit's per-pad LFO and slots, until
    // they are router slots), so that it cannot disagree with it.
    float viaOf(const Router& r, int s) const { return via(r, std::clamp(s, 0, (int) Src::TOTAL - 1)); }
    static float lawOf(const Dest& d, float base, float sum) { return law(d, base, sum); }

private:
    enum { FirstEnvelope = 4 };     // target[]: amp, filter, pressure, pressure both ways, ENV2 ..

    const float* baseOf(const Router& r) const { return own != nullptr ? own : r.base.data(); }

    // The track's effect cells are moved by one voice (Router: THE TRACK'S EFFECTS). A voice that
    // sounds asks for the lead of the next sub-block (the newest note gets it); true: this voice
    // leads this one.
    bool leads(const Router& r, bool sounding) const
    {
        if (sounding && serial >= r.claimSerial)
        {
            r.claim = this;
            r.claimSerial = serial;
        }
        if (r.leader == nullptr)
            r.leader = this;
        return r.leader == this;
    }

    // idle() for the voice that leads: no envelope, no pad; the rest as it stands.
   #if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
   #endif
    void leadAtRest(const Router& r, const Router::State& s, int n, bool first)
    {
        router = &r;
        target.fill(0.0f);
        const bool stripUp = s.end[Router::StripGate] > s.end[Router::PressureGate] && (r.strip > 0.0f || src[Src::Strip] != 0.0f);
        run(r, s, baseOf(r), Mask(), false, stripUp, 0, n, false, false, first ? ~uint64_t (0) : 0, false);
    }

    // An audio-rate slot that was heard and is now gone, re-aimed or reading another source: what
    // it gave is faded where it was (a slot's gain is never dropped, any more than a sum is).
    void retireAudio(const Router::State& s)
    {
        std::array<Heard, Router::NUM_SLOTS> next {};
        for (int a = 0; a < s.end[Router::Always]; ++a)
            if (const auto& slot = s.active[(size_t) a]; slot.flags & Router::Audio)
                next[slot.slot] = { slot.src, slot.dest };
        for (size_t k = 0; k < next.size(); ++k)
        {
            if (heard[k] != 0.0f && (next[k].src != route[k].src || next[k].dest != route[k].dest))
            {
                fadingGain[k] = heard[k];
                fadingRoute[k] = route[k];
                fading |= 1u << k;
                fadingDests.set(route[k].dest);
                heard[k] = 0.0f;
            }
            route[k] = next[k];
        }
    }
    void dropFading()
    {
        fading = 0;
        fadingDests = Mask();
    }

    // This voice's audio-rate sources of its own, for the n samples of the sub-block: what it
    // handed over after its last one, into the router's scratch (held to -1 .. 1; silence where
    // it handed nothing over).
    void takePublished(const Router& r, const Router::State& s, int n)
    {
        for (uint32_t bits = ((uint32_t) s.published | r.publishedKept) & r.sourceList->audioPublished(); bits != 0; bits &= bits - 1)
        {
            const auto k = (size_t) __builtin_ctz(bits);
            float* to = r.publishedAudio[k].data();
            if (! r.sourceList->immediate((int) k))
            {
                // What the voice made, exactly one whole sub-block ago (publishAudio): what it
                // handed over since the last tick goes into its ring first.
                auto& ring = madeRing[k];
                if ((handed >> k) & 1u)
                {
                    if (! ((made >> k) & 1u))
                    {
                        ring.fill(0.0f);
                        madeAt[k] = 0;
                        made |= 1u << k;
                    }
                    const float* from = publishedSamples[k];
                    const int have = std::min(publishedCount[k], (int) Router::MAX_CHUNK);
                    for (int i = 0; i < have; ++i)
                        ring[(size_t) ((madeAt[k] + i) & MADE_MASK)] = from[i];
                    madeAt[k] = (madeAt[k] + have) & MADE_MASK;
                    handed &= ~(1u << k);
                }
                if (! ((made >> k) & 1u))
                {
                    std::fill(to, to + n, 0.0f);
                    continue;
                }
                const int from = madeAt[k] - std::max(r.chunk, n);
                for (int i = 0; i < n; ++i)
                {
                    const float x = ring[(size_t) ((from + i) & MADE_MASK)];
                    to[i] = std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f;
                }
                continue;
            }
            const float* from = publishedSamples[k];
            const int have = from != nullptr ? std::min(publishedCount[k], (int) Router::MAX_CHUNK) : 0;
            if (have <= 0)
            {
                std::fill(to, to + n, 0.0f);
                continue;
            }
            for (int i = 0; i < n; ++i)
            {
                const float x = from[i < have ? i : have - 1];
                to[i] = std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f;
            }
        }
    }

    void syncLfo(const Router& r, int i)
    {
        const auto n = (size_t) i;
        auto& l = lfos[n];
        bool begun = false;
        if (lfoRestart[n] != r.lfoRestart[n])
        {
            lfoRestart[n] = r.lfoRestart[n];
            l = LFO();
            l.setSampleRate(sampleRate);
            lfoGen[n] = r.lfoGen[n] - 1;
            begun = true;
        }
        if (lfoGen[n] == r.lfoGen[n] && lfoSet[n])
            return;
        lfoGen[n] = r.lfoGen[n];
        lfoSet[n] = true;
        const auto& c = r.lfoCells[n];
        l.setUserShape(r.levels.data());
        l.setDepth(1.0f);
        l.setRate(c.rate);
        l.setShape((LFO::Shape) c.shape);
        const auto mode = (LFO::TriggerMode) Router::lfoMode(c.trigger);
        l.setTriggerMode(mode);
        l.setTempo(r.tempo);
        l.setSyncDivision(c.division);
        l.setTempoSync(c.sync);
        l.setFadeTime(c.fade);
        l.setStartPhase(c.phase);
        l.setDelayTime(c.delay);
        l.setKeyTrack(c.keyTrack);
        lfoUnipolar[n] = LFO::isUnipolar((LFO::Shape) c.shape);
        // A one-shot that waits for the track input rests at the END of its cycle until the first
        // hit (where every later one leaves it), not somewhere in a cycle nothing began.
        if (begun && Router::triggeredByInput(c.trigger) && (mode == LFO::TriggerMode::Single || mode == LFO::TriggerMode::Half))
        {
            l.trigger();
            l.advanceIdle(1 << 30);
        }
    }

    // A source as a VIA: 0..1.
    float via(int source) const { return via(*router, source); }
    float via(const Router& r, int source) const
    {
        const float v = src[(size_t) source];
        const int l = Src::lfoIndex(source);
        const bool bipolar = l >= 0 ? ! lfoUnipolar[(size_t) l] : (r.bipolarSources & Src::bit(source)) != 0;
        return bipolar ? 0.5f * (v + 1.0f) : v;
    }

    static float law(const Dest& d, float base, float sum)
    {
        const float v = d.exp ? base * std::exp2(std::clamp(sum * d.span, -24.0f, 24.0f)) : base + sum * d.span;
        return v < d.hi ? (v > d.lo ? v : d.lo) : d.hi;     // (not a number: held)
    }

    // `count` samples from `i0` of the sub-block. snap: everything as it stands (a note's first sample).
    // starting: the sources that begin to be read by a slot on a phase destination.
    // sounding: false for the leader of the track's effect cells at rest (leadAtRest): the sources
    // and the slots only, none of the voice's own destinations.
    void run(const Router& r, const Router::State& s, const float* base, const Mask& mask, bool pressed, bool stripUp,
             int i0, int count, bool snap, bool structural, uint64_t starting, bool sounding)
    {
        const float fast = r.slewStep * (float) count, slow = r.blockStep * (float) count;
        auto move = [&](int k, float to, float step)
        {
            raw[(size_t) k] = to;
            src[(size_t) k] = snap || ((starting >> k) & 1u) ? to : dsp::towards(src[(size_t) k], to, step);
        };

        // ---- The sources -----------------------------------------------------------------------
        if (s.lfoMask != 0)
            for (int i = 0; i < r.lfoCount; ++i)
                if (s.lfoMask & (1u << i))
                {
                    auto& l = lfos[(size_t) i];
                    syncLfo(r, i);
                    if (s.anyRate)
                        l.setRateMod(rateMod[(size_t) i]);
                    l.processBlock(count);
                    move(Src::ofLfo(i), l.getShapeOutput(), fast);
                }
        // (Only what a slot reads: a voice with one route pays for one source.)
        const uint64_t read = s.sources;
        if (read & Src::bit(Src::Env))             move(Src::Env, target[0], fast);
        if (read & Src::bit(Src::FilterEnv))       move(Src::FilterEnv, target[1], fast);
        if (read & Src::bit(Src::Pressure))        move(Src::Pressure, target[2], slow);
        if (read & Src::bit(Src::PressureBipolar)) move(Src::PressureBipolar, target[3], 2.0f * slow);
        if (read & Src::bit(Src::Strip))           move(Src::Strip, r.strip, slow);
        if (read & Src::bit(Src::Input))           move(Src::Input, r.follower, slow);
        if (read >> Src::Env2)
        {
            for (int k = 0; k < Src::MAX_ENVELOPES; ++k)
                if (read & Src::bit(Src::Env2 + k))
                    move(Src::Env2 + k, target[(size_t) (FirstEnvelope + k)], fast);
            // The track input's onsets and bands (the router's, the same for every voice) ...
            if (read & Src::bit(Src::Onset))
                move(Src::Onset, r.onsetValue, fast);
            if (read & Src::BAND_BITS)
                for (int b = 0; b < Src::NUM_BANDS; ++b)
                    if (read & Src::bit(Src::Band1 + b))
                        move(Src::Band1 + b, r.bandNow[(size_t) b], fast);
            // ... and the engine's own: a value led like an envelope, or the last of its samples.
            for (uint32_t bits = s.published; bits != 0; bits &= bits - 1)
            {
                const int k = __builtin_ctz(bits), id = Src::Published1 + k;
                if (r.sourceList->audio(k))     // (a voice at rest makes none: the scratch holds another voice's)
                    raw[(size_t) id] = src[(size_t) id] = sounding ? r.publishedAudio[(size_t) k][(size_t) (i0 + count - 1)] : 0.0f;
                else
                    move(id, publishedValue[(size_t) k], fast);
            }
        }
        raw[Src::Vib] = src[Src::Vib] = r.vibratoNow;
        raw[Src::One] = src[Src::One] = 1.0f;
        if (s.audioSources != 0)
            for (int k = 0; k < Src::NUM_AUDIO; ++k)
                raw[(size_t) (Src::Xin + k)] = src[(size_t) (Src::Xin + k)] = r.audio[(size_t) k][(size_t) (i0 + count - 1)];

        // ---- The slots -------------------------------------------------------------------------
        mask.forEach([&r](int d) { r.sum[(size_t) d] = 0.0f; });
        if (leading)
            for (int i = 0; i < s.numTrack; ++i)
                r.trackSum[(size_t) s.trackList[(size_t) i]] = 0.0f;
        const bool inner = s.anyAmount || s.anyRate || s.anyDepth;
        float nextAmount[Router::NUM_FREE] = {}, nextRate[Router::MAX_LFOS] = {}, nextDepth[Router::MAX_LFOS] = {};
        const bool open[Router::NUM_GATES] = { true, pressed, stripUp };
        // (The slots are ordered by gate: those of a closed one are passed over together.)
        for (int gate = 0, a = 0; gate < Router::NUM_GATES; a = s.end[gate], ++gate)
        {
            for (; open[gate] && a < s.end[gate]; ++a)
            {
                const Router::Active& slot = s.active[(size_t) a];
                float amount = slot.amount;
                if (slot.flags & Router::Modulated)
                    amount = std::clamp(amount + amountMod[(size_t) slot.slot], -1.0f, 1.0f);
                const float squared = (slot.flags & Router::SquaredAmount) ? std::abs(amount) : 1.0f;
                if (slot.ownLfo >= 0 && s.anyDepth)
                    amount *= std::clamp(1.0f + depthMod[(size_t) slot.ownLfo], 0.0f, 2.0f);
                if (slot.via != Src::Off)
                    amount *= via(slot.via);
                amount *= squared * slot.scale;
                if (slot.flags & Router::Audio)
                {
                    r.gain[(size_t) slot.slot] = amount;
                    continue;
                }
                const float v = ((slot.flags & Router::Raw) ? raw : src)[(size_t) slot.src] * amount;
                switch (slot.kind)
                {
                    case Router::ToDest:    r.sum[(size_t) slot.dest] += v; break;      // (at rest: into scratch nobody reads)
                    case Router::ToTrack:   if (leading) r.trackSum[(size_t) slot.dest] += v; break;
                    case Router::ToAmount:  nextAmount[(size_t) slot.target] += v; break;
                    case Router::ToRate:
                        for (int i = 0; i < r.lfoCount; ++i)
                            if (slot.target == i || (slot.target < 0 && slot.ownLfo != i))
                                nextRate[(size_t) i] += v * Router::RATE_OCTAVES;
                        break;
                    case Router::ToDepth:
                        for (int i = 0; i < r.lfoCount; ++i)
                            if (slot.target == i || (slot.target < 0 && slot.ownLfo != i))
                                nextDepth[(size_t) i] += v;
                        break;
                    default: break;
                }
            }
        }
        if (inner)      // heard from the next sub-block on
        {
            std::copy(nextAmount, nextAmount + Router::NUM_FREE, amountMod.begin());
            std::copy(nextRate, nextRate + Router::MAX_LFOS, rateMod.begin());
            std::copy(nextDepth, nextDepth + Router::MAX_LFOS, depthMod.begin());
        }
        // The track's effect cells: what the slots on each come to, for the block's effects.
        if (leading)
        {
            for (int i = 0; i < s.numTrack; ++i)
            {
                const auto d = (size_t) s.trackList[(size_t) i];
                const float sum = r.trackSum[d];
                r.trackValue[d] = sum >= -1.0f && sum <= 1.0f ? sum : (sum > 1.0f ? 1.0f : (sum < -1.0f ? -1.0f : 0.0f));  // (not a number: nothing)
            }
            r.led = true;
        }
        if (! sounding)
            return;

        // ---- The destinations ------------------------------------------------------------------
        const DestTable& t = *r.table;
        const float step = 1.0f / (float) count;
        const Mask audioDests = s.audioDests | fadingDests;
        mask.forEach([&](int d)
        {
            const auto n = (size_t) d;
            const Dest& dest = t[d];
            float m = r.sum[n];
            if (snap)
                carry[n] = 0.0f;
            else if (dest.cycle > 0.0f)
            {
                // A PHASE: whole periods of what a change would jump by are no change of the sound.
                // They are dropped at once (where it was and where it goes both move by them, so
                // nothing ramps across them); what is left, half a period at most, is carried off
                // at half a period in 4 ms (never faster than any destination is carried: the
                // whole span in 2 ms). The slots on it are heard as they are meanwhile, and
                // exactly once the carry has gone: an audio-rate slot is never slowed.
                if (structural && ! dest.steps)
                {
                    const float c = lastSum[n] - m;
                    const float whole = std::round(c / dest.cycle);
                    carry[n] = c - whole * dest.cycle;
                    if (whole != 0.0f)
                    {
                        lastSum[n] -= whole * dest.cycle;
                        lastValue[n] -= whole * dest.period * (dest.span < 0.0f ? -1.0f : 1.0f);
                    }
                }
                else if (carry[n] != 0.0f)
                    carry[n] = dsp::towards(carry[n], 0.0f, std::min(fast, r.phaseStep * dest.cycle * (float) count));
            }
            else if (structural && ! dest.steps)
                carry[n] = lastSum[n] - m;          // what the change would have jumped by: carried off
            else if (carry[n] != 0.0f)
                carry[n] = dsp::towards(carry[n], 0.0f, fast);
            m += carry[n];
            if (carry[n] != 0.0f) carrying.set(d); else carrying.clear(d);

            if (! dest.perSample)
            {
                lastSum[n] = m;
                lastValue[n] = law(dest, base[n], m);
                return;
            }
            float* b = r.bufferOf(d) + i0;
            if (audioDests.test(d))
            {
                // The control-rate part per sample: the sum itself where the law is exponential
                // (the audio-rate slots add to the exponent), else the value it gives, held to the
                // range (they add to that). The audio-rate slots follow below.
                const float m0 = snap ? m : lastSum[n], dm = (m - m0) * step, set = base[n];
                if (dest.exp)
                    for (int i = 0; i < count; ++i)
                        b[i] = m0 + dm * (float) (i + 1);
                else
                    for (int i = 0; i < count; ++i)
                        b[i] = law(dest, set, m0 + dm * (float) (i + 1));
                lastSum[n] = m;
                return;
            }
            const float v1 = law(dest, base[n], m), v0 = snap ? v1 : lastValue[n];
            if (v0 == v1)
                std::fill(b, b + count, v1);
            else
            {
                const float dv = (v1 - v0) * step;
                for (int i = 0; i < count; ++i)
                    b[i] = v0 + dv * (float) (i + 1);
                b[count - 1] = v1;
            }
            lastSum[n] = m;
            lastValue[n] = v1;
        });

        // ---- The audio-rate slots: the only per-sample work, and only for them -----------------
        if (s.numAudio > 0 || fading != 0)
        {
            // gain x input added to the destination's buffer, the gain going from g0 to g1
            // (in units of the destination's sum).
            const auto add = [&](int source, int d, float g0, float g1)
            {
                const float* x = r.audioOf(source) + i0;
                float* b = r.bufferOf(d) + i0;
                const float unit = t[d].exp ? 1.0f : t[d].span;
                g0 *= unit;
                const float dg = (g1 * unit - g0) * step;
                for (int i = 0; i < count; ++i)
                    b[i] += (g0 + dg * (float) (i + 1)) * x[i];
            };
            for (int a = 0; a < s.end[Router::Always]; ++a)
            {
                const Router::Active& slot = s.active[(size_t) a];
                if (! (slot.flags & Router::Audio))
                    continue;
                // (The gain is led like any source: an amount turned under a note is no step.)
                float& g = heard[(size_t) slot.slot];
                const float g1 = snap ? r.gain[(size_t) slot.slot] : dsp::towards(g, r.gain[(size_t) slot.slot], fast);
                add(slot.src, slot.dest, snap ? g1 : g, g1);
                g = g1;
                route[(size_t) slot.slot] = { slot.src, slot.dest };
            }
            if (fading != 0)
            {
                bool ended = false;
                for (uint32_t bits = fading; bits != 0; bits &= bits - 1)
                {
                    const auto k = (size_t) __builtin_ctz(bits);
                    const float g1 = dsp::towards(fadingGain[k], 0.0f, fast);
                    add(fadingRoute[k].src, fadingRoute[k].dest, fadingGain[k], g1);
                    fadingGain[k] = g1;
                    if (g1 == 0.0f)
                    {
                        fading &= ~(1u << k);
                        ended = true;
                    }
                }
                if (ended)
                {
                    fadingDests = Mask();
                    for (uint32_t bits = fading; bits != 0; bits &= bits - 1)
                        fadingDests.set(fadingRoute[(size_t) __builtin_ctz(bits)].dest);
                }
            }
            audioDests.forEach([&](int d)
            {
                const Dest& dest = t[d];
                const float set = base[(size_t) d];
                float* b = r.bufferOf(d) + i0;
                if (dest.exp)
                    for (int i = 0; i < count; ++i)
                        b[i] = law(dest, set, b[i]);
                else
                    for (int i = 0; i < count; ++i)
                        b[i] = b[i] < dest.hi ? (b[i] > dest.floor ? b[i] : dest.floor) : dest.hi;
                lastValue[(size_t) d] = b[count - 1];
            });
        }
    }

    std::array<LFO, Router::MAX_LFOS> lfos;
    std::array<uint32_t, Router::MAX_LFOS> lfoGen {}, lfoRestart {};
    std::array<bool, Router::MAX_LFOS> lfoSet {}, lfoUnipolar {};
    std::array<float, Router::MAX_LFOS> rateMod {}, depthMod {};
    std::array<float, Router::NUM_FREE> amountMod {};
    std::array<float, Src::TOTAL> src {}, raw {};       // the sources: on their lead, and as they are
    std::array<float, FirstEnvelope + Src::MAX_ENVELOPES> target {};
    // The engine's own sources as the voice handed them over (publish / publishAudio)
    std::array<float, Src::MAX_PUBLISHED> publishedValue {};
    std::array<const float*, Src::MAX_PUBLISHED> publishedSamples {};
    std::array<int, Src::MAX_PUBLISHED> publishedCount {};
    // ... and what it made of those that are not Immediate: the last MAX_CHUNK samples of each
    static constexpr int MADE_MASK = Router::MAX_CHUNK - 1;
    static_assert((Router::MAX_CHUNK & MADE_MASK) == 0, "a power of two");
    std::array<std::array<float, Router::MAX_CHUNK>, Src::MAX_PUBLISHED> madeRing {};
    std::array<int, Src::MAX_PUBLISHED> madeAt {};
    uint32_t made = 0;                                  // bit k: source k has samples in its ring
    uint32_t handed = 0;                                // bit k: ... and more handed over since the last tick
    uint64_t readBefore = 0;                            // the sources the last tick moved (0: it moved none)
    // The audio-rate slots: each one's gain at the end of the last sub-block and where it was
    // heard; and what is being faded of those that went (at most one a slot)
    struct Heard { uint8_t src = 0, dest = 0; };
    std::array<float, Router::NUM_SLOTS> heard {}, fadingGain {};
    std::array<Heard, Router::NUM_SLOTS> route {}, fadingRoute {};
    uint32_t fading = 0;
    Mask fadingDests;
    const float* own = nullptr;                         // this voice's own set values (useBase)
    std::array<float, DestTable::MAX> lastSum {}, lastValue {}, carry {};
    Mask now, carrying, pushed;
    const Router* router = nullptr;
    uint32_t seen = 0;
    uint32_t serial = 0;                                // the note-on's number: the newest note leads the track's effect cells
    bool fresh = true, busy = false, leading = false, ledAtRest = false;
    double sampleRate = 44100.0;

    // The per-note randoms' generator while the engine hands none over. Until seed() it is
    // seeded by the order the voices were made in, so that no two voices draw the same.
    static spread::Generator unseeded()
    {
        static std::atomic<uint32_t> made { 0 };
        spread::Generator g;
        g.seed(0x9e3779b9u, (int) (made.fetch_add(1, std::memory_order_relaxed) & 0xffffffu) + 0x1000000);
        return g;
    }
    spread::Generator random = unseeded();
};

// ---------------------------------------------------------------------------
// THE ROUTER'S STORED POSITIONS (format 3).
//
// Some of the router's cells hold a POSITION in a list that grows: the DEST of an LFO, of a STRIP
// route, of an XIN route, of a free slot and of a pressure slot, a free slot's SRC and VIA, an
// LFO's SHPE, TRIG and DIV. A file stores such a cell with the entry's NAME beside its number
// (project/StoredLists.h does the storing, for these and for the engines' own such cells), so a
// list that grows in its middle no longer moves a saved route.
//
// This says, for the router's OWN ids (Router::owns: 158-189, 700-772, 773-784), which cell is a
// position and in which list. It is here, beside the cells, so that whoever adds a cell to the
// router adds its line here; "Format V3" walks every engine's pages and fails for a cell that
// shows one of these lists and is not known here. An engine that shows the router's pages at ids
// of its own (the unified voice: STRIP at 158, XIN at 166) hands over the router's id, as it does
// to set() and get(). The per-pad cells of the drum kit (PadModParam) are the kit's and are
// classified with it, in StoredLists.h.
// ---------------------------------------------------------------------------
namespace NamedList
{
    enum class Kind
    {
        None,
        CellDest,       // DestTable::cellLabels: the table's visible entries, then the track's effect cells
        SlotDest,       // DestTable::slotLabels: the visible entries, L1RT .., AMT1 .., the effect cells
        PressureDest,   // an index of the table itself, its hidden entries too (Router::resolve)
        Source,         // SourceTable::labels (a slot's SRC and VIA)
        Shape,          // LFO::shapeLabels
        Trigger,        // Router::triggerLabels
        Division        // LFO::divisionLabels
    };

    constexpr Kind classify(int id)
    {
        if (id >= ModSlotParam::BASE && id < ModSlotParam::END)
        {
            const int cell = (id - ModSlotParam::BASE) % ModSlotParam::STRIDE;
            return cell == ModSlotParam::DEST ? Kind::SlotDest
                 : (cell == ModSlotParam::SRC || cell == ModSlotParam::VIA ? Kind::Source : Kind::None);
        }
        if (PressureSlotParam::contains(id))
            return (id - PressureSlotParam::BASE) % PressureSlotParam::STRIDE == PressureSlotParam::DEST_OFFSET ? Kind::PressureDest : Kind::None;
        if (id >= Router::LFO_BASE && id < Router::STRIP_BASE)
        {
            const int cell = (id - Router::LFO_BASE) % Router::PER_LFO;
            return cell == Router::DestCell ? Kind::CellDest
                 : (cell == Router::Shape ? Kind::Shape
                 : (cell == Router::Trigger ? Kind::Trigger
                 : (cell == Router::Division ? Kind::Division : Kind::None)));
        }
        if (id >= Router::STRIP_BASE && id < Router::STRIP_VIBRATO)
            return (id - Router::STRIP_BASE) % 2 == 0 ? Kind::CellDest : Kind::None;
        if (id >= Router::XIN_BASE && id < Router::XIN_END)
            return (id - Router::XIN_BASE) % 4 == Router::XinDest1 ? Kind::CellDest : Kind::None;
        return Kind::None;
    }

    struct List
    {
        const char* const* labels = nullptr;
        int count = 0;
    };

    // The list of a kind, for an engine's tables.
    inline List listOf(Kind kind, const DestTable& dests, const SourceTable& sources)
    {
        switch (kind)
        {
            case Kind::CellDest:        return { dests.cellLabels(), dests.cellChoices() };
            case Kind::SlotDest:        return { dests.slotLabels(), dests.slotChoices() };
            case Kind::PressureDest:    return { dests.pressureLabels(), dests.pressureChoices() };   // the pressure slots' own list, by value (THE PRESSURE SLOTS)
            case Kind::Source:          return { sources.labels(), sources.size() };
            case Kind::Shape:           return { LFO::shapeLabels(), (int) LFO::Shape::COUNT };
            case Kind::Trigger:         return { Router::triggerLabels(), Router::NUM_TRIGGERS };
            case Kind::Division:        return { LFO::divisionLabels(), LFO::NUM_DIVISIONS };
            default:                    return {};
        }
    }
}

} // namespace mod

// Deferred definitions (EngineBase.h forward-declares mod::Router; needs it complete here).
#include "EngineBase.h"

// Routes as projects stored them before the pressure slots had cells, into the cells: each
// Destination is what the engine says it is here (pressureDestOf: its PressureMap of before), and
// Vibrato is VIB on every engine. Nothing is kept of `routes`: it may go away afterwards.
// nullptr: what an engine that was handed no router did (the allocator's own: the level at 1).
inline void EngineBase::setAftertouchRouter(const AftertouchRouter* routes)
{
    aftertouchRouter = routes;      // (the reference engines of tests/old/ read it block by block)
    if (modRouter() == nullptr)
        return;
    const AftertouchRouter standard = legacyPressureRoutes();
    const AftertouchRouter& from = routes != nullptr ? *routes : standard;
    for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
        setPressureRoute(i, from.effectiveRoute(i));
}

// One old route into slot `index`.
inline void EngineBase::setPressureRoute(int index, const AftertouchRouter::Route& route)
{
    const auto* r = modRouter();
    if (r == nullptr || index < 0 || index >= AftertouchRouter::MAX_ROUTES)
        return;
    int dest = route.dest == AftertouchRouter::Destination::Vibrato ? r->dests().pressureVibrato() : pressureDestOf(route.dest);
    float amount = std::isfinite(route.depth) ? route.depth : 0.0f;
    if (dest <= 0 || dest >= r->dests().pressureTrack())
    {
        dest = 0;                                       // (a route that is off holds nothing:
        amount = mod::DestTable::PRESSURE_AMOUNT;       // the cells of an empty slot)
    }
    setParameter(PressureSlotParam::id(index, PressureSlotParam::DEST_OFFSET), (float) dest);
    setParameter(PressureSlotParam::id(index, PressureSlotParam::AMT_OFFSET), amount);
    setParameter(PressureSlotParam::id(index, PressureSlotParam::BIPOLAR_OFFSET), route.bipolar && dest > 0 ? 1.0f : 0.0f);
}

// What a slot's DEST is as an old route (Off: nothing a route could say). Where several
// Destinations are the same thing here (FM: FM DEPTH and TIMBRE are both the index) the first in
// the enum's order that `allowed` accepts.
inline AftertouchRouter::Destination EngineBase::pressureDestAsRoute(int dest, const std::function<bool(AftertouchRouter::Destination)>& allowed) const
{
    using D = AftertouchRouter::Destination;
    const auto* r = modRouter();
    if (r == nullptr || dest <= 0)
        return D::Off;
    if (dest == r->dests().pressureVibrato())
        return allowed == nullptr || allowed(D::Vibrato) ? D::Vibrato : D::Off;
    for (int d = 1; d < AftertouchRouter::DEST_COUNT; ++d)
        if ((D) d != D::Vibrato && pressureDestOf((D) d) == dest && (allowed == nullptr || allowed((D) d)))
            return (D) d;
    return D::Off;
}
