#pragma once

#include "EngineBase.h"
#include "../dsp/DSPUtils.h"
#include "SampleBank.h"
#include "EngineLfoSection.h"
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

    // LFO destinations (EngineLfoSection), in the order of the DEST cell. Every voice has its own
    // pair of LFOs.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // playback speed, +-12 semitones, depth squared
        LfoAmp,         // gain 0..2
        LfoPan,         // +-1 around PAN
        LfoCutoff,      // +-4 octaves
        LfoResonance,   // +-0.5
        LfoStart,       // start point, +-1 of the region around STRT; read once, when the note starts
        LfoDrive,       // +-1 around DRIV (0..1)
        LfoGrainPos,    // GRAN: where new grains start, +-1 of the region around the scan position (clamped to it)
        LfoGrainSize,   // GRAN: +-1 of the SIZE knob's (logarithmic) range, read when a grain starts
        LfoGrainDens,   // GRAN: +-1 of the DENS knob's (logarithmic) range
        LfoSpeed,       // stretch speed (and SCAN in GRAN mode), x 2^(+-2): a quarter .. four times
        NumLfoDests
    };

    static const char* const* lfoDestLabels()
    {
        static const char* const names[NumLfoDests] = {
            "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "STRT", "DRIV", "GPOS", "SIZE", "DENS", "SPD" };
        return names;
    }

    static constexpr int MAX_VOICES = 8;
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

    // The "every voice's LFOs" argument for the LFO section (all voices, whatever VOX is set to).
    auto eachLfos()
    {
        return [this](auto&& fn) { for (auto& v : voices) fn(v.lfos); };
    }

public:

    SamplerEngine(int maxVoices = MAX_VOICES)
        : numVoices(std::min(maxVoices, MAX_VOICES))
    {
        for (auto& v : voices)
            v = Voice();
        setGrainSeed(0x9E3779B9u);
        setSliceSeed(0x2545F491u);
        playingSlice.store(-1, std::memory_order_relaxed);
        grainWindowTables();    // built here, not on the audio thread
        cellRanges();
        clearPlayheads();
        for (auto& p : playheadGrain)
            p.store(-1.0f, std::memory_order_relaxed);
        static_assert((int) NumLfoDests <= EngineLfoSection::MAX_DESTS, "too many LFO destinations");

        // Real defaults for every parameter, so getParameter is always the true value.
        for (auto& page : pages())
            for (auto& p : page.params)
                if (p.isValid() && ! EngineLfoSection::owns(p.id))   // the LFO section keeps its own values
                    params[(size_t) p.id] = p.defaultVal;
        params[Pan] = 0.0f;
        lfoSection.applyAll(eachLfos());

        selectBankSample(SampleBank::get().defaultIndex());
    }

    // SAMPL (which sample and what part of it), GRAIN (the granular play mode), SLICE (how the sample
    // is cut up and which slice a note plays), STRCH (time-stretch), TONE (pitch and colour), FILTR,
    // AMP (envelope, voices).
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
                { "VOX",  Voices, 1, MAX_VOICES, MAX_VOICES, I },
                { "PAN",  Pan, -1, 1, 0, L },
                { "RAND", StartRandom, 0, 1, 0, L } }} },
          };
          EngineLfoSection(lfoDests(&l)).appendPages(l);
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

    // The LFO destinations: the original eleven, then the other continuous cells of the pages
    // (each voice reads its own value of them: voiceValue) and the other LFO.
    //   Read when the note starts: END, LPST, LPEN, SLIC, FADE, SLEN, VELF, KTRK, ATK, DEC, SUS,
    //   REL, RAND (with STRT of the original list). Read when a grain starts: SPRY, PJIT, SPRD,
    //   RVRS. The rest follow the LFO while the note sounds.
    // Not offered: SMPL, the slicing cells and NORM (they rebuild data), MODE, SHAP, PLAY, ALGO,
    // SYNC, LEN, KEY, TYPE, VOX (choices and counts), ROOT, PTCH, FINE and BEND (PTCH of the
    // original list is the pitch).
    static const LfoDestList& lfoDests(const ParamPages* enginePages = nullptr)
    {
        if (enginePages == nullptr)
            pages();                    // the list is made on the way
        static const LfoDestList list = makeLfoDests(*enginePages);
        static const bool registered = (list.registerGroups(), true);
        juce::ignoreUnused(registered);
        return list;
    }

    static LfoDestList makeLfoDests(const ParamPages& p)
    {
        LfoDestList l(lfoDestLabels(), NumLfoDests);
        l.group();
        for (int id : { (int) EndPoint, (int) LoopStart, (int) LoopEnd })
            l.add(p, id);
        l.group();
        for (int id : { (int) GrainScan, (int) GrainSpray, (int) GrainPitchJit, (int) GrainSpread, (int) GrainReverse })
            l.add(p, id);
        l.group();
        for (int id : { (int) SliceSelect, (int) SliceFade, (int) SliceLen })
            l.add(p, id);
        l.group();
        l.add(p, StretchSize, "SSIZ");
        l.add(p, StretchXfade);
        l.add(p, StretchJitter);
        l.add(p, StretchSpread, "SSPR");
        l.group();
        for (int id : { (int) BitReduce, (int) RateReduce, (int) Volume })
            l.add(p, id);
        l.group();
        for (int id : { (int) VelToFilter, (int) FilterEnv, (int) FilterKey, (int) FilterDrive })
            l.add(p, id);
        l.group();
        for (int id : { (int) Attack, (int) Decay, (int) Sustain, (int) Release, (int) VelSens, (int) StartRandom })
            l.add(p, id);
        l.addCross();
        // The mod strip until the STRIP page says otherwise: more drive (with the level taken
        // back, see processBlock), a lower sample rate, and looser grains in GRAN mode.
        l.stripRoute(0, LfoDrive, 0.7f);
        l.stripRoute(1, l.indexOfKey(RateReduce), 0.3f);
        l.stripRoute(2, l.indexOfKey(GrainSpray), 0.5f);
        return l;
    }

    // The filter MODE cell lists the modes of the filter type currently chosen, and the SMPL cell
    // the samples the bank holds now (it grows while the app runs: recordings are appended).
    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
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
    // The mod strip wears the sound down: it adds to DRIV and to RATE (sample-rate reduction) on
    // the TONE page, and in GRAN mode to SPRY as well.
    void modWheel(float value) override { wheel = std::clamp(value, 0.0f, 1.0f); }
    void setTempo(float bpm) override
    {
        if (bpm > 1.0f)
            tempoBpm = bpm;     // read once per block: a synced stretch follows the tempo while notes sound
        lfoSection.setTempo(bpm, eachLfos());
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
        endSampleChange();
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
        endSampleChange();
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
        endSampleChange();
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
        sliceRng = seed != 0 ? seed : 0x1234567u;
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

    // Restarts the per-voice random generators of the granular mode (same seed = same cloud).
    void setGrainSeed(uint32_t seed)
    {
        for (int i = 0; i < POOL_VOICES; ++i)
        {
            uint32_t s = (seed ^ 0xA5A5A5A5u) + (uint32_t) (i + 1) * 0x9E3779B9u;
            voices[i].grainRng = s != 0 ? s : 0x1234567u;
        }
    }

    // Grains sounding now, over all voices (plain read; for tests and meters, not for decisions).
    int getActiveGrainCount() const
    {
        int n = 0;
        for (int i = 0; i < POOL_VOICES; ++i)
            if (voices[i].active)
                n += voices[i].numGrains;
        return n;
    }

    // --- EngineBase overrides ---

    void prepareToPlay(double sr, int blockSize) override
    {
        currentSampleRate = sr;
        currentBlockSize = blockSize;
        for (auto& v : voices)
            v.lfos.setSampleRate(sr);
    }

    void releaseResources() override
    {
        for (auto& v : voices)
        {
            v.active = false;
            v.numGrains = 0;
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

        int startSamp = 0, endSamp = sampleLength;

        // The LFOs are read here for everything that is fixed when the note starts (the start
        // point, the slice, the envelope times ...), and run on from this value. P is this
        // voice's value of a parameter: the set value unless one of its LFOs is routed to it.
        v.lfos.noteOnAndSample();
        v.lfos.snapSlews();     // a new note in this voice
        v.fresh = true;         // ... which takes every glided value as it stands
        v.lfoRate = 1.0;
        v.lfoGain = 1.0f;
        auto P = [this, &v](int id, float fallback) { return voiceValue(v.lfos, id, fallback); };

        // The SLCE stretch walks through every slice itself; otherwise the note plays one of them.
        int sliceIdx = getStretchAlgo() == StretchSlice ? -1 : pickSlice(note, &v.lfos);
        v.fadeFrames = 0.0;
        if (sliceIdx >= 0 && sliceIdx < sliceCount)
        {
            startSamp = slices[sliceIdx].startSample;
            endSamp = slices[sliceIdx].endSample;
            const float keep = P(SliceLen, 1.0f);
            if (keep < 1.0f)
                endSamp = startSamp + std::max(1, (int) ((float) (endSamp - startSamp) * std::max(keep, 0.0f)));
            v.fadeFrames = (double) std::clamp(P(SliceFade, 0.0f), 0.0f, 1.0f) * originalSampleRate;
        }
        else
        {
            sliceIdx = -1;
        }
        playingSlice.store(sliceIdx, std::memory_order_relaxed);

        float startFrac = getParamValue(StartPoint, 0.0f);
        if (v.lfos.routed(LfoStart))
            startFrac = std::clamp(startFrac + v.lfos.amount(LfoStart), 0.0f, 1.0f);
        float endFrac = P(EndPoint, 1.0f);
        int rangeLen = endSamp - startSamp;

        v.regionStart = startSamp + (int)(startFrac * rangeLen);
        v.regionEnd = startSamp + (int)(endFrac * rangeLen);
        if (v.regionEnd <= v.regionStart) v.regionEnd = v.regionStart + 1;

        float loopStartFrac = P(LoopStart, 0.0f);
        float loopEndFrac = P(LoopEnd, 1.0f);
        int regionLen = v.regionEnd - v.regionStart;
        v.loopStart = v.regionStart + (int)(loopStartFrac * regionLen);
        v.loopEnd = v.regionStart + (int)(loopEndFrac * regionLen);
        if (v.loopEnd <= v.loopStart) v.loopEnd = v.loopStart + 1;

        PlayMode mode = getPlayMode();
        v.forward = (mode != Reverse);
        v.position = v.forward ? (double) v.regionStart : (double)(v.regionEnd - 1);

        // Granular: no grains yet, the first one starts with the note's first sample. Grains may
        // reach anywhere in the slice (the whole sample without slices).
        v.grainAreaStart = startSamp;
        v.grainAreaEnd = std::max(endSamp, startSamp + 1);
        v.numGrains = 0;
        v.grainPhase = 1.0;
        v.grainThreshold = 1.0f;
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

        float atk = P(Attack, 0.001f);
        v.attackRate = 1.0f / std::max(atk * (float) currentSampleRate, 1.0f);
        float dec = P(Decay, 0.01f);
        v.decayRate = 1.0f / std::max(dec * (float) currentSampleRate, 1.0f);
        float rel = P(Release, 0.01f);
        v.releaseRate = 1.0f / std::max(rel * (float) currentSampleRate, 1.0f);
        v.sustain = std::clamp(P(Sustain, 1.0f), 0.0f, 1.0f);

        // Random start offset (up to the given fraction of the region).
        float startRand = P(StartRandom, 0.0f);
        if (startRand > 0.0f && v.forward)
        {
            int span = (int) ((float) (v.regionEnd - v.regionStart) * startRand * dsp::xorshiftFloat(randomSeed));
            v.position = (double) std::min(v.regionStart + span, v.regionEnd - 1);
        }

        // Per-voice filter. Velocity and key tracking set its starting point; the amp envelope can
        // move it while the note sounds.
        float velToFilter = P(VelToFilter, 0.0f);
        float baseCutoff = getParamValue(FilterCutoff, 1.0f);
        float keyOffset = P(FilterKey, 0.0f) * (float) (note - 60) / 120.0f;   // 1/10 of the range per octave
        v.filterCutoff = std::clamp(baseCutoff + velToFilter * velocity + keyOffset, 0.0f, 1.0f);
        v.filterEnvAmount = getParamValue(FilterEnv, 0.0f);
        auto model = FilterSlot::modelFromIndex((int) getParamValue(FilterType, 0.0f));
        v.filterOn = model != FilterModel::Bypass
                     && (v.filterCutoff < 0.999f || v.filterEnvAmount != 0.0f
                         || v.lfos.routed(LfoCutoff) || v.lfos.routed(LfoResonance)
                         || v.lfos.has(FilterEnv) || v.lfos.has(FilterDrive));
        v.filterDriveMoved = false;
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
            v.numGrains = 0;
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
        if (EngineLfoSection::owns(id)) return EngineLfoSection::name(id);
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
        if (lfoSection.owns(id))
        {
            lfoSection.set(id, value, eachLfos());
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
        if (lfoSection.owns(id))
            return lfoSection.get(id);
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
        // The mod strip: the routes of the STRIP page (by default DRIV, RATE and SPRY).
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, currentSampleRate);
        lfoSection.stripBlock(wheelNow, numSamples, currentSampleRate, eachLfos());
        const float velSens = getParamValue(VelSens, 1.0f);

        float* outL = buffer.getWritePointer(0);
        float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        PlayMode mode = getPlayMode();
        float volume = getParamValue(Volume, 0.8f);
        const float volumeSet = volume;
        const float volumeScale = getParamValue(Normalize, 0.0f) > 0.5f ? normalizeGain : 1.0f;
        if (volumeScale != 1.0f)
            volume *= volumeScale;
        float pan = getParamValue(Pan, 0.0f);
        float panL = std::cos((pan + 1.0f) * 0.25f * dsp::PI_F);
        float panR = std::sin((pan + 1.0f) * 0.25f * dsp::PI_F);

        int bitRedBits = (int)(getParamValue(BitReduce, 0.0f) * 12.0f);
        float rateReduce = std::min(1.0f, getParamValue(RateReduce, 0.0f));
        int rateDiv = 1 + (int)(rateReduce * 31.0f);
        const float setDrive = getParamValue(DriveAmt, 0.0f);
        float driveAmt = std::min(1.0f, setDrive);
        // The drive stage has gain: take most of what the strip adds to DRIV (its routes to that
        // destination) out again, so the strip changes the tone more than the level (exactly 1
        // with the strip at rest, or with no route to DRIV).
        const float stripDrive = lfoSection.stripAmount(LfoDrive);
        const float wheelLevel = stripDrive > 0.0f
            ? std::pow((1.0f + setDrive * 10.0f) / (1.0f + std::min(1.0f, setDrive + stripDrive) * 10.0f), 0.58f) : 1.0f;

        bool hasStereoSample = !sampleR.empty();
        const bool lfosOn = lfoSection.anyOn() || lfoSection.stripOn();    // the strip's routes are read like LFOs
        const float glideStep = (float) (1.0 / (0.004 * std::max(currentSampleRate, 1.0)));     // a whole range in 4 ms

        const bool granular = mode == Granular;
        const GrainSettings gs = granular ? readGrainSettings() : GrainSettings();
        const int stretchAlgo = getStretchAlgo();       // off in GRAN mode
        const StretchSettings stretch = stretchAlgo != StretchOff ? readStretchSettings(stretchAlgo) : StretchSettings();
        // Where a note that does not loop reaches the end of its region it is faded, never cut:
        // over the slice's FADE, and at least over END_DECLICK_SECONDS (the grain and slice
        // stretches have that short fade of their own, declickEnd).
        const bool endFades = ! granular && mode != Loop && mode != PingPong;
        const bool sliceFades = endFades && stretchAlgo != StretchSlice;
        const bool ownDeclick = stretchAlgo == StretchGrain || stretchAlgo == StretchSmear || stretchAlgo == StretchSlice;
        const double declickSamples = endFades && ! ownDeclick ? END_DECLICK_SECONDS * currentSampleRate : 0.0;
        // A voice whose LFOs move a grain or stretch cell works from its own copy of the settings.
        GrainSettings ownGrains;
        StretchSettings ownStretch;

        for (int i = 0; i < numSamples; ++i)
        {
            float mixL = 0.0f, mixR = 0.0f;

            for (int vi = 0; vi < POOL_VOICES; ++vi)
            {
                auto& v = voices[vi];
                if (!v.active) continue;

                // LFOs, per voice and per sample. Without them every value below is as it was.
                float voiceDrive = driveAmt;
                float voicePanL = panL, voicePanR = panR;
                float lfoCutOct = 0.0f;
                bool lfoFilter = false;
                int voiceBits = bitRedBits, voiceRateDiv = rateDiv;
                float voiceVolume = volume, voiceVelSens = velSens;
                const GrainSettings* voiceGrains = &gs;
                const StretchSettings* voiceStretch = &stretch;
                if (lfosOn)
                {
                    v.lfos.tick();
                    if (v.lfos.keyed())
                    {
                        // LFOs on page cells: this voice's own value of each.
                        voiceBits = (int) (voiceValue(v.lfos, BitReduce, 0.0f) * 12.0f);
                        voiceRateDiv = 1 + (int) (std::min(1.0f, voiceValue(v.lfos, RateReduce, 0.0f)) * 31.0f);
                        voiceVolume = v.lfos.value(Volume, volumeSet) * volumeScale;
                        voiceVelSens = v.lfos.value(VelSens, velSens);
                        if (v.filterOn && (v.lfos.has(FilterEnv) || v.lfos.has(FilterDrive)))
                            lfoFilter = true;
                        if (granular && keyedIn(v.lfos, GrainScan, GrainReverse))
                        {
                            ownGrains = readGrainSettings(&v.lfos);
                            voiceGrains = &ownGrains;
                        }
                        else if (stretchAlgo != StretchOff && (keyedIn(v.lfos, StretchSize, StretchSpread)
                                                               || keyedIn(v.lfos, SliceFade, SliceLen)))
                        {
                            ownStretch = readStretchSettings(stretchAlgo, &v.lfos);
                            voiceStretch = &ownStretch;
                        }
                    }
                    const float semis = v.lfos.pitchSemitones(LfoPitch);
                    v.lfoRate = semis != 0.0f ? (double) std::exp2f(semis / 12.0f) : 1.0;
                    v.lfoGain = v.lfos.gain(LfoAmp);
                    if (v.lfos.routed(LfoDrive))
                        voiceDrive = std::clamp(driveAmt + v.lfos.amount(LfoDrive), 0.0f, 1.0f);
                    if (v.lfos.routed(LfoPan))
                    {
                        const float p = std::clamp(pan + v.lfos.panAmount(LfoPan), -1.0f, 1.0f);
                        voicePanL = std::cos((p + 1.0f) * 0.25f * dsp::PI_F);
                        voicePanR = std::sin((p + 1.0f) * 0.25f * dsp::PI_F);
                    }
                    if (v.filterOn && (v.lfos.routed(LfoCutoff) || v.lfos.routed(LfoResonance)))
                    {
                        lfoFilter = true;
                        lfoCutOct = v.lfos.amount(LfoCutoff) * VoiceLfos::CUTOFF_OCTAVES;
                        const float resoMod = v.lfos.amount(LfoResonance) * VoiceLfos::RESONANCE_RANGE;
                        if (resoMod != v.filterResoMod)
                        {
                            v.filterResoMod = resoMod;
                            const float r = std::clamp(v.filterReso + resoMod, 0.0f, 1.0f);
                            v.filterL.setResonance(r);
                            v.filterR.setResonance(r);
                        }
                    }
                }
                else if (v.lfoRate != 1.0 || v.lfoGain != 1.0f || v.filterResoMod != 0.0f)
                {
                    // The LFOs were switched off under a sounding note.
                    v.lfoRate = 1.0;
                    v.lfoGain = 1.0f;
                    v.lfos.snapSlews();
                    if (v.filterResoMod != 0.0f)
                    {
                        v.filterResoMod = 0.0f;
                        v.filterL.setResonance(v.filterReso);
                        v.filterR.setResonance(v.filterReso);
                    }
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
                    v.filterEnvNow = lfoFilter ? v.lfos.value(FilterEnv, v.filterEnvAmount) : v.filterEnvAmount;
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
                if (granular)
                {
                    processGrains(v, *voiceGrains, lfosOn, hasStereoSample, sampL, sampR);
                }
                else if (stretchAlgo != StretchOff)
                {
                    processStretch(v, mode, *voiceStretch, lfosOn, hasStereoSample, sampL, sampR);
                }
                else
                {
                    readVoice(v, mode, hasStereoSample, sampL, sampR);
                    advancePosition(v, mode);
                }

                // The end of the region fades out (not while it loops): FADE for a slice, and the
                // short declick, measured in output samples at the speed the note moves at.
                if (endFades)
                {
                    double fade = declickSamples * std::abs(v.position - positionBefore);
                    if (sliceFades)
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

                if (!v.active) continue;

                float gain = v.envLevel * (1.0f - voiceVelSens + voiceVelSens * v.velocity) * voiceVolume * v.lfoGain;
                sampL *= gain;
                sampR *= gain;

                // Per-voice filter; its cutoff is refreshed every 16 samples (every sample while an
                // LFO moves it, in octaves: the 0..1 cutoff range is log2(1000) octaves wide).
                if (v.filterOn)
                {
                    const float envTarget = lfoFilter ? v.lfos.value(FilterEnv, v.filterEnvAmount) : v.filterEnvAmount;
                    const bool envGlides = v.filterEnvNow != envTarget;
                    if (envGlides)      // 8 octaves in 4 ms at most: a filter shut in one sample is a click
                        v.filterEnvNow = dsp::towards(v.filterEnvNow, envTarget, 0.8f * glideStep);
                    if (--v.filterCountdown <= 0 || lfoFilter || envGlides)
                    {
                        v.filterCountdown = 16;
                        const float envAmount = v.filterEnvNow;
                        if (lfoFilter && v.lfos.has(FilterDrive))
                        {
                            const float d = voiceValue(v.lfos, FilterDrive, 0.0f);
                            v.filterL.setDrive(d);
                            v.filterR.setDrive(d);
                            v.filterDriveMoved = true;
                        }
                        else if (v.filterDriveMoved)
                        {
                            // The LFO let go of FDRV under a sounding note.
                            v.filterL.setDrive(getParamValue(FilterDrive, 0.0f));
                            v.filterR.setDrive(getParamValue(FilterDrive, 0.0f));
                            v.filterDriveMoved = false;
                        }
                        float norm = std::clamp(v.filterCutoff + envAmount * v.envLevel, 0.0f, 1.0f);
                        if (lfoCutOct != 0.0f)
                            norm = std::clamp(norm + lfoCutOct / 9.9657843f, 0.0f, 1.0f);
                        float hz = 20.0f * std::pow(1000.0f, norm);
                        v.filterL.setCutoff(hz);
                        v.filterR.setCutoff(hz);
                    }
                    sampL = v.filterL.process(sampL);
                    sampR = v.filterR.process(sampR);
                }

                if (voiceBits > 0)
                {
                    sampL = applyBitReduction(sampL, voiceBits);
                    sampR = applyBitReduction(sampR, voiceBits);
                }

                if (voiceRateDiv > 1 && (i % voiceRateDiv) != 0)
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
                mixL += sampL * voicePanL;
                mixR += sampR * voicePanR;
            }

            if (wheelLevelNow != wheelLevel)    // (worked out once per block: not jumped to)
                wheelLevelNow = dsp::towards(wheelLevelNow, wheelLevel, glideStep);
            outL[i] += mixL * wheelLevelNow;
            if (outR) outR[i] += mixR * wheelLevelNow;
        }

        lfoSection.endBlock(numSamples, eachLfos());
        publishPlayheads(granular || stretchAlgo == StretchGrain || stretchAlgo == StretchSmear);

        bool anyActive = false;
        for (int vi = 0; vi < POOL_VOICES; ++vi)
            anyActive = anyActive || voices[vi].active;
        if (! anyActive)
            playingSlice.store(-1, std::memory_order_relaxed);
    }

private:
    float wheelLevelNow = 1.0f;     // the strip's level correction as it is heard (audio thread)

    enum EnvPhase { EnvAttack, EnvDecay, EnvSustain, EnvRelease, EnvOff };

    struct Grain
    {
        double pos = 0.0;           // read position, sample frames
        float ratio = 1.0f;         // speed relative to the note's (pitch jitter); negative = backwards
        float phase = 0.0f;         // 0..1 through the window
        float phaseInc = 0.0f;
        float gainL = 1.0f;         // loudness normalisation x stereo placement
        float gainR = 1.0f;
        int window = 0;
    };

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
        float filterReso = 0.0f;        // the set resonance, and what the LFOs add to it
        float filterResoMod = 0.0f;
        bool filterDriveMoved = false;  // an LFO has set the filter's drive away from FDRV
        VoiceLfos lfos;
        double lfoRate = 1.0;           // playback-speed ratio from the LFOs
        float lfoGain = 1.0f;
        // What the voice is heard with (see processBlock): moved to, never jumped to, under a note.
        bool fresh = true;
        float volumeNow = 0.0f, velSensNow = 1.0f, driveNow = 0.0f, panLNow = 0.7071f, panRNow = 0.7071f, filterEnvNow = 0.0f;

        // Granular mode. `position` is the scan position; grains[0 .. numGrains) are sounding.
        Grain grains[MAX_GRAINS];
        int numGrains = 0;
        int grainAreaStart = 0;         // what grains may read: the slice, or the whole sample
        int grainAreaEnd = 0;
        double grainPhase = 1.0;        // grain clock: a grain starts when it reaches grainThreshold
        float grainThreshold = 1.0f;
        uint32_t grainRng = 0x9E3779B9u;
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

    // One voice's value of a parameter: the set value unless one of the voice's LFOs is routed
    // to it (EngineLfoSection: parameter destinations).
    float voiceValue(const VoiceLfos& lfos, int id, float defaultVal) const
    {
        return lfos.value(id, getParamValue(id, defaultVal));
    }

    // True when one of the voice's LFOs is routed to a parameter in first..last.
    static bool keyedIn(const VoiceLfos& lfos, int first, int last)
    {
        for (int i = 0; i < VoiceLfos::NUM_SOURCES; ++i)      // the LFOs and the strip's routes
        {
            const int k = lfos.keyAt(i);
            if (k >= first && k <= last)
                return true;
        }
        return false;
    }

    // lfos: for a voice (its LFOs may be moving SLIC).
    int getActiveSlice(const VoiceLfos* lfos = nullptr) const
    {
        if (sliceCount <= 0) return -1;
        float sel = lfos != nullptr ? voiceValue(*lfos, SliceSelect, 0.0f) : getParamValue(SliceSelect, 0.0f);
        return std::clamp((int)(sel * sliceCount), 0, sliceCount - 1);
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
    int pickSlice(int note, const VoiceLfos* lfos = nullptr)
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
            case SliceRandom: return (int) (dsp::xorshift32(sliceRng) % (uint32_t) count);
            default:          return getActiveSlice(lfos);
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

    // Compute one-pole filter coefficient from normalized cutoff (0..1)
    static float computeFilterCoeff(float normalizedCutoff, float sampleRate)
    {
        // Map 0..1 to 20Hz..20kHz exponentially, then to coefficient
        float freq = 20.0f * std::pow(1000.0f, normalizedCutoff);
        float w = dsp::TWO_PI_F * freq / std::max(8000.0f, sampleRate);
        return std::clamp(w / (1.0f + w), 0.0f, 1.0f);
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
    // Real time: fixed grain pool in the voice, no allocation, no locks; every length is in seconds
    // or in sample frames scaled by the two sample rates. All reads go through
    // readSampleStereoInterp, which clamps to the current sample, and the block is not rendered at
    // all while the sample is being changed (changingSample), which also empties every grain pool.
    // -------------------------------------------------------------------------------------------

    struct GrainSettings
    {
        float size = 0.1f;          // seconds
        float density = 40.0f;      // grains per second
        double scanInc = 0.0;       // sample frames per output sample
        float spray = 0.0f;
        float sprayFrames = 0.0f;   // frames at SPRY = 1
        float pitchJitter = 0.0f;   // octaves at u = 1 (already squared)
        float spread = 0.0f;
        float reverse = 0.0f;
        int window = 0;
        double invSampleRate = 1.0 / 44100.0;
    };

    static constexpr int WINDOW_TABLE_SIZE = 1024;
    static constexpr float MIN_GRAIN_SIZE = 0.005f, MAX_GRAIN_SIZE = 0.5f;
    static constexpr float MIN_GRAIN_DENSITY = 1.0f, MAX_GRAIN_DENSITY = 200.0f;

    // Window shapes over 0..1 (one guard point so the interpolated lookup needs no wrap).
    //   HANN  0.5 - 0.5 cos(2 pi p)
    //   TRI   1 - |2p - 1|
    //   PERC  "expodec": linear attack over the first 2 %, then exp(-5 q) brought to exactly 0 at the end
    //   SWEL  "rexpodec": PERC backwards
    static const std::array<std::array<float, WINDOW_TABLE_SIZE + 2>, NumGrainWindows>& grainWindowTables()
    {
        static const auto tables = []
        {
            std::array<std::array<float, WINDOW_TABLE_SIZE + 2>, NumGrainWindows> t {};
            const double attack = 0.02, k = 5.0, floorLevel = std::exp(-k);
            auto perc = [&](double p)
            {
                if (p < attack) return p / attack;
                const double q = (p - attack) / (1.0 - attack);
                return (std::exp(-k * q) - floorLevel) / (1.0 - floorLevel);
            };
            for (int i = 0; i <= WINDOW_TABLE_SIZE + 1; ++i)
            {
                const double p = std::min(1.0, (double) i / (double) WINDOW_TABLE_SIZE);
                t[WindowHann][(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * p));
                t[WindowTriangle][(size_t) i] = (float) (1.0 - std::abs(2.0 * p - 1.0));
                t[WindowPerc][(size_t) i] = (float) perc(p);
                t[WindowSwell][(size_t) i] = (float) perc(1.0 - p);
            }
            return t;
        }();
        return tables;
    }

    // lfos: for one voice, whose LFOs may be moving SCAN, SPRY, PJIT, SPRD or RVRS.
    GrainSettings readGrainSettings(const VoiceLfos* lfos = nullptr) const
    {
        auto P = [this, lfos](int id, float fallback)
        {
            return lfos != nullptr ? voiceValue(*lfos, id, fallback) : getParamValue(id, fallback);
        };
        GrainSettings g;
        const double sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;
        g.invSampleRate = 1.0 / sr;
        g.size = std::clamp(getParamValue(GrainSize, 0.1f), MIN_GRAIN_SIZE, MAX_GRAIN_SIZE);
        g.density = std::clamp(getParamValue(GrainDensity, 40.0f), MIN_GRAIN_DENSITY, MAX_GRAIN_DENSITY);
        g.scanInc = (double) std::clamp(P(GrainScan, 1.0f), -2.0f, 2.0f) * originalSampleRate / sr;
        // STRCH SYNC in GRAN mode: SCAN 1.00 = STRT..END lasts LEN at the tempo.
        if (getParamValue(StretchSync, 0.0f) > 0.5f)
            g.scanInc *= syncedSpeed();
        g.spray = std::clamp(P(GrainSpray, 0.0f), 0.0f, 1.0f);
        g.sprayFrames = GRAIN_SPRAY_SECONDS * (float) originalSampleRate;
        const float pj = std::clamp(P(GrainPitchJit, 0.0f), 0.0f, 1.0f);
        g.pitchJitter = pj * pj * GRAIN_PITCH_JITTER_SEMITONES / 12.0f;
        g.spread = std::clamp(P(GrainSpread, 0.0f), 0.0f, 1.0f);
        g.reverse = std::clamp(P(GrainReverse, 0.0f), 0.0f, 1.0f);
        g.window = std::clamp((int) std::lround(getParamValue(GrainShape, 0.0f)), 0, (int) NumGrainWindows - 1);
        return g;
    }

    // One output sample of a granular voice: runs the grain clock, sums the grains, moves the scan.
    void processGrains(Voice& v, const GrainSettings& gs, bool lfosOn, bool stereo, float& outL, float& outR)
    {
        processGrainCloud(v, gs, lfosOn, stereo, outL, outR);

        const double rStart = (double) v.regionStart, rEnd = (double) v.regionEnd, rLen = rEnd - rStart;
        if (gs.scanInc != 0.0)
        {
            // The SPD LFO destination scales the scan speed.
            if (lfosOn && v.lfos.routed(LfoSpeed))
                v.position += gs.scanInc * (double) std::exp2f(SPEED_LFO_OCTAVES * v.lfos.amount(LfoSpeed));
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
    void processGrainCloud(Voice& v, const GrainSettings& gs, bool lfosOn, bool stereo, float& outL, float& outR)
    {
        const double baseInc = v.playbackRate * (double) bendRatio * v.lfoRate;

        float size = gs.size, density = gs.density;
        if (lfosOn)
        {
            if (v.lfos.routed(LfoGrainSize))
                size = std::clamp(size * std::pow(MAX_GRAIN_SIZE / MIN_GRAIN_SIZE, v.lfos.amount(LfoGrainSize)),
                                  MIN_GRAIN_SIZE, MAX_GRAIN_SIZE);
            if (v.lfos.routed(LfoGrainDens))
                density = std::clamp(density * std::pow(MAX_GRAIN_DENSITY / MIN_GRAIN_DENSITY, v.lfos.amount(LfoGrainDens)),
                                     MIN_GRAIN_DENSITY, MAX_GRAIN_DENSITY);
        }

        v.grainPhase += (double) density * gs.invSampleRate;
        if (v.grainPhase >= (double) v.grainThreshold)
        {
            v.grainPhase = std::max(0.0, v.grainPhase - (double) v.grainThreshold);
            startGrain(v, gs, size, density, baseInc, lfosOn);
            // Next interval: 1 / DENS, scattered by SPRY (mean unchanged).
            v.grainThreshold = gs.spray > 0.0f
                ? std::max(0.05f, 1.0f + gs.spray * dsp::xorshiftNoise(v.grainRng))
                : 1.0f;
        }

        const double aStart = (double) v.grainAreaStart, aEnd = (double) v.grainAreaEnd, aLen = aEnd - aStart;
        const auto& tables = grainWindowTables();
        float l = 0.0f, r = 0.0f;
        for (int gi = 0; gi < v.numGrains; )
        {
            auto& g = v.grains[gi];
            float sl, sr;
            readSampleStereoInterp(g.pos, stereo, sl, sr);

            const float x = g.phase * (float) WINDOW_TABLE_SIZE;
            const int xi = (int) x;
            const float* tbl = tables[(size_t) g.window].data();
            const float w = tbl[xi] + (x - (float) xi) * (tbl[xi + 1] - tbl[xi]);
            l += sl * w * g.gainL;
            r += sr * w * g.gainR;

            g.pos += baseInc * (double) g.ratio;
            if (g.pos > aEnd) g.pos -= aLen;
            else if (g.pos < aStart) g.pos += aLen;
            if (g.pos > aEnd || g.pos < aStart)
            {
                // A step longer than the area (a very high note over a short slice): one wrap is
                // not enough, and the grain ran further out with every sample.
                const double off = std::fmod(g.pos - aStart, aLen);
                g.pos = aStart + (off < 0.0 ? off + aLen : off);
            }

            g.phase += g.phaseInc;
            if (g.phase >= 1.0f)
                g = v.grains[--v.numGrains];    // finished: the last grain takes its place
            else
                ++gi;
        }
        outL = l;
        outR = r;
    }

    void startGrain(Voice& v, const GrainSettings& gs, float size, float density, double baseInc, bool lfosOn)
    {
        // The random numbers are drawn whether or not the grain is used, so a full pool does not
        // change what the following grains get.
        const float uPos = dsp::xorshiftNoise(v.grainRng);
        const float uPitch = dsp::xorshiftNoise(v.grainRng);
        const float uPan = dsp::xorshiftNoise(v.grainRng);
        const float uReverse = dsp::xorshiftFloat(v.grainRng);
        if (v.numGrains >= MAX_GRAINS)
            return;

        auto& g = v.grains[v.numGrains++];
        const float lengthSamples = std::max(8.0f, size * (float) currentSampleRate);
        g.phase = 0.0f;
        g.phaseInc = 1.0f / lengthSamples;
        g.window = gs.window;

        const float ratio = gs.pitchJitter > 0.0f ? std::exp2f(gs.pitchJitter * uPitch) : 1.0f;

        const double aStart = (double) v.grainAreaStart, aEnd = (double) v.grainAreaEnd, aLen = aEnd - aStart;
        double p = v.position;
        if (lfosOn && v.lfos.routed(LfoGrainPos))
            p = std::clamp(p + (double) v.lfos.amount(LfoGrainPos) * (double) (v.regionEnd - v.regionStart),
                           aStart, aEnd - 1.0);
        if (gs.spray > 0.0f)
        {
            p += (double) (gs.spray * uPos) * std::min((double) gs.sprayFrames, aLen);
            if (p < aStart) p = aStart + (aStart - p);
            else if (p > aEnd - 1.0) p = (aEnd - 1.0) - (p - (aEnd - 1.0));
        }
        const double span = (double) lengthSamples * baseInc * (double) ratio;   // frames the grain covers
        if (v.reverseGrains)
            p -= span;      // a note running backwards: the grain ends at the position
        if (span < aLen)
            p = std::min(p, aEnd - span);
        p = std::clamp(p, aStart, aEnd - 1.0);

        const bool reversed = (uReverse < gs.reverse) != v.reverseGrains;
        g.pos = reversed ? std::min(p + span, aEnd) : p;
        g.ratio = reversed ? -ratio : ratio;

        const float overlap = std::clamp(size * density, 1.0f, (float) MAX_GRAINS);
        const float gain = 1.0f / std::sqrt(overlap);
        if (gs.spread > 0.0f)
        {
            const float angle = (gs.spread * uPan + 1.0f) * 0.25f * dsp::PI_F;
            g.gainL = gain * 1.41421356f * std::cos(angle);
            g.gainR = gain * 1.41421356f * std::sin(angle);
        }
        else
        {
            g.gainL = g.gainR = gain;
        }
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
        float peak = 0.0f;
        for (float x : sampleL) peak = std::max(peak, std::abs(x));
        for (float x : sampleR) peak = std::max(peak, std::abs(x));
        normalizeGain = peak > 1.0e-4f ? std::min(1.0f / peak, 100.0f) : 1.0f;

        analyseOnsets();
        nextSlice = 0;
        if (getSliceMode() == SliceTransient)
            rebuildSlices();
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

    // lfos: for one voice, whose LFOs may be moving SIZE, XFAD, JIT or SPRD (or FADE, SLEN).
    StretchSettings readStretchSettings(int algo, const VoiceLfos* lfos = nullptr) const
    {
        auto P = [this, lfos](int id, float fallback)
        {
            return lfos != nullptr ? voiceValue(*lfos, id, fallback) : getParamValue(id, fallback);
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
            auto& g = s.grains;
            g.invSampleRate = 1.0 / sr;
            float overlap;
            if (algo == StretchSmear)
            {
                g.size = std::min(size * 8.0f, 2.0f);
                overlap = 8.0f + 24.0f * xfade;
                g.spray = 1.0f;
                g.sprayFrames = (0.05f + 0.45f * s.jitter) * (float) originalSampleRate;
            }
            else
            {
                // The grain intervals are always scattered by +-30 % (more with JIT): grains at
                // exactly regular intervals turn a steady tone into a comb at the grain rate, which
                // moves its pitch or cancels it. Scattered TIMES leave the grains lined up at speed
                // 1; JIT scatters the POSITIONS too (up to +-one grain).
                g.size = size;
                overlap = 2.0f + 8.0f * xfade;
                g.spray = 0.3f + 0.7f * s.jitter;
                g.sprayFrames = s.jitter * size * (float) originalSampleRate / g.spray;
            }
            g.density = overlap / g.size;
            g.scanInc = 0.0;
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
    void processStretch(Voice& v, PlayMode mode, const StretchSettings& st, bool lfosOn, bool stereo, float& outL, float& outR)
    {
        const double pitchInc = v.playbackRate * (double) bendRatio * v.lfoRate;
        double speed = st.speed;
        if (lfosOn && v.lfos.routed(LfoSpeed))
            speed = std::clamp(speed * (double) std::exp2f(SPEED_LFO_OCTAVES * v.lfos.amount(LfoSpeed)),
                               MIN_STRETCH_SPEED, MAX_STRETCH_SPEED);
        const double timeInc = speed * st.frameRatio;

        if (st.algo == StretchTape)
        {
            readVoice(v, mode, stereo, outL, outR);
            advanceScan(v, mode, pitchInc * speed);
        }
        else if (st.algo == StretchGrain || st.algo == StretchSmear)
        {
            v.reverseGrains = ! v.forward;
            processGrainCloud(v, st.grains, lfosOn, stereo, outL, outR);
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
                    startPos += (double) (st.jitter * dsp::xorshiftNoise(v.grainRng)) * (double) st.cycleSamples * pitchInc;
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
                totalGrains += v.numGrains;
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
                for (int gi = 0; gi < v.numGrains && written < MAX_PLAYHEAD_GRAINS; ++gi)
                    if (counter++ % step == 0)
                        playheadGrain[written++].store(norm(v.grains[gi].pos), std::memory_order_relaxed);
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
            v.numGrains = 0;
        clearPlayheads();
    }
    void endSampleChange() { changingSample.store(false); }

    std::atomic<bool> inProcess { false };
    std::atomic<bool> changingSample { false };
    int bankIndex = -1;
    uint32_t randomSeed = 0x9E3779B9u;
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
    uint32_t sliceRng = 0x2545F491u;    // PLAY = RAND
    std::atomic<int> playingSlice { -1 };

    // Onset analysis of the sample (see the transient-slicing section): mean-square level of the
    // sound and of its first difference per hop, and working space for the detection function.
    std::vector<float> onsetEnergy, onsetEnergyHigh, onsetScratch;
    int onsetHop = 128;
    float onsetPeak = 0.0f, onsetPeakHigh = 0.0f;

    // Published by the audio thread once per block (see getPlayheads). -1 = nothing there.
    std::atomic<float> playheadVoice[POOL_VOICES];
    std::atomic<float> playheadGrain[MAX_PLAYHEAD_GRAINS];
    std::atomic<int> playheadGrainCount { 0 };
    EngineLfoSection lfoSection { lfoDests() };
};
