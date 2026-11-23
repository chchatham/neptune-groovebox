#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>
#include "track_input_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/dsp/effects/Detector.h"
#include "../src/dsp/effects/EqBand.h"
#include "../src/dsp/effects/CellEffect.h"
#include "../src/dsp/effects/Compressor.h"
#include "../src/dsp/effects/SidechainInsert.h"
#include "../src/dsp/effects/Limiter.h"
#include "../src/dsp/effects/TransientShaper.h"
#include "../src/dsp/effects/Wah.h"
#include "../src/dsp/effects/ParametricEQ.h"
#include "../src/dsp/effects/GraphicEQ.h"
#include "../src/dsp/effects/AmpSim.h"
#include "../src/dsp/effects/EffectChain.h"

// Phase E3 of .ralph/spec-unify.md: the dynamics core (Detector.h: one detector, one follower, one
// sliding minimum, one gain computer, one compressor), the key from the track's input (KEY: SELF /
// IN), the one equaliser band (EqBand.h) and modulateCell on the effects built from them.
//
// Levels are RMS over a window. The limiter and the compressor at 1:1 sit in the master chain,
// whose sound is pinned ("P01 Keyboard"): here the limiter is compared, bit for bit, with the
// arithmetic it had before it was moved onto the core (kept below as OldLimiter).
class EffectCoresDynamicsTests : public juce::UnitTest
{
public:
    EffectCoresDynamicsTests() : UnitTest("Effect Cores Dynamics") {}

    void runTest() override
    {
        testSlidingMin();
        testPeakHold();
        testFollower();
        testDetector();
        testGainComputer();
        testLimiterIsWhatItWas();
        testCompressor();
        testKeying();
        testKeyThroughTheAudioEngine();
        testTransientShaper();
        testEqBand();
        testEqualisers();
        testModulateCell();
        testBounds();
        testReview();
    }

private:
    using Buf = std::vector<float>;
    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 256;
    static constexpr double TWO_PI = 6.283185307179586;

    static float dB(double ratio) { return (float) (20.0 * std::log10(std::max(ratio, 1.0e-12))); }

    static double rms(const Buf& x, int from, int to)
    {
        from = std::max(0, from);
        to = std::min(to, (int) x.size());
        double sum = 0.0;
        for (int i = from; i < to; ++i) sum += (double) x[(size_t) i] * x[(size_t) i];
        return to > from ? std::sqrt(sum / (to - from)) : 0.0;
    }

    static bool allFinite(const Buf& x)
    {
        for (float v : x) if (! std::isfinite(v)) return false;
        return true;
    }

    static float biggestStep(const Buf& x, int from, int to)
    {
        float step = 0.0f;
        for (int i = std::max(1, from); i < std::min(to, (int) x.size()); ++i)
            step = std::max(step, std::abs(x[(size_t) i] - x[(size_t) i - 1]));
        return step;
    }

    static Buf sine(double hz, float amp, int n, double sr = SR)
    {
        Buf x((size_t) n);
        for (int i = 0; i < n; ++i) x[(size_t) i] = amp * (float) std::sin(TWO_PI * hz * i / sr);
        return x;
    }

    static Buf noise(float amp, int n, uint32_t seed)
    {
        Buf x((size_t) n);
        for (auto& v : x)
        {
            seed = seed * 1664525u + 1013904223u;
            v = amp * (float) (int32_t) seed * (1.0f / 2147483648.0f);
        }
        return x;
    }

    // A tone that is switched on and off: `onSamples` sounding out of every `period`.
    static Buf bursts(double hz, float amp, int n, int period, int onSamples)
    {
        Buf x = sine(hz, amp, n);
        for (int i = 0; i < n; ++i)
            if (i % period >= onSamples) x[(size_t) i] = 0.0f;
        return x;
    }

    // Runs `in` (both channels) through the effect in blocks; `key`, when given, is handed over
    // before every block (setInput), as the AudioEngine does. Returns the left channel.
    static Buf run(EffectBase& fx, const Buf& in, const Buf* key = nullptr, int block = BLOCK,
                   const std::function<void(int)>& atBlock = {})
    {
        Buf out;
        out.reserve(in.size());
        std::vector<float> l((size_t) block), r((size_t) block);
        for (int done = 0, b = 0; done < (int) in.size(); done += block, ++b)
        {
            const int n = std::min(block, (int) in.size() - done);
            std::copy(in.begin() + done, in.begin() + done + n, l.begin());
            std::copy(in.begin() + done, in.begin() + done + n, r.begin());
            if (atBlock) atBlock(b);
            if (key != nullptr && fx.wantsInput())
                fx.setInput(key->data() + done, key->data() + done, n);
            fx.processStereo(l.data(), r.data(), n);
            out.insert(out.end(), l.begin(), l.begin() + n);
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------
    void testSlidingMin()
    {
        beginTest("SlidingMin: the smallest of the last N, as a scan of the window gives it, the window changed on the way");
        dyn::SlidingMin<64> window;
        window.reset(1.0f);
        std::array<float, 4096> history;
        history.fill(1.0f);
        juce::Random rng(5);
        int n = 17, wrong = 0;
        for (int i = 0; i < 4096; ++i)
        {
            if (i % 301 == 300) n = 1 + rng.nextInt(63);            // LOOK turned
            // long runs of 1 (a quiet stretch), now and then a dip; equal values too
            const float v = rng.nextFloat() < 0.7f ? 1.0f : (float) rng.nextInt(8) / 8.0f;
            history[(size_t) i] = v;
            const float got = window.push(v, n);
            float want = 1.0f;
            for (int back = 0; back < n; ++back)
                want = std::min(want, i - back >= 0 ? history[(size_t) (i - back)] : 1.0f);
            if (got != want) ++wrong;
        }
        expectEquals(wrong, 0);

        window.reset(1.0f);
        expectEquals(window.push(0.5f, 4), 0.5f);
        for (int i = 0; i < 3; ++i) expectEquals(window.push(1.0f, 4), 0.5f, "still inside the window");
        expectEquals(window.push(1.0f, 4), 1.0f, "and gone after it");
    }

    void testPeakHold()
    {
        beginTest("PeakHold: never less than the largest of the last N, never more than that of the last 2 N; a tone reads steady");
        dyn::PeakHold hold;
        hold.length = 50;
        hold.reset();
        std::vector<float> history;
        juce::Random rng(11);
        int wrong = 0;
        for (int i = 0; i < 5000; ++i)
        {
            const float v = rng.nextFloat() < 0.05f ? rng.nextFloat() : 0.01f * rng.nextFloat();
            history.push_back(v);
            const float got = hold.push(v);
            float lastN = 0.0f, last2N = 0.0f;
            for (int back = 0; back < 100 && back <= i; ++back)
            {
                const float h = history[(size_t) (i - back)];
                if (back < 50) lastN = std::max(lastN, h);
                last2N = std::max(last2N, h);
            }
            if (got < lastN || got > last2N) ++wrong;
        }
        expectEquals(wrong, 0);

        hold.length = 480;                      // 10 ms: a 50 Hz tone's two half-waves
        hold.reset();
        float low = 1.0f;
        const Buf tone = sine(50.0, 1.0f, 9600);
        for (int i = 0; i < 9600; ++i)
        {
            const float v = hold.push(std::abs(tone[(size_t) i]));
            if (i > 960) low = std::min(low, v);
        }
        expect(low > 0.999f, "50 Hz held flat: " + juce::String(low));
    }

    void testFollower()
    {
        beginTest("Follower: attack, hold and release, as an envelope (up) and as a gain (down)");
        {
            dyn::Follower env;
            env.reset(0.0f);
            env.setTimes(SR, 10.0f, 100.0f);
            env.setHold(SR, 20.0f);
            float y = 0.0f;
            for (int i = 0; i < 480; ++i) y = env.up(1.0f);         // one attack time
            expectWithinAbsoluteError(y, 1.0f - std::exp(-1.0f), 0.01f);
            for (int i = 0; i < 9600; ++i) y = env.up(1.0f);
            expectWithinAbsoluteError(y, 1.0f, 0.001f);
            for (int i = 0; i < 959; ++i) y = env.up(0.0f);         // just short of the hold
            expectWithinAbsoluteError(y, 1.0f, 0.001f, "held");
            for (int i = 0; i < 4800 + 1; ++i) y = env.up(0.0f);    // the hold's last sample, then one release time
            expectWithinAbsoluteError(y, std::exp(-1.0f), 0.01f);
            for (int i = 0; i < 480000; ++i) y = env.up(0.0f);
            expect(y == 0.0f, "falls to nothing, not into the denormals: " + juce::String(y));
        }
        {
            dyn::Follower gain;
            gain.reset(1.0f);
            gain.setTimes(SR, 0.0f, 50.0f);
            expectEquals(gain.down(0.25f), 0.25f, "no attack time: there at once");
            float y = 0.0f;
            for (int i = 0; i < 2400; ++i) y = gain.down(1.0f);
            expectWithinAbsoluteError(y, 1.0f - 0.75f * std::exp(-1.0f), 0.01f);
            gain.reset(1.0f);
            for (int i = 0; i < 1000; ++i) y = gain.down(1.0f);
            expect(y == 1.0f, "a gain of 1 asked for 1 stays exactly 1 (the master compressor at 1:1)");
        }
    }

    void testDetector()
    {
        beginTest("Detector: peak, RMS, the side-chain high-pass, and levels that are not numbers");
        dyn::Detector d;
        d.prepare(SR);
        expectEquals(d.level(0.25f, -0.75f), 0.75f, "peak: the larger channel");

        d.setMode(dyn::Detector::RMS);
        float level = 0.0f;
        const Buf tone = sine(1000.0, 0.8f, 9600);
        for (float v : tone) level = d.level(v, v);
        expectWithinAbsoluteError(level, 0.8f * 0.7071f, 0.02f);

        auto steady = [&](float highPassHz, double hz)
        {
            dyn::Detector det;
            det.prepare(SR);
            det.setHighPass(highPassHz);
            float top = 0.0f;
            const Buf x = sine(hz, 1.0f, 9600);
            for (int i = 0; i < 9600; ++i)
            {
                const float v = det.level(x[(size_t) i], x[(size_t) i]);
                if (i > 4800) top = std::max(top, v);
            }
            return top;
        };
        expectWithinAbsoluteError(steady(dyn::Detector::HP_OFF, 50.0), 1.0f, 0.001f, "at 20 Hz the high-pass is not there");
        expect(steady(1000.0f, 50.0) < 0.1f, "a bass note under a 1 kHz high-pass: " + juce::String(steady(1000.0f, 50.0)));
        expect(steady(1000.0f, 7000.0) > 0.9f, "the top still reads");

        for (auto mode : { dyn::Detector::Peak, dyn::Detector::RMS })
            for (float hp : { 0.0f, 500.0f })
            {
                dyn::Detector det;
                det.prepare(SR);
                det.setMode(mode);
                det.setHighPass(hp);
                bool ok = true;
                for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                   -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, 0.5f, 0.0f })
                    for (int i = 0; i < 64; ++i)
                    {
                        const float v = det.level(bad, i % 2 ? bad : 0.1f);
                        ok = ok && std::isfinite(v) && v >= 0.0f && v <= dyn::Detector::MAX_LEVEL * 2.5f;
                    }
                expect(ok, "a level whatever comes in");
                float after = 1.0f;
                for (int i = 0; i < 96000; ++i) after = det.level(0.0f, 0.0f);
                expect(after < 1.0e-3f, "and it comes back down: " + juce::String(after));
            }
    }

    void testGainComputer()
    {
        beginTest("GainComputer: threshold, ratio, knee");
        dyn::GainComputer hard;
        hard.set(-20.0f, 4.0f, 0.0f);
        expectEquals(hard.gain(dsp::dbToLinear(-30.0f)), 1.0f);
        expectWithinAbsoluteError(dB(hard.gain(1.0f)), -15.0f, 0.01f);          // 20 dB over, 4:1
        expectWithinAbsoluteError(dB(hard.gain(dsp::dbToLinear(-10.0f))), -7.5f, 0.01f);

        dyn::GainComputer soft;
        soft.set(-20.0f, 4.0f, 12.0f);
        expectWithinAbsoluteError(dB(soft.gain(dsp::dbToLinear(-20.0f))), -1.125f, 0.01f);   // (1/4 - 1) 6^2 / 24
        expectWithinAbsoluteError(dB(soft.gain(dsp::dbToLinear(-26.0f))), 0.0f, 0.01f);
        expectWithinAbsoluteError(dB(soft.gain(dsp::dbToLinear(-14.0f))), -4.5f, 0.02f);     // the knee's end, on the line
        float last = 1.0f;
        bool falls = true;
        for (float level = -40.0f; level <= 0.0f; level += 0.25f)
        {
            const float g = soft.gain(dsp::dbToLinear(level));
            falls = falls && g <= last + 1.0e-6f;
            last = g;
        }
        expect(falls, "more level never asks for more gain");

        dyn::GainComputer unity;
        unity.set(-40.0f, 1.0f, 6.0f);
        expect(unity.idle());
    }

    // ---------------------------------------------------------------------------------------
    // The limiter as it was before phase E3, kept here as the reference for its arithmetic.
    struct OldLimiter
    {
        static constexpr int MAX_DELAY = 1024;
        float ceilingLinear = 0.966f, releaseCoeff = 0.0f, currentGain = 1.0f;
        int lookaheadSamples = 132, writePos = 0;
        std::array<float, MAX_DELAY> delayL {}, delayR {}, gains;

        OldLimiter() { gains.fill(1.0f); }
        void set(double sr, float ceilingDb, float lookaheadMs, float releaseMs)
        {
            ceilingLinear = std::pow(10.0f, ceilingDb / 20.0f);
            lookaheadSamples = std::clamp((int) (lookaheadMs * 0.001f * (float) sr), 1, MAX_DELAY - 1);
            releaseCoeff = std::exp(-1.0f / (float) (releaseMs * 0.001 * sr));
        }
        void process(float& left, float& right)
        {
            delayL[(size_t) writePos] = left;
            delayR[(size_t) writePos] = right;
            float peak = std::max(std::abs(left), std::abs(right));
            float neededGain = 1.0f;
            if (peak > ceilingLinear)
                neededGain = ceilingLinear / peak;
            gains[(size_t) writePos] = neededGain;
            float minGain = 1.0f;
            for (int i = 0; i < lookaheadSamples; ++i)
            {
                int idx = (writePos - i + MAX_DELAY) % MAX_DELAY;
                minGain = std::min(minGain, gains[(size_t) idx]);
            }
            if (minGain < currentGain)
                currentGain = minGain;
            else
                currentGain = releaseCoeff * currentGain + (1.0f - releaseCoeff) * minGain;
            currentGain = std::min(currentGain, 1.0f);
            int readPos = (writePos - lookaheadSamples + MAX_DELAY) % MAX_DELAY;
            left = delayL[(size_t) readPos] * currentGain;
            right = delayR[(size_t) readPos] * currentGain;
            left = std::clamp(left, -ceilingLinear, ceilingLinear);
            right = std::clamp(right, -ceilingLinear, ceilingLinear);
            writePos = (writePos + 1) % MAX_DELAY;
        }
    };

    void testLimiterIsWhatItWas()
    {
        beginTest("Limiter: on the shared core it gives the samples it gave before, bit for bit");
        for (double sr : { 22050.0, 44100.0, 48000.0, 192000.0 })
            for (float lookMs : { 1.0f, 3.0f, 5.0f })
                for (float ceilDb : { -0.3f, -3.0f, 0.0f })
                {
                    Limiter now;
                    now.setCeiling(ceilDb);
                    now.setLookahead(lookMs);
                    now.setRelease(120.0f);
                    now.prepare(sr, BLOCK);
                    OldLimiter old;
                    old.set(sr, ceilDb, lookMs, 120.0f);

                    // loud bursts over a quiet bed, a few single spikes, a stretch of silence
                    const int n = 20000;
                    Buf l = noise(0.3f, n, 91), r = noise(0.3f, n, 17);
                    for (int i = 0; i < n; ++i)
                    {
                        const float swell = (i / 1500) % 3 == 1 ? 6.0f : 1.0f;
                        l[(size_t) i] *= swell;
                        r[(size_t) i] *= (i / 2100) % 2 == 1 ? 4.0f : 1.0f;
                        if (i % 3331 == 0) l[(size_t) i] = 30.0f;
                        if (i > 15000 && i < 17000) l[(size_t) i] = r[(size_t) i] = 0.0f;
                    }
                    Buf wantL = l, wantR = r;
                    for (int i = 0; i < n; ++i) old.process(wantL[(size_t) i], wantR[(size_t) i]);
                    now.processStereo(l.data(), r.data(), n / 2);
                    for (int i = n / 2; i < n; ++i) now.process(l[(size_t) i], r[(size_t) i]);
                    const bool same = std::memcmp(l.data(), wantL.data(), sizeof(float) * (size_t) n) == 0
                                   && std::memcmp(r.data(), wantR.data(), sizeof(float) * (size_t) n) == 0;
                    expect(same, "at " + juce::String(sr, 0) + " Hz, LOOK " + juce::String(lookMs) + ", CEIL " + juce::String(ceilDb));
                    expect(now.getGainReductionDb() <= 0.0f);
                }

        beginTest("Limiter: LOOK turned under sound, still the old samples");
        {
            Limiter now;
            now.setCeiling(-0.3f);      // (as the master section sets it: until then the class holds 0.966, not 10^(-0.3/20))
            now.prepare(SR, BLOCK);
            OldLimiter old;
            old.set(SR, -0.3f, 3.0f, 200.0f);
            Buf l = noise(2.0f, 12000, 3), want = l, r = l, wantR = l;
            const float looks[] = { 3.0f, 5.0f, 1.0f, 4.2f, 1.7f, 5.0f };
            // (A cell is looked at every CellEffect::CONTROL samples, counted from reset(): the knob is
            // turned where a look falls, and the samples from there on are the old ones.)
            for (int i = 0; i < 12000; ++i)
            {
                if (i % 2048 == 0)
                {
                    now.setLookahead(looks[i / 2048]);
                    old.set(SR, -0.3f, looks[i / 2048], 200.0f);
                }
                now.process(l[(size_t) i], r[(size_t) i]);
                old.process(want[(size_t) i], wantR[(size_t) i]);
            }
            expect(std::memcmp(l.data(), want.data(), sizeof(float) * l.size()) == 0);
        }

        beginTest("Limiter: nothing passes the ceiling; CEIL and REL move with modulateCell");
        {
            Limiter lim;
            lim.setCeiling(-1.0f);
            lim.prepare(SR, BLOCK);
            const Buf loud = noise(4.0f, 24000, 8);
            Buf out = run(lim, loud);
            float top = 0.0f;
            for (float v : out) top = std::max(top, std::abs(v));
            expect(top <= dsp::dbToLinear(-1.0f) + 1.0e-6f, "peak " + juce::String(top));

            lim.modulateCell(0, -2.0f / 3.0f);          // two thirds of a 3 dB range down: -3 dB
            out = run(lim, loud);
            top = 0.0f;
            for (float v : out) top = std::max(top, std::abs(v));
            // (the loudest sample leaves the delay one sample after the window that held it down, so
            // the peak sits a hair under the ceiling, never over)
            expect(dB(top) <= -3.0f + 0.001f && dB(top) > -3.3f, "CEIL modulated down to -3 dB: " + juce::String(dB(top)));
            expectEquals(lim.getParameter(0), -1.0f, "the knob has not moved");
            lim.modulateCell(0, 0.0f);
            out = run(lim, loud);
            top = 0.0f;
            for (float v : out) top = std::max(top, std::abs(v));
            expect(dB(top) <= -1.0f + 0.001f && dB(top) > -1.3f, "and back at -1 dB: " + juce::String(dB(top)));

            // REL: how far the gain is back 50 ms after a loud stretch
            auto recovered = [&](float offset)
            {
                Limiter l2;
                l2.setRelease(500.0f);
                l2.prepare(SR, BLOCK);
                l2.modulateCell(1, offset);
                Buf in = sine(500.0, 0.2f, 24000);
                for (int i = 0; i < 9600; ++i) in[(size_t) i] *= 20.0f;
                const Buf o = run(l2, in);
                return rms(o, 9600 + 2400, 9600 + 3360) / (0.2 * 0.7071);
            };
            expect(recovered(-1.0f) > recovered(0.0f) * 1.5, "REL modulated to its short end comes back sooner: "
                                                             + juce::String(recovered(-1.0f)) + " against " + juce::String(recovered(0.0f)));
        }
    }

    // ---------------------------------------------------------------------------------------
    void testCompressor()
    {
        beginTest("Compressor: its eight cells, defaults seeded, values round-trip; DUCK's likewise");
        {
            const char* cmp[8] = { "THR", "RATI", "ATK", "MAKE", "KEY", "REL", "KNEE", "SCHP" };
            const char* duck[8] = { "SRC", "THR", "RATI", "REL", "ATK", "HOLD", "KNEE", "SCHP" };
            Compressor c;
            SidechainInsert d;
            auto check = [&](EffectBase& fx, const char* const* labels)
            {
                for (int i = 0; i < 8; ++i)
                {
                    const ParamSpec spec = i < 4 ? fx.getParamSpec(i) : fx.getExtraSpec(i - 4);
                    expect(spec.isValid() && juce::String(spec.label) == labels[i], juce::String(labels[i]));
                    expectEquals(spec.id, i % 4);
                    auto get = [&] { return i < 4 ? fx.getParameter(i) : fx.getExtra(i - 4); };
                    auto set = [&](float v) { if (i < 4) fx.setParameter(i, v); else fx.setExtra(i - 4, v); };
                    expectWithinAbsoluteError(get(), spec.defaultVal, 1.0e-5f, juce::String(labels[i]) + " starts at its default");
                    set(spec.maxVal + 1000.0f);
                    expectWithinAbsoluteError(get(), spec.maxVal, 1.0e-4f);
                    set(spec.minVal - 1000.0f);
                    expectWithinAbsoluteError(get(), spec.minVal, 1.0e-4f);
                    set(std::numeric_limits<float>::quiet_NaN());
                    expectWithinAbsoluteError(get(), spec.minVal, 1.0e-4f, "what is not a number is not taken");
                    set(spec.defaultVal);
                }
                expect(! fx.getParamSpec(4).isValid() && ! fx.getExtraSpec(4).isValid());
            };
            check(c, cmp);
            check(d, duck);
            expectEquals(d.getExtra(SidechainInsert::Attack), 2.0f);
            expectEquals(d.getExtra(SidechainInsert::Knee), 6.0f);
            expectEquals(d.keySource(), (int) EffectBase::KEY_SELF);
        }

        beginTest("Compressor: at 1:1 the sound passes bit for bit, whatever the threshold (the master compressor)");
        for (float thr : { 0.0f, -20.0f, -60.0f })
        {
            Compressor comp;
            comp.setThreshold(thr);
            comp.setRatio(1.0f);
            comp.prepare(SR, BLOCK);
            const Buf in = noise(1.5f, 6000, 44);
            Buf l = in, r = noise(1.5f, 6000, 45);
            const Buf rIn = r;
            for (int i = 0; i < 3000; ++i) comp.process(l[(size_t) i], r[(size_t) i]);
            comp.processStereo(l.data() + 3000, r.data() + 3000, 3000);
            expect(l == in && r == rIn, "THR " + juce::String(thr));
            expectEquals(comp.getGainReductionDb(), 0.0f);
        }

        beginTest("Compressor: a steady tone over the threshold comes down by the ratio; MAKE brings it back");
        {
            auto level = [&](float thr, float ratio, float makeDb)
            {
                Compressor comp;
                comp.setThreshold(thr);
                comp.setRatio(ratio);
                comp.setKneeWidth(0.0f);
                comp.setAttack(1.0f);
                comp.setRelease(500.0f);
                comp.setMakeupGain(makeDb);
                comp.prepare(SR, BLOCK);
                const Buf out = run(comp, sine(1000.0, 1.0f, 48000));
                return dB(rms(out, 24000, 48000) / 0.7071);
            };
            // 20 dB over at 4:1 is 15 dB down at the peaks; between them the gain creeps back a little
            const float down = level(-20.0f, 4.0f, 0.0f);
            expect(down < -13.5f && down > -15.2f, juce::String(down) + " dB");
            expect(level(-20.0f, 2.0f, 0.0f) > down + 4.0f, "a lower ratio takes less");
            expect(level(-40.0f, 4.0f, 0.0f) < down - 12.0f, "a lower threshold takes more");
            expectWithinAbsoluteError(level(-20.0f, 4.0f, 12.0f), down + 12.0f, 0.1f);
            expectWithinAbsoluteError(level(0.0f, 4.0f, 0.0f), 0.0f, 0.05f);
        }

        beginTest("Compressor: REL, KNEE, SCHP, HOLD, RMS and WET each do what they say");
        {
            // REL: a loud second, then a quiet tone: how far back it is 100 ms later
            auto recovered = [&](float releaseMs, float holdMs)
            {
                Compressor comp;
                comp.setThreshold(-30.0f);
                comp.setRatio(20.0f);
                comp.setAttack(1.0f);
                comp.setExtra(1, releaseMs);
                comp.setHold(holdMs);
                comp.prepare(SR, BLOCK);
                Buf in = sine(500.0, 0.01f, 48000);
                for (int i = 0; i < 24000; ++i) in[(size_t) i] *= 90.0f;
                const Buf out = run(comp, in);
                return rms(out, 24000 + 4800, 24000 + 5760) / (0.01 * 0.7071);
            };
            expect(recovered(20.0f, 0.0f) > 0.9, "short release: back within 100 ms");
            expect(recovered(1000.0f, 0.0f) < 0.5, "long release: still down");
            expect(recovered(20.0f, 300.0f) < 0.1, "HOLD keeps it down after the loud part has gone: " + juce::String(recovered(20.0f, 300.0f)));

            // KNEE: a tone exactly at the threshold is untouched with a hard knee, bent with a soft one
            auto atThreshold = [&](float kneeDb)
            {
                Compressor comp;
                comp.setThreshold(-12.0f);
                comp.setRatio(8.0f);
                comp.setAttack(1.0f);
                comp.setRelease(1000.0f);
                comp.setExtra(2, kneeDb);
                comp.prepare(SR, BLOCK);
                const float amp = dsp::dbToLinear(-12.0f);
                const Buf out = run(comp, sine(1000.0, amp, 24000));
                return dB(rms(out, 12000, 24000) / (amp * 0.7071));
            };
            expectWithinAbsoluteError(atThreshold(0.0f), 0.0f, 0.05f);
            expect(atThreshold(12.0f) < -0.8f, "soft knee: " + juce::String(atThreshold(12.0f)) + " dB at the threshold");

            // SCHP: a loud bass note stops working the detector
            auto bassDown = [&](float hpHz)
            {
                Compressor comp;
                comp.setThreshold(-30.0f);
                comp.setRatio(10.0f);
                comp.setExtra(3, hpHz);
                comp.prepare(SR, BLOCK);
                const Buf out = run(comp, sine(60.0, 0.8f, 48000));
                return dB(rms(out, 24000, 48000) / (0.8 * 0.7071));
            };
            expect(bassDown(20.0f) < -15.0f, "no high-pass: " + juce::String(bassDown(20.0f)));
            expect(bassDown(2000.0f) > -1.0f, "under a 2 kHz high-pass the bass passes: " + juce::String(bassDown(2000.0f)));

            // RMS reads a sine 3 dB lower than peak does, so it takes less off
            auto byMode = [&](Compressor::DetectionMode mode)
            {
                Compressor comp;
                comp.setThreshold(-20.0f);
                comp.setRatio(20.0f);
                comp.setKneeWidth(0.0f);
                comp.setAttack(1.0f);
                comp.setRelease(1000.0f);
                comp.setDetectionMode(mode);
                comp.prepare(SR, BLOCK);
                const Buf out = run(comp, sine(1000.0, 1.0f, 48000));
                return dB(rms(out, 24000, 48000) / 0.7071);
            };
            const float peakMode = byMode(Compressor::DetectionMode::Peak), rmsMode = byMode(Compressor::DetectionMode::RMS);
            expect(rmsMode > peakMode + 2.0f && rmsMode < peakMode + 4.0f, juce::String(peakMode) + " peak, " + juce::String(rmsMode) + " RMS");

            // WET: half the dry sound beside half the compressed one
            Compressor full, half;
            for (auto* comp : { &full, &half })
            {
                comp->setThreshold(-30.0f);
                comp->setRatio(20.0f);
                comp->prepare(SR, BLOCK);
            }
            half.setMix(0.5f);
            const Buf in = sine(700.0, 0.9f, 24000);
            const Buf wet = run(full, in), mixed = run(half, in);
            float worst = 0.0f;
            for (size_t i = 0; i < in.size(); ++i)
                worst = std::max(worst, std::abs(mixed[i] - (0.5f * in[i] + 0.5f * wet[i])));
            expect(worst < 1.0e-5f, "dry (1 - mix) + wet mix: " + juce::String(worst));
        }
    }

    // ---------------------------------------------------------------------------------------
    void testKeying()
    {
        const int n = 48000;
        const Buf quiet = sine(300.0, 0.05f, n);
        const Buf loudKey = noise(0.9f, n, 7);
        const double quietRms = 0.05 * 0.7071;

        beginTest("KEY: no effect asks for the track's input until KEY (or DUCK's SRC) says IN");
        {
            Compressor c; TransientShaper t; Wah w; SidechainInsert d;
            expect(! c.wantsInput() && ! t.wantsInput() && ! w.wantsInput() && ! d.wantsInput());
            c.setExtra(0, (float) dyn::KEY_CHOICE_IN); t.setExtra(0, (float) dyn::KEY_CHOICE_IN); w.setExtra(Wah::KEY_EXTRA, (float) dyn::KEY_CHOICE_IN);
            d.setParameter(SidechainInsert::Source, (float) dyn::KEY_CHOICE_IN);
            expect(c.wantsInput() && t.wantsInput() && w.wantsInput() && d.wantsInput());
            expectEquals(c.getExtra(0), (float) dyn::KEY_CHOICE_IN);
            expectEquals(t.getExtra(0), (float) dyn::KEY_CHOICE_IN);
            expectEquals(w.getExtra(Wah::KEY_EXTRA), (float) dyn::KEY_CHOICE_IN);
            for (auto* fx : std::initializer_list<EffectBase*> { &c, &t, &w, &d })
                expectEquals(fx->keySource(), (int) EffectBase::KEY_IN, "IN is not one of the tracks");
            // (E5: one key choice. Every KEY cell and DUCK's SRC are the same list, and a track by
            // number is a choice of each of them.)
            d.setParameter(SidechainInsert::Source, 3.0f);
            c.setExtra(0, 3.0f); t.setExtra(0, 16.0f); w.setExtra(Wah::KEY_EXTRA, 1.0f);
            expect(! d.wantsInput() && ! c.wantsInput() && ! t.wantsInput() && ! w.wantsInput(), "a track's sound is not the track's input");
            expectEquals(d.keySource(), 2);
            expectEquals(c.keySource(), 2);
            expectEquals(t.keySource(), 15);
            expectEquals(w.keySource(), 0);
            const ParamSpec src = d.getParamSpec(0);
            expect(juce::String(src.label) == "SRC");
            for (auto* fx : std::initializer_list<EffectBase*> { &c, &t, &w })
            {
                const ParamSpec key = fx == &w ? fx->getExtraSpec(Wah::KEY_EXTRA) : fx->getExtraSpec(0);
                expect(juce::String(key.label) == "KEY" && key.kind == ParamKind::Choice && key.numChoices == 18);
                expect(key.choices == src.choices, "KEY and SRC are one list");
            }
            expectEquals(src.numChoices, 18);
            expect(juce::String(src.choices[0]) == "SELF" && juce::String(src.choices[1]) == "T1");
            expect(juce::String(src.choices[17]) == "IN" && juce::String(src.choices[16]) == "T16", "IN is the end of the list");
        }

        auto compressor = []
        {
            auto c = std::make_unique<Compressor>();
            c->setThreshold(-20.0f);
            c->setRatio(10.0f);
            c->prepare(SR, BLOCK);
            return c;
        };

        beginTest("KEY IN on the compressor: a quiet track goes down under a loud input, and only then");
        {
            auto self = compressor();
            const Buf alone = run(*self, quiet, &loudKey);
            expectWithinAbsoluteError((float) rms(alone, n / 2, n), (float) quietRms, (float) quietRms * 0.01f, "SELF: untouched");
            expect(! self->keyConnected());

            auto keyed = compressor();
            keyed->setExtra(0, (float) dyn::KEY_CHOICE_IN);
            const Buf ducked = run(*keyed, quiet, &loudKey);
            expect(rms(ducked, n / 2, n) < quietRms * 0.35, "IN: down by " + juce::String(dB(rms(ducked, n / 2, n) / quietRms)) + " dB");
            expect(keyed->keyConnected());
            expect(keyed->getGainReductionDb() < -8.0f);

            // a quiet input does nothing
            auto calm = compressor();
            calm->setExtra(0, (float) dyn::KEY_CHOICE_IN);
            const Buf quietKey = noise(0.01f, n, 9);
            const Buf untouched = run(*calm, quiet, &quietKey);
            expectWithinAbsoluteError((float) rms(untouched, n / 2, n), (float) quietRms, (float) quietRms * 0.01f);

            // no input at all (no source chosen, a PERF pad): as SELF, sample for sample
            auto none = compressor();
            none->setExtra(0, (float) dyn::KEY_CHOICE_IN);
            const Buf loud = sine(300.0, 0.9f, n);
            auto reference = compressor();
            expect(run(*none, loud) == run(*reference, loud), "KEY IN with nothing connected is SELF");
            expect(! none->keyConnected(), "and says so (for the screen's notice)");

            // the key is for one block: the next block without one keys itself again
            auto once = compressor();
            once->setExtra(0, (float) dyn::KEY_CHOICE_IN);
            once->setRelease(20.0f);
            Buf out = run(*once, Buf(quiet.begin(), quiet.begin() + 24000), &loudKey);
            expect(rms(out, 12000, 24000) < quietRms * 0.35);
            out = run(*once, quiet);
            expectWithinAbsoluteError((float) rms(out, 24000, 48000), (float) quietRms, (float) quietRms * 0.01f, "the key gone: back up");

            // a key shorter than the block keys its own length only; SELF ignores a key it is handed
            auto shortKey = compressor();
            shortKey->setExtra(0, (float) dyn::KEY_CHOICE_IN);
            shortKey->setAttack(0.1f);
            shortKey->setRelease(10.0f);
            Buf l(quiet.begin(), quiet.begin() + 4096), r = l;
            const Buf silentKey(4096, 0.0f);
            // (the track itself is loud here, the key silent: where the key reaches, nothing is taken off)
            for (auto& v : l) v *= 18.0f;
            r = l;
            const Buf loudIn = l;
            shortKey->setInput(silentKey.data(), silentKey.data(), 2048);
            shortKey->processStereo(l.data(), r.data(), 4096);
            expect(std::equal(l.begin(), l.begin() + 2048, loudIn.begin()), "keyed by silence: untouched");
            expect(rms(l, 3072, 4096) < rms(loudIn, 3072, 4096) * 0.5, "past the key's end it keys itself");
        }

        beginTest("KEY IN: a key that is not a number, infinite or huge leaves the sound finite");
        for (int which = 0; which < 3; ++which)
        {
            std::unique_ptr<EffectBase> fx;
            if (which == 0) { auto c = compressor(); c->setExtra(0, (float) dyn::KEY_CHOICE_IN); fx = std::move(c); }
            if (which == 1) { auto t = std::make_unique<TransientShaper>(); t->setExtra(0, (float) dyn::KEY_CHOICE_IN); t->setParameter(0, 1.0f); t->setParameter(1, 1.0f); t->prepare(SR, BLOCK); fx = std::move(t); }
            if (which == 2) { auto w = std::make_unique<Wah>(); w->setExtra(Wah::KEY_EXTRA, (float) dyn::KEY_CHOICE_IN); w->setParameter(Wah::Sens, 1.0f); w->prepare(SR, BLOCK); fx = std::move(w); }
            Buf bad((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
                bad[(size_t) i] = i % 7 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                : i % 7 == 1 ? std::numeric_limits<float>::infinity()
                                : i % 7 == 2 ? -1.0e30f : (i % 7 == 3 ? 1.0e30f : 0.3f);
            Buf out = run(*fx, quiet, &bad);
            expect(allFinite(out), "effect " + juce::String(which));
            float top = 0.0f;
            for (float v : out) top = std::max(top, std::abs(v));
            expect(top < 2.0f, "and bounded: " + juce::String(top));
            // and it recovers once the key is sane again
            const Buf calm((size_t) n, 0.0f);
            out = run(*fx, quiet, &calm);
            expect(allFinite(out) && rms(out, n / 2, n) > 1.0e-4, "it comes back");
        }

        beginTest("KEY IN on the transient shaper: a steady pad takes on the attacks of the input");
        {
            const Buf pad = sine(220.0, 0.2f, n);
            const Buf hits = bursts(1500.0, 0.8f, n, 12000, 3000);        // four hits a second
            auto shaper = [&](float atk, float sus, bool keyIn)
            {
                auto t = std::make_unique<TransientShaper>();
                t->setParameter(TransientShaper::Attack, atk);
                t->setParameter(TransientShaper::Sustain, sus);
                t->setExtra(0, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                t->prepare(SR, BLOCK);
                return t;
            };
            const double padRms = 0.2 * 0.7071;
            // by itself a steady tone has no attack and no tail
            auto self = shaper(1.0f, 1.0f, false);
            const Buf alone = run(*self, pad, &hits);
            expectWithinAbsoluteError(dB(rms(alone, n / 2, n) / padRms), 0.0f, 0.1f);

            auto up = shaper(1.0f, 0.0f, true);
            const Buf swelled = run(*up, pad, &hits);
            const float atHit = dB(rms(swelled, 24000, 24000 + 480) / padRms);
            const float between = dB(rms(swelled, 24000 + 9000, 24000 + 11000) / padRms);
            expect(atHit > 6.0f, "the pad jumps with each hit: +" + juce::String(atHit) + " dB");
            expectWithinAbsoluteError(between, 0.0f, 0.3f, "and is itself between them");

            auto down = shaper(-1.0f, 0.0f, true);
            const Buf dipped = run(*down, pad, &hits);
            expect(dB(rms(dipped, 24000, 24000 + 480) / padRms) < -6.0f, "ATK below 0: it drops under each hit");

            auto tail = shaper(0.0f, 1.0f, true);
            const Buf tailed = run(*tail, pad, &hits);
            expect(dB(rms(tailed, 24000 + 3000 + 1200, 24000 + 3000 + 2400) / padRms) > 3.0f, "SUS: it swells as each hit dies away");
            expectWithinAbsoluteError(dB(rms(tailed, 24000 + 1500, 24000 + 2900) / padRms), 0.0f, 0.5f, "not while the hit sounds");
        }

        beginTest("KEY IN on the wah: the input's level opens the filter, the track's own no longer does");
        {
            auto wah = [&](bool keyIn)
            {
                auto w = std::make_unique<Wah>();
                w->setParameter(Wah::Freq, 300.0f);
                w->setParameter(Wah::Sens, 1.0f);
                w->setExtra(Wah::KEY_EXTRA, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                w->prepare(SR, BLOCK);
                return w;
            };
            const Buf soft = sine(300.0, 0.002f, 24000), hard = sine(300.0, 0.5f, 24000);
            const Buf loud = sine(900.0, 0.5f, 24000), silent(24000, 0.0f);

            auto self = wah(false);
            run(*self, soft, &loud);
            expect(self->getCurrentFrequency() < 330.0f, "SELF, a quiet track: at rest whatever the input does");

            auto keyed = wah(true);
            run(*keyed, soft, &loud);
            expect(keyed->getCurrentFrequency() > 3000.0f, "IN, a loud input: open, " + juce::String(keyed->getCurrentFrequency()) + " Hz");
            expect(keyed->keyConnected());

            auto shut = wah(true);
            run(*shut, hard, &silent);
            expect(shut->getCurrentFrequency() < 330.0f, "IN, a silent input: shut though the track is loud");

            auto unplugged = wah(true);
            auto reference = wah(false);
            expect(run(*unplugged, hard) == run(*reference, hard), "IN with nothing connected is SELF");
            expect(! unplugged->keyConnected());
        }

        beginTest("DUCK: SRC IN and SRC T2 both take what they are handed through setInput (which sound that is, is the KeyFeed's to say)");
        {
            auto duck = [&](float source)
            {
                auto d = std::make_unique<SidechainInsert>();
                d->setParameter(SidechainInsert::Source, source);
                d->setParameter(SidechainInsert::Threshold, -20.0f);
                d->setParameter(SidechainInsert::Ratio, 10.0f);
                d->prepare(SR, BLOCK);
                return d;
            };
            auto in = duck((float) dyn::KEY_CHOICE_IN);
            expect(rms(run(*in, quiet, &loudKey), n / 2, n) < quietRms * 0.35, "IN ducks");

            // the KeyFeed hands SRC IN the track's input and SRC T2 the second track's sound, and
            // neither the other's
            const Buf silentKey((size_t) n, 0.0f);
            auto fed = [&](float source, const Buf& input, const Buf& trackTwo)
            {
                auto d = duck(source);
                return run(*d, quiet, nullptr, BLOCK, [&](int b)
                {
                    const int at = b * BLOCK, count = std::min(BLOCK, n - at);
                    const float* taps[2] = { silentKey.data() + at, trackTwo.data() + at };
                    KeyFeed feed;
                    feed.inLeft = feed.inRight = input.data() + at;
                    feed.inFrames = count;
                    feed.tapLeft = feed.tapRight = taps;
                    feed.taps = 2;
                    feed.frames = count;
                    feed.self = 5;
                    feed.hand(*d);
                });
            };
            expect(rms(fed((float) dyn::KEY_CHOICE_IN, loudKey, silentKey), n / 2, n) < quietRms * 0.35, "IN: the input ducks");
            expectWithinAbsoluteError((float) rms(fed((float) dyn::KEY_CHOICE_IN, silentKey, loudKey), n / 2, n), (float) quietRms, (float) quietRms * 0.01f,
                                      "IN: a track's sound is not used");
            expect(rms(fed(2.0f, silentKey, loudKey), n / 2, n) < quietRms * 0.35, "T2: the second track ducks");
            expectWithinAbsoluteError((float) rms(fed(2.0f, loudKey, silentKey), n / 2, n), (float) quietRms, (float) quietRms * 0.01f,
                                      "T2: the input is not used");
            // a track that is not there (T9 of two), and the effect's own track by number: SELF
            auto own = duck(6.0f);      // (the feed says the effect is on track 6)
            auto reference = duck(0.0f);
            const Buf hot = sine(300.0, 0.9f, n);
            const Buf asSelf = run(*reference, hot);
            auto handed = [&](SidechainInsert& effect) { auto* d = &effect; return [&, d](int b)
            {
                const int at = b * BLOCK;
                const float* taps[2] = { loudKey.data() + at, loudKey.data() + at };
                KeyFeed feed;
                feed.tapLeft = feed.tapRight = taps;
                feed.taps = 2;
                feed.frames = std::min(BLOCK, n - at);
                feed.self = 5;
                feed.hand(*d);
            }; };
            expect(run(*own, hot, nullptr, BLOCK, handed(*own)) == asSelf, "its own track by number is SELF, sample for sample");
            auto missing = duck(9.0f);
            expect(run(*missing, hot, nullptr, BLOCK, handed(*missing)) == asSelf, "a track that is not there is SELF");
            expect(! missing->keyConnected());

            // HOLD: the shape of the pump
            auto pump = [&](float holdMs)
            {
                auto d = duck((float) dyn::KEY_CHOICE_IN);
                d->setParameter(SidechainInsert::Release, 20.0f);
                d->setExtra(SidechainInsert::Hold, holdMs);
                Buf key = loudKey;
                for (int i = 24000; i < n; ++i) key[(size_t) i] = 0.0f;
                const Buf o = run(*d, quiet, &key);
                return rms(o, 24000 + 4800, 24000 + 5760) / quietRms;
            };
            expect(pump(0.0f) > 0.9 && pump(400.0f) < 0.4, "HOLD: " + juce::String(pump(0.0f)) + " without, " + juce::String(pump(400.0f)) + " with");
        }
    }

    // The whole way: a track with a compressor in its first insert slot, KEY set through the
    // slot's extra-cell id as TRK FX page FX2 sets it, the track's IN source another track.
    void testKeyThroughTheAudioEngine()
    {
        beginTest("KEY IN through the AudioEngine: the track's IN source keys its compressor, wah and transient shaper");
        auto level = [&](EffectType type, bool keyIn, bool source, const std::function<void(AudioEngine&)>& setUp, float own = 0.05f)
        {
            TrackInputTests::Rig rig;
            auto* quietTrack = rig.add<TrackInputTests::ToneEngine>(0);
            auto* loudTrack = rig.add<TrackInputTests::ToneEngine>(1);
            quietTrack->left = quietTrack->right = own;
            loudTrack->left = loudTrack->right = 0.9f;
            rig.audio.setTrackParameter(1, MixParam::VOLUME, 0.0f);         // heard only by who listens to it
            rig.audio.addInsertEffect(0, type);
            if (source) rig.audio.setTrackParameter(0, TrackInParam::SRC, 2.0f);
            setUp(rig.audio);
            auto* fx = rig.audio.getInsertChain(0)->getEffect(0);
            int keyCell = -1;
            for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
                if (juce::String(fx->getExtraSpec(i).label) == "KEY") keyCell = i;
            expect(keyCell >= 0);
            const int keyId = AudioEngine::INSERT_EXTRA_BASE + keyCell;
            rig.audio.setTrackParameter(0, keyId, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
            expectEquals(rig.audio.getTrackParameter(0, keyId), keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
            rig.blocks(400);
            double sum = 0.0;
            for (int b = 0; b < 50; ++b)
            {
                rig.blocks(1);
                for (float v : rig.left) sum += (double) v * v;
            }
            expectEquals((int) rig.audio.getNonFiniteBlocks(), 0);
            return std::sqrt(sum / (50.0 * rig.block));
        };
        const int p = AudioEngine::EFFECT_PARAM_BASE;
        auto cmp = [p](AudioEngine& a) { a.setTrackParameter(0, p + 0, -20.0f); a.setTrackParameter(0, p + 1, 10.0f); };
        const double self = level(EffectType::CompressorEffect, false, true, cmp);
        const double keyed = level(EffectType::CompressorEffect, true, true, cmp);
        const double noSource = level(EffectType::CompressorEffect, true, false, cmp);
        expect(self > 1.0e-3, "the track sounds: " + juce::String(self));
        expect(keyed < self * 0.4, "CMP KEY IN: " + juce::String(dB(keyed / self)) + " dB under the other track");
        expectWithinAbsoluteError((float) noSource, (float) self, (float) self * 0.001f, "IN with no source chosen: as SELF");

        // the wah's band-pass passes nothing of a steady level; what it lets through of the level's
        // onset is not a measure. Its centre is: read it from the effect.
        auto centre = [&](bool keyIn)
        {
            TrackInputTests::Rig rig;
            auto* quietTrack = rig.add<TrackInputTests::ToneEngine>(0);
            auto* loudTrack = rig.add<TrackInputTests::ToneEngine>(1);
            quietTrack->left = quietTrack->right = 0.001f;
            loudTrack->left = loudTrack->right = 0.9f;
            rig.audio.addInsertEffect(0, EffectType::WahEffect);
            rig.audio.setTrackParameter(0, TrackInParam::SRC, 2.0f);
            rig.audio.setTrackParameter(0, p + Wah::Sens, 1.0f);
            rig.audio.setTrackParameter(0, AudioEngine::INSERT_EXTRA_BASE + Wah::KEY_EXTRA, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
            rig.blocks(300);
            return dynamic_cast<Wah*>(rig.audio.getInsertChain(0)->getEffect(0))->getCurrentFrequency();
        };
        expect(centre(false) < 450.0f && centre(true) > 3000.0f, "WAH KEY IN: " + juce::String(centre(false)) + " Hz by itself, "
                                                                 + juce::String(centre(true)) + " Hz under the other track");

        // DUCK's SRC IN the same way (its first cell, not an extra)
        auto duck = [&](float source)
        {
            TrackInputTests::Rig rig;
            auto* quietTrack = rig.add<TrackInputTests::ToneEngine>(0);
            auto* loudTrack = rig.add<TrackInputTests::ToneEngine>(1);
            quietTrack->left = quietTrack->right = 0.05f;
            loudTrack->left = loudTrack->right = 0.9f;
            rig.audio.setTrackParameter(1, MixParam::VOLUME, 0.0f);
            rig.audio.addInsertEffect(0, EffectType::SidechainEffect);
            rig.audio.setTrackParameter(0, TrackInParam::SRC, 2.0f);
            rig.audio.setTrackParameter(0, p + 0, source);
            rig.audio.setTrackParameter(0, p + 2, 10.0f);
            rig.blocks(400);
            double sum = 0.0;
            for (float v : rig.left) sum += (double) v * v;
            return std::sqrt(sum / rig.block);
        };
        const double duckSelf = duck(0.0f);
        expect(duck((float) dyn::KEY_CHOICE_IN) < duckSelf * 0.5, "DUCK SRC IN");
        expect(duck(2.0f) < duckSelf * 0.5, "DUCK SRC T2, as before");
    }

    // ---------------------------------------------------------------------------------------
    void testTransientShaper()
    {
        const int n = 48000, period = 12000, on = 6000;
        // a hit: a tone with a sharp start that dies away (21 ms to a third)
        auto hits = [&](float amp)
        {
            Buf x = sine(400.0, amp, n);
            for (int i = 0; i < n; ++i)
                x[(size_t) i] *= i % period < on ? std::exp(-(float) (i % period) / 1000.0f) : 0.0f;
            return x;
        };
        auto shaped = [&](float atk, float sus, float amp, float spd = 0.5f)
        {
            TransientShaper t;
            t.setParameter(TransientShaper::Attack, atk);
            t.setParameter(TransientShaper::Sustain, sus);
            t.setParameter(TransientShaper::Speed, spd);
            t.prepare(SR, BLOCK);
            return run(t, hits(amp));
        };
        // the third hit: its first 3 ms, and its tail from 60 to 110 ms
        auto head = [&](const Buf& x) { return rms(x, 2 * period, 2 * period + 144); };
        auto tail = [&](const Buf& x) { return rms(x, 2 * period + 2880, 2 * period + 5280); };

        beginTest("Transient shaper: at rest the sound passes untouched, sample for sample");
        {
            const Buf in = hits(0.5f);
            expect(shaped(0.0f, 0.0f, 0.5f) == in);
        }

        beginTest("Transient shaper: ATK works on the start of a hit, SUS on its tail, each both ways");
        {
            const Buf plain = hits(0.5f);
            const Buf sharp = shaped(1.0f, 0.0f, 0.5f), soft = shaped(-1.0f, 0.0f, 0.5f);
            const Buf longer = shaped(0.0f, 1.0f, 0.5f), shorter = shaped(0.0f, -1.0f, 0.5f);
            const float sharpDb = dB(head(sharp) / head(plain)), softDb = dB(head(soft) / head(plain));
            const float longDb = dB(tail(longer) / tail(plain)), shortDb = dB(tail(shorter) / tail(plain));
            logMessage("TRNS: ATK +1 " + juce::String(sharpDb, 1) + " dB on the first 3 ms, ATK -1 " + juce::String(softDb, 1)
                       + " dB; SUS +1 " + juce::String(longDb, 1) + " dB on the tail, SUS -1 " + juce::String(shortDb, 1) + " dB");
            expect(sharpDb > 6.0f && sharpDb < 12.5f, "sharper: " + juce::String(sharpDb));
            expect(softDb < -6.0f && softDb > -12.5f, "softer: " + juce::String(softDb));
            expectWithinAbsoluteError(sharpDb, -softDb, 1.0f, "the same in dB both ways");
            expect(longDb > 2.5f, "longer: " + juce::String(longDb));
            expect(shortDb < -2.5f, "shorter: " + juce::String(shortDb));
            expectWithinAbsoluteError(dB(tail(sharp) / tail(plain)), 0.0f, 0.5f, "ATK leaves the tail");
            expectWithinAbsoluteError(dB(head(longer) / head(plain)), 0.0f, 0.5f, "SUS leaves the start");
        }

        beginTest("Transient shaper: the same at -46 dB as at -6 dB (no threshold)");
        {
            const float loud = dB(head(shaped(1.0f, 0.0f, 0.5f)) / head(hits(0.5f)));
            const float quiet = dB(head(shaped(1.0f, 0.0f, 0.005f)) / head(hits(0.005f)));
            expectWithinAbsoluteError(quiet, loud, 0.5f);
            const float loudTail = dB(tail(shaped(0.0f, -1.0f, 0.5f)) / tail(hits(0.5f)));
            const float quietTail = dB(tail(shaped(0.0f, -1.0f, 0.005f)) / tail(hits(0.005f)));
            expectWithinAbsoluteError(quietTail, loudTail, 0.5f);
        }

        beginTest("Transient shaper: a steady tone is left alone from 50 Hz up; OUT is a level; WET mixes");
        for (double hz : { 50.0, 110.0, 1000.0, 8000.0 })
        {
            TransientShaper t;
            t.setParameter(TransientShaper::Attack, 1.0f);
            t.setParameter(TransientShaper::Sustain, 1.0f);
            t.setParameter(TransientShaper::Speed, 1.0f);
            t.prepare(SR, BLOCK);
            const Buf in = sine(hz, 0.5f, n);
            const Buf out = run(t, in);
            expectWithinAbsoluteError(dB(rms(out, n / 2, n) / rms(in, n / 2, n)), 0.0f, 0.15f, juce::String(hz) + " Hz");
        }
        {
            TransientShaper t;
            t.setParameter(TransientShaper::Output, 6.0f);
            t.prepare(SR, BLOCK);
            const Buf in = sine(500.0, 0.25f, 9600);
            expectWithinAbsoluteError(dB(rms(run(t, in), 4800, 9600) / rms(in, 4800, 9600)), 6.0f, 0.05f);

            TransientShaper full, half;
            for (auto* fx : { &full, &half })
            {
                fx->setParameter(TransientShaper::Attack, 1.0f);
                fx->setParameter(TransientShaper::Sustain, -1.0f);
                fx->prepare(SR, BLOCK);
            }
            half.setMix(0.5f);
            const Buf x = hits(0.5f), wet = run(full, x), mixed = run(half, x);
            float worst = 0.0f;
            for (size_t i = 0; i < x.size(); ++i)
                worst = std::max(worst, std::abs(mixed[i] - (0.5f * x[i] + 0.5f * wet[i])));
            expect(worst < 1.0e-5f, "dry (1 - mix) + wet mix: " + juce::String(worst));
        }

        beginTest("Transient shaper: ATK, SUS and OUT turned under a steady tone do not click");
        {
            TransientShaper t;
            t.prepare(SR, 64);
            const Buf in = sine(200.0, 0.5f, 48000);
            const float sineStep = 0.5f * (float) (TWO_PI * 200.0 / SR);
            const Buf out = run(t, in, nullptr, 64, [&](int b)
            {
                if (b == 100) t.setParameter(TransientShaper::Attack, 1.0f);
                if (b == 200) t.setParameter(TransientShaper::Sustain, -1.0f);
                if (b == 300) t.setParameter(TransientShaper::Output, 12.0f);
                if (b == 400) t.setParameter(TransientShaper::Output, -12.0f);
                if (b == 500) t.setParameter(TransientShaper::Speed, 1.0f);
                if (b == 600) t.setExtra(0, (float) dyn::KEY_CHOICE_IN);
            });
            // (at +12 dB the tone itself steps four times as far)
            expect(biggestStep(out, 0, 48000) < sineStep * 4.0f * 1.25f, "biggest step " + juce::String(biggestStep(out, 0, 48000))
                                                                         + ", the tone's own " + juce::String(sineStep));
        }
    }

    // ---------------------------------------------------------------------------------------
    // The gain of a stereo processor at one frequency, measured with a sine (RMS out / RMS in).
    static float gainAt(const std::function<void(float&, float&)>& process, double hz, double sr = SR)
    {
        const int n = (int) (sr * 0.25);
        const Buf in = sine(hz, 0.25f, n, sr);
        Buf out = in;
        for (auto& v : out) { float r = v; process(v, r); }
        return dB(rms(out, n / 2, n) / rms(in, n / 2, n));
    }

    void testEqBand()
    {
        beginTest("EqBand: peak, low shelf and high shelf give the set gain where they should, and none elsewhere");
        for (double sr : { 44100.0, 96000.0 })
        {
            auto band = [&](fxb::EqBand::Shape shape, float hz, float q, float db)
            {
                auto b = std::make_shared<fxb::EqBand>();
                b->setShape(shape);
                b->prepare(sr);
                b->set(hz, q, db);
                return [b](float& l, float& r) { b->process(l, r); };
            };
            expectWithinAbsoluteError(gainAt(band(fxb::EqBand::Peak, 1000.0f, 1.0f, 12.0f), 1000.0, sr), 12.0f, 0.2f);
            expectWithinAbsoluteError(gainAt(band(fxb::EqBand::Peak, 1000.0f, 1.0f, 12.0f), 60.0, sr), 0.0f, 0.3f);
            expectWithinAbsoluteError(gainAt(band(fxb::EqBand::Peak, 1000.0f, 1.0f, -12.0f), 1000.0, sr), -12.0f, 0.2f);
            expectWithinAbsoluteError(gainAt(band(fxb::EqBand::LowShelf, 200.0f, 0.707f, 9.0f), 30.0, sr), 9.0f, 0.3f);
            expectWithinAbsoluteError(gainAt(band(fxb::EqBand::LowShelf, 200.0f, 0.707f, 9.0f), 5000.0, sr), 0.0f, 0.2f);
            expectWithinAbsoluteError(gainAt(band(fxb::EqBand::HighShelf, 3000.0f, 0.707f, -9.0f), 15000.0, sr), -9.0f, 0.4f);
            expectWithinAbsoluteError(gainAt(band(fxb::EqBand::HighShelf, 3000.0f, 0.707f, -9.0f), 100.0, sr), 0.0f, 0.2f);
            // a narrow peak is narrow
            const float wide = gainAt(band(fxb::EqBand::Peak, 1000.0f, 0.5f, 12.0f), 2000.0, sr);
            const float narrow = gainAt(band(fxb::EqBand::Peak, 1000.0f, 8.0f, 12.0f), 2000.0, sr);
            expect(wide > narrow + 5.0f, juce::String(wide) + " dB an octave up when wide, " + juce::String(narrow) + " when narrow");
        }

        beginTest("EqBand: at 0 dB it is not there (bit for bit); set before it runs it is there at once; turned, it glides");
        {
            fxb::EqBand band;
            band.prepare(SR);
            band.set(500.0f, 0.9f, 0.0f);
            const Buf in = noise(0.5f, 4000, 21);
            Buf l = in, r = in;
            for (size_t i = 0; i < l.size(); ++i) band.process(l[i], r[i]);
            expect(l == in && band.isFlat());

            // +12 dB under a 500 Hz tone: no step bigger than the louder tone's own
            const Buf tone = sine(500.0, 0.2f, 48000);
            Buf out = tone;
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (i == 12000) band.setGain(12.0f);
                if (i == 30000) band.setGain(0.0f);
                float rr = out[i];
                band.process(out[i], rr);
            }
            const float loudStep = 0.2f * 4.0f * (float) (TWO_PI * 500.0 / SR);
            expect(biggestStep(out, 0, 48000) < loudStep * 1.1f, "no click: " + juce::String(biggestStep(out, 0, 48000)));
            expectWithinAbsoluteError(dB(rms(out, 24000, 30000) / rms(tone, 24000, 30000)), 12.0f, 0.2f, "arrived at +12 dB");
            expect(rms(out, 12000, 12240) < rms(out, 24000, 24240) * 0.5, "not there at once: it glides");
            expect(std::equal(out.begin() + 44000, out.end(), tone.begin() + 44000), "back at 0 dB it is out of the path again");

            // a frequency sweep glides too
            fxb::EqBand sweep;
            sweep.prepare(SR);
            sweep.set(300.0f, 4.0f, 12.0f);
            Buf swept = tone;
            for (size_t i = 0; i < swept.size(); ++i)
            {
                if (i == 12000) sweep.setFrequency(5000.0f);
                float rr = swept[i];
                sweep.process(swept[i], rr);
            }
            expect(biggestStep(swept, 0, 48000) < loudStep * 1.1f, "frequency: no click");
            expectWithinAbsoluteError(sweep.currentFrequency(), 5000.0f, 1.0f);

            // fresh: no glide from nowhere
            fxb::EqBand fresh;
            fresh.prepare(SR);
            fresh.set(500.0f, 0.9f, 12.0f);
            Buf first = tone;
            for (size_t i = 0; i < 9600; ++i) { float rr = first[i]; fresh.process(first[i], rr); }
            expectWithinAbsoluteError(dB(rms(first, 4800, 9600) / rms(tone, 4800, 9600)), 12.0f, 0.2f);
        }
    }

    void testEqualisers()
    {
        auto of = [](std::shared_ptr<EffectBase> fx) { return [fx](float& l, float& r) { fx->process(l, r); }; };

        beginTest("EQ: flat is the sound untouched; its seven cells; LFRQ, MIDQ and HFRQ move the bands");
        {
            ParametricEQ flat;
            flat.prepare(SR, BLOCK);
            const Buf in = noise(0.7f, 6000, 33);
            Buf l = in, r = in;
            flat.processStereo(l.data(), r.data(), 3000);
            for (int i = 3000; i < 6000; ++i) flat.process(l[(size_t) i], r[(size_t) i]);
            expect(l == in && r == in, "bit for bit (the master EQ of a project that has not set it)");

            const char* labels[7] = { "LOW", "MID", "MFRQ", "HIGH", "LFRQ", "MIDQ", "HFRQ" };
            for (int i = 0; i < 7; ++i)
            {
                const ParamSpec spec = i < 4 ? flat.getParamSpec(i) : flat.getExtraSpec(i - 4);
                expect(spec.isValid() && juce::String(spec.label) == labels[i]);
                expectWithinAbsoluteError(i < 4 ? flat.getParameter(i) : flat.getExtra(i - 4), spec.defaultVal, 1.0e-4f);
            }
            expect(! flat.getExtraSpec(3).isValid());

            auto eq = [&](const std::function<void(ParametricEQ&)>& set)
            {
                auto e = std::make_shared<ParametricEQ>();
                e->prepare(SR, BLOCK);
                set(*e);
                return of(e);
            };
            expectWithinAbsoluteError(gainAt(eq([](ParametricEQ& e) { e.setParameter(1, 12.0f); }), 1000.0), 12.0f, 0.2f);
            // LFRQ: a +12 dB low shelf reaches 300 Hz only when its corner is moved up
            const float low80 = gainAt(eq([](ParametricEQ& e) { e.setParameter(0, 12.0f); }), 300.0);
            const float low1k = gainAt(eq([](ParametricEQ& e) { e.setParameter(0, 12.0f); e.setExtra(0, 1000.0f); }), 300.0);
            expect(low80 < 2.5f && low1k > 9.0f, "LFRQ: " + juce::String(low80) + " dB at 80 Hz, " + juce::String(low1k) + " at 1 kHz");
            // HFRQ
            const float high8k = gainAt(eq([](ParametricEQ& e) { e.setParameter(3, -12.0f); }), 3000.0);
            const float high2k = gainAt(eq([](ParametricEQ& e) { e.setParameter(3, -12.0f); e.setExtra(2, 2000.0f); }), 3000.0);
            expect(high8k > -2.5f && high2k < -7.0f, "HFRQ: " + juce::String(high8k) + " dB at 8 kHz, " + juce::String(high2k) + " at 2 kHz");
            // MIDQ: an octave from the centre
            const float wide = gainAt(eq([](ParametricEQ& e) { e.setParameter(1, 12.0f); e.setExtra(1, 0.3f); }), 2000.0);
            const float narrow = gainAt(eq([](ParametricEQ& e) { e.setParameter(1, 12.0f); e.setExtra(1, 10.0f); }), 2000.0);
            expect(wide > narrow + 6.0f, "MIDQ: " + juce::String(wide) + " dB wide, " + juce::String(narrow) + " narrow");
            // MFRQ swept under sound glides
            ParametricEQ sweep;
            sweep.setParameter(1, 15.0f);
            sweep.setExtra(1, 6.0f);
            sweep.prepare(SR, 64);
            const Buf tone = sine(700.0, 0.1f, 48000);
            const Buf out = run(sweep, tone, nullptr, 64, [&](int b) { if (b > 100 && b < 600 && b % 20 == 0) sweep.setParameter(2, b % 40 == 0 ? 8000.0f : 100.0f); });
            expect(biggestStep(out, 0, 48000) < 0.1f * 5.7f * (float) (TWO_PI * 700.0 / SR) * 1.15f, "MFRQ thrown about: " + juce::String(biggestStep(out, 0, 48000)));
        }

        beginTest("GEQ: flat is the sound untouched; each band's frequency cell moves it");
        {
            GraphicEQ flat;
            flat.prepare(SR, BLOCK);
            const Buf in = noise(0.7f, 4000, 34);
            Buf l = in, r = in;
            flat.processStereo(l.data(), r.data(), 4000);
            expect(l == in);
            const char* labels[4] = { "LFRQ", "LMFQ", "HMFQ", "HFRQ" };
            for (int b = 0; b < 4; ++b)
            {
                const ParamSpec spec = flat.getExtraSpec(b);
                expect(juce::String(spec.label) == labels[b]);
                expectWithinAbsoluteError(flat.getExtra(b), GraphicEQ::bandFrequency(b), 0.01f, "starts where the band always was");
                expect(spec.defaultVal == GraphicEQ::bandFrequency(b) && spec.minVal < spec.defaultVal && spec.maxVal > spec.defaultVal);
            }
            for (int b = 1; b <= 2; ++b)
            {
                const float moved = b == 1 ? 1500.0f : 600.0f;
                auto geq = [&](bool move)
                {
                    auto g = std::make_shared<GraphicEQ>();
                    g->prepare(SR, BLOCK);
                    g->setParameter(b, 12.0f);
                    if (move) g->setExtra(b, moved);
                    return of(g);
                };
                expectWithinAbsoluteError(gainAt(geq(false), GraphicEQ::bandFrequency(b)), 12.0f, 0.3f);
                expectWithinAbsoluteError(gainAt(geq(true), moved), 12.0f, 0.3f, "the peak is where the cell put it");
                expect(gainAt(geq(true), GraphicEQ::bandFrequency(b)) < 8.0f, "and no longer where it was");
            }
        }

        beginTest("AMP: its tone stack is the same band: TONE tilts, and at 0 it is out of the path");
        {
            auto amp = [&](float tone)
            {
                auto a = std::make_shared<AmpSim>();
                a->setParameter(AmpSim::Gain, 0.0f);
                a->setParameter(AmpSim::Cab, 10000.0f);
                a->setParameter(AmpSim::Tone, tone);
                a->prepare(SR, BLOCK);
                return of(a);
            };
            const float lowFlat = gainAt(amp(0.0f), 150.0), highFlat = gainAt(amp(0.0f), 5000.0);
            expectWithinAbsoluteError(gainAt(amp(1.0f), 150.0) - lowFlat, -9.0f * 0.75f, 2.5f, "bright: bass down");
            expectWithinAbsoluteError(gainAt(amp(1.0f), 5000.0) - highFlat, 9.0f * 0.9f, 2.0f, "bright: treble up");
            expectWithinAbsoluteError(gainAt(amp(-1.0f), 5000.0) - highFlat, -9.0f * 0.9f, 2.0f, "dark: treble down");
        }
    }

    // ---------------------------------------------------------------------------------------
    void testModulateCell()
    {
        beginTest("CellEffect::moved, as the dynamics use it: a share of the range, in octaves on a logarithmic cell; 0 is the knob's value to the bit");
        {
            const ParamSpec lin { "THR", 0, -60.0f, 0.0f, -20.0f, ParamKind::Linear, "dB" };
            const ParamSpec log { "REL", 1, 10.0f, 1000.0f, 100.0f, ParamKind::Log, "ms" };
            const ParamSpec list = dyn::keySpec(0);
            expectWithinAbsoluteError(CellEffect::moved(lin, -20.0f, 0.25f), -5.0f, 1.0e-3f);
            expectWithinAbsoluteError(CellEffect::moved(lin, -20.0f, 1.0f), 0.0f, 1.0e-4f, "held at the end of the range");
            expectWithinAbsoluteError(CellEffect::moved(log, 100.0f, 0.5f), 1000.0f, 0.5f);
            expectWithinAbsoluteError(CellEffect::moved(log, 100.0f, -0.25f), 31.62f, 0.05f);
            expect(CellEffect::moved(lin, -17.123457f, 0.0f) == -17.123457f && CellEffect::moved(log, 123.4567f, 0.0f) == 123.4567f);
            expectEquals(CellEffect::moved(list, 0.0f, 1.0f), 0.0f, "a list does not move");
            expectEquals(CellEffect::moved(lin, -20.0f, std::numeric_limits<float>::quiet_NaN()), -20.0f);
            expectEquals(CellEffect::offsetOf(7.0f), 1.0f);
            expectEquals(CellEffect::offsetOf(std::numeric_limits<float>::infinity()), 0.0f);
        }

        // For every continuous cell of every effect of this phase: modulated by an offset, the effect
        // sounds as it does with the knob turned to that value, and the knob has not moved; offset 0
        // puts it back.
        struct Case { const char* name; std::function<std::unique_ptr<EffectBase>()> make; std::vector<int> cells; };
        const std::vector<Case> cases = {
            { "CMP",  [] { auto c = std::make_unique<Compressor>(); c->setThreshold(-30.0f); c->setExtra(3, 200.0f); return std::unique_ptr<EffectBase>(std::move(c)); }, { 0, 1, 2, 3, 5, 6, 7 } },
            { "DUCK", [] { auto c = std::make_unique<SidechainInsert>(); c->setExtra(3, 200.0f); return std::unique_ptr<EffectBase>(std::move(c)); }, { 1, 2, 3, 4, 5, 6, 7 } },
            { "LMT",  [] { auto l = std::make_unique<Limiter>(); l->setCeiling(-0.3f); return std::unique_ptr<EffectBase>(std::move(l)); }, { 0, 1 } },
            { "TRNS", [] { auto t = std::make_unique<TransientShaper>(); t->setParameter(0, 0.3f); t->setParameter(1, -0.3f); return std::unique_ptr<EffectBase>(std::move(t)); }, { 0, 1, 2, 3 } },
            { "EQ",   [] { auto e = std::make_unique<ParametricEQ>(); e->setParameter(0, 4.0f); e->setParameter(1, 6.0f); e->setParameter(3, -4.0f); return std::unique_ptr<EffectBase>(std::move(e)); }, { 0, 1, 2, 3, 4, 5, 6 } },
            { "GEQ",  [] { auto g = std::make_unique<GraphicEQ>(); for (int i = 0; i < 4; ++i) g->setParameter(i, i % 2 ? 5.0f : -5.0f); return std::unique_ptr<EffectBase>(std::move(g)); }, { 0, 1, 2, 3, 4, 5, 6, 7 } },
        };
        // drum-like hits of noise over a low tone: something for every one of them to work on
        Buf in = noise(0.8f, 36000, 77);
        for (int i = 0; i < 36000; ++i)
            in[(size_t) i] = in[(size_t) i] * std::exp(-(float) (i % 6000) / 900.0f) + 0.3f * (float) std::sin(TWO_PI * 90.0 * i / SR);

        for (const auto& c : cases)
        {
            beginTest(juce::String(c.name) + ": modulateCell moves each cell as the knob would, and leaves the knob");
            for (int cell : c.cells)
                for (float offset : { 0.3f, -0.3f })
                {
                    auto spec = [&](EffectBase& fx) { return cell < 4 ? fx.getParamSpec(cell) : fx.getExtraSpec(cell - 4); };
                    auto get = [&](EffectBase& fx) { return cell < 4 ? fx.getParameter(cell) : fx.getExtra(cell - 4); };
                    auto plain = c.make(), modulated = c.make(), turned = c.make();
                    const float knob = get(*plain);
                    const float target = CellEffect::moved(spec(*plain), knob, offset);
                    if (cell < 4) turned->setParameter(cell, target); else turned->setExtra(cell - 4, target);
                    for (auto* fx : { plain.get(), modulated.get(), turned.get() }) fx->prepare(SR, BLOCK);
                    modulated->modulateCell(cell, offset);
                    const juce::String what = juce::String(c.name) + " " + spec(*plain).label + (offset > 0 ? " up" : " down");

                    const Buf a = run(*plain, in), b = run(*modulated, in), t = run(*turned, in);
                    expectEquals(get(*modulated), knob, what + ": the knob stays");
                    // (a modulation glides to where the knob, set before the first sample, already is:
                    // the first tenth of a second is left out)
                    double diff = 0.0, moved = 0.0;
                    for (size_t i = 4800; i < a.size(); ++i)
                    {
                        diff += (double) (b[i] - t[i]) * (b[i] - t[i]);
                        moved += (double) (b[i] - a[i]) * (b[i] - a[i]);
                    }
                    if (target != knob)
                        expect(moved > 1.0e-6 * (double) a.size() * 0.0001, what + " is heard: " + juce::String(std::sqrt(moved / (double) a.size())));
                    expect(diff <= moved * 1.0e-4 + 1.0e-9, what + " = the knob at " + juce::String(target) + " (difference "
                                                            + juce::String(std::sqrt(diff / (double) a.size())) + ")");

                    modulated->modulateCell(cell, 0.0f);
                    modulated->reset();
                    plain->reset();
                    expect(run(*modulated, in) == run(*plain, in), what + ": offset 0 is the knob again, bit for bit");
                    // what is not a number, and a cell that is not there, change nothing
                    modulated->modulateCell(cell, std::numeric_limits<float>::quiet_NaN());
                    modulated->modulateCell(-1, 0.5f);
                    modulated->modulateCell(8, 0.5f);
                    modulated->modulateCell(1000, 0.5f);
                    modulated->reset();
                    plain->reset();
                    expect(run(*modulated, in) == run(*plain, in), what + ": no number, no cell, no change");
                }
        }

        beginTest("modulateCell: a list cell (KEY, SRC) does not move; an effect without it ignores the call");
        {
            Compressor c;
            c.modulateCell(4, 1.0f);
            expect(! c.wantsInput() && c.getExtra(0) == 0.0f);
            SidechainInsert d;
            d.modulateCell(0, 1.0f);
            expectEquals(d.keySource(), (int) EffectBase::KEY_SELF);
            expect(! d.wantsInput());
            EffectBase* base = &c;
            base->modulateCell(0, 0.1f);     // (virtual on EffectBase)
            expectEquals(c.getThreshold(), -20.0f);
        }

        beginTest("modulateCell at block rate: a threshold, a make-up gain and an EQ band swept under sound do not click");
        {
            const Buf tone = sine(200.0, 0.5f, 48000);
            const float own = 0.5f * (float) (TWO_PI * 200.0 / SR);
            Compressor comp;
            comp.setThreshold(-30.0f);
            comp.setMakeupGain(6.0f);
            comp.prepare(SR, 64);
            Buf out = run(comp, tone, nullptr, 64, [&](int b)
            {
                comp.modulateCell(0, 0.4f * std::sin(0.11f * (float) b));
                comp.modulateCell(3, b % 60 < 30 ? 0.2f : -0.2f);           // MAKE jumped by 12 dB each way
            });
            expect(biggestStep(out, 4800, 48000) < own * 4.0f, "CMP: " + juce::String(biggestStep(out, 4800, 48000)) + " against the tone's " + juce::String(own));

            ParametricEQ eq;
            eq.prepare(SR, 64);
            out = run(eq, tone, nullptr, 64, [&](int b)
            {
                eq.modulateCell(1, b % 50 < 25 ? 0.5f : -0.5f);             // MID thrown from +18 to -18 dB
                eq.modulateCell(2, 0.5f * std::sin(0.3f * (float) b));
            });
            expect(biggestStep(out, 0, 48000) < own * 8.0f * 1.2f, "EQ: " + juce::String(biggestStep(out, 0, 48000)));
        }
    }

    // ---------------------------------------------------------------------------------------
    void testBounds()
    {
        beginTest("Bounds: every effect of the phase, keyed, 22.05 to 192 kHz, blocks of 1 to 4096, cells at both ends, "
                  "sound and key that are not numbers");
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        struct Rate { double sr; int block; };
        for (const Rate rate : { Rate { 22050.0, 1 }, Rate { 44100.0, 64 }, Rate { 96000.0, 333 }, Rate { 192000.0, 4096 } })
            for (int which = 0; which < 7; ++which)
            {
                std::unique_ptr<EffectBase> fx;
                switch (which)
                {
                    case 0: fx = std::make_unique<Compressor>(); break;
                    case 1: fx = std::make_unique<SidechainInsert>(); break;
                    case 2: fx = std::make_unique<Limiter>(); break;
                    case 3: fx = std::make_unique<TransientShaper>(); break;
                    case 4: fx = std::make_unique<Wah>(); break;
                    case 5: fx = std::make_unique<ParametricEQ>(); break;
                    default: fx = std::make_unique<GraphicEQ>(); break;
                }
                fx->prepare(rate.sr, rate.block);
                const int n = std::max(rate.block * 3, (int) (rate.sr * 0.05));
                const Buf sound = noise(1.0f, n, 5 + (uint32_t) which), key = noise(1.0f, n, 50);
                bool finite = true;
                float top = 0.0f;
                for (int corner = 0; corner < 4; ++corner)
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        const ParamSpec spec = i < 4 ? fx->getParamSpec(i) : fx->getExtraSpec(i - 4);
                        if (! spec.isValid()) continue;
                        const float v = ((corner >> (i % 2)) & 1) ? spec.maxVal : spec.minVal;
                        if (i < 4) fx->setParameter(i, v); else fx->setExtra(i - 4, v);
                        fx->modulateCell(i, corner % 2 ? 1.0f : -1.0f);
                    }
                    const Buf out = run(*fx, sound, &key, rate.block);
                    finite = finite && allFinite(out);
                    for (float v : out) top = std::max(top, std::abs(v));
                }
                expect(finite, "effect " + juce::String(which) + " at " + juce::String(rate.sr, 0));
                expect(top < 300.0f, "effect " + juce::String(which) + " peak " + juce::String(top));      // (EQ: three bands of +18 dB)

                // a key and a sound that are not numbers: the effect is finite again once they are
                for (int i = 0; i < 8; ++i)
                {
                    const ParamSpec spec = i < 4 ? fx->getParamSpec(i) : fx->getExtraSpec(i - 4);
                    if (! spec.isValid()) continue;
                    fx->modulateCell(i, 0.0f);
                    const float v = juce::String(spec.label) == "KEY" ? (float) dyn::KEY_CHOICE_IN
                                  : juce::String(spec.label) == "SRC" ? (float) dyn::KEY_CHOICE_IN : spec.defaultVal;
                    if (i < 4) fx->setParameter(i, v); else fx->setExtra(i - 4, v);
                }
                Buf badKey = key;
                for (size_t i = 0; i < badKey.size(); ++i)
                    badKey[i] = i % 5 == 0 ? nan : (i % 5 == 1 ? inf : (i % 5 == 2 ? -1.0e30f : badKey[i]));
                expect(allFinite(run(*fx, sound, &badKey, rate.block)), "effect " + juce::String(which) + ": a bad key");
                Buf huge = sound;
                for (auto& v : huge) v *= 1.0e30f;
                expect(allFinite(run(*fx, huge, &key, rate.block)), "effect " + juce::String(which) + ": a sound of 1e30");
                fx->reset();
                const Buf after = run(*fx, sound, &key, rate.block);
                expect(allFinite(after), "effect " + juce::String(which) + ": and sane after reset");
                // silence in, silence out
                fx->reset();
                const Buf nothing((size_t) n, 0.0f);
                const Buf quiet = run(*fx, nothing, nullptr, rate.block);
                float residue = 0.0f;
                for (float v : quiet) residue = std::max(residue, std::abs(v));
                expect(residue < 1.0e-9f, "effect " + juce::String(which) + ": silence stays silence");
            }
    }

    // ---------------------------------------------------------------------------------------
    // What the review of the phase found (each of these failed on the tree as it was handed over,
    // or holds something no test held).
    void testReview()
    {
        beginTest("Transient shaper: the gain does not step where the attack hands over to the tail, nor under a key from outside");
        {
            const int n = 48000, period = 12000;
            Buf drum((size_t) n);
            for (int i = 0; i < n; ++i)
                drum[(size_t) i] = std::exp(-(float) (i % period) / 2400.0f) * (float) std::sin(0.03 * (i % period));

            // a steady level keyed by the drum: what comes out is the gain itself
            float worstKeyed = 0.0f;
            for (float spd : { 0.0f, 0.5f, 1.0f })
                for (int signs = 0; signs < 4; ++signs)
                {
                    TransientShaper t;
                    t.setParameter(TransientShaper::Attack, (signs & 1) ? 1.0f : -1.0f);
                    t.setParameter(TransientShaper::Sustain, (signs & 2) ? 1.0f : -1.0f);
                    t.setParameter(TransientShaper::Speed, spd);
                    t.setExtra(0, (float) dyn::KEY_CHOICE_IN);
                    t.prepare(SR, BLOCK);
                    const Buf gain = run(t, Buf((size_t) n, 1.0f), &drum);
                    expect(t.keyConnected());
                    float low = 1.0f, high = 1.0f;
                    for (int i = period; i < n; ++i)
                    {
                        worstKeyed = std::max(worstKeyed, std::abs(gain[(size_t) i] - gain[(size_t) i - 1]) / gain[(size_t) i - 1]);
                        low = std::min(low, gain[(size_t) i]);
                        high = std::max(high, gain[(size_t) i]);
                    }
                    expect(high / low > 2.0f, "and the key does move the gain: " + juce::String(high / low));
                }
            // (it was 2.9: the gain went from a quarter to 1 in one sample; a 1 ms rise is 0.15 at most)
            expect(worstKeyed < 0.25f, "keyed: the gain moved by " + juce::String(worstKeyed) + " of itself in one sample");

            // keyed by itself: after the first 3 ms of a hit the gain moves smoothly (it stepped by
            // 0.86 of itself 55 ms into the hit, where the slow follower caught up with the level)
            TransientShaper t;
            t.setParameter(TransientShaper::Attack, 1.0f);
            t.setParameter(TransientShaper::Sustain, 1.0f);
            t.prepare(SR, BLOCK);
            const Buf out = run(t, drum);
            float worstSelf = 0.0f;
            for (int i = period + 1; i < n; ++i)
            {
                const float a = drum[(size_t) i - 1], b = drum[(size_t) i];
                if (i % period < 144 || std::abs(a) < 0.05f || std::abs(b) < 0.05f) continue;
                const float g0 = out[(size_t) i - 1] / a, g1 = out[(size_t) i] / b;
                worstSelf = std::max(worstSelf, std::abs(g1 - g0) / g0);
            }
            expect(worstSelf < 0.05f, "by itself: the gain moved by " + juce::String(worstSelf) + " of itself in one sample");
        }

        beginTest("EQ, GEQ: a shelf or a band thrown end to end under a low note does not crackle");
        {
            // (A high shelf has a large b0: coefficients put on in steps, even small ones every 16
            // samples, stepped the output at 220 Hz by six times the note's own step.)
            const Buf tone = sine(220.0, 0.5f, 48000);
            const float own = 0.5f * (float) (TWO_PI * 220.0 / SR);
            for (int which = 0; which < 2; ++which)
                for (int cell = 0; cell < 8; ++cell)
                {
                    std::unique_ptr<EffectBase> fx;
                    if (which == 0) fx = std::make_unique<ParametricEQ>(); else fx = std::make_unique<GraphicEQ>();
                    const ParamSpec spec = cell < 4 ? fx->getParamSpec(cell) : fx->getExtraSpec(cell - 4);
                    if (! spec.isValid()) continue;
                    auto set = [&](float v) { if (cell < 4) fx->setParameter(cell, v); else fx->setExtra(cell - 4, v); };
                    // (a frequency or Q cell is heard only with its band up)
                    for (int g = 0; g < 4; ++g)
                        if (cell >= 4 || juce::String(spec.unit) != "dB")
                            if (juce::String(fx->getParamSpec(g).unit) == "dB") fx->setParameter(g, 12.0f);
                    set(spec.minVal);
                    fx->prepare(SR, 64);
                    const Buf out = run(*fx, tone, nullptr, 64, [&](int b)
                    {
                        if (b == 250) set(spec.maxVal);
                        if (b == 500) set(spec.minVal);
                    });
                    float top = 0.0f;
                    for (float v : out) top = std::max(top, std::abs(v));
                    const float step = biggestStep(out, 64, 48000);
                    // the note's own step at the loudest it gets, and half as much again
                    expect(step < own * (top / 0.5f) * 1.5f, juce::String(which == 0 ? "EQ " : "GEQ ") + spec.label + ": a step of "
                                                             + juce::String(step) + ", the note's own " + juce::String(own * top / 0.5f));
                }
        }

        beginTest("EqBand: between two steps of a glide the coefficients move in a line; it arrives at the set response exactly");
        {
            fxb::EqBand moved, fresh;
            for (auto* b : { &moved, &fresh }) { b->setShape(fxb::EqBand::HighShelf); b->prepare(SR); }
            moved.set(8000.0f, 0.707f, -18.0f);
            float l = 0.0f, r = 0.0f;
            moved.process(l, r);
            moved.set(3000.0f, 0.707f, 18.0f);
            for (int i = 0; i < 48000; ++i) { l = r = 0.0f; moved.process(l, r); }
            expectEquals(moved.currentGain(), 18.0f);
            fresh.set(3000.0f, 0.707f, 18.0f);
            // the same filter now: the same samples from the same input (the glide leaves nothing behind)
            const Buf in = noise(0.5f, 4096, 77);
            bool same = true;
            for (float v : in)
            {
                float a = v, a2 = v, b = v, b2 = v;
                moved.process(a, a2);
                fresh.process(b, b2);
                same = same && std::abs(a - b) < 1.0e-5f && a == a2;
            }
            expect(same);

            // a flat band whose frequency is moved has nothing to glide: it is there when it is turned up
            fxb::EqBand band;
            band.prepare(SR);
            band.set(200.0f, 1.0f, 0.0f);
            band.process(l, r);
            band.setFrequency(5000.0f);
            band.process(l, r);
            expect(band.isFlat());
            expectEquals(band.currentFrequency(), 5000.0f);
        }

        beginTest("EQ, GEQ: a block gives the samples that one sample at a time gives, at rest and while cells are turned");
        for (int which = 0; which < 2; ++which)
        {
            std::unique_ptr<EffectBase> byBlock, bySample;
            if (which == 0) { byBlock = std::make_unique<ParametricEQ>(); bySample = std::make_unique<ParametricEQ>(); }
            else            { byBlock = std::make_unique<GraphicEQ>();    bySample = std::make_unique<GraphicEQ>(); }
            const Buf in = noise(0.5f, 20 * 100, 31);
            Buf a = in, a2 = in, b = in, b2 = in;
            for (auto* fx : { byBlock.get(), bySample.get() }) fx->prepare(SR, 100);
            for (int block = 0; block < 20; ++block)
            {
                for (auto* fx : { byBlock.get(), bySample.get() })
                {
                    if (block == 2) fx->setParameter(0, 9.0f);
                    if (block == 5) { fx->setParameter(3, -12.0f); fx->setExtra(0, 300.0f); }
                    if (block == 9) fx->setParameter(0, 0.0f);
                    if (block >= 12) fx->modulateCell(1, 0.05f * (float) (block - 12));
                }
                byBlock->processStereo(a.data() + block * 100, a2.data() + block * 100, 100);
                for (int i = block * 100; i < block * 100 + 100; ++i) bySample->process(b[(size_t) i], b2[(size_t) i]);
            }
            expect(a == b && a2 == b2, which == 0 ? "EQ" : "GEQ");
            expect(a != in);
        }

        beginTest("EffectChain: series, parallel and mixed, the phase's effects in it, keyed and not");
        {
            const Buf pad = sine(300.0, 0.1f, 24000), loud = noise(0.9f, 24000, 9);
            auto level = [&](EffectChain::Routing routing, bool keyIn, bool handKey)
            {
                EffectChain chain;
                chain.addEffect(std::make_unique<ParametricEQ>());
                chain.addEffect(std::make_unique<Compressor>());
                chain.addEffect(std::make_unique<TransientShaper>());
                chain.addEffect(std::make_unique<Wah>());
                chain.addEffect(std::make_unique<SidechainInsert>());
                chain.addEffect(std::make_unique<Limiter>());
                chain.setRouting(routing);
                chain.prepare(SR, BLOCK);
                chain.getEffect(0)->setParameter(0, 6.0f);
                chain.getEffect(1)->setParameter(0, -30.0f);
                chain.getEffect(1)->setParameter(1, 20.0f);
                chain.getEffect(1)->setExtra(0, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                chain.getEffect(2)->setParameter(0, 1.0f);
                chain.getEffect(2)->setExtra(0, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                chain.getEffect(3)->setExtra(Wah::KEY_EXTRA, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                chain.getEffect(4)->setParameter(0, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                Buf out;
                std::vector<float> l((size_t) BLOCK), r((size_t) BLOCK);
                for (int done = 0; done + BLOCK <= (int) pad.size(); done += BLOCK)
                {
                    std::copy(pad.begin() + done, pad.begin() + done + BLOCK, l.begin());
                    r = l;
                    for (int i = 0; i < chain.getEffectCount(); ++i)        // as the AudioEngine does
                        if (auto* fx = chain.getEffect(i); fx->wantsInput())
                            fx->setInput(handKey ? loud.data() + done : nullptr, handKey ? loud.data() + done : nullptr, handKey ? BLOCK : 0);
                    chain.processStereo(l.data(), r.data(), BLOCK);
                    out.insert(out.end(), l.begin(), l.end());
                }
                expect(allFinite(out));
                return out;
            };
            for (auto routing : { EffectChain::Routing::Series, EffectChain::Routing::Parallel, EffectChain::Routing::Mixed })
            {
                const Buf self = level(routing, false, false), unplugged = level(routing, true, false), keyed = level(routing, true, true);
                const juce::String name = "routing " + juce::String((int) routing);
                expect(self == unplugged, name + ": KEY IN with nothing handed over is SELF, sample for sample");
                expect(rms(self, 12000, 24000) > 1.0e-3, name + ": it sounds");
                expect(rms(keyed, 12000, 24000) < rms(self, 12000, 24000) * 0.8, name + ": the key presses the pad down, "
                       + juce::String(dB(rms(keyed, 12000, 24000) / rms(self, 12000, 24000))) + " dB");
            }
        }

        beginTest("As a send effect: KEY IN has no input there and works as SELF; the engine stays finite");
        {
            auto sendLevel = [&](EffectType type, bool keyIn)
            {
                TrackInputTests::Rig rig;
                auto* tone = rig.add<TrackInputTests::ToneEngine>(0);
                auto* other = rig.add<TrackInputTests::ToneEngine>(1);
                tone->left = tone->right = 0.5f;
                other->left = other->right = 0.9f;
                rig.audio.setTrackParameter(1, MixParam::VOLUME, 0.0f);
                rig.audio.setTrackParameter(0, TrackInParam::SRC, 2.0f);
                rig.audio.setTrackParameter(0, AudioEngine::PARAM_SEND1, 1.0f);
                rig.audio.setInsertEffect(AudioEngine::sendTrack(0), 0, type);
                auto* fx = rig.audio.getSendBus(0).getChain().getEffect(0);
                expect(fx != nullptr);
                if (fx == nullptr) return 0.0;
                fx->setParameter(0, type == EffectType::CompressorEffect ? -30.0f : 1.0f);
                for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
                    if (juce::String(fx->getExtraSpec(i).label) == "KEY") fx->setExtra(i, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                rig.blocks(300);
                double sum = 0.0;
                for (float v : rig.left) sum += (double) v * v;
                expectEquals((int) rig.audio.getNonFiniteBlocks(), 0);
                return std::sqrt(sum / rig.block);
            };
            for (auto type : { EffectType::CompressorEffect, EffectType::TransientEffect, EffectType::WahEffect })
            {
                const double self = sendLevel(type, false), in = sendLevel(type, true);
                expectWithinAbsoluteError((float) in, (float) self, (float) self * 1.0e-4f + 1.0e-9f, "send effect " + juce::String((int) type));
            }
        }

        beginTest("Silence after sound: nothing any of them puts out lingers in the denormal range, and it ends at nothing");
        for (int which = 0; which < 8; ++which)
        {
            std::unique_ptr<EffectBase> fx;
            switch (which)
            {
                case 0: fx = std::make_unique<Compressor>(); break;
                case 1: fx = std::make_unique<SidechainInsert>(); break;
                case 2: fx = std::make_unique<Limiter>(); break;
                case 3: fx = std::make_unique<TransientShaper>(); break;
                case 4: fx = std::make_unique<Wah>(); break;
                case 5: fx = std::make_unique<ParametricEQ>(); break;
                case 6: fx = std::make_unique<GraphicEQ>(); break;
                default: fx = std::make_unique<AmpSim>(); break;
            }
            for (int i = 0; i < 8; ++i)
            {
                const ParamSpec spec = i < 4 ? fx->getParamSpec(i) : fx->getExtraSpec(i - 4);
                if (! spec.isValid() || spec.isDiscrete()) continue;
                const float v = spec.fromNorm(0.8f);
                if (i < 4) fx->setParameter(i, v); else fx->setExtra(i - 4, v);
            }
            fx->prepare(SR, BLOCK);
            run(*fx, noise(0.5f, 24000, 3 + (uint32_t) which));
            const Buf after = run(*fx, Buf((size_t) (4 * 48000), 0.0f));
            int subnormal = 0;
            for (float v : after)
                if (std::fpclassify(v) == FP_SUBNORMAL) ++subnormal;
            expectEquals(subnormal, 0, "effect " + juce::String(which));
            // (below -240 dB: a biquad's memory is emptied under 1e-15, and can idle just above it)
            expect(rms(after, 3 * 48000, 4 * 48000) < 1.0e-12, "effect " + juce::String(which) + " still sounds after 3 s of nothing: "
                                                            + juce::String(rms(after, 3 * 48000, 4 * 48000)));
        }

        beginTest("A key handed over and then not used (the effect run sample by sample) is dropped, not kept for a later block");
        {
            // (Kept, its pointers would be read a block later, when what they pointed at may be gone.)
            const Buf quiet = sine(300.0, 0.02f, 4800), loud = noise(0.9f, 4800, 4);
            Compressor keyed, self;
            for (auto* c : { &keyed, &self }) { c->setThreshold(-30.0f); c->setRatio(20.0f); c->prepare(SR, BLOCK); }
            keyed.setExtra(0, (float) dyn::KEY_CHOICE_IN);
            Buf a = quiet, b = quiet;
            keyed.setInput(loud.data(), loud.data(), BLOCK);
            for (int i = 0; i < BLOCK; ++i) { float r = a[(size_t) i]; keyed.process(a[(size_t) i], r); }
            Buf right = a;
            keyed.processBlock(a.data() + BLOCK, right.data() + BLOCK, 4800 - BLOCK);       // no setInput before it
            expect(! keyed.keyConnected());
            for (auto& v : b) { float r = v; self.process(v, r); }
            expect(a == b, "the compressor heard a key nobody handed it");
        }

        beginTest("MAKE and OUT thrown under a tone at its peak: the level glides there");
        {
            const Buf tone = sine(200.0, 0.5f, 9600);
            const float own = 0.5f * (float) (TWO_PI * 200.0 / SR);
            Compressor comp;
            comp.setRatio(1.0f);
            comp.prepare(SR, 64);
            // (block 16 begins 4 samples after a peak of the tone: unsmoothed, a step of 7)
            const Buf out = run(comp, tone, nullptr, 64, [&](int b) { if (b == 16) comp.setMakeupGain(24.0f); });
            expectWithinAbsoluteError(dB(rms(out, 7200, 9600) / rms(tone, 7200, 9600)), 24.0f, 0.1f);
            expect(biggestStep(out, 0, 9600) < own * 15.9f * 1.5f, "MAKE: a step of " + juce::String(biggestStep(out, 0, 9600)));
        }

        beginTest("WAH on the shared key plumbing: sample by sample it is the block; a key is used once and dropped");
        {
            const Buf in = noise(0.3f, 4800, 21), loud = Buf(4800, 0.9f);
            Wah block, single, keyed;
            for (auto* w : { &block, &single, &keyed })
            {
                w->setParameter(Wah::Sens, 1.0f);
                w->prepare(SR, BLOCK);
            }
            const Buf a = run(block, in);
            Buf b = in;
            for (auto& v : b) { float r = v; single.process(v, r); }
            expect(a == b);
            keyed.setExtra(Wah::KEY_EXTRA, (float) dyn::KEY_CHOICE_IN);
            run(keyed, in, &loud);
            expect(keyed.keyConnected() && keyed.getCurrentFrequency() > 3000.0f);
            run(keyed, in);                 // no key handed over any more
            run(keyed, in);
            expect(! keyed.keyConnected() && keyed.getCurrentFrequency() < 3000.0f, "the old key is not read again");
            EffectBase* base = &keyed;
            expect(base->wantsInput());
            keyed.setExtra(Wah::KEY_EXTRA, 0.0f);
            expect(! base->wantsInput());
        }

        // (Review of the one mechanism: each of these three passed with the product code broken.)
        beginTest("A key in blocks that do not fall on the clock: every stretch of the block reads its own part of the key");
        {
            // (A block of 100 is run in stretches, up to the next look and on from it; the key has to go on with them.)
            const int n = 9600;
            const Buf in = sine(300.0, 0.2f, n), key = bursts(80.0, 0.9f, n, 1200, 150);
            const auto keyed = [&](int block)
            {
                Compressor comp;
                comp.setThreshold(-30.0f);
                comp.setRatio(20.0f);
                comp.setAttack(0.1f);
                comp.setRelease(10.0f);
                comp.setExtra(0, (float) dyn::KEY_CHOICE_IN);         // KEY IN
                comp.prepare(SR, block);
                return run(comp, in, &key, block);
            };
            const Buf whole = keyed(n), odd = keyed(100), small = keyed(7);
            expect(rms(whole, 0, n) < 0.9f * rms(in, 0, n), "the key is heard at all");
            expect(odd == whole, "blocks of 100 give the samples of one block");
            expect(small == whole, "blocks of 7 too");
        }

        beginTest("A setting without a cell (the compressor's HOLD) reaches the core when it is set under sound, nothing else turned");
        {
            Compressor held, plain;
            for (auto* comp : { &held, &plain })
            {
                comp->setThreshold(-30.0f);
                comp->setRatio(20.0f);
                comp->setAttack(1.0f);
                comp->setRelease(20.0f);
                comp->prepare(SR, BLOCK);
                run(*comp, sine(500.0, 0.9f, 4800));
            }
            held.setHold(300.0f);
            Buf in = sine(500.0, 0.01f, 24000);
            for (int i = 0; i < 4800; ++i) in[(size_t) i] *= 90.0f;
            const Buf a = run(held, in), b = run(plain, in);
            const float heldLevel = rms(a, 4800 + 4800, 4800 + 5760), plainLevel = rms(b, 4800 + 4800, 4800 + 5760);
            expect(heldLevel < 0.2f * plainLevel, "HOLD set under sound keeps the gain down: " + juce::String(heldLevel, 6) + " against " + juce::String(plainLevel, 6));
            expectEquals(held.getHold(), 300.0f);
        }

        beginTest("Limiter: modulation on LOOK does not move the length of its delay (CEIL and REL it does move)");
        {
            const Buf in = bursts(200.0, 1.5f, 9600, 2400, 800);
            Limiter still, moved;
            for (auto* limiter : { &still, &moved }) limiter->prepare(SR, BLOCK);
            const Buf a = run(still, in);
            const Buf b = run(moved, in, nullptr, BLOCK, [&](int block) { moved.modulateCell(Limiter::Lookahead, (block & 1) ? 1.0f : -1.0f); });
            expect(a == b, "the samples of the limiter left alone");
        }
    }
};

static EffectCoresDynamicsTests effectCoresDynamicsTests;
