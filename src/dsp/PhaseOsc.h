#pragma once

// The one table set and the one read-out (Wavetable, WavetableFactory, WavetableRead), and the
// sample bank a sample's cycles are cut from. No engine is included: every engine can use this file.
#include "Wavetable.h"
#include "../engine/SampleBank.h"
#include "DSPUtils.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// =============================================================================
// PhaseOsc — the one phase-modulatable oscillator.
//
// Every pitched oscillator in the program is the same three things: a phase that
// advances by the pitch, something added to that phase (phase modulation, a sync),
// and a waveform read at the result. This file is those three things once:
//
//   phaseosc::Wave        WHAT is read. The sine (computed), or a table with a position:
//                         - a wavetable: any Wavetable, scanned continuously by the position;
//                         - a wave sequence: a table read as a list of waves, each held and then
//                           crossfaded into the next (`steps`, `fade`). ANY table can be read that
//                           way (its frames at equal distances are the waves), and makeSequence()
//                           builds the table for a list of waves picked from anywhere;
//                         - the cycles of a sample: sampleCycles() / cyclesFromSample() give one
//                           period per frame along a recording, so the position is the place in
//                           the sample.
//                         Three of the four sources are therefore one thing, a table and a
//                         position, read by the one read-out the program has (WavetableRead).
//   phaseosc::read()      one sample of a Wave as a FUNCTION of (phase, step, position, swing):
//                         no memory. This is what an oscillator "kind" is in the unified voice
//                         (UnifiedVoice::generateOsc: kinds are functions of the slot's phase,
//                         which is what lets stack, detune and cross-mod act on all of them).
//   phaseosc::PhaseOsc    the phase: advance, sync inside the sample, and the read with phase
//                         modulation. It measures how fast the modulation moves the phase (the
//                         "swing", see the class) and reads a table's band-limited copy for the
//                         pitch AND that, so deep modulation of a bright table does not fold
//                         more than it must. 32 bytes; nothing else is kept.
//
// Units: a phase is in cycles, 0 .. 1 (double). A step is cycles per sample (pitch / sample
// rate), any sign or size. Phase modulation is in RADIANS (float), the unit an FM operator's
// output has always had here (one operator at full level = an index of 1). Position is 0 .. 1.
//
// Guarantees: read() and every PhaseOsc call but one return a finite value for ANY input (not a
// number, infinite, 1e30: such a phase or modulation reads as 0, such a position as its nearest
// end), at most the peak of the wave read (1 for the sine). The one is PhaseOsc::sine(), the bare
// arithmetic of an operator's hot path: finite for a finite modulation. Nothing here allocates,
// locks or logs, except the three builders at the bottom, which say so (message thread).
//
// The sine is exactly an FM operator's arithmetic as it was before this file existed
// (std::sin((float) (phase * 2 pi) + radians), then the advance): the FM engine renders the same
// samples with every operator on the sine ("Phase Osc" pins it against a copy of the old operator).
// A table read without modulation is exactly WavetableRead::sample at WavetableRead::mipLevel of
// the step: the wavetable engine and the unified voice's WT kind read the same samples through it.
//
// -----------------------------------------------------------------------------
// HOW TO PORT AN ENGINE (the FM engine is the model: FMOperator.h, FMEngine.h)
//
//  1. State. Replace each `double phase` (and its hand-written wrap, and its std::sin or table
//     look-up) by a `phaseosc::PhaseOsc osc` and a `phaseosc::Wave wave`. Note-on: osc.reset()
//     (or reset(startPhase) for a random / free start). Prepare: osc.setSampleRate(sr), nothing else.
//  2. Per sample, in the order the engine has today (keep it: the order is audible in feedback):
//       read then advance (FM):     s = osc.read(wave, step, radians, position); osc.advance(step);
//                                   or, the two in one:  s = osc.next(wave, step, radians, position);
//       advance then read (unified voice, where the kinds are functions of the phase):
//                                   osc.advance(step); s = osc.readCycles(wave, step, cycles, position);
//                                   or, for a kind computed by the engine itself from the phase
//                                   (a polyBLEP saw): osc.advance(step); p = osc.at(cycles); ...
//     `step` is the pitch as cycles per sample WITH everything that moves it (detune, bend, LFO).
//     An engine whose modulation is already a phase in cycles (cross-mod's running sum, XIN PM)
//     uses readCycles / at instead of converting to radians and back. Several reads of one
//     wave in a sample (a stack, a spread copy) choose the band-limited copy once and read it at
//     each phase:  c = phaseosc::copyFor(*wave.table, step, osc.getSwing());
//                  s = phaseosc::readAt(wave, c, phaseOfThisCopy, position);
//  3. A sine path that must stay bit for bit and cost what it did: call osc.sine(radians) on it,
//     and make it a BODY OF ITS OWN, chosen by one branch on wave.isSine() at the top of the
//     voice's sample (FMOperator::process and its run<bool>). With the table read as a branch
//     inside the one body the sine was measured a third slower: the compiler then keeps the
//     voice's state in memory around a call that is never made.
//  4. Sync: on the sample where the master wrapped (or the input crossed its threshold),
//     osc.sync(step, since) AFTER the advance, `since` = how much of this sample had passed at
//     that instant (0 .. 1). The restart is a step of the waveform: give the voice's Declick a
//     chance (declick.arm()) or band-limit it yourself; the core does not.
//  5. Cells. A WAVE cell is a Choice over phaseosc::waveLabels() (stored: SINE, CYCL, then the
//     built-in tables in WavetableFactory's order, which only grows at the end); set it with
//     phaseosc::waveTable(index, cyclesTable). A position cell is 0 .. 1, continuous, and becomes
//     an LFO / mod-slot destination by being added to the engine's LfoDestList; the voice applies
//     the keyed value per sample (FMEngine::applyVoiceParam). A change of WAVE under a sounding
//     note is a swap: EngineBase::soundSwapped(). A position that is SET under a note is moved to
//     (phaseosc::POSITION_GLIDE_SECONDS, dsp::towards, snapped at the note's first sample);
//     a position that is MODULATED per sample (an envelope, the track input) is added after that
//     glide, or the glide would hold it back.
//  6. A sample as the waveform: `phaseosc::sampleCycles(bankIndex)` when the SAMPLE cell is set
//     (a cell that isRebuildParam(): message thread, never a step lock), and keep the pointer in
//     the engine. Not when the WAVE cell chooses the cycles: a WAVE cell can be step-locked, and
//     a lock is applied on the audio thread, where nothing may be built (FMEngine::applyWave
//     only hands the kept pointer on). The tables are immutable and live for the whole run,
//     like the bank's entries. (frames, aligned) choose how the recording is cut.
//  7. Delete what this replaced, run the engine's suites and the Mod Slots checksum tool
//     (NEPTUNE_MODSLOT_CHECKSUMS) before and after: with the new cells at rest the lines must
//     not change. Time it (NEPTUNE_PHASEOSC_BENCH=1 on the "Phase Osc" suite shows how).
//
// WHAT IS NOT HERE (an engine that needs it keeps it, on this phase):
//  - Waves computed from the phase (the unified voice's polyBLEP saw, pulse and triangle with
//    their PW shaping, the wave sequencer's and the macro oscillator's): the engine takes the
//    phase from osc.at(cycles) and computes them. They are not a Wave, so an FM operator cannot
//    read them yet; one set of them as functions of (phase, step, shape) beside read() would
//    make them one (the pinned SAW would have to be that function to the bit).
//  - Noise (the wave sequencer's noise step): not a function of a phase.
//  - Through-zero linear FM (the unified voice's XMOD and XIN PM): the engine keeps the running
//    sum of the modulator and hands it over as cycles (readCycles / at).
//  - A band-limited sync restart (step 4), and stereo: a sample's cycles are its two sides mixed.
// =============================================================================
namespace phaseosc
{
    constexpr double CYCLES_PER_RADIAN = 1.0 / dsp::TWO_PI_D;

    // A position set under a sounding note crosses the whole table in this time, never faster.
    constexpr double POSITION_GLIDE_SECONDS = 0.003;

    // The shortest crossfade of a wave sequence, as a share of one step: a position that moves is
    // never a jump from one wave to the next.
    constexpr float MIN_FADE = 1.0f / 32.0f;

    // ---- What is read ---------------------------------------------------------------------------
    struct Wave
    {
        const Wavetable* table = nullptr;       // nullptr: the sine
        WavetableInterpMode interp = WavetableInterpMode::Linear;   // the table read-out's INTP
        int steps = 0;                          // under 2: the position scans the table continuously.
                                                // 2 and up: a sequence of that many waves (the table's
                                                // frames at equal distances), the position its place
        float fade = 1.0f;                      // sequence only: the share of each step spent
                                                // crossfading into the next wave (the rest is held)

        bool isSine() const { return table == nullptr; }
        // Whether the position does anything: more than one frame to move through.
        bool hasPosition() const { return table != nullptr && table->numFrames > 1; }

        static Wave sine() { return {}; }
        static Wave of(const Wavetable* t, WavetableInterpMode mode = WavetableInterpMode::Linear)
        {
            Wave w;
            w.table = t;
            w.interp = mode;
            return w;
        }
        // A table read as a list of waves. numWaves 0: every frame is one (a makeSequence() table).
        static Wave sequence(const Wavetable* t, float fadeShare, int numWaves = 0)
        {
            Wave w = of(t);
            w.steps = t == nullptr ? 0 : (numWaves >= 2 ? numWaves : t->numFrames);
            w.fade = fadeShare;
            return w;
        }
    };

    // 0 .. 1 whatever is given (not a number reads as 0).
    inline float unit(float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }

    // Where in a sequence of `steps` waves the position stands: the wave that sounds, and how far
    // the crossfade into the next one has come (0 while the wave is held).
    struct SequencePlace { int wave = 0; float into = 0.0f; };
    inline SequencePlace sequencePlace(float position, int steps, float fade)
    {
        const float u = unit(position) * (float) (steps - 1);
        SequencePlace p;
        p.wave = std::min((int) u, steps - 2);
        const float f = fade > MIN_FADE ? (fade < 1.0f ? fade : 1.0f) : MIN_FADE;
        const float into = (u - (float) p.wave - (1.0f - f)) / f;
        p.into = into > 0.0f ? (into < 1.0f ? into : 1.0f) : 0.0f;
        return p;
    }

    // ---- The read-out: one sample of a wave, a function of its arguments --------------------------

    // Octaves in a ratio of 1 and up: exact at every power of two, straight lines between them
    // (no logarithm: this runs per sample). Below 1: 0. Too large to say, or not a number: 16.
    inline float octavesIn(double ratio)
    {
        if (! (ratio > 1.0))
            return ratio == ratio ? 0.0f : 16.0f;
        if (! (ratio < 65536.0))
            return 16.0f;
        // ratio = (1 + f) * 2^e, f in [0, 1): e + f, read from the number's own bits.
        std::uint64_t bits = 0;
        std::memcpy(&bits, &ratio, sizeof(bits));
        const int e = (int) (bits >> 52) - 1023;
        bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
        double oneToTwo = 1.0;
        std::memcpy(&oneToTwo, &bits, sizeof(bits));
        return (float) ((double) (e - 1) + oneToTwo);
    }

    // The band-limited copy, as a number with a fraction, for a phase that moves `speed` cycles a
    // sample at its fastest: copy L holds kFrameSize / 2 >> L harmonics, and the whole number here
    // is the first copy whose top harmonic then stays under half the sample rate. The fraction
    // is how far (in octaves) that top harmonic has come from a quarter of the sample rate towards
    // half of it; over the last half octave of that way read() mixes the next copy in, so the
    // top octave has gone when it would fold. The table read-out's own rule for a steady pitch
    // (WavetableRead::mipLevel) drops that octave at the quarter: it is this number rounded up,
    // so a modulation never reads a brighter copy than the steady pitch does, and a modulation
    // too small to bring a harmonic near half the sample rate reads exactly the same one.
    constexpr double SWING_BAND = 2.0 * (double) Wavetable::kFrameSize;
    inline float copyForSpeed(double speed) { return octavesIn(speed * SWING_BAND); }

    // Which of a table's band-limited copies a read takes: one, or two neighbours mixed.
    struct Copy
    {
        int level = 0;
        float next = 0.0f;      // the share of copy level + 1 mixed in (0: the one copy alone)
    };

    // The copy for a phase that advances by `step` cycles a sample (its size counts) and is moved
    // by a modulation `swing` cycles a sample at its fastest on top of that (PhaseOsc measures
    // it). Without swing: the table read-out's own rule (WavetableRead::mipLevel), one copy. With
    // it: the copy for step + swing where that is a duller one, two neighbours mixed so that a
    // swing that grows never steps. An engine that reads one wave several times in a sample at
    // one pitch (a stack, a spread copy) asks once and reads with readAt().
    inline Copy copyFor(const Wavetable& table, double step, float swing = 0.0f)
    {
        const double pitch = std::abs(step);
        Copy c;
        c.level = WavetableRead::mipLevel(table, pitch);
        if (! (swing > 0.0f))
            return c;
        const float duller = copyForSpeed(pitch + (double) swing) - (float) c.level;
        if (! (duller > 0.0f))
            return c;
        const int top = table.getMipLevelCount() - 1;
        const int whole = (int) duller;
        c.level = std::min(c.level + whole, top);
        const float part = 2.0f * (duller - (float) whole) - 1.0f;      // (the last half octave: copyForSpeed)
        if (c.level < top && part > 0.0f)
            c.next = part;
        return c;
    }

    // One sample of a TABLE wave (w.table is not null) at a copy: the scan, or the sequence.
    // phase: 0 .. 1 (anything else reads as 0). position: 0 .. 1 (anything else: its nearest end).
    // (The scan with one copy - every read of a steady pitch - is the read-out itself and nothing
    // around it: the wavetable engine reads two or three of these a sample, and the general path
    // below cost its spread + sub sound 4 %. The samples are the same either way.)
   #if defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline))
   #endif
    inline float readAt(const Wave& w, Copy copy, double phase, float position)
    {
        const Wavetable& table = *w.table;
        if (! (phase >= 0.0 && phase < 1.0))
            phase = 0.0;
        if (w.steps < 2)
        {
            const float place = unit(position);
            const float a = WavetableRead::sample(table, copy.level, place, phase, w.interp);
            if (! (copy.next > 0.0f))
                return a;
            return a + (WavetableRead::sample(table, copy.level + 1, place, phase, w.interp) - a) * copy.next;
        }
        // Where the table is read: one place (the scan, a held wave of a sequence) or two crossfaded.
        float placeA = unit(position), placeB = 0.0f, into = 0.0f;
        if (w.steps >= 2)
        {
            const SequencePlace place = sequencePlace(position, w.steps, w.fade);
            const float last = (float) (w.steps - 1);
            placeA = (float) place.wave / last;
            placeB = (float) (place.wave + 1) / last;
            into = place.into;
        }
        auto at = [&](int level)
        {
            const float a = WavetableRead::sample(table, level, placeA, phase, w.interp);
            if (! (into > 0.0f))
                return a;
            return a + (WavetableRead::sample(table, level, placeB, phase, w.interp) - a) * into;
        };
        const float a = at(copy.level);
        if (! (copy.next > 0.0f))
            return a;
        return a + (at(copy.level + 1) - a) * copy.next;
    }

    // One sample of any wave. phase: 0 .. 1 (dsp::wrapPhase gives one for any number). step: the
    // pitch in cycles per sample. position: 0 .. 1. swing: see copyFor(); 0 = exactly the plain
    // read, WavetableRead::sample at WavetableRead::mipLevel of the step.
    inline float read(const Wave& w, double phase, double step, float position, float swing = 0.0f)
    {
        if (w.table == nullptr)
        {
            const float s = std::sin((float) (phase * dsp::TWO_PI_D));
            return s == s ? s : 0.0f;
        }
        return readAt(w, copyFor(*w.table, step, swing), phase, position);
    }

    // ---- The phase --------------------------------------------------------------------------------
    //
    // Band-limiting under phase modulation. A table's copy is chosen for the pitch; a modulated
    // phase moves faster than the pitch says, and every harmonic k of the table then reaches k
    // times the fastest movement. Choosing the copy afresh each sample for the speed of that
    // sample was measured and is WORSE than doing nothing in most cases: the harmonics are then
    // switched on and off at the modulator's rate, and every switch is a step that folds back
    // (a square table at 1 kHz under a 200 Hz sine, index 8: 75 dB below the signal untouched,
    // 48 dB that way). So the oscillator follows the SWING instead: how far the modulation moves
    // the phase per sample, as a meter reads it (the mean of the movement over about
    // SWING_SECONDS, times pi / 2: for a sine modulator, whatever its pitch, that is the
    // movement at its fastest), and reads the copy that suits pitch + swing (phaseosc::read),
    // mixing two neighbouring copies so that a modulation that grows never steps. Within a
    // steady tone the copy then stands still. It is a mean on purpose: a modulator with an edge
    // (a sawtooth) moves the phase far in one or two samples of each of its cycles, and a
    // follower that held on to that peak would read every table under it as a sine. What folds
    // back during the edge itself is the sound of such a modulator; the carrier keeps its
    // harmonics. Without modulation the swing is 0 and the read is exactly the plain one.
    class PhaseOsc
    {
    public:
        // Only the time the swing is measured over depends on it (48 kHz until told otherwise).
        void setSampleRate(double sampleRate)
        {
            const double sr = sampleRate > 1.0 ? sampleRate : 1.0;      // (also what is not a number)
            swingRate = (float) (1.0 - std::exp(-1.0 / (SWING_SECONDS * sr)));
        }

        // A note starts: the phase (0 unless the engine wants another start), no modulation yet.
        void reset(double startPhase = 0.0)
        {
            phase = dsp::wrapPhase(startPhase);
            lastCycles = 0.0;
            swing = 0.0f;
            warm = 1.0f;
            primed = false;
        }

        double getPhase() const { return phase; }

        // One sample on. Any step: a pitch above the sample rate, a negative one (through zero),
        // an infinite one (the phase is then 0).
        void advance(double step) { phase = dsp::wrapPhase(phase + step); }

        // The cycle starts again inside this sample: `since` of the sample (0 .. 1) had passed
        // when it did, so the phase is where the new cycle has come to by now. `start`: where a
        // cycle starts, for an engine whose waves begin elsewhere than 0.
        void sync(double step, double since = 0.0, double start = 0.0)
        {
            phase = dsp::wrapPhase(start + step * since);
        }

        // The phase with a modulation in cycles added (for a read through phaseosc::read()).
        double at(double cycles) const { return cycles != 0.0 ? dsp::wrapPhase(phase + cycles) : phase; }

        // The sine under phase modulation: an FM operator's arithmetic, unchanged, and nothing
        // else (a test for "not a number" here sits in the loop a feedback operator's output
        // goes round, and was measured at a nanosecond a sample). So this one call is finite
        // for a FINITE modulation only: what an operator is given always is. read() below takes
        // anything.
        float sine(float radians) const
        {
            return std::sin((float) (phase * dsp::TWO_PI_D) + radians);
        }

        // One sample of a wave at the present phase, modulated by `radians`. `step` is the pitch
        // (cycles per sample) the caller will advance by.
        float read(const Wave& w, double step, float radians, float position)
        {
            if (w.table == nullptr)
            {
                const float s = sine(radians);
                return s == s ? s : 0.0f;
            }
            return readCycles(w, step, (double) radians * CYCLES_PER_RADIAN, position);
        }

        // The same with the modulation as a phase in cycles (cross-mod's running sum, XIN PM).
        float readCycles(const Wave& w, double step, double cycles, float position)
        {
            // Nothing is modulating and nothing was: the plain read, and no book-keeping.
            if (cycles == 0.0 && lastCycles == 0.0 && swing == 0.0f)
            {
                primed = true;
                return phaseosc::read(w, phase, step, position);
            }
            // The copy is chosen by the swing as it stood after the LAST read, and this read's
            // movement counts from the next one on. One sample late in a mean over a fiftieth
            // of a second is nothing to hear, and it takes the choice of the copy (the meter,
            // the octave count, the way to the table's memory) off the path from this sample's
            // modulation to this sample's output: in a chain of operators that path is what
            // the time goes on, each waiting for the one before (six table operators in a
            // chain, eight voices: a quarter less time than with this read's own swing).
            const float swingBefore = swing;
            if (primed)
            {
                // (Sample by sample, and smoothly: measured in blocks of 32 samples the copy moved
                // in small steps or kinks, and those were measured to fold back 10 to 20 dB more
                // than taking no notice at all. The mean of the movement, not of its square: a
                // square root a sample cost a quarter of the whole read.)
                const double d = std::abs(cycles - lastCycles);
                const float moved = d < 1.0 ? (float) d : 1.0f;     // (a whole cycle and more, or not a number: 1)
                // Where a modulation begins (a note's first samples, or the first movement
                // after none) the swing is the plain mean of what has been seen so far (weights
                // 1, 1/2, 1/3 ... down to the meter's own): a meter that rose from nothing over
                // its 20 ms left the start of every note under a deep modulation as bright as
                // if nobody had looked, and that folded back 50 dB more than the note's middle.
                swing += (moved * SWING_OF_MEAN - swing) * warm;
                if (warm > swingRate)
                    warm = std::max(warm / (1.0f + warm), swingRate);
                if (! (swing >= SWING_FLOOR))
                {
                    swing = 0.0f;       // the modulation has stopped: the plain read again,
                    warm = 1.0f;        // and the next one is a beginning
                }
            }
            lastCycles = cycles;
            primed = true;
            return phaseosc::read(w, at(cycles), step, position, swingBefore);
        }

        // Read, then advance: the order of an FM operator.
        float next(const Wave& w, double step, float radians, float position)
        {
            const float s = read(w, step, radians, position);
            advance(step);
            return s;
        }

        // What the read is given, in cycles a sample: the fastest movement of a sine as strong
        // as the modulation.
        float getSwing() const { return swing; }

        static constexpr double SWING_SECONDS = 0.02;
        // A sine moves pi / 2 times its mean movement at its fastest.
        static constexpr float SWING_OF_MEAN = 1.5707963f;

    private:
        static constexpr float SWING_FLOOR = 1.0e-7f;   // cycles a sample: below this the modulation has stopped

        double phase = 0.0;
        double lastCycles = 0.0;        // the modulation at the last table read
        float swing = 0.0f;             // cycles per sample: see above
        float swingRate = 0.0010411f;   // (48 kHz)
        float warm = 1.0f;              // the weight of the next movement in the swing: 1 at a beginning, then
                                        // 1/2, 1/3 ... until it is swingRate
        bool primed = false;            // false until the first read of a note: no movement to measure yet
    };

    // =========================================================================
    // Builders (message thread: they allocate, and the last one locks)
    // =========================================================================

    // ---- A wave sequence: a list of waves picked from any tables ---------------------------------
    // Frame k of the result is wave k; read it with Wave::sequence(&table, fade).
    struct Pick
    {
        const Wavetable* table = nullptr;
        float position = 0.0f;
    };

    inline Wavetable makeSequence(const std::vector<Pick>& picks, const char* name = "SEQ")
    {
        Wavetable wt;
        wt.name = name;
        wt.frames.resize(1);
        for (auto& pick : picks)
        {
            if (pick.table == nullptr || pick.table->getMipLevelCount() == 0 || (int) wt.frames[0].size() >= Wavetable::kMaxFrames)
                continue;
            std::array<float, Wavetable::kFrameSize> frame {};
            for (int n = 0; n < Wavetable::kFrameSize; ++n)
                frame[(size_t) n] = WavetableRead::sample(*pick.table, 0, unit(pick.position),
                                                          (double) n / (double) Wavetable::kFrameSize);
            wt.frames[0].push_back(frame);
        }
        if (wt.frames[0].empty())       // nothing picked: a sine, so that the table can always be read
        {
            wt.frames[0].resize(1);
            for (int n = 0; n < Wavetable::kFrameSize; ++n)
                wt.frames[0][0][(size_t) n] = std::sin(dsp::TWO_PI_F * (float) n / (float) Wavetable::kFrameSize);
        }
        wt.buildMipMapsFFT();
        return wt;
    }

    // ---- The cycles of a sample -------------------------------------------------------------------
    // CYCLE_FRAMES places along the recording, one period of the sound at each where it has a
    // pitch (phase-aligned from frame to frame: WavetableFactory::createFromSamplesEx), the plain
    // piece where it has none. The position of a Wave on this table is the place in the sample.
    // `frames` (64, 128 or 256) and `aligned` (false: equal pieces, whatever the pitch) are how
    // the recording is cut: the wavetable engine's FRMS.
    constexpr int CYCLE_FRAMES = 64;

    inline Wavetable cyclesFromSample(const float* left, const float* right, int numSamples, double sampleRate,
                                      const char* name = "CYCL", int frames = CYCLE_FRAMES, bool aligned = true)
    {
        Wavetable wt;
        if (left != nullptr && numSamples >= 64)
        {
            std::vector<float> mono(left, left + numSamples);
            if (right != nullptr)
                for (int i = 0; i < numSamples; ++i)
                    mono[(size_t) i] = 0.5f * (left[i] + right[i]);
            for (auto& s : mono)
                if (! std::isfinite(s))
                    s = 0.0f;
            wt = WavetableFactory::createFromSamplesEx(mono.data(), numSamples, sampleRate > 1.0 ? sampleRate : 44100.0,
                                                       frames, aligned);
        }
        else
            wt = makeSequence({});
        wt.name = name;
        return wt;
    }

    // The cycles of a sample of the bank, one table for each way of cutting it: built once, kept
    // for the whole run (the bank's entries never change), immutable, so every engine and voice
    // can point at them.
    namespace detail
    {
        constexpr int CUTS = 6;     // 64 / 128 / 256 frames, aligned or not
        inline int cut(int frames, bool aligned) { return (frames >= 256 ? 2 : (frames >= 128 ? 1 : 0)) * 2 + (aligned ? 0 : 1); }
        inline std::atomic<const Wavetable*>* cycleSlot(int bankIndex, int frames, bool aligned)
        {
            static std::array<std::atomic<const Wavetable*>, SampleBank::MAX_ENTRIES * CUTS> cache {};
            if (bankIndex < 0 || bankIndex >= SampleBank::MAX_ENTRIES)
                return nullptr;
            return &cache[(size_t) (bankIndex * CUTS + cut(frames, aligned))];
        }
    }

    // The table if it has been made, nullptr if not. Never waits, never allocates: this is the
    // one of the two that the audio thread may call.
    inline const Wavetable* builtSampleCycles(int bankIndex, int frames = CYCLE_FRAMES, bool aligned = true)
    {
        const auto* slot = detail::cycleSlot(bankIndex, frames, aligned);
        return slot != nullptr ? slot->load(std::memory_order_acquire) : nullptr;
    }

    // The table, made if it has to be (MESSAGE THREAD: it allocates, locks, and reads through the
    // whole recording). nullptr when the bank has no such sample.
    inline const Wavetable* sampleCycles(int bankIndex, int frames = CYCLE_FRAMES, bool aligned = true)
    {
        auto* slot = detail::cycleSlot(bankIndex, frames, aligned);
        const auto* e = SampleBank::get().entry(bankIndex);
        if (slot == nullptr || e == nullptr)
            return nullptr;
        if (const Wavetable* made = slot->load(std::memory_order_acquire))
            return made;
        static std::mutex lock;
        static std::vector<std::unique_ptr<Wavetable>> owned;
        const std::lock_guard<std::mutex> guard(lock);
        if (const Wavetable* made = slot->load(std::memory_order_acquire))
            return made;
        const int cut = detail::cut(frames, aligned);
        owned.push_back(std::make_unique<Wavetable>(cyclesFromSample(e->left.data(), e->right.size() == e->left.size() ? e->right.data() : nullptr,
                                                                     (int) e->left.size(), e->sampleRate, e->name.c_str(),
                                                                     64 << (cut / 2), cut % 2 == 0)));
        slot->store(owned.back().get(), std::memory_order_release);
        return owned.back().get();
    }

    // ---- A WAVE cell --------------------------------------------------------------------------------
    // What an oscillator can read, as one stored list: the sine, the cycles of a sample, then the
    // built-in tables in WavetableFactory::builtInTables()'s order (which grows at the end only, so
    // an index keeps its meaning).
    constexpr int WAVE_SINE = 0;
    constexpr int WAVE_CYCLES = 1;
    constexpr int FIRST_TABLE_WAVE = 2;

    inline int waveCount() { return FIRST_TABLE_WAVE + WavetableFactory::builtInCount(); }

    inline const char* const* waveLabels()
    {
        static const std::vector<const char*> labels = []
        {
            std::vector<const char*> l { "SINE", "CYCL" };
            const char* const* builtIn = WavetableFactory::builtInLabels();
            for (int i = 0; i < WavetableFactory::builtInCount(); ++i)
                l.push_back(builtIn[i]);
            return l;
        }();
        return labels.data();
    }

    // The table of a WAVE choice (nullptr: the sine). `cycles`: the engine's sample-cycle table.
    inline const Wavetable* waveTable(int index, const Wavetable* cycles)
    {
        if (index == WAVE_CYCLES)
            return cycles;
        const auto& tables = WavetableFactory::builtInTables();
        const int t = index - FIRST_TABLE_WAVE;
        return t >= 0 && t < (int) tables.size() ? tables[(size_t) t] : nullptr;
    }
}
