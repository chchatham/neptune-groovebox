#pragma once
#include <juce_core/juce_core.h>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "../src/dsp/effects/DelayCore.h"
#include "../src/dsp/effects/ReverbParts.h"
#include "../src/dsp/effects/TapeDelay.h"
#include "../src/dsp/effects/LCRDelay.h"
#include "../src/dsp/effects/MultiTapInsert.h"
#include "../src/dsp/effects/ReverseDelay.h"
#include "../src/dsp/effects/SpringReverb.h"
#include "../src/dsp/effects/ShimmerReverb.h"
#include "../src/dsp/effects/PerfEffects.h"
#include "../src/dsp/effects/Delay.h"
#include "../src/dsp/effects/EffectChain.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"

// The time side of the effects (phase E2 of .ralph/spec-unify.md): one delay line (dly::Core,
// DelayCore.h) of which the tape delay, the L/C/R delay, the multi-tap, the reverse delay and PERF's
// dub echo are settings; one set of reverb parts (rvb::Core, ReverbParts.h) of which the spring and
// the shimmer reverb are settings; and the cells of all seven moved from outside
// (EffectBase::modulateCell). What each effect always did is held by the suites that were there
// before ("Effects More", "Effects Expansion", "Perf FX", "Saturation", "Send Reverb", "Robustness");
// here: the cores' own parts, what each effect gained from the others, and modulation.
// Levels are RMS (a tone that has been through a moving delay is smeared for a narrow band).
// NEPTUNE_E2_TIME=1 prints the cost of each effect in this build.
class EffectCoresTimeTests : public juce::UnitTest
{
public:
    EffectCoresTimeTests() : UnitTest("Effect Cores Time") {}

    using Signal = std::vector<float>;
    struct Stereo { Signal l, r; };
    static constexpr double SR = 48000.0;

    // ---- signals and measurements ---------------------------------------------------------------
    static Signal sine(double seconds, double hz, float amp, double sr = SR)
    {
        Signal s((size_t) (seconds * sr));
        for (size_t i = 0; i < s.size(); ++i) s[i] = amp * (float) std::sin(6.283185307179586 * hz * (double) i / sr);
        return s;
    }
    static Signal silence(double seconds, double sr = SR) { return Signal((size_t) (seconds * sr), 0.0f); }
    static Signal impulse(double seconds, float amp = 1.0f)
    {
        Signal s = silence(seconds);
        s[0] = amp;
        return s;
    }
    static Signal noise(double seconds, float amp, uint32_t seed = 4242, double sr = SR)
    {
        Signal s((size_t) (seconds * sr));
        for (auto& v : s)
        {
            seed = seed * 1664525u + 1013904223u;
            v = amp * ((float) (seed >> 8) / 8388608.0f - 1.0f);
        }
        return s;
    }
    static Signal joined(Signal a, const Signal& b)
    {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }
    // A burst with a sharp start and a slow end (so that forwards and backwards can be told apart).
    static Signal pluck(double seconds, double hz, double burst)
    {
        Signal s = silence(seconds);
        const size_t n = (size_t) (burst * SR);
        for (size_t i = 0; i < n && i < s.size(); ++i)
            s[i] = 0.5f * (float) (std::exp(-6.0 * (double) i / (double) n) * std::sin(6.283185307179586 * hz * (double) i / SR));
        return s;
    }

    static Stereo through(EffectBase& fx, const Signal& left, const Signal& right, int block = 256)
    {
        Stereo out { left, right };
        for (size_t done = 0; done < left.size(); done += (size_t) block)
            fx.processBlock(out.l.data() + done, out.r.data() + done, (int) std::min((size_t) block, left.size() - done));
        return out;
    }
    static Stereo through(EffectBase& fx, const Signal& both, int block = 256) { return through(fx, both, both, block); }

    static float rms(const Signal& s, size_t from, size_t to)
    {
        double sum = 0.0;
        to = std::min(to, s.size());
        for (size_t i = from; i < to; ++i) sum += (double) s[i] * (double) s[i];
        return to > from ? (float) std::sqrt(sum / (double) (to - from)) : 0.0f;
    }
    static float rms(const Signal& s) { return rms(s, 0, s.size()); }
    static float peak(const Signal& s, size_t from = 0, size_t to = (size_t) -1)
    {
        float p = 0.0f;
        to = std::min(to, s.size());
        for (size_t i = from; i < to; ++i) p = std::max(p, std::abs(s[i]));
        return p;
    }
    static size_t peakAt(const Signal& s, size_t from, size_t to)
    {
        size_t at = from;
        float p = -1.0f;
        for (size_t i = from; i < std::min(to, s.size()); ++i)
            if (std::abs(s[i]) > p) { p = std::abs(s[i]); at = i; }
        return at;
    }
    static bool finite(const Signal& s)
    {
        for (float v : s) if (! std::isfinite(v)) return false;
        return true;
    }
    static float biggestStep(const Signal& s, size_t from = 1, size_t to = (size_t) -1)
    {
        float step = 0.0f;
        for (size_t i = std::max<size_t>(1, from); i < std::min(to, s.size()); ++i) step = std::max(step, std::abs(s[i] - s[i - 1]));
        return step;
    }
    // The sharpest corner: |x[n] - 2 x[n-1] + x[n-2]|. A jump of J in a signal gives about J; a smooth tone of
    // amplitude A at f Hz gives A (2 pi f / fs)^2 (0.0002 for 0.3 at 220 Hz, 0.02 at 2 kHz), however its pitch
    // is being bent by a glide.
    static float sharpestCorner(const Signal& s, size_t from = 2, size_t to = (size_t) -1)
    {
        float corner = 0.0f;
        for (size_t i = std::max<size_t>(2, from); i < std::min(to, s.size()); ++i)
            corner = std::max(corner, std::abs(s[i] - 2.0f * s[i - 1] + s[i - 2]));
        return corner;
    }
    static float sharpestCorner(const Stereo& s, size_t from = 2) { return std::max(sharpestCorner(s.l, from), sharpestCorner(s.r, from)); }
    // Upward zero crossings a second: the pitch of a tone, whatever has happened to its level.
    static float crossingsPerSecond(const Signal& s, size_t from, size_t to)
    {
        int count = 0;
        to = std::min(to, s.size());
        for (size_t i = from + 1; i < to; ++i) if (s[i - 1] < 0.0f && s[i] >= 0.0f) ++count;
        return to > from ? (float) ((double) count * SR / (double) (to - from)) : 0.0f;
    }
    // Longest / shortest period between upward zero crossings, in cents: 0 for a steady tone.
    static float wobbleCents(const Signal& s, size_t from, size_t to)
    {
        double last = -1.0, shortest = 1.0e9, longest = 0.0;
        for (size_t i = from + 1; i < std::min(to, s.size()); ++i)
            if (s[i - 1] < 0.0f && s[i] >= 0.0f)
            {
                const double t = (double) (i - 1) + (double) (-s[i - 1]) / (double) (s[i] - s[i - 1]);
                if (last >= 0.0) { shortest = std::min(shortest, t - last); longest = std::max(longest, t - last); }
                last = t;
            }
        return longest > 0.0 ? (float) (1200.0 * std::log2(longest / shortest)) : 0.0f;
    }
    // Where the energy of a stretch sits, 0 = at its start, 1 = at its end.
    static float centroid(const Signal& s, size_t from, size_t to)
    {
        double e = 0.0, m = 0.0;
        to = std::min(to, s.size());
        for (size_t i = from; i < to; ++i)
        {
            const double p = (double) s[i] * (double) s[i];
            e += p;
            m += p * (double) (i - from);
        }
        return e > 0.0 ? (float) (m / (e * (double) (to - from))) : 0.5f;
    }
    // What is left above `hz` (a one-pole high-pass, twice): how bright a sound is.
    static float rmsAbove(const Signal& s, double hz, size_t from, size_t to)
    {
        fxb::OnePole a, b;
        a.setCutoff(SR, (float) hz);
        b.setCutoff(SR, (float) hz);
        double sum = 0.0;
        to = std::min(to, s.size());
        for (size_t i = 0; i < to; ++i)
        {
            const float y = b.highPass(a.highPass(s[i]));
            if (i >= from) sum += (double) y * (double) y;
        }
        return to > from ? (float) std::sqrt(sum / (double) (to - from)) : 0.0f;
    }
    static float differenceRms(const Signal& a, const Signal& b, size_t from, size_t to)
    {
        double sum = 0.0;
        to = std::min({ to, a.size(), b.size() });
        for (size_t i = from; i < to; ++i) sum += (double) (a[i] - b[i]) * (double) (a[i] - b[i]);
        return to > from ? (float) std::sqrt(sum / (double) (to - from)) : 0.0f;
    }
    static size_t at(double seconds) { return (size_t) std::llround(seconds * SR); }

    // ---- the seven effects -----------------------------------------------------------------------
    struct Kind
    {
        const char* name;
        std::function<std::unique_ptr<CellEffect>()> make;
        bool delay;
    };
    static std::vector<Kind> kinds()
    {
        return {
            { "TAPE", [] { return std::unique_ptr<CellEffect>(new TapeDelay()); }, true },
            { "LCR",  [] { return std::unique_ptr<CellEffect>(new LCRDelay()); }, true },
            { "MTAP", [] { return std::unique_ptr<CellEffect>(new MultiTapInsert()); }, true },
            { "RVSE", [] { return std::unique_ptr<CellEffect>(new ReverseDelay()); }, true },
            { "DUB",  [] { return std::unique_ptr<CellEffect>(new DubDelay()); }, true },
            { "SPRG", [] { return std::unique_ptr<CellEffect>(new SpringReverb()); }, false },
            { "SHIM", [] { return std::unique_ptr<CellEffect>(new ShimmerReverb()); }, false } };
    }
    static ParamSpec specOf(const EffectBase& fx, int cell) { return cell < 4 ? fx.getParamSpec(cell) : fx.getExtraSpec(cell - 4); }
    static void setCell(EffectBase& fx, int cell, float v) { if (cell < 4) fx.setParameter(cell, v); else fx.setExtra(cell - 4, v); }
    static float getCell(const EffectBase& fx, int cell) { return cell < 4 ? fx.getParameter(cell) : fx.getExtra(cell - 4); }
    static int cellNamed(const EffectBase& fx, const char* label)
    {
        for (int c = 0; c < CellEffect::CELLS; ++c)
            if (specOf(fx, c).isValid() && juce::String(specOf(fx, c).label) == label) return c;
        return -1;
    }
    // A core of its own, set by hand.
    struct Bare
    {
        dly::Core core;
        explicit Bare(dly::Core::Setup s = {}, float seconds = 2.0f) { core.prepare(SR, seconds, s); }
        void settle() { core.control(); core.reset(); }
        Stereo run(const Signal& l, const Signal& r, float mix = 1.0f)
        {
            Stereo out { l, r };
            for (size_t done = 0; done < l.size(); done += (size_t) dly::CONTROL)
            {
                core.control();
                core.run(out.l.data() + done, out.r.data() + done, (int) std::min((size_t) dly::CONTROL, l.size() - done), mix, mix);
            }
            return out;
        }
    };

    void runTest() override
    {
        testLine();
        testTapsAndLoop();
        testTransportAndGlide();
        testHeads();
        testCurveInTheLoop();
        testWhatEachDelayGained();
        testReverbParts();
        testWhatEachReverbGained();
        testModulation();
        testContractAndBounds();
        testReviewGaps();
        testInChainsAndEngine();
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_E2_TIME", "").isNotEmpty())
            timeThem();
    }

    // ---- the line: reads between samples, two ways ----------------------------------------------
    void testLine()
    {
        beginTest("Line: a whole delay is exact either way; between two samples the Hermite read keeps the top that the straight line loses");
        {
            dly::Line line;
            line.prepare(1000);
            dly::Line::Cursor c = line.open();
            for (int i = 0; i < 600; ++i) c.write((float) i);
            expectWithinAbsoluteError(c.readLinear(1.0f), 599.0f, 1.0e-3f);
            expectWithinAbsoluteError(c.readLinear(100.0f), 500.0f, 1.0e-3f);
            expectWithinAbsoluteError(c.readHermite(100.0f), 500.0f, 1.0e-3f);
            expectWithinAbsoluteError(c.readLinear(100.5f), 499.5f, 1.0e-3f);
            expectWithinAbsoluteError(c.readHermite(100.5f), 499.5f, 1.0e-3f);       // (a ramp is a cubic too)
            // Any delay is held to the line: nothing is read outside it.
            for (float d : { -1.0e9f, 0.0f, 1.0e9f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
                expect(std::isfinite(c.readLinear(d)) && std::isfinite(c.readHermite(d)));
            line.close(c);

            auto topKept = [](bool hermite)
            {
                dly::Core::Setup s;
                s.hermite = hermite;
                s.filterInput = false;      // (the loop filter stays out of the first echo)
                Bare b(s);
                b.core.setTapCount(1);
                b.core.setTap(0, 100.5f / (float) SR, 1.0f, 1.0f, 0.0f);
                b.core.setFilter(20000.0f, 0.0f);
                b.settle();
                const Signal in = sine(0.5, 12000.0, 0.5f);
                return rms(b.run(in, in).l, 12000, 24000) / (0.5f * 0.7071f);
            };
            const float hermite = topKept(true), linear = topKept(false);
            logMessage("12 kHz read half a sample off: Hermite " + juce::String(hermite, 3) + ", linear " + juce::String(linear, 3));
            expectWithinAbsoluteError(linear, 0.707f, 0.03f);        // cos(pi f / fs) at 12 kHz
            expect(hermite > 0.85f && hermite < 1.05f, juce::String(hermite));
        }
    }

    // ---- taps: time, level, side, feedback each ------------------------------------------------
    void testTapsAndLoop()
    {
        beginTest("Core: every tap at its own time, level and side; the loop comes round by the tap that feeds it");
        {
            dly::Core::Setup setup;
            setup.filterInput = false;
            Bare b(setup);
            b.core.setTapCount(3);
            b.core.setTap(0, 0.05f, 0.8f, 0.0f, 0.0f);
            b.core.setTap(1, 0.10f, 0.0f, 0.6f, 0.0f);
            b.core.setTap(2, 0.20f, 0.3f, 0.3f, 1.0f);
            b.core.setFeedback(0.5f);
            b.core.setFilter(20000.0f, 0.0f);
            b.settle();
            const auto out = b.run(impulse(0.7), impulse(0.7, 0.5f));
            // Left input 1, right input 0.5: a line for each side.
            expectWithinAbsoluteError(peak(out.l, at(0.05) - 2, at(0.05) + 3), 0.8f, 0.02f);
            expect(peak(out.r, at(0.05) - 2, at(0.05) + 3) < 1.0e-4f, "tap 1 is on the left alone");
            expectWithinAbsoluteError(peak(out.r, at(0.10) - 2, at(0.10) + 3), 0.3f, 0.02f);
            expect(peak(out.l, at(0.10) - 2, at(0.10) + 3) < 1.0e-4f, "tap 2 is on the right alone");
            expectWithinAbsoluteError(peak(out.l, at(0.20) - 2, at(0.20) + 3), 0.3f, 0.02f);
            // Round again 0.2 s later, at half the level: tap 1 at 0.25, tap 2 at 0.3, tap 3 at 0.4.
            // (Through the loop's low-pass, here wide open: a click loses a little of its tip.)
            expectWithinAbsoluteError(peak(out.l, at(0.25) - 3, at(0.25) + 4), 0.4f, 0.05f);
            expectWithinAbsoluteError(peak(out.r, at(0.30) - 3, at(0.30) + 4), 0.15f, 0.02f);
            expectWithinAbsoluteError(peak(out.l, at(0.40) - 3, at(0.40) + 4), 0.15f, 0.02f);
            expect(peak(out.l, at(0.06), at(0.19)) < 1.0e-3f && peak(out.l, 0, at(0.049)) < 1.0e-6f, "nothing between the taps");
        }

        beginTest("Core: eight taps at once; one line for both sides when it is set up so");
        {
            dly::Core::Setup s;
            s.stereo = false;
            s.hermite = false;
            s.filterInput = false;
            Bare b(s);
            b.core.setTapCount(8);
            for (int k = 0; k < 8; ++k)
                b.core.setTap(k, 0.02f * (float) (k + 1), k % 2 == 0 ? 1.0f : 0.0f, k % 2 == 0 ? 0.0f : 1.0f, 0.0f);
            b.settle();
            const auto out = b.run(impulse(0.3), silence(0.3));        // the right input is silent: the sum is half
            for (int k = 0; k < 8; ++k)
            {
                const size_t t = at(0.02 * (k + 1));
                const float l = peak(out.l, t - 2, t + 3), r = peak(out.r, t - 2, t + 3);
                expectWithinAbsoluteError(k % 2 == 0 ? l : r, 0.5f, 0.01f, "tap " + juce::String(k + 1));
                expect((k % 2 == 0 ? r : l) < 1.0e-5f);
            }
        }

        beginTest("Core: the loop filter before the line dulls the first echo too; in the feedback alone it leaves it bright");
        {
            auto firstEcho = [](bool before)
            {
                dly::Core::Setup s;
                s.filterInput = before;
                Bare b(s);
                b.core.setTapCount(1);
                b.core.setTap(0, 0.1f, 1.0f, 1.0f, 1.0f);
                b.core.setFilter(1000.0f, 0.0f);
                b.settle();
                const Signal in = joined(sine(0.05, 6000.0, 0.5f), silence(0.3));
                return rms(b.run(in, in).l, at(0.11), at(0.14));
            };
            const float dull = firstEcho(true), bright = firstEcho(false);
            expectWithinAbsoluteError(bright, 0.5f * 0.7071f, 0.02f);
            expect(dull < bright * 0.3f, juce::String(dull / bright, 3));
        }
    }

    // ---- the transport, and times that are glided to --------------------------------------------
    void testTransportAndGlide()
    {
        beginTest("Transport: off, the echo's pitch stands still; on, it wows and flutters; switched off again it is still again");
        {
            Bare b;
            b.core.setTapCount(1);
            b.core.setTap(0, 0.3f, 1.0f, 1.0f, 0.0f);
            b.core.setFilter(20000.0f, 0.0f);
            b.settle();
            const Signal tone = sine(3.0, 1000.0, 0.3f);
            const auto steady = b.run(tone, tone);
            expect(wobbleCents(steady.l, at(0.5), at(3.0)) < 1.0f, juce::String(wobbleCents(steady.l, at(0.5), at(3.0))));
            b.core.setTransport(1.0f);
            const auto wavering = b.run(tone, tone);
            const float cents = wobbleCents(wavering.l, at(0.5), at(3.0));
            logMessage("transport at full depth: " + juce::String(cents, 1) + " cents between the longest and the shortest period");
            expect(cents > 10.0f && cents < 80.0f, juce::String(cents));
            expectWithinAbsoluteError(rms(wavering.l, at(0.5), at(3.0)), 0.3f * 0.7071f, 0.01f);      // the level is what it was
            expect(sharpestCorner(wavering.l) < 0.01f, "it comes in without a step: " + juce::String(sharpestCorner(wavering.l), 4));
            b.core.setTransport(0.0f);
            b.run(tone, tone);
            const auto again = b.run(tone, tone);
            expect(wobbleCents(again.l, at(0.5), at(3.0)) < 1.0f);
        }

        beginTest("Transport: its two sines keep their size and rate for minutes (the coupled form does not drift)");
        {
            dly::Transport t;
            t.prepare(SR);
            t.reset();
            float most = 0.0f, late = 0.0f;
            int crossings = 0;
            float before = 0.0f;
            const int total = (int) (SR * 60.0);
            for (int i = 0; i < total; ++i)
            {
                const float v = t.next();
                most = std::max(most, std::abs(v));
                if (i > total - (int) (SR * 5.0)) late = std::max(late, std::abs(v));
                if (before < 0.0f && v >= 0.0f && std::abs(t.flutter.sin) < 0.2f) ++crossings;
                before = v;
            }
            const float whole = dly::Transport::WOW_DEPTH + dly::Transport::FLUTTER_DEPTH;
            expect(most < whole * 1.02f && late > whole * 0.9f, juce::String(most / whole, 4) + " " + juce::String(late / whole, 4));
        }

        beginTest("A time, a head count, a tap count, a spread or a way of reading changed under a tone: glided to, no step");
        {
            struct Move { const char* kind; const char* cell; float from, to; };
            const Move moves[] = {
                { "TAPE", "TIME", 0.2f, 0.31f }, { "TAPE", "HEAD", 1.0f, 4.0f }, { "TAPE", "HEAD", 4.0f, 2.0f }, { "TAPE", "WIDE", 0.0f, 1.0f },
                { "TAPE", "READ", 0.0f, 1.0f }, { "TAPE", "READ", 1.0f, 2.0f }, { "TAPE", "READ", 2.0f, 0.0f },
                { "LCR", "TIME", 0.2f, 0.12f }, { "LCR", "SPRD", 1.0f, 0.2f }, { "LCR", "WOW", 0.0f, 1.0f }, { "LCR", "DRIV", 0.0f, 1.0f },
                { "LCR", "READ", 0.0f, 3.0f },
                { "MTAP", "TIME", 0.2f, 0.3f }, { "MTAP", "TAPS", 4.0f, 8.0f }, { "MTAP", "TAPS", 8.0f, 1.0f }, { "MTAP", "READ", 0.0f, 1.0f },
                { "RVSE", "TIME", 0.2f, 0.3f }, { "RVSE", "TAPS", 1.0f, 4.0f }, { "RVSE", "READ", 1.0f, 0.0f }, { "RVSE", "READ", 0.0f, 4.0f },
                { "DUB", "TIME", 0.2f, 0.3f }, { "DUB", "HEAD", 1.0f, 3.0f }, { "DUB", "READ", 0.0f, 1.0f }, { "DUB", "DRIV", 0.0f, 1.0f },
                { "SPRG", "TENS", 0.2f, 0.9f }, { "SPRG", "PDLY", 0.0f, 0.2f }, { "SPRG", "MODE", 0.0f, 2.0f }, { "SPRG", "DISP", 1.0f, 0.0f },
                { "SHIM", "MODE", 0.0f, 3.0f }, { "SHIM", "SIZE", 1.0f, 0.5f }, { "SHIM", "DISP", 0.0f, 1.0f }, { "SHIM", "DISP", 1.0f, 0.0f },
                { "SHIM", "PDLY", 0.1f, 0.0f } };
            const Signal tone = sine(1.0, 220.0, 0.3f);
            // (A glide bends the pitch, sometimes far: that is a smooth sound. A jump is a corner.)
            for (const auto& m : moves)
                for (const auto& k : kinds())
                {
                    if (juce::String(k.name) != m.kind) continue;
                    auto fx = k.make();
                    fx->prepare(SR, 256);
                    fx->setMix(1.0f);
                    const int cell = cellNamed(*fx, m.cell);
                    expect(cell >= 0, juce::String(m.kind) + " has no " + m.cell);
                    if (cell < 0) continue;
                    if (juce::String(m.kind) == "SPRG" || juce::String(m.kind) == "SHIM")
                        setCell(*fx, cellNamed(*fx, "SHIM"), 0.6f);     // (so that MODE has something to act on)
                    setCell(*fx, cell, m.from);
                    fx->reset();
                    const auto before = through(*fx, tone);
                    setCell(*fx, cell, m.to);
                    const auto after = through(*fx, tone);
                    const auto later = through(*fx, tone);
                    const float steady = std::max(sharpestCorner(before, at(0.6)), sharpestCorner(later, at(0.6)));
                    const float moved = sharpestCorner(after);
                    expect(finite(after.l) && moved < std::max(0.02f, 4.0f * steady), juce::String(m.kind) + " " + m.cell + " " + juce::String(m.from, 2)
                           + " -> " + juce::String(m.to, 2) + ": a corner of " + juce::String(moved, 4) + " (steady " + juce::String(steady, 4) + ")");
                    expect(differenceRms(before.l, later.l, at(0.6), at(1.0)) > 1.0e-4f || differenceRms(before.r, later.r, at(0.6), at(1.0)) > 1.0e-4f,
                           juce::String(m.kind) + " " + m.cell + ": and the sound is another");
                }
        }
    }

    // ---- the moving heads: backwards, and at another pitch --------------------------------------
    void testHeads()
    {
        beginTest("Heads: the two weights add up to 1, the offsets stay within the span, for every way of reading");
        for (float speed : { -1.0f, 0.5f, 1.4983f, 2.0f, 2.9966f })
            for (float fade : { 0.1f, 1.0f })
            {
                dly::Heads h;
                h.set(speed, fade);
                h.reset();
                bool ok = true;
                float least = 2.0f;
                for (int i = 0; i < 20000; ++i)
                {
                    h.next(700.0f);
                    ok = ok && std::abs(h.weight[0] + h.weight[1] - 1.0f) < 1.0e-6f && h.weight[0] >= 0.0f && h.weight[1] >= 0.0f;
                    for (int k = 0; k < 2; ++k)
                        if (h.weight[k] > 0.0f) ok = ok && h.offset[k] >= -1.0e-3f && h.offset[k] <= h.span * 1.0001f;
                    least = std::min(least, std::max(h.weight[0], h.weight[1]));
                }
                expect(ok && least >= 0.5f, "speed " + juce::String(speed) + " fade " + juce::String(fade));
            }

        beginTest("Reverse: a stretch comes back backwards (the slow end first, the sharp start last), and a held tone has no gaps");
        {
            ReverseDelay fx;
            fx.prepare(SR, 256);
            fx.setParameter(ReverseDelay::Time, 0.5f);
            fx.setParameter(ReverseDelay::Feedback, 0.0f);
            fx.setParameter(ReverseDelay::HighCut, 20000.0f);
            fx.setParameter(ReverseDelay::Mix, 1.0f);
            fx.reset();
            // A pluck 0.1 s into a stretch: forwards its energy sits early in its 0.3 s, backwards late.
            const Signal in = joined(silence(0.1), pluck(1.9, 500.0, 0.3));
            const auto out = through(fx, in);
            const float dryPlace = centroid(in, at(0.1), at(0.4));
            const size_t loudest = peakAt(out.l, 0, out.l.size());
            const float wetPlace = centroid(out.l, loudest - at(0.29), loudest + at(0.01));
            logMessage("pluck: energy at " + juce::String(dryPlace, 3) + " of its length going in, at " + juce::String(wetPlace, 3) + " coming back");
            expect(dryPlace < 0.25f && wetPlace > 0.75f, juce::String(dryPlace) + " " + juce::String(wetPlace));
            expect(rms(out.l, at(0.6), at(1.4)) > 0.3f * rms(in, at(0.1), at(0.4)), "and it is all there");

            fx.setExtra(ReverseDelay::Read, (float) dly::read::Forward);
            fx.reset();
            const auto forwards = through(fx, in);
            const size_t loudestForwards = peakAt(forwards.l, 0, forwards.l.size());
            expect(centroid(forwards.l, loudestForwards - at(0.01), loudestForwards + at(0.29)) < 0.25f, "READ FWD: a plain echo");

            fx.setExtra(ReverseDelay::Read, (float) dly::read::Reverse);
            fx.reset();
            const Signal tone = sine(4.0, 300.0, 0.3f);
            const auto held = through(fx, tone);
            float lowest = 1.0f, highest = 0.0f;
            for (double t = 1.0; t < 3.9; t += 0.02)
            {
                const float level = rms(held.l, at(t), at(t + 0.02));
                lowest = std::min(lowest, level);
                highest = std::max(highest, level);
            }
            logMessage("held tone backwards: level between " + juce::String(lowest, 4) + " and " + juce::String(highest, 4));
            // (Two heads crossfade: where they meet, the two copies of the tone may be out of step.)
            expect(highest < 0.3f * 0.7071f * 1.05f && lowest > 0.0f, juce::String(lowest) + " " + juce::String(highest));
            expect(rms(held.l, at(1.0), at(3.9)) > 0.3f * 0.7071f * 0.85f, juce::String(rms(held.l, at(1.0), at(3.9))));
        }

        beginTest("READ on every delay: OCT+ doubles the pitch of each repeat, 5TH+ raises it a fifth, OCT- halves it, REV keeps it");
        for (const auto& k : kinds())
        {
            if (! k.delay) continue;
            auto pitchOf = [&](int mode, double from, double to)
            {
                auto fx = k.make();
                fx->prepare(SR, 256);
                fx->setMix(1.0f);
                setCell(*fx, cellNamed(*fx, "TIME"), 0.4f);
                setCell(*fx, cellNamed(*fx, "FDBK"), 0.0f);
                for (const char* clean : { "AGE", "WOW", "DRIV" })
                    if (cellNamed(*fx, clean) >= 0) setCell(*fx, cellNamed(*fx, clean), 0.0f);
                if (cellNamed(*fx, "TAPS") >= 0) setCell(*fx, cellNamed(*fx, "TAPS"), 1.0f);
                setCell(*fx, cellNamed(*fx, "READ"), (float) mode);
                fx->reset();
                const auto out = through(*fx, sine(2.0, 440.0, 0.3f));
                return crossingsPerSecond(out.l, at(from), at(to));
            };
            const juce::String name = k.name;
            expectWithinAbsoluteError(pitchOf(dly::read::Forward, 1.0, 2.0), 440.0f, 3.0f, name + " FWD");
            expectWithinAbsoluteError(pitchOf(dly::read::Reverse, 1.0, 2.0), 440.0f, 8.0f, name + " REV");
            expectWithinAbsoluteError(pitchOf(dly::read::OctaveUp, 1.0, 2.0), 880.0f, 30.0f, name + " OCT+");
            expectWithinAbsoluteError(pitchOf(dly::read::FifthUp, 1.0, 2.0), 659.3f, 25.0f, name + " 5TH+");
            // (Sixty-six times a second one head takes over from the other: the joins add a crossing now and then.)
            expectWithinAbsoluteError(pitchOf(dly::read::OctaveDown, 1.0, 2.0), 225.0f, 20.0f, name + " OCT-");
        }

        beginTest("OCT+ in a loop: each time round another octave (the second repeat is two octaves up)");
        {
            TapeDelay fx;
            fx.prepare(SR, 256);
            fx.setParameter(TapeDelay::Time, 0.5f);
            fx.setParameter(TapeDelay::Feedback, 0.8f);
            fx.setParameter(TapeDelay::Age, 0.0f);
            fx.setParameter(TapeDelay::Mix, 1.0f);
            fx.setExtra(TapeDelay::Read, (float) dly::read::OctaveUp);
            fx.reset();
            const auto out = through(fx, joined(sine(0.3, 220.0, 0.3f), silence(1.7)));
            expectWithinAbsoluteError(crossingsPerSecond(out.l, at(0.56), at(0.76)), 440.0f, 25.0f);
            expectWithinAbsoluteError(crossingsPerSecond(out.l, at(1.06), at(1.26)), 880.0f, 60.0f);
            expectWithinAbsoluteError(crossingsPerSecond(out.l, at(1.56), at(1.76)), 1760.0f, 140.0f);
        }
    }

    // ---- a curve in the loop, held ---------------------------------------------------------------
    void testCurveInTheLoop()
    {
        beginTest("Tape: every curve passes a quiet repeat at unity, and holds the loop at the top of FDBK (howling, never running away)");
        {
            auto echoLevel = [](int c)
            {
                TapeDelay fx;
                fx.prepare(SR, 64);
                fx.setParameter(TapeDelay::Time, 0.25f);
                fx.setParameter(TapeDelay::Feedback, 0.0f);
                fx.setParameter(TapeDelay::Age, 0.0f);
                fx.setParameter(TapeDelay::Mix, 1.0f);
                fx.setExtra(TapeDelay::Sat, (float) c);
                fx.reset();
                return rms(through(fx, sine(0.6, 600.0, 0.002f)).l, 14400, 26400);
            };
            const float reference = echoLevel(sat::Fast);
            expectWithinAbsoluteError(reference, 0.002f * 0.7071f, 0.0001f);
            int howling = 0;
            for (int c = 0; c < sat::Count; ++c)
            {
                const juce::String name = sat::names()[c];
                const float level = echoLevel(c);
                // The tape works its curve at the centre (the dub echo off-centre): there RECT turns a quiet tone
                // into its octave and XOVR, whose dead zone it is, lets nothing quiet through (the repeats stop short).
                if (c == sat::Crossover)     expect(level < 0.01f * reference, name + ": quiet repeats are gated, " + juce::String(level / reference, 3));
                else if (c != sat::Rectify)  expect(level > 0.9f * reference && level < 1.1f * reference, name + ": a quiet repeat at unity, " + juce::String(level / reference, 3));
                for (float age : { 0.0f, 1.0f })
                {
                    if (age > 0.0f && c % 5 != 0) continue;     // (the old tape with every fifth curve)
                    TapeDelay fx;
                    fx.prepare(SR, 64);
                    fx.setParameter(TapeDelay::Time, 0.05f);
                    fx.setParameter(TapeDelay::Feedback, 1.0f);
                    fx.setParameter(TapeDelay::Age, age);
                    fx.setParameter(TapeDelay::Mix, 1.0f);
                    fx.setExtra(TapeDelay::Sat, (float) c);
                    fx.reset();
                    expectEquals((int) fx.getExtra(TapeDelay::Sat), c);
                    const auto loud = through(fx, noise(0.3, 0.7f));
                    Stereo last;
                    for (int i = 0; i < 3; ++i) last = through(fx, silence(0.7));
                    expect(finite(loud.l) && finite(last.l) && peak(loud.l) < 8.0f && peak(last.l) < 8.0f,
                           name + " AGE " + juce::String(age, 0) + ": " + juce::String(peak(loud.l)) + " " + juce::String(peak(last.l)));
                    if (age == 0.0f && rms(last.l) > 0.02f) ++howling;
                }
            }
            logMessage(juce::String(howling) + " of " + juce::String((int) sat::Count) + " curves keep the new tape howling at the top of FDBK");
            expect(howling >= sat::Count / 2, juce::String(howling));
        }

        beginTest("L/C/R, multi-tap, reverse: DRIV 0 is a clean loop (twice the input, twice the output); driven, a hot repeat is squashed "
                  "and the curve is heard; at the top of FDBK it still dies away");
        for (const auto& k : kinds())
        {
            const juce::String name = k.name;
            if (name != "LCR" && name != "MTAP" && name != "RVSE") continue;
            auto repeat = [&](float driv, int curve, float amp)
            {
                auto fx = k.make();
                fx->prepare(SR, 256);
                fx->setMix(1.0f);
                setCell(*fx, cellNamed(*fx, "TIME"), 0.2f);
                setCell(*fx, cellNamed(*fx, "FDBK"), 0.6f);
                setCell(*fx, cellNamed(*fx, "DRIV"), driv);
                setCell(*fx, cellNamed(*fx, "SAT"), (float) curve);
                if (cellNamed(*fx, "READ") >= 0) setCell(*fx, cellNamed(*fx, "READ"), 0.0f);
                fx->reset();
                return through(*fx, joined(sine(0.2, 300.0, amp), silence(1.0)));
            };
            const auto quiet = repeat(0.0f, sat::Fast, 0.4f), loud = repeat(0.0f, sat::Fast, 0.8f);
            Signal twice = quiet.l;
            for (auto& v : twice) v *= 2.0f;
            expect(differenceRms(twice, loud.l, 0, loud.l.size()) < 1.0e-6f, name + ": clean at DRIV 0, " + juce::String(differenceRms(twice, loud.l, 0, loud.l.size()), 8));

            const auto drivenQuiet = repeat(1.0f, sat::Fast, 0.01f), drivenLoud = repeat(1.0f, sat::Fast, 0.8f);
            const float small = rms(drivenQuiet.l, at(0.5), at(1.1)) / 0.01f, large = rms(drivenLoud.l, at(0.5), at(1.1)) / 0.8f;
            logMessage(name + " DRIV 1: later repeats of a quiet tone " + juce::String(small, 3) + " of it, of a loud one " + juce::String(large, 3));
            expect(large < small * 0.7f, name + ": a hot repeat is squashed, " + juce::String(large / small, 3));
            const auto folded = repeat(1.0f, sat::Fold, 0.8f);
            expect(differenceRms(folded.l, drivenLoud.l, at(0.3), at(1.1)) > 0.01f, name + ": SAT is heard");

            for (float driv : { 0.0f, 0.05f, 1.0f })
                for (int curve : { (int) sat::Fast, (int) sat::Soft, (int) sat::Cheby3 })
                {
                    if (driv == 0.05f && curve != sat::Soft) continue;      // (half in: with the curve that has no ceiling)
                    if (driv == 0.0f && curve != sat::Fast) continue;       // (clean: the curve is not in it)
                    auto fx = k.make();
                    fx->prepare(SR, 256);
                    fx->setMix(1.0f);
                    setCell(*fx, cellNamed(*fx, "TIME"), 0.1f);
                    setCell(*fx, cellNamed(*fx, "FDBK"), 10.0f);        // (held to its top)
                    setCell(*fx, cellNamed(*fx, "DRIV"), driv);
                    setCell(*fx, cellNamed(*fx, "SAT"), (float) curve);
                    fx->reset();
                    const auto playing = through(*fx, noise(0.5, 0.7f));
                    Stereo late;
                    for (int i = 0; i < 7; ++i) late = through(*fx, silence(1.0));
                    expect(finite(playing.l) && finite(late.l) && peak(playing.l) < 8.0f,
                           name + " DRIV " + juce::String(driv, 2) + " " + sat::names()[curve] + ": peak " + juce::String(peak(playing.l)));
                    // (0.95 a round trip: about a twentieth after six seconds; a driven loop was held low while it played.)
                    expect(rms(late.l) < 0.3f * rms(playing.l), name + " DRIV " + juce::String(driv, 2) + " " + sat::names()[curve]
                           + " dies away: " + juce::String(rms(late.l) / rms(playing.l), 4));
                }
        }
    }

    // ---- what each delay has from the others ----------------------------------------------------
    void testWhatEachDelayGained()
    {
        beginTest("Tape and dub: HEAD gives that many evenly spaced heads, the last feeding back; WIDE puts the right side later");
        for (const auto& k : kinds())
        {
            const juce::String name = k.name;
            if (name != "TAPE" && name != "DUB") continue;
            auto fx = k.make();
            fx->prepare(SR, 256);
            fx->setMix(1.0f);
            setCell(*fx, cellNamed(*fx, "TIME"), 0.3f);
            setCell(*fx, cellNamed(*fx, "FDBK"), 0.4f);
            if (name == "TAPE") setCell(*fx, cellNamed(*fx, "AGE"), 0.0f);
            if (name == "DUB") { setCell(*fx, cellNamed(*fx, "DRIV"), 0.0f); setCell(*fx, cellNamed(*fx, "TONE"), 0.5f); }
            setCell(*fx, cellNamed(*fx, "HEAD"), 3.0f);
            fx->reset();
            // (A short burst, not a click: the dub echo's band rings on a click.)
            const Signal in = joined(sine(0.02, 700.0, 0.2f), silence(0.9));
            const auto out = through(*fx, in);
            const float first = rms(out.l, at(0.1), at(0.125)), second = rms(out.l, at(0.2), at(0.225)), third = rms(out.l, at(0.3), at(0.325));
            const float between = rms(out.l, at(0.14), at(0.19)), next = rms(out.l, at(0.4), at(0.425));
            logMessage(name + " HEAD 3: heads " + juce::String(first, 4) + " " + juce::String(second, 4) + " " + juce::String(third, 4)
                       + ", between " + juce::String(between, 5) + ", round again " + juce::String(next, 4));
            expect(first > 0.03f && second > 0.03f && third > first * 1.2f, name + ": three heads, the last the loudest");
            expectWithinAbsoluteError(first / third, 0.7f, 0.12f);
            expect(between < first * 0.2f, name + ": nothing between two heads");
            expect(next > 0.1f * first && next < first, name + ": the loop comes round by the last head");
            expect(rms(out.l, 0, at(0.09)) < 1.0e-4f);

            setCell(*fx, cellNamed(*fx, "HEAD"), 1.0f);
            setCell(*fx, cellNamed(*fx, "FDBK"), 0.0f);
            fx->reset();
            const auto one = through(*fx, in);
            expect(rms(one.l, at(0.1), at(0.25)) < 1.0e-3f && rms(one.l, at(0.3), at(0.325)) > 0.03f, name + ": HEAD 1 is the echo alone");
            const size_t together = peakAt(one.r, at(0.28), at(0.36));
            setCell(*fx, cellNamed(*fx, "WIDE"), 1.0f);
            fx->reset();
            const auto wide = through(*fx, in);
            const int later = (int) peakAt(wide.r, at(0.28), at(0.36)) - (int) together;
            expectWithinAbsoluteError((float) later, (float) (dly::MAX_WIDE * SR), 8.0f, name + " WIDE");
            expectEquals((int) peakAt(wide.l, at(0.28), at(0.36)), (int) peakAt(one.l, at(0.28), at(0.36)), name + ": the left side stays");
        }

        beginTest("L/C/R and multi-tap: WOW makes the taps waver like a tape (and at 0 they stand still)");
        for (const auto& k : kinds())
        {
            const juce::String name = k.name;
            if (name != "LCR" && name != "MTAP") continue;
            auto wobble = [&](float wow)
            {
                auto fx = k.make();
                fx->prepare(SR, 256);
                fx->setMix(1.0f);
                setCell(*fx, cellNamed(*fx, "TIME"), 0.4f);
                setCell(*fx, cellNamed(*fx, "FDBK"), 0.0f);
                if (name == "LCR") setCell(*fx, cellNamed(*fx, "SPRD"), 0.0f);
                if (name == "MTAP") setCell(*fx, cellNamed(*fx, "TAPS"), 1.0f);
                setCell(*fx, cellNamed(*fx, "WOW"), wow);
                fx->reset();
                const auto out = through(*fx, sine(3.0, 1000.0, 0.2f));
                return wobbleCents(out.l, at(0.6), at(3.0));
            };
            const float still = wobble(0.0f), wavering = wobble(1.0f);
            logMessage(name + " WOW: " + juce::String(still, 2) + " cents at 0, " + juce::String(wavering, 1) + " at 1");
            expect(still < 1.0f && wavering > 10.0f && wavering < 120.0f, name);
        }

        beginTest("Reverse: TAPS adds heads TIME / TAPS apart, each playing its own stretches backwards: everything is heard again "
                  "on that grid, quieter and to one side");
        {
            auto run = [&](int taps)
            {
                ReverseDelay fx;
                fx.prepare(SR, 256);
                fx.setParameter(ReverseDelay::Time, 0.4f);
                fx.setParameter(ReverseDelay::Feedback, 0.0f);
                fx.setParameter(ReverseDelay::Mix, 1.0f);
                fx.setExtra(ReverseDelay::Taps, (float) taps);
                fx.reset();
                return through(fx, joined(joined(silence(0.25), sine(0.05, 800.0, 0.3f)), silence(1.2)));
            };
            const auto one = run(1), two = run(2);
            // The burst at 0.25 - 0.3 s: the first head plays the stretch 0 - 0.4 s backwards from 0.4 s on (the burst at
            // 0.5 s); the second head, 0.2 s behind, has it in its stretch 0.2 - 0.6 s, played from 0.8 s on (at 1.1 s).
            const float main = rms(one.l, at(0.48), at(0.57));
            expect(main > 0.1f && rms(one.l, at(0.6), at(1.5)) < 1.0e-4f, "one head: once, " + juce::String(main, 4));
            expectWithinAbsoluteError(rms(two.l, at(0.48), at(0.57)), main, 0.01f);
            const float l = rms(two.l, at(1.08), at(1.17)), r = rms(two.r, at(1.08), at(1.17));
            logMessage("reverse, two heads: the stretch " + juce::String(main, 4) + ", again " + juce::String(l, 4) + " left, " + juce::String(r, 4) + " right");
            expect(l > 0.3f * main && l < 0.7f * main && r < 0.6f * l, "the second head: quieter, to one side");
            expect(rms(two.l, at(0.6), at(1.05)) < 1.0e-4f, "and nowhere else");
        }
    }

    // ---- the reverb's parts ----------------------------------------------------------------------
    void testReverbParts()
    {
        beginTest("Allpass: loses and adds nothing (the energy of a click is kept), whatever its coefficient");
        for (float g : { 0.0f, 0.5f, 0.7f, -0.6f })
        {
            rvb::Allpass ap;
            ap.prepare(600);
            double energy = 0.0;
            float after = 0.0f;
            for (int i = 0; i < 200000; ++i)
            {
                const float y = ap.process(i == 0 ? 1.0f : 0.0f, 441.0f, g);
                energy += (double) y * (double) y;
                if (i > 190000) after = std::max(after, std::abs(y));
            }
            expectWithinAbsoluteError((float) energy, 1.0f, 2.0e-3f, "g " + juce::String(g));
            expect(after < 1.0e-6f);
        }

        beginTest("Dispersion: energy kept; at 0 a plain delay; up, the top of the spring's band arrives after the bottom");
        {
            auto arrival = [](float a, double hz)
            {
                rvb::Dispersion d;
                d.prepare(SR);
                const Signal in = joined(silence(0.01), joined(pluck(0.02, hz, 0.02), silence(0.1)));
                Signal out(in.size());
                for (size_t i = 0; i < in.size(); ++i)
                {
                    float x0 = in[i], x1 = in[i];
                    d.process(x0, x1, a);
                    out[i] = x0;
                }
                return std::pair<float, float> { centroid(out, 0, out.size()) * (float) out.size(), rms(out) / rms(in) };
            };
            rvb::Dispersion d;
            d.prepare(SR);
            const float plain = (float) (rvb::Dispersion::STAGES * d.stretch);
            const auto low0 = arrival(0.0f, 400.0), high0 = arrival(0.0f, 3600.0);
            const auto low = arrival(0.7f, 400.0), high = arrival(0.7f, 3600.0);
            const float dry = centroid(joined(silence(0.01), joined(pluck(0.02, 400.0, 0.02), silence(0.1))), 0, at(0.13)) * (float) at(0.13);
            logMessage("dispersion 0.7: a 400 Hz pluck arrives " + juce::String(low.first - dry, 1) + " samples late, a 3.6 kHz one "
                       + juce::String(high.first - (high0.first - plain), 1) + " (plain delay " + juce::String(plain, 0) + ")");
            expectWithinAbsoluteError(low0.first - dry, plain, 1.5f);
            expect(high.first - high0.first > 3.0f * (low.first - low0.first) + 40.0f, "the top is later");
            expect(low.first < low0.first, "and the bottom earlier than the plain delay");
            for (const auto& r : { low0, high0, low, high })
                expectWithinAbsoluteError(r.second, 1.0f, 0.02f);
        }

        beginTest("Loop: dies away by 60 dB in DCAY (spring and shimmer), and a longer DCAY lasts longer");
        for (const auto& k : kinds())
        {
            if (k.delay) continue;
            const juce::String name = k.name;
            auto fall = [&](float decay)
            {
                auto fx = k.make();
                fx->prepare(SR, 256);
                fx->setMix(1.0f);
                setCell(*fx, cellNamed(*fx, "DCAY"), decay);
                setCell(*fx, cellNamed(*fx, "DAMP"), 0.0f);
                setCell(*fx, cellNamed(*fx, "SHIM"), 0.0f);
                fx->reset();
                const auto out = through(*fx, joined(noise(0.5, 0.25f), silence(decay + 0.5)));
                const float early = rms(out.l, at(0.55), at(0.75)), late = rms(out.l, at(0.55 + 0.5 * decay), at(0.75 + 0.5 * decay));
                return 20.0f * std::log10(std::max(late, 1.0e-9f) / std::max(early, 1.0e-9f));
            };
            const float two = fall(2.0f), four = fall(4.0f);
            logMessage(name + ": half of DCAY later the tail is " + juce::String(two, 1) + " dB down (DCAY 2 s), " + juce::String(four, 1) + " dB (4 s)");
            // (30 dB if nothing but the loop gain acted; the dispersion and the low-pass take a little more.)
            expect(two < -24.0f && two > -45.0f, name + " " + juce::String(two));
            expect(four < -24.0f && four > -45.0f, name + " " + juce::String(four));
        }
    }

    void testWhatEachReverbGained()
    {
        auto tailOf = [](CellEffect& fx, const Signal& in) { fx.reset(); return through(fx, in); };
        const Signal note = joined(sine(0.5, 330.0, 0.3f), silence(2.5));

        beginTest("Spring: DISP chirps (a click's echoes spread out), TENS sets the round trip, and it has the shimmer's heads (SHIM, MODE)");
        {
            SpringReverb fx;
            fx.prepare(SR, 256);
            fx.setMix(1.0f);
            fx.setParameter(SpringReverb::Damping, 0.0f);
            fx.setParameter(SpringReverb::Decay, 3.0f);

            // The first answer to a click arrives after one round trip: shorter the tighter the spring.
            auto firstAnswer = [&](float tension)
            {
                fx.setParameter(SpringReverb::Tension, tension);
                fx.setParameter(SpringReverb::Dispersion, 0.0f);
                const auto out = tailOf(fx, impulse(0.5));
                size_t i = 0;
                while (i < out.l.size() && std::abs(out.l[i]) < 0.05f) ++i;
                return (float) i / (float) SR;
            };
            const float slack = firstAnswer(0.0f), tight = firstAnswer(1.0f);
            logMessage("spring: the first answer after " + juce::String(slack * 1000.0f, 1) + " ms slack, " + juce::String(tight * 1000.0f, 1) + " ms tight");
            expectWithinAbsoluteError(slack, 0.041f * SpringReverb::sizeFor(0.0f), 0.004f);
            expectWithinAbsoluteError(tight, 0.041f * SpringReverb::sizeFor(1.0f), 0.004f);

            // Dispersion: after ten round trips a click has become a chirp, its energy spread in time.
            auto spreadOf = [&](float disp)
            {
                fx.setParameter(SpringReverb::Tension, 0.5f);
                fx.setParameter(SpringReverb::Dispersion, disp);
                const auto out = tailOf(fx, impulse(1.0));
                // How much of the energy of the stretch 0.3 - 0.6 s sits in its loudest 2 %.
                Signal power;
                for (size_t i = at(0.3); i < at(0.6); ++i) power.push_back(out.l[i] * out.l[i]);
                std::sort(power.begin(), power.end(), std::greater<float>());
                double top = 0.0, all = 0.0;
                for (size_t i = 0; i < power.size(); ++i) { all += power[i]; if (i < power.size() / 50) top += power[i]; }
                return (float) (top / std::max(all, 1.0e-30));
            };
            const float clicks = spreadOf(0.0f), chirps = spreadOf(1.0f);
            logMessage("spring: the loudest 2 % of the tail holds " + juce::String(clicks, 3) + " of its energy at DISP 0, " + juce::String(chirps, 3) + " at DISP 1");
            expect(clicks > 0.5f && chirps < 0.6f * clicks, juce::String(clicks) + " " + juce::String(chirps));

            fx.setParameter(SpringReverb::Dispersion, 0.6f);
            const auto plain = tailOf(fx, note);
            fx.setExtra(SpringReverb::Shimmer, 0.7f);
            const auto shimmered = tailOf(fx, note);
            // (A spring's round trip is short: what climbs an octave every 30 ms is out of its band at once.
            // So it is heard while the note plays, as octaves over it; the long tail is what did not climb.)
            const float before = rmsAbove(plain.l, 600.0, at(0.2), at(0.5)) / rms(plain.l, at(0.2), at(0.5));
            const float after = rmsAbove(shimmered.l, 600.0, at(0.2), at(0.5)) / rms(shimmered.l, at(0.2), at(0.5));
            logMessage("spring with SHIM: share of the sound above 600 Hz " + juce::String(before, 4) + " -> " + juce::String(after, 4));
            expect(after > 2.0f * before, "octaves over the note");
            fx.setExtra(SpringReverb::ShiftMode, (float) rvb::shift::Sub);
            const auto sunk = tailOf(fx, note);
            const float down = rmsAbove(sunk.l, 600.0, at(0.2), at(0.5)) / rms(sunk.l, at(0.2), at(0.5));
            expect(down < 0.6f * after, "MODE SUB: it sinks instead, " + juce::String(down, 4));
        }

        beginTest("Shimmer: SHIM and MODE as before; it has the spring's dispersion (DISP), and SIZE, PDLY and MOD");
        {
            ShimmerReverb fx;
            fx.prepare(SR, 256);
            fx.setMix(1.0f);
            fx.setParameter(ShimmerReverb::Damping, 0.0f);
            auto bright = [&](const Stereo& s) { return rmsAbove(s.l, 1500.0, at(0.8), at(2.5)) / std::max(1.0e-9f, rms(s.l, at(0.8), at(2.5))); };
            fx.setParameter(ShimmerReverb::Shimmer, 0.0f);
            const auto plain = tailOf(fx, note);
            fx.setParameter(ShimmerReverb::Shimmer, 0.8f);
            const auto up = tailOf(fx, note);
            fx.setParameter(ShimmerReverb::ShiftMode, (float) rvb::shift::Sub);
            const auto down = tailOf(fx, note);
            logMessage("shimmer: share of the tail above 1.5 kHz: none " + juce::String(bright(plain), 4) + ", OCT " + juce::String(bright(up), 4)
                       + ", SUB " + juce::String(bright(down), 4));
            expect(bright(up) > 3.0f * bright(plain) && bright(down) < 0.5f * bright(up));
            expect(rms(plain.l, at(0.8), at(2.5)) > 0.003f, "there is a tail");

            fx.setParameter(ShimmerReverb::Shimmer, 0.0f);
            fx.setParameter(ShimmerReverb::ShiftMode, 0.0f);
            fx.setExtra(ShimmerReverb::Modulation, 0.0f);       // (a moving tank answers differently every time)
            // PDLY: the same answer, later.
            const auto soon = tailOf(fx, impulse(1.0));
            fx.setExtra(ShimmerReverb::PreDelay, 0.1f);
            const auto later = tailOf(fx, impulse(1.0));
            Signal shifted(soon.l.size(), 0.0f);
            for (size_t i = at(0.1); i < shifted.size(); ++i) shifted[i] = soon.l[i - at(0.1)];
            expect(differenceRms(shifted, later.l, 0, at(0.9)) < 0.02f * rms(soon.l, 0, at(0.8)), "PDLY 0.1 s: the same, 0.1 s later");
            expect(peak(later.l, 0, at(0.1)) < 1.0e-6f);
            fx.setExtra(ShimmerReverb::PreDelay, 0.0f);

            // SIZE: the first answer comes sooner from a smaller tank.
            auto firstAnswer = [&](float size)
            {
                fx.setExtra(ShimmerReverb::Size, size);
                const auto out = tailOf(fx, impulse(1.0));
                const float loud = 0.1f * peak(out.l);
                size_t i = 0;
                while (i < out.l.size() && std::abs(out.l[i]) < loud) ++i;
                return (float) i / (float) SR;
            };
            const float small = firstAnswer(0.25f), large = firstAnswer(2.0f);
            logMessage("shimmer: the tank first answers after " + juce::String(small * 1000.0f, 1) + " ms at SIZE 0.25, " + juce::String(large * 1000.0f, 1) + " ms at 2");
            expect(small < 0.5f * large, "SIZE: " + juce::String(small, 3) + " " + juce::String(large, 3));
            fx.setExtra(ShimmerReverb::Size, 1.0f);

            // DISP: the tank chirps (it changes the tail, keeps about its level), and at 0 nothing of it runs.
            fx.setExtra(ShimmerReverb::Modulation, 0.0f);
            const auto dry = tailOf(fx, note);
            fx.setExtra(ShimmerReverb::Dispersion, 1.0f);
            const auto chirped = tailOf(fx, note);
            expect(differenceRms(dry.l, chirped.l, at(0.6), at(2.0)) > 0.3f * rms(dry.l, at(0.6), at(2.0)), "DISP changes the tail");
            const float ratio = rms(chirped.l, at(0.6), at(2.0)) / rms(dry.l, at(0.6), at(2.0));
            expect(ratio > 0.4f && ratio < 2.0f, "and keeps its level: " + juce::String(ratio, 3));
            fx.setExtra(ShimmerReverb::Dispersion, 0.0f);

            // MOD: the tail is moved (another tail), its level about the same.
            fx.setExtra(ShimmerReverb::Modulation, 1.0f);
            const auto swayed = tailOf(fx, note);
            expect(differenceRms(dry.l, swayed.l, at(0.6), at(2.0)) > 0.2f * rms(dry.l, at(0.6), at(2.0)), "MOD moves the tail");
            const float kept = rms(swayed.l, at(0.6), at(2.0)) / rms(dry.l, at(0.6), at(2.0));
            expect(kept > 0.5f && kept < 1.6f, juce::String(kept, 3));
        }
    }

    // ---- EffectBase::modulateCell ---------------------------------------------------------------
    void testModulation()
    {
        beginTest("modulateCell: a cell reads as it was set; what it is worth moves by the offset's share of its range (a time by ratio); "
                  "lists and counts stay; what is not a number is no offset");
        for (const auto& k : kinds())
        {
            auto fx = k.make();
            const juce::String name = k.name;
            for (int c = 0; c < CellEffect::CELLS; ++c)
            {
                const ParamSpec spec = specOf(*fx, c);
                expect(spec.isValid(), name + " cell " + juce::String(c));
                if (! spec.isValid()) continue;
                const juce::String what = name + " " + spec.label;
                expectEquals(spec.id, c % 4, what);
                expectWithinAbsoluteError(getCell(*fx, c), spec.defaultVal, 1.0e-5f, what + " starts at its default");
                const float set = getCell(*fx, c);
                for (float offset : { 1.0f, -1.0f, 0.25f, 5.0f })
                {
                    fx->modulateCell(c, offset);
                    expectWithinAbsoluteError(getCell(*fx, c), set, 1.0e-6f, what + " reads as set");
                    const float worth = fx->getMovedCell(c);
                    if (spec.isDiscrete())
                        expectWithinAbsoluteError(worth, set, 1.0e-6f, what + " is not moved");
                    else if (offset >= 1.0f)
                        expectWithinAbsoluteError(worth, spec.maxVal, 1.0e-4f * spec.maxVal, what + " at its top");
                    else if (offset <= -1.0f)
                        expectWithinAbsoluteError(worth, spec.minVal, 1.0e-4f * std::max(1.0f, spec.minVal), what + " at its bottom");
                    else if (spec.kind == ParamKind::Log)
                        expectWithinAbsoluteError(worth, std::min(spec.maxVal, set * std::pow(spec.maxVal / spec.minVal, offset)), 1.0e-3f * spec.maxVal, what);
                    else
                        expectWithinAbsoluteError(worth, std::min(spec.maxVal, set + offset * (spec.maxVal - spec.minVal)), 1.0e-5f, what);
                }
                fx->modulateCell(c, std::numeric_limits<float>::quiet_NaN());
                expectWithinAbsoluteError(fx->getMovedCell(c), set, 1.0e-6f, what + ": not a number = none");
            }
            fx->modulateCell(-1, 1.0f);
            fx->modulateCell(CellEffect::CELLS, 1.0f);      // (no such cell: nothing happens)
        }

        beginTest("modulateCell is heard exactly as the cell set to the moved value would be: every time, feedback, tone and mix");
        for (const auto& k : kinds())
        {
            const juce::String name = k.name;
            const Signal in = joined(noise(0.2, 0.2f, 99), silence(0.6));
            for (int c = 0; c < CellEffect::CELLS; ++c)
            {
                auto plain = k.make(), moved = k.make(), set = k.make();
                const ParamSpec spec = specOf(*plain, c);
                if (! spec.isValid() || spec.isDiscrete()) continue;
                const juce::String what = name + " " + spec.label;
                for (auto* fx : { plain.get(), moved.get(), set.get() })
                {
                    fx->prepare(SR, 256);
                    if (! k.delay) fx->setMix(0.6f);
                    // (Somewhere every cell is heard: a loop that comes round, a shimmer that is on.)
                    for (const char* on : { "FDBK", "SHIM", "DRIV", "AGE" })
                        if (cellNamed(*fx, on) >= 0 && cellNamed(*fx, on) != c) setCell(*fx, cellNamed(*fx, on), 0.5f);
                    if (cellNamed(*fx, "TIME") >= 0 && cellNamed(*fx, "TIME") != c) setCell(*fx, cellNamed(*fx, "TIME"), 0.1f);
                    if (cellNamed(*fx, "HEAD") >= 0) setCell(*fx, cellNamed(*fx, "HEAD"), 2.0f);
                }
                const float offset = getCell(*plain, c) > 0.5f * (spec.minVal + spec.maxVal) ? -0.3f : 0.3f;
                moved->modulateCell(c, offset);
                setCell(*set, c, moved->getMovedCell(c));
                expect(std::abs(getCell(*set, c) - getCell(*plain, c)) > 1.0e-4f * spec.maxVal, what + ": the offset moves it");
                for (auto* fx : { plain.get(), moved.get(), set.get() }) fx->reset();
                const auto a = through(*plain, in), b = through(*moved, in), s = through(*set, in);
                const float level = std::max(1.0e-6f, std::max(rms(a.l) + rms(a.r), rms(s.l) + rms(s.r)));
                const float heard = (differenceRms(a.l, b.l, 0, a.l.size()) + differenceRms(a.r, b.r, 0, a.r.size())) / level;
                const float same = (differenceRms(s.l, b.l, 0, s.l.size()) + differenceRms(s.r, b.r, 0, s.r.size())) / level;
                expect(heard > 0.01f, what + ": modulation is heard, " + juce::String(heard, 4));
                expect(same < 1.0e-5f, what + ": as the cell set there, " + juce::String(same, 7));
            }
        }

        beginTest("modulateCell: an offset that changes every block is glided to (no step in a tone), and taken away it leaves the cell's own sound");
        for (const auto& k : kinds())
        {
            const juce::String name = k.name;
            for (int c = 0; c < CellEffect::CELLS; ++c)
            {
                auto fx = k.make();
                const ParamSpec spec = specOf(*fx, c);
                if (! spec.isValid() || spec.isDiscrete()) continue;
                fx->prepare(SR, 256);
                if (! k.delay) fx->setMix(0.6f);
                fx->reset();
                const Signal tone = sine(1.0, 220.0, 0.3f);
                const auto steady = through(*fx, tone);
                Stereo out { tone, tone };
                uint32_t seed = 5;
                for (size_t done = 0; done < tone.size(); done += 256)
                {
                    seed = seed * 1664525u + 1013904223u;
                    // A new offset every block: anywhere in +-0.2 of the range; for a time (which is glided to, and
                    // whose every change of course bends the pitch) the steps of a 2 Hz sine a tenth of the range deep.
                    if (spec.kind == ParamKind::Log && juce::String(spec.unit) == "s")
                        fx->modulateCell(c, 0.05f * (float) std::sin(6.283185307179586 * 2.0 * (double) done / SR));
                    else
                        fx->modulateCell(c, ((float) (seed >> 8) / 8388608.0f - 1.0f) * 0.2f);
                    fx->processBlock(out.l.data() + done, out.r.data() + done, (int) std::min<size_t>(256, tone.size() - done));
                }
                const float calm = sharpestCorner(steady, at(0.6)), moved = sharpestCorner(out);
                expect(finite(out.l) && moved < std::max(0.03f, 4.0f * calm), name + " " + spec.label + ": a corner of " + juce::String(moved, 4)
                       + " (steady " + juce::String(calm, 4) + ")");
            }
        }

        beginTest("modulateCell: the mix goes to where it is put in a straight line, 4 ms for a full swing, whoever moved it; "
                  "an effect that does not take modulation ignores it");
        {
            TapeDelay fx;
            fx.prepare(SR, 256);
            fx.setParameter(TapeDelay::Mix, 0.0f);
            fx.reset();
            const Signal in = noise(0.2, 0.5f);
            const auto dry = through(fx, in);
            expect(dry.l == in, "mix 0 is the input, to the bit");
            fx.setParameter(TapeDelay::Mix, 1.0f);
            fx.setParameter(TapeDelay::Time, 0.1f);
            fx.reset();
            const auto wet = through(fx, in);
            expect(peak(wet.l, 0, at(0.09)) < 1.0e-6f, "mix 1 has none of it");
            // Turned under sound, with no reset: a straight line to the new mix, there within 4 ms and
            // a look (CellEffect::MIX_SWING_SECONDS), and from then on the input to the bit.
            fx.setMix(0.0f);
            {
                const auto going = through(fx, in);
                const size_t there = (size_t) at((double) CellEffect::MIX_SWING_SECONDS) + (size_t) CellEffect::CONTROL;
                expect(! (going.l == in) && std::abs(going.l[there / 2] - in[there / 2]) > 1.0e-4f, "mix turned to 0: on its way after 2 ms");
                expect(std::equal(going.l.begin() + (long) there, going.l.end(), in.begin() + (long) there), "and the input, to the bit, after 4 ms");
                // (On the way the mix falls by the same amount every sample: with silence going in, what
                // is left of the echoes falls in a straight line too. "Effect Cores Mod" holds the bend.)
            }
            // (What is on the line does not depend on the mix: an effect that was at 1 all along says what
            // the wet sound is.)
            TapeDelay always;
            always.prepare(SR, 256);
            always.setParameter(TapeDelay::Mix, 1.0f);
            always.setParameter(TapeDelay::Time, 0.1f);
            always.reset();
            through(always, in);
            through(always, in);
            fx.setMix(1.0f);
            {
                const auto back = through(fx, in, 97).l, wetSound = through(always, in).l;
                const long there = (long) at((double) CellEffect::MIX_SWING_SECONDS) + CellEffect::CONTROL;
                expect(! (back == wetSound), "and back to 1: on its way");
                expect(std::equal(back.begin() + there, back.end(), wetSound.begin() + there), "the wet sound alone, to the bit, after 4 ms");
            }
            fx.modulateCell(TapeDelay::Mix, -1.0f);
            fx.reset();
            expect(through(fx, in).l == in, "mix modulated down to 0: the input");
            // Modulation is glided: the first samples after it arrives are on their way.
            fx.modulateCell(TapeDelay::Mix, 0.0f);
            fx.reset();
            through(fx, in);
            fx.modulateCell(TapeDelay::Mix, -1.0f);
            const auto arriving = through(fx, in);
            expect(! (arriving.l == in) && rms(arriving.l, 0, 8) > 0.0f, "an offset on the mix arrives over a few samples");
            expect(differenceRms(arriving.l, in, at(0.01), at(0.2)) < 1.0e-7f, "and has arrived a moment later");

            // (E7: the send delay is on the core now, and its cells are destinations like the others'.)
            Delay moved, same;
            moved.prepare(SR, 256);
            same.prepare(SR, 256);
            moved.modulateCell(2, -0.5f);
            expect(! (through(moved, in).l == through(same, in).l), "the send delay's MIX is a destination");
            moved.modulateCell(2, 0.0f);
            moved.reset();
            same.reset();
            expect(through(moved, in).l == through(same, in).l, "and an offset of 0 is the cell itself");
        }
    }

    // ---- the page convention, block sizes, absurd input ----------------------------------------
    void testContractAndBounds()
    {
        beginTest("All seven: eight cells with labels that fit, values held to their range and rounded where they are lists; "
                  "the same sound whatever the block size");
        for (const auto& k : kinds())
        {
            const juce::String name = k.name;
            auto fx = k.make();
            expect(! fx->getParamSpec(4).isValid() && ! fx->getExtraSpec(4).isValid() && ! fx->getParamSpec(-1).isValid());
            for (int c = 0; c < CellEffect::CELLS; ++c)
            {
                const ParamSpec spec = specOf(*fx, c);
                const juce::String what = name + " " + spec.label;
                const size_t len = std::strlen(spec.label);
                expect(len >= 1 && len <= 4, what);
                expect(spec.maxVal > spec.minVal && spec.defaultVal >= spec.minVal && spec.defaultVal <= spec.maxVal, what);
                if (spec.kind == ParamKind::Log) expect(spec.minVal > 0.0f, what);
                if (spec.kind == ParamKind::Choice) expect(spec.choices != nullptr && spec.numChoices == (int) spec.maxVal + 1, what);
                for (float v : { spec.maxVal, spec.minVal, spec.maxVal + 1.0e9f, spec.minVal - 1.0e9f })
                {
                    setCell(*fx, c, v);
                    expectWithinAbsoluteError(getCell(*fx, c), std::clamp(v, spec.minVal, spec.maxVal), 1.0e-4f * std::max(1.0f, spec.maxVal), what);
                }
                setCell(*fx, c, std::numeric_limits<float>::quiet_NaN());
                expect(std::isfinite(getCell(*fx, c)), what + ": what is not a number is not taken");
                if (spec.isDiscrete())
                {
                    setCell(*fx, c, spec.minVal + 0.6f);
                    expectWithinAbsoluteError(getCell(*fx, c), spec.minVal + 1.0f, 1.0e-6f, what + " is rounded");
                }
                setCell(*fx, c, spec.defaultVal);
            }
            // (A delay's TIME; since phase E6 a reverb's PDLY, a time-like cell of its second page: SPRG's first there, SHIM's second.)
            const int sync = fx->getSyncParamIndex();
            expectEquals(sync, k.delay ? 0 : (name == "SPRG" ? 4 : 5), name + ": TIME, or the pre-delay, is what tempo sync moves");

            const Signal in = joined(noise(0.15, 0.3f, 7), silence(0.25));
            Stereo reference;
            for (int block : { 256, 1, 7, 4096 })
            {
                auto one = k.make();
                one->prepare(SR, block);
                if (one->getMix() == 1.0f && ! k.delay) one->setMix(0.5f);
                // In the middle of it a cell is turned, at the same sample for every block size.
                Stereo out { in, in };
                size_t done = 0;
                auto run = [&](size_t until)
                {
                    while (done < until)
                    {
                        const int n = (int) std::min((size_t) block, until - done);
                        one->processBlock(out.l.data() + done, out.r.data() + done, n);
                        done += (size_t) n;
                    }
                };
                run(4800);
                setCell(*one, 0, specOf(*one, 0).minVal * 2.0f);
                run(in.size());
                if (block == 256) reference = out;
                else expect(out.l == reference.l && out.r == reference.r, name + ": blocks of " + juce::String(block) + " sound as blocks of 256");
            }
        }

        beginTest("All seven: bounded and finite for any input (huge, infinite, not a number), every cell at either end, "
                  "22.05 to 192 kHz; silent again afterwards unless the loop is set to howl");
        for (const auto& k : kinds())
            for (double sr : { 22050.0, 48000.0, 192000.0 })
            {
                const juce::String name = juce::String(k.name) + " at " + juce::String(sr, 0);
                for (int corner = 0; corner < (sr > 100000.0 ? 2 : 4); ++corner)
                {
                    auto fx = k.make();
                    fx->prepare(sr, 512);
                    for (int c = 0; c < CellEffect::CELLS; ++c)
                    {
                        const ParamSpec spec = specOf(*fx, c);
                        const bool top = corner == 0 || (corner == 2 && c % 2 == 0) || (corner == 3 && c % 2 == 1);
                        setCell(*fx, c, top ? spec.maxVal : spec.minVal);
                    }
                    fx->setMix(1.0f);
                    fx->reset();
                    Signal bad = noise(0.2, 1.0f, 3, sr);
                    const float nasty[] = { 1.0e30f, -1.0e30f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                            std::numeric_limits<float>::quiet_NaN(), 1.0e-30f, 64.0f, -3.0f };
                    for (size_t i = 0; i < bad.size(); i += 97) bad[i] = nasty[(i / 97) % 8];
                    const auto loud = through(*fx, bad, 512);
                    const auto after = through(*fx, silence(0.3, sr), 512);
                    expect(finite(loud.l) && finite(loud.r) && finite(after.l) && finite(after.r), name + " corner " + juce::String(corner) + " is finite");
                    const float most = std::max({ peak(loud.l), peak(loud.r), peak(after.l), peak(after.r) });
                    expect(most < 600.0f, name + " corner " + juce::String(corner) + ": peak " + juce::String(most));
                }
            }
    }

    // ---- what a review found untested (each of these fails when the line it holds is broken) ---
    void testReviewGaps()
    {
        beginTest("Tape: no curve brings a loud repeat back louder than it went in, and none howls far above full scale "
                  "(RECT and XOVR are flat at the centre: their gain is not made up for)");
        for (int c = 0; c < sat::Count; ++c)
        {
            const juce::String name = sat::names()[c];
            TapeDelay fx;
            fx.prepare(SR, 64);
            fx.setParameter(TapeDelay::Time, 0.25f);
            fx.setParameter(TapeDelay::Feedback, 0.0f);
            fx.setParameter(TapeDelay::Age, 0.0f);
            fx.setParameter(TapeDelay::Mix, 1.0f);
            fx.setExtra(TapeDelay::Sat, (float) c);
            fx.reset();
            const float echo = rms(through(fx, sine(0.6, 600.0, 0.5f)).l, 14400, 26400) / (0.5f * 0.7071f);
            expect(echo < 1.1f, name + ": a loud repeat against its input, " + juce::String(echo, 3));
            if (c == sat::Rectify || c == sat::Crossover) expect(echo > 0.3f, name + " still sounds: " + juce::String(echo, 3));

            fx.setParameter(TapeDelay::Time, 0.05f);
            fx.setParameter(TapeDelay::Feedback, 1.0f);
            fx.reset();
            through(fx, noise(0.3, 0.7f));
            Stereo last;
            for (int i = 0; i < 4; ++i) last = through(fx, silence(0.7));
            expect(peak(last.l) < 2.5f, name + ": howling at " + juce::String(peak(last.l), 3));
        }

        beginTest("SAT turned under sound, curve after curve: exchanged behind a gate, no step (tape, L/C/R driven, dub)");
        {
            // A curve with a corner of its own (a clip, a fold, a rectifier) puts corners into a tone all the
            // time: the exchange may be no sharper than the two curves it goes between are by themselves.
            auto stepThrough = [this](const std::function<std::unique_ptr<EffectBase>()>& make, int satCell, const char* name)
            {
                const Signal tone = sine(0.25, 220.0, 0.6f);
                float own[sat::Count];
                for (int c = 0; c < sat::Count; ++c)
                {
                    auto fx = make();
                    fx->prepare(SR, 256);
                    setCell(*fx, satCell, (float) c);
                    fx->setMix(1.0f);
                    fx->reset();
                    own[c] = sharpestCorner(through(*fx, sine(1.0, 220.0, 0.6f)).l, at(0.5));
                }
                auto fx = make();
                fx->prepare(SR, 256);
                fx->setMix(1.0f);
                fx->reset();
                int before = (int) getCell(*fx, satCell);
                Signal tail = through(*fx, sine(1.0, 220.0, 0.6f)).l;
                int smooth = 0;
                for (int i = 0; i < sat::Count; ++i)
                {
                    const int c = (i * 7) % sat::Count;      // (far apart on the list each time)
                    setCell(*fx, satCell, (float) c);
                    Signal out = through(*fx, tone).l;
                    out.insert(out.begin(), tail.end() - 2, tail.end());       // (the join with the block before)
                    const float corner = sharpestCorner(out), allowed = 1.25f * std::max(own[before], own[c]) + 0.004f;
                    expect(corner < allowed, juce::String(name) + " " + sat::names()[before] + " to " + sat::names()[c] + ": sharpest corner "
                                             + juce::String(corner, 5) + ", the curves' own " + juce::String(own[before], 5) + " and " + juce::String(own[c], 5));
                    if (allowed < 0.03f) ++smooth;
                    tail = out;
                    before = c;
                }
                expect(smooth >= 4, juce::String(name) + ": " + juce::String(smooth) + " exchanges between curves smooth enough to show a step");
            };
            stepThrough([] { auto fx = std::make_unique<TapeDelay>(); fx->setParameter(TapeDelay::Time, 0.11f); return fx; }, 4 + TapeDelay::Sat, "TAPE");
            stepThrough([] { auto fx = std::make_unique<LCRDelay>(); fx->setExtra(LCRDelay::Drive, 1.0f); fx->setParameter(LCRDelay::Time, 0.11f); return fx; },
                        4 + LCRDelay::Sat, "LCR");
            stepThrough([] { auto fx = std::make_unique<DubDelay>(); fx->setParameter(DubDelay::Time, 0.11f); fx->setParameter(DubDelay::DriveAmt, 0.8f); return fx; },
                        4 + DubDelay::Sat, "DUB");
        }

        beginTest("SAT turned away and straight back, before the stage has shut: the stage opens again (the echo is as it was)");
        {
            TapeDelay fx;
            fx.prepare(SR, 256);
            fx.setParameter(TapeDelay::Time, 0.05f);
            fx.setParameter(TapeDelay::Mix, 1.0f);
            fx.reset();
            const Signal tone = sine(0.5, 220.0, 0.3f);
            const float before = rms(through(fx, tone).l, at(0.2), at(0.5));
            fx.setExtra(TapeDelay::Sat, (float) sat::Hard);
            through(fx, sine(64.0 / SR, 220.0, 0.3f), 64);          // (two control periods: shutting)
            fx.setExtra(TapeDelay::Sat, (float) sat::Fast);
            const float after = rms(through(fx, tone).l, at(0.2), at(0.5));
            expect(before > 0.1f && after > 0.9f * before && after < 1.1f * before, juce::String(before, 4) + " then " + juce::String(after, 4));
        }

        beginTest("DRIV: the tape stage comes in over the first eighth of the knob (just above 0 is all but the clean loop), "
                  "and brings a quiet repeat up by the root of how hard it is hit");
        {
            auto echo = [](float driv, float amp)
            {
                LCRDelay fx;
                fx.prepare(SR, 256);
                fx.setParameter(LCRDelay::Time, 0.2f);
                fx.setParameter(LCRDelay::Spread, 0.0f);
                fx.setParameter(LCRDelay::Feedback, 0.0f);
                fx.setParameter(LCRDelay::Mix, 1.0f);
                fx.setExtra(LCRDelay::Drive, driv);
                fx.reset();
                return through(fx, sine(0.5, 600.0, amp)).l;
            };
            const Signal clean = echo(0.0f, 0.8f), just = echo(0.005f, 0.8f), eighth = echo(0.125f, 0.8f);
            const float little = differenceRms(just, clean, at(0.25), at(0.45)), whole = differenceRms(eighth, clean, at(0.25), at(0.45));
            expect(whole > 0.01f, "the stage is heard at an eighth: " + juce::String(whole, 4));
            expect(little < 0.1f * whole, "just above 0: " + juce::String(little, 5) + " against " + juce::String(whole, 5) + " at an eighth");
            const float up = rms(echo(1.0f, 0.002f), at(0.25), at(0.45)) / rms(echo(0.0f, 0.002f), at(0.25), at(0.45));
            expectWithinAbsoluteError(up, std::sqrt(8.0f), 0.3f);
        }

        beginTest("A glide arrives, however long the time it goes to (a share of the last few samples is less than a float "
                  "of that size moves by: it stalled there, and every tap was read and glided for ever after)");
        {
            const float share = dly::glideShare(60.0f, SR);
            for (auto fromTo : { std::pair<float, float> { 2400.0f, 19200.0f }, { 96000.0f, 2.0f }, { 16000.0f, 16457.14f }, { 0.0f, 1.0f }, { 1.0f, 0.0f } })
            {
                dly::Glide g;
                g.snap(fromTo.first);
                g.set(fromTo.second);
                int controls = 0;
                float biggest = 0.0f;
                for (; controls < 4000; ++controls)
                {
                    const float before = g.now;
                    g.aim(share);
                    biggest = std::max(biggest, std::abs(g.now - before));      // (a snap)
                    if (! g.moving()) break;
                    for (int i = 0; i < dly::CONTROL; ++i) g.now += g.step;
                }
                const float size = 1.0f + std::max(std::abs(fromTo.first), std::abs(fromTo.second));
                expect(! g.moving() && g.now == fromTo.second && controls < 3000,       // (two seconds)
                       juce::String(fromTo.first) + " to " + juce::String(fromTo.second) + ": " + juce::String(controls) + " control periods, at " + juce::String(g.now, 6));
                expect(biggest <= 5.0e-6f * size, "the last of the way is next to nothing: " + juce::String(biggest, 8));
            }

            // The multi-tap turned from eight taps to one, then left alone: it is the one-tap delay, to the sample.
            MultiTapInsert turned, fresh;
            for (auto* fx : { &turned, &fresh })
            {
                fx->prepare(SR, 256);
                fx->setParameter(MultiTapInsert::Time, 0.4f);
                fx->setParameter(MultiTapInsert::Taps, fx == &turned ? 8.0f : 1.0f);
                fx->setParameter(MultiTapInsert::Feedback, 0.0f);
                fx->setParameter(MultiTapInsert::Mix, 1.0f);
                fx->reset();
            }
            through(turned, noise(0.5, 0.3f));
            turned.setParameter(MultiTapInsert::Taps, 1.0f);
            through(turned, silence(4.5));
            through(fresh, silence(1.0));
            const Signal a = through(turned, impulse(0.6)).l, b = through(fresh, impulse(0.6)).l;
            expect(peak(b) > 0.3f && peakAt(a, 0, a.size()) == peakAt(b, 0, b.size()) && differenceRms(a, b, 0, a.size()) < 1.0e-6f,
                   "difference " + juce::String(differenceRms(a, b, 0, a.size()), 8) + ", peaks at " + juce::String((int) peakAt(a, 0, a.size()))
                   + " and " + juce::String((int) peakAt(b, 0, b.size())));
        }

        beginTest("Multi-tap: taps taken away under sound fade out, they are not cut");
        {
            MultiTapInsert fx;
            fx.prepare(SR, 256);
            fx.setParameter(MultiTapInsert::Time, 0.4f);
            fx.setParameter(MultiTapInsert::Taps, 8.0f);
            fx.setParameter(MultiTapInsert::Feedback, 0.0f);
            fx.setParameter(MultiTapInsert::Mix, 1.0f);
            fx.reset();
            // (A tone so low that a glide's swoop stays smooth: 0.5 at 55 Hz bends by 0.00003 a sample.)
            const Signal tone = sine(1.0, 55.0, 0.5f);
            Stereo out = through(fx, tone);
            fx.setParameter(MultiTapInsert::Taps, 7.0f);     // (the taps move by a seventh at most, one goes)
            const Stereo after = through(fx, joined(tone, tone));
            out.l.insert(out.l.end(), after.l.begin(), after.l.end());
            out.r.insert(out.r.end(), after.r.begin(), after.r.end());
            const float corner = sharpestCorner(out, at(0.9));
            expect(corner < 0.004f, "sharpest corner " + juce::String(corner, 5));
        }

        beginTest("Reverb loops are held: a second of sound far over full scale does not pile up in a long tail");
        for (int which = 0; which < 2; ++which)
        {
            std::unique_ptr<CellEffect> fx;
            if (which == 0) fx.reset(new SpringReverb()); else fx.reset(new ShimmerReverb());
            fx->prepare(SR, 256);
            setCell(*fx, 0, specOf(*fx, 0).maxVal);                         // DCAY
            setCell(*fx, cellNamed(*fx, "DAMP"), 0.0f);
            if (which == 0) setCell(*fx, cellNamed(*fx, "TENS"), 1.0f);     // (short springs: many round trips)
            else { setCell(*fx, cellNamed(*fx, "SIZE"), 0.25f); setCell(*fx, cellNamed(*fx, "SHIM"), 0.0f); }
            fx->setMix(1.0f);
            fx->reset();
            const auto out = through(*fx, Signal(at(1.0), 60.0f));
            // (A loop holds 16 at most; the spring is heard at 1.4 times that, the tank's three taps at 2.5 x 1.3: 52.)
            expect(finite(out.l) && peak(out.l) < (which == 0 ? 30.0f : 60.0f), juce::String(which == 0 ? "SPRG " : "SHIM ") + juce::String(peak(out.l), 2));
        }

        beginTest("Shimmer: the diffusers smear a click before the tank, and the tank is heard from inside its lines "
                  "(long before a loop has come round); spring: each side hears a fifth of the other spring");
        {
            ShimmerReverb fx;
            fx.prepare(SR, 256);
            fx.setParameter(ShimmerReverb::Shimmer, 0.0f);
            fx.setParameter(ShimmerReverb::Damping, 0.0f);
            fx.setExtra(ShimmerReverb::Modulation, 0.0f);
            fx.setMix(1.0f);
            fx.reset();
            const auto out = through(fx, impulse(0.4));
            // The loops are 161 and 143 ms; the first places heard inside them are 51 and 62 ms in.
            const float early = rms(out.l, at(0.045), at(0.125));
            expect(early > 1.0e-4f, "heard from inside the lines: " + juce::String(early, 6));
            // Smeared: within those 80 ms the click has become dozens (three diffusers), not the two places alone.
            int carrying = 0;
            const float top = peak(out.l, at(0.045), at(0.125));
            for (size_t i = at(0.045); i < at(0.125); ++i) if (std::abs(out.l[i]) > 0.05f * top) ++carrying;
            // (Since the one reverb network the input allpasses are whole samples long: each echo is
            // one sample, where a read between two samples made it two. 20 measured, against 2 unsmeared.)
            expect(carrying > 12, juce::String(carrying) + " samples carry the click");

            SpringReverb spring;
            spring.prepare(SR, 256);
            spring.setMix(1.0f);
            spring.reset();
            const auto sides = through(spring, noise(0.5, 0.3f), silence(0.5));
            const float ratio = rms(sides.r) / rms(sides.l);
            expectWithinAbsoluteError(ratio, 0.25f, 0.02f);     // (0.2 of it against 0.8)
        }

        beginTest("A modulated mix arrives over a control period (no step in the dry sound); the dub echo's TONE glides");
        {
            TapeDelay fx;
            fx.prepare(SR, 256);
            fx.setParameter(TapeDelay::Time, 1.5f);          // (nothing comes back while this is measured)
            fx.setParameter(TapeDelay::Mix, 0.2f);
            fx.reset();
            const Signal level(at(0.05), 1.0f);
            Signal out = through(fx, level).l;
            fx.modulateCell(TapeDelay::Mix, 0.5f);
            const Signal after = through(fx, level, 1).l;       // (in blocks of one sample: the glide goes on across them)
            out.insert(out.end(), after.begin(), after.end());
            expectWithinAbsoluteError(out.back(), 0.3f, 0.001f);
            expect(biggestStep(out) < 0.5f / 20.0f, "biggest step " + juce::String(biggestStep(out), 4));

            // TONE from the top to the bottom under a 3 kHz tone: what comes back 100 ms later is still most of
            // what it was 3 ms after the turn (set at once, the new band would have let a tenth through by then).
            DubDelay dub;
            dub.prepare(SR, 32);
            dub.setParameter(DubDelay::Time, 0.1f);
            dub.setParameter(DubDelay::Feedback, 0.0f);
            dub.setParameter(DubDelay::Tone, 1.0f);
            dub.setParameter(DubDelay::DriveAmt, 0.0f);
            dub.reset();
            const Signal tone = sine(0.5, 3000.0, 0.1f);
            Signal heard = through(dub, tone, 32).l;
            const float before = rms(heard, at(0.4), at(0.5));
            dub.setParameter(DubDelay::Tone, 0.0f);
            heard = through(dub, tone, 32).l;
            const float soon = rms(heard, at(0.1005), at(0.103)), late = rms(heard, at(0.4), at(0.5));
            expect(soon > 0.5f * before, "3 ms after the turn comes back: " + juce::String(soon / before, 3));
            expect(late < 0.2f * before, "and it does arrive: " + juce::String(late / before, 3));
        }
    }

    // ---- in a chain, in the engine ---------------------------------------------------------------
    void testInChainsAndEngine()
    {
        beginTest("In an EffectChain, in series, in parallel and mixed, in blocks of any size: finite, bounded, the wet sound there, "
                  "and silent again when switched off");
        for (auto routing : { EffectChain::Routing::Series, EffectChain::Routing::Parallel, EffectChain::Routing::Mixed })
        {
            EffectChain chain;
            for (const auto& k : kinds())
            {
                auto fx = k.make();
                fx->setMix(0.5f);
                chain.addEffect(std::move(fx));
            }
            chain.setRouting(routing);
            chain.prepare(SR, 512);
            const Signal in = joined(noise(0.5, 0.3f), silence(1.0));
            Stereo out { in, in };
            size_t done = 0;
            for (int b = 0; done < in.size(); ++b)
            {
                const int n = (int) std::min<size_t>((size_t) (1 + (b * 131) % 512), in.size() - done);
                if (b == 40) chain.getEffect(1)->setEnabled(false);     // (switched off and on under sound)
                if (b == 80) chain.getEffect(1)->setEnabled(true);
                chain.processStereo(out.l.data() + done, out.r.data() + done, n);
                done += (size_t) n;
            }
            const juce::String name = "routing " + juce::String((int) routing);
            expect(finite(out.l) && finite(out.r) && peak(out.l) < 8.0f, name + ": peak " + juce::String(peak(out.l), 3));
            expect(rms(out.l, at(0.8), at(1.4)) > 1.0e-4f, name + ": the tails are heard after the sound: " + juce::String(rms(out.l, at(0.8), at(1.4)), 6));
        }

        beginTest("In the engine: each as a track's insert (its FX and FX2 cells set by the track's parameter ids, read back) "
                  "and as a send's effect: finite, heard after the note, no block dropped");
        const EffectType types[] = { EffectType::TapeDelayEffect, EffectType::LCRDelayEffect, EffectType::MultiTapEffect,
                                     EffectType::ReverseDelayEffect, EffectType::SpringReverbEffect, EffectType::ShimmerReverbEffect };
        for (EffectType type : types)
            for (int asSend = 0; asSend < 2; ++asSend)
            {
                const juce::String name = juce::String(AudioEngine::effectTypeName(type)) + (asSend ? " as a send" : " as an insert");
                SequencerCore seq;
                AudioEngine audio;
                seq.prepareToPlay(SR, 128);
                audio.setSequencer(&seq);
                audio.addEngine(0, AudioEngine::createEngine(EngineType::FM));
                audio.prepare(SR, 128);
                // The dry mix alone, to compare with.
                auto render = [&](bool withEffect)
                {
                    audio.getEngine(0)->noteOn(60, 1.0f, -1);
                    Signal l(128), r(128);
                    Stereo out;
                    for (int b = 0; b < 750; ++b)       // two seconds; the note is let go after 0.2
                    {
                        if (b == 75) audio.getEngine(0)->noteOff(60, -1);
                        audio.renderBlock(l.data(), r.data(), 128);
                        out.l.insert(out.l.end(), l.begin(), l.end());
                        out.r.insert(out.r.end(), r.begin(), r.end());
                    }
                    juce::ignoreUnused(withEffect);
                    return out;
                };
                audio.setTrackParameter(0, AudioEngine::PARAM_SEND1, 0.0f);
                audio.setTrackParameter(0, AudioEngine::PARAM_SEND2, 0.0f);
                const Stereo dry = render(false);
                if (asSend)
                {
                    audio.setInsertEffect(AudioEngine::sendTrack(0), 0, type);
                    audio.setTrackParameter(0, AudioEngine::PARAM_SEND1, 1.0f);
                }
                else
                {
                    audio.addInsertEffect(0, type);
                    auto* fx = audio.getInsertChain(0)->getEffect(0);
                    expect(dynamic_cast<CellEffect*>(fx) != nullptr, name + " is an effect of cells");
                    for (int c = 0; c < CellEffect::CELLS; ++c)
                    {
                        const ParamSpec spec = specOf(*fx, c);
                        const float v = spec.isDiscrete() ? spec.maxVal : 0.5f * (spec.minVal + spec.maxVal);
                        const int id = c < 4 ? AudioEngine::EFFECT_PARAM_BASE + c : AudioEngine::INSERT_EXTRA_BASE + (c - 4);
                        audio.setTrackParameter(0, id, v);
                        expectWithinAbsoluteError(audio.getTrackParameter(0, id), v, 1.0e-4f);
                        expectWithinAbsoluteError(getCell(*fx, c), v, 1.0e-4f);
                        audio.setTrackParameter(0, id, spec.defaultVal);
                    }
                }
                const Stereo wet = render(true);
                expect(finite(wet.l) && finite(wet.r) && audio.getNonFiniteBlocks() == 0, name + ": finite");
                expect(peak(wet.l) < 4.0f, name + ": peak " + juce::String(peak(wet.l), 3));
                const float tailDry = rms(dry.l, at(0.9), at(1.9)), tailWet = rms(wet.l, at(0.9), at(1.9));
                expect(tailWet > 1.0e-4f && tailWet > 4.0f * tailDry, name + ": heard after the note, " + juce::String(tailWet, 6)
                                                                       + " against " + juce::String(tailDry, 6) + " without");
            }
    }

    // ---- cost (NEPTUNE_E2_TIME=1) ---------------------------------------------------------------
    void timeThem()
    {
        beginTest("Cost of each effect: microseconds for a block of 512 stereo samples at 48 kHz, in this build");
        for (const auto& k : kinds())
        {
            auto fx = k.make();
            fx->prepare(SR, 512);
            Signal l = noise(512.0 / SR, 0.3f), r = l;
            double best = 1.0e9;
            for (int rep = 0; rep < 5; ++rep)
            {
                const auto t0 = std::chrono::steady_clock::now();
                for (int b = 0; b < 400; ++b)
                {
                    Signal a = l, c = r;
                    fx->processBlock(a.data(), c.data(), 512);
                }
                best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 400.0);
            }
            logMessage(juce::String(k.name) + ": " + juce::String(best, 2) + " us");
        }
        expect(true);
    }
};

static EffectCoresTimeTests effectCoresTimeTests;
