#pragma once

#include "EngineBase.h"
#include "../dsp/DSPUtils.h"
#include "SampleBank.h"
#include "ModRouter.h"
#include "VoiceSpread.h"
#include "../dsp/GrainScheduler.h"
#include "../dsp/Envelope.h"
#include "../dsp/filters/FilterSlot.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

class SamplerEngine : public EngineBase
{
public:
    // Granular is appended: projects store the index.
    enum PlayMode { Forward = 0, Reverse, OneShot, Gated, Loop, PingPong, Granular, NumPlayModes };

    enum Param
    {
        SliceSelect = 0,
        StartPoint  = 1,
        EndPoint    = 2,
        LoopStart   = 3,
        LoopEnd     = 4,
        PitchSemi   = 5,
        Mode        = 6,
        FineTune    = 7,
        BitReduce   = 8,
        RateReduce  = 9,
        DriveAmt    = 10,
        Volume      = 11,
        Pan         = 12,
        Attack      = 13,
        Decay       = 14,
        Release     = 15,
        VelToFilter = 16,   // velocity-to-filter-cutoff amount (0..1)
        FilterCutoff = 17,  // base filter cutoff (0..1 -> 20Hz..20kHz)
        SliceCount  = 18,   // grid slices (1 = whole sample)
        Sustain     = 19,   // envelope sustain level
        VelSens     = 20,   // velocity -> level
        KeyTrack    = 21,   // 1 = pitch follows the note, 0 = always original pitch
        Voices      = 22,   // polyphony 1..8
        BendRange   = 23,   // semitones
        SampleSelect = 24,  // index into the SampleBank
        FilterReso  = 25,
        FilterType  = 26,   // index into FilterSlot's display order (SVF, SEM, LADR, DIOD, COMB, OFF)
        FilterMode  = 27,   // mode within the type
        FilterEnv   = 28,   // amp envelope -> cutoff, -1..1
        FilterKey   = 29,   // key tracking of the cutoff
        FilterDrive = 30,
        StartRandom = 31,   // random offset of the start point, as a fraction of the region

        // GRAIN page (used while MODE = GRAN only; see the granular section below)
        GrainSize     = 32, // grain length, 0.005..0.5 s
        GrainDensity  = 33, // grains per second, 1..200
        GrainScan     = 34, // speed of the read position through STRT..END, -2..2 x original speed, 0 = frozen
        GrainSpray    = 35, // random position offset per grain, 0..1 = up to +-250 ms; also loosens the grain timing
        GrainPitchJit = 36, // random pitch per grain, 0..1 = up to +-12 semitones (squared)
        GrainSpread   = 37, // random stereo position per grain, 0 = centre, 1 = full width
        GrainShape    = 38, // GrainWindow
        GrainReverse  = 39, // probability that a grain plays backwards

        // SLICE page (with SliceCount and SliceSelect above)
        SliceMode     = 40, // SliceModeType: GRID (SliceCount equal parts) or TRNS (detected onsets)
        SliceSens     = 41, // TRNS: detection sensitivity, 0..1
        SliceGap      = 42, // TRNS: shortest slice, 0.01..1 s
        SlicePlay     = 43, // SlicePlayMode: which slice a note plays
        SliceFade     = 44, // fade-out at the end of a slice, 0..0.1 s (0 = none, as before)
        SliceLen      = 45, // how much of a slice plays, 0..1 of its length

        // SAMPL page
        RootNote      = 46, // the key that plays the sample at its own pitch (and the first slice in KEYS)
        Normalize     = 47, // 1 = the sample's peak is brought to full scale

        // STRCH page (classic play modes; see the time-stretch section below)
        StretchMode   = 48, // StretchAlgo
        StretchSpeed  = 49, // 0.25..4 x the original speed (SMER: a sixteenth of it)
        StretchSync   = 50, // 1 = the speed comes from StretchLength and the tempo
        StretchLength = 51, // index into stretchLengthBars(): how long the loop lasts
        StretchSize   = 52, // cycle / grain length, 0.005..0.5 s
        StretchXfade  = 53, // crossfade between cycles / grain overlap, 0..1
        StretchJitter = 54, // random position per cycle / grain, 0..1
        StretchSpread = 55, // GRAN, SMER: random stereo position per grain

        NumParams     = 56
    };

    enum SliceModeType { SliceGrid = 0, SliceTransient, NumSliceModes };
    // ONE: the slice chosen with SLIC. KEYS: the root key plays slice 1, the next key slice 2, ...
    // (at the sample's own pitch). NEXT: every note plays the slice after the last one. RAND: any.
    enum SlicePlayMode { SliceOne = 0, SliceKeys, SliceNext, SliceRandom, NumSlicePlayModes };
    enum StretchAlgo { StretchOff = 0, StretchTape, StretchCycle, StretchGrain, StretchSlice, StretchSmear,
                       StretchWsol, NumStretchAlgos };

    static constexpr int NUM_STRETCH_LENGTHS = 10;
    // The LEN cell, in bars of four beats.
    static const float* stretchLengthBars()
    {
        static const float bars[NUM_STRETCH_LENGTHS] = { 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
        return bars;
    }
    static constexpr float SMEAR_SPEED_FACTOR = 1.0f / 16.0f;   // SMER: SPD 1.00 = sixteen times as long
    static constexpr double MIN_STRETCH_SPEED = 1.0 / 256.0, MAX_STRETCH_SPEED = 16.0;
    static constexpr float SPEED_LFO_OCTAVES = 2.0f;
    static constexpr int MAX_GRID_SLICES = 64;                  // the range of the SLCS cell

    enum GrainWindow { WindowHann = 0, WindowTriangle, WindowPerc, WindowSwell, NumGrainWindows };

    // Grains per voice. A grain that is due while all of them sound is skipped (nothing is stolen).
    static constexpr int MAX_GRAINS = 48;
    static constexpr float GRAIN_SPRAY_SECONDS = 0.25f;
    static constexpr float GRAIN_PITCH_JITTER_SEMITONES = 12.0f;

    // What can be moved (the modulation router's table, lfoDests()), in the order of the DEST cells.
    // The first twelve are the original list (stored DEST values keep their meaning), then the
    // page cells in the order they had, LRAT / LDPT, and ETIM. Every voice has its own LFOs.
    //   Per sample (the track input reaches them at audio rate): PTCH, AMP, PAN, CUT, GPOS (read
    //   at the sample a grain starts on).
    //   Read once, at the note's first sub-block: STRT, END, LPST, LPEN, SLIC (PLAY = ONE), FADE,
    //   SLEN, VELF, KTRK, ATK, DEC, SUS, REL, RAND. (In the SLCE stretch FADE and SLEN follow
    //   while the note sounds: every slice it reaches reads them.)
    //   Read when a grain starts: SIZE, DENS, SPRY, PJIT, SPRD, RVRS (and the stretch's SSIZ,
    //   XFAD, JIT, SSPR when a cycle or grain starts).
    //   The rest follow while the note sounds, once a sub-block: RESO, DRIV, SPD, SCAN, BITS,
    //   RATE, LEVL, ENVF, FDRV, VEL, ETIM.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // playback speed, a ratio: one octave x the amount squared
        LfoAmp,         // gain 0..4 (below 0 from an audio-rate source: ring modulation)
        LfoPan,         // +-1 around PAN
        LfoCutoff,      // +-4 octaves
        LfoResonance,   // +-0.5
        LfoStart,       // start point, +-1 of the region around STRT; read once, when the note starts
        LfoDrive,       // the DRIV cell (0..1)
        LfoGrainPos,    // GRAN: where new grains start, +-1 of the region around the scan position (clamped to it)
        LfoGrainSize,   // GRAN: a factor on SIZE, its whole (logarithmic) range at full depth; read when a grain starts
        LfoGrainDens,   // GRAN: a factor on DENS, its whole (logarithmic) range at full depth
        LfoSpeed,       // stretch speed (and SCAN in GRAN mode), x 2^(+-2): a quarter .. four times
        NumLfoDests,    // (the original list ends here)
        LfoEnd = NumLfoDests, LfoLoopStart, LfoLoopEnd,
        LfoScan, LfoSpray, LfoPitchJit, LfoSpread, LfoReverse,
        LfoSlice, LfoFade, LfoSliceLen,
        LfoStretchSize, LfoXfade, LfoJitter, LfoStretchSpread,
        LfoBits, LfoRate, LfoLevel,
        LfoVelFilter, LfoFilterEnv, LfoFilterKey, LfoFilterDrive,
        LfoAttack, LfoDecay, LfoSustain, LfoRelease, LfoVelSens, LfoStartRandom,
        LfoCrossRate, LfoCrossDepth,
        LfoEnvTime,     // ETIM: the envelope's three times, a factor (three octaves either way)
        NumDests
    };

    static const char* const* lfoDestLabels() { return lfoDests().labels(); }

    // The voice's own sources, in the order the engine publishes them (sources()).
    enum Published
    {
        PubPos = 0,     // POS: where the playhead is in what the note plays (STRT..END of the slice or sample), 0..1
        PubGrain,       // GRN: where the grain started last lies in what grains may read (the slice or sample), 0..1
        PubLevel        // SLEV: the sample's own level at the playhead (the onset analysis' 23 ms mean), 0..1 of its loudest
    };

    static constexpr int MAX_VOICES = 16;
    // Voice slots: the notes that sound (VOX of them at most) and, for a few milliseconds each,
    // the ones that gave way to a new note and are fading out (see allocateVoice).
    static constexpr int POOL_VOICES = MAX_VOICES * 2;
    static constexpr float STEAL_FADE_SECONDS = 0.004f;     // fade of a note that gives way
    static constexpr double END_DECLICK_SECONDS = 0.003;    // fade into the end of a sample that does not loop
    static constexpr int MAX_SLICES = 128;

    struct Slice
    {
        int startSample = 0;
        int endSample = 0;
    };

private:
    struct Voice;

public:

    SamplerEngine(int maxVoices = MAX_VOICES)
        : numVoices(std::min(maxVoices, MAX_VOICES))
    {
        for (auto& v : voices)
            v = Voice();
        // The per-note randoms (RAND, RND2 .., the RAND start, PLAY = RAND, the grains) repeat from
        // run to run: the same notes in the same order on a new engine draw the same values. (This
        // engine has no ANLG page, so no SEED cell: the seed is fixed, as the macro oscillator's.)
        setGrainSeed(0);
        setSliceSeed(0);
        playingSlice.store(-1, std::memory_order_relaxed);
        cellRanges();
        clearPlayheads();
        for (auto& p : playheadGrain)
            p.store(-1.0f, std::memory_order_relaxed);

        // Real defaults for every parameter, so getParameter is always the true value.
        for (auto& page : pages())
            for (auto& p : page.params)
                if (p.isValid())
                {
                    params[(size_t) p.id] = p.defaultVal;
                    router.setBase(p.id, p.defaultVal);
                }
        params[Pan] = 0.0f;

        selectBankSample(SampleBank::get().defaultIndex());
    }

    // SAMPL (which sample and what part of it), GRAIN (the granular play mode), SLICE (how the sample
    // is cut up and which slice a note plays), STRCH (time-stretch), TONE (pitch and colour), FILTR,
    // AMP (envelope, voices). The modulation router's pages follow (getParamPages).
    static const ParamPages& pages()
    {
        static const char* const modes[NumPlayModes] = { "FWD", "REV", "1SHT", "GATE", "LOOP", "PING", "GRAN" };
        static const char* const windows[NumGrainWindows] = { "HANN", "TRI", "PERC", "SWEL" };
        static const char* const sliceModes[NumSliceModes] = { "GRID", "TRNS" };
        static const char* const slicePlays[NumSlicePlayModes] = { "ONE", "KEYS", "NEXT", "RAND" };
        static const char* const algos[NumStretchAlgos] = { "OFF", "TAPE", "CYCL", "GRAN", "SLCE", "SMER", "WSOL" };
        static const char* const lengths[NUM_STRETCH_LENGTHS] = { "1/16", "1/8", "1/4", "1/2", "1", "2", "4", "8", "16", "32" };
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        auto& bank = SampleBank::get();
        static const ParamPages list = [&]
        {
          ParamPages l = {
            { "SAMPL", "SRC", {{
                // A new sampler plays the bank's sustaining tone, looped: a held key sounds until it
                // is let go and then fades over REL. MODE follows the sample (see followSampleMode).
                { "SMPL", SampleSelect, 0, (float) (bank.size() - 1), (float) bank.defaultIndex(), C, "", bank.names(), bank.size() },
                { "MODE", Mode, 0, (float) (NumPlayModes - 1), (float) Loop, C, "", modes, NumPlayModes },
                { "STRT", StartPoint, 0, 1, 0, L },
                { "END",  EndPoint, 0, 1, 1, L },
                { "LPST", LoopStart, 0, 1, 0, L },
                { "LPEN", LoopEnd, 0, 1, 1, L },
                { "ROOT", RootNote, 0, 127, 60, I, "note" },
                { "NORM", Normalize, 0, 1, 0, C, "", ParamChoices::offOn, 2 } }} },
            // Heard while MODE = GRAN. SIZE x DENS = how many grains overlap (defaults: 4).
            { "GRAIN", "SRC", {{
                { "SIZE", GrainSize, 0.005f, 0.5f, 0.1f, G, "s" },
                { "DENS", GrainDensity, 1, 200, 40, G, "Hz" },
                { "SCAN", GrainScan, -2, 2, 1, L, "x", nullptr, 0, 0.05f },
                { "SPRY", GrainSpray, 0, 1, 0, L },
                { "PJIT", GrainPitchJit, 0, 1, 0, L },
                { "SPRD", GrainSpread, 0, 1, 0, L },
                { "SHAP", GrainShape, 0, (float) (NumGrainWindows - 1), 0, C, "", windows, NumGrainWindows },
                { "RVRS", GrainReverse, 0, 1, 0, L } }} },
            // SLCS: GRID = how many equal slices; TRNS = how many were found (turning it down keeps
            // only the strongest; 1 = no limit). FADE and SLEN shape the end of every slice played.
            { "SLICE", "SRC", {{
                { "MODE", SliceMode, 0, (float) (NumSliceModes - 1), 0, C, "", sliceModes, NumSliceModes },
                { "SLCS", SliceCount, 1, (float) MAX_GRID_SLICES, 1, I },
                { "SENS", SliceSens, 0, 1, 0.5f, L },
                { "GAP",  SliceGap, 0.01f, 1, 0.05f, G, "s" },
                { "SLIC", SliceSelect, 0, 1, 0, L },
                { "PLAY", SlicePlay, 0, (float) (NumSlicePlayModes - 1), 0, C, "", slicePlays, NumSlicePlayModes },
                { "FADE", SliceFade, 0, 0.1f, 0, L, "s" },
                { "SLEN", SliceLen, 0, 1, 1, L } }} },
            // Heard in every play mode but GRAN (there SYNC and LEN set what SCAN 1.00 means). SPD is
            // logarithmic, so Func lands on 0.25 / 0.5 / 1 / 2 / 4. LEN is in bars.
            { "STRCH", "SRC", {{
                { "ALGO", StretchMode, 0, (float) (NumStretchAlgos - 1), 0, C, "", algos, NumStretchAlgos },
                { "SPD",  StretchSpeed, 0.25f, 4, 1, G, "x" },
                { "SYNC", StretchSync, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
                { "LEN",  StretchLength, 0, (float) (NUM_STRETCH_LENGTHS - 1), 4, C, "", lengths, NUM_STRETCH_LENGTHS },
                { "SIZE", StretchSize, 0.005f, 0.5f, 0.05f, G, "s" },
                { "XFAD", StretchXfade, 0, 1, 0.25f, L },
                { "JIT",  StretchJitter, 0, 1, 0, L },
                { "SPRD", StretchSpread, 0, 1, 0, L } }} },
            { "TONE", "SRC", {{
                { "PTCH", PitchSemi, -24, 24, 0, I, "st" },
                { "FINE", FineTune, -1, 1, 0, L, "st" },
                { "BITS", BitReduce, 0, 1, 0, L },
                { "RATE", RateReduce, 0, 1, 0, L },
                { "DRIV", DriveAmt, 0, 1, 0, L },
                { "LEVL", Volume, 0, 1, 0.8f, L },
                { "KEY",  KeyTrack, 0, 1, 1, C, "", ParamChoices::offOn, 2 },
                { "BEND", BendRange, 0, 24, 2, I, "st" } }} },
            // Per-voice filter: any type from the filter set. CUT fully open with no envelope = bypassed.
            { "FILTR", "FLTR", {{
                { "CUT",  FilterCutoff, 0, 1, 1, L },
                { "RESO", FilterReso, 0, 1, 0.2f, L },
                { "TYPE", FilterType, 0, FilterSlot::NUM_MODELS - 1, 0, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
                { "MODE", FilterMode, 0, 5, 0, C, "", FilterSlot::modeNames(FilterModel::SVF), 4 },
                { "VELF", VelToFilter, 0, 1, 0, L },
                { "ENVF", FilterEnv, -1, 1, 0, L },
                { "KTRK", FilterKey, -1, 1, 0, L },
                { "FDRV", FilterDrive, 0, 1, 0, L } }} },
            { "AMP", "AMP", {{
                { "ATK",  Attack, 0.001f, 5, 0.001f, G, "s" },
                { "DEC",  Decay, 0.001f, 5, 0.1f, G, "s" },
                { "SUS",  Sustain, 0, 1, 1, L },
                { "REL",  Release, 0.001f, 5, 0.05f, G, "s" },
                { "VEL",  VelSens, 0, 1, 0.8f, L },
                { "VOX",  Voices, 1, MAX_VOICES, 8, I },
                { "PAN",  Pan, -1, 1, 0, L },
                { "RAND", StartRandom, 0, 1, 0, L } }} },
          };
          return l;
        }();
        return list;
    }

    // Each parameter's cell (its range), by id; null for an id no page shows. Built once, off the
    // audio thread (the constructor asks for it).
    static const std::array<const ParamSpec*, NumParams>& cellRanges()
    {
        static const std::array<const ParamSpec*, NumParams> table = []
        {
            std::array<const ParamSpec*, NumParams> t {};
            for (auto& page : pages())
                for (auto& p : page.params)
                    if (p.id >= 0 && p.id < NumParams)
                        t[(size_t) p.id] = &p;
            return t;
        }();
        return table;
    }

    // What can be moved: the modulation router's table (see LfoDest for how each entry is read).
    // Not offered: SMPL, the slicing cells and NORM (they rebuild data), MODE, SHAP, PLAY, ALGO,
    // SYNC, LEN, KEY, TYPE, VOX (choices and counts), ROOT, PTCH, FINE and BEND (PTCH of the
    // original list is the pitch). There is one envelope, so ETIM and no FTIM.
    static const mod::DestTable& lfoDests()
    {
        static const mod::DestTable table = []
        {
            const ParamPages& p = pages();
            mod::DestTable t;
            t.group();
            t.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);       // a ratio: one octave x the amount squared
            t.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample).audioFloor(-4.0f);   // (below 0: ring modulation)
            t.input("PAN", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);
            t.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);               // octaves
            t.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
            t.input("STRT", 0.0f, 1.0f, -1.0f, 1.0f);
            t.add(p, DriveAmt);
            t.input("GPOS", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);
            // (Factors: the cell's whole logarithmic range at full depth, as before the router.)
            t.input("SIZE", 1.0f, std::log2(MAX_GRAIN_SIZE / MIN_GRAIN_SIZE), MIN_GRAIN_SIZE / MAX_GRAIN_SIZE,
                    MAX_GRAIN_SIZE / MIN_GRAIN_SIZE, mod::Exp);
            t.input("DENS", 1.0f, std::log2(MAX_GRAIN_DENSITY / MIN_GRAIN_DENSITY), MIN_GRAIN_DENSITY / MAX_GRAIN_DENSITY,
                    MAX_GRAIN_DENSITY / MIN_GRAIN_DENSITY, mod::Exp);
            t.input("SPD", 1.0f, SPEED_LFO_OCTAVES, 1.0f / 16.0f, 16.0f, mod::Exp);
            t.group();
            for (int id : { (int) EndPoint, (int) LoopStart, (int) LoopEnd })
                t.add(p, id);
            t.group();
            for (int id : { (int) GrainScan, (int) GrainSpray, (int) GrainPitchJit, (int) GrainSpread, (int) GrainReverse })
                t.add(p, id);
            t.group();
            for (int id : { (int) SliceSelect, (int) SliceFade, (int) SliceLen })
                t.add(p, id);
            t.group();
            t.add(p, StretchSize, "SSIZ");
            t.add(p, StretchXfade);
            t.add(p, StretchJitter);
            t.add(p, StretchSpread, "SSPR");
            t.group();
            for (int id : { (int) BitReduce, (int) RateReduce, (int) Volume })
                t.add(p, id);
            t.group();
            for (int id : { (int) VelToFilter, (int) FilterEnv, (int) FilterKey, (int) FilterDrive })
                t.add(p, id);
            t.group();
            for (int id : { (int) Attack, (int) Decay, (int) Sustain, (int) Release, (int) VelSens, (int) StartRandom })
                t.add(p, id);
            t.addCross();
            // The envelope's time (after what was there before: a stored DEST keeps its meaning).
            t.group();
            t.envTimes(false);
            // The mod strip until the STRIP page says otherwise: more drive (with the level taken
            // back, see processBlock), a lower sample rate, and looser grains in GRAN mode.
            t.stripRoute(0, LfoDrive, 0.7f);
            t.stripRoute(1, LfoRate, 0.3f);
            t.stripRoute(2, LfoSpray, 0.5f);
            t.finish(2);
            jassert(t.size() == NumDests && t.ampTime() == LfoEnvTime && t.indexOfLabel("SPD") == LfoSpeed
                    && t.indexOfId(DriveAmt) == LfoDrive && t.indexOfId(EndPoint) == LfoEnd
                    && t.indexOfId(GrainScan) == LfoScan && t.indexOfId(SliceSelect) == LfoSlice
                    && t.indexOfId(StretchSize) == LfoStretchSize && t.indexOfId(BitReduce) == LfoBits
                    && t.indexOfId(VelToFilter) == LfoVelFilter && t.indexOfId(Attack) == LfoAttack
                    && t.indexOfId(StartRandom) == LfoStartRandom && t.indexOfLabel("LRAT") == LfoCrossRate);
            return t;
        }();
        static const bool registered = (table.registerGroups(), true);
        juce::ignoreUnused(registered);
        return table;
    }

    // What a slot can read here beside the router's own sources: the list SRC and VIA show.
    //   POS   where the playhead is in what the note plays, 0 at STRT (of the slice, or the
    //         sample) and 1 at END: a filter or a pan can follow where the sample is. In GRAN
    //         mode and in a stretch it is the scan / time position.
    //   GRN   where the grain that started last lies in what grains may read (the slice, or the
    //         whole sample): the scan position with GPOS and SPRY on it. It keeps its last value
    //         where no grain starts; a note begins with the place it starts at.
    //   SLEV  the sample's own level at the playhead: the mean level over 23 ms that the transient
    //         slicing is worked out from (analyseOnsets), as a fraction of the sample's loudest
    //         23 ms. Nothing is analysed for it.
    // All three are control-rate (once a sub-block, led over 4 ms) and 0..1.
    static const mod::SourceTable& sources()
    {
        static const mod::SourceTable table = []
        {
            mod::SourceTable t(2);
            t.publish("POS", mod::Unipolar);
            t.publish("GRN", mod::Unipolar);
            t.publish("SLEV", mod::Unipolar);
            return t;
        }();
        static const bool registered = (table.registerGroups(), true);
        juce::ignoreUnused(registered);
        return table;
    }

    // What an old pressure route (AftertouchRouter::Destination) is here, as a slot's DEST:
    //   VOL the level, FLT the cutoff, PAN the pan, TBR the drive (DRIV), WAV where grains are
    //   taken from (GPOS); VIB is the router's own (vibrato on the pitch).
    // A sampler is made with no route (its table names none): a pad's pressure moves a sample only
    // where AT ROUTE, or a free slot from PRES, asks for it.
    int pressureDestOf(AftertouchRouter::Destination d) const override
    {
        using D = AftertouchRouter::Destination;
        switch (d)
        {
            case D::Volume:         return LfoAmp;
            case D::FilterCutoff:   return LfoCutoff;
            case D::MacroTimbre:    return LfoDrive;
            case D::WavePosition:   return LfoGrainPos;
            case D::PanSpread:      return LfoPan;
            default:                return 0;
        }
    }
    // (A sampler that was handed no routes had none.)
    AftertouchRouter legacyPressureRoutes() const override
    {
        AftertouchRouter none;
        none.clear();
        return none;
    }

    mod::Router* modRouter() override { return &router; }
    const mod::Router* modRouter() const override { return &router; }

    // The filter MODE cell lists the modes of the filter type currently chosen, and the SMPL cell
    // the samples the bank holds now (it grows while the app runs: recordings are appended).
    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        router.appendPages(out);        // XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2
        auto model = FilterSlot::modelFromIndex((int) params[FilterType]);
        auto& bank = SampleBank::get();
        const int bankSize = bank.size();
        for (auto& page : out)
            for (auto& p : page.params)
            {
                if (p.id == FilterMode)
                {
                    p.choices = FilterSlot::modeNames(model);
                    p.numChoices = FilterSlot::modeCount(model);
                    p.maxVal = (float) (p.numChoices - 1);
                }
                else if (p.id == SampleSelect)
                {
                    p.choices = bank.names();
                    p.numChoices = bankSize;
                    p.maxVal = (float) std::max(0, bankSize - 1);
                }
            }
        return out;
    }

    void pitchBend(float amount) override { bend = std::clamp(amount, -1.0f, 1.0f); }
    // Pad pressure: kept for each voice that holds the note (the source PRES and the pressure routes).
    void aftertouch(int note, float pressure) override
    {
        for (auto& v : voices)
            if (v.active && v.gateOn && v.note == note)
                v.pressure = std::clamp(pressure, 0.0f, 1.0f);
    }
    // The mod strip wears the sound down until the STRIP page says otherwise: it adds to DRIV and
    // to RATE (sample-rate reduction) on the TONE page, and in GRAN mode to SPRY as well.
    void modWheel(float value) override { wheel = std::clamp(value, 0.0f, 1.0f); }
    void setTempo(float bpm) override
    {
        if (bpm > 1.0f)
            tempoBpm = bpm;     // read once per block: a synced stretch follows the tempo while notes sound
        router.setTempo(bpm);
    }

    // The sample is chosen first when a stored sound is loaded: choosing it resets the slices to
    // the sample's own suggestion, and the stored slice settings then apply on top.
    int getMasterParamId() const override { return SampleSelect; }
    bool isRebuildParam(ParamId id) const override
    {
        return id == SampleSelect || id == SliceCount || id == SliceMode || id == SliceSens
            || id == SliceGap || id == Normalize;
    }

    // Point the engine at a bank sample (message thread).
    void selectBankSample(int index)
    {
        const auto* e = SampleBank::get().entry(index);
        if (e == nullptr || e->left.empty())
            return;

        const PlayMode suggestedBefore = suggestedMode();
        beginSampleChange();
        sampleL = e->left;
        sampleR = e->right;
        sampleLength = (int) e->left.size();
        originalSampleRate = e->sampleRate;
        sliceCount = 0;
        samplePoolId = -1;
        bankIndex = index;
        levelTable.clear();     // (SLEV reads 0 until the new table is handed over: no free here)
        endSampleChange();
        analyseSample();
        followSampleMode(suggestedBefore);

        // A sample that suggests slices (the factory loop) gets them, and the cell shows it.
        int slices = e->defaultSlices > 1 ? e->defaultSlices : 1;
        params[SliceCount] = (float) slices;
        if (slices > 1)
            sliceByGrid(slices);

        // A drum break knows how many bars it is: LEN shows it, so SYNC alone plays it in time.
        if (e->bars > 0.0f)
            params[StretchLength] = (float) stretchLengthIndex(e->bars);
        sampleChanged();
    }

    // The LEN choice nearest to a length in bars.
    static int stretchLengthIndex(float bars)
    {
        int best = 0;
        for (int i = 1; i < NUM_STRETCH_LENGTHS; ++i)
            if (std::abs(stretchLengthBars()[i] - bars) < std::abs(stretchLengthBars()[best] - bars))
                best = i;
        return best;
    }

    int getBankIndex() const { return bankIndex; }

    // The play mode the sample in use suggests: LOOP for a sustaining loop of the bank (the factory
    // tone), FWD for everything else (hits, the beat, files, recordings).
    PlayMode suggestedMode() const
    {
        const auto* e = SampleBank::get().entry(bankIndex);
        return e != nullptr && e->sustains ? Loop : Forward;
    }

    EngineType getType() const override { return EngineType::Sampler; }
    juce::String getName() const override { return "Sampler"; }

    // --- Sample loading (message thread only) ---

    void loadSample(const float* data, int numSamples, int numChannels, double fileSampleRate)
    {
        const PlayMode suggestedBefore = suggestedMode();
        beginSampleChange();
        bankIndex = -1;
        sampleL.resize((size_t) numSamples);
        if (numChannels >= 2)
            sampleR.resize((size_t) numSamples);
        else
            sampleR.clear();

        std::memcpy(sampleL.data(), data, sizeof(float) * (size_t) numSamples);
        if (numChannels >= 2)
            std::memcpy(sampleR.data(), data + numSamples, sizeof(float) * (size_t) numSamples);

        sampleLength = numSamples;
        originalSampleRate = fileSampleRate;
        sliceCount = 0;
        samplePoolId = -1;
        levelTable.clear();     // (SLEV reads 0 until the new table is handed over: no free here)
        endSampleChange();
        analyseSample();
        followSampleMode(suggestedBefore);
        sampleChanged();
    }

    void loadSampleStereoInterleaved(const float* interleavedData, int numFrames, double fileSampleRate)
    {
        const PlayMode suggestedBefore = suggestedMode();
        beginSampleChange();
        bankIndex = -1;
        sampleL.resize((size_t) numFrames);
        sampleR.resize((size_t) numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            sampleL[(size_t) i] = interleavedData[i * 2];
            sampleR[(size_t) i] = interleavedData[i * 2 + 1];
        }
        sampleLength = numFrames;
        originalSampleRate = fileSampleRate;
        sliceCount = 0;
        samplePoolId = -1;
        levelTable.clear();     // (SLEV reads 0 until the new table is handed over: no free here)
        endSampleChange();
        analyseSample();
        followSampleMode(suggestedBefore);
        sampleChanged();
    }

    // --- Sample pool integration ---
    void setSamplePoolId(int id) { samplePoolId = id; }
    int getSamplePoolId() const { return samplePoolId; }

    // --- Slice management ---

    void setSlices(const Slice* sliceArray, int count)
    {
        sliceCount = std::min(count, MAX_SLICES);
        for (int i = 0; i < sliceCount; ++i)
            slices[i] = sliceArray[i];
    }

    void sliceByGrid(int numSlices)
    {
        if (sampleLength <= 0 || numSlices <= 0) return;
        sliceCount = std::min(numSlices, MAX_SLICES);
        int sliceLen = sampleLength / sliceCount;
        for (int i = 0; i < sliceCount; ++i)
        {
            slices[i].startSample = i * sliceLen;
            slices[i].endSample = (i == sliceCount - 1) ? sampleLength : (i + 1) * sliceLen;
        }
    }

    int getSliceCount() const { return sliceCount; }

    // Where slice `slice` starts, 0..1 of the whole sample; -1 if there is no such slice.
    float getSliceStartNormalized(int slice) const
    {
        const int count = sliceCount, length = sampleLength;
        if (slice < 0 || slice >= count || length <= 0)
            return -1.0f;
        return std::clamp((float) slices[slice].startSample / (float) length, 0.0f, 1.0f);
    }

    // The slice the most recent note plays (in the SLCE stretch: the slice it has reached);
    // -1 while nothing sounds or the note plays the whole sample. Safe from any thread.
    int getPlayingSlice() const { return playingSlice.load(std::memory_order_relaxed); }

    // Restarts the generator of PLAY = RAND (same seed = same order) and the order of PLAY = NEXT.
    void setSliceSeed(uint32_t seed)
    {
        sliceGen.seed(seed, POOL_VOICES);       // (no voice's number: the order of the notes, not of a voice)
        nextSlice = 0;
    }

    // Stretch speed in use (original speed = 1): from SPD, or from LEN and the tempo while SYNC is on.
    float getStretchSpeed() const { return (float) stretchSpeed(getStretchAlgo()); }
    int getSampleLength() const { return sampleLength; }
    int getSampleChannels() const { return sampleR.empty() ? 1 : 2; }
    double getOriginalSampleRate() const { return originalSampleRate; }
    const float* getSampleData() const { return sampleL.empty() ? nullptr : sampleL.data(); }
    const float* getSampleDataR() const { return sampleR.empty() ? nullptr : sampleR.data(); }
    const Slice& getSlice(int idx) const { return slices[std::clamp(idx, 0, MAX_SLICES - 1)]; }

    bool hasSample() const { return sampleLength > 0 && !sampleL.empty(); }

    // --- Playhead position for display ---
    double getVoicePosition(int voiceIdx) const
    {
        if (voiceIdx < 0 || voiceIdx >= POOL_VOICES) return -1.0;
        const auto& v = voices[voiceIdx];
        if (!v.active) return -1.0;
        return v.position;
    }

    // Return normalized playhead of first active voice (for waveform display). In GRAN mode a
    // voice's position is its scan position.
    float getPlayheadNormalized() const
    {
        for (int i = 0; i < POOL_VOICES; ++i)
        {
            if (voices[i].active && ! voices[i].dying && sampleLength > 0)
                return (float)(voices[i].position / (double) sampleLength);
        }
        return -1.0f;
    }

    // Normalised positions (0..1 of the whole sample) of what is sounding now: one per active voice in
    // the classic modes; in GRAN mode the scan position of each active voice first, then active grain
    // positions (at most MAX_PLAYHEAD_GRAINS of them, evenly thinned when more are sounding).
    // Returns the number written (<= maxOut). `isGrain[i]` (optional, may be nullptr) says which are
    // grains.
    //
    // Safe from any thread while audio runs, without locks: the audio thread publishes the positions
    // into atomics (relaxed) at the end of every processBlock, so this is the state as of the last
    // block. The values are not one consistent snapshot (a voice and its grains may come from two
    // successive blocks), which is fine for drawing. Nothing is reported while no block has run since
    // the notes stopped, the sample changed or the engine was released.
    static constexpr int MAX_PLAYHEAD_GRAINS = 64;

    int getPlayheads(float* out, int maxOut, bool* isGrain = nullptr) const
    {
        if (out == nullptr || maxOut <= 0)
            return 0;
        int n = 0;
        for (int i = 0; i < POOL_VOICES && n < maxOut; ++i)
        {
            const float p = playheadVoice[i].load(std::memory_order_relaxed);
            if (p < 0.0f)
                continue;
            if (isGrain != nullptr) isGrain[n] = false;
            out[n++] = std::min(p, 1.0f);
        }
        const int grains = std::clamp(playheadGrainCount.load(std::memory_order_relaxed), 0, MAX_PLAYHEAD_GRAINS);
        for (int i = 0; i < grains && n < maxOut; ++i)
        {
            const float p = playheadGrain[i].load(std::memory_order_relaxed);
            if (p < 0.0f)
                continue;
            if (isGrain != nullptr) isGrain[n] = true;
            out[n++] = std::min(p, 1.0f);
        }
        return n;
    }

    // --- Granular mode: test and display helpers ---

    // Restarts the voices' generators (spread::Generator, one a voice, advanced once a note): the
    // same seed, the same notes in the same order = the same grain clouds and stretch cycles
    // (spread::Sequence over the stream Grain, begun at each note), the same RAND start offsets
    // and the same RAND, RND2 .. RND4 of the router.
    void setGrainSeed(uint32_t seed)
    {
        for (int i = 0; i < POOL_VOICES; ++i)
            voices[i].gen.seed(seed, i);
    }

    // Grains sounding now, over all voices (plain read; for tests and meters, not for decisions).
    int getActiveGrainCount() const
    {
        int n = 0;
        for (int i = 0; i < POOL_VOICES; ++i)
            if (voices[i].active)
                n += voices[i].cloud.sounding();
        return n;
    }

    // The grains of a voice slot as they sound now (plain read; for tests: what each grain was given).
    const grn::Cloud<MAX_GRAINS>& getGrainCloud(int voiceIdx) const { return voices[std::clamp(voiceIdx, 0, POOL_VOICES - 1)].cloud; }

    // --- EngineBase overrides ---

    void prepareToPlay(double sr, int blockSize) override
    {
        currentSampleRate = sr;
        currentBlockSize = blockSize;
        router.prepare(sr);
        for (auto& v : voices)
            v.mods.prepare(sr);
    }

    void releaseResources() override
    {
        for (auto& v : voices)
        {
            v.active = false;
            v.cloud.clear();
        }
        clearPlayheads();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        int vi = allocateVoice();
        if (vi < 0) return;

        auto& v = voices[vi];
        v.active = true;
        v.dying = false;
        v.serial = ++noteSerial;
        v.note = note;
        v.velocity = velocity;
        v.gateOn = true;

        float pitchRatio = computePitchRatio(note);
        v.playbackRate = pitchRatio * (currentSampleRate > 0 ? originalSampleRate / currentSampleRate : 1.0);

        // The note's randoms: one generator a voice, advanced once a note. The router's RAND,
        // RND2 .. RND4, the RAND start offset and the grains' sequence are all draws of it.
        v.pressure = 0.0f;
        v.gen.advance();
        v.mods.noteOn(router, note, velocity, v.gen);
        v.grainSeq.start(v.gen, spread::Grain);
        v.fresh = true;         // a new note in this voice takes every glided value as it stands
        v.starting = true;      // ... and reads what is routed at its first sub-block (renderVoice)
        v.lfoRate = 1.0;
        v.lfoGain = 1.0f;
        v.envScale = 1.0f;
        v.grainPlace = 0.0f;

        // The SLCE stretch walks through every slice itself; otherwise the note plays one of them.
        // (PLAY = ONE reads the SLIC cell, which may be moved: placeNote reads it again then.)
        const bool walks = getStretchAlgo() == StretchSlice;
        v.pickedSlice = walks ? -1 : pickSlice(note);
        v.sliceByCell = ! walks && getSlicePlay() == SliceOne;

        // Where the note plays, its envelope and its filter's starting point, from the cells as
        // they are set. Whatever a slot moves of them exists at the note's first sub-block only
        // (after mod::Voice::tick): if something does, the same is worked out again there, before
        // the first sample.
        placeNote(v, false);

        // Granular: no grains yet, the first one starts with the note's first sample.
        v.cloud.reset();
        v.reverseGrains = false;

        // Time-stretch: the first cycle / slice starts with the note's first sample.
        v.cur.on = v.prev.on = false;
        v.xfade = 1.0f;
        v.spliceCount = 0;
        v.forceSplice = true;
        v.curSlice = -1;
        v.searchIdx = 0;
        v.searchValid = false;

        v.envPhase = EnvAttack;
        v.envLevel = 0.0f;
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        for (auto& v : voices)
        {
            if (v.active && ! v.dying && v.note == note && v.gateOn)
            {
                v.gateOn = false;
                // One-shot ignores the key release and plays to the end of its region.
                if (getPlayMode() != OneShot)
                    v.envPhase = EnvRelease;
            }
        }
    }

    void allNotesOff() override
    {
        soundCut();
        for (auto& v : voices)
        {
            v.active = false;
            v.envPhase = EnvOff;
            v.cloud.clear();
        }
        nextSlice = 0;      // PLAY = NEXT starts again from the first slice
        playingSlice.store(-1, std::memory_order_relaxed);
    }

    juce::String getParameterName(ParamId id) const override
    {
        static const char* names[] = {
            "Slice", "Start", "End", "LpStart", "LpEnd",
            "Pitch", "Mode", "Fine", "BitRed", "RateRed",
            "Drive", "Volume", "Pan", "Attack", "Decay", "Release",
            "V>Flt", "FltCut", "SlcCnt"
        };
        if (id >= 0 && id < 19) return names[id];
        static const char* midNames[] = {
            "Sustain", "VelSens", "KeyTrk", "Voices", "Bend", "Sample", "FltReso", "FltType", "FltMode",
            "FltEnv", "FltKey", "FltDrv", "StRand" };
        if (id >= Sustain && id <= StartRandom) return midNames[id - Sustain];
        static const char* grainNames[] = {
            "GrSize", "GrDens", "GrScan", "GrSpray", "GrPJit", "GrSprd", "GrShape", "GrRev" };
        if (id >= GrainSize && id <= GrainReverse) return grainNames[id - GrainSize];
        static const char* moreNames[] = {
            "SlcMode", "SlcSens", "SlcGap", "SlcPlay", "SlcFade", "SlcLen", "Root", "Norm",
            "StrAlgo", "StrSpd", "StrSync", "StrLen", "StrSize", "StrXfad", "StrJit", "StrSprd" };
        if (id >= SliceMode && id < NumParams) return moreNames[id - SliceMode];
        if (mod::Router::owns(id)) return mod::Router::name(id);
        return {};
    }

    int getParameterCount() const override { return NumParams; }

    // ---- Per-stage envelope curves (StageCurveParam): the one envelope is envelope slot 0 --------
    int getStageCurveParam(ParamId cellId) const override
    {
        if (cellId == Attack)  return StageCurveParam::id(0, StageCurveParam::Attack);
        if (cellId == Decay)   return StageCurveParam::id(0, StageCurveParam::Decay);
        if (cellId == Release) return StageCurveParam::id(0, StageCurveParam::Release);
        return -1;
    }

    void setParameter(ParamId id, float value) override
    {
        if (mod::Router::owns(id))
        {
            router.set(id, value);
            return;
        }
        if (StageCurveParam::owns(id))
        {
            stageCurves.set(id, value);     // read by the voices as they run
            return;
        }
        if (id < 0|| id >= (int) params.size())
            return;

        if (id == SampleSelect)
        {
            int index = std::clamp((int) std::lround(value), 0, SampleBank::get().size() - 1);
            params[(size_t) id] = (float) index;
            if (index != bankIndex)
                selectBankSample(index);
            return;
        }

        // A value outside its cell's range has no meaning, and neither has one that is not a
        // number (a hand-edited file): start points, lengths and rates are all worked out from
        // these, so they are made valid here, where they come in.
        if (! std::isfinite(value))
            return;
        if (const ParamSpec* cell = cellRanges()[(size_t) id])
            value = cell->clamp(value);

        // How the sample is read changes under the notes that are sounding (the track carries the
        // jump off; SAMP itself rebuilds, and its notes are let go).
        if ((id == Mode || id == StretchMode) && std::lround(params[(size_t) id]) != std::lround(value))
            soundSwapped();
        params[(size_t) id] = value;
        router.setBase(id, value);      // (the set value of a cell that is a destination)

        if (id == FilterType || id == FilterMode)
        {
            params[FilterType] = (float) std::clamp((int) std::lround(params[FilterType]), 0, FilterSlot::NUM_MODELS - 1);
            auto model = FilterSlot::modelFromIndex((int) params[FilterType]);
            params[FilterMode] = (float) std::clamp((int) std::lround(params[FilterMode]), 0, FilterSlot::modeCount(model) - 1);
        }

        if (id == SliceCount)
        {
            if (getSliceMode() == SliceTransient)
            {
                rebuildSlices();
                return;
            }
            int n = std::clamp((int) std::lround(value), 1, 64);
            if (n > 1) sliceByGrid(n);
            else sliceCount = 0;
        }
        else if (id == SliceMode || id == SliceSens || id == SliceGap)
        {
            if (id == SliceMode)
                params[SliceMode] = (float) std::clamp((int) std::lround(value), 0, (int) NumSliceModes - 1);
            // GRID again: the slices the SLCS cell asks for. SENS and GAP only matter in TRNS.
            if (id == SliceMode || getSliceMode() == SliceTransient)
                rebuildSlices();
        }
    }

    float getParameter(ParamId id) const override
    {
        if (mod::Router::owns(id))
            return router.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        // TRNS: the SLCS cell shows how many slices were found.
        if (id == SliceCount && getSliceMode() == SliceTransient)
            return (float) std::clamp(sliceCount, 1, MAX_GRID_SLICES);
        if (id >= 0 && id < (int) params.size())
            return params[(size_t) id];
        return 0.0f;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        int numSamples = buffer.getNumSamples();

        // Never read the sample while the message thread is replacing it.
        inProcess.store(true);
        struct Leave { std::atomic<bool>& f; ~Leave() { f.store(false); } } leave { inProcess };
        if (changingSample.load() || sampleLength <= 0)
        {
            clearPlayheads();
            return;
        }

        bendRatio = std::exp2f(bend * getParamValue(BendRange, 2.0f) / 12.0f);
        // Once per block: the mod strip (its routes are the router's: by default DRIV, RATE and
        // SPRY) and the track input.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, currentSampleRate);
        router.beginBlock(audioInput.left, audioInput.right, audioInput.connected() ? audioInput.frames : 0,
                          audioInput.envelope, wheelNow, currentSampleRate);

        float* outL = buffer.getWritePointer(0);
        float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        // What every voice starts from in this block: the cells as they are set.
        Block b;
        b.mode = getPlayMode();
        b.velSens = getParamValue(VelSens, 1.0f);
        float volume = getParamValue(Volume, 0.8f);
        b.volumeScale = getParamValue(Normalize, 0.0f) > 0.5f ? normalizeGain : 1.0f;
        if (b.volumeScale != 1.0f)
            volume *= b.volumeScale;
        b.volume = volume;
        b.pan = getParamValue(Pan, 0.0f);
        b.panL = std::cos((b.pan + 1.0f) * 0.25f * dsp::PI_F);
        b.panR = std::sin((b.pan + 1.0f) * 0.25f * dsp::PI_F);

        b.bits = (int)(getParamValue(BitReduce, 0.0f) * 12.0f);
        float rateReduce = std::min(1.0f, getParamValue(RateReduce, 0.0f));
        b.rateDiv = 1 + (int)(rateReduce * 31.0f);
        const float setDrive = getParamValue(DriveAmt, 0.0f);
        b.driveAmt = std::min(1.0f, setDrive);
        // The drive stage has gain: take most of what the strip adds to DRIV (its routes to that
        // destination, as the STRIP page has them) out again, so the strip changes the tone more
        // than the level (exactly 1 with the strip at rest, or with no route to DRIV). The rule
        // counts the strip's own routes only: a free slot from STRP to DRIV is drive as asked for.
        float stripDrive = 0.0f;
        if (wheelNow > 0.0f)
            for (int r = 0; r < mod::Router::NUM_STRIP; ++r)
                if ((int) router.get(mod::Router::stripDestId(r)) == (int) LfoDrive)
                    stripDrive += wheelNow * router.get(mod::Router::stripAmountId(r));
        const float wheelLevel = stripDrive > 0.0f
            ? std::pow((1.0f + setDrive * 10.0f) / (1.0f + std::min(1.0f, setDrive + stripDrive) * 10.0f), 0.58f) : 1.0f;

        b.stereo = !sampleR.empty();
        b.glideStep = (float) (1.0 / (0.004 * std::max(currentSampleRate, 1.0)));     // a whole range in 4 ms

        b.granular = b.mode == Granular;
        b.gs = b.granular ? readGrainSettings() : GrainSettings();
        b.stretchAlgo = getStretchAlgo();       // off in GRAN mode
        b.stretch = b.stretchAlgo != StretchOff ? readStretchSettings(b.stretchAlgo) : StretchSettings();
        // Where a note that does not loop reaches the end of its region it is faded, never cut:
        // over the slice's FADE, and at least over END_DECLICK_SECONDS (the grain and slice
        // stretches have that short fade of their own, declickEnd).
        b.endFades = ! b.granular && b.mode != Loop && b.mode != PingPong;
        b.sliceFades = b.endFades && b.stretchAlgo != StretchSlice;
        const bool ownDeclick = b.stretchAlgo == StretchGrain || b.stretchAlgo == StretchSmear || b.stretchAlgo == StretchSlice;
        b.declickSamples = b.endFades && ! ownDeclick ? END_DECLICK_SECONDS * currentSampleRate : 0.0;
        b.plain.drive = b.driveAmt;
        b.plain.volume = b.volume;
        b.plain.velSens = b.velSens;
        b.plain.bits = b.bits;
        b.plain.rateDiv = b.rateDiv;
        b.plain.grains = &b.gs;
        b.plain.stretch = &b.stretch;

        // Sub-blocks: the router's control rate. Inside one the voices are rendered one after the
        // other (they share the router's per-sample buffers), each into the mix.
        for (int offset = 0; offset < numSamples;)
        {
            const int n = router.beginChunk(offset, numSamples);
            std::fill(mixL.begin(), mixL.begin() + n, 0.0f);
            std::fill(mixR.begin(), mixR.begin() + n, 0.0f);
            for (int vi = 0; vi < POOL_VOICES; ++vi)
            {
                auto& v = voices[vi];
                if (! v.active)
                {
                    // Its LFOs keep time. (Nothing is pushed into a voice here: each sub-block
                    // reads what is moved and otherwise the set value, so nothing is to give back.)
                    if (v.mods.idle(router, n))
                        v.mods.restore(router, [](int, float) {});
                    continue;
                }
                renderVoice(v, b, offset, n);
            }
            for (int i = 0; i < n; ++i)
            {
                if (wheelLevelNow != wheelLevel)    // (worked out once per block: not jumped to)
                    wheelLevelNow = dsp::towards(wheelLevelNow, wheelLevel, b.glideStep);
                outL[offset + i] += mixL[(size_t) i] * wheelLevelNow;
                if (outR) outR[offset + i] += mixR[(size_t) i] * wheelLevelNow;
            }
            offset += n;
        }

        publishPlayheads(b.granular || b.stretchAlgo == StretchGrain || b.stretchAlgo == StretchSmear);

        bool anyActive = false;
        for (int vi = 0; vi < POOL_VOICES; ++vi)
            anyActive = anyActive || voices[vi].active;
        if (! anyActive)
            playingSlice.store(-1, std::memory_order_relaxed);
    }

private:
    float wheelLevelNow = 1.0f;     // the strip's level correction as it is heard (audio thread)

    enum EnvPhase { EnvAttack, EnvDecay, EnvSustain, EnvRelease, EnvOff };

    // A read position of the cyclic, slice and WSOL stretches (two per voice: the cycle that
    // sounds and the one it is fading out of).
    struct Head
    {
        double pos = 0.0;           // sample frames
        double limit = 0.0;         // SLCE: where the slice ends
        double dir = 1.0;           // +1 / -1
        bool on = false;
    };

    struct Voice
    {
        bool active = false;
        bool gateOn = false;
        bool dying = false;             // gave way to a new note: fading out fast, no longer counted
        uint32_t serial = 0;            // order of the notes (the oldest gives way first)
        int note = 60;
        float velocity = 1.0f;
        double position = 0.0;
        double playbackRate = 1.0;
        bool forward = true;
        int regionStart = 0;
        int regionEnd = 0;
        int loopStart = 0;
        int loopEnd = 0;
        EnvPhase envPhase = EnvOff;
        float envLevel = 0.0f;
        // For the stage curves (updateCurvedEnvelope): the straight-line level behind envLevel,
        // the envLevel written last, and both levels where the release began.
        float envLine = 0.0f, envWritten = 0.0f, relLine = 0.0f, relLevel = 0.0f;
        bool envReleasing = false;
        // The envelope's times as read at the note (seconds) and ETIM's factor on them; the
        // three rates are worked out from these (setEnvRates).
        float attackTime = 0.001f, decayTime = 0.1f, releaseTime = 0.05f, envScale = 1.0f;
        float attackRate = 0.001f;
        float decayRate = 0.001f;
        float releaseRate = 0.001f;
        float sustain = 1.0f;
        float lastSampleL = 0.0f;
        float lastSampleR = 0.0f;
        float filterCutoff = 1.0f;      // normalised 0..1 (20 Hz .. 20 kHz)
        float filterEnvAmount = 0.0f;
        bool filterOn = false;
        int filterCountdown = 0;
        FilterSlot filterL, filterR;
        float filterReso = 0.0f;        // the set resonance, and what the router adds to it
        float filterResoMod = 0.0f;
        float filterDriveNow = 0.0f;    // the filter's drive as it is heard
        bool filterDriveMoved = false;  // a slot has it, or had it, away from FDRV

        // This voice's side of the modulation router, and the note's randoms
        mod::Voice mods;
        spread::Generator gen;          // advanced once a note (the engine seeds it: setGrainSeed)
        spread::Sequence grainSeq;      // the note's grains and stretch cycles, in the order they start
        bool starting = false;          // the note has not had its first sub-block yet
        bool live = false;              // something of the router's moves this voice in this sub-block
        int pickedSlice = -1;           // the slice chosen at the note-on (-1: none)
        bool sliceByCell = false;       // ... by the SLIC cell (PLAY = ONE), which a slot may move
        float pressure = 0.0f;          // this note's pad pressure, 0..1
        double lfoRate = 1.0;           // playback-speed ratio from the router (PTCH)
        float lfoGain = 1.0f;           // gain from the router (AMP)
        // What the voice is heard with (see renderSamples): moved to, never jumped to, under a note.
        bool fresh = true;
        float volumeNow = 0.0f, velSensNow = 1.0f, driveNow = 0.0f, panLNow = 0.7071f, panRNow = 0.7071f, filterEnvNow = 0.0f;

        // Granular mode. `position` is the scan position; the cloud holds the grains that sound.
        grn::Cloud<MAX_GRAINS> cloud;
        int grainAreaStart = 0;         // what grains may read: the slice, or the whole sample
        int grainAreaEnd = 0;
        float grainPlace = 0.0f;        // where the grain started last lies in that area, 0..1 (the source GRN)
        bool reverseGrains = false;     // stretch GRAN / SMER while the note runs backwards

        double fadeFrames = 0.0;        // FADE of the slice this note plays, in sample frames (0 = none)

        // Time-stretch (CYCL, SLCE, WSOL). `position` is the time position; the heads read the sound.
        Head cur, prev;
        float xfade = 1.0f;             // 0..1 from prev to cur
        float xfadeInc = 0.0f;
        int spliceCount = 0;            // output samples until the next cycle starts
        bool forceSplice = false;       // start one now (note start, loop jump)
        int curSlice = -1;              // SLCE: the slice the time position is in
        int searchIdx = 0;              // WSOL: next candidate offset to try
        float searchBest = -2.0f;
        double searchBestD = 0.0;       // best start for the next cycle, relative to where `cur` will be
        bool searchValid = false;
    };

    struct GrainSettings;
    struct StretchSettings;
    struct Moved;
    struct Block;

    // One voice's value of a cell: the set value unless a slot moves it for this voice (as of the
    // voice's last mod::Voice::tick: `live`).
    float cellOf(const Voice& v, int id) const
    {
        if (v.live)
            if (const int d = router.dests().indexOfId(id); d > 0 && v.mods.moved(d))
                return v.mods.value(d);
        return params[(size_t) id];
    }

    // True when a slot moves one of the destinations first..last for this voice.
    static bool movedIn(const Voice& v, int first, int last)
    {
        for (int d = first; d <= last; ++d)
            if (v.mods.moved(d))
                return true;
        return false;
    }

    // Everything that is fixed when a note starts: the slice, the region and the loop inside it,
    // where the playhead begins, the envelope's times and the filter's starting point. Called at
    // the note-on with the cells as set, and once more at the note's first sub-block when the
    // router moves this voice (v.live): only there do a note-on's values of the sources exist.
    void placeNote(Voice& v, bool live)
    {
        v.live = live;
        const int note = v.note;
        int startSamp = 0, endSamp = sampleLength;

        int sliceIdx = v.pickedSlice;
        if (live && v.sliceByCell && sliceCount > 0)
            sliceIdx = std::clamp((int) (cellOf(v, SliceSelect) * sliceCount), 0, sliceCount - 1);
        v.fadeFrames = 0.0;
        if (sliceIdx >= 0 && sliceIdx < sliceCount)
        {
            startSamp = slices[sliceIdx].startSample;
            endSamp = slices[sliceIdx].endSample;
            const float keep = cellOf(v, SliceLen);
            if (keep < 1.0f)
                endSamp = startSamp + std::max(1, (int) ((float) (endSamp - startSamp) * std::max(keep, 0.0f)));
            v.fadeFrames = (double) std::clamp(cellOf(v, SliceFade), 0.0f, 1.0f) * originalSampleRate;
        }
        else
        {
            sliceIdx = -1;
        }
        playingSlice.store(sliceIdx, std::memory_order_relaxed);

        float startFrac = getParamValue(StartPoint, 0.0f);
        if (live && v.mods.moved(LfoStart))
            startFrac = std::clamp(startFrac + v.mods.value(LfoStart), 0.0f, 1.0f);
        float endFrac = cellOf(v, EndPoint);
        int rangeLen = endSamp - startSamp;

        v.regionStart = startSamp + (int)(startFrac * rangeLen);
        v.regionEnd = startSamp + (int)(endFrac * rangeLen);
        if (v.regionEnd <= v.regionStart) v.regionEnd = v.regionStart + 1;

        float loopStartFrac = cellOf(v, LoopStart);
        float loopEndFrac = cellOf(v, LoopEnd);
        int regionLen = v.regionEnd - v.regionStart;
        v.loopStart = v.regionStart + (int)(loopStartFrac * regionLen);
        v.loopEnd = v.regionStart + (int)(loopEndFrac * regionLen);
        if (v.loopEnd <= v.loopStart) v.loopEnd = v.loopStart + 1;

        PlayMode mode = getPlayMode();
        v.forward = (mode != Reverse);
        v.position = v.forward ? (double) v.regionStart : (double)(v.regionEnd - 1);

        // Grains may reach anywhere in the slice (the whole sample without slices).
        v.grainAreaStart = startSamp;
        v.grainAreaEnd = std::max(endSamp, startSamp + 1);

        v.attackTime = cellOf(v, Attack);
        v.decayTime = cellOf(v, Decay);
        v.releaseTime = cellOf(v, Release);
        v.envScale = live && v.mods.moved(LfoEnvTime) ? v.mods.value(LfoEnvTime) : 1.0f;
        setEnvRates(v);
        v.sustain = std::clamp(cellOf(v, Sustain), 0.0f, 1.0f);

        // Random start offset (up to the given fraction of the region): the note's own draw.
        float startRand = cellOf(v, StartRandom);
        if (startRand > 0.0f && v.forward)
        {
            int span = (int) ((float) (v.regionEnd - v.regionStart) * startRand * v.gen.unipolar(spread::Phase));
            v.position = (double) std::min(v.regionStart + span, v.regionEnd - 1);
        }
        // GRN before the note's first grain: where the note begins in that area (not 0: a slot that
        // reads GRN would begin every note at the area's start and move to the grains' place over 4 ms).
        v.grainPlace = (float) std::clamp((v.position - (double) v.grainAreaStart) / (double) (v.grainAreaEnd - v.grainAreaStart), 0.0, 1.0);

        // Per-voice filter. Velocity and key tracking set its starting point; the amp envelope can
        // move it while the note sounds.
        float velToFilter = cellOf(v, VelToFilter);
        float baseCutoff = getParamValue(FilterCutoff, 1.0f);
        float keyOffset = cellOf(v, FilterKey) * (float) (note - 60) / 120.0f;   // 1/10 of the range per octave
        v.filterCutoff = std::clamp(baseCutoff + velToFilter * v.velocity + keyOffset, 0.0f, 1.0f);
        v.filterEnvAmount = getParamValue(FilterEnv, 0.0f);
    }

    // The envelope's three rates from its times and ETIM's factor. (A time's factor of 1 leaves
    // the rate exactly what it was without one.)
    void setEnvRates(Voice& v) const
    {
        const float sr = (float) currentSampleRate;
        v.attackRate = 1.0f / std::max(v.attackTime * v.envScale * sr, 1.0f);
        v.decayRate = 1.0f / std::max(v.decayTime * v.envScale * sr, 1.0f);
        v.releaseRate = 1.0f / std::max(v.releaseTime * v.envScale * sr, 1.0f);
    }

    // The note's filter, at its first sub-block (what is routed is known there).
    void startFilter(Voice& v)
    {
        auto model = FilterSlot::modelFromIndex((int) getParamValue(FilterType, 0.0f));
        v.filterOn = model != FilterModel::Bypass
                     && (v.filterCutoff < 0.999f || v.filterEnvAmount != 0.0f
                         || router.routed(LfoCutoff) || router.routed(LfoResonance)
                         || router.routed(LfoFilterEnv) || router.routed(LfoFilterDrive));
        v.filterDriveMoved = false;
        v.filterDriveNow = getParamValue(FilterDrive, 0.0f);
        v.filterReso = getParamValue(FilterReso, 0.0f);
        v.filterResoMod = 0.0f;
        if (v.filterOn)
        {
            for (auto* f : { &v.filterL, &v.filterR })
            {
                f->setSampleRate(currentSampleRate);
                f->setModel(0, model);
                f->setMode(0, (int) getParamValue(FilterMode, 0.0f));
                f->setResonance(getParamValue(FilterReso, 0.0f));
                f->setDrive(getParamValue(FilterDrive, 0.0f));
                f->reset();
            }
            v.filterCountdown = 0;
        }
    }

    // One sub-block of a voice that sounds, added to the mix. A note's first sample is a run of
    // its own: the router's values for it are those of that very sample (an LFO that starts with
    // the key is read where it starts), and what is read once at the note is read from them.
    // (The rest of that sub-block is a second run of the voice, whose per-sample buffers the
    // router fills from the sub-block's start: for those fifteen samples of a note an audio-rate
    // source - the track input - is heard one sample late, and the sub-block's last input sample
    // is not heard. Measured: the probe of "U4 Sampler", NEPTUNE_U4_REVIEW_PROBE.)
    void renderVoice(Voice& v, Block& b, int offset, int n)
    {
        if (! v.starting)
        {
            renderRun(v, b, offset, 0, n);
            return;
        }
        renderRun(v, b, offset, 0, 1);
        if (n > 1 && v.active)
            renderRun(v, b, offset + 1, 1, n - 1);
    }

    // n samples of a voice: the sample `at` of the block, the sample `into` of the sub-block's mix.
    void renderRun(Voice& v, Block& b, int at, int into, int n)
    {
        // The voice's own sources, worked out only while a slot reads them.
        if (router.wants(PubPos))
            v.mods.publish(PubPos, (float) std::clamp((v.position - (double) v.regionStart)
                                                      / (double) std::max(1, v.regionEnd - v.regionStart), 0.0, 1.0));
        if (router.wants(PubGrain))
            v.mods.publish(PubGrain, v.grainPlace);
        if (router.wants(PubLevel))
            v.mods.publish(PubLevel, levelAt(v.position));

        // (The filter follows the one envelope: a slot's FENV is ENV here.)
        v.live = v.mods.tick(router, n, v.envLevel, v.envLevel, v.pressure);
        const bool first = v.starting;
        if (v.starting)
        {
            // The note's first sample: what a slot moves of the things read once is read now.
            v.starting = false;
            if (v.live)
                placeNote(v, true);
            startFilter(v);
        }
        if (! v.live && ! v.filterDriveMoved)
        {
            // Nothing moves this voice: the sampler as it is without any modulation. (A filter
            // drive that a slot has just let go of is first brought back to its cell, below.)
            v.lfoRate = 1.0;
            v.lfoGain = 1.0f;
            renderSamples<false>(v, b, b.plain, at, into, n);
            return;
        }

        const mod::Voice& mods = v.mods;
        Moved m = b.plain;
        m.pitch = mods.buffer(LfoPitch);
        m.amp = mods.buffer(LfoAmp);
        m.pan = mods.buffer(LfoPan);
        m.gpos = mods.buffer(LfoGrainPos);
        if (m.pitch == nullptr) v.lfoRate = 1.0;
        if (m.amp == nullptr)   v.lfoGain = 1.0f;

        // Slots on page cells: this voice's own value of each.
        m.bits = (int) (cellOf(v, BitReduce) * 12.0f);
        m.rateDiv = 1 + (int) (std::min(1.0f, cellOf(v, RateReduce)) * 31.0f);
        m.volume = cellOf(v, Volume) * b.volumeScale;
        m.velSens = cellOf(v, VelSens);
        m.drive = std::min(1.0f, cellOf(v, DriveAmt));
        if (mods.moved(LfoGrainSize))   { m.size = mods.value(LfoGrainSize); m.sizeMoved = true; }
        if (mods.moved(LfoGrainDens))   { m.density = mods.value(LfoGrainDens); m.densityMoved = true; }
        if (mods.moved(LfoSpeed))       m.speed = mods.value(LfoSpeed);
        if (b.granular)
        {
            if (movedIn(v, LfoScan, LfoReverse))
            {
                b.ownGrains = readGrainSettings(&v);
                m.grains = &b.ownGrains;
            }
        }
        else if (b.stretchAlgo != StretchOff && (movedIn(v, LfoStretchSize, LfoStretchSpread) || movedIn(v, LfoFade, LfoSliceLen)))
        {
            b.ownStretch = readStretchSettings(b.stretchAlgo, &v);
            m.stretch = &b.ownStretch;
        }

        // ETIM: the envelope's times under the sounding note; the stage goes on from where it is.
        // (A note that is giving way keeps the fast release it was given.)
        const float scale = mods.moved(LfoEnvTime) ? mods.value(LfoEnvTime) : 1.0f;
        if (scale != v.envScale && ! v.dying)
        {
            v.envScale = scale;
            setEnvRates(v);
        }

        if (v.filterOn)
        {
            m.cut = mods.buffer(LfoCutoff);
            const float resoMod = mods.moved(LfoResonance) ? mods.value(LfoResonance) : 0.0f;
            if (resoMod != v.filterResoMod)
            {
                v.filterResoMod = resoMod;
                const float r = std::clamp(v.filterReso + resoMod, 0.0f, 1.0f);
                v.filterL.setResonance(r);
                v.filterR.setResonance(r);
            }
            // FDRV: the filter's drive is moved to sample by sample (renderSamples), never set in
            // steps of a sub-block; a note takes it as it stands, and a slot that lets go of it
            // under a sounding note leaves it to come back to the cell the same way.
            if (mods.moved(LfoFilterDrive))
            {
                m.filterDrive = mods.value(LfoFilterDrive);
                v.filterDriveMoved = true;
                if (first)
                {
                    v.filterDriveNow = m.filterDrive;
                    v.filterL.setDrive(m.filterDrive);
                    v.filterR.setDrive(m.filterDrive);
                }
            }
            else if (v.filterDriveMoved)
            {
                m.filterDrive = getParamValue(FilterDrive, 0.0f);
                if (v.filterDriveNow == m.filterDrive)
                    v.filterDriveMoved = false;
            }
            m.filterDriveMoves = v.filterDriveMoved && v.filterDriveNow != m.filterDrive;
            m.envMoved = mods.moved(LfoFilterEnv);
            m.envAmount = mods.value(LfoFilterEnv);
        }
        renderSamples<true>(v, b, m, at, into, n);
    }

    // The n samples of a voice's sub-block. Mod = false is the plain path: nothing of the router's
    // is read (m is the block's own, its buffers empty), and the loop is what it was before the
    // router existed.
    template <bool Mod>
    void renderSamples(Voice& v, const Block& b, Moved m, int at, int into, int n)
    {
        const PlayMode mode = b.mode;
        const float glideStep = b.glideStep, volumeScale = b.volumeScale;
        const bool lfoFilterEnv = Mod && m.envMoved;
        for (int i = 0; i < n; ++i)
        {
            float voiceDrive = m.drive;
            float voicePanL = b.panL, voicePanR = b.panR;
            float lfoCutOct = 0.0f;
            bool lfoFilter = false;
            float voiceVolume = m.volume, voiceVelSens = m.velSens;
            if (Mod)
            {
                // What the router has per sample for this voice: pitch (a ratio), level (a gain),
                // pan (added to PAN), cutoff (octaves), the grains' place.
                if (m.pitch != nullptr) v.lfoRate = (double) m.pitch[i];
                if (m.amp != nullptr)   v.lfoGain = m.amp[i];
                if (m.pan != nullptr)
                {
                    const float p = std::clamp(b.pan + m.pan[i], -1.0f, 1.0f);
                    voicePanL = std::cos((p + 1.0f) * 0.25f * dsp::PI_F);
                    voicePanR = std::sin((p + 1.0f) * 0.25f * dsp::PI_F);
                }
                if (m.cut != nullptr)
                {
                    lfoFilter = true;
                    lfoCutOct = m.cut[i];
                }
                if (m.gpos != nullptr)  m.gposNow = m.gpos[i];
            }

            // Level, velocity's share, pan, drive and the filter envelope's amount: a note takes
            // them as they stand at its first sample; under a sounding note (a step lock, a
            // knob, the strip) they are moved to over a few milliseconds, never jumped to.
            if (v.fresh)
            {
                v.fresh = false;
                v.volumeNow = voiceVolume;
                v.velSensNow = voiceVelSens;
                v.driveNow = voiceDrive;
                v.panLNow = voicePanL;
                v.panRNow = voicePanR;
                v.filterEnvNow = lfoFilterEnv ? m.envAmount : v.filterEnvAmount;
            }
            else
            {
                if (v.volumeNow != voiceVolume)   v.volumeNow = dsp::towards(v.volumeNow, voiceVolume, glideStep * std::max(1.0f, volumeScale));
                if (v.velSensNow != voiceVelSens) v.velSensNow = dsp::towards(v.velSensNow, voiceVelSens, glideStep);
                if (v.driveNow != voiceDrive)     v.driveNow = dsp::towards(v.driveNow, voiceDrive, glideStep);
                if (v.panLNow != voicePanL)       v.panLNow = dsp::towards(v.panLNow, voicePanL, glideStep);
                if (v.panRNow != voicePanR)       v.panRNow = dsp::towards(v.panRNow, voicePanR, glideStep);
            }
            voiceVolume = v.volumeNow;
            voiceVelSens = v.velSensNow;
            voiceDrive = v.driveNow;
            voicePanL = v.panLNow;
            voicePanR = v.panRNow;

            float sampL, sampR;
            const double positionBefore = v.position;
            if (b.granular)
            {
                processGrains(v, *m.grains, m, b.stereo, sampL, sampR);
            }
            else if (b.stretchAlgo != StretchOff)
            {
                processStretch(v, mode, *m.stretch, m, b.stereo, sampL, sampR);
            }
            else
            {
                readVoice(v, mode, b.stereo, sampL, sampR);
                advancePosition(v, mode);
            }

            // The end of the region fades out (not while it loops): FADE for a slice, and the
            // short declick, measured in output samples at the speed the note moves at.
            if (b.endFades)
            {
                double fade = b.declickSamples * std::abs(v.position - positionBefore);
                if (b.sliceFades)
                    fade = std::max(fade, v.fadeFrames);
                const double left = v.forward ? (double) v.regionEnd - v.position : v.position - (double) v.regionStart;
                if (left < fade)
                {
                    const float g = (float) std::max(0.0, left / fade);
                    sampL *= g;
                    sampR *= g;
                }
            }
            updateCurvedEnvelope(v, mode);

            if (!v.active) break;

            float gain = v.envLevel * (1.0f - voiceVelSens + voiceVelSens * v.velocity) * voiceVolume * v.lfoGain;
            sampL *= gain;
            sampR *= gain;

            // Per-voice filter; its cutoff is refreshed every 16 samples (every sample while a
            // slot moves it, in octaves: the 0..1 cutoff range is log2(1000) octaves wide).
            if (v.filterOn)
            {
                const float envTarget = lfoFilterEnv ? m.envAmount : v.filterEnvAmount;
                const bool envGlides = v.filterEnvNow != envTarget;
                if (envGlides)      // 8 octaves in 4 ms at most: a filter shut in one sample is a click
                    v.filterEnvNow = dsp::towards(v.filterEnvNow, envTarget, 0.8f * glideStep);
                if (--v.filterCountdown <= 0 || lfoFilter || envGlides)
                {
                    v.filterCountdown = 16;
                    const float envAmount = v.filterEnvNow;
                    float norm = std::clamp(v.filterCutoff + envAmount * v.envLevel, 0.0f, 1.0f);
                    if (lfoCutOct != 0.0f)
                        norm = std::clamp(norm + lfoCutOct / 9.9657843f, 0.0f, 1.0f);
                    float hz = 20.0f * std::pow(1000.0f, norm);
                    v.filterL.setCutoff(hz);
                    v.filterR.setCutoff(hz);
                }
                if (Mod && m.filterDriveMoves && v.filterDriveNow != m.filterDrive)
                {
                    v.filterDriveNow = dsp::towards(v.filterDriveNow, m.filterDrive, glideStep);
                    v.filterL.setDrive(v.filterDriveNow);
                    v.filterR.setDrive(v.filterDriveNow);
                }
                sampL = v.filterL.process(sampL);
                sampR = v.filterR.process(sampR);
            }

            if (m.bits > 0)
            {
                sampL = applyBitReduction(sampL, m.bits);
                sampR = applyBitReduction(sampR, m.bits);
            }

            if (m.rateDiv > 1 && ((at + i) % m.rateDiv) != 0)
            {
                sampL = v.lastSampleL;
                sampR = v.lastSampleR;
            }
            else
            {
                v.lastSampleL = sampL;
                v.lastSampleR = sampR;
            }

            if (voiceDrive > 0.01f)
            {
                sampL = applySoftClip(sampL, voiceDrive);
                sampR = applySoftClip(sampR, voiceDrive);
            }

            // For stereo samples, pan each channel independently
            // For mono samples (sampR == sampL), this still works correctly
            mixL[(size_t) (into + i)] += sampL * voicePanL;
            mixR[(size_t) (into + i)] += sampR * voicePanR;
        }
    }

    // The slot for a new note. At most VOX notes sound; when a new one would make it more, one
    // gives way: the quietest of those whose key has been let go, else the oldest. It is not cut
    // (a step in the sound: a click on every new note under a long release) but fades out over
    // STEAL_FADE_SECONDS in its own slot while the new note starts in a free one - the pool has
    // twice as many slots as VOX can ask for.
    int allocateVoice()
    {
        const int limit = std::clamp((int) std::lround(getParamValue(Voices, (float) numVoices)), 1, numVoices);

        int sounding = 0;
        for (auto& v : voices)
            if (v.active && ! v.dying)
                ++sounding;

        const float fadeRate = 1.0f / std::max(STEAL_FADE_SECONDS * (float) currentSampleRate, 1.0f);
        for (; sounding >= limit; --sounding)
        {
            int victim = -1;
            for (int i = 0; i < POOL_VOICES; ++i)
            {
                const auto& v = voices[i];
                if (! v.active || v.dying)
                    continue;
                if (victim < 0) { victim = i; continue; }
                const auto& best = voices[victim];
                const bool better = v.gateOn != best.gateOn ? ! v.gateOn
                                  : ! v.gateOn ? v.envLevel < best.envLevel
                                               : (int32_t) (v.serial - best.serial) < 0;
                if (better)
                    victim = i;
            }
            if (victim < 0)
                break;
            auto& v = voices[victim];
            v.dying = true;
            v.gateOn = false;
            v.envPhase = EnvRelease;
            v.releaseRate = std::max(v.releaseRate, fadeRate);
        }

        for (int i = 0; i < POOL_VOICES; ++i)
            if (! voices[i].active) return i;

        // Every slot in use (more new notes within a few milliseconds than there are spare
        // slots): the fading note that has got furthest is cut.
        int quietest = -1;
        for (int i = 0; i < POOL_VOICES; ++i)
            if (voices[i].dying && (quietest < 0 || voices[i].envLevel < voices[quietest].envLevel))
                quietest = i;
        return std::max(quietest, 0);
    }

    // MODE follows the sample until it is set by hand: when the sample changes and MODE is still
    // what the old sample suggested, it becomes what the new one suggests (the tone loops, a hit
    // plays once). A mode chosen differently - GRAN, REV, LOOP on a hit - is kept. Message thread.
    void followSampleMode(PlayMode suggestedBefore)
    {
        if (getPlayMode() == suggestedBefore)
            params[Mode] = (float) suggestedMode();
    }

    float getParamValue(int id, float defaultVal) const
    {
        // Every parameter is seeded with its default at construction, so the stored value is the
        // value (zero included).
        if (id >= 0 && id < (int) params.size())
            return params[(size_t) id];
        return defaultVal;
    }

    int getActiveSlice() const
    {
        if (sliceCount <= 0) return -1;
        return std::clamp((int) (getParamValue(SliceSelect, 0.0f) * sliceCount), 0, sliceCount - 1);
    }

    int getSliceMode() const { return std::clamp((int) std::lround(getParamValue(SliceMode, 0.0f)), 0, (int) NumSliceModes - 1); }
    int getSlicePlay() const { return std::clamp((int) std::lround(getParamValue(SlicePlay, 0.0f)), 0, (int) NumSlicePlayModes - 1); }
    int getRootNote() const { return std::clamp((int) std::lround(getParamValue(RootNote, 60.0f)), 0, 127); }

    // The stretch algorithm in use: none in GRAN mode, which has its own time control (SCAN).
    int getStretchAlgo() const
    {
        if (getPlayMode() == Granular) return StretchOff;
        return std::clamp((int) std::lround(getParamValue(StretchMode, 0.0f)), 0, (int) NumStretchAlgos - 1);
    }

    // Which slice a new note plays (PLAY cell); -1 = the whole sample. Audio thread.
    int pickSlice(int note)
    {
        const int count = sliceCount;
        if (count <= 0) return -1;
        switch (getSlicePlay())
        {
            case SliceKeys:   return std::clamp(note - getRootNote(), 0, count - 1);
            case SliceNext:
            {
                const int s = nextSlice >= 0 && nextSlice < count ? nextSlice : 0;
                nextSlice = (s + 1) % count;
                return s;
            }
            case SliceRandom:
                sliceGen.advance();
                return std::min((int) (sliceGen.unipolar(spread::Slice) * (float) count), count - 1);
            default:          return getActiveSlice();
        }
    }

    PlayMode getPlayMode() const
    {
        int m = (int) getParamValue(Mode, 0.0f);
        return (PlayMode) std::clamp(m, 0, (int) NumPlayModes - 1);
    }

    float computePitchRatio(int note) const
    {
        float semi = getParamValue(PitchSemi, 0.0f);
        float fine = getParamValue(FineTune, 0.0f);
        float keyed = getParamValue(KeyTrack, 1.0f) > 0.5f ? (float)(note - getRootNote()) : 0.0f;
        // KEYS: the keys choose slices, every slice at the sample's own pitch.
        if (sliceCount > 0 && getSlicePlay() == SliceKeys && getStretchAlgo() != StretchSlice)
            keyed = 0.0f;
        float totalSemi = keyed + semi + fine;
        return std::exp2f(totalSemi / 12.0f);
    }

    // The classic read of a voice. In LOOP (with the loop ending where the region ends, as it
    // does unless LPEN is moved) the frame after the last one is the first of the loop, so a
    // sample that repeats seamlessly is read seamlessly across the join.
    void readVoice(const Voice& v, PlayMode mode, bool stereo, float& outL, float& outR) const
    {
        const int idx0 = (int) v.position;
        if (mode == Loop && v.forward && v.loopEnd == v.regionEnd && idx0 + 1 >= v.regionEnd
            && idx0 >= 0 && idx0 < sampleLength && v.loopStart >= 0 && v.loopStart < sampleLength)
        {
            const float frac = (float) (v.position - (double) idx0);
            const size_t a = (size_t) idx0, b = (size_t) v.loopStart;
            outL = sampleL[a] + frac * (sampleL[b] - sampleL[a]);
            outR = stereo ? sampleR[a] + frac * (sampleR[b] - sampleR[a]) : outL;
            return;
        }
        readSampleStereoInterp(v.position, stereo, outL, outR);
    }

    // Stereo interpolated sample read
    void readSampleStereoInterp(double pos, bool stereo, float& outL, float& outR) const
    {
        if (sampleLength <= 0)
        {
            outL = outR = 0.0f;
            return;
        }
        // (a position far outside the sample, or not a number, is held before the cast: beyond the
        // range of an int the cast is undefined)
        if (! (pos >= -1.0e9 && pos <= 1.0e9))
            pos = pos > 0.0 ? 1.0e9 : -1.0e9;
        int idx0 = (int) pos;
        int idx1 = idx0 + 1;
        float frac = (float)(pos - idx0);

        idx0 = std::clamp(idx0, 0, sampleLength - 1);
        idx1 = std::clamp(idx1, 0, sampleLength - 1);

        float s0L = sampleL[(size_t) idx0];
        float s1L = sampleL[(size_t) idx1];
        outL = s0L + frac * (s1L - s0L);

        if (stereo)
        {
            float s0R = sampleR[(size_t) idx0];
            float s1R = sampleR[(size_t) idx1];
            outR = s0R + frac * (s1R - s0R);
        }
        else
        {
            outR = outL;
        }
    }


    // -------------------------------------------------------------------------------------------
    // Granular play mode (MODE = GRAN)
    //
    // The textbook granular scheme of Curtis Roads, "Microsound" (MIT Press, 2001), ch. 3 and 5
    // (granulation of a stored sound): short windowed grains are read from the sample with the
    // sampler's own interpolated read and overlap-added. Quasi-synchronous while SPRY = 0 (grains
    // start at regular intervals of 1 / DENS), asynchronous as SPRY is raised (the intervals are
    // scattered as well as the positions). Nothing new at the signal level: standard windows
    // (Hann, triangle, Roads's "expodec" / "rexpodec"), linear-interpolated reads, a sum.
    //
    // Time and pitch are separate:
    //   * the scan position (the voice's `position`) moves through STRT..END at SCAN x the sample's
    //     original speed, whatever note is played; 0 = frozen. It starts where a classic note would
    //     (STRT, slice, RAND, the STRT LFO destination) and WRAPS: past END it continues from STRT,
    //     and with a negative SCAN past STRT it continues from END.
    //   * every grain reads at the note's pitch (PTCH, FINE, KEY, bend and the PTCH LFO, which keep
    //     acting on grains that are already sounding) times its own PJIT ratio.
    //
    // STRT..END bounds the scan position only. Grains are placed around it and may read anywhere
    // in the "grain area": the slice the note plays, or the whole sample when there are no slices.
    // So a frozen position (SCAN 0, set with STRT) has sample on both sides for SPRY and GPOS.
    //
    // A grain: starts at scan position (+ GPOS LFO x the length of STRT..END, clamped to the area)
    //          + SPRY x (+-250 ms of the sample, at most the area; reflected at its ends);
    //          it is then moved back, if needed, so that all of it lies inside the area (a grain
    //          longer than the area wraps inside it instead). A reversed grain (RVRS = the
    //          probability) covers the same stretch from its end to its start.
    //          PJIT: ratio 2^(PJIT^2 x u), u uniform in -1..1, i.e. up to +-12 semitones with the
    //          amount squared (10 % = +-12 cents, 30 % = +-1 semitone, 50 % = +-3).
    //          SPRD: equal-power position SPRD x u around the centre (centre = gain 1 on both
    //          sides, so SPRD = 0 is exactly the mono path; PAN and the PAN LFO act afterwards).
    //
    // Overlap and loudness. overlap = SIZE (s) x DENS (grains/s) = the mean number of grains
    // sounding at once (below 1 there are gaps between grains). Each grain is scaled by
    // 1 / sqrt(max(1, min(overlap, MAX_GRAINS))): uncorrelated grains add in power, so the level
    // of a cloud stays the same whatever SIZE and DENS are (about -4 dB re. the sample with Hann
    // grains). Grains that line up exactly (SPRY and PJIT at 0 with SCAN at the played speed, or a
    // frozen periodic sound) add in amplitude instead and come out at 0.5 x sqrt(overlap): unity
    // at the default overlap of 4, +6 dB at 16. Sparse grains (overlap < 1) play at full level.
    // Overlap above MAX_GRAINS cannot be reached: the surplus grains are skipped.
    //
    // The clock, the pool and the grains are grn::Cloud (dsp/GrainScheduler.h: the same arithmetic,
    // shared with the resonator's exciter); the engine gives it the position, the area and the
    // read. A grain's five random numbers (place, pitch, pan, reverse, the next interval) are the
    // next draws of the note's spread::Sequence (stream Grain), so a seed repeats a cloud.
    //
    // Real time: fixed grain pool in the voice, no allocation, no locks; every length is in seconds
    // or in sample frames scaled by the two sample rates. All reads go through
    // readSampleStereoInterp, which clamps to the current sample, and the block is not rendered at
    // all while the sample is being changed (changingSample), which also empties every grain pool.
    // -------------------------------------------------------------------------------------------

    struct GrainSettings
    {
        // size (seconds), density (grains per second), SPRY as both the jitter of the intervals
        // and the scatter of the places (scatterFrames = frames at SPRY 1), PJIT in octaves at
        // u = 1 (already squared), SPRD, RVRS, SHAP
        grn::Spec spec;
        double scanInc = 0.0;       // sample frames per output sample
        double invSampleRate = 1.0 / 44100.0;
    };

    static constexpr float MIN_GRAIN_SIZE = 0.005f, MAX_GRAIN_SIZE = 0.5f;
    static constexpr float MIN_GRAIN_DENSITY = 1.0f, MAX_GRAIN_DENSITY = 200.0f;

    // voice: for one voice, when a slot moves SCAN, SPRY, PJIT, SPRD or RVRS of it.
    GrainSettings readGrainSettings(const Voice* voice = nullptr) const
    {
        auto P = [this, voice](int id, float fallback)
        {
            return voice != nullptr ? cellOf(*voice, id) : getParamValue(id, fallback);
        };
        GrainSettings g;
        auto& s = g.spec;
        const double sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        g.invSampleRate = 1.0 / sr;
        s.size = std::clamp(getParamValue(GrainSize, 0.1f), MIN_GRAIN_SIZE, MAX_GRAIN_SIZE);
        s.density = std::clamp(getParamValue(GrainDensity, 40.0f), MIN_GRAIN_DENSITY, MAX_GRAIN_DENSITY);
        g.scanInc = (double) std::clamp(P(GrainScan, 1.0f), -2.0f, 2.0f) * originalSampleRate / sr;
        // STRCH SYNC in GRAN mode: SCAN 1.00 = STRT..END lasts LEN at the tempo.
        if (getParamValue(StretchSync, 0.0f) > 0.5f)
            g.scanInc *= syncedSpeed();
        s.jitter = s.scatter = std::clamp(P(GrainSpray, 0.0f), 0.0f, 1.0f);
        s.scatterFrames = GRAIN_SPRAY_SECONDS * (float) originalSampleRate;
        const float pj = std::clamp(P(GrainPitchJit, 0.0f), 0.0f, 1.0f);
        s.pitchJitter = pj * pj * GRAIN_PITCH_JITTER_SEMITONES / 12.0f;
        s.spread = std::clamp(P(GrainSpread, 0.0f), 0.0f, 1.0f);
        s.reverse = std::clamp(P(GrainReverse, 0.0f), 0.0f, 1.0f);
        s.window = std::clamp((int) std::lround(getParamValue(GrainShape, 0.0f)), 0, (int) NumGrainWindows - 1);
        return g;
    }

    // One output sample of a granular voice: runs the grain clock, sums the grains, moves the scan.
    void processGrains(Voice& v, const GrainSettings& gs, const Moved& m, bool stereo, float& outL, float& outR)
    {
        processGrainCloud(v, gs, m, stereo, outL, outR);

        const double rStart = (double) v.regionStart, rEnd = (double) v.regionEnd, rLen = rEnd - rStart;
        if (gs.scanInc != 0.0)
        {
            // The SPD destination scales the scan speed.
            if (m.speed != 1.0f)
                v.position += gs.scanInc * (double) m.speed;
            else
                v.position += gs.scanInc;
            if (v.position >= rEnd || v.position < rStart)
            {
                double off = std::fmod(v.position - rStart, rLen);
                if (off < 0.0) off += rLen;
                v.position = rStart + off;
            }
        }
    }

    // The grain clock and the sum of the grains around the voice's `position` (which the caller
    // moves): GRAN mode, and the GRAN and SMER stretches of the other modes.
    void processGrainCloud(Voice& v, const GrainSettings& gs, const Moved& m, bool stereo, float& outL, float& outR)
    {
        const double baseInc = v.playbackRate * (double) bendRatio * v.lfoRate;

        // SIZE and DENS as the router has them for this voice (factors on the cells).
        float size = gs.spec.size, density = gs.spec.density;
        if (m.sizeMoved)
            size = std::clamp(size * m.size, MIN_GRAIN_SIZE, MAX_GRAIN_SIZE);
        if (m.densityMoved)
            density = std::clamp(density * m.density, MIN_GRAIN_DENSITY, MAX_GRAIN_DENSITY);

        const grn::Area area { (double) v.grainAreaStart, (double) v.grainAreaEnd };
        if (v.cloud.due(density, gs.invSampleRate))
            startGrain(v, gs, m, size, density, baseInc, area);
        v.cloud.sum([this, stereo](double pos, float& l, float& r) { readSampleStereoInterp(pos, stereo, l, r); },
                    baseInc, area, outL, outR);
    }

    void startGrain(Voice& v, const GrainSettings& gs, const Moved& m, float size, float density, double baseInc, const grn::Area& area)
    {
        grn::Spec s = gs.spec;
        s.size = size;
        s.density = density;
        s.backwards = v.reverseGrains;      // a note running backwards: the grain ends at the position

        // GPOS: the place as the router has it at this very sample.
        double p = v.position;
        if (m.gpos != nullptr)
            p = std::clamp(p + (double) m.gposNow * (double) (v.regionEnd - v.regionStart), area.start, area.end - 1.0);

        // The grain's random numbers: the next draws of the note's sequence, taken whether or not
        // the grain is used, so a full pool does not change what the following grains get.
        grn::Cloud<MAX_GRAINS>::Draws d;
        d.pos = v.grainSeq.bipolar();
        d.pitch = v.grainSeq.bipolar();
        d.pan = v.grainSeq.bipolar();
        d.reverse = v.grainSeq.unipolar();
        if (s.jitter > 0.0f)
            d.interval = v.grainSeq.bipolar();
        if (const grn::Grain* g = v.cloud.start(s, p, area, baseInc, currentSampleRate, d))
            v.grainPlace = (float) std::clamp((g->pos - area.start) / std::max(1.0, area.end - area.start), 0.0, 1.0);
    }

    // -------------------------------------------------------------------------------------------
    // Transient slicing (SLICE page, MODE = TRNS)
    //
    // Onset detection in the time domain, after J. P. Bello et al., "A Tutorial on Onset Detection
    // in Music Signals", IEEE Trans. Speech and Audio Processing 13(5), 2005:
    //   * detection function (sec. III-A, temporal features): the half-wave rectified rise of the
    //     LOG of the local energy - A. Klapuri's relative difference function ("Sound onset
    //     detection by applying psychoacoustic knowledge", ICASSP 1999) - taken for the signal and
    //     for its first difference. Differencing weights the energy by frequency squared, the
    //     time-domain form of the high-frequency content measure (P. Masri, 1996), so a hi-hat over
    //     a held bass note still shows. The larger of the two rises is used.
    //   * peak picking (sec. IV): a local maximum above the adaptive threshold
    //     delta + median(neighbourhood), delta set by SENS, and at least GAP after the last onset.
    // Energy is the mean square over 8 hops of 2.9 ms (23 ms), read every hop; a rise is measured
    // over two hops. SENS also sets the level below which nothing counts (-20 dB re. the sample's
    // peak at 0, -60 dB at 100 %), so quiet hits appear as it is raised.
    // Each onset is then placed on the sample where the level first rises out of what came before,
    // and moved back to the nearest zero crossing within 2 ms so the slice starts without a click.
    // A slice ends where the next begins; silence before the first onset is not a slice.
    //
    // Threads. The per-hop energies are worked out when the sample changes (message thread; the
    // only step that allocates). Picking the onsets again when SENS, GAP or SLCS changes reads
    // those tables and allocates nothing; the new slices are handed to the audio thread with the
    // sample-change handshake (changingSample / inProcess), without stopping the voices.
    // -------------------------------------------------------------------------------------------

    static constexpr int ONSET_WINDOW_HOPS = 8;
    static constexpr int ONSET_LAG = 2;
    static constexpr int ONSET_MEDIAN_HALF = 34;        // +-100 ms
    static constexpr int MAX_ONSET_CANDIDATES = 256;

    float monoAt(int i) const
    {
        if (i < 0 || i >= sampleLength) return 0.0f;
        return sampleR.empty() ? sampleL[(size_t) i] : 0.5f * (sampleL[(size_t) i] + sampleR[(size_t) i]);
    }

    // Message thread, after every sample change.
    void sampleChanged()
    {
        nextSlice = 0;
        if (getSliceMode() == SliceTransient)
            rebuildSlices();
    }

    // The sample's peak and its level per hop (message thread, after the sample-change handshake,
    // as before the port: the analysis of a long sample takes tens of milliseconds and the track
    // must not be kept silent that long). The audio thread reads only the level table and its hop
    // (the source SLEV): they are made aside and handed over with a short handshake of their own.
    void analyseSample()
    {
        std::vector<float> levelTable;      // (made here; swapped in at the end)
        float peak = 0.0f;
        for (float x : sampleL) peak = std::max(peak, std::abs(x));
        for (float x : sampleR) peak = std::max(peak, std::abs(x));
        normalizeGain = peak > 1.0e-4f ? std::min(1.0f / peak, 100.0f) : 1.0f;
        analyseOnsets();

        // SLEV's table, one value a hop: the root of the mean square over the ONSET_WINDOW_HOPS hops
        // round it (23 ms - one hop of 2.9 ms follows the waveform of a low note, not its level:
        // 15 .. 100 % a hundred and ten times a second on a steady 55 Hz tone), as a fraction of
        // the loudest such stretch. A sample shorter than a hop has one value: its own level.
        const int hops = (int) onsetEnergy.size();
        levelTable.assign((size_t) std::max(1, hops), 0.0f);
        if (hops == 0)
        {
            double e = 0.0;
            for (int i = 0; i < sampleLength; ++i) e += (double) monoAt(i) * (double) monoAt(i);
            levelTable[0] = e > 1.0e-12 * (double) std::max(1, sampleLength) ? 1.0f : 0.0f;
            publishLevelTable(levelTable);
            return;
        }
        const int before = (ONSET_WINDOW_HOPS - 1) / 2, after = ONSET_WINDOW_HOPS / 2;
        double sum = 0.0, loudest = 0.0;
        for (int h = 0; h < std::min(hops, after); ++h) sum += (double) onsetEnergy[(size_t) h];
        for (int h = 0; h < hops; ++h)
        {
            if (h + after < hops)       sum += (double) onsetEnergy[(size_t) (h + after)];
            if (h - before - 1 >= 0)    sum -= (double) onsetEnergy[(size_t) (h - before - 1)];
            const int count = std::min(hops - 1, h + after) - std::max(0, h - before) + 1;
            const double mean = std::max(0.0, sum) / (double) count;
            levelTable[(size_t) h] = (float) mean;
            loudest = std::max(loudest, mean);
        }
        for (float& x : levelTable)
            x = loudest > 1.0e-12 ? (float) std::sqrt((double) x / loudest) : 0.0f;
        publishLevelTable(levelTable);
    }

    // Hands a new level table to the audio thread; the old one is freed here, on this thread.
    void publishLevelTable(std::vector<float>& table)
    {
        changingSample.store(true);
        for (int spins = 0; inProcess.load() && spins < 2000; ++spins)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        this->levelTable.swap(table);
        levelHop = onsetHop;
        changingSample.store(false);
    }

    // SLEV: the sample's level where a voice's playhead is, 0..1 of its loudest hop. Audio thread.
    float levelAt(double position) const
    {
        if (levelTable.empty())
            return 0.0f;
        const int hop = (int) (position >= 0.0 ? std::min(position, 1.0e9) : 0.0) / std::max(1, levelHop);
        return levelTable[(size_t) std::min(hop, (int) levelTable.size() - 1)];     // (the last, shorter hop: the one before it)
    }

    void analyseOnsets()
    {
        onsetHop = std::max(16, (int) std::lround(originalSampleRate * 0.0029));
        const int blocks = sampleLength / onsetHop;
        onsetEnergy.assign((size_t) std::max(0, blocks), 0.0f);
        onsetEnergyHigh.assign((size_t) std::max(0, blocks), 0.0f);
        onsetScratch.assign((size_t) std::max(0, blocks) + 1, 0.0f);
        onsetPeak = onsetPeakHigh = 0.0f;

        float prev = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            double e = 0.0, eh = 0.0;
            for (int i = b * onsetHop; i < (b + 1) * onsetHop; ++i)
            {
                const float m = monoAt(i), d = m - prev;
                prev = m;
                e += (double) m * m;
                eh += (double) d * d;
                onsetPeak = std::max(onsetPeak, std::abs(m));
                onsetPeakHigh = std::max(onsetPeakHigh, std::abs(d));
            }
            onsetEnergy[(size_t) b] = (float) (e / onsetHop);
            onsetEnergyHigh[(size_t) b] = (float) (eh / onsetHop);
        }
    }

    // Slices from the settings: GRID as the SLCS cell says, TRNS from the onsets.
    void rebuildSlices()
    {
        if (getSliceMode() == SliceTransient)
        {
            Slice found[MAX_GRID_SLICES];
            const int stored = (int) std::lround(getParamValue(SliceCount, 1.0f));
            const int limit = stored > 1 ? std::min(stored, (int) MAX_GRID_SLICES) : (int) MAX_GRID_SLICES;
            const int count = detectTransientSlices(found, limit);
            publishSlices(found, count > 1 ? count : 0);
        }
        else
        {
            const int n = std::clamp((int) std::lround(getParamValue(SliceCount, 1.0f)), 1, (int) MAX_GRID_SLICES);
            if (n > 1) sliceByGrid(n);
            else sliceCount = 0;
        }
    }

    void publishSlices(const Slice* list, int count)
    {
        changingSample.store(true);
        for (int spins = 0; inProcess.load() && spins < 2000; ++spins)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        count = std::clamp(count, 0, (int) MAX_SLICES);
        for (int i = 0; i < count; ++i)
            slices[i] = list[i];
        sliceCount = count;
        nextSlice = 0;
        changingSample.store(false);
    }

    // The detection function at hop k (the rise of the log energy up to sample k * hop). Fills
    // onsetScratch; returns the number of values.
    int computeOnsetCurve(float sens)
    {
        const int blocks = (int) onsetEnergy.size();
        if (blocks < ONSET_WINDOW_HOPS || (int) onsetScratch.size() < blocks + 1)
            return 0;
        // Nothing below this level counts: -20 dB (SENS 0) .. -60 dB (SENS 1) re. the peak.
        const float rel = std::pow(10.0f, -(20.0f + 40.0f * sens) / 20.0f);
        const double floorLow = std::max(1.0e-12, (double) (onsetPeak * rel) * (double) (onsetPeak * rel));
        const double floorHigh = std::max(1.0e-12, (double) (onsetPeakHigh * rel) * (double) (onsetPeakHigh * rel));

        // Log of the mean energy of the (up to) 8 hops that end at k, compared with LAG frames before.
        double sum = 0.0, sumHigh = 0.0;
        float logLow[ONSET_LAG], logHigh[ONSET_LAG];      // [0] = LAG frames back (silence before the start)
        for (int j = 0; j < ONSET_LAG; ++j)
        {
            logLow[j] = (float) std::log(floorLow);
            logHigh[j] = (float) std::log(floorHigh);
        }
        onsetScratch[0] = 0.0f;
        for (int k = 1; k <= blocks; ++k)
        {
            sum += (double) onsetEnergy[(size_t) (k - 1)];
            sumHigh += (double) onsetEnergyHigh[(size_t) (k - 1)];
            if (k > ONSET_WINDOW_HOPS)
            {
                sum -= (double) onsetEnergy[(size_t) (k - 1 - ONSET_WINDOW_HOPS)];
                sumHigh -= (double) onsetEnergyHigh[(size_t) (k - 1 - ONSET_WINDOW_HOPS)];
            }
            const int n = std::min(k, (int) ONSET_WINDOW_HOPS);
            const float lo = (float) std::log(std::max(0.0, sum) / n + floorLow);
            const float hi = (float) std::log(std::max(0.0, sumHigh) / n + floorHigh);
            onsetScratch[(size_t) k] = std::max(0.0f, std::max(lo - logLow[0], hi - logHigh[0]));
            for (int j = 0; j + 1 < ONSET_LAG; ++j)
            {
                logLow[j] = logLow[j + 1];
                logHigh[j] = logHigh[j + 1];
            }
            logLow[ONSET_LAG - 1] = lo;
            logHigh[ONSET_LAG - 1] = hi;
        }
        return blocks + 1;
    }

    // The sample on which the rise found at hop k begins.
    int refineOnset(int k) const
    {
        const int n = sampleLength;
        const int a = std::max(0, (k - ONSET_LAG - 1) * onsetHop);
        const int b = std::min(n, (k + 1) * onsetHop);
        const int preA = std::max(0, a - 4 * onsetHop);
        for (int pass = 0; pass < 2; ++pass)
        {
            auto level = [this, pass](int i)
            {
                return pass == 0 ? std::abs(monoAt(i)) : std::abs(monoAt(i) - monoAt(i - 1));
            };
            float pre = 0.0f, loc = 0.0f;
            for (int i = preA; i < a; ++i) pre = std::max(pre, level(i));
            for (int i = a; i < b; ++i) loc = std::max(loc, level(i));
            if (loc <= 2.0f * pre || loc <= 0.0f)
                continue;
            const float threshold = pre + 0.2f * (loc - pre);
            for (int i = a; i < b; ++i)
                if (level(i) > threshold)
                    return i;
        }
        return std::min(std::max(0, n - 1), a + onsetHop);
    }

    // The nearest zero crossing: up to 2 ms before p, else up to 0.5 ms after it, else p.
    int snapToZeroCrossing(int p) const
    {
        const int n = sampleLength;
        if (p <= 0 || p >= n) return std::clamp(p, 0, std::max(0, n - 1));
        const int reach = std::max(2, (int) (originalSampleRate * 0.002));
        const float tiny = onsetPeak * 1.0e-4f;
        auto crossing = [this, tiny](int i)      // between i - 1 and i
        {
            const float x0 = monoAt(i - 1), x1 = monoAt(i);
            return std::abs(x0) <= tiny || std::abs(x1) <= tiny || (x0 < 0.0f) != (x1 < 0.0f);
        };
        auto nearer = [this](int i) { return std::abs(monoAt(i - 1)) < std::abs(monoAt(i)) ? i - 1 : i; };
        for (int i = p; i > std::max(0, p - reach); --i)
            if (crossing(i))
                return nearer(i);
        for (int i = p + 1; i < std::min(n, p + reach / 4); ++i)
            if (crossing(i))
                return nearer(i);
        return p;
    }

    // Fills `out` with at most maxSlices slices (strongest onsets kept); returns how many.
    int detectTransientSlices(Slice* out, int maxSlices)
    {
        const int n = sampleLength;
        const float sens = std::clamp(getParamValue(SliceSens, 0.5f), 0.0f, 1.0f);
        const int frames = computeOnsetCurve(sens);
        maxSlices = std::clamp(maxSlices, 1, (int) MAX_GRID_SLICES);
        if (frames < 4 || n <= 0)
            return 0;

        // delta: a rise of 6.5 dB within two hops at SENS 0, 2 dB at 50 %, 0.65 dB at 100 %.
        const float delta = 1.5f * std::pow(0.1f, sens);
        const int gap = std::max(1, (int) ((double) std::clamp(getParamValue(SliceGap, 0.05f), 0.01f, 1.0f) * originalSampleRate));
        const float* odf = onsetScratch.data();

        int pos[MAX_ONSET_CANDIDATES];
        float strength[MAX_ONSET_CANDIDATES];
        int count = 0;
        auto removeWeakest = [&]
        {
            int weakest = 0;
            for (int i = 1; i < count; ++i)
                if (strength[i] < strength[weakest]) weakest = i;
            for (int i = weakest; i + 1 < count; ++i) { pos[i] = pos[i + 1]; strength[i] = strength[i + 1]; }
            --count;
        };

        for (int k = 1; k < frames; ++k)
        {
            const float v = odf[k];
            if (v <= delta)
                continue;
            bool peak = true;
            for (int j = 1; j <= 3 && peak; ++j)
                peak = ! (k - j >= 0 && odf[k - j] >= v) && ! (k + j < frames && odf[k + j] > v);
            if (! peak)
                continue;

            float window[2 * ONSET_MEDIAN_HALF + 1];
            int w = 0;
            for (int j = std::max(0, k - ONSET_MEDIAN_HALF); j <= std::min(frames - 1, k + ONSET_MEDIAN_HALF); ++j)
                window[w++] = odf[j];
            std::nth_element(window, window + w / 2, window + w);
            if (v <= delta + window[w / 2])
                continue;

            const int p = snapToZeroCrossing(refineOnset(k));
            if (count > 0 && p - pos[count - 1] < gap)
                continue;
            if (count == MAX_ONSET_CANDIDATES)
                removeWeakest();
            pos[count] = p;
            strength[count] = v;
            ++count;
        }
        if (count == 0)
            return 0;

        // Sound before the first onset is the first slice (and is merged with an onset that follows
        // it too closely); silence there is left out.
        float lead = 0.0f;
        for (int i = 0; i < pos[0]; ++i) lead = std::max(lead, std::abs(monoAt(i)));
        if (lead > onsetPeak * 0.003f)
        {
            if (pos[0] < gap)
            {
                pos[0] = 0;
                strength[0] = 1.0e9f;
            }
            else
            {
                if (count == MAX_ONSET_CANDIDATES)
                    removeWeakest();
                for (int i = count; i > 0; --i) { pos[i] = pos[i - 1]; strength[i] = strength[i - 1]; }
                pos[0] = 0;
                strength[0] = 1.0e9f;
                ++count;
            }
        }
        while (count > maxSlices)
            removeWeakest();

        for (int i = 0; i < count; ++i)
        {
            out[i].startSample = pos[i];
            out[i].endSample = i + 1 < count ? pos[i + 1] : n;
        }
        return count;
    }

    // -------------------------------------------------------------------------------------------
    // Time-stretch (STRCH page; every play mode but GRAN)
    //
    // Time and pitch are separated the way GRAN mode does it: the voice's `position` becomes a TIME
    // position that moves through STRT..END at `speed` x the sample's original speed whatever note
    // is played, and the sound around it is read at the note's pitch (PTCH, FINE, key, bend, PTCH
    // LFO). The play mode acts on the time position exactly as it does on an unstretched note:
    // FWD / 1SHT / GATE end at END, REV runs backwards, LOOP and PING wrap and turn - so every
    // mode, the start / end / loop points, slices, RAND and the STRT destination work with every
    // algorithm, and the filter, envelope, LFOs, BITS / RATE / DRIV follow as before.
    //
    // speed: the SPD cell (0.25 .. 4), or with SYNC on (loop length in seconds) / (LEN in bars at
    // the current tempo), read every block, so a held note follows a tempo change. "Loop length"
    // is STRT..END of the whole sample when the note plays the whole sample, and the whole sample
    // when it plays one slice (the slices of a loop then keep the loop's speed). The SPD LFO
    // destination multiplies it by 2^(+-2).
    //
    //   TAPE  varispeed: the sample is resampled, so pitch follows speed (speed x the note's
    //         pitch). With SYNC: the loop repitched to the tempo.
    //   CYCL  the cyclic time-stretch of the 1980s / 90s hardware samplers (often called the
    //         "Akai-style" stretch): the sound is read in cycles of SIZE; each new cycle starts at
    //         the time position, so stretched material repeats parts of cycles and compressed
    //         material skips them, with a linear crossfade of XFAD x SIZE between cycles. No
    //         attempt to line up waveforms: the buzz at 1 / SIZE and the stutter on long cycles are
    //         the sound. JIT moves each cycle's start at random (up to +-one cycle), which blurs
    //         the buzz. At speed 1 and the root key the cycles join seamlessly: the original.
    //   GRAN  the granular mode's grain engine (processGrainCloud; Roads, "Microsound") around the
    //         time position: Hann grains of SIZE, overlap 2 + 8 x XFAD (4 at the default), started
    //         at intervals scattered by +-30 %; JIT = random position up to +-one grain (and looser
    //         timing), SPRD = stereo scatter. Smooth, slightly chorused; a steady tone gets a
    //         faint flutter. At speed 1 the grains line up, so only the level ripples.
    //   SLCE  beat slicing (the "sliced loop" playback of loop-slicing tools): the slices (grid or
    //         transient) are not stretched at all. Each plays at the note's pitch from its start
    //         when the time position reaches it; it ends at its own end (FADE / SLEN apply) when
    //         the loop is slowed down, leaving a gap, and is cut with a 1..20 ms (XFAD) fade when
    //         the next one arrives early. The note plays the whole STRT..END (SLIC and PLAY are not
    //         used). Without slices the region is one slice.
    //   SMER  smear / freeze: the same grain engine with very long grains (8 x SIZE, up to 2 s),
    //         overlap 8 + 24 x XFAD, grains started at random times and at random positions within
    //         +-50..500 ms (JIT). This is a time-domain stand-in for spectral (phase-vocoder)
    //         smearing: transients dissolve and the sound becomes a slowly moving pad. Its SPD is
    //         a sixteenth of the cell (1.00 = 16 x as long, 0.25 = 64 x); SYNC lengths are exact.
    //   WSOL  waveform-similarity overlap-add (W. Verhelst and M. Roelands, "An overlap-add
    //         technique based on waveform similarity (WSOLA) for high quality time-scale
    //         modification of speech", ICASSP 1993): CYCL with cycles of SIZE / 2 whose start is
    //         moved by up to +-12 ms to where the sound best matches the natural continuation of
    //         the cycle before (largest normalised cross-correlation over the crossfade, the
    //         smallest shift winning ties). The splices then join in phase: the clean option. The
    //         search for the next cycle is spread over the current one, a few candidates per
    //         sample, so there is no burst of work at a cycle boundary.
    //
    // CYCL, SLCE and WSOL share one mechanism: two read heads per voice (the cycle that sounds and
    // the one fading out) on the sampler's interpolated read. Nothing allocates; every loop is
    // bounded by a constant; all reads clamp to the current sample and a sample change stops every
    // voice first (beginSampleChange).
    // -------------------------------------------------------------------------------------------

    static constexpr int WSOL_TEMPLATE_POINTS = 40;
    static constexpr double WSOL_SEARCH_SECONDS = 0.012;

    struct StretchSettings
    {
        int algo = StretchOff;
        double speed = 1.0;             // source seconds per output second
        double frameRatio = 1.0;        // sample frames per output sample at speed 1
        int cycleSamples = 2048;        // CYCL, WSOL: output samples per cycle
        float xfadeSamples = 0.0f;      // crossfade into a new cycle / slice
        float jitter = 0.0f;
        double declickSamples = 0.0;    // SLCE, GRAN, SMER: fade at the very end of the note
        double sliceFadeFrames = 0.0;   // SLCE: FADE
        float sliceKeep = 1.0f;         // SLCE: SLEN
        int searchLags = 0;             // WSOL
        int lagsPerSample = 1;
        double lagStep = 1.0;           // frames
        double templateStride = 1.0;    // output samples between the points compared
        GrainSettings grains;
    };

    // What one voice is rendered with in one sub-block: the block's own values, and over them
    // what the router moves for this voice (renderVoice).
    struct Moved
    {
        // Per sample, or null: pitch (a ratio), level (a gain), pan (added to PAN), cutoff
        // (octaves), the grains' place (GPOS, of the region)
        const float* pitch = nullptr;
        const float* amp = nullptr;
        const float* pan = nullptr;
        const float* cut = nullptr;
        const float* gpos = nullptr;
        float gposNow = 0.0f;                           // GPOS at the sample being made
        float size = 1.0f, density = 1.0f, speed = 1.0f;    // factors on SIZE, DENS and the stretch speed / SCAN
        bool sizeMoved = false, densityMoved = false;
        bool envMoved = false;                          // ENVF is moved: its value
        float envAmount = 0.0f;
        bool filterDriveMoves = false;                  // FDRV is on its way to this
        float filterDrive = 0.0f;
        float drive = 0.0f, volume = 0.8f, velSens = 1.0f;
        int bits = 0, rateDiv = 1;
        const GrainSettings* grains = nullptr;
        const StretchSettings* stretch = nullptr;
    };

    // What processBlock works out once for all voices.
    struct Block
    {
        PlayMode mode = Forward;
        bool granular = false, stereo = false, endFades = false, sliceFades = false;
        int stretchAlgo = StretchOff;
        float volume = 0.8f, volumeScale = 1.0f, velSens = 1.0f, pan = 0.0f, panL = 0.7071f, panR = 0.7071f;
        float driveAmt = 0.0f, glideStep = 0.01f;
        int bits = 0, rateDiv = 1;
        double declickSamples = 0.0;
        GrainSettings gs;
        StretchSettings stretch;
        // A voice whose slots move a grain or stretch cell works from its own copy of the settings.
        GrainSettings ownGrains;
        StretchSettings ownStretch;
        Moved plain;                // a voice nothing moves
    };

    // SYNC: the loop lasts LEN bars (of four beats) at the tempo.
    double syncedSpeed() const
    {
        const int index = std::clamp((int) std::lround(getParamValue(StretchLength, 4.0f)), 0, NUM_STRETCH_LENGTHS - 1);
        const double seconds = (double) stretchLengthBars()[index] * 240.0 / (double) std::max(1.0f, tempoBpm);
        double frames = (double) sampleLength;
        if (sliceCount <= 0 || getStretchAlgo() == StretchSlice)
            frames *= (double) std::clamp(getParamValue(EndPoint, 1.0f) - getParamValue(StartPoint, 0.0f), 0.0f, 1.0f);
        const double loopSeconds = std::max(1.0, frames) / std::max(1.0, originalSampleRate);
        return std::clamp(loopSeconds / seconds, MIN_STRETCH_SPEED, MAX_STRETCH_SPEED);
    }

    double stretchSpeed(int algo) const
    {
        if (getParamValue(StretchSync, 0.0f) > 0.5f)
            return syncedSpeed();
        double speed = (double) std::clamp(getParamValue(StretchSpeed, 1.0f), 0.25f, 4.0f);
        if (algo == StretchSmear)
            speed *= (double) SMEAR_SPEED_FACTOR;
        return speed;
    }

    // voice: for one voice, when a slot moves SIZE, XFAD, JIT or SPRD (or FADE, SLEN) of it.
    StretchSettings readStretchSettings(int algo, const Voice* voice = nullptr) const
    {
        auto P = [this, voice](int id, float fallback)
        {
            return voice != nullptr ? cellOf(*voice, id) : getParamValue(id, fallback);
        };
        StretchSettings s;
        const double sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        s.algo = algo;
        s.speed = stretchSpeed(algo);
        s.frameRatio = originalSampleRate / sr;
        const float size = std::clamp(P(StretchSize, 0.05f), 0.005f, 0.5f);
        const float xfade = std::clamp(P(StretchXfade, 0.25f), 0.0f, 1.0f);
        s.jitter = std::clamp(P(StretchJitter, 0.0f), 0.0f, 1.0f);
        s.declickSamples = 0.003 * sr;

        if (algo == StretchCycle || algo == StretchWsol)
        {
            s.cycleSamples = std::max(16, (int) ((algo == StretchWsol ? 0.5 : 1.0) * (double) size * sr));
            s.xfadeSamples = xfade * (float) s.cycleSamples;
            if (algo == StretchWsol)
            {
                s.xfadeSamples = std::max(s.xfadeSamples, std::min(32.0f, (float) s.cycleSamples));
                s.lagStep = std::max(1.0, std::floor(originalSampleRate / 48000.0 + 0.5));
                s.searchLags = 2 * (int) (WSOL_SEARCH_SECONDS * originalSampleRate / s.lagStep) + 1;
                s.lagsPerSample = s.searchLags / std::max(1, s.cycleSamples * 3 / 4) + 1;
                const double span = std::clamp((double) s.xfadeSamples, 0.005 * sr, 0.012 * sr);
                s.templateStride = span / (double) WSOL_TEMPLATE_POINTS;
            }
        }
        else if (algo == StretchSlice)
        {
            s.xfadeSamples = (0.001f + 0.019f * xfade) * (float) sr;
            s.sliceFadeFrames = (double) std::clamp(P(SliceFade, 0.0f), 0.0f, 1.0f) * originalSampleRate;
            s.sliceKeep = std::clamp(P(SliceLen, 1.0f), 0.0f, 1.0f);
        }
        else if (algo == StretchGrain || algo == StretchSmear)
        {
            s.grains.invSampleRate = 1.0 / sr;
            auto& g = s.grains.spec;
            float overlap, spray;
            if (algo == StretchSmear)
            {
                g.size = std::min(size * 8.0f, 2.0f);
                overlap = 8.0f + 24.0f * xfade;
                spray = 1.0f;
                g.scatterFrames = (0.05f + 0.45f * s.jitter) * (float) originalSampleRate;
            }
            else
            {
                // The grain intervals are always scattered by +-30 % (more with JIT): grains at
                // exactly regular intervals turn a steady tone into a comb at the grain rate, which
                // moves its pitch or cancels it. Scattered TIMES leave the grains lined up at speed
                // 1; JIT scatters the POSITIONS too (up to +-one grain).
                g.size = size;
                overlap = 2.0f + 8.0f * xfade;
                spray = 0.3f + 0.7f * s.jitter;
                g.scatterFrames = s.jitter * size * (float) originalSampleRate / spray;
            }
            g.jitter = g.scatter = spray;
            g.density = overlap / g.size;
            s.grains.scanInc = 0.0;
            g.pitchJitter = 0.0f;
            g.spread = std::clamp(P(StretchSpread, 0.0f), 0.0f, 1.0f);
            g.reverse = 0.0f;
            g.window = WindowHann;
        }
        return s;
    }

    // Moves the time position by `inc` frames in the voice's direction; the play mode decides what
    // happens at the ends (the same rules as advancePosition). True when the position jumped.
    bool advanceScan(Voice& v, PlayMode mode, double inc)
    {
        v.position += v.forward ? inc : -inc;
        const bool looping = (mode == Loop || mode == PingPong);

        if (v.forward && v.position >= v.regionEnd)
        {
            if (! looping)
            {
                v.active = false;
                return false;
            }
            if (mode == PingPong)
            {
                v.forward = false;
                v.position = v.regionEnd - 1;
            }
            else
            {
                const double loopLen = (double) (v.loopEnd - v.loopStart);
                v.position = loopLen > 0 ? v.loopStart + std::fmod(v.position - v.loopEnd, loopLen) : (double) v.loopStart;
            }
            return true;
        }
        if (! v.forward && v.position < v.regionStart)
        {
            if (mode == PingPong)
            {
                v.forward = true;
                v.position = v.regionStart;
                return true;
            }
            v.active = false;
        }
        return false;
    }

    // A short fade where a note that does not loop reaches the end of its region.
    void declickEnd(const Voice& v, PlayMode mode, double spanFrames, float& l, float& r) const
    {
        if (mode == Loop || mode == PingPong || spanFrames <= 0.0)
            return;
        const double left = v.forward ? (double) v.regionEnd - v.position : v.position - (double) v.regionStart;
        if (left < spanFrames)
        {
            const float g = (float) std::max(0.0, left / spanFrames);
            l *= g;
            r *= g;
        }
    }

    // One output sample of a stretched voice.
    void processStretch(Voice& v, PlayMode mode, const StretchSettings& st, const Moved& m, bool stereo, float& outL, float& outR)
    {
        const double pitchInc = v.playbackRate * (double) bendRatio * v.lfoRate;
        double speed = st.speed;
        if (m.speed != 1.0f)
            speed = std::clamp(speed * (double) m.speed, MIN_STRETCH_SPEED, MAX_STRETCH_SPEED);
        const double timeInc = speed * st.frameRatio;

        if (st.algo == StretchTape)
        {
            readVoice(v, mode, stereo, outL, outR);
            advanceScan(v, mode, pitchInc * speed);
        }
        else if (st.algo == StretchGrain || st.algo == StretchSmear)
        {
            v.reverseGrains = ! v.forward;
            processGrainCloud(v, st.grains, m, stereo, outL, outR);
            advanceScan(v, mode, timeInc);
            declickEnd(v, mode, st.declickSamples * timeInc, outL, outR);
        }
        else
        {
            processSplice(v, mode, st, pitchInc, timeInc, stereo, outL, outR);
        }
    }

    // The slice (index into `slices`) that holds frame `pos`, starting the search at `hint`, or -1;
    // without slices, 0 = the voice's whole area.
    int sliceAt(const Voice& v, double pos, int hint) const
    {
        const int count = sliceCount;
        if (count <= 0)
            return 0;
        if (hint >= 0 && hint < count && pos >= slices[hint].startSample && pos < slices[hint].endSample)
            return hint;
        for (int i = 0; i < count; ++i)
            if (pos >= slices[i].startSample && pos < slices[i].endSample)
                return i;
        juce::ignoreUnused(v);
        return -1;      // between slices (silence before the first transient)
    }

    float nearestSample(double pos) const
    {
        return sampleL[(size_t) std::clamp((int) pos, 0, sampleLength - 1)];
    }

    void readHead(const Head& h, bool stereo, double fadeFrames, float& l, float& r) const
    {
        if (! h.on)
        {
            l = r = 0.0f;
            return;
        }
        readSampleStereoInterp(h.pos, stereo, l, r);
        if (fadeFrames > 0.0)
        {
            const double left = h.limit - h.pos;
            if (left < fadeFrames)
            {
                const float g = (float) std::max(0.0, left / fadeFrames);
                l *= g;
                r *= g;
            }
        }
    }

    // CYCL, WSOL and SLCE: see the section comment.
    void processSplice(Voice& v, PlayMode mode, const StretchSettings& st, double pitchInc, double timeInc,
                       bool stereo, float& outL, float& outR)
    {
        const bool slicing = st.algo == StretchSlice;
        bool start = false;
        double startPos = v.position, limit = 0.0, dir = v.forward ? 1.0 : -1.0;

        if (slicing)
        {
            const int s = sliceAt(v, v.position, v.curSlice);
            if (s < 0)
            {
                // No slice here: whatever sounds plays on to its own end.
                v.curSlice = -1;
                v.forceSplice = false;
            }
            else if (s != v.curSlice || v.forceSplice)
            {
                start = true;
                double sliceStart = (double) v.grainAreaStart, sliceEnd = (double) v.grainAreaEnd;
                if (sliceCount > 0)
                {
                    sliceStart = (double) slices[s].startSample;
                    sliceEnd = (double) slices[s].endSample;
                    playingSlice.store(s, std::memory_order_relaxed);
                }
                // Slices always play forwards, from their start - except the one the note (or a
                // loop) begins inside.
                const bool inside = v.forceSplice && v.forward && v.position >= sliceStart && v.position < sliceEnd;
                startPos = inside ? v.position : sliceStart;
                limit = sliceStart + std::max(1.0, (sliceEnd - sliceStart) * (double) st.sliceKeep);
                dir = 1.0;
                v.curSlice = s;
            }
        }
        else if (v.forceSplice || v.spliceCount <= 0)
        {
            start = true;
            if (! v.forceSplice && v.cur.on)
            {
                if (st.algo == StretchWsol && v.searchValid)
                    startPos = v.cur.pos + v.searchBestD;
                else if (st.algo == StretchCycle && st.jitter > 0.0f)
                    startPos += (double) (st.jitter * v.grainSeq.bipolar()) * (double) st.cycleSamples * pitchInc;
            }
            v.spliceCount = st.cycleSamples;
        }

        if (start)
        {
            // The first cycle of a note starts at full level (the envelope shapes the attack).
            if (v.cur.on && st.xfadeSamples >= 1.0f)
            {
                v.prev = v.cur;
                v.xfade = 0.0f;
                v.xfadeInc = 1.0f / st.xfadeSamples;
            }
            else
            {
                v.prev.on = false;
                v.xfade = 1.0f;
            }
            v.cur.pos = startPos;
            v.cur.limit = limit;
            v.cur.dir = dir;
            v.cur.on = true;
            v.forceSplice = false;
            v.searchIdx = 0;
            v.searchBest = -2.0f;
            v.searchValid = false;
        }

        const double headFade = slicing ? st.sliceFadeFrames : 0.0;
        readHead(v.cur, stereo, headFade, outL, outR);
        if (v.xfade < 1.0f)
        {
            if (v.prev.on)
            {
                float pl, pr;
                readHead(v.prev, stereo, headFade, pl, pr);
                outL = pl + (outL - pl) * v.xfade;
                outR = pr + (outR - pr) * v.xfade;
            }
            v.xfade += v.xfadeInc;
            if (v.xfade >= 1.0f)
            {
                v.xfade = 1.0f;
                v.prev.on = false;
            }
        }

        v.cur.pos += v.cur.dir * pitchInc;
        if (v.prev.on)
            v.prev.pos += v.prev.dir * pitchInc;
        if (slicing)
        {
            if (v.cur.pos >= v.cur.limit) v.cur.on = false;
            if (v.prev.pos >= v.prev.limit) v.prev.on = false;
        }
        else
        {
            --v.spliceCount;
        }

        if (advanceScan(v, mode, timeInc))
            v.forceSplice = true;

        if (slicing)
        {
            declickEnd(v, mode, st.declickSamples * timeInc, outL, outR);
        }
        else if (st.algo == StretchWsol && v.cur.on && ! v.forceSplice && v.searchIdx < st.searchLags)
        {
            // Where the cycle that sounds will be when the next one starts (its natural
            // continuation), and where the time position will be: candidates around the latter
            // are compared with the former.
            const double headInc = v.cur.dir * pitchInc;
            const double remaining = (double) std::max(0, v.spliceCount);
            const double natural = v.cur.pos + headInc * remaining;
            const double nominal = v.position + (v.forward ? timeInc : -timeInc) * remaining;
            const double step = st.templateStride * headInc;
            for (int n = st.lagsPerSample; n > 0 && v.searchIdx < st.searchLags; --n)
            {
                const int k = v.searchIdx++;
                const double candidate = nominal + (double) ((k + 1) / 2) * st.lagStep * ((k & 1) ? 1.0 : -1.0);
                double xy = 0.0, xx = 0.0, yy = 0.0;
                for (int j = 0; j < WSOL_TEMPLATE_POINTS; ++j)
                {
                    const double a = (double) nearestSample(candidate + (double) j * step);
                    const double b = (double) nearestSample(natural + (double) j * step);
                    xy += a * b;
                    xx += a * a;
                    yy += b * b;
                }
                const float score = (float) (xy / std::sqrt(xx * yy + 1.0e-18));
                if (score > v.searchBest + 1.0e-4f)
                {
                    v.searchBest = score;
                    v.searchBestD = candidate - natural;
                    v.searchValid = true;
                }
            }
        }
    }

    // --- Playheads for the display (see getPlayheads) ---

    void clearPlayheads()
    {
        for (auto& p : playheadVoice)
            p.store(-1.0f, std::memory_order_relaxed);
        playheadGrainCount.store(0, std::memory_order_relaxed);
    }

    // Audio thread, end of a block.
    void publishPlayheads(bool granular)
    {
        const double inv = sampleLength > 0 ? 1.0 / (double) sampleLength : 0.0;
        auto norm = [inv](double pos) { return (float) std::clamp(pos * inv, 0.0, 1.0); };

        int totalGrains = 0;
        for (int i = 0; i < POOL_VOICES; ++i)
        {
            const auto& v = voices[i];
            const bool on = v.active && ! v.dying;
            playheadVoice[i].store(on ? norm(v.position) : -1.0f, std::memory_order_relaxed);
            if (on && granular)
                totalGrains += v.cloud.sounding();
        }

        int written = 0;
        if (totalGrains > 0)
        {
            const int step = (totalGrains + MAX_PLAYHEAD_GRAINS - 1) / MAX_PLAYHEAD_GRAINS;
            int counter = 0;
            for (int i = 0; i < POOL_VOICES && written < MAX_PLAYHEAD_GRAINS; ++i)
            {
                const auto& v = voices[i];
                if (! v.active || v.dying)
                    continue;
                for (int gi = 0; gi < v.cloud.sounding() && written < MAX_PLAYHEAD_GRAINS; ++gi)
                    if (counter++ % step == 0)
                        playheadGrain[written++].store(norm(v.cloud.grain(gi).pos), std::memory_order_relaxed);
            }
        }
        playheadGrainCount.store(written, std::memory_order_relaxed);
    }

    void advancePosition(Voice& v, PlayMode mode)
    {
        if (v.forward)
            v.position += v.playbackRate * (double) bendRatio * v.lfoRate;
        else
            v.position -= v.playbackRate * (double) bendRatio * v.lfoRate;

        bool looping = (mode == Loop || mode == PingPong);

        if (v.forward && v.position >= v.regionEnd)
        {
            if (looping)
            {
                if (mode == PingPong)
                {
                    v.forward = false;
                    v.position = v.regionEnd - 1;
                }
                else
                {
                    double loopLen = (double)(v.loopEnd - v.loopStart);
                    if (loopLen > 0)
                        v.position = v.loopStart + std::fmod(v.position - v.loopEnd, loopLen);
                    else
                        v.position = v.loopStart;
                }
            }
            else
            {
                v.active = false;
            }
        }
        else if (!v.forward && v.position < v.regionStart)
        {
            if (looping && mode == PingPong)
            {
                v.forward = true;
                v.position = v.regionStart;
            }
            else
            {
                v.active = false;
            }
        }
    }

    // The envelope with its stage curves (StageCurveParam slot 0; the maths of Envelope::shape).
    // updateEnvelope below keeps running the straight-line envelope, on envLine; envLevel is that
    // line bent by the curve of the stage it is in. With all three curves at zero the two are the
    // same. A level somebody else wrote into envLevel (a new note starting it at zero) is taken
    // over as it is.
    void updateCurvedEnvelope(Voice& v, PlayMode mode)
    {
        if (v.envLevel != v.envWritten)
        {
            v.envLine = v.envWritten = v.envLevel;
            v.envReleasing = false;
        }

        const float lineBefore = v.envLine;
        v.envLevel = v.envLine;
        updateEnvelope(v, mode);
        v.envLine = v.envLevel;

        const float line = v.envLine;
        float level = line;
        switch (v.envPhase)
        {
            case EnvAttack:
                v.envReleasing = false;
                level = Envelope::shape(line, stageCurves.of(0, StageCurveParam::Attack));
                break;
            case EnvDecay:
            {
                v.envReleasing = false;
                const float curve = stageCurves.of(0, StageCurveParam::Decay);
                const float span = 1.0f - v.sustain;
                if (curve != 0.0f && span > 1.0e-4f)
                    level = 1.0f - span * Envelope::shape(std::clamp((1.0f - line) / span, 0.0f, 1.0f), curve);
                break;
            }
            case EnvRelease:
            {
                if (!v.envReleasing)
                {
                    // The release starts from where the envelope was heard, not from the line.
                    v.envReleasing = true;
                    v.relLevel = v.envWritten;
                    v.relLine = std::max(lineBefore, 1.0e-6f);
                }
                const float curve = stageCurves.of(0, StageCurveParam::Release);
                if (curve != 0.0f || v.relLevel != v.relLine)
                    level = v.relLevel * (1.0f - Envelope::shape(std::clamp(1.0f - line / v.relLine, 0.0f, 1.0f), curve));
                break;
            }
            default:
                v.envReleasing = false;
                break;
        }
        v.envLevel = v.envWritten = level;
    }

    void updateEnvelope(Voice& v, PlayMode mode)
    {
        switch (v.envPhase)
        {
            case EnvAttack:
                v.envLevel += v.attackRate;
                if (v.envLevel >= 1.0f)
                {
                    v.envLevel = 1.0f;
                    v.envPhase = EnvDecay;
                }
                break;
            case EnvDecay:
                // Linear fall to the sustain level.
                v.envLevel -= v.decayRate * (1.0f - v.sustain + 0.0001f);
                if (v.envLevel <= v.sustain)
                {
                    v.envLevel = v.sustain;
                    v.envPhase = EnvSustain;
                }
                break;
            case EnvSustain:
                if (!v.gateOn && mode == Gated)
                    v.envPhase = EnvRelease;
                break;
            case EnvRelease:
                v.envLevel -= v.releaseRate;
                if (v.envLevel <= 0.0f)
                {
                    v.envLevel = 0.0f;
                    v.active = false;
                    v.envPhase = EnvOff;
                }
                break;
            case EnvOff:
                break;
        }
    }

    static float applyBitReduction(float sample, int reductionAmount)
    {
        int bits = std::max(1, 16 - reductionAmount);
        float levels = (float)(1 << bits);
        return std::round(sample * levels) / levels;
    }

    static float applySoftClip(float sample, float amount)
    {
        float gain = 1.0f + amount * 10.0f;
        float x = sample * gain;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    int numVoices;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    std::vector<float> sampleL;
    std::vector<float> sampleR;
    int sampleLength = 0;
    double originalSampleRate = 44100.0;

    Slice slices[MAX_SLICES];
    int sliceCount = 0;

    int samplePoolId = -1;  // reference into SamplePool (-1 = none)

    // Handshake so the audio thread is never inside the sample while it is replaced.
    void beginSampleChange()
    {
        changingSample.store(true);
        for (auto& v : voices)
            v.active = false;
        for (int spins = 0; inProcess.load() && spins < 2000; ++spins)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        // The audio thread is out of the block (and stays out until endSampleChange): no grain of
        // the old sample survives the change.
        for (auto& v : voices)
            v.cloud.clear();
        clearPlayheads();
    }
    void endSampleChange() { changingSample.store(false); }

    std::atomic<bool> inProcess { false };
    std::atomic<bool> changingSample { false };
    int bankIndex = -1;
    float bend = 0.0f;
    float wheel = 0.0f, wheelNow = 0.0f;                // the mod strip as sent, and smoothed per block
    float bendRatio = 1.0f;

    Voice voices[POOL_VOICES];
    uint32_t noteSerial = 0;
    std::array<float, NumParams> params = {};
    StageCurves stageCurves;

    float tempoBpm = 120.0f;
    float normalizeGain = 1.0f;         // 1 / peak of the sample (message thread, when it changes)
    int nextSlice = 0;                  // PLAY = NEXT
    spread::Generator sliceGen;         // PLAY = RAND: advanced once a note that asks
    std::atomic<int> playingSlice { -1 };

    // Onset analysis of the sample (see the transient-slicing section): mean-square level of the
    // sound and of its first difference per hop, and working space for the detection function.
    std::vector<float> onsetEnergy, onsetEnergyHigh, onsetScratch;
    int onsetHop = 128;
    float onsetPeak = 0.0f, onsetPeakHigh = 0.0f;
    std::vector<float> levelTable;      // SLEV: the level per hop, 0..1 of the loudest 23 ms (made with the onset tables)
    int levelHop = 128;                 // its hop (the audio thread's copy of onsetHop)

    // Published by the audio thread once per block (see getPlayheads). -1 = nothing there.
    std::atomic<float> playheadVoice[POOL_VOICES];
    std::atomic<float> playheadGrain[MAX_PLAYHEAD_GRAINS];
    std::atomic<int> playheadGrainCount { 0 };

    // Everything that moves the sound: LFO1, LFO2, the STRIP and XIN pages, the free slots, the
    // pad's pressure routes (ids 158-189 and 700-772, mod::Router's on every engine).
    mod::Router router { lfoDests(), 2, 0, &sources() };
    std::array<float, mod::Router::MAX_CHUNK> mixL {}, mixR {};
};
