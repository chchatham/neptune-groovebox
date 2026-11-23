#pragma once
#include <juce_core/juce_core.h>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>
#include "../src/dsp/GrainScheduler.h"
#include "../src/dsp/Resonator.h"
#include "../src/engine/VoiceBodySection.h"
#include "../src/engine/WaveSeqEngine.h"
#include "../src/engine/ResonatorEngine.h"
#include "../src/engine/SamplerEngine.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "fast_measure.h"

// -----------------------------------------------------------------------------
// The rest of phase U3 of .ralph/spec-unify.md:
//   the grain scheduler (dsp/GrainScheduler.h) by itself and against the sampler's own grain clock,
//   the resonator's grain source (dsp/Resonator.h, GRAN read from a recording) and RSN's GRAIN page,
//   BODY (engine/VoiceBodySection.h) by itself and on its proof engine, the wave sequencer.
// Levels are RMS; a level "at a frequency" is the root of a Goertzel power of a sound that no
// moving delay has been through. NEPTUNE_BODY_COST=1 times longer stretches (the "Cost" test).
// -----------------------------------------------------------------------------
class VoiceBodyTests : public juce::UnitTest
{
public:
    VoiceBodyTests() : UnitTest("Voice Body") {}

    using R = res::Resonator;
    using W = WaveSeqEngine;
    using E = ResonatorEngine;
    using B = VoiceBody;

    static constexpr double SR = 48000.0;

    // An expectation whose text (the numbers it was judged by) is written to the log either way.
    void check(bool ok, const juce::String& text = {})
    {
        if (text.isNotEmpty())
            logMessage((ok ? "   ok    " : "   FAIL  ") + text);
        expect(ok, text);
    }

    static float dB(double v) { return (float) (20.0 * std::log10(std::max(v, 1.0e-12))); }
    static double noteHz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }
    static double rms(const std::vector<float>& x, size_t from = 0, size_t n = (size_t) -1)
    {
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = from; i < x.size() && count < n; ++i, ++count)
            sum += (double) x[i] * x[i];
        return count > 0 ? std::sqrt(sum / (double) count) : 0.0;
    }
    static double peak(const std::vector<float>& x, size_t from = 0, size_t n = (size_t) -1)
    {
        double most = 0.0;
        size_t count = 0;
        for (size_t i = from; i < x.size() && count < n; ++i, ++count)
            most = std::max(most, (double) std::abs(x[i]));
        return most;
    }
    static bool finite(const std::vector<float>& x)
    {
        for (float v : x)
            if (! std::isfinite(v)) return false;
        return true;
    }
    static double largestStep(const std::vector<float>& x, size_t from = 0, size_t n = (size_t) -1)
    {
        double most = 0.0;
        size_t count = 0;
        for (size_t i = std::max<size_t>(from, 1); i < x.size() && count < n; ++i, ++count)
            most = std::max(most, (double) std::abs(x[i] - x[i - 1]));
        return most;
    }
    static double levelAt(const std::vector<float>& x, size_t from, size_t n, double hz, double sr = SR)
    {
        return std::sqrt(fastmeasure::goertzel(x, from, n, hz, sr));
    }
    // How unlike two renders are: sum |a - b| / sum (|a| + |b|).
    static double unlike(const std::vector<float>& a, const std::vector<float>& b)
    {
        double diff = 0.0, sum = 0.0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        {
            diff += std::abs((double) a[i] - b[i]);
            sum += std::abs((double) a[i]) + std::abs((double) b[i]);
        }
        return sum > 0.0 ? diff / sum : 0.0;
    }
    static std::vector<float> noiseSample(int frames, uint32_t seed = 777u, float level = 0.5f)
    {
        std::vector<float> s((size_t) frames);
        for (auto& x : s) x = level * dsp::xorshiftNoise(seed);
        return s;
    }
    static std::vector<float> sineSample(int frames, double hz, double rate = SR, float level = 0.5f)
    {
        std::vector<float> s((size_t) frames);
        for (int i = 0; i < frames; ++i)
            s[(size_t) i] = level * (float) std::sin(2.0 * 3.141592653589793 * hz * i / rate);
        return s;
    }
    static res::GrainSource sourceOf(const std::vector<float>& s, double rate = SR, float gain = 2.0f)
    {
        res::GrainSource g;
        g.data = s.data();
        g.frames = (int) s.size();
        g.rate = rate;
        g.gain = gain;
        return g;
    }

    // =========================================================================================
    // 1. The grain scheduler by itself
    // =========================================================================================
    void schedulerClock()
    {
        beginTest("grain scheduler: grains start at the density, to the sample while the jitter is 0; "
                  "a jitter scatters the onsets and keeps their number");
        const grn::Area area { 0.0, 48000.0 };
        auto one = [](double, float& l, float& r) { l = r = 1.0f; };
        for (float density : { 5.0f, 40.0f, 200.0f })
        {
            grn::Cloud<48> c;
            c.seed(1);
            c.reset();
            grn::Spec s;
            s.size = 0.001f;
            s.density = density;
            std::vector<int> at;
            for (int i = 0; i < 96000; ++i)
            {
                if (c.due(density, 1.0 / SR))
                {
                    c.start(s, 100.0, area, 1.0, SR);
                    at.push_back(i);
                }
                float l, r;
                c.sum(one, 1.0, area, l, r);
            }
            check(std::abs((int) at.size() - (int) (2.0f * density)) <= 1,
                   juce::String(density, 0) + " a second: " + juce::String((int) at.size()) + " grains in 2 s");
            expectEquals(at.empty() ? -1 : at[0], 0, "the first grain starts with the first sample");
            int widest = 0, narrowest = 1 << 30;
            for (size_t i = 1; i < at.size(); ++i)
            {
                widest = std::max(widest, at[i] - at[i - 1]);
                narrowest = std::min(narrowest, at[i] - at[i - 1]);
            }
            check(widest - narrowest <= 1, "regular: intervals of " + juce::String(narrowest) + " to " + juce::String(widest) + " samples");
        }
        {
            grn::Cloud<48> c;
            c.seed(3);
            c.reset();
            grn::Spec s;
            s.size = 0.001f;
            s.density = 40.0f;
            s.jitter = 1.0f;
            std::vector<int> at;
            const int seconds = 20;
            for (int i = 0; i < seconds * 48000; ++i)
            {
                if (c.due(40.0f, 1.0 / SR))
                {
                    c.start(s, 100.0, area, 1.0, SR);
                    at.push_back(i);
                }
                float l, r;
                c.sum(one, 1.0, area, l, r);
            }
            const double mean = 48000.0 / 40.0;
            int widest = 0, narrowest = 1 << 30;
            for (size_t i = 1; i < at.size(); ++i)
            {
                widest = std::max(widest, at[i] - at[i - 1]);
                narrowest = std::min(narrowest, at[i] - at[i - 1]);
            }
            check(std::abs((int) at.size() - seconds * 40) < seconds * 40 * 8 / 100,
                   "jitter 1: " + juce::String((int) at.size()) + " grains where " + juce::String(seconds * 40) + " are the mean");
            check(narrowest < 0.3 * mean && widest > 1.7 * mean,
                   "irregular: intervals of " + juce::String(narrowest) + " to " + juce::String(widest) + " samples round " + juce::String(mean, 0));
            // A density that is no number starts nothing and does not stop the clock for good.
            grn::Cloud<4> odd;
            odd.reset();
            odd.due(1.0f, 0.0);     // (the first is due at once: taken)
            check(! odd.due(std::numeric_limits<float>::quiet_NaN(), 1.0 / SR) && ! odd.due(-5.0f, 1.0 / SR));
            int started = 0;
            for (int i = 0; i < 48000; ++i)
                started += odd.due(10.0f, 1.0 / SR) ? 1 : 0;
            check(started >= 9 && started <= 11, "after it: " + juce::String(started) + " in a second at 10");
        }
    }

    void schedulerGrain()
    {
        beginTest("grain scheduler: a grain has its length, window, place, pitch, direction, level and pan");
        const grn::Area area { 0.0, 48000.0 };
        // Length and window: one grain of 10 ms read from a constant is its window.
        for (int window = 0; window < grn::NumWindows; ++window)
        {
            grn::Cloud<4> c;
            c.reset();
            grn::Spec s;
            s.size = 0.01f;
            s.density = 1.0f;
            s.window = window;
            c.start(s, 0.0, area, 1.0, SR);
            std::vector<float> out;
            while (c.sounding() > 0 && out.size() < 10000)
                out.push_back(c.sumMono([](double) { return 1.0f; }, 1.0, area));
            expectEquals((int) out.size(), 480, "10 ms at 48 kHz");
            size_t top = 0;
            for (size_t i = 0; i < out.size(); ++i)
                if (out[i] > out[top]) top = i;
            expectWithinAbsoluteError(out[top], 1.0f, 0.01f);
            const double where = (double) top / 480.0;
            if (window == grn::Hann || window == grn::Triangle)
                check(where > 0.48 && where < 0.52, "its top is in the middle");
            else if (window == grn::Perc)
                check(where < 0.05, "PERC: its top is at the start");
            else
                check(where > 0.95, "SWEL: its top is at the end");
            check(out.front() < 0.12f && out.back() < 0.12f, "it starts and ends at nothing");
        }
        // Place, pitch and direction.
        {
            grn::Cloud<4> c;
            c.reset();
            grn::Spec s;
            s.size = 0.01f;
            const auto* g = c.start(s, 1234.0, area, 2.0, SR);
            check(g != nullptr && g->pos == 1234.0 && g->ratio == 1.0f);
            c.sumMono([](double) { return 0.0f; }, 2.0, area);
            expectWithinAbsoluteError(c.grain(0).pos, 1236.0, 1.0e-9, "the step is the note's pitch");
            c.clear();
            s.reverse = 1.0f;
            g = c.start(s, 1234.0, area, 2.0, SR);
            check(g != nullptr && g->ratio == -1.0f, "reverse 1: every grain plays backwards");
            expectWithinAbsoluteError(g->pos, 1234.0 + 480.0 * 2.0, 1.0e-6, "... from the end of the stretch it covers");
            c.clear();
            s.reverse = 0.0f;
            s.backwards = true;
            g = c.start(s, 5000.0, area, 1.0, SR);
            check(g != nullptr && g->ratio == -1.0f && std::abs(g->pos - 5000.0) < 1.0e-6, "a note that runs backwards: the grain ends at the position");
        }
        // Pitch jitter and scatter: within their reach, and using it.
        {
            grn::Cloud<4> c;
            c.seed(11);
            grn::Spec s;
            s.size = 0.01f;
            s.pitchJitter = 1.0f;       // an octave
            s.scatter = 1.0f;
            s.scatterFrames = 4800.0f;
            float low = 10.0f, high = 0.0f;
            double first = 1.0e12, last = -1.0e12;
            for (int i = 0; i < 500; ++i)
            {
                c.clear();
                const auto* g = c.start(s, 24000.0, area, 1.0, SR);
                low = std::min(low, g->ratio);
                high = std::max(high, g->ratio);
                first = std::min(first, g->pos);
                last = std::max(last, g->pos);
            }
            check(low >= 0.5f && high <= 2.0f && low < 0.6f && high > 1.7f, "pitch " + juce::String(low, 2) + " to " + juce::String(high, 2));
            check(first >= 24000.0 - 4800.0 && last <= 24000.0 + 4800.0 && first < 24000.0 - 4000.0 && last > 24000.0 + 4000.0,
                   "place " + juce::String(first, 0) + " to " + juce::String(last, 0));
        }
        // Level: a sparse grain plays at full level; overlapping grains are scaled by the root of the overlap.
        {
            grn::Cloud<48> c;
            grn::Spec s;
            s.size = 0.1f;
            s.density = 5.0f;       // overlap 0.5
            expectWithinAbsoluteError(c.start(s, 0.0, area, 1.0, SR)->gainL, 1.0f, 1.0e-6f);
            s.density = 160.0f;     // overlap 16
            expectWithinAbsoluteError(c.start(s, 0.0, area, 1.0, SR)->gainL, 0.25f, 1.0e-6f);
            s.level = 0.5f;
            expectWithinAbsoluteError(c.start(s, 0.0, area, 1.0, SR)->gainL, 0.125f, 1.0e-6f);
            s.level = 1.0f;
            s.density = 1.0e6f;     // more than the pool: the pool
            expectWithinAbsoluteError(c.start(s, 0.0, area, 1.0, SR)->gainL, 1.0f / std::sqrt(48.0f), 1.0e-6f);
            // Pan: the centre is 1 on both sides; a spread keeps the power.
            s.density = 5.0f;
            s.spread = 1.0f;
            float leftMost = 0.0f, rightMost = 0.0f;
            for (int i = 0; i < 40; ++i)
            {
                c.clear();
                const auto* g = c.start(s, 0.0, area, 1.0, SR);
                expectWithinAbsoluteError(g->gainL * g->gainL + g->gainR * g->gainR, 2.0f, 1.0e-4f);
                leftMost = std::max(leftMost, g->gainL);
                rightMost = std::max(rightMost, g->gainR);
            }
            check(leftMost > 1.3f && rightMost > 1.3f, "spread 1 reaches both sides");
        }
        // A cloud of noise grains is as loud whatever its size and density.
        {
            const auto noise = noiseSample(48000);
            auto cloudRms = [&](float size, float density)
            {
                grn::Cloud<48> c;
                c.seed(5);
                c.reset();
                grn::Spec s;
                s.size = size;
                s.density = density;
                s.jitter = 1.0f;
                s.scatter = 1.0f;
                s.scatterFrames = 12000.0f;
                std::vector<float> out(96000);
                for (auto& v : out)
                {
                    if (c.due(density, 1.0 / SR))
                        c.start(s, 24000.0, area, 1.0, SR);
                    v = c.sumMono([&](double at) { return grn::readLinear(noise.data(), 48000, at); }, 1.0, area);
                }
                return rms(out);
            };
            const double four = cloudRms(0.05f, 80.0f), sixteen = cloudRms(0.05f, 320.0f), longer = cloudRms(0.2f, 40.0f);
            check(std::abs(dB(sixteen / four)) < 1.5f && std::abs(dB(longer / four)) < 1.5f,
                   "overlap 4: " + juce::String(dB(four), 1) + " dB, 16: " + juce::String(dB(sixteen), 1) + " dB, 8 of longer grains: " + juce::String(dB(longer), 1) + " dB");
        }
    }

    void schedulerPool()
    {
        beginTest("grain scheduler: a full pool skips the grain and changes nothing for those that follow; the same seed is the "
                  "same cloud; grains stay inside their area whatever they are given");
        const grn::Area area { 0.0, 48000.0 };
        grn::Spec s;
        s.size = 0.5f;
        s.scatter = 1.0f;
        s.scatterFrames = 9000.0f;
        s.pitchJitter = 0.5f;
        s.jitter = 0.5f;
        grn::Cloud<4> small;
        grn::Cloud<48> large;
        small.seed(99);
        large.seed(99);
        int made = 0;
        for (int i = 0; i < 6; ++i)
        {
            made += small.start(s, 20000.0, area, 1.0, SR) != nullptr ? 1 : 0;
            large.start(s, 20000.0, area, 1.0, SR);
        }
        expectEquals(made, 4, "a pool of four takes four of six");
        expectEquals(small.sounding(), 4);
        small.clear();
        large.clear();
        const auto* a = small.start(s, 20000.0, area, 1.0, SR);
        const auto* b = large.start(s, 20000.0, area, 1.0, SR);
        check(a != nullptr && b != nullptr && a->pos == b->pos && a->ratio == b->ratio, "the seventh grain is the same in both");

        // Nothing leaves the area: tiny areas, long grains, steps of any size and of no size.
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        double lowest = 1.0e300, highest = -1.0e300;
        bool allFinite = true;
        for (double step : { 0.0, 1.0, -3.0, 700.0, 1.0e9, (double) nan, (double) inf })
            for (double length : { 1.0, 10.0, 5000.0 })
                for (float size : { 0.0f, 0.001f, 0.5f, 1.0e30f, nan })
                {
                    grn::Cloud<8> c;
                    c.seed(4);
                    c.reset();
                    const grn::Area tiny { 100.0, 100.0 + length };
                    grn::Spec odd = s;
                    odd.size = size;
                    odd.reverse = 0.5f;
                    for (int i = 0; i < 400; ++i)
                    {
                        if (i % 7 == 0)
                            c.start(odd, i % 3 == 0 ? (double) nan : 100.0 + (double) (i % 50) * 400.0 - 3000.0, tiny, step, SR);
                        float l = 0.0f, r = 0.0f;
                        c.sum([&](double at, float& sl, float& sr)
                              {
                                  lowest = std::min(lowest, at - 100.0);
                                  highest = std::max(highest, at - (100.0 + length));
                                  allFinite = allFinite && std::isfinite(at);
                                  sl = sr = 1.0f;
                              }, step, tiny, l, r);
                        allFinite = allFinite && std::isfinite(l) && std::isfinite(r);
                    }
                }
        check(allFinite && lowest >= 0.0 && highest <= 0.0,
               "asked for places from " + juce::String(lowest) + " past the start to " + juce::String(highest) + " past the end");
        // readLinear holds any place to its buffer.
        const float four[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        expectEquals(grn::readLinear(four, 4, -5.0), 1.0f);
        expectEquals(grn::readLinear(four, 4, 1.0e30), 4.0f);
        expectEquals(grn::readLinear(four, 4, (double) nan), 1.0f);
        expectWithinAbsoluteError(grn::readLinear(four, 4, 1.5), 2.5f, 1.0e-6f);
    }

    // The scheduler runs the sampler's clock: the same seed, size, density and spray give the same
    // number of grains sounding at every block's end as the sampler's own granular mode. (The
    // sampler is on it since phase U4: its grains' random numbers are the draws of the note's
    // spread::Sequence, in the scheduler's order, and are handed over as grn::Cloud::Draws.)
    void schedulerAgainstSampler()
    {
        beginTest("grain scheduler: with the sampler's seed and cells it has as many grains sounding, block by block, as the "
                  "sampler's granular mode");
        const auto noise = noiseSample(96000);
        for (float spray : { 0.0f, 0.5f, 1.0f })
        {
            SamplerEngine e;
            e.prepareToPlay(SR, 64);
            e.loadSample(noise.data(), (int) noise.size(), 1, SR);
            e.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Granular);
            e.setParameter(SamplerEngine::GrainSize, 0.05f);
            e.setParameter(SamplerEngine::GrainDensity, 60.0f);
            e.setParameter(SamplerEngine::GrainSpray, spray);
            e.setGrainSeed(7u);
            e.noteOn(60, 1.0f, 0);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;

            grn::Cloud<SamplerEngine::MAX_GRAINS> c;
            c.reset();
            // The sampler's first voice, its first note (setGrainSeed, then the note-on).
            spread::Generator generator;
            generator.seed(7u, 0);
            generator.advance();
            spread::Sequence draws;
            draws.start(generator, spread::Grain);
            grn::Spec s;
            s.size = 0.05f;
            s.density = 60.0f;
            s.jitter = s.scatter = spray;
            s.scatterFrames = SamplerEngine::GRAIN_SPRAY_SECONDS * (float) SR;
            const grn::Area area { 0.0, 96000.0 };

            int differ = 0, most = 0;
            for (int block = 0; block < 400; ++block)
            {
                buf.clear();
                e.processBlock(buf, midi);
                for (int i = 0; i < 64; ++i)
                {
                    if (c.due(60.0f, 1.0 / SR))
                    {
                        grn::Cloud<SamplerEngine::MAX_GRAINS>::Draws d;
                        d.pos = draws.bipolar();
                        d.pitch = draws.bipolar();
                        d.pan = draws.bipolar();
                        d.reverse = draws.unipolar();
                        if (s.jitter > 0.0f)
                            d.interval = draws.bipolar();
                        c.start(s, 1000.0, area, 1.0, SR, d);
                    }
                    c.sumMono([](double) { return 0.0f; }, 1.0, area);
                }
                differ += c.sounding() != e.getActiveGrainCount() ? 1 : 0;
                most = std::max(most, c.sounding());
            }
            expectEquals(differ, 0, "spray " + juce::String(spray, 1) + ": blocks of 400 where the counts differ (up to "
                                    + juce::String(most) + " grains at once)");
            check(most >= 3);
        }
    }

    // =========================================================================================
    // 2. The resonator's grain source
    // =========================================================================================
    struct Scrape
    {
        R r;
        double sr;
        explicit Scrape(double rate = SR, float gran = 0.6f, double hz = 220.0, float decay = 0.6f) : sr(rate)
        {
            r.prepare(rate);
            r.setBody((float) res::Comb);
            r.setExciter(res::ExciteLatent { { 1.0f, gran, 0.0f, 0.5f, 1.0f } });
            r.setPitch(hz);
            r.setDecay(decay);
            r.seedNoise(4711u);
            r.strike(0.5f);
        }
        std::vector<float> run(double seconds, float energy = 0.5f)
        {
            std::vector<float> out((size_t) std::lround(seconds * sr));
            for (auto& v : out)
                v = r.tick(0.0f, energy);
            return out;
        }
    };

    void resonatorGrains()
    {
        beginTest("resonator grains: GRAN's impulses read from a recording drive the body with what the recording holds, "
                  "where it holds it, as often as GRAN says");
        // A recording that is one tone on the body's fifth partial: the body answers there; with
        // noise impulses it answers on every partial.
        const auto tone = sineSample(48000, 1100.0);
        const auto toneSource = sourceOf(tone);
        // A grain's length is what setGrains says: ten times as long, ten times as many sound at once.
        {
            auto soundingAt = [&](float size)
            {
                Scrape s;
                s.r.setGrainSource(&toneSource);
                res::GrainSet g;
                g.size = size;
                s.r.setGrains(g);
                s.r.reset();
                s.r.strike(0.5f);
                double sum = 0.0;
                for (int i = 0; i < 96000; ++i)
                {
                    s.r.tick(0.0f, 0.5f);
                    sum += (double) s.r.grainsSounding();
                }
                return sum / 96000.0;
            };
            const double brief = soundingAt(0.008f), longer = soundingAt(0.08f);
            check(longer > 7.0 * brief && longer < 13.0 * brief, "grains sounding at once: " + juce::String(brief, 2) + " at 8 ms, " + juce::String(longer, 2) + " at 80 ms");
        }
        double withNoise, withTone;
        {
            Scrape s;
            const auto out = s.run(2.0);
            withNoise = levelAt(out, 48000, 48000, 1100.0) / std::max(levelAt(out, 48000, 48000, 440.0), 1.0e-9);
            check(! s.r.readsGrains() && s.r.grainsSounding() == 0);
        }
        {
            Scrape s;
            s.r.setGrainSource(&toneSource);
            s.r.strike(0.5f);
            s.r.reset();
            s.r.strike(0.5f);
            const auto out = s.run(2.0);
            withTone = levelAt(out, 48000, 48000, 1100.0) / std::max(levelAt(out, 48000, 48000, 440.0), 1.0e-9);
            check(s.r.readsGrains() && finite(out) && rms(out, 48000) > 1.0e-3,
                   "it sounds: " + juce::String(dB(rms(out, 48000)), 1) + " dB");
        }
        check(withTone > 10.0 * withNoise, "partial 5 against partial 2: " + juce::String(dB(withTone), 1) + " dB with the tone's grains, "
                                            + juce::String(dB(withNoise), 1) + " dB with noise impulses");

        // Where: a recording that is silent in its first half. Grains from there are silence;
        // from the second half they sound; a place that moves on arrives there.
        std::vector<float> half = noiseSample(48000);
        std::fill(half.begin(), half.begin() + 24000, 0.0f);
        const auto halfSource = sourceOf(half);
        auto at = [&](float position, float scan, double seconds)
        {
            Scrape s;
            s.r.setGrainSource(&halfSource);
            res::GrainSet g;
            g.position = position;
            g.scan = scan;
            s.r.setGrains(g);
            s.r.reset();
            s.r.seedNoise(99u);
            s.r.strike(0.5f);
            return s.run(seconds);
        };
        const auto silentPart = at(0.1f, 0.0f, 1.0), soundPart = at(0.75f, 0.0f, 1.0), moving = at(0.0f, 1.0f, 1.0);
        check(rms(silentPart) < 1.0e-6 && rms(soundPart) > 1.0e-3,
               "GPOS in the silent half: " + juce::String(dB(rms(silentPart)), 1) + " dB; in the other: " + juce::String(dB(rms(soundPart)), 1) + " dB");
        check(rms(moving, 0, 21000) < 1.0e-6 && rms(moving, 30000, 18000) > 1.0e-3,
               "SCAN 1: silent for the first half second (" + juce::String(dB(rms(moving, 0, 21000)), 1) + " dB), then "
               + juce::String(dB(rms(moving, 30000, 18000)), 1) + " dB");

        // How many: 2 a second rising to 400 with GRAN; none at 0.
        const auto noise = noiseSample(48000);
        const auto noiseSource = sourceOf(noise);
        for (float gran : { 0.0f, 0.25f, 0.5f, 1.0f })
        {
            Scrape s(SR, gran);
            s.r.setGrainSource(&noiseSource);
            s.r.reset();
            s.r.strike(0.5f);
            double sounding = 0.0;
            const int n = 96000;
            for (int i = 0; i < n; ++i)
            {
                s.r.tick(0.0f, 0.5f);
                sounding += s.r.grainsSounding();
            }
            const double wanted = gran <= 0.0f ? 0.0 : 2.0 * std::pow(200.0, (double) gran);
            expectWithinAbsoluteError((double) s.r.grainDensity(), wanted, 0.01 * wanted + 1.0e-6);
            const double overlap = std::min(wanted * 0.03, (double) R::GRAIN_POOL);
            check(std::abs(sounding / n - overlap) <= 0.25 * overlap + 1.0e-9,
                   "GRAN " + juce::String(gran, 2) + ": " + juce::String(wanted, 1) + " grains a second, " + juce::String(sounding / n, 2)
                   + " sounding on average (" + juce::String(overlap, 2) + " by size x density)");
        }

        // Its level is that of the noise it stands in for, within a few decibels (a recording brought to a level of 1).
        {
            const auto unit = sourceOf(noise, SR, 1.0f / 0.2887f);       // (uniform noise of +-0.5 has an RMS of 0.2887)
            for (float gran : { 0.3f, 0.6f, 1.0f })
            {
                Scrape a(SR, gran), b(SR, gran);
                b.r.setGrainSource(&unit);
                b.r.reset();
                b.r.strike(0.5f);
                const double asNoise = rms(a.run(3.0), 48000), asGrains = rms(b.run(3.0), 48000);
                check(std::abs(dB(asGrains / asNoise)) < 7.0f, "GRAN " + juce::String(gran, 1) + ": noise impulses " + juce::String(dB(asNoise), 1)
                                                               + " dB, grains of noise " + juce::String(dB(asGrains), 1) + " dB");
            }
        }
    }

    void resonatorGrainsSafe()
    {
        beginTest("resonator grains: another recording under sound waits for the grains to end (no jump); any recording, cell and "
                  "rate gives a finite, bounded sound; without a recording nothing of it is run");
        // Two tones on two partials: after the change the body answers on the second.
        const auto first = sineSample(48000, 1100.0), second = sineSample(48000, 660.0);
        const auto firstSource = sourceOf(first), secondSource = sourceOf(second);
        {
            Scrape s(SR, 0.8f, 220.0, 0.25f);
            s.r.setGrainSource(&firstSource);
            s.r.reset();
            s.r.strike(0.5f);
            auto out = s.run(1.0);
            s.r.setGrainSource(&secondSource);
            const auto after = s.run(1.5);
            out.insert(out.end(), after.begin(), after.end());
            const double before5 = levelAt(out, 24000, 24000, 1100.0), before3 = levelAt(out, 24000, 24000, 660.0);
            const double after5 = levelAt(out, 96000, 24000, 1100.0), after3 = levelAt(out, 96000, 24000, 660.0);
            check(before5 > 5.0 * before3 && after3 > 5.0 * after5,
                   "partial 5 / 3 before: " + juce::String(dB(before5 / before3), 1) + " dB, after: " + juce::String(dB(after5 / after3), 1) + " dB");
            const double steady = largestStep(out, 24000, 24000), atChange = largestStep(out, 48000, 9600);
            check(atChange < 1.5 * steady, "largest step at the change " + juce::String(atChange, 4) + ", before it " + juce::String(steady, 4));
        }
        // The change itself: the grains that sound are read from the first recording to their end, none
        // starts meanwhile, and the second is taken when the last has ended.
        {
            Scrape s(SR, 1.0f);
            s.r.setGrainSource(&firstSource);
            s.r.reset();
            s.r.strike(0.5f);
            s.run(0.5);
            const int sounding = s.r.grainsSounding();
            s.r.setGrainSource(&secondSource);
            bool keptFirst = true, noneStarted = true;
            int waited = 0, before = sounding;
            while (s.r.grainSource() != &secondSource && waited < 48000)
            {
                keptFirst = keptFirst && s.r.grainSource() == &firstSource;
                s.r.tick(0.0f, 0.5f);
                noneStarted = noneStarted && s.r.grainsSounding() <= before;
                before = s.r.grainsSounding();
                keptFirst = keptFirst && (s.r.grainSource() == &firstSource || s.r.grainsSounding() == 0);
                ++waited;
            }
            check(sounding >= 6 && keptFirst && noneStarted && waited > 480 && waited < 2400,
                  juce::String(sounding) + " grains sounded at the change; the second recording was taken " + juce::String(waited) + " samples later, when none did");
            s.run(0.1);
            check(s.r.grainSource() == &secondSource && s.r.grainsSounding() >= 6, "and its grains sound");
        }
        // Anything for a recording, every cell at both ends, three rates.
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        std::vector<float> wild(4000);
        for (size_t i = 0; i < wild.size(); ++i)
            wild[i] = i % 97 == 0 ? nan : (i % 89 == 0 ? inf : (i % 83 == 0 ? -1.0e30f : (i % 2 == 0 ? 1.0f : -1.0f)));
        const auto wildSource = sourceOf(wild, 8000.0, 32.0f);
        const auto shortest = sourceOf(first, 192000.0);
        res::GrainSource two = sourceOf(first);
        two.frames = 2;
        double most = 0.0;
        bool allFinite = true;
        for (double rate : { 22050.0, 48000.0, 192000.0 })
            for (const res::GrainSource* source : { &wildSource, &shortest, (const res::GrainSource*) &two })
                for (int end = 0; end < 2; ++end)
                    for (float gran : { 0.2f, 1.0f })
                    {
                        Scrape s(rate, gran, end == 0 ? 20.0 : 12000.0, end == 0 ? 0.05f : 20.0f);
                        s.r.setGrainSource(source);
                        res::GrainSet g;
                        g.position = end == 0 ? 0.0f : 1.0f;
                        g.scan = end == 0 ? -2.0f : 2.0f;
                        g.scatter = (float) end;
                        g.size = end == 0 ? 0.0f : 10.0f;
                        g.pitch = end == 0 ? -24.0f : 24.0f;
                        g.pitchJitter = (float) end;
                        g.window = end == 0 ? -3 : 99;
                        s.r.setGrains(g);
                        s.r.reset();
                        s.r.strike(2.0f);
                        const auto out = s.run(0.4, 2.0f);
                        allFinite = allFinite && finite(out);
                        most = std::max(most, peak(out));
                        // Cells that are no numbers are taken as their rest values.
                        g.position = g.scan = g.scatter = g.size = g.pitch = g.pitchJitter = nan;
                        s.r.setGrains(g);
                        const auto more = s.run(0.1, 2.0f);
                        allFinite = allFinite && finite(more);
                        most = std::max(most, peak(more));
                    }
        check(allFinite && most <= R::OUT_LIMIT, "peak " + juce::String(most, 3));
        // A recording given between two notes is read from the next note on (and one taken away is not).
        {
            Scrape s(SR, 1.0f);
            s.run(0.2);
            s.r.setGrainSource(&firstSource);
            s.r.reset();
            s.r.strike(0.5f);
            s.run(0.2);
            check(s.r.readsGrains() && s.r.grainsSounding() >= 6, "given between two notes: " + juce::String(s.r.grainsSounding()) + " grains sound in the second");
            s.r.setGrainSource(nullptr);
            s.r.reset();
            s.r.strike(0.5f);
            const auto noise = s.run(0.2);
            check(! s.r.readsGrains() && s.r.grainsSounding() == 0 && rms(noise) > 1.0e-3, "taken away: noise impulses again");
        }
        // A recording that is none is no recording.
        {
            Scrape s;
            res::GrainSource none;
            s.r.setGrainSource(&none);
            s.r.reset();
            check(! s.r.readsGrains());
            two.frames = 1;
            s.r.setGrainSource(&two);
            s.r.reset();
            check(! s.r.readsGrains());
        }
        // The same seed, the same scrape.
        {
            Scrape a, b;
            a.r.setGrainSource(&firstSource);
            b.r.setGrainSource(&firstSource);
            a.r.reset(); a.r.seedNoise(5u); a.r.strike(0.5f);
            b.r.reset(); b.r.seedNoise(5u); b.r.strike(0.5f);
            check(a.run(0.5) == b.run(0.5));
        }
    }

    // =========================================================================================
    // 3. RSN's GRAIN page
    // =========================================================================================
    struct Rsn
    {
        E engine;
        juce::AudioBuffer<float> buf;
        juce::MidiBuffer midi;
        int block;
        explicit Rsn(double sr = SR, int blockSize = 64) : buf(2, blockSize), block(blockSize)
        {
            engine.prepareToPlay(sr, blockSize);
        }
        std::vector<float> render(double seconds, double sr = SR)
        {
            std::vector<float> out;
            const int blocks = (int) std::lround(seconds * sr / block);
            out.reserve((size_t) blocks * (size_t) block);
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                engine.processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back(buf.getSample(0, i));
            }
            return out;
        }
    };

    static int pageIndex(const ParamPages& pages, const char* name)
    {
        for (size_t i = 0; i < pages.size(); ++i)
            if (pages[i].name == name)
                return (int) i;
        return -1;
    }

    void rsnGrainPage()
    {
        beginTest("RSN: page GRAIN (SMPL GPOS SCAN SPRY | SIZE GPIT PJIT SHAP) chooses a sample of the bank for the exciter's "
                  "grains; OFF is the engine as it was; GPOS is a destination of the LFOs and the mod slots");
        auto& bank = SampleBank::get();
        {
            E e;
            const auto pages = e.getParamPages();
            const int grain = pageIndex(pages, "GRAIN");
            expectEquals(grain, 4, "after EXC, before AMP");        // (3 and 4 until the body's second eight got a page, BOD2: phase U2b)
            expectEquals(pageIndex(pages, "AMP"), 5);
            static const char* const labels[8] = { "SMPL", "GPOS", "SCAN", "SPRY", "SIZE", "GPIT", "PJIT", "SHAP" };
            if (grain >= 0)
            {
                for (int i = 0; i < 8; ++i)
                {
                    const auto& cell = pages[(size_t) grain].params[(size_t) i];
                    check(cell.isValid() && std::strcmp(cell.label, labels[i]) == 0 && cell.id == E::GrainSample + i, labels[i]);
                    expectEquals(e.getParameter(cell.id), cell.defaultVal, juce::String(labels[i]) + " reads its default");
                }
                const auto& smpl = pages[(size_t) grain].params[0];
                expectEquals(smpl.numChoices, bank.size() + 1);
                check(std::strcmp(smpl.choices[0], "OFF") == 0 && std::strcmp(smpl.choices[1], bank.names()[0]) == 0);
            }
            check(e.isRebuildParam(E::GrainSample) && ! e.isRebuildParam(E::GrainPosParam), "SMPL takes no step lock, GPOS does");
            // SMPL is held to the bank.
            e.setParameter(E::GrainSample, 1.0e6f);
            expectEquals(e.getParameter(E::GrainSample), (float) bank.size());
            e.setParameter(E::GrainSample, -4.0f);
            expectEquals(e.getParameter(E::GrainSample), 0.0f);
            // The destinations.
            const auto& dests = E::lfoDests();
            for (const char* label : { "GPOS", "SCAN", "SPRY", "GSIZ", "GPIT", "PJIT" })
                check(dests.indexOfLabel(label) > 0, juce::String(label) + " is a destination");
            expectEquals(dests[dests.indexOfLabel("GPOS")].id, (int) E::GrainPosParam);      // ([U4-RSN] a router destination: .id, was .key)
        }

        // A scrape (XMPH at SCRP) of a string with a drum break, against the same with noise.
        const int breakIndex = bank.breakIndex(0);
        check(breakIndex >= 0, "the bank has a break");
        // What the exciter is given of a bank sample: the sample where it lies, and the gain that
        // brings it to GRAIN_LEVEL whatever level it was recorded at.
        for (int index : { bank.defaultIndex(), breakIndex, 0 })
            if (const auto* entry = bank.entry(index))
            {
                const auto* source = E::grainSource(index);
                if (source == nullptr)
                {
                    expect(false, "sample " + juce::String(index) + " has a grain source");
                    continue;
                }
                double sum = 0.0;
                for (float x : entry->left)
                    sum += (double) x * (double) x;
                const double level = std::sqrt(sum / (double) entry->left.size()) * (double) source->gain;
                check(source->data == entry->left.data() && source->frames == (int) entry->left.size() && source->rate == entry->sampleRate,
                      juce::String(entry->name) + ": read in place");
                check(std::abs(level - E::GRAIN_LEVEL) < 0.01 || source->gain == 32.0f,
                      juce::String(entry->name) + ": brought to " + juce::String(level, 3) + " (gain " + juce::String(source->gain, 2) + ")");
                check(E::grainSource(index) == source, "the same source every time it is asked for");
            }
        check(E::grainSource(-1) == nullptr && E::grainSource(bank.size()) == nullptr && E::grainSource(1 << 20) == nullptr, "no sample, no source");
        auto scrape = [&](float sample, const std::function<void(E&)>& more = {})
        {
            Rsn rig;
            rig.engine.setParameter(E::ExciterMorphParam, 3.0f);
            rig.engine.setParameter(E::GrainSample, sample);
            if (more) more(rig.engine);
            rig.engine.noteOn(57, 0.9f, 0);
            return rig.render(2.0);
        };
        const auto off = scrape(0.0f), offAgain = scrape(0.0f), withBreak = scrape((float) (breakIndex + 1));
        check(off == offAgain, "(a render repeats)");
        check(finite(withBreak) && rms(withBreak, 24000) > 1.0e-3 && unlike(off, withBreak) > 0.5,
               "noise impulses " + juce::String(dB(rms(off, 24000)), 1) + " dB, grains of the break " + juce::String(dB(rms(withBreak, 24000)), 1)
               + " dB, unlike by " + juce::String(unlike(off, withBreak), 2));
        check(std::abs(dB(rms(withBreak, 24000) / rms(off, 24000))) < 12.0f, "within 12 dB of each other");

        // Every cell of the page moves the sound of a scrape with the break.
        const float sample = (float) (breakIndex + 1);
        const auto base = scrape(sample);
        struct Move { int id; float value; const char* name; };
        for (const Move& m : { Move { E::GrainPosParam, 0.6f, "GPOS" }, Move { E::GrainScanParam, -1.0f, "SCAN" }, Move { E::GrainSprayParam, 1.0f, "SPRY" },
                               Move { E::GrainSizeParam, 0.2f, "SIZE" }, Move { E::GrainPitchParam, 12.0f, "GPIT" }, Move { E::GrainJitterParam, 1.0f, "PJIT" },
                               Move { E::GrainShapeParam, 2.0f, "SHAP" } })
        {
            const auto moved = scrape(sample, [&](E& e) { e.setParameter(m.id, m.value); });
            check(finite(moved) && unlike(base, moved) > 0.05, juce::String(m.name) + ": unlike by " + juce::String(unlike(base, moved), 3));
        }

        // GPOS moved within a note: by LFO 1, and by a mod slot from LFO 2.
        const int gpos = E::lfoDests().indexOfLabel("GPOS");
        auto lfoOn = [&](E& e, float depth)
        {
            e.setParameter(E::GrainScanParam, 0.0f);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 3.0f);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) gpos);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), depth);
        };
        const auto still = scrape(sample, [&](E& e) { lfoOn(e, 0.0f); });
        const auto byLfo = scrape(sample, [&](E& e) { lfoOn(e, 1.0f); });
        check(finite(byLfo) && unlike(still, byLfo) > 0.2, "LFO 1 on GPOS: unlike by " + juce::String(unlike(still, byLfo), 3));
        const auto bySlot = scrape(sample, [&](E& e)
        {
            e.setParameter(E::GrainScanParam, 0.0f);
            e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Rate), 3.0f);
            e.setParameter(ModSlotParam::id(0, ModSlotParam::SRC), (float) ModSrc::Lfo2);
            e.setParameter(ModSlotParam::id(0, ModSlotParam::DEST), (float) gpos);
            e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 1.0f);
        });
        check(finite(bySlot) && unlike(still, bySlot) > 0.2, "a slot from LFO 2 on GPOS: unlike by " + juce::String(unlike(still, bySlot), 3));

        // SMPL changed under a held note: finite, bounded, and nothing jumps.
        {
            Rsn rig;
            rig.engine.setParameter(E::ExciterMorphParam, 3.0f);
            rig.engine.setParameter(E::GrainSample, sample);
            rig.engine.noteOn(57, 0.9f, 0);
            auto out = rig.render(1.0);
            const double steady = largestStep(out, 24000);
            double atChange = 0.0;
            for (int n = 0; n < 6; ++n)
            {
                rig.engine.setParameter(E::GrainSample, n % 2 == 0 ? 1.0f : sample);
                const auto next = rig.render(0.25);
                atChange = std::max(atChange, largestStep(next));
                check(finite(next));
            }
            check(atChange < 2.0 * steady, "largest step at a change " + juce::String(atChange, 4) + ", before " + juce::String(steady, 4));
        }
    }

    // =========================================================================================
    // 4. BODY by itself
    // =========================================================================================
    struct Body
    {
        B body;
        double sr;
        explicit Body(double rate = SR, int note = 57) : sr(rate)
        {
            body.setSampleRate(rate);
            body.set(B::Mix, 1.0f);
            body.noteOn(note);
        }
        // Noise for `drive` seconds, then nothing; the body's output over the whole of `seconds`.
        std::vector<float> noiseThenRing(double drive, double seconds, float level = 0.2f, uint32_t seed = 31u)
        {
            std::vector<float> out((size_t) std::lround(seconds * sr));
            const size_t stop = (size_t) std::lround(drive * sr);
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (! body.engaged())
                    break;
                out[i] = body.process(i < stop ? level * dsp::xorshiftNoise(seed) : 0.0f, i < stop);
            }
            return out;
        }
    };

    void bodyPartials()
    {
        beginTest("BODY: a voice's sound passed through it rings at the body's partials on the voice's note; TUNE moves it, "
                  "KTRK lets it follow the keys or stay; DCAY is how long");
        // The plate on A3: what rings after the noise has stopped is at its partials, not between them.
        for (int note : { 45, 57, 69 })
        {
            Body b(SR, note);
            b.body.set(B::Morph, (float) res::Plate);
            const auto out = b.noiseThenRing(0.3, 1.0);
            const double hz = noteHz(note);
            auto& r = b.body.getResonator();
            const size_t from = (size_t) (0.35 * SR), n = (size_t) (0.5 * SR);
            juce::String text;
            double least = 1.0e9;
            for (int p = 1; p <= 4; ++p)
            {
                const double on = levelAt(out, from, n, hz * r.partial(p));
                const double between = levelAt(out, from, n, hz * std::sqrt((double) r.partial(p) * r.partial(p + 1)));
                least = std::min(least, on / std::max(between, 1.0e-12));
                text << " " << juce::String(r.partial(p), 3) << ": " << juce::String(dB(on / std::max(between, 1.0e-12)), 0) << " dB";
            }
            expectWithinAbsoluteError(r.partial(2), 2.04f, 0.03f, "the plate's second partial");
            check(least > 4.0, "note " + juce::String(note) + ", partial against the place between it and the next:" + text);
        }
        // TUNE: a fifth up. KTRK 0: at middle C whatever the note.
        {
            Body b(SR, 57);
            b.body.set(B::Morph, (float) res::String);
            b.body.set(B::Tune, 7.0f);
            expectWithinAbsoluteError(b.body.frequency(), noteHz(64), 0.01);
            const auto out = b.noiseThenRing(0.3, 1.0);
            const size_t from = (size_t) (0.35 * SR), n = (size_t) (0.5 * SR);
            check(levelAt(out, from, n, noteHz(64)) > 8.0 * levelAt(out, from, n, noteHz(57)),
                   "TUNE +7: it rings on the fifth (" + juce::String(dB(levelAt(out, from, n, noteHz(64)) / levelAt(out, from, n, noteHz(57))), 0) + " dB over the note)");
        }
        for (int note : { 36, 84 })
        {
            Body b(SR, note);
            b.body.set(B::KeyTrack, 0.0f);
            expectWithinAbsoluteError(b.body.frequency(), noteHz(60), 0.01);
            b.body.set(B::KeyTrack, 0.5f);
            expectWithinAbsoluteError(b.body.frequency(), noteHz(60 + (note - 60) / 2), 0.01);
            b.body.setPitchOffset(2.0f);
            expectWithinAbsoluteError(b.body.frequency(), noteHz(62 + (note - 60) / 2), 0.01, "the engine's tune and bend move it");
        }
        // DCAY: the ring a second after the drive has stopped.
        {
            auto ringAt = [&](float decay)
            {
                Body b;
                b.body.set(B::Decay, decay);
                const auto out = b.noiseThenRing(0.3, 1.6);
                return rms(out, (size_t) (1.2 * SR), (size_t) (0.3 * SR)) / std::max(rms(out, (size_t) (0.31 * SR), (size_t) (0.1 * SR)), 1.0e-12);
            };
            const double shortRing = ringAt(0.3f), longRing = ringAt(4.0f);
            check(longRing > 0.05 && shortRing < 0.01, "a second later: DCAY 0.3 s " + juce::String(dB(shortRing), 0) + " dB, 4 s " + juce::String(dB(longRing), 0) + " dB");
        }
        // DAMP and POS are the body's latents.
        {
            auto ring = [&](int cell, float value)
            {
                Body b;
                b.body.set(cell, value);
                return b.noiseThenRing(0.3, 0.8);
            };
            const auto plain = ring(B::Damp, 0.0f), damped = ring(B::Damp, 0.7f), middle = ring(B::Pos, 0.75f);
            const size_t from = (size_t) (0.4 * SR), n = (size_t) (0.3 * SR);
            const double high = levelAt(plain, from, n, 220.0 * 9.0) / levelAt(plain, from, n, 220.0);
            const double highDamped = levelAt(damped, from, n, 220.0 * 9.0) / levelAt(damped, from, n, 220.0);
            check(highDamped < 0.3 * high, "DAMP: partial 9 against 1 " + juce::String(dB(high), 0) + " dB, damped " + juce::String(dB(highDamped), 0) + " dB");
            const double even = levelAt(plain, from, n, 440.0) / levelAt(plain, from, n, 220.0);
            const double evenMiddle = levelAt(middle, from, n, 440.0) / levelAt(middle, from, n, 220.0);
            check(evenMiddle < 0.3 * even, "POS: partial 2 against 1 " + juce::String(dB(even), 0) + " dB, driven in the middle " + juce::String(dB(evenMiddle), 0) + " dB");
        }
    }

    void bodyLevelAndCoupling()
    {
        beginTest("BODY: a sound that sits on the body's partials does not build up without end (the drive is eased as the body "
                  "fills, without a ripple); COUP makes the voice's level a bow: the body sings by itself");
        // A sine on the note, and one between two partials, three seconds each.
        auto sineThrough = [&](double hz, float decay, float level = 0.3f)
        {
            Body b;
            b.body.set(B::Morph, (float) res::Comb);
            b.body.set(B::Decay, decay);
            std::vector<float> out((size_t) (3.0 * SR));
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = b.body.process(level * (float) std::sin(2.0 * 3.141592653589793 * hz * (double) i / SR), true);
            return out;
        };
        const double in = 0.3 / std::sqrt(2.0);
        for (float decay : { 0.3f, 2.0f, 20.0f })
        {
            const auto on = sineThrough(220.0, decay), off = sineThrough(220.0 * 1.5, decay);
            const double onLevel = rms(on, 96000), offLevel = rms(off, 96000);
            // Its level in steady state, in windows of 50 ms: no ripple from the easing.
            double low = 1.0e9, high = 0.0;
            for (size_t w = 96000; w + 2400 <= on.size(); w += 2400)       // (2400 samples: eleven whole cycles of 220 Hz)
            {
                const double level = rms(on, w, 2400);
                low = std::min(low, level);
                high = std::max(high, level);
            }
            check(finite(on) && onLevel < 4.0 * in && onLevel > 0.5 * in && peak(on) <= 1.0,
                   "DCAY " + juce::String(decay, 1) + " s, on the note: " + juce::String(dB(onLevel / in), 1) + " dB re. the sine (peak "
                   + juce::String(peak(on), 2) + "); between two partials: " + juce::String(dB(offLevel / in), 1) + " dB");
            check(high / low < 1.03, "steady within " + juce::String(dB(high / low), 2) + " dB");
            check(offLevel < onLevel);
        }
        // Noise: about as loud out as in, for every body.
        {
            juce::String text;
            double least = 1.0e9, most = 0.0;
            for (int point = 0; point < res::NumBodyPoints; ++point)
            {
                Body b;
                b.body.set(B::Morph, (float) point);
                const auto out = b.noiseThenRing(2.0, 2.0);
                const double level = rms(out, 48000) / (0.2 / std::sqrt(3.0));
                least = std::min(least, level);
                most = std::max(most, level);
                text << " " << E::bodyName(point) << " " << juce::String(dB(level), 1);
            }
            check(least > 0.25 && most < 2.5, "noise through each body, dB re. the noise:" + text);
        }
        // COUP: a quiet sound that is nowhere near a partial; with the grip the string sings on its note.
        {
            auto bowed = [&](float couple)
            {
                Body b;
                b.body.set(B::Morph, (float) res::String);
                b.body.set(B::Couple, couple);
                std::vector<float> out((size_t) (3.0 * SR));
                for (size_t i = 0; i < out.size(); ++i)
                    out[i] = b.body.process(i < 96000 ? 0.25f * (float) std::sin(2.0 * 3.141592653589793 * 3000.5 * (double) i / SR) : 0.0f, i < 96000);
                return out;
            };
            const auto struck = bowed(0.0f), sung = bowed(0.75f);
            const double plain = levelAt(struck, 48000, 48000, 220.0), singing = levelAt(sung, 48000, 48000, 220.0);
            check(finite(sung) && singing > 20.0 * plain && singing > 0.02,
                   "at the string's note: " + juce::String(dB(plain), 0) + " dB struck, " + juce::String(dB(singing), 0) + " dB with COUP 75");
            // The bow leaves with the voice: half a second after it the string is dying.
            check(rms(sung, 130000, 14000) < 0.5 * rms(sung, 72000, 24000),
                   "after the voice has stopped: " + juce::String(dB(rms(sung, 130000, 14000) / rms(sung, 72000, 24000)), 1) + " dB");
        }
    }

    void bodySafe()
    {
        beginTest("BODY: off it is not engaged; MIX raised or dropped under sound does not jump; a body that has rung out is put "
                  "away and holds nothing; any input, cell and rate gives a finite, bounded sound");
        {
            B body;
            body.setSampleRate(SR);
            body.noteOn(60);
            check(! body.engaged() && ! body.ringing(), "MIX 0: not engaged");
            for (int c = B::Morph; c < B::NumCells; ++c)
                body.set(c, 0.7f);
            check(! body.engaged(), "... whatever the other cells say");
            body.set(B::Mix, 0.5f);
            check(body.engaged() && ! body.ringing());
        }
        // MIX from 0 to 1 and back under a steady tone: the largest step stays that of the tone.
        {
            B body;
            body.setSampleRate(SR);
            body.noteOn(57);
            body.set(B::Morph, (float) res::Plate);
            std::vector<float> out;
            auto run = [&](int samples)
            {
                for (int i = 0; i < samples; ++i)
                {
                    const float x = 0.3f * (float) std::sin(2.0 * 3.141592653589793 * 220.0 * (double) out.size() / SR);
                    out.push_back(body.engaged() ? body.process(x, true) : x);
                }
            };
            // (MIX is turned where the tone is at its top - 54 samples past a crossing - so that a
            // jump to or from the body's sound cannot hide in a zero of the tone.)
            run(4854);
            const double tone = largestStep(out);
            check(std::abs(out.back()) > 0.29f, "(the tone is at its top where MIX is raised: " + juce::String(out.back(), 3) + ")");
            body.set(B::Mix, 1.0f);
            run(24000);
            const double up = largestStep(out, 4754, 400), full = largestStep(out, 20000, 8000);
            body.set(B::Mix, 0.0f);
            run(4800);
            const double down = largestStep(out, 28754, 400);
            check(up < 1.5 * std::max(tone, full) && down < 1.5 * std::max(tone, full),
                   "largest step: the tone " + juce::String(tone, 4) + ", the body " + juce::String(full, 4) + ", MIX going up "
                   + juce::String(up, 4) + ", coming down " + juce::String(down, 4));
            check(! body.engaged() && ! body.ringing() && body.getResonator().modesAwake() == 0, "at 0 again it is put away, empty");
        }
        // Rung out: put away, nothing awake, and the time it took is of the order of DCAY.
        {
            Body b;
            b.body.set(B::Decay, 0.5f);
            int samples = 0;
            uint32_t seed = 5u;
            for (; samples < 4800; ++samples)
                b.body.process(0.2f * dsp::xorshiftNoise(seed), true);
            int tail = 0;
            while (b.body.ringing() && tail < 20 * 48000)
            {
                b.body.process(0.0f, false);
                ++tail;
            }
            check(! b.body.ringing() && b.body.getResonator().modesAwake() == 0 && b.body.getResonator().held() == 0.0f,
                   "put away after " + juce::String(tail / SR, 2) + " s");
            check(tail > 0.3 * SR && tail < 2.0 * SR);
            // The voice struck again: it starts empty and rings again.
            b.body.noteOn(57);
            check(b.body.engaged());
            b.body.process(0.5f, true);
            check(b.body.ringing());
        }
        // A note that begins with the body in has all of MIX from its first sample (it is not
        // faded in from the dry voice); and a body that rings goes on ringing through the next
        // note of its voice (it moves to the new note, it is not emptied).
        {
            Body b;
            b.body.process(0.3f, true);
            expectEquals(b.body.mixNowValue(), 1.0f, "MIX at the note's first sample");
            uint32_t seed = 11u;
            for (int i = 0; i < 9600; ++i)
                b.body.process(0.2f * dsp::xorshiftNoise(seed), true);
            for (int i = 0; i < 480; ++i)
                b.body.process(0.0f, false);
            const float before = b.body.getResonator().held();
            b.body.noteOn(64);
            b.body.process(0.0f, true);
            const float after = b.body.getResonator().held();
            check(before > 1.0e-6f && after > 0.9f * before && b.body.ringing(),
                  "what the body holds before the voice's next note " + juce::String(before, 6) + ", after its first sample " + juce::String(after, 6));
            expectWithinAbsoluteError(b.body.frequency(), noteHz(64), 0.01, "... and it is on the new note");
        }
        // The easing of the drive is moved to, not jumped to: a loud tone on the body's own note
        // fills it at once, and the drive comes down in steps no larger than its 2 ms allow.
        {
            Body b;
            b.body.set(B::Decay, 20.0f);
            float least = 1.0f, largest = 0.0f, last = b.body.driveNow();
            for (int i = 0; i < 24000; ++i)
            {
                b.body.process(0.9f * (float) std::sin(2.0 * 3.141592653589793 * 220.0 * (double) i / SR), true);
                const float d = b.body.driveNow();
                largest = std::max(largest, std::abs(d - last));
                least = std::min(least, d);
                last = d;
            }
            check(least < 0.3f && largest < 0.012f, "the drive comes down to " + juce::String(least, 3) + ", by " + juce::String(largest, 4) + " a sample at most");
        }
        // Anything in, every cell at both ends, four rates.
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        double most = 0.0;
        bool allFinite = true;
        const auto page = VoiceBodySection::makePage();
        for (double rate : { 22050.0, 44100.0, 96000.0, 192000.0 })
            for (int end = 0; end < 2; ++end)
                for (int note : { 0, 127 })
                {
                    B body;
                    body.setSampleRate(rate);
                    for (int c = 0; c < B::NumCells; ++c)
                        body.set(c, end == 0 ? page.params[(size_t) c].minVal : page.params[(size_t) c].maxVal);
                    body.set(B::Mix, 1.0f);
                    body.setPitchOffset(end == 0 ? -96.0f : 96.0f);
                    body.noteOn(note);
                    uint32_t seed = 9u;
                    for (int i = 0; i < 6000; ++i)
                    {
                        float x = dsp::xorshiftNoise(seed);
                        if (i % 500 == 100) x = nan;
                        if (i % 500 == 200) x = inf;
                        if (i % 500 == 300) x = -1.0e30f;
                        if (i == 3000)
                            for (int c = 0; c < B::NumCells; ++c)
                                body.set(c, c == B::Mix ? 1.0f : (end == 1 ? page.params[(size_t) c].minVal : page.params[(size_t) c].maxVal));
                        if (i == 4000) { body.set(B::Tune, nan); body.set(B::Decay, inf); body.setPitchOffset(nan); }
                        const float y = body.process(x, true);
                        allFinite = allFinite && std::isfinite(y);
                        most = std::max(most, (double) std::abs(y));
                    }
                }
        check(allFinite && most <= B::MAKEUP * R::OUT_LIMIT, "peak " + juce::String(most, 3));
    }

    // =========================================================================================
    // 5. BODY on the wave sequencer
    // =========================================================================================
    struct Seq
    {
        W engine;
        juce::AudioBuffer<float> buf;
        juce::MidiBuffer midi;
        int block;
        double sr;
        explicit Seq(double rate = SR, int blockSize = 64) : buf(2, blockSize), block(blockSize), sr(rate)
        {
            engine.prepareToPlay(rate, blockSize);
        }
        std::vector<float> render(double seconds, const std::function<void(int)>& perBlock = {})
        {
            std::vector<float> out;
            const int blocks = (int) std::lround(seconds * sr / block);
            out.reserve((size_t) blocks * (size_t) block);
            for (int b = 0; b < blocks; ++b)
            {
                if (perBlock) perBlock(b);
                buf.clear();
                engine.processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back(buf.getSample(0, i));
            }
            return out;
        }
        int sounding()
        {
            int n = 0;
            for (int v = 0; v < 8; ++v)
                n += engine.getVoice(v).isActive() ? 1 : 0;
            return n;
        }
    };

    void waveSeqPage()
    {
        beginTest("wave sequencer: page BODY (MIX MRPH DCAY COUP | DAMP POS TUNE KTRK) after the filter's pages; its cells read "
                  "their values and are destinations of the LFOs, the strip and the mod slots");
        W e;
        const auto pages = e.getParamPages();
        const int body = pageIndex(pages, "BODY");
        expectEquals(body, pageIndex(pages, "F ENV") + 1);
        check(body >= 0 && body < pageIndex(pages, "STRIP") && body < pageIndex(pages, "LFO1"), "before the mod slots, STRIP and the LFOs");
        static const char* const labels[8] = { "MIX", "MRPH", "DCAY", "COUP", "DAMP", "POS", "TUNE", "KTRK" };
        static const float values[8] = { 0.4f, 3.0f, 5.0f, 0.75f, -0.5f, 0.5f, 7.0f, 0.25f };
        if (body >= 0)
            for (int i = 0; i < 8; ++i)
            {
                const auto& cell = pages[(size_t) body].params[(size_t) i];
                check(cell.isValid() && std::strcmp(cell.label, labels[i]) == 0 && cell.id == WaveSeqEngine::BodyBase + i, labels[i]);
                expectEquals(e.getParameter(cell.id), cell.defaultVal, juce::String(labels[i]) + " reads its default");
                e.setParameter(cell.id, values[i]);
                expectEquals(e.getParameter(cell.id), values[i]);
                expectEquals(e.getVoice(3).getBody().get(i), values[i], juce::String(labels[i]) + " reaches the voices");
                e.setParameter(cell.id, 1.0e9f);
                expectEquals(e.getParameter(cell.id), cell.maxVal, juce::String(labels[i]) + " is held to its range");
                check(e.getParameterName(cell.id).startsWith("Body"));
                check(EngineParamRange::contains(cell.id), "its id is an engine id (stored with the sound)");
            }
        expectEquals(e.getParameter(WaveSeqEngine::BodyBase + B::Mix), 1.0f);
        const auto& dests = W::lfoDests();
        for (int c = 0; c < B::NumCells; ++c)
        {
            const int index = dests.indexOfLabel(VoiceBodySection::destLabel(c));
            // (Where they were when the list ended with them: 75-82. The router's ETIM, FTIM and PM follow.)
            expectEquals(index, 75 + c, juce::String(VoiceBodySection::destLabel(c)) + " is where it was in the list");
            if (index >= 0)
                expectEquals(dests[index].id, WaveSeqEngine::BodyBase + c);
        }
        expectEquals(dests.indexOfLabel("T16"), 74, "what was in the list keeps its place");
        expectEquals(dests.indexOfLabel("ETIM"), 91, "what is new comes after BODY's cells (shifted +8 by BDY2)");
    }

    // A sequence that is one step of noise (a steady, wide excitation), or the engine's own.
    static void noiseSequence(W& e)
    {
        e.setParameter(W::SeqLength, 1.0f);
        e.setParameter(W::ShapeMix, 5.0f);
        e.setParameter(W::PitchRange, 0.0f);
    }
    static void setBody(W& e, int cell, float value) { e.setParameter(WaveSeqEngine::BodyBase + cell, value); }

    void waveSeqOff()
    {
        beginTest("wave sequencer: with MIX at 0 the engine renders, sample for sample, what it renders without BODY, whatever "
                  "the other cells say and whatever was done to MIX before the note");
        auto play = [&](const std::function<void(W&)>& prepare, int left = 0)
        {
            Seq rig;
            if (prepare) prepare(rig.engine);
            rig.engine.noteOn(57, 0.9f, 0);
            rig.engine.noteOn(64, 0.6f, 0);
            auto out = rig.render(0.6);
            // (Not by the clock: with MIX at 0 no voice's body is run, held or waited for.)
            if (left == 0)
                for (int v = 0; v < 8; ++v)
                {
                    auto& body = rig.engine.getVoice(v).getBody();
                    check(! body.engaged() && ! body.ringing() && body.getResonator().modesAwake() == 0, "voice " + juce::String(v + 1) + ": its body is not run");
                }
            rig.engine.noteOff(57, 0);
            rig.engine.noteOff(64, 0);
            const auto tail = rig.render(1.5);
            out.insert(out.end(), tail.begin(), tail.end());
            expectEquals(rig.sounding(), left, "the voices that are left a second and a half after the note-offs");
            return out;
        };
        const auto plain = play({});
        const auto cells = play([](W& e)
        {
            setBody(e, B::Morph, 3.0f); setBody(e, B::Decay, 9.0f); setBody(e, B::Couple, 0.75f); setBody(e, B::Damp, 0.5f);
            setBody(e, B::Pos, 0.5f); setBody(e, B::Tune, 7.0f); setBody(e, B::KeyTrack, 0.0f);
        });
        const auto wasOn = play([](W& e) { setBody(e, B::Mix, 0.8f); setBody(e, B::Mix, 0.0f); });
        check(rms(plain) > 0.01, "(it sounds)");
        check(plain == cells, "the other seven cells at other values: the same samples");
        check(plain == wasOn, "MIX raised and put back before the note: the same samples");
        const auto on = play([](W& e) { setBody(e, B::Mix, 0.5f); }, 2);
        check(! (plain == on) && unlike(plain, on) > 0.05, "(with MIX up it is another sound: unlike by " + juce::String(unlike(plain, on), 3) + ")");
    }

    void waveSeqPlate()
    {
        beginTest("wave sequencer: a voice through PLAT rings at the plate's partials on the played note, and goes on ringing "
                  "when the note's own release is over");
        for (int note : { 45, 57 })
        {
            Seq rig;
            setBody(rig.engine, B::Mix, 1.0f);
            setBody(rig.engine, B::Morph, (float) res::Plate);
            setBody(rig.engine, B::Decay, 3.0f);
            rig.engine.noteOn(note, 0.9f, 0);
            auto out = rig.render(0.5);
            rig.engine.noteOff(note, 0);
            const size_t off = out.size();
            const auto tail = rig.render(1.0);
            out.insert(out.end(), tail.begin(), tail.end());
            // From 0.3 s after the note-off (the amp release is 0.2 s): only the body sounds.
            const size_t from = off + (size_t) (0.3 * SR), n = (size_t) (0.6 * SR);
            auto& r = rig.engine.getVoice(0).getBody().getResonator();
            const double hz = noteHz(note);
            juce::String text;
            double least = 1.0e9;
            for (int p = 1; p <= 4; ++p)
            {
                const double on = levelAt(out, from, n, hz * r.partial(p));
                const double between = levelAt(out, from, n, hz * std::sqrt((double) r.partial(p) * r.partial(p + 1)));
                least = std::min(least, on / std::max(between, 1.0e-12));
                text << " " << juce::String(r.partial(p), 3) << ": " << juce::String(dB(on / std::max(between, 1.0e-12)), 0) << " dB";
            }
            check(rms(out, from, n) > 1.0e-3, "it still rings: " + juce::String(dB(rms(out, from, n)), 1) + " dB");
            check(least > 4.0, "note " + juce::String(note) + ", partial against the place between it and the next:" + text);
            check(rig.engine.getVoice(0).isActive() && rig.sounding() == 1, "the voice is alive while its body rings");
            check(finite(out) && peak(out) < 1.5, "peak " + juce::String(peak(out), 2));
        }
        // Without BODY the same note is silent by then; and a body that has rung out frees its voice.
        {
            Seq plain, ringing;
            setBody(ringing.engine, B::Mix, 1.0f);
            setBody(ringing.engine, B::Morph, (float) res::Plate);
            setBody(ringing.engine, B::Decay, 0.4f);
            for (Seq* rig : { &plain, &ringing })
            {
                rig->engine.noteOn(57, 0.9f, 0);
                rig->render(0.3);
                rig->engine.noteOff(57, 0);
            }
            const auto a = plain.render(0.6), b = ringing.render(0.6);
            check(rms(a, 14400) == 0.0 && plain.sounding() == 0, "without it: silent 0.3 s after the note-off");
            check(rms(b, 14400, 4800) > 1.0e-5 && ringing.sounding() == 1, "with it: " + juce::String(dB(rms(b, 14400, 4800)), 0) + " dB there");
            ringing.render(3.0);
            const auto later = ringing.render(0.1);
            check(ringing.sounding() == 0 && rms(later) == 0.0, "rung out: the voice is free and gives exact silence");
            // allNotesOff empties bodies too.
            ringing.engine.noteOn(57, 0.9f, 0);
            ringing.render(0.2);
            ringing.engine.allNotesOff();
            expectEquals(ringing.sounding(), 0);
        }
        // The engine's tune and bend take the body with the voice; the sequence's own steps do not.
        {
            Seq rig;
            noiseSequence(rig.engine);
            setBody(rig.engine, B::Mix, 1.0f);
            rig.engine.setParameter(W::TuneParam, 5.0f);
            rig.engine.pitchBend(1.0f);
            rig.engine.noteOn(57, 0.9f, 0);
            rig.render(0.1);
            expectWithinAbsoluteError(rig.engine.getVoice(0).getBody().frequency(), noteHz(57 + 5 + 2), 0.01);
        }
    }

    void waveSeqModulated()
    {
        beginTest("wave sequencer: the body's position is moved within a note by the engine's LFO and by a mod slot; MIX from an "
                  "LFO brings the body in and out");
        const auto& dests = W::lfoDests();
        const int morph = dests.indexOfLabel("BMRP"), mix = dests.indexOfLabel("BMIX");
        auto play = [&](const std::function<void(W&)>& prepare, float* sheetLow = nullptr, float* sheetHigh = nullptr)
        {
            Seq rig;
            noiseSequence(rig.engine);
            setBody(rig.engine, B::Mix, 1.0f);
            setBody(rig.engine, B::Morph, 2.5f);        // between COMB and PLAT
            if (prepare) prepare(rig.engine);
            rig.engine.noteOn(57, 0.9f, 0);
            float low = 2.0f, high = -1.0f;
            auto out = rig.render(1.5, [&](int)
            {
                const float sheet = rig.engine.getVoice(0).getBody().getResonator().body()[res::Sheet];
                low = std::min(low, sheet);
                high = std::max(high, sheet);
            });
            if (sheetLow != nullptr) *sheetLow = low;
            if (sheetHigh != nullptr) *sheetHigh = high;
            return out;
        };
        float low = 0.0f, high = 0.0f;
        const auto still = play({}, &low, &high);
        check(high - low < 0.51f && high <= 0.51f, "at rest the body stays where MRPH puts it (SHT " + juce::String(low, 2) + " to " + juce::String(high, 2) + ")");
        const auto byLfo = play([&](W& e)
        {
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 3.0f);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) morph);
            e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.4f * 5.0f / (float) (res::NumBodyPoints - 1));
        }, &low, &high);
        check(high > 0.95f && low < 0.05f, "LFO 1 on BMRP: SHT goes from " + juce::String(low, 2) + " to " + juce::String(high, 2) + " within the note");
        check(finite(byLfo) && unlike(still, byLfo) > 0.1, "... and the sound with it: unlike by " + juce::String(unlike(still, byLfo), 3));
        const auto bySlot = play([&](W& e)
        {
            e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Rate), 3.0f);
            e.setParameter(ModSlotParam::id(0, ModSlotParam::SRC), (float) ModSrc::Lfo2);
            e.setParameter(ModSlotParam::id(0, ModSlotParam::DEST), (float) morph);
            e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.4f * 5.0f / (float) (res::NumBodyPoints - 1));
        }, &low, &high);
        check(high > 0.95f && low < 0.05f, "a slot from LFO 2 on BMRP: SHT goes from " + juce::String(low, 2) + " to " + juce::String(high, 2));
        check(finite(bySlot) && unlike(still, bySlot) > 0.1, "... unlike by " + juce::String(unlike(still, bySlot), 3));

        // MIX at 0 with an LFO on it: the body comes and goes, and nothing jumps.
        {
            Seq rig;
            setBody(rig.engine, B::Morph, (float) res::Plate);
            rig.engine.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 2.0f);
            rig.engine.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) mix);
            rig.engine.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 1.0f);
            rig.engine.noteOn(57, 0.9f, 0);
            float least = 2.0f, most = -1.0f;
            const auto out = rig.render(1.5, [&](int)
            {
                const float m = rig.engine.getVoice(0).getBody().mixNowValue();
                least = std::min(least, m);
                most = std::max(most, m);
            });
            check(most > 0.9f && least == 0.0f && finite(out), "MIX runs from " + juce::String(least, 2) + " to " + juce::String(most, 2));
        }
        // The LFO goes on under a body that rings out: what it moves in the body keeps moving
        // when the voice's own sound has ended.
        {
            Seq rig;
            noiseSequence(rig.engine);
            setBody(rig.engine, B::Mix, 1.0f);
            setBody(rig.engine, B::Morph, 2.5f);
            setBody(rig.engine, B::Decay, 8.0f);
            rig.engine.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 3.0f);
            rig.engine.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) morph);
            rig.engine.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.4f * 5.0f / (float) (res::NumBodyPoints - 1));
            rig.engine.noteOn(57, 0.9f, 0);
            rig.render(0.3);
            rig.engine.noteOff(57, 0);
            rig.render(0.5);        // (the amp release is 0.2 s: only the body is left)
            float tailLow = 2.0f, tailHigh = -1.0f;
            const auto tail = rig.render(1.0, [&](int)
            {
                const float sheet = rig.engine.getVoice(0).getBody().getResonator().body()[res::Sheet];
                tailLow = std::min(tailLow, sheet);
                tailHigh = std::max(tailHigh, sheet);
            });
            check(rig.sounding() == 1 && rms(tail) > 1.0e-5, "(the body still rings: " + juce::String(dB(rms(tail)), 0) + " dB)");
            check(tailHigh > 0.95f && tailLow < 0.05f, "in the tail SHT still goes from " + juce::String(tailLow, 2) + " to " + juce::String(tailHigh, 2));
        }
    }

    // =========================================================================================
    // 6. Extremes, and the engine inside the app's audio engine (added by the U3 review)
    // =========================================================================================
    void waveSeqExtremes()
    {
        beginTest("wave sequencer with BODY: 22.05 and 192 kHz, blocks of 1 and 4096, every BODY cell at both ends with both LFOs "
                  "at full depth on its destinations, notes 0 to 127, values that are no numbers: finite, bounded, and every "
                  "voice is free again once the bodies have rung out");
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        const auto page = VoiceBodySection::makePage();
        const auto& dests = W::lfoDests();
        const int first = dests.indexOfLabel("BMIX");
        check(first > 0 && dests.indexOfLabel("BKTK") == first + 7, "the eight destinations are together, BMIX first");
        struct Case { double rate; int block; };
        for (auto c : { Case { 22050.0, 1 }, Case { 22050.0, 4096 }, Case { 192000.0, 1 }, Case { 192000.0, 4096 } })
            for (int end = 0; end < 2; ++end)
            {
                Seq rig(c.rate, c.block);
                auto& e = rig.engine;
                for (int cell = 0; cell < B::NumCells; ++cell)
                    setBody(e, cell, end == 0 ? page.params[(size_t) cell].minVal : page.params[(size_t) cell].maxVal);
                setBody(e, B::Mix, 1.0f);
                e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 30.0f);
                e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 1.0f);
                e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Rate), 17.0f);
                e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Depth), -1.0f);
                e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Dest), (float) (first + B::Tune));
                e.setParameter(W::TuneParam, end == 0 ? -24.0f : 24.0f);
                e.pitchBend(end == 0 ? -1.0f : 1.0f);
                static const int notes[8] = { 0, 127, 12, 115, 36, 60, 84, 101 };
                for (int n : notes)
                    e.noteOn(n, 1.0f, 0);
                bool allFinite = true;
                double most = 0.0;
                const double seconds = 0.24;
                const int blocks = std::max(1, (int) std::lround(seconds * c.rate / c.block));
                int step = 0;
                const int every = std::max(1, blocks / 8);
                const auto out = rig.render(seconds, [&](int b)
                {
                    if (b % every != 0)
                        return;
                    // LFO 1 goes round the eight destinations; the cells get what is no number.
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) (first + step % 8));
                    setBody(e, step % 8, step % 3 == 0 ? nan : (step % 3 == 1 ? inf : -1.0e30f));
                    setBody(e, B::Mix, 1.0f);
                    ++step;
                });
                for (float v : out)
                {
                    allFinite = allFinite && std::isfinite(v);
                    most = std::max(most, (double) std::abs(v));
                }
                const juce::String where = juce::String(c.rate, 0) + " Hz, block " + juce::String(c.block) + ", cells at their "
                                           + (end == 0 ? "lowest" : "highest");
                // (Eight voices, each held to MAKEUP x the resonator's limit, x the engine's 0.7.)
                check(allFinite && most <= 8.0 * 0.7 * B::MAKEUP * R::OUT_LIMIT, where + ": peak " + juce::String(most, 3));
                // Let go, the shortest ring, the LFOs off: every voice is put away.
                for (int n : notes)
                    e.noteOff(n, 0);
                e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), 0.0f);
                e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Dest), 0.0f);
                setBody(e, B::Decay, 0.05f);
                setBody(e, B::Damp, 1.0f);
                setBody(e, B::Couple, 0.0f);
                e.setParameter(W::AmpRelease, 0.01f);
                rig.render(1.2);
                expectEquals(rig.sounding(), 0, where + ": the voices left 1.2 s after the note-offs with DCAY at 50 ms");
                const auto after = rig.render(0.02);
                check(rms(after) == 0.0, where + ": exact silence then");
            }

        // The longest ring there is (DCAY 20 s, DAMP at its lowest, each body) still ends: a voice is not held for ever.
        for (int point = 0; point < res::NumBodyPoints; ++point)
        {
            Body b;
            b.body.set(B::Morph, (float) point);
            b.body.set(B::Decay, 20.0f);
            b.body.set(B::Damp, -1.0f);
            uint32_t seed = 77u;
            for (int i = 0; i < 4800; ++i)
                b.body.process(0.5f * dsp::xorshiftNoise(seed), true);
            long tail = 0;
            const long limit = (long) (90.0 * SR);
            while (b.body.ringing() && tail < limit)
            {
                b.body.process(0.0f, false);
                ++tail;
            }
            check(! b.body.ringing(), "body " + juce::String(point) + " at DCAY 20 s, DAMP -100: put away after " + juce::String(tail / SR, 1) + " s");
        }
    }

    void throughAudioEngine()
    {
        beginTest("in the app's audio engine: BODY's cells are the track's parameters (set, read back, step-locked), and the "
                  "track goes on sounding after the note while the body rings");
        check(EngineParamRange::contains(WaveSeqEngine::BodyBase) && EngineParamRange::contains(WaveSeqEngine::BodyBase + B::KeyTrack),
              "its ids are engine ids");
        SequencerCore seq;
        AudioEngine audio;
        audio.setSequencer(&seq);
        audio.prepare(SR, 256);
        audio.addEngine(0, AudioEngine::createEngine(EngineType::WaveSeq));
        auto* e = dynamic_cast<W*>(audio.getEngine(0));
        if (e == nullptr)
        {
            expect(false, "track 1 is a wave sequencer");
            return;
        }
        const int mixId = WaveSeqEngine::BodyBase + B::Mix, morphId = WaveSeqEngine::BodyBase + B::Morph;
        audio.setTrackParameter(0, morphId, (float) res::Plate);
        audio.setTrackParameter(0, WaveSeqEngine::BodyBase + B::Decay, 2.0f);
        expectWithinAbsoluteError(audio.getTrackParameter(0, morphId), (float) res::Plate, 1.0e-6f);
        std::vector<float> l(256), r(256);
        auto render = [&](double seconds)
        {
            std::vector<float> out;
            for (int b = 0; b < (int) std::lround(seconds * SR / 256); ++b)
            {
                audio.renderBlock(l.data(), r.data(), 256);
                out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        };
        auto tailOf = [&]
        {
            e->noteOn(57, 0.9f, -1);
            render(0.3);
            e->noteOff(57, -1);
            render(0.4);
            const auto tail = render(0.3);
            render(4.0);
            return tail;
        };
        const auto dry = tailOf();
        check(rms(dry) < 1.0e-6, "MIX 0: silent 0.4 s after the note-off (" + juce::String(dB(rms(dry)), 0) + " dB)");
        // A step lock on MIX: the trig's own value, and back afterwards.
        Step locked;
        locked.paramLocks[mixId] = 1.0f;
        audio.applyStepLocks(0, locked);
        expectWithinAbsoluteError(audio.getTrackParameter(0, mixId), 1.0f, 1.0e-6f, "MIX as the step locks it");
        const auto wet = tailOf();
        check(finite(wet) && rms(wet) > 1.0e-4, "MIX locked to 100: the plate rings there (" + juce::String(dB(rms(wet)), 0) + " dB)");
        audio.applyStepLocks(0, Step());
        expectWithinAbsoluteError(audio.getTrackParameter(0, mixId), 0.0f, 1.0e-6f, "MIX after the lock");
        const auto again = tailOf();
        check(rms(again) < 1.0e-6, "... and the track is as it was");
    }

    // =========================================================================================
    // 7. Cost
    // =========================================================================================
    template <typename Fn>
    static double microsPerBlock(int blocks, Fn&& renderBlock)
    {
        // The best of three passes (the machine is shared).
        double best = 1.0e30;
        for (int pass = 0; pass < 3; ++pass)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b)
                renderBlock();
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            best = std::min(best, us / blocks);
        }
        return best;
    }

    void cost()
    {
        beginTest("Cost: microseconds a block of 512 samples at 48 kHz (this build; the numbers of the optimised build are in the "
                  "commit that added BODY)");
        const bool longRun = juce::SystemStats::getEnvironmentVariable("NEPTUNE_BODY_COST", {}).isNotEmpty();
        const int blocks = longRun ? 400 : 12;
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buf(2, 512);

        // The wave sequencer, eight notes held: BODY off, on, on with COUP.
        auto seq = [&](float mixValue, float couple)
        {
            W e;
            e.prepareToPlay(SR, 512);
            setBody(e, B::Mix, mixValue);
            setBody(e, B::Morph, (float) res::Plate);
            setBody(e, B::Couple, couple);
            static const int steps[8] = { 0, 4, 7, 11, 14, 17, 21, 24 };
            for (int s : steps)
                e.noteOn(48 + s, 0.8f, 0);
            for (int i = 0; i < 20; ++i) { buf.clear(); e.processBlock(buf, midi); }
            return microsPerBlock(blocks, [&] { buf.clear(); e.processBlock(buf, midi); });
        };
        const double off = seq(0.0f, 0.0f), offAgain = seq(0.0f, 0.0f), on = seq(1.0f, 0.0f), coupled = seq(1.0f, 0.75f);
        logMessage("wave sequencer, 8 notes: BODY off " + juce::String(off, 1) + " / " + juce::String(offAgain, 1) + " us, on "
                   + juce::String(on, 1) + " us, on with COUP 75 " + juce::String(coupled, 1) + " us  (a body: "
                   + juce::String((on - off) / 8.0, 1) + " us a voice)");

        // One body by itself, and what it rests at.
        {
            Body b;
            b.body.set(B::Morph, (float) res::Plate);
            uint32_t seed = 3u;
            float sink = 0.0f;
            const double driven = microsPerBlock(blocks, [&] { for (int i = 0; i < 512; ++i) sink += b.body.process(0.2f * dsp::xorshiftNoise(seed), true); });
            int tail = 0;
            while (b.body.ringing() && tail++ < 30 * 48000)
                b.body.process(0.0f, false);
            B offBody;
            offBody.setSampleRate(SR);
            int engagedCount = 0;
            const double asleep = microsPerBlock(blocks, [&] { for (int i = 0; i < 512; ++i) engagedCount += offBody.engaged() ? 1 : 0; });
            logMessage("one body driven by noise " + juce::String(driven, 1) + " us; the test a voice makes when it is off "
                       + juce::String(asleep, 2) + " us" + (sink == 12345.0f || engagedCount < 0 ? "." : ""));
            check(engagedCount == 0);
        }

        // The exciter's grains: a scrape with noise impulses against one with grains of a recording.
        {
            const auto noise = noiseSample(48000);
            const auto source = sourceOf(noise);
            auto scrape = [&](const res::GrainSource* s, float gran)
            {
                Scrape rig(SR, gran);
                rig.r.setGrainSource(s);
                rig.r.reset();
                rig.r.strike(0.5f);
                float sink = 0.0f;
                for (int i = 0; i < 4800; ++i) sink += rig.r.tick(0.0f, 0.5f);
                const double us = microsPerBlock(blocks, [&] { for (int i = 0; i < 512; ++i) sink += rig.r.tick(0.0f, 0.5f); });
                return sink == 12345.0f ? 0.0 : us;
            };
            logMessage("one resonator scraped: noise impulses GRAN 45 " + juce::String(scrape(nullptr, 0.45f), 1) + " us, 100 "
                       + juce::String(scrape(nullptr, 1.0f), 1) + " us; grains of a recording GRAN 45 " + juce::String(scrape(&source, 0.45f), 1)
                       + " us (0.65 sounding), 100 " + juce::String(scrape(&source, 1.0f), 1) + " us (12 sounding)");
        }

        // The scheduler against the sampler's own grain code: one voice, 0.1 s x 40 a second, spray.
        {
            const auto noise = noiseSample(96000);
            SamplerEngine e;
            e.prepareToPlay(SR, 512);
            e.loadSample(noise.data(), (int) noise.size(), 1, SR);
            e.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Granular);
            e.setParameter(SamplerEngine::GrainSpray, 0.5f);
            e.setParameter(SamplerEngine::GrainDensity, 160.0f);
            e.noteOn(60, 1.0f, 0);
            for (int i = 0; i < 20; ++i) { buf.clear(); e.processBlock(buf, midi); }
            const double sampler = microsPerBlock(blocks, [&] { buf.clear(); e.processBlock(buf, midi); });
            const int grains = e.getActiveGrainCount();

            grn::Cloud<48> c;
            c.reset();
            grn::Spec s;
            s.size = 0.1f;
            s.density = 160.0f;
            s.jitter = s.scatter = 0.5f;
            s.scatterFrames = 12000.0f;
            const grn::Area area { 0.0, 96000.0 };
            float sink = 0.0f;
            auto run = [&]
            {
                for (int i = 0; i < 512; ++i)
                {
                    if (c.due(160.0f, 1.0 / SR))
                        c.start(s, 30000.0, area, 1.0, SR);
                    float l, r;
                    c.sum([&](double at, float& sl, float& sr) { sl = sr = grn::readLinear(noise.data(), 96000, at); }, 1.0, area, l, r);
                    sink += l + r;
                }
            };
            for (int i = 0; i < 20; ++i) run();
            const double cloud = microsPerBlock(blocks, run);
            logMessage("16 grains at once: the sampler's voice (its grain code, filter, envelope and the engine round it) "
                       + juce::String(sampler, 1) + " us with " + juce::String(grains) + " grains; the scheduler alone "
                       + juce::String(cloud, 1) + " us with " + juce::String(c.sounding()) + (sink == 12345.0f ? "." : ""));
        }
        // (With MIX at 0 nothing of the body is run: a body that ran would cost what one costs.)
        check(on > 1.5 * std::max(off, offAgain), "eight bodies cost more than half as much again as the voices alone, and none is run at MIX 0");
    }

    void runTest() override
    {
        schedulerClock();
        schedulerGrain();
        schedulerPool();
        schedulerAgainstSampler();
        resonatorGrains();
        resonatorGrainsSafe();
        rsnGrainPage();
        bodyPartials();
        bodyLevelAndCoupling();
        bodySafe();
        waveSeqPage();
        waveSeqOff();
        waveSeqPlate();
        waveSeqModulated();
        waveSeqExtremes();
        throughAudioEngine();
        cost();
    }
};

static VoiceBodyTests voiceBodyTests;
