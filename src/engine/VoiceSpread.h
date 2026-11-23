#pragma once

#include "ParamSpec.h"
#include "../dsp/Envelope.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// -----------------------------------------------------------------------------
// The per-note spread: what makes two notes of one sound a little different, as on an
// instrument whose voices are never identical. One generator and one page for every engine.
//
//   spread::Generator   one per voice, 8 bytes. Seeded by the engine (its SEED cell and the
//                       voice's number), advanced once per note. A draw is a FUNCTION of
//                       (seed, voice, how many notes this voice has played, the stream's name,
//                       an index): nothing is consumed, so there is no draw order to keep, a
//                       voice may ask for any number of values in any order, and a draw nobody
//                       asks for costs nothing. No global state, no lock, no allocation.
//   VoiceSpread         the engine side: the cells of the ANLG page (ids, true values), and
//                       next(): the note's draws scaled by the cells, as named values.
//
//   ANLG    SLOP FVAR EVAR WVAR | PHAS PVAR LVAR SEED
//     SLOP  each note lands up to this many cents off pitch (0 .. 50)
//     FVAR  each note's cutoff is off by up to this many octaves (0 .. 1), both filters
//     EVAR  each note's envelope times are longer or shorter by up to this many octaves (0 .. 1:
//           at 1 between half and twice), every envelope of the voice by the same factor
//     WVAR  each note's wave is a little different: what "wave" is, is the engine's (the wavetable
//           engine: the table position, up to half the table either way)
//     PHAS  where in its cycle each oscillator starts: 0 = always at the same place (a fixed
//           attack), 1 = anywhere
//     PVAR  each note sits up to this far left or right of the centre (0 .. 1)
//     LVAR  each note is quieter by up to this much (0 .. 1 = down to silence); never louder
//     SEED  which sequence of draws (0 .. 127). The same seed, the same notes in the same order
//           on a newly loaded sound: the same render, on any machine. Setting it starts the
//           sequence again.
//   With every amount at 0 a voice is sample for sample what it is without the section.
//
// THE ROUTER'S RANDOMS AND EVERY OTHER PER-NOTE DRAW. There is one generator: this one.
//   RAND, RND2, RND3, RND4 (mod::Src, one value a note each, -1 .. 1, independent of one another
//   and of the spread) are generator.bipolar(spread::Free, 0 .. 3). An engine with this section
//   hands its voice's generator to mod::Voice::noteOn(router, note, velocity, generator) AFTER
//   next() has advanced it, so SEED repeats the randoms with the spread. An engine without the
//   section calls mod::Voice::seed(engineSeed, voiceIndex) once and the three-argument noteOn:
//   the router's voice then keeps a generator of its own.
//   The wavetable's RAND scan (a new place now and then while a note sounds; today a private
//   xorshift) is spread::Sequence over the stream spread::Scan: start(generator) at the note-on,
//   unipolar() / bipolar() for each further draw.
//   Anything else an engine draws per note takes a stream of its own, appended to Stream.
// The ANLG amounts stay cells: they are read once, at the note-on, where a slot's per-sub-block
// machinery has nothing to add; being continuous cells they are router destinations like any
// other (KEY > FVAR: the low notes vary more), read at the note's first sub-block. EVAR's factor
// goes to Envelope::setTimeScale (times the router's ETIM / FTIM where the engine has them): no
// envelope keeps a second copy of its params for it any more.
//
// HOW TO PORT AN ENGINE (the wavetable engine is the model: WavetableEngine.h)
//  1. The engine owns a VoiceSpread (eight ids of its block) and each voice a spread::Generator.
//     Constructor and whenever SEED is set (VoiceSpread::reseeds(id)): generator.seed(
//     spread.seed(), voiceIndex) for every voice. Route the ids: owns(id) -> set / get / name;
//     appendPage() where the engine's pages want ANLG (before STRIP and the LFO pages).
//  2. Note-on (the allocator's callback, before the voice's first sample):
//         const VoiceSpread::Note n = spread.next(voice.generator());
//     and the voice takes, each where it already has the plain value:
//         n.semitones       added to its pitch offset
//         n.cutoffOctaves   VoiceFilter::noteOn(note, velocity, sounding, n.cutoffOctaves)
//         n.timeScale       Envelope::setTimeScale() on each of its envelopes (the filter's:
//                           VoiceFilter::setEnvTimeScale()), times the router's ETIM / FTIM where
//                           the engine declares them (mod::DestTable::envTimes). The params stay
//                           as set. (spread::scaleTimes, a scaled copy of the params, is what this
//                           replaces: the engines that still use it drop it when they are ported.)
//         n.wave            -1 .. 1 x WVAR: the engine's wave control (position, timbre, PW ...)
//         n.phase(i)        oscillator i's start phase, 0 .. 1 x PHAS; any number of them
//         n.pan             -1 .. 1 x PVAR, added to the voice's pan. A voice struck again while it
//                           sounds is carried to its new place (dsp::towards, a few ms), not jumped:
//                           a voice's declick works on its one signal, before it is placed
//         n.gain            0 .. 1, multiplied into the voice's level
//     Each is exactly 0 (1 for the two factors) when its amount is 0: add and multiply them in
//     where x + 0 and x * 1 leave the sample as it was.
//  3. The unified voice (U4, under the P01 pin): replace both juce::Random::getSystemRandom()
//     blocks of UnifiedVoice::noteOn by
//         noteSlopSemis      = n.semitones          (SLOP, cents as now)
//         noteFilterOct      = n.cutoffOctaves      (FVAR)
//         envelope scale     = n.timeScale          (EVAR; scaledEnvelope stays)
//         oscs[i].phase      = n.phase(i)           (PHAS: OFF = amount 0, ON = amount 1)
//         stackPhases[i][v]  = generator.unipolar(spread::Phase, MAX_OSCS + i * MAX_STACK_VOICES + v)
//                              (always, as today: the stack's copies never start together)
//         oscWaveshapeOffset[i] = generator.bipolar(spread::Wave, i) * waveshapeVar
//                              (the cell labelled WSHP, id WAVE_VAR: it IS the per-note wave
//                              spread, WVAR here; its fixed 0.003 x WSHP scale stays the voice's)
//     DRFT / DRAT (a slow wander while the note sounds) and SOFT (a pitch overshoot at the
//     attack) are not drawn per note and stay the unified voice's own. Its analog.enabled
//     switch becomes "every amount 0". The pin's "seeded" family is made again at that commit;
//     the "plain" family (the five cells at 0) must not move: with STCK at 1 no stack phase is
//     read, and every other draw is multiplied by an amount of 0.
// -----------------------------------------------------------------------------

namespace spread
{

// What a draw is for. The names are part of the sequence (stored sounds with a SEED depend on
// them): append only.
//   Free   the router's per-note randoms: index 0 = RAND, 1 .. 3 = RND2 .. RND4
//   Scan   draws made while the note sounds (Sequence): the wavetable's RAND scan
//   Grain  the sampler's grains and stretch cycles (Sequence: place, pitch, pan, reverse, interval)
//   Slice  the sampler's PLAY = RAND: which slice a note plays
//   Lanes  draws made while the note sounds (Sequence): the wave sequencer's random loop, one
//          draw each time a lane steps
//          (Grain and Slice before Lanes: the sampler's tests were written with those numbers, and
//          "Sampler Grain"'s pitch estimate fails on some other draws of the same statistics)
//   Drum, Exciter, Model  set aside before U4 wave 2 for the drum kit, RSN's exciter and the macro oscillator's
//          models, so that the three ports do not renumber each other at the merge (unused until then)
enum Stream : uint32_t { Pitch = 0, Cutoff, EnvTime, Wave, Phase, Pan, Level, Free, Scan, Grain, Slice, Lanes, Drum, Exciter, Model, NumStreams };

// A 32-bit integer hash with full avalanche ("lowbias32", Chris Wellons, hash-prospector,
// public domain / Unlicense).
inline uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

class Generator
{
public:
    // The engine's seed and this voice's number. Starts the sequence again.
    void seed(uint32_t engineSeed, int voice)
    {
        key = mix(mix(engineSeed + 0x9e3779b9u) ^ ((uint32_t) voice * 0x85ebca6bu + 0xc2b2ae35u));
        notes = 0;
    }

    // A new note: every stream gives new values from here on.
    void advance() { ++notes; }
    uint32_t noteCount() const { return notes; }

    // 0 .. 1 (never 1) and -1 .. 1, the same every time they are asked for within a note.
    float unipolar(Stream stream, int index = 0) const
    {
        return (float) (bits(stream, index) >> 8) * (1.0f / 16777216.0f);
    }
    float bipolar(Stream stream, int index = 0) const { return unipolar(stream, index) * 2.0f - 1.0f; }

private:
    uint32_t bits(Stream stream, int index) const
    {
        return mix(mix(key ^ (notes * 0x9e3779b9u)) + (uint32_t) stream * 0x632be5abu + (uint32_t) index * 0x7f4a7c15u + 1u);
    }

    uint32_t key = 0x4e455054u;
    uint32_t notes = 0;
};

// A run of draws within one note, for what wants a new value now and then while the note sounds
// (the wavetable's RAND scan): the n-th draw of a note is a function of (seed, voice, note count,
// stream, n), so a render repeats. start() at the note-on, after the generator was advanced.
class Sequence
{
public:
    void start(const Generator& g, Stream s = Scan) { generator = g; stream = s; drawn = 0; }
    float unipolar() { return generator.unipolar(stream, drawn++); }
    float bipolar() { return generator.bipolar(stream, drawn++); }
    int count() const { return drawn; }

private:
    Generator generator;
    Stream stream = Scan;
    int drawn = 0;
};

// An envelope whose three times are `scale` times as long (EVAR). 1 leaves it exactly as it is.
// (Superseded by Envelope::setTimeScale, which needs no copy of the params; kept until the
// engines that use it are ported.)
inline Envelope::Params scaleTimes(Envelope::Params p, float scale)
{
    p.attack *= scale;
    p.decay *= scale;
    p.release *= scale;
    return p;
}

} // namespace spread

class VoiceSpread
{
public:
    enum Offset { Slop = 0, Cutoff, EnvTime, Wave, Phase, Pan, Level, Seed, Count };

    static constexpr float MAX_SLOP_CENTS = 50.0f;
    static constexpr int MAX_SEED = 127;

    explicit VoiceSpread(int baseParamId) : base(baseParamId) {}

    int id(Offset o) const { return base + (int) o; }
    bool owns(int paramId) const { return paramId >= base && paramId < base + (int) Count; }
    float get(int paramId) const { return owns(paramId) ? values[(size_t) (paramId - base)] : 0.0f; }
    // True for the cell whose change starts the sequence again: the engine then seeds its voices.
    bool reseeds(int paramId) const { return paramId == base + (int) Seed; }
    uint32_t seed() const { return (uint32_t) values[Seed]; }
    // False while every amount is 0: a note then takes nothing from the section.
    bool on() const { return anyOn; }

    void set(int paramId, float value)
    {
        if (! owns(paramId))
            return;
        const int o = paramId - base;
        if (! std::isfinite(value))
            value = 0.0f;
        if (o == (int) Seed)
            value = (float) std::clamp((int) std::lround(std::clamp(value, 0.0f, (float) MAX_SEED)), 0, MAX_SEED);
        else
            value = std::clamp(value, 0.0f, o == (int) Slop ? MAX_SLOP_CENTS : 1.0f);
        values[(size_t) o] = value;
        anyOn = false;
        for (int i = 0; i < (int) Seed; ++i)
            anyOn = anyOn || values[(size_t) i] > 0.0f;
    }

    // What one note takes: the draws times the cells.
    struct Note
    {
        float semitones = 0.0f;         // SLOP
        float cutoffOctaves = 0.0f;     // FVAR
        float timeScale = 1.0f;         // EVAR: envelope times x this
        float wave = 0.0f;              // WVAR: -1 .. 1 x the amount
        float pan = 0.0f;               // PVAR: -1 .. 1 x the amount
        float gain = 1.0f;              // LVAR: 1 down to 1 - the amount
        // Oscillator i's start phase in cycles, 0 .. 1 x PHAS (0 for every i with PHAS at 0).
        float phase(int i) const { return phaseAmount > 0.0f ? generator.unipolar(spread::Phase, i) * phaseAmount : 0.0f; }

        float phaseAmount = 0.0f;
        spread::Generator generator;    // as it stands for this note (a copy: 8 bytes)
    };

    // Advances the voice's generator to its next note and gives that note's values. Audio thread.
    Note next(spread::Generator& g) const
    {
        g.advance();
        Note n;
        if (! anyOn)
            return n;
        n.generator = g;
        n.phaseAmount = values[Phase];
        if (values[Slop] > 0.0f)    n.semitones = g.bipolar(spread::Pitch) * values[Slop] / 100.0f;
        if (values[Cutoff] > 0.0f)  n.cutoffOctaves = g.bipolar(spread::Cutoff) * values[Cutoff];
        if (values[EnvTime] > 0.0f) n.timeScale = std::exp2(g.bipolar(spread::EnvTime) * values[EnvTime]);
        if (values[Wave] > 0.0f)    n.wave = g.bipolar(spread::Wave) * values[Wave];
        if (values[Pan] > 0.0f)     n.pan = g.bipolar(spread::Pan) * values[Pan];
        if (values[Level] > 0.0f)   n.gain = 1.0f - g.unipolar(spread::Level) * values[Level];
        return n;
    }

    static ParamPage makePage(int baseId)
    {
        auto L = ParamKind::Linear; auto I = ParamKind::Int;
        const int b = baseId;
        ParamPage page;
        page.name = "ANLG"; page.section = "SRC";
        page.params = {{
            { "SLOP", b + Slop, 0, MAX_SLOP_CENTS, 0, L, "ct" },
            { "FVAR", b + Cutoff, 0, 1, 0, L },
            { "EVAR", b + EnvTime, 0, 1, 0, L },
            { "WVAR", b + Wave, 0, 1, 0, L },
            { "PHAS", b + Phase, 0, 1, 0, L },
            { "PVAR", b + Pan, 0, 1, 0, L },
            { "LVAR", b + Level, 0, 1, 0, L },
            { "SEED", b + Seed, 0, (float) MAX_SEED, 0, I } }};
        return page;
    }
    static void appendPage(ParamPages& pages, int baseId) { pages.push_back(makePage(baseId)); }

    static const char* name(int offset)
    {
        static const char* const names[Count] = {
            "Note Pitch Spread", "Note Cutoff Spread", "Note Env Time Spread", "Note Wave Spread",
            "Note Start Phase", "Note Pan Spread", "Note Level Spread", "Spread Seed" };
        return (offset >= 0 && offset < (int) Count) ? names[offset] : "";
    }

private:
    int base;
    std::array<float, Count> values {};
    bool anyOn = false;
};
