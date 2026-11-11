#pragma once
#include "../src/engine/SamplerEngine.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <vector>

// The sampler's transient slicing (SLICE page), slice-play modes and time-stretch (STRCH page).
// Everything is rendered through processBlock at 48 kHz from known samples, so onset positions,
// durations and pitch can be measured.
class SamplerStretchTests : public juce::UnitTest
{
public:
    SamplerStretchTests() : UnitTest("Sampler Stretch") {}

    using S = SamplerEngine;
    static constexpr double SR = 48000.0;
    static constexpr double PI = 3.141592653589793;
    static constexpr float CENTRE = 0.70710678f;   // the sampler's pan law at centre

    // ---------------------------------------------------------------------------------------
    // Reference renders of the sampler as it was BEFORE slicing / stretch were added: every play
    // mode, four settings each, hashed bit for bit (hashes taken from the unchanged engine with
    // this same function). Only parameters that existed then are used.
    // The hashes of FWD, REV, 1SHT, GATE and of the first LOOP setting were taken again when a note
    // that reaches the end of its region got a 3 ms fade instead of a cut, and LOOP began to read
    // across the join (sampler_release_tests.h); PING, GRAN and LOOP with moved loop points are
    // the original ones.
    // ---------------------------------------------------------------------------------------
    static constexpr int NUM_LEGACY_CONFIGS = 7 * 4;

    static const uint64_t* legacyHashes()
    {
        static const uint64_t h[NUM_LEGACY_CONFIGS] = {
            0x40095ac86759771bull, 0x7295d70d5e44f698ull, 0x82738397bf64be9bull, 0xaa5435ed85e13714ull,
            0x1752459286a75a43ull, 0x27ec206fbbece45eull, 0xb9f7bd764ab24d97ull, 0xd281d81916b8614cull,
            0x5add9a26972c1f4bull, 0x2977022ee0b73a11ull, 0x82738397bf64be9bull, 0xaa5435ed85e13714ull,
            0x40095ac86759771bull, 0x7295d70d5e44f698ull, 0x82738397bf64be9bull, 0xaa5435ed85e13714ull,
            0x1bbdb1850166121full, 0xc69c92989f9dc34bull, 0x940c301197f9071full, 0x8c01306fe5a0ff8full,
            0xc8c7822cfeee2b13ull, 0x877bf840e8f00d1cull, 0xafbf103c91de084full, 0x64a2ca4e7bc94ccaull,
            0x1d8035a940d8cc73ull, 0xb51b252fd5584f83ull, 0x148ebbe29aae4bd7ull, 0xefec113c501885f3ull };
        return h;
    }

    static std::vector<float> legacySample(int frames, bool stereo)
    {
        std::vector<float> s((size_t) frames * (stereo ? 2u : 1u));
        uint32_t lcg = 12345u;
        for (int i = 0; i < frames; ++i)
        {
            lcg = lcg * 1664525u + 1013904223u;
            const float noise = (float) (int32_t) lcg / 2147483648.0f;
            const double t = (double) i / 44100.0;
            const float env = (float) std::exp(-3.0 * std::fmod(t, 0.25));
            s[(size_t) i] = 0.4f * env * (float) std::sin(2.0 * 3.141592653589793 * 330.0 * t) + 0.1f * env * noise;
            if (stereo)
                s[(size_t) (frames + i)] = 0.4f * env * (float) std::sin(2.0 * 3.141592653589793 * 220.0 * t) - 0.1f * env * noise;
        }
        return s;
    }

    // stretchKnobs: every STRCH cell but ALGO away from its default (they must not be heard while
    // ALGO = OFF).
    static uint64_t legacyHash(int config, bool stretchKnobs = false)
    {
        const int mode = config / 4, variant = config % 4;
        auto e = std::make_unique<SamplerEngine>();
        e->prepareToPlay(48000.0, 64);
        const bool stereo = variant == 1 || variant == 3;
        const int frames = 30000;
        auto data = legacySample(frames, stereo);
        e->loadSample(data.data(), frames, stereo ? 2 : 1, 44100.0);
        e->setParameter(SamplerEngine::Mode, (float) mode);
        if (variant >= 1)
        {
            e->setParameter(SamplerEngine::StartPoint, 0.1f);
            e->setParameter(SamplerEngine::EndPoint, 0.9f);
            e->setParameter(SamplerEngine::LoopStart, 0.2f);
            e->setParameter(SamplerEngine::LoopEnd, 0.7f);
            e->setParameter(SamplerEngine::Release, 0.05f);
        }
        if (variant >= 2)
        {
            e->setParameter(SamplerEngine::SliceCount, 4.0f);
            e->setParameter(SamplerEngine::SliceSelect, 0.5f);
            e->setParameter(SamplerEngine::FilterCutoff, 0.6f);
            e->setParameter(SamplerEngine::FilterEnv, 0.2f);
            e->setParameter(SamplerEngine::PitchSemi, 3.0f);
            e->setParameter(SamplerEngine::FineTune, -0.3f);
            e->setParameter(SamplerEngine::DriveAmt, 0.3f);
            e->setParameter(SamplerEngine::Attack, 0.01f);
        }
        if (variant == 3)
        {
            e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) SamplerEngine::LfoPitch);
            e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.4f);
            e->setParameter(EngineLfoSection::id(1, EngineLfoSection::Dest), (float) SamplerEngine::LfoPan);
            e->setParameter(EngineLfoSection::id(1, EngineLfoSection::Depth), -0.5f);
            e->setParameter(SamplerEngine::GrainSpray, 0.3f);
            e->setParameter(SamplerEngine::GrainSpread, 0.5f);
            e->setParameter(SamplerEngine::StartRandom, 0.2f);
            e->setParameter(SamplerEngine::BitReduce, 0.3f);
            e->setParameter(SamplerEngine::RateReduce, 0.1f);
        }
        if (stretchKnobs && mode != SamplerEngine::Granular)
        {
            e->setParameter(S::StretchSpeed, 2.5f);
            e->setParameter(S::StretchSync, 1.0f);
            e->setParameter(S::StretchLength, 2.0f);
            e->setParameter(S::StretchSize, 0.2f);
            e->setParameter(S::StretchXfade, 0.9f);
            e->setParameter(S::StretchJitter, 0.7f);
            e->setParameter(S::StretchSpread, 0.6f);
            e->setTempo(97.0f);
        }

        uint64_t h = 1469598103934665603ull;
        juce::AudioBuffer<float> buf(2, 64);
        juce::MidiBuffer midi;
        const int blocks = 48000 / 64;      // one second
        for (int b = 0; b < blocks; ++b)
        {
            if (b == 0) e->noteOn(60, 0.9f, -1);
            if (b == 40) e->noteOn(67, 0.6f, -1);
            if (b == 300) e->noteOff(60, -1);
            if (b == 500) e->noteOff(67, -1);
            if (b == 520) e->pitchBend(0.5f);
            if (b == 530) e->noteOn(55, 1.0f, -1);
            buf.clear();
            e->processBlock(buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 64; ++i)
                {
                    uint32_t bits;
                    const float f = buf.getSample(ch, i);
                    std::memcpy(&bits, &f, 4);
                    h = (h ^ bits) * 1099511628211ull;
                }
        }
        return h;
    }

    // --- test samples (48 kHz, mono) ---
    static std::vector<float> tone(double hz, double seconds, float amp = 0.5f)
    {
        std::vector<float> s((size_t) std::lround(seconds * SR));
        for (size_t i = 0; i < s.size(); ++i) s[i] = amp * (float) std::sin(2.0 * PI * hz * (double) i / SR);
        return s;
    }
    // Linear sweep 300 -> 1500 Hz with a little noise: nothing in it repeats.
    static std::vector<float> noisySweep(double seconds)
    {
        std::vector<float> s((size_t) std::lround(seconds * SR));
        double ph = 0.0;
        uint32_t seed = 4242u;
        for (size_t i = 0; i < s.size(); ++i)
        {
            ph += 2.0 * PI * (300.0 + 1200.0 * (double) i / (double) s.size()) / SR;
            s[i] = 0.4f * (float) std::sin(ph) + 0.05f * dsp::xorshiftNoise(seed);
        }
        return s;
    }
    // Eight plateaus: slice i (of 8) holds the value 0.1 * (i + 1).
    static std::vector<float> steps(int framesPerStep)
    {
        std::vector<float> s((size_t) framesPerStep * 8);
        for (size_t i = 0; i < s.size(); ++i) s[i] = 0.1f * (float) (i / (size_t) framesPerStep + 1);
        return s;
    }
    static int stepOf(float outputValue) { return (int) std::lround(outputValue / CENTRE / 0.1f) - 1; }

    struct Burst { double at; float amp; bool noise; };
    // Decaying bursts (a 1 kHz sine or noise, 15 ms time constant, 90 ms long) on silence.
    static std::vector<float> bursts(const std::vector<Burst>& list, double seconds)
    {
        std::vector<float> s((size_t) std::lround(seconds * SR), 0.0f);
        uint32_t seed = 99u;
        for (auto& b : list)
        {
            const size_t at = (size_t) std::lround(b.at * SR);
            for (size_t i = 0; i < (size_t) (0.09 * SR) && at + i < s.size(); ++i)
            {
                const double t = (double) i / SR;
                const float env = b.amp * (float) std::exp(-t / 0.015);
                s[at + i] += env * (b.noise ? dsp::xorshiftNoise(seed) : (float) std::sin(2.0 * PI * 1000.0 * t));
            }
        }
        return s;
    }

    // --- engines and rendering ---
    static std::unique_ptr<SamplerEngine> engine(const std::vector<float>& sample, int algo = S::StretchOff, float speed = 1.0f)
    {
        auto e = std::make_unique<SamplerEngine>();
        e->prepareToPlay(SR, 64);
        e->loadSample(sample.data(), (int) sample.size(), 1, SR);
        e->setParameter(S::VelSens, 0.0f);
        e->setParameter(S::Volume, 1.0f);
        e->setParameter(S::StretchMode, (float) algo);
        e->setParameter(S::StretchSpeed, speed);
        return e;
    }

    static std::vector<float> render(SamplerEngine& e, double seconds, std::vector<float>* right = nullptr)
    {
        std::vector<float> out;
        juce::AudioBuffer<float> buf(2, 64);
        juce::MidiBuffer midi;
        const int blocks = (int) std::lround(seconds * SR / 64.0);
        out.reserve((size_t) blocks * 64);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < 64; ++i)
            {
                out.push_back(buf.getSample(0, i));
                if (right != nullptr) right->push_back(buf.getSample(1, i));
            }
        }
        return out;
    }

    // --- measurements ---
    // Seconds until the last sample above the threshold.
    static double soundEnd(const std::vector<float>& x, float threshold = 2.0e-3f)
    {
        for (size_t i = x.size(); i > 0; --i)
            if (std::abs(x[i - 1]) > threshold)
                return (double) i / SR;
        return 0.0;
    }
    static double goertzel(const std::vector<float>& x, size_t from, size_t n, double hz)
    {
        const double w = 2.0 * PI * hz / SR, c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < n && from + i < x.size(); ++i)
        {
            const double win = 0.5 - 0.5 * std::cos(2.0 * PI * (double) i / (double) n);
            const double s0 = win * x[from + i] + c * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    // The strongest frequency between lo and hi in x[fromSec, fromSec + lenSec).
    static double peakHz(const std::vector<float>& x, double fromSec, double lenSec, double lo = 150.0, double hi = 2000.0)
    {
        double best = -1.0, at = 0.0;
        for (double f = lo; f <= hi; f += 2.0)
        {
            const double p = goertzel(x, (size_t) (fromSec * SR), (size_t) (lenSec * SR), f);
            if (p > best) { best = p; at = f; }
        }
        return at;
    }
    static float rms(const std::vector<float>& x, double fromSec, double toSec)
    {
        const size_t a = (size_t) (fromSec * SR), b = std::min(x.size(), (size_t) (toSec * SR));
        double s = 0.0;
        for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
        return b > a ? (float) std::sqrt(s / (double) (b - a)) : 0.0f;
    }
    static bool allFinite(const std::vector<float>& x, float bound)
    {
        for (float v : x)
            if (! std::isfinite(v) || std::abs(v) > bound)
                return false;
        return true;
    }

    // How far the strongest frequency of a stretched 440 Hz sine may be from 440 Hz. TAPE and WSOL
    // give the sine itself. CYCL joins cycles of 50 ms out of phase, which puts the energy on a comb
    // 20 Hz apart (its buzz); GRAN and SMER add grains with random phases: a narrow band of noise
    // around the pitch, as wide as one over the grain length.
    static double pitchTolerance(int algo)
    {
        if (algo == SamplerEngine::StretchCycle) return 12.0;
        if (algo == SamplerEngine::StretchGrain) return 25.0;
        return 6.0;
    }

    static const char* algoName(int a) { static const char* n[] = { "OFF", "TAPE", "CYCL", "GRAN", "SLCE", "SMER", "WSOL" }; return n[a]; }

    void runTest() override
    {
        beginTest("Rebuild parameters (sample, slicing, normalise) are flagged so screens do not lock them");
        {
            S e;
            for (int id : { (int) S::SampleSelect, (int) S::SliceCount, (int) S::SliceMode, (int) S::SliceSens,
                            (int) S::SliceGap, (int) S::Normalize })
                expect(e.isRebuildParam(id), "id " + juce::String(id));
            for (int id : { (int) S::SliceSelect, (int) S::StretchMode, (int) S::StretchSpeed, (int) S::StartPoint })
                expect(! e.isRebuildParam(id), "id " + juce::String(id));
        }

        beginTest("Pages: SAMPL GRAIN SLICE STRCH TONE FILTR AMP STRIP LFO1 LFO2, eight working cells each, ids round-trip");
        {
            S e;
            auto pages = e.getParamPages();
            const char* names[] = { "SAMPL", "GRAIN", "SLICE", "STRCH", "TONE", "FILTR", "AMP", "STRIP", "LFO1", "LFO2" };
            expectEquals((int) pages.size(), 10);
            const char* cells[4][8] = {
                { "SMPL", "MODE", "STRT", "END", "LPST", "LPEN", "ROOT", "NORM" },
                { "SIZE", "DENS", "SCAN", "SPRY", "PJIT", "SPRD", "SHAP", "RVRS" },
                { "MODE", "SLCS", "SENS", "GAP", "SLIC", "PLAY", "FADE", "SLEN" },
                { "ALGO", "SPD", "SYNC", "LEN", "SIZE", "XFAD", "JIT", "SPRD" } };
            const int cellIds[4][8] = {
                { 24, 6, 1, 2, 3, 4, 46, 47 }, { 32, 33, 34, 35, 36, 37, 38, 39 },
                { 40, 18, 41, 42, 0, 43, 44, 45 }, { 48, 49, 50, 51, 52, 53, 54, 55 } };
            std::set<int> seen;
            for (size_t pi = 0; pi < pages.size() && pi < 10; ++pi)
            {
                expect(pages[pi].name == names[pi], names[pi]);
                expect(pages[pi].name.length() <= 5);
                for (int c = 0; c < 8; ++c)
                {
                    const auto& p = pages[pi].params[(size_t) c];
                    const juce::String label(p.label);
                    expect(p.isValid(), pages[pi].name + " cell " + juce::String(c));
                    expect(label.length() <= 4 && label == label.toUpperCase(), label);
                    expect(seen.insert(p.id).second, "id used once: " + label);
                    expect((p.id >= 0 && p.id < 100) || EngineLfoSection::owns(p.id), label);
                    expect(e.getParameterName(p.id).isNotEmpty(), label);
                    if (pi < 4)
                    {
                        expect(label == cells[pi][c], cells[pi][c]);
                        expectEquals(p.id, cellIds[pi][c]);
                    }
                    if (p.kind == ParamKind::Choice)
                    {
                        expect(p.choices != nullptr && p.numChoices == (int) p.maxVal + 1, label);
                        if (p.id != S::SampleSelect && pi < 7)      // the bank's names and the LFO pages are not this test's
                            for (int i = 0; i < p.numChoices; ++i)
                                expect(juce::String(p.choices[i]).length() <= 4, label + " choice " + juce::String(i));
                    }
                    if (p.id == S::SampleSelect || p.id == S::SliceCount)   // SLCS: the bank sample's own suggestion
                        continue;
                    expectEquals(e.getParameter(p.id), p.defaultVal, label + " default");
                }
            }
            // Set every cell, then read every cell back (TYPE before MODE so the filter mode is in range).
            for (auto& page : pages)
                for (auto& p : page.params)
                    if (p.id != S::SampleSelect && p.id != S::FilterMode)
                        e.setParameter(p.id, p.fromNorm(0.4f));
            for (auto& page : pages)
                for (auto& p : page.params)
                    if (p.id != S::SampleSelect && p.id != S::FilterMode)
                        expectEquals(e.getParameter(p.id), p.fromNorm(0.4f), juce::String(p.label) + " round trip");

            expectEquals((int) S::NumParams, 56);
            expectEquals(e.getParameterCount(), 56);
            expectEquals(e.getMasterParamId(), (int) S::SampleSelect);

            // SPD: Func lands on 25 / 50 / 100 / 200 / 400 %, plain steps are about 3 %.
            const auto& spd = pages[3].params[1];
            float v = 1.0f;
            v = spd.turned(v, 1, TurnStep::Coarse);  expectWithinAbsoluteError(v, 2.0f, 1.0e-4f);
            v = spd.turned(v, 1, TurnStep::Coarse);  expectWithinAbsoluteError(v, 4.0f, 1.0e-4f);
            v = spd.turned(v, -3, TurnStep::Coarse); expectWithinAbsoluteError(v, 0.5f, 1.0e-4f);
            v = spd.turned(v, -1, TurnStep::Coarse); expectWithinAbsoluteError(v, 0.25f, 1.0e-4f);
            expectWithinAbsoluteError(spd.turned(1.0f, 1, TurnStep::Plain), 1.028f, 0.002f);
            expectWithinAbsoluteError(spd.turned(1.0f, 1, TurnStep::Fine), 1.0028f, 0.0005f);
            expect(juce::String(pages[3].params[3].choices[4]) == "1");

            // LFO destinations: the old ones in place, SPD appended.
            expectEquals((int) S::NumLfoDests, 12);
            expectEquals((int) S::LfoSpeed, 11);
            expect(juce::String(S::lfoDestLabels()[11]) == "SPD" && juce::String(S::lfoDestLabels()[10]) == "DENS");
        }

        beginTest("Stretch OFF: every play mode is bit-identical to the engine before the change");
        {
            for (int c = 0; c < NUM_LEGACY_CONFIGS; ++c)
            {
                expect(legacyHash(c) == legacyHashes()[c], "config " + juce::String(c));
                expect(legacyHash(c, true) == legacyHashes()[c], "config " + juce::String(c) + " with the STRCH cells moved");
            }
        }

        const std::vector<Burst> train = { { 0.10, 0.8f, false }, { 0.31, 0.5f, true }, { 0.47, 0.7f, false },
                                           { 0.80, 0.4f, true }, { 1.22, 0.6f, false }, { 1.45, 0.3f, false } };

        beginTest("Transients: an uneven burst train gives one slice per burst, within 3 ms, on zero crossings");
        {
            auto sample = bursts(train, 2.0);
            auto e = engine(sample);
            expectEquals(e->getSliceCount(), 0);
            e->setParameter(S::SliceMode, (float) S::SliceTransient);
            expectEquals(e->getSliceCount(), 6);
            expectEquals(e->getParameter(S::SliceCount), 6.0f, "the SLCS cell shows how many were found");
            const int n = (int) sample.size();
            for (int i = 0; i < 6 && i < e->getSliceCount(); ++i)
            {
                const double at = (double) e->getSliceStartNormalized(i) * n / SR;
                expectWithinAbsoluteError(at, train[(size_t) i].at, 0.003, "slice " + juce::String(i));
                expectWithinAbsoluteError((double) e->getSlice(i).startSample / SR, train[(size_t) i].at, 0.003);
                expect(std::abs(sample[(size_t) e->getSlice(i).startSample]) < 0.02f, "starts near zero");
                expectEquals(e->getSlice(i).endSample, i < 5 ? e->getSlice(i + 1).startSample : n);
            }
            expectEquals(e->getSliceStartNormalized(6), -1.0f);
            expectEquals(e->getSliceStartNormalized(-1), -1.0f);

            // Sine bursts start exactly on their first sample (the zero crossing before the rise).
            expectEquals(e->getSlice(0).startSample, (int) std::lround(0.10 * SR));
            expectEquals(e->getSlice(2).startSample, (int) std::lround(0.47 * SR));

            // SLCS limits the number: the strongest stay. 1 = no limit.
            e->setParameter(S::SliceCount, 3.0f);
            expectEquals(e->getSliceCount(), 3);
            expectEquals(e->getParameter(S::SliceCount), 3.0f);
            expectWithinAbsoluteError((double) e->getSlice(0).startSample / SR, 0.10, 0.003);
            expectWithinAbsoluteError((double) e->getSlice(1).startSample / SR, 0.47, 0.003);
            expectWithinAbsoluteError((double) e->getSlice(2).startSample / SR, 1.22, 0.003);
            expectEquals(e->getSlice(2).endSample, n);
            e->setParameter(S::SliceCount, 40.0f);
            expectEquals(e->getSliceCount(), 6);
            e->setParameter(S::SliceCount, 1.0f);
            expectEquals(e->getSliceCount(), 6);

            // Back to GRID: equal parts, as the cell says.
            e->setParameter(S::SliceCount, 4.0f);
            e->setParameter(S::SliceMode, (float) S::SliceGrid);
            expectEquals(e->getSliceCount(), 4);
            expectEquals(e->getSlice(1).startSample, n / 4);
            expectEquals(e->getParameter(S::SliceCount), 4.0f);

            // Another sample rate, same result in seconds; a new sample is analysed when it is loaded.
            std::vector<float> at44((size_t) (2.0 * 44100.0), 0.0f);
            for (auto& b : train)
                for (int i = 0; i < 3000; ++i)
                    at44[(size_t) std::lround(b.at * 44100.0) + (size_t) i] += b.amp * (float) (std::exp(-i / 660.0) * std::sin(2.0 * PI * 1000.0 * i / 44100.0));
            e->setParameter(S::SliceMode, (float) S::SliceTransient);
            e->setParameter(S::SliceCount, 1.0f);
            e->loadSample(at44.data(), (int) at44.size(), 1, 44100.0);
            expectEquals(e->getSliceCount(), 6);
            for (int i = 0; i < 6 && i < e->getSliceCount(); ++i)
                expectWithinAbsoluteError((double) e->getSlice(i).startSample / 44100.0, train[(size_t) i].at, 0.003);
        }

        beginTest("Transients: a steady tone has none; sound before the first onset is the first slice");
        {
            auto e = engine(tone(220.0, 2.0));
            e->setParameter(S::SliceMode, (float) S::SliceTransient);
            for (float sens : { 0.0f, 0.5f, 1.0f })
            {
                e->setParameter(S::SliceSens, sens);
                expectEquals(e->getSliceCount(), 0, "sens " + juce::String(sens));
                expectEquals(e->getParameter(S::SliceCount), 1.0f);
            }
            auto low = engine(tone(55.0, 2.0));
            low->setParameter(S::SliceMode, (float) S::SliceTransient);
            expectEquals(low->getSliceCount(), 0, "a low steady tone");

            // A held tone with bursts on top: the tone before the first burst is slice 1.
            auto mix = tone(110.0, 2.0, 0.2f);
            auto hits = bursts({ { 0.5, 0.7f, true }, { 1.2, 0.7f, true } }, 2.0);
            for (size_t i = 0; i < mix.size(); ++i) mix[i] += hits[i];
            auto m = engine(mix);
            m->setParameter(S::SliceMode, (float) S::SliceTransient);
            expectEquals(m->getSliceCount(), 3);
            expectEquals(m->getSlice(0).startSample, 0);
            expectWithinAbsoluteError((double) m->getSlice(1).startSample / SR, 0.5, 0.004);
            expectWithinAbsoluteError((double) m->getSlice(2).startSample / SR, 1.2, 0.004);
        }

        beginTest("Transients: SENS brings in the quiet hits, GAP merges close ones, never more than 64 slices");
        {
            auto withQuiet = train;
            withQuiet.push_back({ 0.62, 0.016f, false });      // 34 dB below the loudest
            withQuiet.push_back({ 1.00, 0.016f, true });
            auto e = engine(bursts(withQuiet, 2.0));
            e->setParameter(S::SliceMode, (float) S::SliceTransient);
            e->setParameter(S::SliceSens, 0.3f);
            const int few = e->getSliceCount();
            e->setParameter(S::SliceSens, 0.9f);
            const int many = e->getSliceCount();
            expectEquals(few, 6);
            expectEquals(many, 8);
            int previous = 0;
            for (float sens = 0.0f; sens <= 1.0f; sens += 0.1f)
            {
                e->setParameter(S::SliceSens, sens);
                expect(e->getSliceCount() >= previous, "more sensitive never finds fewer here");
                previous = e->getSliceCount();
            }

            // Two hits 40 ms apart.
            auto pair = engine(bursts({ { 0.2, 0.8f, false }, { 0.24, 0.8f, true }, { 0.6, 0.8f, false } }, 1.0));
            pair->setParameter(S::SliceMode, (float) S::SliceTransient);
            pair->setParameter(S::SliceGap, 0.02f);
            expectEquals(pair->getSliceCount(), 3);
            pair->setParameter(S::SliceGap, 0.1f);
            expectEquals(pair->getSliceCount(), 2);
            expectWithinAbsoluteError((double) pair->getSlice(1).startSample / SR, 0.6, 0.003);

            // 130 clicks, 30 ms apart: the engine holds 64 at most, in order, inside the sample.
            std::vector<float> clicks((size_t) (4.0 * SR), 0.0f);
            for (int c = 0; c < 130; ++c)
                for (int i = 0; i < 48; ++i)
                    clicks[(size_t) (c * 1440 + 960 + i)] = 0.8f * (float) std::sin(2.0 * PI * i / 12.0) * (1.0f - i / 48.0f);
            auto dense = engine(clicks);
            dense->setParameter(S::SliceCount, 1.0f);       // no limit of its own
            dense->setParameter(S::SliceMode, (float) S::SliceTransient);
            dense->setParameter(S::SliceGap, 0.01f);
            dense->setParameter(S::SliceSens, 1.0f);
            const int count = dense->getSliceCount();
            expectEquals(count, 64);
            expect(count <= S::MAX_SLICES);
            expectEquals(dense->getParameter(S::SliceCount), (float) count);
            for (int i = 0; i < count; ++i)
            {
                const auto& sl = dense->getSlice(i);
                expect(sl.startSample >= 0 && sl.endSample > sl.startSample && sl.endSample <= (int) clicks.size());
                if (i > 0) expect(sl.startSample == dense->getSlice(i - 1).endSample);
            }
        }

        beginTest("Slice play: ONE, KEYS from the root key, NEXT in order and wrapping, RAND covers them all");
        {
            const int stepLen = 2400;       // 50 ms
            auto e = engine(steps(stepLen));
            e->setParameter(S::SliceCount, 8.0f);
            expectEquals(e->getSliceCount(), 8);
            for (int i = 0; i < 8; ++i)
                expectWithinAbsoluteError(e->getSliceStartNormalized(i), (float) i / 8.0f, 1.0e-6f);
            expectEquals(e->getPlayingSlice(), -1);

            // Plays one note to the end of its slice; returns the slice heard and how long it lasted.
            auto play = [&](int note, double* seconds = nullptr, int* reported = nullptr)
            {
                e->noteOn(note, 1.0f, -1);
                if (reported != nullptr) *reported = e->getPlayingSlice();
                auto out = render(*e, 0.07);
                if (seconds != nullptr) *seconds = soundEnd(out);
                return stepOf(out[480]);
            };

            e->setParameter(S::SliceSelect, 0.5f);
            int reported = -2;
            double seconds = 0.0;
            expectEquals(play(60, &seconds, &reported), 4);
            expectEquals(reported, 4);
            expectWithinAbsoluteError(seconds, 0.05, 0.001);
            expectEquals(e->getPlayingSlice(), -1, "nothing sounds any more");
            expectEquals(play(72, &seconds), 4, "ONE: the key transposes, the slice stays");
            expectWithinAbsoluteError(seconds, 0.025, 0.001);

            e->setParameter(S::SlicePlay, (float) S::SliceKeys);
            for (int k = 0; k < 8; ++k)
            {
                expectEquals(play(60 + k, &seconds, &reported), k, "KEYS key " + juce::String(k));
                expectEquals(reported, k);
                expectWithinAbsoluteError(seconds, 0.05, 0.001, "KEYS: every slice at the sample's own pitch");
            }
            expectEquals(play(50), 0, "below the root: the first slice");
            expectEquals(play(90), 7, "above the last: the last slice");
            e->setParameter(S::RootNote, 48.0f);
            expectEquals(play(48), 0);
            expectEquals(play(51), 3);
            e->setParameter(S::PitchSemi, 12.0f);
            play(51, &seconds);
            expectWithinAbsoluteError(seconds, 0.025, 0.001, "PTCH still transposes in KEYS");
            e->setParameter(S::PitchSemi, 0.0f);

            // ROOT without KEYS: the root key plays the original pitch.
            e->setParameter(S::SlicePlay, (float) S::SliceOne);
            play(48, &seconds);
            expectWithinAbsoluteError(seconds, 0.05, 0.001);
            play(60, &seconds);
            expectWithinAbsoluteError(seconds, 0.025, 0.001);
            e->setParameter(S::RootNote, 60.0f);

            e->setParameter(S::SlicePlay, (float) S::SliceNext);
            e->allNotesOff();
            for (int i = 0; i < 19; ++i)
                expectEquals(play(60), i % 8, "NEXT note " + juce::String(i));
            e->allNotesOff();
            expectEquals(play(60), 0, "NEXT starts again after all notes off");

            e->setParameter(S::SlicePlay, (float) S::SliceRandom);
            e->setSliceSeed(7u);
            std::vector<int> first;
            std::set<int> heard;
            bool inOrder = true;
            for (int i = 0; i < 80; ++i)
            {
                first.push_back(play(60, nullptr, &reported));
                expectEquals(reported, first.back());
                expect(first.back() >= 0 && first.back() < 8);
                heard.insert(first.back());
                if (i > 0 && first[(size_t) i] != (first[(size_t) i - 1] + 1) % 8) inOrder = false;
            }
            expectEquals((int) heard.size(), 8, "RAND reaches every slice");
            expect(! inOrder);
            e->setSliceSeed(7u);
            for (int i = 0; i < 80; ++i)
                expectEquals(play(60), first[(size_t) i], "same seed, same order");

            // FADE and SLEN.
            e->setParameter(S::SlicePlay, (float) S::SliceOne);
            e->setParameter(S::SliceSelect, 0.0f);
            e->noteOn(60, 1.0f, -1);
            auto plain = render(*e, 0.07);
            e->setParameter(S::SliceFade, 0.02f);
            e->noteOn(60, 1.0f, -1);
            auto faded = render(*e, 0.07);
            expectWithinAbsoluteError(faded[1200], plain[1200], 1.0e-6f, "before the fade");
            expectWithinAbsoluteError(faded[1920] / plain[1920], 0.5f, 0.02f, "half-way through the fade");
            // (Without FADE the slice keeps its level until the 3 ms declick at its very end.)
            expect(std::abs(faded[2390]) < 0.01f * CENTRE && std::abs(plain[2200]) > 0.09f * CENTRE);
            e->setParameter(S::SliceFade, 0.0f);
            e->setParameter(S::SliceLen, 0.5f);
            play(60, &seconds);
            expectWithinAbsoluteError(seconds, 0.025, 0.001, "SLEN 50 %");
        }

        beginTest("Stretch: the note lasts length / speed in every algorithm; pitch stays except in TAPE");
        {
            auto sample = tone(440.0, 0.5);
            for (int algo : { S::StretchTape, S::StretchCycle, S::StretchGrain, S::StretchSlice, S::StretchWsol })
                for (float speed : { 0.5f, 1.0f, 2.0f })
                {
                    const juce::String what = juce::String(algoName(algo)) + " at " + juce::String(speed);
                    auto e = engine(sample, algo, speed);
                    if (algo == S::StretchSlice)
                        e->setParameter(S::SliceCount, 64.0f);      // 7.8 ms slices: no gap is longer than that
                    expectWithinAbsoluteError(e->getStretchSpeed(), speed, 1.0e-5f);
                    e->noteOn(60, 1.0f, -1);
                    auto out = render(*e, 1.3);
                    const double expected = 0.5 / speed;
                    expectWithinAbsoluteError(soundEnd(out), expected, algo == S::StretchSlice && speed < 1.0f ? 0.012 : 0.005, what + " duration");
                    const double hz = peakHz(out, expected * 0.25, expected * 0.5);
                    if (algo == S::StretchSlice)
                        continue;       // slices, gaps and cuts: the pitch is checked in the SLCE test
                    expectWithinAbsoluteError(hz, algo == S::StretchTape ? 440.0 * speed : 440.0, pitchTolerance(algo), what + " pitch");
                    expect(rms(out, expected * 0.3, expected * 0.7) > 0.1f, what + " level");
                    expect(allFinite(out, 2.0f));
                }

            // SMER: a sixteenth of the SPD cell (4.00 = 4 x as long, 1.00 = 16 x).
            for (float speed : { 4.0f, 2.0f, 1.0f })
            {
                auto e = engine(sample, S::StretchSmear, speed);
                expectWithinAbsoluteError(e->getStretchSpeed(), speed / 16.0f, 1.0e-6f);
                e->noteOn(60, 1.0f, -1);
                const double expected = 0.5 * 16.0 / speed;
                auto out = render(*e, expected + 0.5);
                expectWithinAbsoluteError(soundEnd(out), expected, 0.01, "SMER duration at " + juce::String(speed));
                expectWithinAbsoluteError(peakHz(out, expected * 0.4, 0.4), 440.0, 4.0, "SMER pitch");
                expect(rms(out, expected * 0.4, expected * 0.8) > 0.1f && allFinite(out, 2.0f));
            }

            // Pitch and time are separate: an octave up is an octave up and lasts as long; PTCH,
            // FINE and bend move the pitch only. In TAPE they move the length too.
            for (int algo : { S::StretchCycle, S::StretchGrain, S::StretchWsol, S::StretchSmear })
            {
                auto e = engine(sample, algo, algo == S::StretchSmear ? 4.0f : 0.5f);
                const double expected = algo == S::StretchSmear ? 2.0 : 1.0;
                e->noteOn(72, 1.0f, -1);
                auto out = render(*e, expected + 0.3);
                expectWithinAbsoluteError(soundEnd(out), expected, 0.01, juce::String(algoName(algo)) + " key 72 duration");
                expectWithinAbsoluteError(peakHz(out, 0.4, 0.4), 880.0, pitchTolerance(algo) + 2.0, juce::String(algoName(algo)) + " key 72 pitch");

                e->setParameter(S::PitchSemi, -12.0f);
                e->setParameter(S::BendRange, 12.0f);
                e->pitchBend(1.0f);
                e->noteOn(60, 1.0f, -1);
                out = render(*e, expected + 0.3);
                expectWithinAbsoluteError(soundEnd(out), expected, 0.01);
                expectWithinAbsoluteError(peakHz(out, 0.4, 0.4), 440.0, pitchTolerance(algo), juce::String(algoName(algo)) + " PTCH -12 and bend +12");
            }
            auto t = engine(sample, S::StretchTape, 0.5f);
            t->noteOn(72, 1.0f, -1);
            auto out = render(*t, 1.0);
            expectWithinAbsoluteError(soundEnd(out), 0.5, 0.005, "TAPE: key and speed both set the length");
            expectWithinAbsoluteError(peakHz(out, 0.1, 0.3), 440.0, 6.0);
        }

        beginTest("Stretch at 100 %: CYCL, WSOL and TAPE are the unstretched sound; GRAN and SLCE close to it");
        {
            auto sample = noisySweep(0.5);
            auto ref = engine(sample);
            ref->noteOn(60, 1.0f, -1);
            auto dry = render(*ref, 0.6);
            auto worst = [&](int algo, double fromSec, double toSec, int slices = 1)
            {
                auto e = engine(sample, algo, 1.0f);
                if (slices > 1) e->setParameter(S::SliceCount, (float) slices);
                e->noteOn(60, 1.0f, -1);
                auto wet = render(*e, 0.6);
                float d = 0.0f;
                for (size_t i = (size_t) (fromSec * SR); i < (size_t) (toSec * SR); ++i)
                    d = std::max(d, std::abs(wet[i] - dry[i]));
                return d;
            };
            // Tolerances (peak difference; the signal peaks at about 0.3 per side):
            expect(worst(S::StretchTape, 0.0, 0.6) < 1.0e-6f, "TAPE: the same samples");
            expect(worst(S::StretchCycle, 0.0, 0.6) < 1.0e-5f, "CYCL: cycles join seamlessly");
            expect(worst(S::StretchWsol, 0.0, 0.6) < 1.0e-5f, "WSOL: the best match is the continuation itself");
            expect(worst(S::StretchSlice, 0.0, 0.49) < 1.0e-5f, "SLCE without slices, up to the 3 ms end fade");
            expect(worst(S::StretchSlice, 0.0, 0.49, 8) < 1.0e-5f, "SLCE with 8 slices: each starts as the last ends");
            // GRAN: the grains line up, so it is the same sound with a level ripple of about +-2 dB
            // (scattered grain intervals), between the first and the last grain length (50 ms; the
            // grain engine keeps the last grains inside the sample, which repeats its end).
            expect(worst(S::StretchGrain, 0.06, 0.44) < 0.1f, "GRAN: within a quarter of the signal's peak (0.32)");
            // SMER is not meant to (its slowest speed is 4 x as long).
        }

        beginTest("WSOL splices in phase: a stretched sine keeps its level where CYCL beats");
        {
            auto flatness = [&](int algo, float speed)
            {
                auto e = engine(tone(440.0, 1.0), algo, speed);
                e->noteOn(60, 1.0f, -1);
                auto out = render(*e, 0.9 / speed);
                float lo = 1.0e9f, hi = 0.0f;
                for (double t = 0.1; t + 0.01 < 0.8 / speed; t += 0.002)
                {
                    const float r = rms(out, t, t + 0.01);
                    lo = std::min(lo, r);
                    hi = std::max(hi, r);
                }
                return lo / hi;
            };
            for (float speed : { 0.37f, 0.5f, 1.7f, 3.1f })
            {
                const float w = flatness(S::StretchWsol, speed), c = flatness(S::StretchCycle, speed);
                expect(w > 0.9f, "WSOL at " + juce::String(speed) + ": " + juce::String(w));
                expect(c < w, "CYCL at " + juce::String(speed) + ": " + juce::String(c));
            }
        }

        beginTest("SLCE: every slice starts at its stretched grid time, at its own speed, and is cut by the next");
        {
            const int stepLen = 4800;       // 100 ms per slice, 0.8 s in all
            auto sample = steps(stepLen);
            for (float speed : { 0.5f, 1.0f, 2.0f })
            {
                auto e = engine(sample, S::StretchSlice, speed);
                e->setParameter(S::SliceCount, 8.0f);
                e->setParameter(S::SlicePlay, (float) S::SliceRandom);      // not used by SLCE
                e->noteOn(60, 1.0f, -1);
                expectEquals(e->getPlayingSlice(), -1);
                auto out = render(*e, 0.8 / speed + 0.1);
                const double grid = 0.1 / speed;
                for (int i = 0; i < 8; ++i)
                {
                    const size_t at = (size_t) std::lround(i * grid * SR);
                    expectEquals(stepOf(out[at + 480]), i, "slice " + juce::String(i) + " at speed " + juce::String(speed));
                    if (i > 0)
                        expect(stepOf(out[at - 96]) != i, "not before its time");
                    if (speed < 1.0f)
                    {
                        expectEquals(stepOf(out[at + 4320]), i, "plays its own length");
                        expect(std::abs(out[at + 5280]) < 1.0e-6f, "then a gap until the next slice");
                    }
                }
                expectWithinAbsoluteError(soundEnd(out), speed < 1.0f ? 7 * grid + 0.1 : 0.8 / speed, 0.004);
                expectEquals(e->getPlayingSlice(), -1);
            }

            // Sliced sound keeps its pitch at any speed and with FADE the slice ends smoothly.
            auto e = engine(tone(440.0, 0.8), S::StretchSlice, 0.5f);
            e->setParameter(S::SliceCount, 8.0f);
            e->noteOn(60, 1.0f, -1);
            auto out = render(*e, 0.3);
            expectWithinAbsoluteError(peakHz(out, 0.01, 0.08), 440.0, 6.0);
            expect(rms(out, 0.105, 0.195) < 1.0e-6f, "the gap");
            expectEquals(e->getPlayingSlice(), 1, "the slice the note has reached");
            e->allNotesOff();
            e->setParameter(S::SliceFade, 0.05f);
            e->noteOn(60, 1.0f, -1);
            auto faded = render(*e, 0.3);
            expect(rms(faded, 0.08, 0.1) < 0.5f * rms(out, 0.08, 0.1));
            expectWithinAbsoluteError(rms(faded, 0.0, 0.04), rms(out, 0.0, 0.04), 1.0e-5f);

            // Transient slices: each burst is retriggered at its stretched time.
            auto drums = engine(bursts(train, 2.0), S::StretchSlice, 0.5f);
            drums->setParameter(S::SliceMode, (float) S::SliceTransient);
            drums->noteOn(60, 1.0f, -1);
            auto slow = render(*drums, 4.0);
            for (auto& b : train)
            {
                expect(rms(slow, 2.0 * b.at + 0.001, 2.0 * b.at + 0.02) > 0.02f, "burst at " + juce::String(b.at));
                expect(rms(slow, 2.0 * b.at - 0.03, 2.0 * b.at - 0.003) < 0.003f, "silence before it");
            }
        }

        beginTest("SYNC: the loop lasts LEN at the tempo, at two tempos and through a tempo change");
        {
            auto sample = tone(440.0, 1.0);
            for (int algo : { S::StretchTape, S::StretchCycle, S::StretchGrain, S::StretchWsol, S::StretchSmear })
                for (float bpm : { 120.0f, 90.0f, 150.0f })
                {
                    const juce::String what = juce::String(algoName(algo)) + " at " + juce::String(bpm);
                    auto e = engine(sample, algo, 3.3f);            // SPD is not used while SYNC is on
                    e->setParameter(S::StretchSync, 1.0f);
                    e->setParameter(S::StretchLength, 3.0f);        // half a bar = two beats
                    e->setTempo(bpm);
                    const double expected = 2.0 * 60.0 / bpm;
                    expectWithinAbsoluteError((double) e->getStretchSpeed(), 1.0 / expected, 1.0e-4, what);
                    e->noteOn(60, 1.0f, -1);
                    auto out = render(*e, expected + 0.3);
                    expectWithinAbsoluteError(soundEnd(out), expected, 0.008, what + " duration");
                    expectWithinAbsoluteError(peakHz(out, 0.3, 0.3), algo == S::StretchTape ? 440.0 / expected : 440.0, pitchTolerance(algo), what + " pitch");
                }

            // LEN: a 1 s loop over one bar at 120 = half speed; over 1/4 bar = double speed.
            auto e = engine(sample, S::StretchCycle);
            e->setParameter(S::StretchSync, 1.0f);
            e->setTempo(120.0f);
            const float bars[] = { 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
            for (int i = 0; i < 10; ++i)
            {
                e->setParameter(S::StretchLength, (float) i);
                expectWithinAbsoluteError(e->getStretchSpeed(), 1.0f / (bars[i] * 2.0f), 1.0e-4f, "LEN " + juce::String(i));
            }
            // The region counts: half the sample over half a bar = half speed.
            e->setParameter(S::StretchLength, 3.0f);
            e->setParameter(S::EndPoint, 0.5f);
            expectWithinAbsoluteError(e->getStretchSpeed(), 0.5f, 1.0e-4f);
            e->noteOn(60, 1.0f, -1);
            expectWithinAbsoluteError(soundEnd(render(*e, 1.2)), 1.0, 0.005);
            // With slices a note plays one slice at the whole loop's speed.
            e->setParameter(S::EndPoint, 1.0f);
            e->setParameter(S::SliceCount, 4.0f);
            e->setTempo(60.0f);
            expectWithinAbsoluteError(e->getStretchSpeed(), 0.5f, 1.0e-4f);
            e->noteOn(60, 1.0f, -1);
            expectWithinAbsoluteError(soundEnd(render(*e, 0.7)), 0.5, 0.005, "a quarter of the loop, half speed");

            // A tempo change under a held note: half the loop at 120 (0.5 s), the rest at 60 (1 s).
            for (int algo : { S::StretchTape, S::StretchCycle, S::StretchSlice, S::StretchWsol })
            {
                auto live = engine(sample, algo);
                if (algo == S::StretchSlice) live->setParameter(S::SliceCount, 64.0f);
                live->setParameter(S::StretchSync, 1.0f);
                live->setParameter(S::StretchLength, 3.0f);
                live->setTempo(120.0f);
                live->noteOn(60, 1.0f, -1);
                auto out = render(*live, 0.5);
                live->setTempo(60.0f);
                auto rest = render(*live, 1.3);
                out.insert(out.end(), rest.begin(), rest.end());
                expectWithinAbsoluteError(soundEnd(out), 1.5, 0.02, juce::String(algoName(algo)) + " follows the tempo");
            }

            // GRAN mode: SYNC makes SCAN 1.00 = the region over LEN.
            auto g = engine(sample);
            g->setParameter(S::Mode, (float) S::Granular);
            g->setParameter(S::StretchSync, 1.0f);
            g->setParameter(S::StretchLength, 4.0f);     // one bar = 2 s at 120
            g->setTempo(120.0f);
            g->noteOn(60, 1.0f, -1);
            render(*g, 1.0);
            expectWithinAbsoluteError(g->getPlayheadNormalized(), 0.5f, 0.01f);
            g->setParameter(S::StretchSync, 0.0f);
            g->allNotesOff();
            g->noteOn(60, 1.0f, -1);
            render(*g, 0.5);
            expectWithinAbsoluteError(g->getPlayheadNormalized(), 0.5f, 0.01f, "SCAN as before without SYNC");
        }

        beginTest("Stretch with the play modes: REV runs backwards, LOOP and PING go on, GATE and the envelope end the note");
        {
            auto sample = noisySweep(0.5);      // 300 Hz at the start, 1500 Hz at the end
            for (int algo : { S::StretchTape, S::StretchCycle, S::StretchGrain, S::StretchWsol })
            {
                const juce::String name(algoName(algo));
                const bool tape = algo == S::StretchTape;
                auto e = engine(sample, algo, 0.5f);
                e->setParameter(S::Mode, (float) S::Reverse);
                e->noteOn(60, 1.0f, -1);
                auto out = render(*e, 1.2);
                expectWithinAbsoluteError(soundEnd(out), 1.0, 0.01, name + " REV duration");
                const double early = peakHz(out, 0.1, 0.1, 100.0, 1800.0), late = peakHz(out, 0.8, 0.1, 100.0, 1800.0);
                expect(early > late + 300.0, name + " REV: high first (" + juce::String(early) + " then " + juce::String(late) + ")");
                if (! tape)
                    expectWithinAbsoluteError(early, 1500.0 - 1200.0 * 0.15, 80.0, name + " REV at the sample's pitch");

                for (int mode : { (int) S::Loop, (int) S::PingPong })
                {
                    auto l = engine(sample, algo, 2.0f);
                    l->setParameter(S::Mode, (float) mode);
                    l->noteOn(60, 1.0f, -1);
                    auto looped = render(*l, 1.5);
                    expect(rms(looped, 1.3, 1.5) > 0.05f, name + " keeps looping");
                    expect(allFinite(looped, 2.0f));
                    l->noteOff(60, -1);
                    auto tail = render(*l, 0.3);
                    expect(rms(tail, 0.2, 0.3) < 1.0e-6f, name + " released");
                }

                auto gate = engine(sample, algo, 0.25f);
                gate->setParameter(S::Mode, (float) S::Gated);
                gate->setParameter(S::Release, 0.01f);
                gate->noteOn(60, 1.0f, -1);
                render(*gate, 0.3);
                gate->noteOff(60, -1);
                expectWithinAbsoluteError(soundEnd(render(*gate, 0.3)), 0.01, 0.003, name + " GATE release");
            }

            // The filter and the start point act on a stretched note as on a plain one.
            auto bright = engine(sample, S::StretchCycle, 0.5f);
            bright->setParameter(S::StartPoint, 0.5f);
            bright->noteOn(60, 1.0f, -1);
            auto open = render(*bright, 0.6);
            expectWithinAbsoluteError(soundEnd(open), 0.5, 0.005, "STRT 50 %: half the sample, twice as long");
            expectWithinAbsoluteError(peakHz(open, 0.0, 0.1, 100.0, 1800.0), 960.0, 80.0);
            bright->setParameter(S::FilterCutoff, 0.3f);
            bright->noteOn(60, 1.0f, -1);
            auto dark = render(*bright, 0.6);
            expect(rms(dark, 0.1, 0.4) < 0.3f * rms(open, 0.1, 0.4), "the voice filter");

            // Eight stretched notes at once.
            auto poly = engine(tone(440.0, 0.5), S::StretchWsol, 0.5f);
            for (int n = 0; n < 8; ++n) poly->noteOn(48 + 3 * n, 1.0f, -1);
            auto chord = render(*poly, 1.2);
            expectWithinAbsoluteError(soundEnd(chord), 1.0, 0.01, "every key lasts as long");
            expect(allFinite(chord, 8.0f) && rms(chord, 0.2, 0.8) > 0.3f);
        }

        beginTest("LFO destination SPD moves the stretch speed, not the pitch");
        {
            auto sample = tone(440.0, 0.5);
            auto timed = [&](float depth, std::vector<float>* keep = nullptr)
            {
                auto e = engine(sample, S::StretchCycle, 1.0f);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Shape), (float) LFO::Shape::Square);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Trigger), (float) LFO::TriggerMode::KeySync);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 0.02f);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) S::LfoSpeed);
                e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), depth);
                e->noteOn(60, 1.0f, -1);
                auto out = render(*e, 2.5);
                if (keep != nullptr) *keep = out;
                return soundEnd(out);
            };
            std::vector<float> fast;
            const double none = timed(0.0f), up = timed(0.5f, &fast), down = timed(-0.5f);
            expectWithinAbsoluteError(none, 0.5, 0.005);
            // +-0.5 of +-2 octaves: twice or half the speed (the square's first half decides which).
            expectWithinAbsoluteError(std::min(up, down), 0.25, 0.006);
            expectWithinAbsoluteError(std::max(up, down), 1.0, 0.01);
            expectWithinAbsoluteError(peakHz(fast, 0.05, 0.15), 440.0, 8.0);
            expectWithinAbsoluteError(std::min(timed(1.0f), timed(-1.0f)), 0.125, 0.006, "full depth: four times");

            // In GRAN mode it scales SCAN.
            auto g = engine(tone(440.0, 1.0));
            g->setParameter(S::Mode, (float) S::Granular);
            g->setParameter(S::GrainScan, 0.5f);
            g->noteOn(60, 1.0f, -1);
            render(*g, 0.4);
            const float plain = g->getPlayheadNormalized();
            g->allNotesOff();
            g->setParameter(EngineLfoSection::id(0, EngineLfoSection::Shape), (float) LFO::Shape::Square);
            g->setParameter(EngineLfoSection::id(0, EngineLfoSection::Trigger), (float) LFO::TriggerMode::KeySync);
            g->setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 0.02f);
            g->setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) S::LfoSpeed);
            g->setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.5f);
            g->noteOn(60, 1.0f, -1);
            render(*g, 0.4);
            const float moved = g->getPlayheadNormalized();
            expectWithinAbsoluteError(plain, 0.2f, 0.01f);
            expect(std::abs(moved - 0.4f) < 0.02f || std::abs(moved - 0.1f) < 0.01f, "scan x 2 or / 2: " + juce::String(moved));
        }

        beginTest("NORM brings a quiet sample to full level; the STRCH cells shape the sound they belong to");
        {
            auto quiet = tone(440.0, 0.5, 0.05f);
            auto e = engine(quiet);
            e->noteOn(60, 1.0f, -1);
            const float before = rms(render(*e, 0.4), 0.1, 0.3);
            e->setParameter(S::Normalize, 1.0f);
            e->noteOn(60, 1.0f, -1);
            const float after = rms(render(*e, 0.4), 0.1, 0.3);
            expectWithinAbsoluteError(after / before, 20.0f, 0.1f);

            auto sample = noisySweep(0.5);
            auto differs = [&](int algo, int param, float a, float b)
            {
                std::vector<float> out[2];
                for (int k = 0; k < 2; ++k)
                {
                    auto s = engine(sample, algo, algo == S::StretchSmear ? 4.0f : 0.5f);
                    if (algo == S::StretchSlice) s->setParameter(S::SliceCount, 8.0f);
                    s->setParameter(param, k == 0 ? a : b);
                    s->noteOn(60, 1.0f, -1);
                    out[k] = render(*s, 0.8);
                }
                double d = 0.0;
                for (size_t i = 0; i < out[0].size(); ++i) d += std::abs(out[0][i] - out[1][i]);
                return d / (double) out[0].size() > 1.0e-4;
            };
            expect(differs(S::StretchCycle, S::StretchSize, 0.02f, 0.1f), "CYCL SIZE");
            expect(differs(S::StretchCycle, S::StretchXfade, 0.0f, 0.8f), "CYCL XFAD");
            expect(differs(S::StretchCycle, S::StretchJitter, 0.0f, 0.8f), "CYCL JIT");
            expect(differs(S::StretchWsol, S::StretchSize, 0.02f, 0.1f), "WSOL SIZE");
            expect(differs(S::StretchWsol, S::StretchXfade, 0.2f, 1.0f), "WSOL XFAD");
            expect(differs(S::StretchGrain, S::StretchSize, 0.02f, 0.1f), "GRAN SIZE");
            expect(differs(S::StretchGrain, S::StretchXfade, 0.0f, 0.8f), "GRAN XFAD");
            expect(differs(S::StretchGrain, S::StretchJitter, 0.0f, 0.8f), "GRAN JIT");
            expect(differs(S::StretchSmear, S::StretchSize, 0.02f, 0.1f), "SMER SIZE");
            expect(differs(S::StretchSmear, S::StretchXfade, 0.0f, 0.8f), "SMER XFAD");
            expect(differs(S::StretchSmear, S::StretchJitter, 0.0f, 0.8f), "SMER JIT");

            // SPRD: grains leave the centre (left and right differ), only with SPRD up.
            for (int algo : { S::StretchGrain, S::StretchSmear })
                for (float spread : { 0.0f, 1.0f })
                {
                    auto s = engine(sample, algo, algo == S::StretchSmear ? 4.0f : 0.5f);
                    s->setParameter(S::StretchSpread, spread);
                    s->noteOn(60, 1.0f, -1);
                    std::vector<float> right;
                    auto left = render(*s, 0.8, &right);
                    double d = 0.0;
                    for (size_t i = 0; i < left.size(); ++i) d += std::abs(left[i] - right[i]);
                    expect((d / (double) left.size() > 1.0e-3) == (spread > 0.0f), juce::String(algoName(algo)) + " SPRD " + juce::String(spread));
                }

            // SMER at 16 x and 64 x: a steady, bounded pad at the sample's pitch, no silences. (Six
            // partials: a single sine fades in and out as its grains drift in and out of phase.)
            std::vector<float> chord((size_t) (0.5 * SR), 0.0f);
            {
                const double partials[6] = { 440.0, 557.0, 661.0, 883.0, 1109.0, 1327.0 };
                for (size_t i = 0; i < chord.size(); ++i)
                    for (int k = 0; k < 6; ++k)
                        chord[i] += (k == 0 ? 0.2f : 0.1f) * (float) std::sin(2.0 * PI * partials[k] * (double) i / SR);
            }
            for (float speed : { 1.0f, 0.25f })
            {
                auto s = engine(chord, S::StretchSmear, speed);
                s->noteOn(60, 1.0f, -1);
                auto out = render(*s, 6.0);
                expect(allFinite(out, 2.0f));
                float lo = 1.0e9f, hi = 0.0f;
                for (double t = 1.5; t < 5.5; t += 0.1)
                {
                    const float r = rms(out, t, t + 0.2);
                    lo = std::min(lo, r);
                    hi = std::max(hi, r);
                }
                expect(lo > 0.05f && hi < 0.6f && lo > 0.3f * hi, "SMER level " + juce::String(lo) + " .. " + juce::String(hi));
                expectWithinAbsoluteError(peakHz(out, 2.0, 2.0, 300.0, 500.0), 440.0, 4.0);
            }
        }

        beginTest("Extremes: every algorithm, mode and corner of the STRCH page stays finite and bounded");
        {
            auto mono = noisySweep(0.3);
            std::vector<float> stereo(mono.size() * 2);
            for (size_t i = 0; i < mono.size(); ++i) { stereo[i] = mono[i]; stereo[mono.size() + i] = -mono[mono.size() - 1 - i]; }
            int runs = 0;
            for (int algo = 1; algo < S::NumStretchAlgos; ++algo)
                for (int mode = 0; mode < 6; ++mode)
                    for (int corner = 0; corner < 4; ++corner)
                    {
                        auto e = std::make_unique<SamplerEngine>();
                        const double sr = corner == 3 ? 96000.0 : 44100.0;
                        e->prepareToPlay(sr, 64);
                        e->loadSampleStereoInterleaved(nullptr, 0, 48000.0);
                        if (corner % 2 == 0) e->loadSample(stereo.data(), (int) mono.size(), 2, 22050.0);
                        else                 e->loadSample(mono.data(), (int) mono.size(), 1, 48000.0);
                        e->setParameter(S::Mode, (float) mode);
                        e->setParameter(S::StretchMode, (float) algo);
                        e->setParameter(S::StretchSpeed, corner < 2 ? 0.25f : 4.0f);
                        e->setParameter(S::StretchSize, corner == 0 || corner == 3 ? 0.005f : 0.5f);
                        e->setParameter(S::StretchXfade, corner % 2 == 0 ? 0.0f : 1.0f);
                        e->setParameter(S::StretchJitter, 1.0f);
                        e->setParameter(S::StretchSpread, 1.0f);
                        e->setParameter(S::SliceMode, (float) (corner % 2));
                        e->setParameter(S::SliceCount, corner == 1 ? 1.0f : 16.0f);
                        e->setParameter(S::SlicePlay, (float) corner);
                        e->setParameter(S::SliceFade, corner == 2 ? 0.1f : 0.0f);
                        e->setParameter(S::SliceLen, corner == 3 ? 0.0f : 1.0f);
                        e->setParameter(S::StartPoint, corner == 1 ? 0.999f : 0.2f);
                        e->setParameter(S::EndPoint, corner == 2 ? 0.2f : 0.9f);
                        e->setParameter(S::StartRandom, 1.0f);
                        e->setParameter(S::Normalize, 1.0f);
                        e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) S::LfoSpeed);
                        e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 1.0f);
                        e->setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 30.0f);
                        e->setParameter(EngineLfoSection::id(1, EngineLfoSection::Dest), (float) S::LfoPitch);
                        e->setParameter(EngineLfoSection::id(1, EngineLfoSection::Depth), -1.0f);
                        if (corner == 1) { e->setParameter(S::StretchSync, 1.0f); e->setParameter(S::StretchLength, 9.0f); e->setTempo(20.0f); }
                        if (corner == 3) { e->setParameter(S::StretchSync, 1.0f); e->setParameter(S::StretchLength, 0.0f); e->setTempo(300.0f); }
                        juce::AudioBuffer<float> buf(2, 64);
                        juce::MidiBuffer midi;
                        bool ok = true;
                        for (int b = 0; b < 600; ++b)
                        {
                            if (b % 20 == 0) e->noteOn(24 + (b / 20 * 7) % 84, 1.0f, -1);
                            if (b % 50 == 49) e->noteOff(24 + (b / 20 * 7) % 84, -1);
                            if (b == 300) e->pitchBend(-1.0f);
                            buf.clear();
                            e->processBlock(buf, midi);
                            for (int ch = 0; ch < 2; ++ch)
                                for (int i = 0; i < 64; ++i)
                                {
                                    const float x = buf.getSample(ch, i);
                                    if (! std::isfinite(x) || std::abs(x) > 40.0f) ok = false;
                                }
                            float heads[80];
                            const int n = e->getPlayheads(heads, 80);
                            for (int i = 0; i < n; ++i)
                                if (! (heads[i] >= 0.0f && heads[i] <= 1.0f)) ok = false;
                            const int playing = e->getPlayingSlice();
                            if (playing < -1 || playing >= std::max(1, e->getSliceCount())) ok = false;
                        }
                        expect(ok, juce::String(algoName(algo)) + " mode " + juce::String(mode) + " corner " + juce::String(corner));
                        ++runs;
                    }
            expectEquals(runs, 6 * 6 * 4);
        }

        beginTest("A sample swap and re-slicing under stretched notes is safe");
        {
            auto big = noisySweep(1.0);
            auto hits = bursts(train, 2.0);
            std::vector<float> tiny(128, 0.5f);
            for (int algo = 1; algo < S::NumStretchAlgos; ++algo)
            {
                auto live = engine(big, algo, 0.5f);
                live->setParameter(S::SliceMode, (float) S::SliceTransient);
                live->setParameter(S::Mode, (float) S::Loop);
                std::atomic<bool> stop { false }, bad { false };
                std::thread audio([&]
                {
                    juce::AudioBuffer<float> buf(2, 64);
                    juce::MidiBuffer midi;
                    int block = 0;
                    while (! stop.load())
                    {
                        if (block++ % 6 == 0) live->noteOn(40 + block % 40, 1.0f, -1);
                        buf.clear();
                        live->processBlock(buf, midi);
                        for (int i = 0; i < 64; ++i)
                            if (! std::isfinite(buf.getSample(0, i)) || std::abs(buf.getSample(0, i)) > 40.0f)
                                bad.store(true);
                    }
                });
                for (int i = 0; i < 60; ++i)
                {
                    switch (i % 4)
                    {
                        case 0: live->loadSample(tiny.data(), 20 + i, 1, SR); break;
                        case 1: live->loadSample(hits.data(), (int) hits.size() - i * 300, 1, SR); break;
                        case 2: live->loadSample(big.data(), (int) big.size() - i * 200, 1, 44100.0); break;
                        default: live->setParameter(S::SampleSelect, (float) (i % 3)); break;
                    }
                    live->setParameter(S::SliceSens, (float) (i % 11) / 10.0f);
                    live->setParameter(S::SliceCount, (float) (1 + i % 20));
                    (void) live->getPlayingSlice();
                    (void) live->getSliceStartNormalized(i % 8);
                    std::this_thread::sleep_for(std::chrono::microseconds(400));
                }
                stop.store(true);
                audio.join();
                expect(! bad.load(), algoName(algo));

                // Still plays afterwards.
                live->loadSample(big.data(), (int) big.size(), 1, SR);
                live->setParameter(S::Mode, (float) S::Forward);
                live->allNotesOff();
                live->noteOn(60, 1.0f, -1);
                expect(rms(render(*live, 0.5), 0.2, 0.4) > 0.02f, juce::String(algoName(algo)) + " plays the new sample");
            }
        }
    }
};
static SamplerStretchTests SamplerStretchTestsInstance;
