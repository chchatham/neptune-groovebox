#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <vector>
#include "../src/engine/VoiceFilterSection.h"
#include "../src/engine/VoiceSpread.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/ModRouter.h"
#include "fast_measure.h"
#include "mod_slots_tests.h"
#include "mod_router_tests.h"     // (ModRouterAllocations: the malloc counter)

// -----------------------------------------------------------------------------
// "Voice Filter Spread": the per-voice filter section every engine shares (two filters, routing,
// balance, filter FM: engine/VoiceFilterSection.h), the seeded per-note spread (engine/
// VoiceSpread.h), and the wavetable engine on both.
//
// Held bit for bit: the section with filter B off against the single filter as it was (a copy
// below), and against a copy of the unified voice's filter path for the sounds of the P01 pin.
// NEPTUNE_FILTER_BENCH=1 prints what the old filter, the section and the engine cost and runs
// no test.
// -----------------------------------------------------------------------------

// The single per-voice filter as it was at a9e52aa, before the section had a second filter: the
// reference the section's plain path is held to, and what it is timed against. Do not "tidy" it.
class LegacyVoiceFilter
{
public:
    static constexpr float ENV_OCTAVES = 7.0f;       // envelope amount of 1 = this many octaves
    static constexpr float VEL_OCTAVES = 4.0f;
    static constexpr float WHEEL_OCTAVES = 5.0f;
    static constexpr float PRESSURE_OCTAVES = 4.0f;
    static constexpr float MAX_HZ = 20000.0f;
    static constexpr float MIN_HZ = 20.0f;

    LegacyVoiceFilter()
    {
        slot.setCutoff(0, baseTarget);
        slot.setResonance(0, 0.0f);
    }

    void setSampleRate(double sr)
    {
        sampleRate = (float) sr;
        maxHz = std::min(MAX_HZ, sampleRate * 0.49f);
        smoothCoeff = 1.0f - std::exp(-1.0f / (0.005f * sampleRate));   // 5 ms
        octaveStep = 8.0f / (0.004f * sampleRate);                      // 8 octaves in 4 ms
        driveStep = 1.0f / (0.004f * sampleRate);
        env.setSampleRate(sr);
        slot.setSampleRate(sr);
        lastHz = -1.0f;
    }

    // voiceWasSounding: a retriggered voice keeps its filter state (no click); an idle one starts clean.
    void noteOn(int note, float velocity, bool voiceWasSounding)
    {
        if (! voiceWasSounding)
        {
            slot.reset();
            env.reset();
            baseSmoothed = baseTarget;
        }
        currentNote = note;
        currentVelocity = velocity;
        updateNoteOctaves();
        // The new note takes its own values at once, as they stand at its first sample (a voice
        // struck again is declicked by its owner).
        struck = true;
        fresh = true;
        env.noteOn();
    }

    void noteOff() { env.noteOff(); }

    void reset()
    {
        slot.reset();
        env.reset();
        baseSmoothed = baseTarget;
        struck = false;
        envOctavesNow = envOctaves;
        noteOctavesNow = noteOctaves;
        externalNow = externalOctaves;
        driveNow = driveTarget;
        slot.setDrive(0, driveNow);
    }

    float process(float in)
    {
        if (bypassed)
            return in;

        const float e = env.process();

        if (baseSmoothed != baseTarget)
        {
            const float d = baseTarget - baseSmoothed;
            baseSmoothed += d * smoothCoeff;
            if (std::abs(d) < baseTarget * 1.0e-4f)
                baseSmoothed = baseTarget;
        }

        // What is set from outside under a sounding note (the envelope amount by a step lock,
        // strip and pressure once per block, the drive) is moved to over a few milliseconds:
        // opening or closing a filter in one sample is a click. The envelope itself is not held.
        if (fresh)
        {
            fresh = false;
            envOctavesNow = envOctaves;
            noteOctavesNow = noteOctaves;
            externalNow = externalOctaves;
            if (driveNow != driveTarget)
            {
                driveNow = driveTarget;
                slot.setDrive(0, driveNow);
            }
        }
        if (envOctavesNow != envOctaves)    envOctavesNow = dsp::towards(envOctavesNow, envOctaves, octaveStep);
        if (noteOctavesNow != noteOctaves)  noteOctavesNow = dsp::towards(noteOctavesNow, noteOctaves, octaveStep);
        if (externalNow != externalOctaves) externalNow = dsp::towards(externalNow, externalOctaves, octaveStep);
        if (driveNow != driveTarget)
        {
            driveNow = dsp::towards(driveNow, driveTarget, driveStep);
            slot.setDrive(0, driveNow);
        }

        const float oct = e * envOctavesNow + noteOctavesNow + externalNow + lfoOctaves;
        float hz = baseSmoothed;
        if (oct != 0.0f)
            hz *= std::exp2(oct);
        hz = std::clamp(hz, MIN_HZ, maxHz);
        if (hz != lastHz)
        {
            slot.setCutoff(0, hz);
            lastHz = hz;
        }
        return slot.process(in);
    }

    // --- settings (the same values go to every voice of an engine) ---
    void setModel(FilterModel m)
    {
        slot.setModel(0, m);
        bypassed = (m == FilterModel::Bypass);
        lastHz = -1.0f;
    }
    void setMode(int m)             { slot.setMode(0, m); }
    void setCutoff(float hz)        { baseTarget = std::clamp(hz, MIN_HZ, MAX_HZ); }
    void setResonance(float r)
    {
        resonance = std::clamp(r, 0.0f, 1.0f);
        slot.setResonance(0, std::clamp(resonance + resonanceMod, 0.0f, 1.0f));
    }
    void setDrive(float d)
    {
        driveTarget = std::clamp(d, 0.0f, 1.0f);
        if (! struck)       // nothing sounding yet: as set (process() glides to it under a note)
        {
            driveNow = driveTarget;
            slot.setDrive(0, driveNow);
        }
    }
    void setMorph(float m)          { slot.setMorph(std::clamp(m, 0.0f, 1.0f)); }
    void setEnvAmount(float a)      { envAmount = std::clamp(a, -1.0f, 1.0f); updateNoteOctaves(); }
    void setEnvVelocity(float a)    { envVelocity = std::clamp(a, 0.0f, 1.0f); updateNoteOctaves(); }
    void setKeyTrack(float a)       { keyTrack = std::clamp(a, -1.0f, 1.0f); updateNoteOctaves(); }
    void setVelAmount(float a)      { velAmount = std::clamp(a, 0.0f, 1.0f); updateNoteOctaves(); }
    void setEnvParams(const Envelope::Params& p) { env.setParams(p); }

    // Mod strip and pad pressure, already in octaves; set once per block by the engine.
    void setExternalOctaves(float oct) { externalOctaves = oct; }

    // Modulation from the voice's LFOs (EngineLfoSection), set per sample by the voice while an LFO
    // is routed here and back to zero when none is: the cutoff in octaves, the resonance as an
    // offset from its set value. Zero leaves the filter exactly as it is without them.
    void setLfoOctaves(float oct) { lfoOctaves = oct; }
    void setResonanceMod(float offset)
    {
        if (offset == resonanceMod)
            return;
        resonanceMod = offset;
        slot.setResonance(0, std::clamp(resonance + resonanceMod, 0.0f, 1.0f));
    }

    FilterSlot& getSlot() { return slot; }
    Envelope& getEnvelope() { return env; }
    float getCurrentCutoff() const { return lastHz; }

private:
    void updateNoteOctaves()
    {
        noteOctaves = keyTrack * (float) (currentNote - 60) / 12.0f + velAmount * currentVelocity * VEL_OCTAVES;
        envOctaves = envAmount * (1.0f - envVelocity + envVelocity * currentVelocity) * ENV_OCTAVES;
    }

    FilterSlot slot;
    Envelope env;
    float envOctavesNow = 0.0f, noteOctavesNow = 0.0f, externalNow = 0.0f, octaveStep = 0.04f;
    float driveTarget = 0.0f, driveNow = 0.0f, driveStep = 0.005f;
    bool struck = false, fresh = true;
    float sampleRate = 44100.0f;
    float maxHz = MAX_HZ;
    float smoothCoeff = 0.0045f;
    float baseTarget = MAX_HZ;
    float baseSmoothed = MAX_HZ;
    float lastHz = -1.0f;
    float envAmount = 0.0f, envVelocity = 0.0f, keyTrack = 0.0f, velAmount = 0.0f;
    float noteOctaves = 0.0f, envOctaves = 0.0f, externalOctaves = 0.0f, lfoOctaves = 0.0f;
    float resonance = 0.0f, resonanceMod = 0.0f;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool bypassed = false;
};

// The unified voice's filter path as it is in UnifiedVoice.h at 6bf3c48 (process(): from "Filter
// cutoff: envelope, key tracking ..." to filterSlot.process; glide(): the filter's values), with
// the voice's other modulation at rest (no LFO, no pressure, no mod matrix, no envelope chain).
// What the shared section is held to for the sounds of the P01 pin. Do not "tidy" it.
struct UnifiedFilterPathCopy
{
    static constexpr float FILTER_ENV_OCTAVES = 7.0f;

    void setSampleRate(double sr)
    {
        filterEnv.setSampleRate(sr);
        filterSlot.setSampleRate(sr);
        levelStep = (float) (1.0 / (0.004 * sr));
        octaveRatio = (float) std::exp2(8.0 / (0.004 * sr));
    }
    void noteOn(int note, float velocity)
    {
        currentPitch = (float) note;
        currentVelocity = velocity;
        struck = true;
        filterEnv.noteOn();
    }
    void noteOff() { filterEnv.noteOff(); }

    float process(float oscMix, float lastOsc2)
    {
        glide();
        const float modCutoffOffset = 0.0f, modResonanceOffset = 0.0f, lfoCutoffOct = 0.0f, lfoCutoffBOct = 0.0f;
        const float lfoFilterFmMod = 0.0f, atCutoffNow = 0.0f;

        float filterEnvVal = filterEnv.process();
        const float filterFmNow = lfoFilterFmMod != 0.0f ? std::clamp(filterFm + lfoFilterFmMod, 0.0f, 1.0f) : filterFm;
        float cutoffOct = filterFmNow * lastOsc2 * 2.0f     // audio-rate cutoff modulation from oscillator 2
                        + filterEnvVal * filterEnvAmountNow * FILTER_ENV_OCTAVES
                        + filterKeyTrack * (currentPitch - 60.0f) / 12.0f
                        + filterVelAmount * currentVelocity * 4.0f
                        + lfoCutoffOct
                        + atCutoffNow * 4.0f
                        + noteFilterOct;
        float cutoff = filterCutoffNow * (cutoffOct != 0.0f ? std::exp2(cutoffOct) : 1.0f) + modCutoffOffset;
        // Filter B follows that modulation fully, partly or not at all.
        float cutoffB = filterCutoffNow * (cutoffOct != 0.0f ? std::exp2(cutoffOct * filterBTrack) : 1.0f)
                        * filterBRatio + modCutoffOffset * filterBTrack;
        if (lfoCutoffBOct != 0.0f)
            cutoffB *= std::exp2(lfoCutoffBOct);

        cutoff = juce::jlimit(20.0f, 20000.0f, cutoff);
        filterSlot.setCutoff(0, cutoff);
        if (filterSlot.getModelB() != FilterModel::Bypass)
            filterSlot.setCutoff(1, juce::jlimit(20.0f, 20000.0f, cutoffB));

        if (modResonanceOffset != 0.0f)
        {
            filterSlot.setResonance(0, std::clamp(resonanceNow + modResonanceOffset, 0.0f, 1.0f));
            filterSlot.setResonance(1, std::clamp(resonanceBNow + modResonanceOffset, 0.0f, 1.0f));
            resonanceModulated = true;
        }
        else if (resonanceModulated || resonanceGlides)
        {
            filterSlot.setResonance(0, resonanceNow);
            filterSlot.setResonance(1, resonanceBNow);
            resonanceModulated = false;
        }

        return filterSlot.process(oscMix);
    }

    void glide()
    {
        if (struck)
        {
            struck = false;
            resonanceGlides = resonanceNow != filterBaseResonance || resonanceBNow != filterResonanceB;
            resonanceNow = filterBaseResonance;
            resonanceBNow = filterResonanceB;
            if (balanceNow != filterBalance)
            {
                balanceNow = filterBalance;
                filterSlot.setBalance(balanceNow);
            }
            filterCutoffNow = filterBaseCutoff;
            filterEnvAmountNow = filterEnvAmount;
        }
        else
        {
            resonanceGlides = resonanceNow != filterBaseResonance || resonanceBNow != filterResonanceB;
            if (resonanceGlides)
            {
                resonanceNow = dsp::towards(resonanceNow, filterBaseResonance, levelStep);
                resonanceBNow = dsp::towards(resonanceBNow, filterResonanceB, levelStep);
            }
            if (balanceNow != filterBalance)
            {
                balanceNow = dsp::towards(balanceNow, filterBalance, levelStep);
                filterSlot.setBalance(balanceNow);
            }
            if (filterCutoffNow != filterBaseCutoff)
                filterCutoffNow = filterCutoffNow < filterBaseCutoff ? std::min(filterBaseCutoff, filterCutoffNow * octaveRatio)
                                                                     : std::max(filterBaseCutoff, filterCutoffNow / octaveRatio);
            if (filterEnvAmountNow != filterEnvAmount)  filterEnvAmountNow = dsp::towards(filterEnvAmountNow, filterEnvAmount, levelStep);
        }
    }

    // (The voice's setters, as they act on a voice that is not sounding.)
    void setFilterResonanceA(float q) { filterBaseResonance = q; filterSlot.setResonance(0, q); }
    void setFilterResonanceB(float q) { filterResonanceB = q; filterSlot.setResonance(1, q); }
    void setFilterBalance(float b) { filterBalance = b; filterSlot.setBalance(b); }
    void setFilterBOffset(float semitones) { filterBRatio = std::exp2(semitones / 12.0f); }

    Envelope filterEnv;
    FilterSlot filterSlot;
    float filterBaseCutoff = 20000.0f, filterBaseResonance = 0.0f, filterEnvAmount = 0.0f, filterKeyTrack = 0.0f;
    float filterVelAmount = 0.0f, filterBRatio = 1.0f, filterResonanceB = 0.0f, filterBTrack = 1.0f, filterFm = 0.0f;
    float noteFilterOct = 0.0f;
    float currentPitch = 60.0f, currentVelocity = 1.0f;
    float resonanceNow = 0.0f, resonanceBNow = 0.0f, filterBalance = 0.5f, balanceNow = 0.5f;
    float filterCutoffNow = 20000.0f, filterEnvAmountNow = 0.0f;
    float levelStep = 0.005f, octaveRatio = 1.03f;
    bool struck = true, resonanceGlides = false, resonanceModulated = false;
};

class VoiceFilterSpreadTests : public juce::UnitTest
{
public:
    VoiceFilterSpreadTests() : UnitTest("Voice Filter Spread") {}

    using F = VoiceFilterSection;
    using W = WavetableEngine;

    // ---- signals and measurements -------------------------------------------------------------
    static double rms(const std::vector<float>& x, size_t from = 0)
    {
        double sum = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            sum += (double) x[i] * x[i];
        return x.size() > from ? std::sqrt(sum / (double) (x.size() - from)) : 0.0;
    }
    static double rmsOfDifference(const std::vector<float>& a, const std::vector<float>& b)
    {
        std::vector<float> d(std::min(a.size(), b.size()));
        for (size_t i = 0; i < d.size(); ++i)
            d[i] = a[i] - b[i];
        return rms(d);
    }
    static bool same(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && ! a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }
    static bool bounded(const std::vector<float>& x, float limit)
    {
        for (float v : x)
            if (! std::isfinite(v) || std::abs(v) > limit)
                return false;
        return true;
    }
    static float largestStep(const std::vector<float>& x, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i)
            m = std::max(m, std::abs(x[i] - x[i - 1]));
        return m;
    }
    // A saw with noise on it: every harmonic, and something no filter can lock to.
    static std::vector<float> rich(double sr, int n, double hz = 110.0, float noise = 0.2f)
    {
        std::vector<float> x((size_t) n);
        uint32_t seed = 0x1234567u;
        double phase = 0.0;
        for (auto& v : x)
        {
            v = (float) (2.0 * phase - 1.0) * 0.6f + noise * dsp::xorshiftNoise(seed);
            phase = dsp::wrapPhase(phase + hz / sr);
        }
        return x;
    }
    static std::vector<float> sine(double sr, int n, double hz, float level = 0.5f)
    {
        std::vector<float> x((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = level * (float) std::sin(2.0 * 3.14159265358979323846 * hz * (double) i / sr);
        return x;
    }
    static double level(const std::vector<float>& x, double hz, double sr)
    {
        return std::sqrt(fastmeasure::goertzel(x, x.size() / 4, x.size() - x.size() / 4, hz, sr));
    }

    // ---- the single filter as it was against the section ---------------------------------------
    // One script for both classes: notes, a retrigger, the envelope, key and velocity amounts,
    // strip octaves by the block, LFO octaves and a resonance offset by the sample, a cutoff and
    // a drive turned under the note.
    template <typename Filter>
    static std::vector<float> singleScript(double sr, int variant)
    {
        Filter f;
        f.setSampleRate(sr);
        static const FilterModel models[] = { FilterModel::SVF, FilterModel::Ladder, FilterModel::SEM, FilterModel::Diode,
                                              FilterModel::Comb, FilterModel::Bypass };
        f.setModel(models[variant % 6]);
        f.setMode(variant % 3);
        f.setCutoff(300.0f + 500.0f * (float) variant);
        f.setResonance(0.1f * (float) (variant % 8));
        f.setDrive(variant % 2 == 0 ? 0.0f : 0.6f);
        f.setMorph(0.3f);
        f.setEnvAmount(variant % 3 == 0 ? 0.0f : 0.5f);
        f.setEnvVelocity(0.4f);
        f.setKeyTrack(0.5f);
        f.setVelAmount(0.25f);
        Envelope::Params p;
        p.attack = 0.02f; p.decay = 0.2f; p.sustain = 0.3f; p.release = 0.1f;
        f.setEnvParams(p);

        const int n = (int) (sr * 0.6);
        const auto in = rich(sr, n);
        std::vector<float> out((size_t) n);
        bool sounding = false;
        for (int i = 0; i < n; ++i)
        {
            if (i == 0)              { f.noteOn(48, 0.8f, sounding); sounding = true; }
            if (i == n / 3)          f.noteOn(67, 0.4f, sounding);      // struck again while sounding
            if (i == n / 2)          f.setCutoff(4000.0f);
            if (i == n / 2 + 100)    f.setDrive(0.9f);
            if (i == (2 * n) / 3)    f.noteOff();
            if (i % 64 == 0)         f.setExternalOctaves(variant % 2 == 0 ? 0.0f : 0.5f * (float) std::sin(i * 0.0005));
            if (variant >= 3)
            {
                f.setLfoOctaves(1.5f * (float) std::sin(i * 0.003));
                f.setResonanceMod(variant >= 5 ? 0.2f * (float) std::sin(i * 0.002) : 0.0f);
            }
            out[(size_t) i] = f.process(in[(size_t) i]);
        }
        return out;
    }

    // ---- the unified voice's path against the section ------------------------------------------
    struct Pinned
    {
        const char* name;
        FilterModel a, b;
        int modeA, modeB;
        FilterRouting routing;
        float cutoff, reso, resoB, drive, driveB, morph, balance, env, key, vel, offsetB, trackB;
    };
    static std::vector<Pinned> pinned()
    {
        return {
            // "User 13": filter A SVF LP with MORP, filter B SVF LP, PAR, BAL 0.5, DRIV / DRVB, key tracking
            { "SVF LP + SVF LP, PAR", FilterModel::SVF, FilterModel::SVF, 0, 0, FilterRouting::Parallel,
              1800.0f, 0.35f, 0.2f, 0.4f, 0.25f, 0.3f, 0.5f, 0.45f, 0.6f, 0.3f, 0.0f, 1.0f },
            // "DuneDrone": ladder LP24, single, with drive
            { "ladder LP24, SNGL", FilterModel::Ladder, FilterModel::Bypass, 0, 0, FilterRouting::Single,
              900.0f, 0.5f, 0.2f, 0.7f, 0.0f, 0.0f, 0.5f, 0.6f, 0.3f, 0.2f, 0.0f, 1.0f },
            // Beyond the pin: filter B at its own offset following half of A's modulation, in series
            { "SEM + diode HP, SER, OFSB, TRKB", FilterModel::SEM, FilterModel::Diode, 0, 1, FilterRouting::Series,
              1200.0f, 0.4f, 0.6f, 0.3f, 0.5f, 0.7f, 0.3f, -0.5f, 1.0f, 0.5f, 7.0f, 0.5f },
            { "SVF BP + ladder LP12, PAR, BAL 0.8", FilterModel::SVF, FilterModel::Ladder, 1, 2, FilterRouting::Parallel,
              600.0f, 0.7f, 0.3f, 0.0f, 0.4f, 0.0f, 0.8f, 0.8f, 0.0f, 0.0f, -12.0f, 0.25f } };
    }
    static Envelope::Params pinnedEnvelope()
    {
        Envelope::Params p;
        p.attack = 0.015f; p.decay = 0.25f; p.sustain = 0.35f; p.release = 0.2f;
        return p;
    }
    // cutoffBetween: a cutoff set just before the second note (0 = none): that voice takes it at
    // once, with its filters' state as the first note left it.
    static std::vector<float> unifiedCopy(const Pinned& c, double sr, const std::vector<float>& in, float cutoffBetween = 0.0f)
    {
        UnifiedFilterPathCopy u;
        u.setSampleRate(sr);
        u.filterSlot.setModel(0, c.a);      u.filterSlot.setMode(0, c.modeA);
        u.filterSlot.setModel(1, c.b);      u.filterSlot.setMode(1, c.modeB);
        u.filterSlot.setRouting(c.routing);
        u.filterBaseCutoff = c.cutoff;
        u.setFilterResonanceA(c.reso);      u.setFilterResonanceB(c.resoB);
        u.filterSlot.setDrive(0, c.drive);  u.filterSlot.setDrive(1, c.driveB);
        u.filterSlot.setMorph(c.morph);
        u.setFilterBalance(c.balance);
        u.filterEnvAmount = c.env;          u.filterKeyTrack = c.key;       u.filterVelAmount = c.vel;
        u.setFilterBOffset(c.offsetB);      u.filterBTrack = c.trackB;
        u.filterEnv.setParams(pinnedEnvelope());
        std::vector<float> out(in.size());
        const size_t n = in.size();
        for (size_t i = 0; i < n; ++i)
        {
            if (i == 0)             u.noteOn(45, 0.85f);
            if (i == n / 3 && cutoffBetween > 0.0f) u.filterBaseCutoff = cutoffBetween;
            if (i == n / 3)         u.noteOn(64, 0.5f);
            if (i == (2 * n) / 3)   u.noteOff();
            out[i] = u.process(in[i], 0.0f);
        }
        return out;
    }
    // The section, with the octaves added up by the caller in the unified voice's order.
    // keeping: every note through noteOnKeepingState(), as the unified voice will start its notes.
    static std::vector<float> sectionAsUnified(const Pinned& c, double sr, const std::vector<float>& in,
                                               float cutoffBetween = 0.0f, bool keeping = false)
    {
        VoiceFilter f;
        f.setSampleRate(sr);
        f.setModel(c.a);        f.setMode(c.modeA);
        f.setModelB(c.b);       f.setModeB(c.modeB);
        f.setRouting(c.routing);
        f.setCutoff(c.cutoff);
        f.setResonance(c.reso); f.setResonanceB(c.resoB);
        f.setDrive(c.drive);    f.setDriveB(c.driveB);
        f.setMorph(c.morph);
        f.setBalance(c.balance);
        f.setOffsetB(c.offsetB);
        f.setTrackB(c.trackB);
        f.setEnvParams(pinnedEnvelope());
        std::vector<float> out(in.size());
        const size_t n = in.size();
        float pitch = 60.0f, velocity = 1.0f;
        for (size_t i = 0; i < n; ++i)
        {
            if (i == n / 3 && cutoffBetween > 0.0f) f.setCutoff(cutoffBetween);
            if (i == 0 || i == n / 3)
            {
                pitch = i == 0 ? 45.0f : 64.0f;
                velocity = i == 0 ? 0.85f : 0.5f;
                if (keeping)    f.noteOnKeepingState((int) pitch, velocity);
                else            f.noteOn((int) pitch, velocity, i != 0);
            }
            if (i == (2 * n) / 3)   f.noteOff();
            const float e = f.tickEnvelope();
            const float oct = 0.0f * 0.0f * 2.0f
                            + e * c.env * UnifiedFilterPathCopy::FILTER_ENV_OCTAVES
                            + c.key * (pitch - 60.0f) / 12.0f
                            + c.vel * velocity * 4.0f
                            + 0.0f
                            + 0.0f * 4.0f
                            + 0.0f;
            out[i] = f.processOctaves(in[i], oct);
        }
        return out;
    }

    // ---- one filter of the section, set up by a lambda, run over an input -----------------------
    static std::vector<float> runFilter(double sr, const std::vector<float>& in, const std::function<void(VoiceFilter&)>& setup,
                                        const std::vector<float>* fm = nullptr,
                                        const std::function<void(VoiceFilter&, size_t)>& perSample = nullptr)
    {
        VoiceFilter f;
        f.setSampleRate(sr);
        Envelope::Params p;
        p.attack = 0.001f; p.decay = 0.1f; p.sustain = 1.0f; p.release = 0.1f;
        f.setEnvParams(p);
        setup(f);
        f.noteOn(60, 1.0f, false);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (perSample)
                perSample(f, i);
            out[i] = f.process(in[i], fm != nullptr ? (*fm)[i] : 0.0f);
        }
        return out;
    }

    // ---- the wavetable engine -----------------------------------------------------------------
    static std::unique_ptr<W> wavetable(double sr = 48000.0)
    {
        auto e = std::make_unique<W>();
        e->prepareToPlay(sr, 64);
        e->setParameter(W::TableSelect, 1.0f);
        e->setParameter(W::Position, 0.8f);
        e->setParameter(W::AmpAttack, 0.002f);
        e->setParameter(W::AmpSustain, 1.0f);
        e->setParameter(W::AmpRelease, 0.05f);
        return e;
    }
    // `notes` notes one after the other, each `noteSeconds` long (gate 80 %): left then right.
    static std::array<std::vector<float>, 2> play(EngineBase& e, int notes, double noteSeconds, double sr = 48000.0,
                                                  int firstNote = 57, int noteStep = 0, int block = 64)
    {
        std::array<std::vector<float>, 2> out;
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        const int blocksPerNote = std::max(2, (int) (noteSeconds * sr / block));
        for (int n = 0; n < notes; ++n)
            for (int b = 0; b < blocksPerNote; ++b)
            {
                if (b == 0)                             e.noteOn(firstNote + n * noteStep, 0.8f);
                if (b == (blocksPerNote * 4) / 5)       e.noteOff(firstNote + n * noteStep);
                buf.clear();
                e.processBlock(buf, midi);
                for (int ch = 0; ch < 2; ++ch)
                    out[(size_t) ch].insert(out[(size_t) ch].end(), buf.getReadPointer(ch), buf.getReadPointer(ch) + block);
            }
        return out;
    }
    static const ParamSpec* cell(const ParamPages& pages, const char* page, const char* label)
    {
        for (auto& p : pages)
            if (juce::String(p.name) == page)
                for (auto& s : p.params)
                    if (s.isValid() && juce::String(s.label) == label)
                        return &s;
        return nullptr;
    }

    // ---- timing (NEPTUNE_FILTER_BENCH=1) --------------------------------------------------------
    template <typename Filter, typename Process>
    static double microsPerBlock(const std::function<void(Filter&)>& setup, Process&& process)
    {
        constexpr int VOICES = 8, BLOCK = 512, BLOCKS = 400;
        const double sr = 48000.0;
        std::vector<Filter> voices((size_t) VOICES);
        int note = 40;
        for (auto& f : voices)
        {
            f.setSampleRate(sr);
            setup(f);
            f.noteOn(note += 5, 0.8f, false);
        }
        const auto in = rich(sr, BLOCK);
        std::vector<float> fm = sine(sr, BLOCK, 220.0, 1.0f);
        double best = 1.0e30;
        float sink = 0.0f;
        for (int pass = 0; pass < 7; ++pass)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < BLOCKS; ++b)
                for (auto& f : voices)
                    for (int i = 0; i < BLOCK; ++i)
                        sink += process(f, in[(size_t) i], fm[(size_t) i]);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / BLOCKS;
            best = std::min(best, us);
        }
        juce::ignoreUnused(sink);
        return best;
    }
    void bench()
    {
        // A filter that works: closed half way. (The envelope has reached its sustain in the passes
        // that count: the cutoff stands still, which is where one more flag per sample would show
        // most. "moving": the cutoff moved every sample, a new set of coefficients each time.)
        auto single = [](auto& f)
        {
            f.setModel(FilterModel::SVF);
            f.setCutoff(900.0f);
            f.setResonance(0.4f);
            f.setEnvAmount(0.5f);
            Envelope::Params p;
            p.attack = 2.0f; p.decay = 8.0f; p.sustain = 0.5f; p.release = 1.0f;
            f.setEnvParams(p);
        };
        auto ladder = [single](auto& f) { single(f); f.setModel(FilterModel::Ladder); f.setDrive(0.4f); };
        auto old = [](LegacyVoiceFilter& f, float x, float) { return f.process(x); };
        auto now = [](VoiceFilter& f, float x, float) { return f.process(x); };
        auto nowFm = [](VoiceFilter& f, float x, float fm) { return f.process(x, fm); };
        auto oldMoving = [](LegacyVoiceFilter& f, float x, float m) { f.setLfoOctaves(m); return f.process(x); };
        auto nowMoving = [](VoiceFilter& f, float x, float m) { f.setLfoOctaves(m); return f.process(x); };
        logMessage("microseconds per 512-sample block, 8 voices, 48 kHz (best of 7 passes of 400 blocks), run alternately:");
        for (int round = 0; round < 3; ++round)
        {
            const double a = microsPerBlock<LegacyVoiceFilter>(single, old);
            const double b = microsPerBlock<VoiceFilter>(single, now);
            const double c = microsPerBlock<LegacyVoiceFilter>(ladder, old);
            const double d = microsPerBlock<VoiceFilter>(ladder, now);
            const double m1 = microsPerBlock<LegacyVoiceFilter>(single, oldMoving);
            const double m2 = microsPerBlock<VoiceFilter>(single, nowMoving);
            logMessage("  round " + juce::String(round + 1) + "   SVF LP: single filter as it was " + juce::String(a, 1)
                       + ", the section (B off, no FM) " + juce::String(b, 1) + "   ladder LP24: " + juce::String(c, 1) + " / " + juce::String(d, 1)
                       + "   SVF LP, moving: " + juce::String(m1, 1) + " / " + juce::String(m2, 1));
        }
        const double par = microsPerBlock<VoiceFilter>([single](VoiceFilter& f)
            { single(f); f.setModelB(FilterModel::SVF); f.setRouting(FilterRouting::Parallel); f.setOffsetB(7.0f); }, now);
        const double ser = microsPerBlock<VoiceFilter>([single](VoiceFilter& f)
            { single(f); f.setModelB(FilterModel::Ladder); f.setRouting(FilterRouting::Series); }, now);
        const double fm = microsPerBlock<VoiceFilter>([single](VoiceFilter& f) { single(f); f.setFmDepth(0.5f); }, nowFm);
        logMessage("  the section with what is new in use:   SVF + SVF in PAR " + juce::String(par, 1) + "   SVF into ladder, SER "
                   + juce::String(ser, 1) + "   SVF with FFM " + juce::String(fm, 1));

        // The wavetable engine, eight voices held.
        for (int dual = 0; dual < 2; ++dual)
        {
            auto e = wavetable();
            e->setParameter(W::FilterBase + F::Cutoff, 900.0f);
            e->setParameter(W::FilterBase + F::EnvAmount, 0.5f);
            if (dual == 1)
            {
                e->setParameter(W::FilterDualBase, 0.0f);                                      // TYPB: SVF
                e->setParameter(W::FilterDualBase + F::Routing - F::Count, 2.0f);
                e->setParameter(W::FilterDualBase + F::FilterFM - F::Count, 0.5f);
            }
            for (int n = 0; n < 8; ++n)
                e->noteOn(40 + 5 * n, 0.8f, -1);
            juce::AudioBuffer<float> buf(2, 512);
            juce::MidiBuffer midi;
            double best = 1.0e30;
            for (int pass = 0; pass < 7; ++pass)
            {
                const auto t0 = std::chrono::steady_clock::now();
                for (int b = 0; b < 100; ++b)
                {
                    buf.clear();
                    e->processBlock(buf, midi);
                }
                best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 100.0);
            }
            logMessage(juce::String("  the wavetable engine, 8 voices, ") + (dual == 1 ? "A + B in PAR with FFM: " : "filter A only:          ")
                       + juce::String(best, 1));
        }
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_FILTER_BENCH", {}).isNotEmpty())
        {
            beginTest("Timed");
            bench();
            expect(true);
            return;
        }

        beginTest("Filter B off, no FM: the section is the single filter as it was, bit for bit");
        {
            for (double sr : { 22050.0, 44100.0, 48000.0, 96000.0 })
                for (int variant = 0; variant < 12; ++variant)
                {
                    const auto before = singleScript<LegacyVoiceFilter>(sr, variant);
                    const auto after = singleScript<VoiceFilter>(sr, variant);
                    expect(rms(before) > 0.01, "the script sounds");
                    expect(same(before, after), "variant " + juce::String(variant) + " at " + juce::String(sr) + " Hz");
                }
        }

        beginTest("The unified voice's filter path, sample for sample: SVF LP + SVF LP in PAR, ladder LP24, and beyond");
        {
            for (double sr : { 44100.0, 48000.0, 96000.0 })
                for (auto& c : pinned())
                {
                    const auto in = rich(sr, (int) (sr * 0.75), 82.4);
                    const auto reference = unifiedCopy(c, sr, in);
                    const auto section = sectionAsUnified(c, sr, in);
                    expect(rms(reference) > 0.01, juce::String(c.name) + " sounds");
                    expect(rmsOfDifference(reference, in) > 0.01, juce::String(c.name) + " filters");
                    expect(same(reference, section), juce::String(c.name) + " at " + juce::String(sr) + " Hz: rms of the difference "
                                                     + juce::String(rmsOfDifference(reference, section), 9));
                }
        }

        beginTest("The unified voice's note-on: the filters' state kept, a cutoff set between two notes taken at once");
        {
            // (The same was run beside the real UnifiedVoice playing the P01 fixture, by a harness
            // that replaced its FilterSlot with a recording one: see VoiceFilterSection.h.)
            for (double sr : { 44100.0, 48000.0 })
                for (auto& c : pinned())
                {
                    const auto in = rich(sr, (int) (sr * 0.75), 82.4);
                    const float between = c.cutoff * 1.7f;
                    const auto reference = unifiedCopy(c, sr, in, between);
                    expect(! same(reference, unifiedCopy(c, sr, in)), "the cutoff set between the notes is heard");
                    expect(same(reference, sectionAsUnified(c, sr, in, between, true)),
                           juce::String(c.name) + " at " + juce::String(sr) + " Hz, noteOnKeepingState: rms of the difference "
                           + juce::String(rmsOfDifference(reference, sectionAsUnified(c, sr, in, between, true)), 9));
                    expect(same(unifiedCopy(c, sr, in), sectionAsUnified(c, sr, in, 0.0f, true)), juce::String(c.name) + ": and with nothing set between");
                    expect(! same(reference, sectionAsUnified(c, sr, in, between, false)),
                           "noteOn(.., true) lets that cutoff glide into the note, as the single filter did: not the unified voice's samples");
                }
            // A voice that was silent: noteOn(.., false) starts from cleared filters, noteOnKeepingState from what is in them.
            const auto in = rich(48000.0, 9600);
            auto second = [&](bool keeping)
            {
                VoiceFilter f;
                f.setSampleRate(48000.0);
                f.setModel(FilterModel::SVF); f.setCutoff(300.0f); f.setResonance(0.95f);
                f.noteOn(60, 1.0f, false);
                for (size_t i = 0; i < in.size(); ++i) f.process(in[i]);       // the filter rings
                if (keeping) f.noteOnKeepingState(60, 1.0f); else f.noteOn(60, 1.0f, false);
                std::vector<float> out(2400);
                for (auto& v : out) v = f.process(0.0f);                        // silence in: only what the filter held
                return out;
            };
            expect(rms(second(false)) == 0.0, "a new note on a silent voice: cleared");
            expect(rms(second(true)) > 0.01, "noteOnKeepingState: the ring goes on (" + juce::String(rms(second(true)), 4) + ")");
        }

        beginTest("Serial and parallel routing are the two single filters, one into the other and side by side");
        {
            const double sr = 48000.0;
            const auto in = rich(sr, 24000);
            auto a = [](VoiceFilter& f) { f.setModel(FilterModel::SVF); f.setCutoff(800.0f); f.setResonance(0.5f); f.setDrive(0.3f); };
            auto bAlone = [](VoiceFilter& f) { f.setModel(FilterModel::Ladder); f.setMode(2); f.setCutoff(1600.0f); f.setResonance(0.3f); f.setDrive(0.2f); };
            auto bOfDual = [](VoiceFilter& f) { f.setModelB(FilterModel::Ladder); f.setModeB(2); f.setOffsetB(12.0f); f.setResonanceB(0.3f); f.setDriveB(0.2f); };
            const auto onlyA = runFilter(sr, in, a);
            const auto onlyB = runFilter(sr, in, bAlone);
            const auto aIntoB = runFilter(sr, onlyA, bAlone);
            const auto serial = runFilter(sr, in, [&](VoiceFilter& f) { a(f); bOfDual(f); f.setRouting(FilterRouting::Series); });
            expect(rmsOfDifference(serial, onlyA) > 0.01, "filter B changes the sound in series");
            expect(rmsOfDifference(serial, aIntoB) < 1.0e-6, "SER is A into B: " + juce::String(rmsOfDifference(serial, aIntoB), 9));

            for (float bal : { 0.0f, 0.3f, 0.5f, 1.0f })
            {
                const auto parallel = runFilter(sr, in, [&](VoiceFilter& f) { a(f); bOfDual(f); f.setRouting(FilterRouting::Parallel); f.setBalance(bal); });
                std::vector<float> mixed(in.size());
                for (size_t i = 0; i < in.size(); ++i)
                    mixed[i] = onlyA[i] * (1.0f - bal) + onlyB[i] * bal;
                expect(rmsOfDifference(parallel, mixed) < 1.0e-6, "PAR at BAL " + juce::String(bal) + ": " + juce::String(rmsOfDifference(parallel, mixed), 9));
            }
            // ROUT at SNGL with a filter B chosen is filter A alone; B off in PAR mixes in the unfiltered sound.
            const auto singleWithB = runFilter(sr, in, [&](VoiceFilter& f) { a(f); bOfDual(f); });
            expect(same(singleWithB, onlyA), "SNGL ignores filter B");
            const auto dry = runFilter(sr, in, [&](VoiceFilter& f) { a(f); f.setRouting(FilterRouting::Parallel); f.setBalance(1.0f); });
            expect(rmsOfDifference(dry, in) < 1.0e-6, "PAR with B off at BAL 100 is the unfiltered sound");
        }

        beginTest("BAL moves between the two filters: a low-pass and a high-pass, a low and a high tone");
        {
            const double sr = 48000.0;
            const auto low = sine(sr, 24000, 100.0), high = sine(sr, 24000, 8000.0);
            auto setup = [](float bal)
            {
                return [bal](VoiceFilter& f)
                {
                    f.setModel(FilterModel::SVF); f.setMode(0); f.setCutoff(400.0f);
                    f.setModelB(FilterModel::SVF); f.setModeB(2); f.setOffsetB(36.0f);     // high-pass at 3.2 kHz
                    f.setResonance(0.12f); f.setResonanceB(0.12f);                         // (a Q of 0.71: flat to the corner)
                    f.setRouting(FilterRouting::Parallel);
                    f.setBalance(bal);
                };
            };
            const double in = rms(low, 4800);
            const double lowAtA = rms(runFilter(sr, low, setup(0.0f)), 4800), highAtA = rms(runFilter(sr, high, setup(0.0f)), 4800);
            const double lowAtB = rms(runFilter(sr, low, setup(1.0f)), 4800), highAtB = rms(runFilter(sr, high, setup(1.0f)), 4800);
            const double lowMid = rms(runFilter(sr, low, setup(0.5f)), 4800), highMid = rms(runFilter(sr, high, setup(0.5f)), 4800);
            expect(lowAtA > 0.9 * in && highAtA < 0.02 * in, "BAL 0: the low-pass alone (" + juce::String(lowAtA / in, 3) + ", " + juce::String(highAtA / in, 3) + ")");
            expect(highAtB > 0.9 * in && lowAtB < 0.02 * in, "BAL 100: the high-pass alone (" + juce::String(lowAtB / in, 3) + ", " + juce::String(highAtB / in, 3) + ")");
            expectWithinAbsoluteError(lowMid / in, 0.5 * lowAtA / in, 0.02, "BAL 50: half of the low-pass");
            expectWithinAbsoluteError(highMid / in, 0.5 * highAtB / in, 0.02, "BAL 50: half of the high-pass");
        }

        beginTest("Filter FM: the signal handed in moves the cutoff at audio rate, and sidebands appear");
        {
            const double sr = 48000.0, tone = 1000.0, mod = 230.0;
            const int n = 48000;
            const auto in = sine(sr, n, tone);
            const auto fm = sine(sr, n, mod, 1.0f);
            auto setup = [](float depth, bool both)
            {
                return [depth, both](VoiceFilter& f)
                {
                    f.setModel(FilterModel::SVF); f.setCutoff(1000.0f); f.setResonance(0.6f);
                    if (both) { f.setModelB(FilterModel::SVF); f.setRouting(FilterRouting::Series); }
                    f.setFmDepth(depth);
                };
            };
            auto sideband = [&](const std::vector<float>& y) { return (level(y, tone + mod, sr) + level(y, tone - mod, sr)) / level(y, tone, sr); };
            const auto off = runFilter(sr, in, setup(0.0f, false), &fm);
            const auto unread = runFilter(sr, in, setup(0.0f, false));
            const auto little = runFilter(sr, in, setup(0.1f, false), &fm);
            const auto half = runFilter(sr, in, setup(0.4f, false), &fm);
            const auto full = runFilter(sr, in, setup(1.0f, false), &fm);
            const auto both = runFilter(sr, in, setup(1.0f, true), &fm);
            const auto bOnly = runFilter(sr, in, setup(0.0f, true), &fm);
            expect(same(off, unread), "FFM at 0: the signal is not read");
            expect(sideband(off) < 0.001, "no sidebands without it: " + juce::String(sideband(off), 5));
            expect(sideband(little) > 0.02, "sidebands at FFM 10: " + juce::String(sideband(little), 4));
            expect(sideband(half) > 2.0 * sideband(little), "more at FFM 40: " + juce::String(sideband(half), 4));
            expect(sideband(full) > 0.5, "and at FFM 100: " + juce::String(sideband(full), 4));
            logMessage("filter FM sidebands over the tone: FFM 10 " + juce::String(sideband(little), 3) + ", 40 "
                       + juce::String(sideband(half), 3) + ", 100 " + juce::String(sideband(full), 3));
            expect(sideband(both) > 0.05 && rmsOfDifference(both, full) > 0.01, "filter B is moved with A");
            expect(sideband(bOnly) < 0.001, "and stands still without FM");
            expect(bounded(full, 8.0f) && bounded(both, 8.0f), "bounded");
            // What is not a number, or far too large, as the modulator: the cutoff stays in its range.
            std::vector<float> wild((size_t) n);
            for (size_t i = 0; i < wild.size(); ++i)
                wild[i] = i % 3 == 0 ? std::numeric_limits<float>::quiet_NaN() : (i % 3 == 1 ? 1.0e30f : -std::numeric_limits<float>::infinity());
            expect(bounded(runFilter(sr, in, setup(1.0f, true), &wild), 8.0f), "a modulator that is not a number");
        }

        beginTest("Every filter type in both slots, every routing, at the ends of every cell: finite and bounded");
        {
            int runs = 0;
            for (double sr : { 22050.0, 48000.0, 192000.0 })
            {
                const int n = (int) (sr * 0.03);
                const auto in = rich(sr, n, 55.0, 1.0f);    // a saw and as much noise: hotter than any voice
                const auto fm = sine(sr, n, 3000.0, 1.0f);
                for (int a = 0; a < FilterSlot::NUM_MODELS; ++a)
                    for (int b = 0; b < FilterSlot::NUM_MODELS; ++b)
                        for (int routing = 0; routing < 3; ++routing)
                            for (int ends = (a + b) % 2 == 0 ? 0 : 4; ends < ((a + b) % 2 == 0 ? 4 : 8); ++ends)
                            {
                                const auto ma = FilterSlot::modelFromIndex(a), mb = FilterSlot::modelFromIndex(b);
                                const float lo = (ends & 1) ? 1.0f : 0.0f, hi = (ends & 2) ? 1.0f : 0.0f;
                                const auto out = runFilter(sr, in, [&](VoiceFilter& f)
                                {
                                    f.setModel(ma);     f.setMode((ends & 4) ? FilterSlot::modeCount(ma) - 1 : 0);
                                    f.setModelB(mb);    f.setModeB((ends & 4) ? FilterSlot::modeCount(mb) - 1 : 0);
                                    f.setRouting(static_cast<FilterRouting>(routing));
                                    f.setCutoff(lo > 0.0f ? 20000.0f : 20.0f);
                                    f.setResonance(hi);     f.setResonanceB(1.0f - lo);
                                    f.setDrive(lo);         f.setDriveB(hi);
                                    f.setMorph(hi);
                                    f.setBalance(lo);
                                    f.setOffsetB(hi > 0.0f ? 48.0f : -48.0f);
                                    f.setTrackB(lo);
                                    f.setFmDepth(1.0f);
                                    f.setEnvAmount(hi > 0.0f ? 1.0f : -1.0f);
                                    f.setKeyTrack(1.0f);
                                }, &fm);
                                ++runs;
                                if (! bounded(out, 1000.0f))        // (a resonant filter has gain; a runaway has no end)
                                    expect(false, juce::String("A ") + FilterSlot::modelNames()[a] + ", B " + FilterSlot::modelNames()[b]
                                                  + ", routing " + juce::String(routing) + ", ends " + juce::String(ends) + " at " + juce::String(sr));
                            }
            }
            expect(runs == 3 * 36 * 3 * 4);
        }

        beginTest("A cell turned under a note is moved to, not jumped to: BAL, FFM, OFSB, RESB, DRVB, TRKB");
        {
            const double sr = 48000.0;
            // (160 Hz: 300 samples a cycle. The turn comes at a crest; the windows compared are whole cycles apart.)
            const int n = 24000, at = 12075;
            const auto in = sine(sr, n, 160.0);
            const auto fm = sine(sr, n, 160.0, 1.0f);
            struct Turn { const char* name; std::function<void(VoiceFilter&, float)> set; float from, to; };
            const std::vector<Turn> turns = {
                { "BAL",  [](VoiceFilter& f, float v) { f.setBalance(v); }, 0.0f, 1.0f },
                { "FFM",  [](VoiceFilter& f, float v) { f.setFmDepth(v); }, 0.0f, 1.0f },
                { "OFSB", [](VoiceFilter& f, float v) { f.setOffsetB(v); }, -48.0f, 48.0f },
                { "RESB", [](VoiceFilter& f, float v) { f.setResonanceB(v); }, 0.0f, 1.0f },
                { "DRVB", [](VoiceFilter& f, float v) { f.setDriveB(v); }, 0.0f, 1.0f },
                { "TRKB", [](VoiceFilter& f, float v) { f.setTrackB(v); }, 0.0f, 1.0f },
                { "RESO", [](VoiceFilter& f, float v) { f.setResonance(v); }, 0.0f, 1.0f } };
            for (auto& t : turns)
            {
                const auto out = runFilter(sr, in, [&](VoiceFilter& f)
                {
                    f.setModel(FilterModel::SVF); f.setMode(0); f.setCutoff(300.0f);
                    f.setModelB(FilterModel::SVF); f.setModeB(2);                           // B: a high-pass
                    f.setRouting(FilterRouting::Parallel);
                    f.setEnvAmount(0.3f);
                    for (auto& other : turns)
                        other.set(f, 0.5f * (other.from + other.to));
                    t.set(f, t.from);
                }, &fm, [&](VoiceFilter& f, size_t i) { if (i == (size_t) at) t.set(f, t.to); });
                const float before = largestStep(out, 4800, (size_t) at - 1), during = largestStep(out, (size_t) at - 1, (size_t) at + 960);
                const float after = largestStep(out, (size_t) n - 4800, (size_t) n);
                expect(rmsOfDifference(std::vector<float>(out.begin() + at - 4800, out.begin() + at),
                                       std::vector<float>(out.begin() + at + 4800, out.begin() + at + 9600)) > 1.0e-4,
                       juce::String(t.name) + " changes the sound");
                expect(during <= 2.0f * std::max(before, after) + 0.002f,
                       juce::String(t.name) + ": a step of " + juce::String(during, 4) + " against " + juce::String(std::max(before, after), 4));
            }
            // The same turn with nothing leading it is a jump: the measurement can tell.
            const auto jumped = runFilter(sr, in, [](VoiceFilter& f)
            {
                f.setModel(FilterModel::SVF); f.setCutoff(300.0f);
                f.setModelB(FilterModel::SVF); f.setModeB(2);
                f.setRouting(FilterRouting::Parallel);
                f.setBalance(0.0f);
            }, nullptr, [&](VoiceFilter& f, size_t i) { if (i == (size_t) at) f.setBalanceNow(1.0f); });
            expect(largestStep(jumped, (size_t) at - 1, (size_t) at + 960) > 4.0f * largestStep(jumped, 4800, (size_t) at - 1), "BAL jumped to is a step");
        }

        beginTest("The caller's-sum door leads and arrives like the other; the router's per-sample values are the cells' own");
        {
            const double sr = 48000.0;
            const int n = 19200;
            const auto in = rich(sr, n);
            const auto fm = sine(sr, n, 230.0, 1.0f);
            struct Turn { const char* name; std::function<void(VoiceFilter&, float)> set; float from, to; };
            const std::vector<Turn> turns = {
                { "CUT",  [](VoiceFilter& f, float v) { f.setCutoff(v); }, 300.0f, 2400.0f },
                { "DRIV", [](VoiceFilter& f, float v) { f.setDrive(v); }, 0.0f, 1.0f },
                { "RESO", [](VoiceFilter& f, float v) { f.setResonance(v); }, 0.1f, 0.7f },
                { "BAL",  [](VoiceFilter& f, float v) { f.setBalance(v); }, 0.0f, 1.0f },
                { "FFM",  [](VoiceFilter& f, float v) { f.setFmDepth(v); }, 0.0f, 0.8f },
                { "OFSB", [](VoiceFilter& f, float v) { f.setOffsetB(v); }, -12.0f, 24.0f },
                { "RESB", [](VoiceFilter& f, float v) { f.setResonanceB(v); }, 0.1f, 0.7f },
                { "DRVB", [](VoiceFilter& f, float v) { f.setDriveB(v); }, 0.0f, 1.0f },
                { "TRKB", [](VoiceFilter& f, float v) { f.setTrackB(v); }, 0.0f, 1.0f } };
            for (auto& t : turns)
            {
                // The cell turned under the note, and there from the start.
                auto render = [&](int turnAt)
                {
                    VoiceFilter f;
                    f.setSampleRate(sr);
                    f.setModel(FilterModel::SVF); f.setCutoff(600.0f); f.setResonance(0.3f);
                    f.setModelB(FilterModel::SVF); f.setModeB(1);
                    f.setRouting(FilterRouting::Parallel);
                    t.set(f, turnAt < 0 ? t.to : t.from);
                    f.noteOn(60, 1.0f, false);
                    std::vector<float> out((size_t) n);
                    for (int i = 0; i < n; ++i)
                    {
                        if (i == turnAt)
                            t.set(f, t.to);
                        out[(size_t) i] = f.processOctaves(in[(size_t) i], 0.4f, fm[(size_t) i]);
                    }
                    return out;
                };
                const auto fromStart = render(-1), under = render(n / 4);
                const auto tail = [n](const std::vector<float>& x) { return std::vector<float>(x.begin() + (3 * n) / 4, x.end()); };
                const auto head = [n](const std::vector<float>& x) { return std::vector<float>(x.begin() + n / 8, x.begin() + n / 4); };
                expect(rmsOfDifference(head(under), head(fromStart)) > 0.003, juce::String(t.name) + " is heard through processOctaves: "
                                                                             + juce::String(rmsOfDifference(head(under), head(fromStart)), 4));
                expect(rmsOfDifference(tail(under), tail(fromStart)) < 0.02 * rms(tail(fromStart)),
                       juce::String(t.name) + " turned under a note arrives: " + juce::String(rmsOfDifference(tail(under), tail(fromStart)) / rms(tail(fromStart)), 4));
            }

            auto two = [](VoiceFilter& f)
            {
                f.setModel(FilterModel::SVF); f.setCutoff(500.0f); f.setResonance(0.4f);
                f.setModelB(FilterModel::SVF); f.setModeB(2);
                f.setRouting(FilterRouting::Parallel);
            };
            const auto plain = runFilter(sr, in, two, &fm);
            const auto fmCell = runFilter(sr, in, [&](VoiceFilter& f) { two(f); f.setFmDepth(0.6f); }, &fm);
            const auto fmNow = runFilter(sr, in, two, &fm, [](VoiceFilter& f, size_t) { f.setFmNow(0.6f); });
            const auto ofsCell = runFilter(sr, in, [&](VoiceFilter& f) { two(f); f.setOffsetB(-19.0f); }, &fm);
            const auto ofsNow = runFilter(sr, in, two, &fm, [](VoiceFilter& f, size_t) { f.setOffsetBNow(-19.0f); });
            expect(rmsOfDifference(fmCell, plain) > 0.01 && same(fmCell, fmNow), "setFmNow per sample is the FFM cell");
            expect(rmsOfDifference(ofsCell, plain) > 0.01 && same(ofsCell, ofsNow), "setOffsetBNow per sample is the OFSB cell");
            const auto ofsRamp = runFilter(sr, in, two, &fm, [n](VoiceFilter& f, size_t i) { f.setOffsetBNow(-24.0f + 48.0f * (float) i / (float) n); });
            expect(rmsOfDifference(ofsRamp, plain) > 0.01 && bounded(ofsRamp, 8.0f), "an OFSB ramp is heard");
        }

        beginTest("EVAR: the filter envelope's times by the note's factor, the envelope as set kept apart");
        {
            Envelope::Params p;
            p.attack = 0.01f; p.decay = 0.2f; p.sustain = 0.1f; p.release = 0.3f;
            VoiceFilter f, unscaled;
            f.setSampleRate(48000.0); unscaled.setSampleRate(48000.0);
            f.setEnvParams(p); unscaled.setEnvParams(p);
            f.setEnvTimeScale(2.0f);
            expectWithinAbsoluteError(f.getEnvelope().getParams().decay, 0.4f, 1.0e-6f);
            expectWithinAbsoluteError(f.getEnvelope().getParams().release, 0.6f, 1.0e-6f);
            expectEquals(f.getEnvelope().getParams().sustain, 0.1f, "levels are not times");
            f.setEnvTimeScale(2.0f); f.setEnvTimeScale(0.5f); f.setEnvTimeScale(2.0f);
            expectWithinAbsoluteError(f.getEnvelope().getParams().decay, 0.4f, 1.0e-6f, "the factor never adds up");
            f.setEnvDecay(0.1f);        // (an LFO on FDEC under the note)
            expectWithinAbsoluteError(f.getEnvelope().getParams().decay, 0.2f, 1.0e-6f, "a stage set under the note takes the factor");
            p.decay = 0.3f;
            f.setEnvParams(p);
            expectWithinAbsoluteError(f.getEnvelope().getParams().decay, 0.6f, 1.0e-6f, "and so does the envelope when it is set again");
            f.setEnvTimeScale(1.0f);
            unscaled.setEnvParams(p);
            expect(std::memcmp(&f.getEnvelope().getParams(), &unscaled.getEnvelope().getParams(), sizeof(Envelope::Params)) == 0,
                   "a factor of 1 is the envelope as set, exactly");
            f.setEnvTimeScale(std::numeric_limits<float>::quiet_NaN());
            expectWithinAbsoluteError(f.getEnvelope().getParams().decay, 0.3f, 1.0e-6f, "a factor that is not a number is 1");

            // In the wavetable engine: one factor per note for the amp and the filter envelope.
            auto e = wavetable();
            e->setParameter(W::AmpDecay, 0.25f);
            e->setParameter(W::FilterBase + F::Decay, 0.5f);
            e->setParameter(W::SpreadBase + VoiceSpread::EnvTime, 1.0f);
            e->setParameter(W::SpreadBase + VoiceSpread::Seed, 1.0f);
            float least = 10.0f, most = 0.0f;
            for (int note = 0; note < 12; ++note)
            {
                e->noteOn(50, 0.8f, -1);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                e->processBlock(buf, midi);
                bool found = false;
                for (int v = 0; v < 8 && ! found; ++v)
                    if (e->getVoice(v).isActive())
                    {
                        found = true;
                        // (The amp envelope keeps its params as set and takes the factor as a time
                        // scale, Envelope::setTimeScale, since the engine is on the router: U4.)
                        expectEquals(e->getVoice(v).getAmpEnvelope().getParams().decay, 0.25f, "the amp envelope's decay stays as set");
                        const float amp = e->getVoice(v).getAmpEnvelope().getTimeScale();
                        const float filter = e->getVoice(v).getFilter().getEnvelope().getParams().decay / 0.5f;
                        expectWithinAbsoluteError(filter, amp, 1.0e-4f, "the filter envelope and the amp envelope by the same factor");
                        least = std::min(least, filter);
                        most = std::max(most, filter);
                    }
                expect(found);
                e->allNotesOff();
                for (int b = 0; b < 200; ++b)
                    e->processBlock(buf, midi);
            }
            expect(least >= 0.5f && most <= 2.0f && most > 1.2f && least < 0.85f,
                   "EVAR 100: between half and twice, " + juce::String(least, 3) + " .. " + juce::String(most, 3));
        }

        beginTest("The section's pages and ids: single, dual, and the layout for an engine on the router");
        {
            const F single(100), dual(100, 116);
            expect(! single.hasDual() && dual.hasDual());
            expect(! single.owns(116) && dual.owns(116) && dual.owns(124) && ! dual.owns(125));
            expectEquals(dual.id(F::TypeB), 116);
            expectEquals(dual.id(F::TrackB), 124);
            expectEquals(dual.offsetOf(dual.id(F::Balance)), (int) F::Balance);
            expectEquals(dual.get(dual.id(F::TypeB)), 0.0f, "filter B is an SVF until chosen");
            expectEquals(dual.get(dual.id(F::Routing)), 0.0f, "and out of the path: ROUT is SNGL");
            expectEquals(dual.get(dual.id(F::Balance)), 0.5f);
            expectEquals(dual.get(dual.id(F::TrackB)), 1.0f);

            ParamPages cells, routed;
            F::appendPages(cells, 100, 116);
            F::appendPages(routed, 100, 116, F::Routed);
            expectEquals((int) cells.size(), 3);
            juce::StringArray names;
            std::set<int> ids;
            for (auto& page : cells)
            {
                names.add(page.name);
                for (auto& p : page.params)
                {
                    expect(p.isValid(), juce::String(page.name) + " has eight working cells");
                    expect(dual.owns(p.id) && ids.insert(p.id).second, juce::String(p.label) + " is the section's, once");
                }
            }
            expectEquals(names.joinIntoString(" "), juce::String("FILTR FLT B F ENV"));
            expect(cell(cells, "FLT B", "FFM") != nullptr && cell(cells, "FLT B", "TRKB") == nullptr);
            expect(cell(routed, "FILTR", "FFM") != nullptr && cell(routed, "FILTR", "KTRK") == nullptr, "Routed: FFM where KTRK was");
            expect(cell(routed, "FLT B", "TRKB") != nullptr, "Routed: TRKB on FLT B");
            for (const char* gone : { "EVEL", "VEL", "WHL" })
                expect(cell(routed, "F ENV", gone) == nullptr, juce::String(gone) + " is a slot on the router");
            expect(cell(routed, "FILTR", "ENV") != nullptr && cell(routed, "F ENV", "CURV") != nullptr);

            // The destinations on the router's table: CUT, BAL, FFM and OFSB sample by sample.
            mod::DestTable table;
            table.group();
            F::addRouterInputs(table);
            F::addLfoDests(table, cells, 100);
            F::addDualDests(table, cells, 116);
            for (const char* label : { "CUT", "BAL", "FFM", "OFSB" })
            {
                const int i = table.indexOfLabel(label);
                expect(i >= 0 && table[i].perSample, juce::String(label) + " is a per-sample destination");
            }
            for (const char* label : { "RESO", "RESB", "DRVB" })
            {
                const int i = table.indexOfLabel(label);
                expect(i >= 0 && ! table[i].perSample, juce::String(label) + " is a control-rate destination");
            }
            expect(table.indexOfLabel("TRKB") < 0, "a cell the pages do not show is not offered");
            // The router's per-sample values go straight in: a balance ramp is the two filters cross-faded.
            const double sr = 48000.0;
            const auto in = rich(sr, 9600);
            auto two = [](VoiceFilter& f)
            {
                f.setModel(FilterModel::SVF); f.setCutoff(500.0f);
                f.setModelB(FilterModel::SVF); f.setModeB(2);
                f.setRouting(FilterRouting::Parallel);
            };
            const auto onlyA = runFilter(sr, in, [&](VoiceFilter& f) { two(f); f.setBalance(0.0f); });
            const auto onlyB = runFilter(sr, in, [&](VoiceFilter& f) { two(f); f.setBalance(1.0f); });
            const auto ramp = runFilter(sr, in, two, nullptr, [&](VoiceFilter& f, size_t i) { f.setBalanceNow((float) i / (float) in.size()); });
            std::vector<float> faded(in.size());
            for (size_t i = 0; i < in.size(); ++i)
            {
                const float b = (float) i / (float) in.size();
                faded[i] = onlyA[i] * (1.0f - b) + onlyB[i] * b;
            }
            expect(rmsOfDifference(ramp, faded) < 1.0e-6, "setBalanceNow per sample");
        }

        beginTest("The generator: repeatable from a seed, different per note, per voice and per stream");
        {
            spread::Generator a, b, other, otherSeed;
            a.seed(7, 3); b.seed(7, 3); other.seed(7, 4); otherSeed.seed(8, 3);
            bool repeat = true;
            int sameAsOtherVoice = 0, sameAsOtherSeed = 0, sameAsLastNote = 0, sameAcrossStreams = 0;
            double sum = 0.0, sumSquares = 0.0, lo = 1.0, hi = -1.0, cross = 0.0;
            float last = 2.0f;
            const int notes = 20000;
            for (int n = 0; n < notes; ++n)
            {
                a.advance(); b.advance(); other.advance(); otherSeed.advance();
                const float v = a.bipolar(spread::Pitch);
                repeat = repeat && v == b.bipolar(spread::Pitch) && v == a.bipolar(spread::Pitch)
                         && a.unipolar(spread::Phase, 5) == b.unipolar(spread::Phase, 5);
                sameAsOtherVoice += v == other.bipolar(spread::Pitch) ? 1 : 0;
                sameAsOtherSeed += v == otherSeed.bipolar(spread::Pitch) ? 1 : 0;
                sameAsLastNote += v == last ? 1 : 0;
                sameAcrossStreams += (v == a.bipolar(spread::Cutoff) || a.unipolar(spread::Phase, 0) == a.unipolar(spread::Phase, 1)) ? 1 : 0;
                cross += (double) v * a.bipolar(spread::Cutoff);
                last = v;
                sum += v; sumSquares += (double) v * v;
                lo = std::min(lo, (double) v); hi = std::max(hi, (double) v);
                const float u = a.unipolar(spread::Phase, n % 7);
                expect(u >= 0.0f && u < 1.0f && v >= -1.0f && v <= 1.0f);
            }
            expect(repeat, "the same seed, voice and note: the same value, however often it is asked for");
            expect(sameAsOtherVoice < 5 && sameAsOtherSeed < 5 && sameAsLastNote < 5 && sameAcrossStreams < 5,
                   "another voice, seed, note or stream: another value");
            expectWithinAbsoluteError(sum / notes, 0.0, 0.02, "centred");
            expectWithinAbsoluteError(sumSquares / notes, 1.0 / 3.0, 0.02, "evenly spread");
            expect(lo < -0.99 && hi > 0.99, "over the whole range");
            expectWithinAbsoluteError(cross / notes, 0.0, 0.02, "two streams have nothing to do with each other");
            // Seeding starts the sequence again.
            a.seed(7, 3);
            spread::Generator fresh;
            fresh.seed(7, 3);
            a.advance(); fresh.advance();
            expect(a.bipolar(spread::Wave) == fresh.bipolar(spread::Wave) && a.noteCount() == 1u);
            expect(sizeof(spread::Generator) == 8);
        }

        beginTest("The amounts scale the draws; at zero a note takes nothing");
        {
            VoiceSpread zero(125), small(125), large(125);
            using S = VoiceSpread;
            for (int o = 0; o < (int) S::Seed; ++o)
            {
                small.set(125 + o, o == S::Slop ? 10.0f : 0.25f);
                large.set(125 + o, o == S::Slop ? 40.0f : 1.0f);
            }
            expect(! zero.on() && small.on());
            spread::Generator gz, gs, gl;
            gz.seed(1, 0); gs.seed(1, 0); gl.seed(1, 0);
            float widest = 0.0f, widestPhase = 0.0f, lowestGain = 1.0f;
            for (int n = 0; n < 2000; ++n)
            {
                const auto z = zero.next(gz);
                const auto s = small.next(gs);
                const auto l = large.next(gl);
                expect(z.semitones == 0.0f && z.cutoffOctaves == 0.0f && z.timeScale == 1.0f && z.wave == 0.0f && z.pan == 0.0f
                       && z.gain == 1.0f && z.phase(0) == 0.0f && z.phase(3) == 0.0f, "nothing at zero");
                expect(gz.noteCount() == gs.noteCount(), "the generator advances whatever the amounts");
                expectWithinAbsoluteError(l.semitones, 4.0f * s.semitones, 1.0e-5f);
                expectWithinAbsoluteError(l.cutoffOctaves, 4.0f * s.cutoffOctaves, 1.0e-5f);
                expectWithinAbsoluteError(std::log2(l.timeScale), 4.0f * std::log2(s.timeScale), 1.0e-4f);
                expectWithinAbsoluteError(l.wave, 4.0f * s.wave, 1.0e-5f);
                expectWithinAbsoluteError(l.pan, 4.0f * s.pan, 1.0e-5f);
                expectWithinAbsoluteError(1.0f - l.gain, 4.0f * (1.0f - s.gain), 1.0e-5f);
                expectWithinAbsoluteError(l.phase(1), 4.0f * s.phase(1), 1.0e-5f);
                expect(std::abs(s.semitones) <= 0.1f && std::abs(s.cutoffOctaves) <= 0.25f && s.timeScale >= 0.84f && s.timeScale <= 1.19f
                       && s.gain >= 0.75f && s.gain <= 1.0f && s.phase(0) >= 0.0f && s.phase(0) < 0.25f, "inside the amount");
                widest = std::max(widest, std::abs(l.semitones));
                widestPhase = std::max(widestPhase, l.phase(2));
                lowestGain = std::min(lowestGain, l.gain);
            }
            expect(widest > 0.38f && widest <= 0.4f, "SLOP 40: up to 40 cents (" + juce::String(widest, 3) + ")");
            expect(widestPhase > 0.98f && lowestGain < 0.02f);
            // Out of range, or not a number: held to the cell.
            large.set(125 + S::Slop, 1.0e9f);
            large.set(125 + S::Pan, std::numeric_limits<float>::quiet_NaN());
            large.set(125 + S::Seed, 5000.0f);
            expectEquals(large.get(125 + S::Slop), S::MAX_SLOP_CENTS);
            expectEquals(large.get(125 + S::Pan), 0.0f);
            expectEquals(large.get(125 + S::Seed), (float) S::MAX_SEED);
            const auto page = S::makePage(125);
            for (auto& p : page.params)
                expect(p.isValid() && large.owns(p.id), "ANLG has eight working cells");
        }

        beginTest("The wavetable engine: its pages, and filter B off with the spread at 0 is the engine as it was");
        {
            auto e = wavetable();
            juce::StringArray names;
            for (auto& page : e->getParamPages())
            {
                names.add(page.name);
                if (page.name == juce::String("FLT B") || page.name == juce::String("ANLG") || page.name == juce::String("FILTR")
                    || page.name == juce::String("F ENV"))
                    for (auto& p : page.params)
                    {
                        expect(p.isValid(), juce::String(page.name) + ": eight working cells");
                        expectEquals(e->getParameter(p.id), p.defaultVal, juce::String(p.label) + " reads its default");
                        e->setParameter(p.id, p.maxVal);
                        expectEquals(e->getParameter(p.id), p.maxVal, juce::String(p.label) + " reads what was set");
                        e->setParameter(p.id, p.defaultVal);
                        expect(e->getParameterName(p.id).isNotEmpty() && ! e->getParameterName(p.id).startsWith("Param "),
                               juce::String(p.label) + " has a name");
                    }
            }
            const juce::String order = names.joinIntoString(" ");
            expect(order.startsWith("WAVE SCAN OSC ANLG AMP KEYS K SMP K POS K PLC VOICE FILTR FLT B F ENV XIN"), order);
            expect(order.endsWith("STRIP LFO1 LFO2"), "the last pages are STRIP and the LFOs: " + order);
            // MODB offers the modes of the type TYPB chooses.
            e->setParameter(W::FilterDualBase, (float) FilterSlot::indexFromModel(FilterModel::Ladder));
            const auto pages = e->getParamPages();
            const auto* modb = cell(pages, "FLT B", "MODB");
            expect(modb != nullptr && modb->numChoices == 6 && juce::String(modb->choices[0]) == "LP24");

            // Renders made by the tree before the port (6bf3c48, the Mod Slots checksum tool, Debug):
            // the default sound, the default sound with strip and pressure, two factory presets.
            // (Debug only: a Release build contracts the engine's arithmetic and renders other bits,
            // before the port as after it. The two bit-for-bit comparisons above hold in both.)
           #if JUCE_DEBUG
            auto hash = [](uint64_t h) { return juce::String::toHexString((juce::int64) h); };
            auto plain = ModSlotsTests::makeEngine(EngineType::Wavetable);
            expectEquals(hash(ModSlotsTests::hashRender(*plain, 1.0, 0.0f, false)), juce::String("c9126ab4dc054a53"), "the default sound");
            auto strip = ModSlotsTests::makeEngine(EngineType::Wavetable);
            // (Since phase U4 the strip and the pressure come through the modulation router, which leads
            // and ramps them in its own way: other samples, the same sound - "U4 Wavetable" renders the
            // engine as it was beside this one, strip and pressure rows. Before: 6efaa74cfa5fa6e3.)
            expectEquals(hash(ModSlotsTests::hashRender(*strip, 1.0, 0.7f, true)), juce::String("283e01dd3afd551b"), "with strip and pressure");
            for (auto& p : FactoryPresets::getAll())
            {
                // (Digital Choir has the sub, whose sine is PhaseOsc's since phase U4: the last bits differ,
                // a null of -139 dB against the engine as it was in "U4 Wavetable". Before: 6848d52a441bed77.)
                const char* expected = p.name == "Digital Choir" ? "bd07a81f84252b37" : (p.name == "Glitch Table" ? "f150b759c15eadd3" : nullptr);
                if (expected == nullptr)
                    continue;
                auto preset = ModSlotsTests::makeEngine(p.engineType, &p.soundParams);
                expectEquals(hash(ModSlotsTests::hashRender(*preset, 1.0, 0.0f, false)), juce::String(expected), p.name);
            }
           #endif
        }

        beginTest("The wavetable engine with filter B, the routing, BAL and filter FM");
        {
            auto render = [](const std::function<void(W&)>& setup)
            {
                auto e = wavetable();
                e->setParameter(W::FilterBase + F::Cutoff, 700.0f);
                e->setParameter(W::FilterBase + F::Resonance, 0.4f);
                setup(*e);
                return play(*e, 1, 0.5, 48000.0, 45)[0];
            };
            const int B = W::FilterDualBase - (int) F::Count;
            const auto onlyA = render([](W&) {});
            const auto highPassB = [B](W& e) { e.setParameter(B + F::TypeB, 0.0f); e.setParameter(B + F::ModeB, 2.0f); e.setParameter(B + F::OffsetB, 30.0f); };
            const auto chosenNotRouted = render(highPassB);
            const auto serial = render([&](W& e) { highPassB(e); e.setParameter(B + F::Routing, 1.0f); });
            const auto parA = render([&](W& e) { highPassB(e); e.setParameter(B + F::Routing, 2.0f); e.setParameter(B + F::Balance, 0.0f); });
            const auto parB = render([&](W& e) { highPassB(e); e.setParameter(B + F::Routing, 2.0f); e.setParameter(B + F::Balance, 1.0f); });
            const auto fm = render([B](W& e) { e.setParameter(B + F::FilterFM, 1.0f); });
            expect(rms(onlyA) > 0.02, "it sounds");
            expect(same(chosenNotRouted, onlyA), "TYPB alone changes nothing while ROUT is SNGL");
            expect(rms(serial) < 0.25 * rms(onlyA), "SER: a low-pass at 700 Hz into a high-pass at 4 kHz leaves little ("
                                                     + juce::String(rms(serial) / rms(onlyA), 3) + ")");
            expect(same(parA, onlyA) || rmsOfDifference(parA, onlyA) < 1.0e-6, "PAR at BAL 0 is filter A");
            expect(rmsOfDifference(parB, onlyA) > 0.3 * rms(onlyA) && rms(parB) > 0.002, "PAR at BAL 100 is filter B: another sound");
            expect(rmsOfDifference(fm, onlyA) > 0.05 * rms(onlyA), "FFM changes the sound: " + juce::String(rmsOfDifference(fm, onlyA) / rms(onlyA), 3));
            for (auto* r : { &serial, &parA, &parB, &fm })
                expect(bounded(*r, 4.0f));
            // An LFO on BAL (a destination of the dual block) moves the sound between the filters.
            const auto& dests = W::lfoDests();
            const int bal = dests.indexOfLabel("BAL");
            expect(bal > 0 && dests.indexOfLabel("FFM") > 0 && dests.indexOfLabel("OFSB") > 0 && dests.indexOfLabel("RESB") > 0
                   && dests.indexOfLabel("DRVB") > 0, "the dual block's cells are LFO destinations");
            const auto swept = render([&](W& e)
            {
                highPassB(e);
                e.setParameter(B + F::Routing, 2.0f);
                e.setParameter(EngineLfoSection::BASE, 6.0f);                  // LFO 1: RATE, DPTH, DEST
                e.setParameter(EngineLfoSection::BASE + 1, 1.0f);
                e.setParameter(EngineLfoSection::BASE + 7, (float) bal);
            });
            const auto still = render([&](W& e) { highPassB(e); e.setParameter(B + F::Routing, 2.0f); });
            expect(rmsOfDifference(swept, still) > 0.05 * rms(still), "LFO > BAL");
        }

        beginTest("The wavetable engine's spread: zero is the sound without it, a seed repeats, every amount is heard");
        {
            using S = VoiceSpread;
            auto render = [](const std::function<void(W&)>& setup, int notes = 6, int noteStep = 0)
            {
                auto e = wavetable();
                e->setParameter(W::FilterBase + F::Cutoff, 1500.0f);
                e->setParameter(W::FilterBase + F::EnvAmount, 0.3f);
                e->setParameter(W::SubLevel, 0.5f);
                e->setParameter(W::UnisonSpread, 0.4f);
                setup(*e);
                return play(*e, notes, 0.12, 48000.0, 57, noteStep);
            };
            auto set = [](int offset, float value) { return [offset, value](W& e) { e.setParameter(W::SpreadBase + offset, value); }; };
            const auto plain = render([](W&) {});
            expect(rms(plain[0]) > 0.02);
            expect(same(plain[0], plain[1]), "no spread: both sides alike");
            const auto otherSeed = render(set(S::Seed, 99.0f));
            expect(same(plain[0], otherSeed[0]) && same(plain[1], otherSeed[1]), "every amount at 0: the seed changes nothing");

            struct Amount { const char* name; int offset; float full; };
            for (auto& a : { Amount { "SLOP", S::Slop, 50.0f }, Amount { "FVAR", S::Cutoff, 1.0f }, Amount { "EVAR", S::EnvTime, 1.0f },
                             Amount { "WVAR", S::Wave, 1.0f }, Amount { "PHAS", S::Phase, 1.0f }, Amount { "PVAR", S::Pan, 1.0f },
                             Amount { "LVAR", S::Level, 1.0f } })
            {
                const auto once = render(set(a.offset, a.full));
                const auto again = render(set(a.offset, a.full));
                const auto seeded = render([&](W& e) { set(a.offset, a.full)(e); e.setParameter(W::SpreadBase + S::Seed, 31.0f); });
                const auto back = render([&](W& e) { set(a.offset, a.full)(e); set(a.offset, 0.0f)(e); });
                const double moved = rmsOfDifference(once[0], plain[0]) + rmsOfDifference(once[1], plain[1]);
                expect(moved > 0.01 * rms(plain[0]), juce::String(a.name) + " is heard: " + juce::String(moved / rms(plain[0]), 4));
                expect(same(once[0], again[0]) && same(once[1], again[1]), juce::String(a.name) + ": the same seed, the same render");
                expect(! same(once[0], seeded[0]) || ! same(once[1], seeded[1]), juce::String(a.name) + ": another seed, another render");
                expect(same(back[0], plain[0]) && same(back[1], plain[1]), juce::String(a.name) + " back at 0: the sound without it");
                expect(bounded(once[0], 4.0f) && bounded(once[1], 4.0f));
                if (a.offset == S::Pan)
                    expect(! same(once[0], once[1]), "PVAR: the two sides differ");
                else
                    expect(same(once[0], once[1]), juce::String(a.name) + " leaves the note in the middle");
                if (a.offset == S::Level)
                    expect(rms(once[0]) < rms(plain[0]), "LVAR only takes away");
            }

            // SLOP in cents: the pitches of sixteen notes of one key, at most that far off and not all alike.
            for (float cents : { 12.0f, 50.0f })
            {
                auto e = wavetable();
                e->setParameter(W::TableSelect, 3.0f);      // the sine table: one partial to measure
                e->setParameter(W::SpreadBase + S::Slop, cents);
                double lowest = 1.0e9, highest = -1.0e9;
                for (int n = 0; n < 16; ++n)
                {
                    const auto note = play(*e, 1, 0.25, 48000.0, 69)[0];
                    double bestCents = 0.0, bestLevel = -1.0;
                    for (double c = -60.0; c <= 60.0; c += 1.0)
                    {
                        const double l = level(note, 440.0 * std::pow(2.0, c / 1200.0), 48000.0);
                        if (l > bestLevel) { bestLevel = l; bestCents = c; }
                    }
                    lowest = std::min(lowest, bestCents);
                    highest = std::max(highest, bestCents);
                }
                expect(lowest >= -cents - 2.0 && highest <= cents + 2.0 && highest - lowest > 0.6 * cents,
                       "SLOP " + juce::String(cents) + ": notes from " + juce::String(lowest) + " to " + juce::String(highest) + " cents");
            }

            // Different voices, the same note number of each: not the same draw (a chord is not detuned as one).
            auto chord = wavetable();
            chord->setParameter(W::SpreadBase + S::Phase, 1.0f);
            for (int note : { 48, 55, 64 })
                chord->noteOn(note, 0.8f, -1);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            chord->processBlock(buf, midi);
            expect(chord->getVoice(0).getSpreadGenerator().bipolar(spread::Pitch) != chord->getVoice(1).getSpreadGenerator().bipolar(spread::Pitch));

            // At the ends of every cell, from 22.05 to 192 kHz, in blocks of 1 and of 4096: bounded.
            for (double sr : { 22050.0, 192000.0 })
                for (int block : { 1, 4096 })
                {
                    auto e = wavetable(sr);
                    for (auto& page : e->getParamPages())
                        if (page.name == juce::String("ANLG") || page.name == juce::String("FLT B"))
                            for (auto& p : page.params)
                                e->setParameter(p.id, p.maxVal);
                    e->setParameter(W::FilterDualBase, 2.0f);       // TYPB: the ladder (the top of the cell is OFF)
                    const auto out = play(*e, 4, 0.1, sr, 30, 31, block);
                    expect(rms(out[0]) + rms(out[1]) > 1.0e-4 && bounded(out[0], 8.0f) && bounded(out[1], 8.0f),
                           "everything up at " + juce::String(sr) + " Hz, blocks of " + juce::String(block));
                }
        }
        beginTest("What is not a number, or far out of range: no cell stores it, no setter lets it reach a filter");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            const double sr = 48000.0;
            const int n = 2400;
            const auto in = rich(sr, n, 110.0, 1.0f);
            std::vector<float> square((size_t) n);
            for (int i = 0; i < n; ++i)
                square[(size_t) i] = (i / 8) % 2 == 0 ? 1.0f : -1.0f;      // 3 kHz, full scale
            struct One { VoiceFilter f; VoiceFilter& getFilter() { return f; } };
            using Set = std::function<void(VoiceFilter&, float)>;
            const std::vector<std::pair<const char*, Set>> setters = {
                { "setCutoff", [](VoiceFilter& f, float v) { f.setCutoff(v); } },
                { "setResonance", [](VoiceFilter& f, float v) { f.setResonance(v); } },
                { "setResonanceB", [](VoiceFilter& f, float v) { f.setResonanceB(v); } },
                { "setDrive", [](VoiceFilter& f, float v) { f.setDrive(v); } },
                { "setDriveB", [](VoiceFilter& f, float v) { f.setDriveB(v); } },
                { "setBalance", [](VoiceFilter& f, float v) { f.setBalance(v); } },
                { "setFmDepth", [](VoiceFilter& f, float v) { f.setFmDepth(v); } },
                { "setOffsetB", [](VoiceFilter& f, float v) { f.setOffsetB(v); } },
                { "setTrackB", [](VoiceFilter& f, float v) { f.setTrackB(v); } },
                { "setMorph", [](VoiceFilter& f, float v) { f.setMorph(v); } },
                { "setEnvAmount", [](VoiceFilter& f, float v) { f.setEnvAmount(v); } },
                { "setKeyTrack", [](VoiceFilter& f, float v) { f.setKeyTrack(v); } },
                { "setVelAmount", [](VoiceFilter& f, float v) { f.setVelAmount(v); } },
                { "setEnvVelocity", [](VoiceFilter& f, float v) { f.setEnvVelocity(v); } },
                { "setExternalOctaves", [](VoiceFilter& f, float v) { f.setExternalOctaves(v); } },
                { "setLfoOctaves", [](VoiceFilter& f, float v) { f.setLfoOctaves(v); } },
                { "setResonanceMod", [](VoiceFilter& f, float v) { f.setResonanceMod(v); } },
                { "setBalanceNow", [](VoiceFilter& f, float v) { f.setBalanceNow(v); } },
                { "setFmNow", [](VoiceFilter& f, float v) { f.setFmNow(v); } },
                { "setOffsetBNow", [](VoiceFilter& f, float v) { f.setOffsetBNow(v); } },
                { "setEnvTimeScale", [](VoiceFilter& f, float v) { f.setEnvTimeScale(v); } },
                { "setEnvAttack", [](VoiceFilter& f, float v) { f.setEnvAttack(v); } },
                { "setEnvDecay", [](VoiceFilter& f, float v) { f.setEnvDecay(v); } },
                { "setEnvSustain", [](VoiceFilter& f, float v) { f.setEnvSustain(v); } },
                { "setEnvRelease", [](VoiceFilter& f, float v) { f.setEnvRelease(v); } } };

            for (float wild : { nan, inf, -inf, 1.0e30f, -1.0e30f })
            {
                // Every cell of the section, set before the note and again under it.
                for (int o = 0; o < (int) F::DualCount; ++o)
                {
                    F section(100, 116);
                    std::vector<One> voices(1);
                    auto& f = voices[0].f;
                    f.setSampleRate(sr);
                    section.applyAll(F::each(voices));
                    section.set(section.id(F::Routing), 2.0f, F::each(voices));
                    section.set(section.id(F::FilterFM), 1.0f, F::each(voices));
                    const int id = section.id((F::Offset) o);
                    const float before = section.get(id);
                    section.set(id, wild, F::each(voices));
                    expect(std::isfinite(section.get(id)), juce::String(F::name(o)) + " stores " + juce::String(wild));
                    if (! std::isfinite(wild))
                        expect(section.get(id) == before, juce::String(F::name(o)) + ": what is not a number is not a setting");
                    f.noteOn(60, 1.0f, false);
                    std::vector<float> out((size_t) n);
                    for (int i = 0; i < n; ++i)
                    {
                        if (i == n / 2)
                            section.set(id, wild, F::each(voices));
                        out[(size_t) i] = f.process(in[(size_t) i], square[(size_t) i]);
                    }
                    if (! bounded(out, 100.0f))
                        expect(false, juce::String(F::name(o)) + " at " + juce::String(wild));
                }
                // Every setter of one voice's filter under a note: filter A alone and both, either door.
                for (auto& setter : setters)
                    for (int dual = 0; dual < 2; ++dual)
                        for (int door = 0; door < 2; ++door)
                        {
                            VoiceFilter f;
                            f.setSampleRate(sr);
                            f.setCutoff(1000.0f); f.setResonance(0.5f); f.setEnvAmount(0.3f);
                            if (dual == 1) { f.setModelB(FilterModel::SVF); f.setRouting(FilterRouting::Parallel); f.setFmDepth(0.5f); }
                            f.noteOn(60, 1.0f, false);
                            std::vector<float> out((size_t) n);
                            for (int i = 0; i < n; ++i)
                            {
                                if (i == n / 3)      setter.second(f, wild);
                                if (i == 2 * n / 3)  setter.second(f, 0.5f);
                                out[(size_t) i] = door == 0 ? f.process(in[(size_t) i], square[(size_t) i])
                                                            : f.processOctaves(in[(size_t) i], f.tickEnvelope() * 2.0f, square[(size_t) i]);
                            }
                            if (! bounded(out, 100.0f))
                                expect(false, juce::String(setter.first) + "(" + juce::String(wild) + "), filter B " + juce::String(dual) + ", door " + juce::String(door));
                        }
                // A sum of octaves, a velocity and a note's spread that are anything.
                for (int dual = 0; dual < 2; ++dual)
                {
                    VoiceFilter f;
                    f.setSampleRate(sr);
                    f.setCutoff(1000.0f);
                    if (dual == 1) { f.setModelB(FilterModel::Ladder); f.setRouting(FilterRouting::Series); }
                    f.noteOn(60, wild, false, wild);
                    std::vector<float> a((size_t) n), b((size_t) n);
                    for (int i = 0; i < n; ++i)
                    {
                        a[(size_t) i] = f.process(in[(size_t) i]);
                        b[(size_t) i] = f.processOctaves(in[(size_t) i], i % 2 == 0 ? wild : 0.0f);
                    }
                    expect(bounded(a, 100.0f) && bounded(b, 100.0f), "octaves / velocity / spread of " + juce::String(wild));
                }

                // The wavetable engine: every cell of the filter's and the spread's pages.
                auto e = wavetable();
                const int B = W::FilterDualBase - (int) F::Count;
                e->setParameter(B + F::Routing, 2.0f);
                e->setParameter(B + F::FilterFM, 1.0f);
                for (int o = 0; o < (int) VoiceSpread::Seed; ++o)
                    e->setParameter(W::SpreadBase + o, o == 0 ? 50.0f : 1.0f);
                juce::AudioBuffer<float> buf(2, 256);
                juce::MidiBuffer midi;
                for (int id = W::FilterBase; id < W::SpreadBase + (int) VoiceSpread::Count; ++id)
                {
                    e->setParameter(id, wild);
                    expect(std::isfinite(e->getParameter(id)), "wavetable cell " + juce::String(id) + " stores " + juce::String(wild));
                    e->noteOn(40 + id % 40, 1.0f, -1);
                    buf.clear();
                    e->processBlock(buf, midi);
                    bool fine = true;
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 256; ++i)
                            fine = fine && std::isfinite(buf.getSample(ch, i)) && std::abs(buf.getSample(ch, i)) < 50.0f;
                    if (! fine)
                        expect(false, "wavetable cell " + juce::String(id) + " at " + juce::String(wild));
                    e->noteOff(40 + id % 40, -1);
                }
            }
        }

        beginTest("Both filters at full resonance and drive, the cutoff moved four octaves by a full-scale square: held");
        {
            float worst = 0.0f;
            for (double sr : { 22050.0, 192000.0 })
                for (int a = 0; a < FilterSlot::NUM_MODELS; ++a)
                    for (int b = 0; b < FilterSlot::NUM_MODELS; ++b)
                        for (int routing = 1; routing < 3; ++routing)
                        {
                            const int variant = (a + b + routing) % 2;
                            const int n = (int) (sr * 0.2);
                            std::vector<float> in((size_t) n), fm((size_t) n);
                            double p1 = 0.0, p2 = 0.0;
                            for (int i = 0; i < n; ++i)
                            {
                                in[(size_t) i] = p1 < 0.5 ? 1.0f : -1.0f;
                                fm[(size_t) i] = p2 < 0.5 ? 1.0f : -1.0f;
                                p1 = dsp::wrapPhase(p1 + 55.0 / sr);
                                p2 = dsp::wrapPhase(p2 + (variant == 0 ? 97.0 / sr : 0.25));       // (0.25: a quarter of the sample rate)
                            }
                            const auto ma = FilterSlot::modelFromIndex(a), mb = FilterSlot::modelFromIndex(b);
                            VoiceFilter f;
                            f.setSampleRate(sr);
                            f.setModel(ma);     f.setMode(variant == 1 ? FilterSlot::modeCount(ma) - 1 : 0);
                            f.setModelB(mb);    f.setModeB(variant == 1 ? 0 : FilterSlot::modeCount(mb) - 1);
                            f.setRouting(static_cast<FilterRouting>(routing));
                            f.setCutoff(variant == 1 ? 8000.0f : 300.0f);
                            f.setResonance(1.0f);   f.setResonanceB(1.0f);
                            f.setDrive(1.0f);       f.setDriveB(1.0f);
                            f.setFmDepth(1.0f);
                            f.setOffsetB(variant == 1 ? -48.0f : 48.0f);
                            f.setEnvAmount(variant == 1 ? -1.0f : 1.0f);
                            f.setMorph(0.5f);
                            f.noteOn(variant == 1 ? 127 : 0, 1.0f, false);
                            float peak = 0.0f;
                            bool finite = true;
                            for (int i = 0; i < n; ++i)
                            {
                                const float y = f.process(in[(size_t) i], fm[(size_t) i]);
                                finite = finite && std::isfinite(y);
                                peak = std::max(peak, std::abs(y));
                            }
                            worst = std::max(worst, peak);
                            if (! finite || peak > 40.0f)
                                expect(false, juce::String("A ") + FilterSlot::modelNames()[a] + ", B " + FilterSlot::modelNames()[b] + ", routing "
                                              + juce::String(routing) + " at " + juce::String(sr) + ": peak " + juce::String(peak, 2));
                        }
            logMessage("largest sample of any pair of filters at full resonance and drive under full filter FM: " + juce::String(worst, 2));
            expect(worst > 1.0f, "the runs sound");
        }

        beginTest("PVAR on a voice struck again while it sounds: carried to its new place, not jumped");
        {
            auto strikes = [this](float pvar, float& widest)
            {
                W e(1);     // one voice: every note after the first strikes it again
                e.prepareToPlay(48000.0, 64);
                e.setParameter(W::TableSelect, 3.0f);
                e.setParameter(W::AmpSustain, 1.0f);
                e.setParameter(W::SpreadBase + VoiceSpread::Pan, pvar);
                e.setParameter(W::SpreadBase + VoiceSpread::Seed, 3.0f);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::array<std::vector<float>, 2> out;
                float step = 0.0f, low = 1.0e9f, high = 0.0f;
                for (int note = 0; note < 24; ++note)
                {
                    e.noteOn(45, 1.0f, -1);
                    for (int b = 0; b < 60; ++b)
                    {
                        buf.clear();
                        e.processBlock(buf, midi);
                        for (int ch = 0; ch < 2; ++ch)
                            out[(size_t) ch].insert(out[(size_t) ch].end(), buf.getReadPointer(ch), buf.getReadPointer(ch) + 64);
                    }
                    const size_t at = (size_t) note * 60 * 64;
                    if (note > 0)
                        for (auto& side : out)
                            step = std::max(step, largestStep(side, at - 1, at + 480));
                    // where the note sits: the left side's share of it, once it has arrived
                    const double l = rms(std::vector<float>(out[0].begin() + (long) at + 1920, out[0].end()));
                    const double r = rms(std::vector<float>(out[1].begin() + (long) at + 1920, out[1].end()));
                    low = std::min(low, (float) (l / (l + r + 1.0e-12)));
                    high = std::max(high, (float) (l / (l + r + 1.0e-12)));
                }
                widest = high - low;
                juce::ignoreUnused(this);
                return step;
            };
            float still = 0.0f, moved = 0.0f;
            const float plain = strikes(0.0f, still), spread = strikes(1.0f, moved);
            expect(still < 0.001f && moved > 0.3f, "PVAR places the notes: " + juce::String(moved, 3));
            expect(spread <= 2.0f * plain + 0.002f, "the largest step at a note struck again: " + juce::String(spread, 4)
                                                    + " with PVAR, " + juce::String(plain, 4) + " without");
        }

        beginTest("ROUT, TYPB and MODB under a held note are swaps: the engine says so, and its track carries the jump off");
        {
            const int B = W::FilterDualBase - (int) F::Count;
            // The contract (EngineBase::takeJump): a selector that changes is a swap, one that is set to what it is, or a continuous cell, is not.
            {
                auto e = wavetable();
                e->takeJump();
                struct Cell { const char* name; int id; float to; };
                for (auto& c : { Cell { "TYPE", W::FilterBase + F::Type, 2.0f }, Cell { "MODE", W::FilterBase + F::Mode, 1.0f },
                                 Cell { "TYPB", B + F::TypeB, 3.0f }, Cell { "MODB", B + F::ModeB, 1.0f }, Cell { "ROUT", B + F::Routing, 2.0f } })
                {
                    e->setParameter(c.id, c.to);
                    expectEquals(e->takeJump(), (int) EngineBase::SoundSwapped, juce::String(c.name) + " changed");
                    e->setParameter(c.id, c.to);
                    expectEquals(e->takeJump(), (int) EngineBase::NoJump, juce::String(c.name) + " set to what it is");
                }
                for (int id : { B + (int) F::Balance, B + (int) F::OffsetB, B + (int) F::FilterFM, W::FilterBase + (int) F::Cutoff })
                {
                    e->setParameter(id, 0.3f);
                    expectEquals(e->takeJump(), (int) EngineBase::NoJump, "a continuous cell is led, not swapped");
                }
            }
            // Heard on a track, as the app plays it: filter B far from filter A (a resonant high-pass two
            // octaves up), brought in, taken out and brought back, where it comes back with the state it was left in.
            struct Change { const char* name; int offset; float from, to; };
            for (auto& c : { Change { "ROUT SNGL > SER", F::Routing, 0.0f, 1.0f }, Change { "ROUT SNGL > PAR", F::Routing, 0.0f, 2.0f },
                             Change { "ROUT SER > PAR", F::Routing, 1.0f, 2.0f }, Change { "TYPB SVF > LADR", F::TypeB, 0.0f, 2.0f },
                             Change { "TYPB SVF > OFF", F::TypeB, 0.0f, 5.0f }, Change { "MODB HP > LP", F::ModeB, 2.0f, 0.0f } })
            {
                SequencerCore seq;
                AudioEngine audio;
                seq.prepareToPlay(48000.0, 64);
                audio.setSequencer(&seq);
                audio.prepare(48000.0, 64);
                audio.addEngine(0, AudioEngine::createEngine(EngineType::Wavetable));
                seq.getPattern().getTrack(0).engineType = EngineType::Wavetable;
                auto* e = audio.getEngine(0);
                e->setParameter(W::TableSelect, 3.0f);
                e->setParameter(W::AmpSustain, 1.0f);
                e->setParameter(W::FilterBase + F::Cutoff, 400.0f);
                e->setParameter(W::FilterBase + F::Resonance, 0.7f);
                e->setParameter(B + F::ModeB, 2.0f);
                e->setParameter(B + F::ResonanceB, 0.9f);
                e->setParameter(B + F::OffsetB, 24.0f);
                e->setParameter(B + F::Routing, 2.0f);
                e->setParameter(B + c.offset, c.from);
                std::vector<float> out, l(64), r(64);
                e->noteOn(45, 1.0f, -1);
                for (int block = 0; block < 600; ++block)
                {
                    if (block == 200 || block == 300 || block == 400)
                        audio.setTrackParameter(0, B + c.offset, block == 300 ? c.from : c.to);
                    audio.renderBlock(l.data(), r.data(), 64);
                    out.insert(out.end(), l.begin(), l.end());
                }
                const float steady = std::max(largestStep(out, 6400, 12700), largestStep(out, 32000, 38000));
                float atChanges = 0.0f;
                for (int block : { 200, 300, 400 })
                    atChanges = std::max(atChanges, largestStep(out, (size_t) block * 64 - 1, (size_t) block * 64 + 480));
                expect(rmsOfDifference(std::vector<float>(out.begin() + 9600, out.begin() + 12800), std::vector<float>(out.begin() + 16000, out.begin() + 19200)) > 0.003,
                       juce::String(c.name) + " changes the sound");
                expect(steady > 0.001f && atChanges <= 8.0f * steady,
                       juce::String(c.name) + ": a step of " + juce::String(atChanges, 4) + " at the change against " + juce::String(steady, 4));
            }
        }

        beginTest("Nothing is allocated: notes and blocks with both filters, filter FM and the whole spread");
        {
            auto e = wavetable();
            const int B = W::FilterDualBase - (int) F::Count;
            e->setParameter(B + F::Routing, 2.0f);
            e->setParameter(B + F::FilterFM, 1.0f);
            for (int o = 0; o < (int) VoiceSpread::Seed; ++o)
                e->setParameter(W::SpreadBase + o, o == 0 ? 50.0f : 1.0f);
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            auto notes = [&](int count)
            {
                for (int i = 0; i < count; ++i)
                {
                    e->noteOn(40 + i % 30, 0.8f, -1);
                    if (i % 4 == 0) e->setParameter(B + F::Balance, 0.01f * (float) (i % 100));
                    if (i % 5 == 0) e->setParameter(B + F::Routing, (float) (1 + i % 2));
                    buf.clear();
                    e->processBlock(buf, midi);
                    if (i % 3 == 0) e->noteOff(40 + i % 30, -1);
                }
            };
            notes(24);      // (everything has been through once)
           #if defined(__has_feature) && __has_feature(address_sanitizer)
            logMessage("(a sanitizer's allocator is not seen by malloc_logger: not counted in this build)");
            expect(true);
           #else
            auto* const before = malloc_logger;
            malloc_logger = ModRouterAllocations::count;
            ModRouterAllocations::events = 0;
            ModRouterAllocations::counting = true;
            { void* volatile probe = std::malloc(4096); std::free(probe); }     // (volatile: an optimised build keeps the pair)
            ModRouterAllocations::counting = false;
            expect(ModRouterAllocations::events >= 2, "the counter counts");
            ModRouterAllocations::events = 0;
            ModRouterAllocations::counting = true;
            notes(96);
            ModRouterAllocations::counting = false;
            malloc_logger = before;
            expectEquals(ModRouterAllocations::events, 0, "mallocs and frees in 96 notes and blocks");
           #endif
        }
    }
};

static VoiceFilterSpreadTests voiceFilterSpreadTests;
