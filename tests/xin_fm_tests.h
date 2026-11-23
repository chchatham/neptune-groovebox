#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>
#include "fast_measure.h"
#include "track_input_tests.h"
#include "../src/engine/FMEngine.h"
#include "../src/engine/AudioEngine.h"

// -----------------------------------------------------------------------------
// XIN on the FM engine: the track input as one more modulator.
//
//   XIN   OFF, OP1 .. OP6, ALL   which operator's phase the input modulates
//   XDPT  0..1                   how far: at 1 a full-level input is a modulator
//                                operator at full level (an index of 1 radian)
//
// The engine is given its input directly (setAudioInput), as AudioEngine gives
// it; one test goes through AudioEngine with a real source track.
// Sidebands are measured with a Goertzel on steady, bin-exact tones (nothing in
// the path moves); levels and differences by RMS.
// -----------------------------------------------------------------------------
class XinFmTests : public juce::UnitTest
{
public:
    XinFmTests() : UnitTest("XIN FM") {}

    using F = FMEngine;
    static constexpr double PI2 = 6.283185307179586;

    // What the engine is given as its input: a function of the sample number for each side
    // (none = nothing connected), how many frames of each block are valid (-1 = all of it) and
    // what lies in the buffer past them.
    struct Input
    {
        std::function<float (long)> left, right;
        int frames = -1;
        float beyond = 0.0f;
        bool connected() const { return left != nullptr; }
    };

    static Input both(std::function<float (long)> f) { return { f, f }; }
    static std::function<float (long)> sine(double hz, double sr, float amp = 1.0f)
    {
        return [=](long n) { return amp * (float) std::sin(PI2 * hz * (double) n / sr); };
    }
    static std::function<float (long)> noise()
    {
        return [](long n)
        {
            juce::uint32 s = (juce::uint32) n * 2654435761u + 12345u;
            s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
            return (float) (s >> 8) / 8388608.0f - 1.0f;
        };
    }

    // One steady operator and nothing else: additive algorithm, the given operator at full level.
    static void oneOperator(F& e, int op, double sr = 48000.0, int block = 64)
    {
        e.prepareToPlay(sr, block);
        e.setParameter(F::AlgorithmParam, 5.0f);                // ADD
        for (int o = 0; o < 6; ++o)
        {
            e.setParameter(o * 10 + 1, o == op ? 1.0f : 0.0f);
            e.setParameter(o * 10 + 4, 0.001f);                 // attack
            e.setParameter(o * 10 + 6, 1.0f);                   // sustain
        }
    }

    // Left channel, then right. `between` runs before the block that starts at the given sample.
    static std::vector<float> render(F& e, int total, const Input& in, int block = 64,
                                     const std::function<void (int)>& between = nullptr)
    {
        std::vector<float> l, r;
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        const int frames = in.frames < 0 ? block : in.frames;
        std::vector<float> inL((size_t) std::max(block, frames) + 8, in.beyond), inR = inL;
        long at = 0;
        while ((int) l.size() < total)
        {
            if (between)
                between((int) at);
            if (in.connected())
            {
                std::fill(inL.begin(), inL.end(), in.beyond);
                std::fill(inR.begin(), inR.end(), in.beyond);
                for (int i = 0; i < frames; ++i)
                {
                    inL[(size_t) i] = in.left(at + i);
                    inR[(size_t) i] = in.right ? in.right(at + i) : inL[(size_t) i];
                }
                e.setAudioInput({ inL.data(), inR.data(), frames, 0.0f });
            }
            else
                e.setAudioInput({});
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
            {
                l.push_back(buf.getSample(0, i));
                r.push_back(buf.getSample(1, i));
            }
            at += block;
        }
        e.setAudioInput({});
        l.insert(l.end(), r.begin(), r.end());
        return l;
    }

    static bool same(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }
    static double rms(const std::vector<float>& a, size_t from, size_t to)
    {
        double sq = 0.0;
        for (size_t i = from; i < to && i < a.size(); ++i)
            sq += (double) a[i] * a[i];
        return std::sqrt(sq / (double) std::max<size_t>(1, to - from));
    }
    static double rmsOfDifference(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
    {
        double sq = 0.0;
        for (size_t i = from; i < to && i < a.size() && i < b.size(); ++i)
            sq += ((double) a[i] - b[i]) * ((double) a[i] - b[i]);
        return std::sqrt(sq / (double) std::max<size_t>(1, to - from));
    }
    static bool finiteAndWithin(const std::vector<float>& a, float bound, float& peak)
    {
        peak = 0.0f;
        for (float s : a)
        {
            if (! std::isfinite(s))
                return false;
            peak = std::max(peak, std::abs(s));
        }
        return peak <= bound;
    }
    // Amplitude of `hz` relative to that of `ref` in one second from 0.1 s (both bin-exact).
    static double relative(const std::vector<float>& x, double hz, double ref, size_t from = 4800, size_t n = 48000)
    {
        return std::sqrt(fastmeasure::goertzel(x, from, n, hz, 48000.0)
                         / std::max(1.0e-30, fastmeasure::goertzel(x, from, n, ref, 48000.0)));
    }

    void runTest() override
    {
        beginTest("The page: the modulation router's XIN page (after the filter's); its DST cells offer PM1 .. PM6 and PMAL; ids 64 and 65 are on no page");
        {
            F e;
            using R = mod::Router;
            const auto pages = e.getParamPages();
            int at = -1, filter = -1, count = 0;
            for (int i = 0; i < (int) pages.size(); ++i)
            {
                if (pages[(size_t) i].name == "XIN") { at = i; ++count; }
                if (pages[(size_t) i].find(F::FilterBase + VoiceFilterSection::Attack) != nullptr) filter = i;
                for (auto& cell : pages[(size_t) i].params)
                    expect(! cell.isValid() || (cell.id != F::LegacyXinParam && cell.id != F::LegacyXinDepthParam), "no cell at 64 / 65");
            }
            expectEquals(count, 1, "one XIN page (the engine's own is gone)");
            expect(at >= 0 && filter >= 0 && at == filter + 1, "XIN follows the filter's pages");
            if (at >= 0)
            {
                const auto& dst = pages[(size_t) at].params[0];
                expectEquals(dst.id, R::xinId(R::XinDest1));
                expectEquals(dst.numChoices, F::lfoDests().visible() + mod::TrackDest::COUNT);     // (and the track's effect cells: E4)
                static const char* const names[] = { "PM1", "PM2", "PM3", "PM4", "PM5", "PM6", "PMAL" };
                for (int k = 0; k < 7; ++k)
                {
                    expectEquals(juce::String(dst.choices[FMVoice::LfoPhase + k]), juce::String(names[k]));
                    const auto& d = F::lfoDests()[FMVoice::LfoPhase + k];
                    expect(d.perSample && d.id < 0 && d.base == 0.0f && d.span == 1.0f, juce::String(names[k]) + ": radians, per sample");
                }
                expectEquals((int) FMVoice::LfoPhaseAll, FMVoice::LfoPhase + 6);
            }
            expectEquals(e.getParameter(R::xinId(R::XinDest1)), 0.0f);
            expectEquals(e.getParameter(R::xinId(R::XinAmount1)), 0.0f);
            expect(F::lfoDests().indexOfLabel("XDPT") < 0 && F::lfoDests().indexOfId(F::LegacyXinDepthParam) < 0);
        }

        beginTest("A sound stored with the old XIN and XDPT cells (ids 64, 65) sets the XIN page's first route: PMn / PMAL and the depth, whatever it is given");
        {
            F e;
            using R = mod::Router;
            e.prepareToPlay(48000.0, 64);
            const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
            struct Case { float given, kept; };
            for (auto c : { Case { 3.0f, 3.0f }, Case { 7.0f, 7.0f }, Case { 99.0f, 7.0f }, Case { 1.0e30f, 7.0f }, Case { -4.0f, 0.0f },
                            Case { inf, 0.0f }, Case { nan, 0.0f }, Case { 2.4f, 2.0f } })
            {
                e.setParameter(F::LegacyXinParam, c.given);
                expectEquals(e.getParameter(R::xinId(R::XinDest1)), c.kept == 0.0f ? 0.0f : (float) (FMVoice::LfoPhase + (int) c.kept - 1),
                             "XIN given " + juce::String(c.given));
                // (The old XDPT was 50 until set.)
                expectEquals(e.getParameter(R::xinId(R::XinAmount1)), 0.5f);
                expectEquals(e.getParameter(R::xinId(R::XinFilter1)), 0.0f);
            }
            e.setParameter(F::LegacyXinParam, 1.0f);
            for (auto c : { Case { 0.25f, 0.25f }, Case { 4.0f, 1.0f }, Case { -1.0e30f, 0.0f }, Case { inf, 0.0f }, Case { nan, 0.0f } })
            {
                e.setParameter(F::LegacyXinDepthParam, c.given);
                expectEquals(e.getParameter(R::xinId(R::XinAmount1)), c.kept, "XDPT given " + juce::String(c.given));
            }
            // Nothing is kept under the old ids, and the second route is not touched.
            expectEquals(e.getParameter(F::LegacyXinParam), 0.0f);
            expectEquals(e.getParameter(F::LegacyXinDepthParam), 0.0f);
            expectEquals(e.getParameter(R::xinId(R::XinDest2)), 0.0f);
        }

        beginTest("The input on OP1 is what operator 2 does to operator 1: same ratio, XDPT = the modulator's level");
        {
            // [2 > 1] with operator 2 at ratio 2.5 and level L, against operator 2 silent and a
            // sine of 2.5 x the note on the input with XDPT = L. Once operator 2's attack is over
            // the two are the same sound, sample by sample.
            for (float level : { 0.3f, 0.6f, 1.0f })
            {
                auto make = [](F& e, float op2Level)
                {
                    e.prepareToPlay(48000.0, 64);
                    e.setParameter(F::AlgorithmParam, 6.0f);        // [2 > 1], the others silent carriers
                    e.setParameter(F::Op2Ratio, 2.5f);
                    e.setParameter(F::Op2Level, op2Level);
                    for (int o = 0; o < 2; ++o)
                    {
                        e.setParameter(o * 10 + 4, 0.001f);
                        e.setParameter(o * 10 + 6, 1.0f);
                    }
                };
                F byOperator, byInput;
                make(byOperator, level);
                make(byInput, 0.0f);
                byInput.setParameter(F::LegacyXinParam, 1.0f);
                byInput.setParameter(F::LegacyXinDepthParam, level);
                byOperator.noteOn(69, 1.0f, -1);
                byInput.noteOn(69, 1.0f, -1);
                const auto a = render(byOperator, 24000, {});
                const auto b = render(byInput, 24000, both(sine(1100.0, 48000.0)));
                const double level0 = rms(a, 4800, 24000), diff = rmsOfDifference(a, b, 4800, 24000);
                expect(level0 > 0.2, "the reference sounds: " + juce::String(level0));
                expect(diff < 0.002 * level0, "level " + juce::String(level) + ": difference " + juce::String(diff / level0, 6));

                // ... and it is the modulation that is compared: without the input the sound is another.
                F plain;
                make(plain, 0.0f);
                plain.noteOn(69, 1.0f, -1);
                const auto c = render(plain, 24000, {});
                expect(rmsOfDifference(a, c, 4800, 24000) > 0.1 * level0, "operator 2 is heard in the reference");
            }
        }

        beginTest("Sidebands of a sine input: at the note +- the input's frequency, as large as the index says; XDPT scales them");
        {
            // One carrier at 440 Hz, the input a full-level sine at 1100 Hz (ratio 2.5). Phase
            // modulation of index I puts J1(I) / J0(I) of the carrier at 440 +- 1100 (1540 and,
            // folded, 660) and J2(I) / J0(I) at 440 + 2200.
            struct Expect { float depth; double first, second; };
            for (auto x : { Expect { 1.0f, 0.44005 / 0.76520, 0.11490 / 0.76520 }, Expect { 0.5f, 0.24227 / 0.93847, 0.03060 / 0.93847 },
                            Expect { 0.25f, 0.12403 / 0.98444, 0.00777 / 0.98444 } })
            {
                F e;
                oneOperator(e, 0);
                e.setParameter(F::LegacyXinParam, 1.0f);
                e.setParameter(F::LegacyXinDepthParam, x.depth);
                e.noteOn(69, 1.0f, -1);
                const auto out = render(e, 52800, both(sine(1100.0, 48000.0)));
                const juce::String at = "XDPT " + juce::String(x.depth) + ": ";
                expectWithinAbsoluteError(relative(out, 1540.0, 440.0), x.first, 0.03 * x.first, at + "upper sideband");
                expectWithinAbsoluteError(relative(out, 660.0, 440.0), x.first, 0.03 * x.first, at + "lower sideband");
                expectWithinAbsoluteError(relative(out, 2640.0, 440.0), x.second, 0.05 * x.second + 0.001, at + "second sideband");
                expect(relative(out, 1100.0, 440.0) < 0.002, at + "the input itself is not in the sound");
            }
            // No input, or XDPT 0: a sine.
            F e;
            oneOperator(e, 0);
            e.setParameter(F::LegacyXinParam, 1.0f);
            e.setParameter(F::LegacyXinDepthParam, 0.0f);
            e.noteOn(69, 1.0f, -1);
            const auto out = render(e, 52800, both(sine(1100.0, 48000.0)));
            expect(relative(out, 1540.0, 440.0) < 0.001 && relative(out, 660.0, 440.0) < 0.001);
        }

        beginTest("OP1 .. OP6 reach that operator and no other; ALL reaches every one, modulators too");
        {
            const auto in = both(sine(1100.0, 48000.0));
            for (int op = 0; op < 6; ++op)
                for (int target = 0; target <= 7; ++target)
                {
                    F e;
                    oneOperator(e, op);
                    e.setParameter(F::LegacyXinParam, (float) target);
                    e.setParameter(F::LegacyXinDepthParam, 1.0f);
                    e.noteOn(69, 1.0f, -1);
                    const auto out = render(e, 14400, in);
                    const double side = relative(out, 1540.0, 440.0, 4800, 9600);
                    const bool reached = target == op + 1 || target == 7;
                    const juce::String where = "operator " + juce::String(op + 1) + ", XIN " + juce::String(target) + ": " + juce::String(side, 4);
                    if (reached) expectWithinAbsoluteError(side, 0.5751, 0.02, where);
                    else         expect(side < 0.001, where);
                }

            // In [2 > 1], ALL also modulates the modulator: another sound than OP1 alone, and OP2
            // alone is heard through operator 1.
            std::vector<float> outs[4];
            const int targets[4] = { 0, 1, 2, 7 };
            for (int k = 0; k < 4; ++k)
            {
                F e;
                e.prepareToPlay(48000.0, 64);
                e.setParameter(F::AlgorithmParam, 6.0f);
                e.setParameter(F::Op2Level, 1.0f);
                e.setParameter(F::LegacyXinParam, (float) targets[k]);
                e.setParameter(F::LegacyXinDepthParam, 1.0f);
                e.noteOn(57, 1.0f, -1);
                outs[k] = render(e, 14400, in);
            }
            const double level = rms(outs[0], 4800, 14400);
            expect(rmsOfDifference(outs[0], outs[2], 4800, 14400) > 0.1 * level, "OP2 (a modulator) is heard");
            expect(rmsOfDifference(outs[1], outs[3], 4800, 14400) > 0.1 * level, "ALL is more than OP1");
            expect(rmsOfDifference(outs[2], outs[3], 4800, 14400) > 0.1 * level, "ALL is more than OP2");
        }

        beginTest("Every voice is modulated: both notes of a chord carry the sidebands");
        {
            F e;
            oneOperator(e, 0);
            e.setParameter(F::LegacyXinParam, 7.0f);
            e.setParameter(F::LegacyXinDepthParam, 1.0f);
            e.noteOn(69, 1.0f, -1);         // 440 Hz
            e.noteOn(81, 1.0f, -1);         // 880 Hz
            const auto out = render(e, 52800, both(sine(1100.0, 48000.0)));
            expectWithinAbsoluteError(relative(out, 1540.0, 440.0), 0.5751, 0.02);
            expectWithinAbsoluteError(relative(out, 1980.0, 880.0), 0.5751, 0.02);
        }

        beginTest("XIN OFF, nothing connected, XDPT 0 or two opposite sides: bit for bit the sound without an input, in every algorithm");
        {
            for (int algo = 0; algo < 8; ++algo)
            {
                auto run = [algo](int target, float depth, const Input& in)
                {
                    F e;
                    e.prepareToPlay(48000.0, 64);
                    e.setParameter(F::AlgorithmParam, (float) algo);
                    for (int o = 0; o < 6; ++o)
                    {
                        e.setParameter(o * 10 + 1, 0.4f + 0.1f * (float) o);
                        e.setParameter(o * 10 + 0, 1.0f + 0.5f * (float) o);
                    }
                    e.setParameter(F::Op6Feedback, 0.5f);
                    e.setParameter(F::LegacyXinParam, (float) target);
                    e.setParameter(F::LegacyXinDepthParam, depth);
                    e.noteOn(48, 0.8f, -1);
                    e.noteOn(55, 0.6f, -1);
                    return render(e, 9600, in, 64, [&e](int at) { if (at == 4800) e.noteOff(48, -1); });
                };
                const juce::String name = "algorithm " + juce::String(algo + 1) + ": ";
                const auto none = run(0, 0.5f, {});
                float peak = 0.0f;
                expect(finiteAndWithin(none, 4.0f, peak) && peak > 0.05f, name + "sounds");
                expect(same(none, run(0, 1.0f, both(noise()))), name + "XIN OFF with an input");
                expect(same(none, run(7, 1.0f, {})), name + "XIN ALL with nothing connected");
                Input empty = both(noise());
                empty.frames = 0;
                expect(same(none, run(7, 1.0f, empty)), name + "XIN ALL with an input of no frames");
                expect(same(none, run(7, 0.0f, both(noise()))), name + "XDPT 0");
                const auto n = noise();
                expect(same(none, run(3, 1.0f, { n, [n](long i) { return -n(i); } })), name + "two opposite sides sum to nothing");
                expect(rmsOfDifference(none, run(7, 0.5f, both(noise())), 0, 9600) > 0.02, name + "and with the input it is another sound");
            }
        }

        beginTest("XIN turned OFF under a note: the sound without an input from then on");
        {
            // Phase modulation leaves nothing behind in an operator without feedback: the phase
            // itself was never moved. (The voice's filter rings for a few samples.)
            auto run = [](bool modulated)
            {
                F e;
                oneOperator(e, 2);
                e.setParameter(F::LegacyXinDepthParam, 1.0f);
                if (modulated) e.setParameter(F::LegacyXinParam, 7.0f);
                e.noteOn(60, 1.0f, -1);
                return render(e, 9600, both(noise()), 64, [&e](int at) { if (at == 4800) e.setParameter(F::LegacyXinParam, 0.0f); });
            };
            const auto with = run(true), without = run(false);
            expect(rmsOfDifference(with, without, 0, 4800) > 0.05, "modulated at first");
            expect(rmsOfDifference(with, without, 4900, 9600) < 1.0e-5, "and as without it afterwards: "
                       + juce::String(rmsOfDifference(with, without, 4900, 9600), 8));
        }

        beginTest("The two sides are summed to one: one side alone is half the depth of both");
        {
            auto run = [](float depth, const Input& in)
            {
                F e;
                oneOperator(e, 0);
                e.setParameter(F::LegacyXinParam, 1.0f);
                e.setParameter(F::LegacyXinDepthParam, depth);
                e.noteOn(69, 1.0f, -1);
                return render(e, 9600, in);
            };
            const auto s = sine(1100.0, 48000.0);
            const auto zero = [](long) { return 0.0f; };
            const auto bothHalf = run(0.5f, both(s)), leftOnly = run(1.0f, { s, zero }), rightOnly = run(1.0f, { zero, s });
            const double level = rms(bothHalf, 0, 9600);
            expect(rmsOfDifference(bothHalf, leftOnly, 0, 9600) < 1.0e-5 * level);
            expect(rmsOfDifference(bothHalf, rightOnly, 0, 9600) < 1.0e-5 * level);
            expect(rmsOfDifference(bothHalf, run(1.0f, both(s)), 0, 9600) > 0.1 * level, "(and the depth is heard)");
        }

        beginTest("An input shorter than the block is not read beyond its frames, whatever the buffer holds there: its last sample is let go of over 4 ms, and the next block let in as fast (U4-1)");
        {
            auto run = [](const Input& in)
            {
                F e;
                oneOperator(e, 0);
                e.setParameter(F::LegacyXinParam, 1.0f);
                e.setParameter(F::LegacyXinDepthParam, 1.0f);
                e.noteOn(69, 1.0f, -1);
                return render(e, 9600, in);
            };
            const auto n = noise();
            Input shortOne = both(n);                       // 20 frames of noise, then 1.0 in the buffer
            shortOne.frames = 20;
            shortOne.beyond = 1.0f;
            const Input zeroed = both([n](long i) { return (i % 64) < 20 ? n(i) : 0.0f; });    // the same, as a whole block
            const Input whole = both([n](long i) { return (i % 64) < 20 ? n(i) : 1.0f; });     // what the buffer really held
            // (What the router makes of it, U4-1: the last sample of the 20 held and let go of at the
            // whole range in 4 ms, the next block's let in from nothing as fast.)
            std::vector<float> fade(9600 + 64);
            {
                const float step = (float) (1.0 / (0.004 * 48000.0));
                float gain = 1.0f, tail = 0.0f, last = 0.0f;
                for (long t = 0; t < (long) fade.size(); ++t)
                {
                    const bool have = (t % 64) < 20;
                    if (! have && gain != 0.0f) { tail = std::clamp(tail + last * gain, -1.0f, 1.0f); gain = 0.0f; }
                    if (tail != 0.0f) tail = dsp::towards(tail, 0.0f, step);
                    float x = 0.0f;
                    if (have)
                    {
                        x = std::clamp(n(t), -1.0f, 1.0f);
                        last = x;
                        if (gain != 1.0f) { gain = dsp::towards(gain, 1.0f, step); x *= gain; }
                    }
                    if (tail != 0.0f) x = std::clamp(x + tail, -1.0f, 1.0f);
                    fade[(size_t) t] = x;
                }
            }
            const Input faded = both([fade](long i) { return fade[(size_t) i]; });
            const auto a = run(shortOne), b = run(faded), c = run(whole);
            expect(same(a, b), "the frames past the input's length: what the router makes of an input that ends");
            expect(rmsOfDifference(a, run(zeroed), 0, 9600) > 1.0e-4, "(not silence from the sample it ends: that steps)");
            expect(rmsOfDifference(a, c, 0, 9600) > 0.05, "(the buffer's own content there would have been heard)");

            // An input longer than the block: the block's part of it, and nothing read past it.
            Input longOne = both(n);
            longOne.frames = 100;
            F e1, e2;
            for (auto* e : { &e1, &e2 })
            {
                oneOperator(*e, 0);
                e->setParameter(F::LegacyXinParam, 1.0f);
                e->noteOn(69, 1.0f, -1);
            }
            juce::AudioBuffer<float> out1(2, 64), out2(2, 64);
            juce::MidiBuffer midi;
            std::vector<float> in((size_t) 100);
            for (int i = 0; i < 100; ++i) in[(size_t) i] = n(i);
            out1.clear(); out2.clear();
            e1.setAudioInput({ in.data(), in.data(), 100, 0.0f });
            e1.processBlock(out1, midi);
            e2.setAudioInput({ in.data(), in.data(), 64, 0.0f });
            e2.processBlock(out2, midi);
            expect(std::memcmp(out1.getReadPointer(0), out2.getReadPointer(0), 64 * sizeof(float)) == 0);
        }

        beginTest("XDPT set under a sounding note is moved to over a few milliseconds, never jumped to");
        {
            // A steady input of 1 is a steady shift of the phase: a jump of XDPT would be a jump of the wave.
            auto run = [](bool change)
            {
                F e;
                oneOperator(e, 0);
                e.setParameter(F::LegacyXinParam, 1.0f);
                e.setParameter(F::LegacyXinDepthParam, 0.0f);
                e.noteOn(69, 1.0f, -1);
                return render(e, 9600, both([](long) { return 1.0f; }), 64,
                              [&e, change](int at) { if (at == 4800 && change) e.setParameter(F::LegacyXinDepthParam, 1.0f); });
            };
            const auto moved = run(true), still = run(false);
            expect(std::memcmp(moved.data(), still.data(), 4800 * sizeof(float)) == 0);
            expect(std::abs(moved[4800] - still[4800]) < 0.01f, "the first sample after the change: " + juce::String(std::abs(moved[4800] - still[4800]), 5));
            // No step on the way: from one sample to the next the wave moves as far as the plain sine does.
            float step = 0.0f, plain = 0.0f;
            for (size_t i = 4800; i < 5400; ++i)
            {
                step = std::max(step, std::abs(moved[i] - moved[i - 1]));
                plain = std::max(plain, std::abs(still[i] - still[i - 1]));
            }
            expect(step < 1.25f * plain, "largest step " + juce::String(step, 5) + " against " + juce::String(plain, 5));
            expect(rmsOfDifference(moved, still, 5300, 9600) > 0.2, "and it arrives");
            // Arrived: the sound of a note that had XDPT 1 from the start.
            F e;
            oneOperator(e, 0);
            e.setParameter(F::LegacyXinParam, 1.0f);
            e.setParameter(F::LegacyXinDepthParam, 1.0f);
            e.noteOn(69, 1.0f, -1);
            const auto from = render(e, 9600, both([](long) { return 1.0f; }));
            expect(rmsOfDifference(moved, from, 5300, 9600) < 1.0e-4, "at the set depth after 10 ms");
        }

        beginTest("An input beyond +-1 is held to +-1 before it reaches a phase: four times too loud is the clipped wave, sample for sample");
        {
            auto run = [](const Input& in)
            {
                F e;
                oneOperator(e, 0);
                e.setParameter(F::LegacyXinParam, 1.0f);
                e.setParameter(F::LegacyXinDepthParam, 1.0f);
                e.noteOn(69, 1.0f, -1);
                return render(e, 9600, in);
            };
            const auto s = sine(1100.0, 48000.0, 4.0f);
            const auto loud = run(both(s));
            const auto clipped = run(both([s](long n) { return juce::jlimit(-1.0f, 1.0f, s(n)); }));
            expect(same(loud, clipped), "held to +-1");
            // (and the clipped wave is not the plain one: the test tells the two apart)
            expect(rmsOfDifference(loud, run(both(sine(1100.0, 48000.0))), 0, 9600) > 0.02);
            // Each side may be anything: it is their mean that is held.
            const auto oneSide = run({ [](long) { return 1.0e30f; }, [](long) { return 0.0f; } });
            const auto atOne = run(both([](long) { return 1.0f; }));
            expect(same(oneSide, atOne), "1e30 on one side is an input of 1");
        }

        beginTest("Pressure on FM depth scales the input's modulation as it scales an operator's: XDPT 50 under full pressure is XDPT 100 without");
        {
            // (The same factor carries the INDX destination: both multiply what reaches the phase.)
            static AftertouchRouter router;
            router.setRoute(0, { AftertouchRouter::Destination::FMDepth, 1.0f, false });    // pressure 1 = twice the index
            router.setRouteCount(1);
            auto run = [](float depth, float pressure)
            {
                F e;
                oneOperator(e, 0);
                e.setAftertouchRouter(&router);
                e.setParameter(F::LegacyXinParam, 1.0f);
                e.setParameter(F::LegacyXinDepthParam, depth);
                e.noteOn(69, 1.0f, -1);
                if (pressure > 0.0f)
                    e.aftertouch(69, pressure);
                return render(e, 9600, both(sine(1100.0, 48000.0)));
            };
            const auto pressed = run(0.5f, 1.0f), full = run(1.0f, 0.0f), half = run(0.5f, 0.0f);
            const double level = rms(full, 2400, 9600);
            expect(level > 0.2);
            expect(rmsOfDifference(pressed, full, 2400, 9600) < 1.0e-4 * level, "pressed 50 against 100: "
                       + juce::String(rmsOfDifference(pressed, full, 2400, 9600) / level, 6));
            expect(rmsOfDifference(pressed, half, 2400, 9600) > 0.1 * level, "(and the pressure is heard)");
        }

        beginTest("Any input stays finite and bounded: not a number, infinite, 1e30; 22.05 to 192 kHz; blocks of 1 to 4096; short frames");
        {
            const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
            const std::vector<std::function<float (long)>> kinds = {
                [nan](long) { return nan; }, [inf](long) { return inf; }, [inf](long n) { return (n & 1) ? inf : -inf; },
                [](long) { return 1.0e30f; }, [](long n) { return (n % 3) ? -1.0e30f : 1.0e30f; },
                [nan, inf](long n) { return n % 5 == 0 ? nan : (n % 5 == 1 ? inf : (n % 5 == 2 ? -1.0e30f : 0.3f)); },
                noise() };
            int runs = 0;
            for (double sr : { 22050.0, 44100.0, 96000.0, 192000.0 })
                for (int block : { 1, 7, 64, 4096 })
                    for (size_t k = 0; k < kinds.size(); ++k)
                        for (int shape = 0; shape < 3; ++shape)
                        {
                            F e;
                            e.prepareToPlay(sr, block);
                            e.setParameter(F::AlgorithmParam, (float) ((int) k % 8));
                            for (int o = 0; o < 6; ++o)
                            {
                                e.setParameter(o * 10 + 1, 1.0f);
                                e.setParameter(o * 10 + 3, 1.0f);       // feedback at the top
                            }
                            e.setParameter(F::LegacyXinParam, shape == 2 ? 4.0f : 7.0f);
                            e.setParameter(F::LegacyXinDepthParam, 1.0f);
                            e.setModWheel(1.0f);                        // the strip triples the index
                            e.noteOn(0, 1.0f, -1);
                            e.noteOn(64, 1.0f, -1);
                            e.noteOn(127, 1.0f, -1);
                            Input in { kinds[k], shape == 1 ? kinds[(k + 1) % kinds.size()] : kinds[k] };
                            in.frames = shape == 0 ? -1 : (shape == 1 ? block / 2 : block + 5);
                            in.beyond = nan;
                            const int total = std::max(2 * block, (int) (0.03 * sr));
                            const auto out = render(e, total, in, block,
                                                    [&e, total](int at) { if (at >= total / 2) e.setParameter(F::LegacyXinDepthParam, 0.2f); });
                            float peak = 0.0f;
                            const bool ok = finiteAndWithin(out, 16.0f, peak);   // (six carriers x three notes x 0.7 x LEVL = 10, and the filter's overshoot)
                            expect(ok, "sr " + juce::String(sr) + " block " + juce::String(block) + " input " + juce::String((int) k)
                                           + " shape " + juce::String(shape) + ": peak " + juce::String(peak));
                            ++runs;
                        }
            expectEquals(runs, 4 * 4 * 7 * 3);
        }

        beginTest("Through AudioEngine: another track's sound (TRK FX page IN, MODE OFF) modulates the FM track; SRC OFF is the track alone");
        {
            auto run = [this](float xin, float src)
            {
                TrackInputTests::Rig r;
                for (int t = 0; t < 2; ++t)
                {
                    r.seq.getPattern().getTrack(t).engineType = EngineType::FM;
                    r.audio.addEngine(t, AudioEngine::createEngine(EngineType::FM));
                }
                r.audio.prepare(48000.0, TrackInputTests::Rig::BLOCK);
                r.audio.setTrackParameter(0, MixParam::VOLUME, 0.0f);           // the source feeds from before its fader
                r.audio.setTrackParameter(1, TrackInParam::SRC, src);
                r.audio.setTrackParameter(1, F::LegacyXinParam, xin);
                r.audio.setTrackParameter(1, F::LegacyXinDepthParam, 1.0f);
                expectEquals(r.audio.getEngine(1)->getParameter(mod::Router::xinId(mod::Router::XinDest1)),
                             xin == 0.0f ? 0.0f : (float) (FMVoice::LfoPhase + (int) xin - 1));
                r.blocks(20);           // (a new XIN is a swap, carried off by the track: let it pass)
                r.audio.getEngine(0)->noteOn(72, 1.0f, -1);
                r.audio.getEngine(1)->noteOn(57, 1.0f, -1);
                std::vector<float> out;
                for (int b = 0; b < 150; ++b)
                {
                    r.blocks(1);
                    out.insert(out.end(), r.left.begin(), r.left.end());
                }
                expectEquals(r.audio.getNonFiniteBlocks(), 0);
                return out;
            };
            const auto alone = run(0.0f, 1.0f), modulated = run(1.0f, 1.0f), unplugged = run(1.0f, 0.0f);
            const double level = rms(alone, 2400, 9600);
            expect(level > 0.01, "the FM track sounds: " + juce::String(level));
            expect(rmsOfDifference(alone, modulated, 2400, 9600) > 0.2 * level, "the source is heard in it: "
                       + juce::String(rmsOfDifference(alone, modulated, 2400, 9600) / level));
            expect(same(alone, unplugged), "XIN OP1 with SRC OFF is the track alone, bit for bit");
        }
    }
};

static XinFmTests xinFmTests;
