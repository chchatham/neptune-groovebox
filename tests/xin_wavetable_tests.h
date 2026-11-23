#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/MacroOscEngine.h"
#include "../src/engine/AftertouchRouter.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "u4_wavetable_tests.h"      // WavetableXinCells: the old XIN cells as router slots

// -----------------------------------------------------------------------------
// The track's input at audio rate in the wavetable engine (page XIN: XIN OFF /
// PM / POS, XDPT, XENV, XVEL, XWHL, XRCT, XLP, XHP), and pad pressure as the bow's
// energy in the macro oscillator's bowed string (the pressure route BOW).
//
// The engines are driven directly: the input through EngineBase::setAudioInput
// before every block (as the AudioEngine does), pressure through aftertouch()
// and a router of the test's own. "The same sound" is the same samples. Levels
// are RMS; a single partial of an unmodulated or phase-modulated sine is read
// with a Goertzel filter (those partials do not move); brightness is the RMS of
// the first difference over the RMS (rises with the share of high partials).
// -----------------------------------------------------------------------------
class XinWavetableTests : public juce::UnitTest
{
public:
    XinWavetableTests() : UnitTest("XIN Wavetable") {}

    using W = WavetableEngine;
    using X = WavetableXinCells;
    using M = MacroOscEngine;
    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;
    static constexpr int SINE_TABLE = 3, PWM_TABLE = 4;

    // ---- measuring ----------------------------------------------------------------------------
    static double rms(const std::vector<float>& y, int from, int to)
    {
        double sq = 0.0;
        for (int i = from; i < to; ++i)
            sq += (double) y[(size_t) i] * y[(size_t) i];
        return std::sqrt(sq / (double) std::max(1, to - from));
    }
    static double brightness(const std::vector<float>& y, int from, int to)
    {
        double sq = 0.0, dq = 0.0;
        for (int i = from + 1; i < to; ++i)
        {
            const double d = (double) y[(size_t) i] - y[(size_t) (i - 1)];
            dq += d * d;
            sq += (double) y[(size_t) i] * y[(size_t) i];
        }
        return std::sqrt(dq / std::max(sq, 1.0e-30));
    }
    // The amplitude of the partial at hz (Goertzel over a Hann window).
    static double partial(const std::vector<float>& y, int from, int to, double hz, double sr = SR)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * hz / sr, c = 2.0 * std::cos(w);
        const int n = to - from;
        double s1 = 0.0, s2 = 0.0, window = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double h = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * (double) i / (double) (n - 1));
            const double s0 = h * (double) y[(size_t) (from + i)] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
            window += h;
        }
        return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / window;
    }
    static float maxStep(const std::vector<float>& y, int from, int to)
    {
        float m = 0.0f;
        for (int i = std::max(1, from); i < to; ++i)
            m = std::max(m, std::abs(y[(size_t) i] - y[(size_t) (i - 1)]));
        return m;
    }
    static bool same(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && ! a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }
    static bool finiteAndUnder(const std::vector<float>& y, float bound)
    {
        for (float s : y)
            if (! std::isfinite(s) || std::abs(s) > bound)
                return false;
        return true;
    }
    static int at(double seconds, double sr = SR) { return (int) (seconds * sr); }

    // ---- an engine with something on its input --------------------------------------------------
    template <typename Engine>
    struct Rig
    {
        Engine engine;
        double sr;
        int block;
        AftertouchRouter router;
        std::function<float(long long)> left, right;       // the input per sample; none = nothing connected
        int frames = -1;                                    // frames given a block; -1 = the whole block
        bool oneChannel = false;                            // right = nullptr
        long long clock = 0;
        std::vector<float> inL, inR;

        explicit Rig(double sampleRate = SR, int blockSize = BLOCK) : sr(sampleRate), block(blockSize)
        {
            router.clear();
            engine.setAftertouchRouter(&router);
            engine.prepareToPlay(sr, block);
            inL.assign((size_t) block, 0.0f);
            inR.assign((size_t) block, 0.0f);
        }
        EngineBase& base() { return engine; }
        // (The wavetable engine's old XIN cells, 65-72, are set as the router slots they became.)
        X xin;
        void set(int id, float v)
        {
            if (std::is_same<Engine, W>::value && X::owns(id))
                xin.set(engine, id, v);
            else
                engine.setParameter(id, v);
        }
        void sine(double hz, float level)
        {
            const double rate = sr;
            left = right = [hz, level, rate](long long n) { return level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * hz * (double) n / rate); };
        }

        // Push the local AftertouchRouter's routes to the engine's pressure slots.
        void pushRoutes() { engine.setAftertouchRouter(&router); }

        // `samples` more (a whole number of blocks); `each` runs before every block with its number.
        std::vector<float> render(int samples, const std::function<void(int)>& each = {})
        {
            pushRoutes();
            std::vector<float> out;
            out.reserve((size_t) samples + (size_t) block);
            juce::AudioBuffer<float> buf(2, block);
            juce::MidiBuffer midi;
            for (int b = 0; (int) out.size() < samples; ++b)
            {
                if (each)
                    each(b);
                if (left)
                {
                    for (int i = 0; i < block; ++i)
                    {
                        inL[(size_t) i] = left(clock + i);
                        inR[(size_t) i] = right ? right(clock + i) : inL[(size_t) i];
                    }
                    engine.setAudioInput({ inL.data(), oneChannel ? nullptr : inR.data(), frames < 0 ? block : std::min(frames, block), 0.5f });
                }
                else
                    engine.setAudioInput({});
                buf.clear();
                engine.processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back(buf.getSample(0, i));
                clock += block;
            }
            return out;
        }
    };
    using WRig = Rig<W>;
    using MRig = Rig<M>;

    // A sine table held on A4, a 1 kHz sine on the input: what the PM tests start from.
    static void pmSound(WRig& r, float depth = 0.5f, float inputLevel = 0.5f)
    {
        r.set(W::TableSelect, (float) SINE_TABLE);
        r.set(W::AmpSustain, 1.0f);
        r.set(X::Mode, (float) X::Phase);
        r.set(X::Depth, depth);
        r.sine(1000.0, inputLevel);
    }
    // The first sideband's share of the carrier: 440 Hz carrier, 1 kHz modulator -> 1440 Hz.
    static double sideband(const std::vector<float>& y, double hz = 1440.0)
    {
        return partial(y, at(0.3), at(0.8), hz) / std::max(1.0e-12, partial(y, at(0.3), at(0.8), 440.0));
    }

    // The bowed string under one pad.
    static void bowSound(MRig& r, bool route = true)
    {
        r.set(M::ModelParam, (float) (int) MacroOscVoice::Model::BowedString);
        r.set(M::AmpSustain, 1.0f);
        if (route)
            r.router.setRoute(0, { AftertouchRouter::Destination::Excite, 1.0f, false });
    }
    static constexpr int BOW_NOTE = 48;
    static std::vector<float> bowAt(float pressure, double seconds = 1.5, int note = BOW_NOTE)
    {
        MRig r;
        bowSound(r);
        r.base().noteOn(note, 0.8f);
        return r.render(at(seconds), [&](int b) { if (b == 1) r.engine.aftertouch(note, pressure); });
    }

    void runTest() override
    {
        wavetableTests();
        bowTests();
    }

    // =============================================================================================
    void wavetableTests()
    {
        beginTest("The XIN page is the router's (ids 182-189, after the filter's pages); the old cells 65-72 are slots (WavetableXinCells)");
        {
            W e;
            const auto pages = e.getParamPages();
            int index = -1;
            for (int i = 0; i < (int) pages.size(); ++i)
                if (pages[(size_t) i].name == "XIN")
                    index = i;
            expect(index > 0, "there is a page XIN");
            if (index > 0)
            {
                expectEquals(pages[(size_t) (index - 1)].name, juce::String("F ENV"));
                expectEquals(pages[(size_t) (index + 1)].name, juce::String("SLT1"));
                expectEquals(pages[(size_t) index].section, juce::String("XIN"));
                static const char* const labels[8] = { "DST1", "AMT1", "FLT1", "LPF", "DST2", "AMT2", "FLT2", "HPF" };
                // (LPF and HPF open, as this engine's XLP and XHP were: its source XFLT reads them.)
                static const float defaults[8] = { 0.0f, 0.0f, 0.0f, 20000.0f, 0.0f, 0.0f, 0.0f, 20.0f };
                for (int c = 0; c < 8; ++c)
                {
                    const auto& p = pages[(size_t) index].params[(size_t) c];
                    expect(p.isValid(), labels[c]);
                    expectEquals(juce::String(p.label), juce::String(labels[c]));
                    expectEquals(p.id, 182 + c);
                    expect(EngineParamRange::contains(p.id) && mod::Router::owns(p.id));
                    expectEquals(p.defaultVal, defaults[c], labels[c]);
                    expectEquals(e.getParameter(p.id), defaults[c], juce::String(labels[c]) + " seeded");
                    expect(! e.isRebuildParam(p.id), "a step lock may hold it");
                    expect(e.getParameterName(p.id).startsWith("Input"), e.getParameterName(p.id));
                    e.setParameter(p.id, p.maxVal);
                    expectEquals(e.getParameter(p.id), p.maxVal, juce::String(labels[c]) + " is returned");
                }
                // What the old XIN cell chose between is two destinations now: PM (new, at the end
                // of the table) and POS (where it always was).
                expectEquals(W::dests().phase, 41);
                expectEquals((int) WavetableVoice::LfoPosition, 6);
                expectEquals((int) X::Off, 0);
                expectEquals((int) X::Phase, 1);
                expectEquals((int) X::Position, 2);
            }
            expect(pages[pages.size() - 3].name == "STRIP" && pages[pages.size() - 2].name == "LFO1" && pages.back().name == "LFO2");
            // The destinations are the list they were, the filter's dual block after it, then ETIM, FTIM and PM.
            expectEquals(W::lfoDests().size(), 42);
            expectEquals(W::lfoDests().indexOfLabel("LDPT"), 33);
            // No page shows a cell at 65-72 any more.
            for (auto& page : pages)
                for (auto& p : page.params)
                    expect(! (p.isValid() && X::owns(p.id)), juce::String(page.name) + " " + p.label);
        }

        beginTest("Off is the same samples: XIN OFF with an input, PM and POS with nothing connected, depth 0, an input that sums to nothing");
        {
            struct Sound { const char* name; std::function<void(WRig&)> make; };
            const Sound sounds[] = {
                { "default", [](WRig&) {} },
                { "PWM table, spread, sub, motion", [](WRig& r) { r.set(W::TableSelect, (float) PWM_TABLE); r.set(W::UnisonSpread, 0.4f);
                                                                  r.set(W::SubLevel, 0.5f); r.set(W::MorphSpeed, 3.0f); r.set(W::Position, 0.3f); } },
                { "scan and an LFO on POS", [](WRig& r) { r.set(W::TableSelect, (float) PWM_TABLE); r.set(W::ScanMode, (float) (int) WavetableScanMode::Ping);
                                                          r.set(W::ScanTime, 0.2f); r.set(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) WavetableVoice::LfoPosition);
                                                          r.set(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.6f); } } };
            struct Case { const char* name; int mode; float depth; int input; };      // input: 0 none, 1 a sine, 2 left = -right
            const Case cases[] = { { "XIN OFF, input connected", 0, 1.0f, 1 }, { "PM, nothing connected", 1, 1.0f, 0 },
                                   { "POS, nothing connected", 2, 1.0f, 0 }, { "PM, depth 0", 1, 0.0f, 1 }, { "POS, depth 0", 2, 0.0f, 1 },
                                   { "PM, left = -right", 1, 1.0f, 2 }, { "POS, left = -right", 2, 1.0f, 2 } };
            for (auto& sound : sounds)
            {
                auto play = [&](WRig& r)
                {
                    sound.make(r);
                    return r.render(at(0.5), [&r](int b)
                    {
                        if (b == 0) { r.base().noteOn(57, 0.9f); r.base().noteOn(64, 0.6f); }
                        if (b == 250) r.base().noteOff(57);
                    });
                };
                WRig plain;
                const auto reference = play(plain);
                expect(rms(reference, 0, (int) reference.size()) > 0.02, "it sounds");
                for (auto& c : cases)
                {
                    WRig r;
                    r.set(X::Mode, (float) c.mode);
                    r.set(X::Depth, c.depth);
                    for (int id : { (int) X::Rectify, (int) X::Env, (int) X::Vel })
                        r.set(id, c.input == 2 ? 0.0f : 0.7f);
                    r.set(X::LowPass, 3000.0f);
                    r.set(X::HighPass, 200.0f);
                    if (c.input >= 1)
                        r.sine(1000.0, 0.8f);
                    if (c.input == 2)
                        r.right = [l = r.left](long long n) { return -l(n); };
                    // (A slot from the input aimed at POS whose input sums to nothing moves nothing and
                    // changes nothing: the LFO's share still stops at the end of the table, U4-1.)
                    const auto played = play(r);
                    bool equal = same(played, reference);
                    float apart = 0.0f;
                    if (! equal && juce::String(sound.name).startsWith("scan and an LFO") && c.mode == 2)
                    {
                        // (A destination with an audio-rate slot aimed at it is worked out sample by
                        // sample by the router, in another order of operations than its ramp between
                        // sub-blocks: with the LFO on POS too, the last bits of the position differ.)
                        for (size_t i = 0; i < played.size(); ++i)
                            apart = std::max(apart, std::abs(played[i] - reference[i]));
                        equal = apart < 1.0e-4f;
                    }
                    expect(equal, juce::String(sound.name) + ": " + c.name + (apart > 0.0f ? " (" + juce::String(apart, 7) + " apart)" : juce::String()));
                }
            }
        }

        beginTest("PM: the input's sidebands around the note, as large as the depth says; the level stays");
        {
            WRig off;
            pmSound(off);
            off.set(X::Mode, 0.0f);
            off.base().noteOn(69, 1.0f);
            const auto dry = off.render(at(0.8));
            expect(sideband(dry) < 0.002, "no sideband without PM: " + juce::String(sideband(dry), 5));

            // XDPT 50 and an input of half level: 2 cycles x 0.25 x 0.5 = a quarter cycle = pi / 2:
            // the carrier is J0(pi / 2) = 0.4720, each first sideband J1(pi / 2) = 0.5668.
            WRig on;
            pmSound(on);
            on.base().noteOn(69, 1.0f);
            const auto wet = on.render(at(0.8));
            const double upper = sideband(wet), lower = sideband(wet, 560.0), second = sideband(wet, 2440.0);
            logMessage("PM at XDPT 50: 1440 Hz " + juce::String(upper, 3) + ", 560 Hz " + juce::String(lower, 3) + ", 2440 Hz "
                       + juce::String(second, 3) + " of the carrier (1.201, 1.201, 0.529 from the Bessel functions)");
            expectWithinAbsoluteError(upper, 0.5668 / 0.4720, 0.06);
            expectWithinAbsoluteError(lower, 0.5668 / 0.4720, 0.06);
            expectWithinAbsoluteError(second, 0.2497 / 0.4720, 0.04);
            const double carrier = partial(wet, at(0.3), at(0.8), 440.0) / partial(dry, at(0.3), at(0.8), 440.0);
            expectWithinAbsoluteError(carrier, 0.4720, 0.02, "the carrier gives way to them");
            expectWithinAbsoluteError(rms(wet, at(0.3), at(0.8)) / rms(dry, at(0.3), at(0.8)), 1.0, 0.03, "a phase-modulated sine keeps its level");

            // More depth, more of it; a louder input likewise. The depth is squared.
            double last = 0.0;
            for (float depth : { 0.1f, 0.25f, 0.4f })
            {
                WRig r;
                pmSound(r, depth);
                r.base().noteOn(69, 1.0f);
                const double s = sideband(r.render(at(0.8)));
                expect(s > last * 1.5, "XDPT " + juce::String(depth) + ": " + juce::String(s, 4));
                // Small index: J1 / J0 = index / 2, and the index is 2 pi x 2 x depth^2 x 0.5.
                if (depth < 0.3f)
                    expectWithinAbsoluteError(s, juce::MathConstants<double>::pi * depth * depth, 0.12 * juce::MathConstants<double>::pi * depth * depth + 0.002);
                last = s;
            }
            WRig quiet;
            pmSound(quiet, 0.25f, 0.25f);
            quiet.base().noteOn(69, 1.0f);
            WRig loud;
            pmSound(loud, 0.25f, 0.5f);
            loud.base().noteOn(69, 1.0f);
            expectWithinAbsoluteError(sideband(loud.render(at(0.8))) / sideband(quiet.render(at(0.8))), 2.0, 0.1, "twice the input, twice the index");

            // Both channels are added to one: the same on the left alone (right missing) at twice the level.
            WRig both;
            pmSound(both, 0.25f, 0.4f);
            both.base().noteOn(69, 1.0f);
            WRig leftOnly;
            pmSound(leftOnly, 0.25f, 0.4f);
            leftOnly.oneChannel = true;
            leftOnly.base().noteOn(69, 1.0f);
            expect(same(both.render(at(0.2)), leftOnly.render(at(0.2))), "no right channel: the left is the input");
            WRig halves;
            pmSound(halves, 0.25f, 0.8f);
            halves.right = [](long long) { return 0.0f; };
            halves.base().noteOn(69, 1.0f);
            WRig whole;
            pmSound(whole, 0.25f, 0.4f);
            whole.base().noteOn(69, 1.0f);
            expectWithinAbsoluteError(sideband(halves.render(at(0.8))) / sideband(whole.render(at(0.8))), 1.0, 0.02, "the mean of the two channels");
        }

        beginTest("PM is per voice and reaches the spread copy; the sub stays a sine");
        {
            // Two notes: each has its own sidebands (1 kHz either side of each note).
            WRig r;
            pmSound(r);
            r.base().noteOn(69, 1.0f);
            r.base().noteOn(76, 1.0f);      // 659.26 Hz
            const auto y = r.render(at(0.8));
            const double a = partial(y, at(0.3), at(0.8), 440.0), b = partial(y, at(0.3), at(0.8), 659.2551);
            expect(partial(y, at(0.3), at(0.8), 1440.0) > a && partial(y, at(0.3), at(0.8), 1659.2551) > b);

            // The sub (220 Hz under A4) has no sidebands: 1220 Hz stays empty.
            WRig sub;
            pmSound(sub);
            sub.set(W::SubLevel, 1.0f);
            sub.base().noteOn(69, 1.0f);
            const auto s = sub.render(at(0.8));
            expect(partial(s, at(0.3), at(0.8), 220.0) > 0.05, "the sub sounds");
            expect(partial(s, at(0.3), at(0.8), 1220.0) < 0.01 * partial(s, at(0.3), at(0.8), 220.0), "and is not modulated");

            // SPRD: the second copy (30 cents up at 1) carries the sidebands too.
            WRig spread;
            pmSound(spread);
            spread.set(W::UnisonSpread, 1.0f);
            spread.base().noteOn(69, 1.0f);
            const auto p = spread.render(at(1.0));
            const double copy = 440.0 * std::pow(2.0, 30.0 / 1200.0);
            expect(partial(p, at(0.3), at(1.0), copy + 1000.0) > 0.8 * partial(p, at(0.3), at(1.0), copy), "the copy's sideband");
        }

        beginTest("POS: the input moves the read position around POS at audio rate, mirrored at the ends of the table");
        {
            expect(W().getTable(PWM_TABLE).numFrames > 8, "a table whose frames differ");
            // One-sample blocks, so the position of every sample can be read.
            auto positions = [this](float pos, float depth, float level, std::vector<float>* sound = nullptr)
            {
                WRig r(SR, 1);
                r.set(W::TableSelect, (float) PWM_TABLE);
                r.set(W::Position, pos);
                r.set(X::Mode, (float) X::Position);
                r.set(X::Depth, depth);
                r.sine(997.0, level);
                r.render(1);        // (the input's first sample, a zero, before the note: a block whose input is silent folds nothing, U4-1)
                r.base().noteOn(57, 1.0f);
                std::vector<float> where;
                for (int i = 0; i < at(0.1); ++i)
                {
                    const auto y = r.render(1);
                    if (sound != nullptr)
                        sound->push_back(y[0]);
                    where.push_back(r.engine.getVoice(0).getReadPosition());
                    expectWithinAbsoluteError(r.engine.getVoice(0).getEffectivePosition(), pos, 1.0e-6f);    // what the screens show stays POS
                }
                return where;
            };
            auto range = [](const std::vector<float>& w, float& low, float& high, float& step)
            {
                low = 2.0f; high = -1.0f; step = 0.0f;
                for (size_t i = 0; i < w.size(); ++i)
                {
                    low = std::min(low, w[i]);
                    high = std::max(high, w[i]);
                    if (i > 0) step = std::max(step, std::abs(w[i] - w[i - 1]));
                }
            };
            float low, high, step;
            range(positions(0.5f, 0.5f, 0.6f), low, high, step);
            expectWithinAbsoluteError(low, 0.2f, 0.005f, "POS 50, XDPT 50, input 0.6: down to 20 %");
            expectWithinAbsoluteError(high, 0.8f, 0.005f, "and up to 80 %");
            range(positions(0.5f, 0.25f, 0.6f), low, high, step);
            expectWithinAbsoluteError(high - low, 0.3f, 0.005f, "half the depth, half the way");

            // Past an end: it comes back in. At POS 10 % with +-50 %: 0 .. 60 %, never outside,
            // and no step larger than the input's own.
            const float inputStep = 0.5f * 2.0f * juce::MathConstants<float>::pi * 997.0f / (float) SR;
            range(positions(0.1f, 0.5f, 1.0f), low, high, step);
            expect(low >= 0.0f && low < 0.01f, "the bottom is reached: " + juce::String(low, 4));
            expectWithinAbsoluteError(high, 0.6f, 0.005f);
            expect(step <= inputStep * 1.001f, "mirrored, not jumped: " + juce::String(step, 5) + " against " + juce::String(inputStep, 5));
            range(positions(0.95f, 1.0f, 1.0f), low, high, step);
            expect(low >= 0.0f && high <= 1.0f, "the whole table each way, inside it: " + juce::String(low, 3) + " .. " + juce::String(high, 3));
            expect(high > 0.99f && low < 0.01f);
            expect(step <= 2.0f * inputStep * 1.001f);

            // And it is heard: the sound differs from the still one by a good part of its level,
            // with new partials beside the note's own (997 Hz either side of 220 Hz).
            std::vector<float> moved, still;
            positions(0.5f, 0.5f, 0.6f, &moved);
            positions(0.5f, 0.0f, 0.6f, &still);
            double diff = 0.0;
            for (size_t i = 0; i < moved.size(); ++i)
                diff += (double) (moved[i] - still[i]) * (moved[i] - still[i]);
            const double changed = std::sqrt(diff / (double) moved.size()) / rms(still, 0, (int) still.size());
            logMessage("POS: the moved sound differs from the still one by " + juce::String(changed, 3) + " of its level");
            expect(changed > 0.15);
            const int from = at(0.02), to = at(0.1);
            expect(partial(moved, from, to, 220.0 + 997.0) > 20.0 * partial(still, from, to, 220.0 + 997.0),
                   "a sideband at 1217 Hz: " + juce::String(partial(moved, from, to, 1217.0), 4) + " against " + juce::String(partial(still, from, to, 1217.0), 5));
            expect(finiteAndUnder(moved, 2.0f));
        }

        beginTest("POS at the ends of the table adds no clicks: the mirrored movement is as smooth as the movement in the middle");
        {
            // The largest step of the sound with the position swinging across an end, against the
            // same swing in the middle of the table (where nothing is mirrored).
            auto steps = [](float pos)
            {
                WRig r;
                r.set(W::TableSelect, (float) PWM_TABLE);
                r.set(W::Position, pos);
                r.set(X::Mode, (float) X::Position);
                r.set(X::Depth, 0.3f);
                r.sine(331.0, 1.0f);
                r.base().noteOn(45, 1.0f);
                const auto y = r.render(at(0.4));
                return maxStep(y, at(0.1), at(0.4));
            };
            const float middle = steps(0.5f), bottom = steps(0.0f), top = steps(1.0f);
            logMessage("POS largest step: middle " + juce::String(middle, 4) + ", at 0 " + juce::String(bottom, 4) + ", at 100 " + juce::String(top, 4));
            expect(bottom < 1.5f * middle && top < 1.5f * middle);
        }

        beginTest("XENV, XVEL, XWHL: the depth follows the envelope, the velocity and the strip");
        {
            auto make = [](WRig& r, float velocity)
            {
                pmSound(r, 0.4f);
                r.set(W::VelSens, 0.0f);
                r.base().noteOn(69, velocity);
            };
            // XVEL: a soft note has less of it (depth x 0.25 -> index x 1 / 16).
            WRig full, soft;
            make(full, 0.25f);
            soft.set(X::Vel, 1.0f);
            make(soft, 0.25f);
            const double a = sideband(full.render(at(0.8))), b = sideband(soft.render(at(0.8)));
            logMessage("XVEL 0 / 100 at velocity 0.25: " + juce::String(a, 4) + " / " + juce::String(b, 5));
            expect(b < a / 10.0 && b > a / 30.0);
            WRig hard;
            hard.set(X::Vel, 1.0f);
            make(hard, 1.0f);
            WRig hardPlain;
            make(hardPlain, 1.0f);
            expectWithinAbsoluteError(sideband(hard.render(at(0.8))), sideband(hardPlain.render(at(0.8))), 1.0e-4, "full velocity: all of the depth");

            // XENV: with the sustain at a half, half the depth (a quarter of the index) while held.
            WRig env, flat;
            for (auto* r : { &env, &flat })
            {
                r->set(X::Env, r == &env ? 1.0f : 0.0f);
                make(*r, 1.0f);
                r->set(W::AmpDecay, 0.05f);
                r->set(W::AmpSustain, 0.5f);
            }
            const double held = sideband(env.render(at(0.8))), whole = sideband(flat.render(at(0.8)));
            logMessage("XENV 100 / 0 at SUS 50: " + juce::String(held, 4) + " / " + juce::String(whole, 4));
            expect(held < whole / 3.0 && held > whole / 5.0);

            // XWHL: XDPT 0 and the strip up is the same as XDPT at the strip's share.
            WRig strip, none, direct;
            for (auto* r : { &strip, &none, &direct })
            {
                make(*r, 1.0f);
                r->set(X::Depth, r == &direct ? 0.4f : 0.0f);
                r->set(X::Strip, r == &direct ? 0.0f : 0.4f);
                // (the strip's own routes off: only XWHL hears it)
                r->set(EngineLfoSection::stripAmountId(0), 0.0f);
                r->set(EngineLfoSection::STRIP_VIBRATO, 0.0f);
            }
            strip.engine.setModWheel(1.0f);
            direct.engine.setModWheel(1.0f);
            const double up = sideband(strip.render(at(0.8))), down = sideband(none.render(at(0.8))), set = sideband(direct.render(at(0.8)));
            logMessage("XWHL 40: strip up " + juce::String(up, 4) + ", down " + juce::String(down, 5) + "; XDPT 40: " + juce::String(set, 4));
            expect(down < 0.002);
            expectWithinAbsoluteError(up, set, 0.01);
        }

        beginTest("XRCT, XLP, XHP shape the input before it modulates");
        {
            auto run = [](int id, float value, double hz = 1440.0)
            {
                WRig r;
                pmSound(r, 0.4f);
                if (id >= 0)
                    r.set(id, value);
                r.base().noteOn(69, 1.0f);
                return sideband(r.render(at(0.8)), hz);
            };
            const double plain = run(-1, 0.0f);
            // The absolute value of a 1 kHz sine has no 1 kHz in it: 2 kHz and its multiples.
            expect(run(X::Rectify, 1.0f) < 0.02 * plain, "rectified: nothing 1 kHz from the note: " + juce::String(run(X::Rectify, 1.0f), 5));
            const double octave = run(X::Rectify, 1.0f, 2440.0);
            expect(octave > 0.15 && octave > 10.0 * run(X::Rectify, 1.0f), "but 2 kHz from it: " + juce::String(octave, 4));
            const double halfway = run(X::Rectify, 0.5f);
            expect(halfway < 0.6 * plain && halfway > 0.4 * plain, "half way: half of it");
            // The filters: 12 dB an octave (the SVF), so a decade away is about a hundredth.
            expect(run(X::LowPass, 100.0f) < 0.03 * plain, "XLP 100 Hz takes a 1 kHz input away: " + juce::String(run(X::LowPass, 100.0f) / plain, 4));
            expect(run(X::LowPass, 10000.0f) > 0.9 * plain, "XLP 10 kHz leaves it");
            expect(run(X::HighPass, 5000.0f) < 0.08 * plain, "XHP 5 kHz takes it away: " + juce::String(run(X::HighPass, 5000.0f) / plain, 4));
            expect(run(X::HighPass, 100.0f) > 0.9 * plain, "XHP 100 Hz leaves it");
        }

        beginTest("Nothing is jumped to: XIN, XDPT, the input going and a short input under a held note");
        {
            // The largest step of the sound in the 10 ms after each change, against the largest
            // step of the same sound left alone: a sine, its phase swung two cycles each way by
            // a slow input, so that the phase is nearly always far from where it would be without
            // the input - a change that was jumped to would be a step of up to the whole wave.
            struct Change { const char* name; float allowed; std::function<void(WRig&)> act; };
            const Change changes[] = {
                { "XIN OFF -> PM", 1.25f, [](WRig& r) { r.set(X::Mode, 1.0f); } },
                { "XIN PM -> OFF", 1.25f, [](WRig& r) { r.set(X::Mode, 0.0f); } },
                { "XIN PM -> POS", 1.25f, [](WRig& r) { r.set(X::Mode, 2.0f); } },
                { "XDPT 100 -> 0", 1.25f, [](WRig& r) { r.set(X::Depth, 0.0f); } },
                // (The input itself is let go of in four milliseconds: faster than this one ever moved, but no step.)
                { "the input is taken away", 2.5f, [](WRig& r) { r.left = nullptr; } },
                { "the input stops half way through every block", 2.5f, [](WRig& r) { r.frames = BLOCK / 2; } } };
            auto slowSwing = [](WRig& r)
            {
                pmSound(r, 1.0f, 1.0f);
                r.sine(53.0, 1.0f);
                r.base().noteOn(57, 1.0f);
            };
            WRig steady;
            slowSwing(steady);
            const float scale = maxStep(steady.render(at(0.4)), at(0.1), at(0.4));
            for (auto& c : changes)
            {
                WRig r;
                slowSwing(r);
                if (juce::String(c.name) == "XIN OFF -> PM")
                    r.set(X::Mode, 0.0f);
                const int when = 150;
                const auto y = r.render(at(0.4), [&](int b) { if (b == when) c.act(r); });
                const float step = maxStep(y, when * BLOCK, when * BLOCK + at(0.01));
                expect(step < c.allowed * scale, juce::String(c.name) + ": " + juce::String(step, 4) + " against " + juce::String(scale, 4));
                expect(finiteAndUnder(y, 2.0f));
            }
            // A step lock on XDPT arrives over about four milliseconds, not in one sample: the
            // sideband a quarter of a millisecond after it is still small.
            WRig lock;
            pmSound(lock, 0.0f, 1.0f);
            lock.base().noteOn(69, 1.0f);
            const auto y = lock.render(at(0.3), [&](int b) { if (b == 100) lock.set(X::Depth, 1.0f); });
            const float before = maxStep(y, at(0.1), 100 * BLOCK), justAfter = maxStep(y, 100 * BLOCK, 100 * BLOCK + 12);
            expect(justAfter < 1.5f * before, "XDPT 0 -> 100 does not arrive at once: " + juce::String(justAfter, 4) + " against " + juce::String(before, 4));
            expect(maxStep(y, at(0.2), at(0.3)) > 3.0f * before, "but it arrives");
        }

        beginTest("Any input, rate and block size: finite and bounded");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            const std::function<float(long long)> inputs[] = {
                [nan](long long) { return nan; },
                [inf](long long n) { return (n & 1) ? inf : -inf; },
                [](long long n) { return (n % 7) < 3 ? 1.0e30f : -1.0e30f; },
                [nan, inf](long long n) { const float v[] = { 0.5f, nan, -0.9f, inf, 1.0e30f, -inf, 0.0f, -1.0e30f }; return v[n & 7]; },
                [](long long n) { return (n & 1) ? 1.0f : -1.0f; } };
            int runs = 0;
            for (double sr : { 22050.0, 44100.0, 96000.0, 192000.0 })
                for (int block : { 1, 7, 64, 4096 })
                    for (int mode : { 1, 2 })
                    {
                        const auto& input = inputs[(size_t) (runs % 5)];
                        WRig r(sr, block);
                        r.set(W::TableSelect, (float) (runs % 3 == 0 ? PWM_TABLE : (runs % 3 == 1 ? 0 : SINE_TABLE)));
                        r.set(X::Mode, (float) mode);
                        r.set(X::Depth, 1.0f);
                        // Every cell of the page at an end, another one each run.
                        r.set(X::Rectify, (runs & 1) ? 1.0f : 0.0f);
                        r.set(X::LowPass, (runs & 2) ? 20.0f : 20000.0f);
                        r.set(X::HighPass, (runs & 4) ? 5000.0f : 20.0f);
                        r.set(X::Env, (runs & 8) ? 1.0f : 0.0f);
                        r.set(X::Vel, (runs & 16) ? 1.0f : 0.0f);
                        r.set(X::Strip, 1.0f);
                        r.set(W::UnisonSpread, (runs & 2) ? 1.0f : 0.0f);
                        r.set(W::Position, (runs & 1) ? 1.0f : 0.0f);
                        r.engine.setModWheel(1.0f);
                        r.left = r.right = input;
                        r.frames = (runs % 4 == 3) ? std::max(0, block / 2) : -1;       // sometimes shorter than the block
                        r.oneChannel = runs % 5 == 2;
                        for (int note : { 0, 60, 127 })
                            r.base().noteOn(note, 1.0f);
                        const int total = std::max(block * 3, (int) (0.05 * sr));
                        const auto y = r.render(total, [&](int b) { if (b == 2 && block >= 64) r.base().noteOff(60); });
                        expect(finiteAndUnder(y, 6.0f), "rate " + juce::String(sr) + ", block " + juce::String(block) + ", mode " + juce::String(mode)
                                                        + ", input " + juce::String(runs % 5));
                        // And it goes on sounding afterwards with a sane input.
                        r.sine(500.0, 0.5f);
                        r.frames = -1;
                        const auto after = r.render(std::max(block * 2, (int) (0.02 * sr)));
                        expect(finiteAndUnder(after, 6.0f) && rms(after, 0, (int) after.size()) > 1.0e-4, "and sounds on");
                        ++runs;
                    }
            expectEquals(runs, 32);

            // An input far out of range leaves nothing behind in the input's filters: a tenth of
            // a second of silence later the sound is the one that never had it.
            auto afterwards = [](bool garbage)
            {
                WRig r;
                pmSound(r, 0.6f, 1.0f);
                r.set(X::LowPass, 20.0f);
                r.set(X::HighPass, 5000.0f);
                r.base().noteOn(69, 1.0f);
                if (garbage)
                    r.left = r.right = [](long long n) { return (n % 5) < 3 ? 1.0e30f : -3.0e29f; };
                else
                    r.left = r.right = [](long long) { return 0.0f; };
                r.render(BLOCK * 20);
                r.left = r.right = [](long long) { return 0.0f; };
                r.render(at(0.1));
                return r.render(at(0.05));
            };
            const auto clean = afterwards(false), recovered = afterwards(true);
            double apart = 0.0;
            for (size_t i = 0; i < clean.size(); ++i)
                apart += (double) (clean[i] - recovered[i]) * (clean[i] - recovered[i]);
            expect(std::sqrt(apart / (double) clean.size()) < 0.001 * rms(clean, 0, (int) clean.size()),
                   "after 1e30 on the input: " + juce::String(std::sqrt(apart / (double) clean.size()), 6) + " apart");

            // The parameters themselves at values no page gives.
            WRig wild;
            pmSound(wild, 1.0f, 1.0f);
            for (int id = X::Mode; id <= X::HighPass; ++id)
                for (float v : { nan, inf, -inf, 1.0e30f, -1.0e30f })
                {
                    wild.set(id, v);
                    wild.base().noteOn(60, 1.0f);
                    expect(finiteAndUnder(wild.render(BLOCK * 8), 6.0f), "cell " + juce::String(id) + " at " + juce::String(v));
                    wild.set(id, id == X::Mode ? 1.0f : 0.5f);
                }
            expect(WavetableVoice::foldPosition(nan) == 0.0f && WavetableVoice::foldPosition(inf) == 0.0f);
            for (float p : { -3.7f, -1.0f, -0.25f, 0.0f, 0.4f, 1.0f, 1.3f, 2.0f, 2.6f, 1.0e9f })
                expect(WavetableVoice::foldPosition(p) >= 0.0f && WavetableVoice::foldPosition(p) <= 1.0f);
            expectWithinAbsoluteError(WavetableVoice::foldPosition(-0.25f), 0.25f, 1.0e-6f);
            expectWithinAbsoluteError(WavetableVoice::foldPosition(1.3f), 0.7f, 1.0e-6f);
            expectWithinAbsoluteError(WavetableVoice::foldPosition(2.6f), 0.6f, 1.0e-6f);
        }

        beginTest("Review: POS follows the mirror sample by sample, XDPT is moved to in POS too, and a voice that has ended lets the input go");
        {
            // The read position of every sample is the mirror of POS + XDPT x input: at POS 10 %
            // with +-50 % the troughs of the input come back up to 40 % (a hard stop would sit at 0).
            {
                WRig r(SR, 1);
                r.set(W::TableSelect, (float) PWM_TABLE);
                r.set(W::Position, 0.1f);
                r.set(X::Mode, (float) X::Position);
                r.set(X::Depth, 0.5f);
                r.sine(997.0, 1.0f);
                r.render(1);        // (the input's first sample, a zero, before the note: a block whose input is silent folds nothing, U4-1)
                r.base().noteOn(57, 1.0f);
                float worst = 0.0f, deepest = 0.0f;
                for (int i = 0; i < at(0.05); ++i)
                {
                    const float x = r.left(r.clock);
                    r.render(1);
                    const float expected = std::abs(0.1f + 0.5f * x);
                    worst = std::max(worst, std::abs(r.engine.getVoice(0).getReadPosition() - expected));
                    if (x < -0.95f)
                        deepest = std::max(deepest, r.engine.getVoice(0).getReadPosition());
                }
                expect(worst < 1.0e-4f, "the read position is the mirrored one: " + juce::String(worst, 6) + " off");
                expect(deepest > 0.37f, "at the input's trough the position has come back in: " + juce::String(deepest, 4));
            }
            // XDPT 0 -> 100 in POS under a held note with the input at a constant 1: the position
            // moves there over about 4 ms (192 samples), it is not there the sample after.
            {
                WRig r(SR, 1);
                r.set(W::TableSelect, (float) PWM_TABLE);
                r.set(W::Position, 0.2f);
                r.set(X::Mode, (float) X::Position);
                r.set(X::Depth, 0.0f);
                r.left = r.right = [](long long) { return 0.5f; };
                r.base().noteOn(57, 1.0f);
                r.render(at(0.05));
                expectWithinAbsoluteError(r.engine.getVoice(0).getReadPosition(), 0.2f, 1.0e-5f);
                r.set(X::Depth, 1.0f);
                r.render(1);
                const float first = r.engine.getVoice(0).getReadPosition();
                expect(first > 0.2f && first < 0.22f, "one sample after: " + juce::String(first, 4));
                r.render(47);
                const float mid = r.engine.getVoice(0).getReadPosition();
                expect(mid > 0.25f && mid < 0.45f, "a millisecond after: " + juce::String(mid, 4));
                r.render(at(0.02));
                expectWithinAbsoluteError(r.engine.getVoice(0).getReadPosition(), 0.7f, 1.0e-4f);
                // And the mode: POS -> OFF glides back the same way.
                r.set(X::Mode, 0.0f);
                r.render(1);
                expect(r.engine.getVoice(0).getReadPosition() > 0.68f, "XIN POS -> OFF is not jumped to");
                r.render(at(0.02));
                expectWithinAbsoluteError(r.engine.getVoice(0).getReadPosition(), 0.2f, 1.0e-5f);
            }
            // A note that ended under PM leaves nothing running: with XIN off afterwards no voice
            // still asks for the input (the engine's per-sample input work stops).
            {
                WRig r;
                pmSound(r, 0.8f, 0.8f);
                r.set(W::AmpRelease, 0.01f);
                r.base().noteOn(69, 1.0f);
                r.render(at(0.1));
                r.base().noteOff(69);
                r.render(at(0.5));
                expect(! r.engine.getVoice(0).isActive(), "the note has ended");
                r.set(X::Mode, 0.0f);
                r.render(BLOCK * 4);
                // And the next note is the one of an engine that never had the input.
                WRig plain;
                pmSound(plain, 0.8f, 0.8f);
                plain.set(W::AmpRelease, 0.01f);
                plain.set(X::Mode, 0.0f);
                plain.render(at(0.6) + BLOCK * 4);
                r.base().noteOn(64, 0.7f);
                plain.base().noteOn(64, 0.7f);
                expect(same(r.render(at(0.1)), plain.render(at(0.1))), "the next note is as if XIN had never been on");
            }
            // XLP and XHP anywhere in their ranges at the lowest rate (the cutoff above half the rate).
            for (float hz : { 5000.0f, 11000.0f, 19999.0f })
            {
                WRig r(22050.0, 64);
                pmSound(r, 1.0f, 1.0f);
                r.set(X::LowPass, hz);
                r.set(X::HighPass, std::min(hz, 5000.0f));
                r.left = r.right = [](long long n) { return (n & 1) ? 1.0f : -1.0f; };
                r.base().noteOn(100, 1.0f);
                expect(finiteAndUnder(r.render(4096), 2.0f), "XLP " + juce::String(hz) + " Hz at 22.05 kHz");
            }
        }

        beginTest("Review: through the AudioEngine's own routing - another track on SRC phase-modulates this one; XIN OFF is the track without a source");
        {
            // Track 1 a macro oscillator (its fader shut: only heard as track 2's input), track 2
            // the wavetable engine on a sine. what: 0 = SRC 1 and PM, 1 = SRC 1 and XIN OFF, 2 = no SRC and PM.
            auto run = [](int what)
            {
                SequencerCore seq;
                AudioEngine audio;
                seq.prepareToPlay(SR, BLOCK);
                audio.setSequencer(&seq);
                audio.prepare(SR, BLOCK);
                auto source = AudioEngine::createEngine(EngineType::MacroOsc);
                auto wave = AudioEngine::createEngine(EngineType::Wavetable);
                audio.addEngine(0, source);
                audio.addEngine(1, wave);
                audio.setTrackParameter(0, MixParam::VOLUME, 0.0f);
                audio.setTrackParameter(1, W::TableSelect, (float) SINE_TABLE);
                audio.setTrackParameter(1, W::AmpSustain, 1.0f);
                // (The XIN page's first route: the input on PM at 60, or on nothing.)
                audio.setTrackParameter(1, mod::Router::xinId(mod::Router::XinAmount1), 0.6f);
                audio.setTrackParameter(1, mod::Router::xinId(mod::Router::XinDest1), what == 1 ? 0.0f : (float) W::dests().phase);
                audio.setTrackParameter(1, TrackInParam::SRC, what == 2 ? 0.0f : 1.0f);
                source->noteOn(52, 1.0f);
                wave->noteOn(69, 1.0f);
                std::vector<float> out, l((size_t) BLOCK), r((size_t) BLOCK);
                for (int b = 0; b < at(0.4) / BLOCK; ++b)
                {
                    audio.renderBlock(l.data(), r.data(), BLOCK);
                    out.insert(out.end(), l.begin(), l.end());
                }
                return out;
            };
            const auto modulated = run(0), off = run(1), unconnected = run(2);
            expect(rms(off, at(0.1), at(0.4)) > 0.01, "the track sounds");
            expect(same(off, unconnected), "XIN OFF with a source, PM without one: the same samples");
            double diff = 0.0;
            for (size_t i = (size_t) at(0.1); i < off.size(); ++i)
                diff += (double) (modulated[i] - off[i]) * (modulated[i] - off[i]);
            const double changed = std::sqrt(diff / (double) (off.size() - (size_t) at(0.1))) / rms(off, at(0.1), at(0.4));
            logMessage("through the AudioEngine: PM changes the track by " + juce::String(changed, 3) + " of its level");
            expect(changed > 0.3, "the other track is heard in this one's phase");
            expect(finiteAndUnder(modulated, 2.0f));
            expectWithinAbsoluteError(rms(modulated, at(0.1), at(0.4)) / rms(off, at(0.1), at(0.4)), 1.0, 0.1, "phase modulation: the level stays");
        }
    }

    // =============================================================================================
    void bowTests()
    {
        beginTest("Bow: without the route, or from the keys, the bowed string is the same samples");
        {
            auto play = [](MRig& r, float pressure)
            {
                return r.render(at(0.6), [&r, pressure](int b)
                {
                    if (b == 0) { r.base().noteOn(BOW_NOTE, 0.8f); r.base().noteOn(55, 0.7f); }
                    if (b == 30 && pressure >= 0.0f) r.engine.aftertouch(BOW_NOTE, pressure);
                    if (b == 60 && pressure >= 0.0f) r.engine.aftertouch(BOW_NOTE, 0.0f);
                    if (b == 300) r.base().noteOff(55);
                });
            };
            MRig plain;
            bowSound(plain, false);
            const auto reference = play(plain, -1.0f);
            expect(rms(reference, at(0.3), at(0.6)) > 0.02, "it sounds");

            MRig keys;                      // the route is there, no pressure ever comes
            bowSound(keys, true);
            expect(same(play(keys, -1.0f), reference), "a BOW route, played from the keys");

            MRig noRoute;                   // pressure comes, no route takes it
            bowSound(noRoute, false);
            expect(same(play(noRoute, 0.3f), reference), "pressure without a BOW route");

            MRig zeroDepth;                 // a route of depth 0 is no route
            bowSound(zeroDepth, false);
            zeroDepth.router.setRoute(0, { AftertouchRouter::Destination::Excite, 0.0f, false });
            expect(same(play(zeroDepth, 0.3f), reference), "a BOW route of depth 0");

            MRig routed;                    // and with both it is another sound
            bowSound(routed, true);
            expect(! same(play(routed, 0.3f), reference), "the route and pressure: heard");

            // Full pressure is the bow as the cells set it: the same samples as the keys.
            MRig full;
            bowSound(full, true);
            full.base().noteOn(BOW_NOTE, 0.8f);
            MRig keyed;
            bowSound(keyed, false);
            keyed.base().noteOn(BOW_NOTE, 0.8f);
            expect(same(full.render(at(0.5), [&full](int b) { if (b == 5) full.engine.aftertouch(BOW_NOTE, 1.0f); }), keyed.render(at(0.5))),
                   "pressure 1 is the model's own bow");

            // The other models take nothing from the route.
            for (int model = 0; model < (int) MacroOscVoice::Model::BowedString; ++model)
            {
                MRig a, b;
                for (auto* r : { &a, &b })
                {
                    r->set(M::ModelParam, (float) model);
                    r->base().noteOn(BOW_NOTE, 0.8f);
                }
                b.router.setRoute(0, { AftertouchRouter::Destination::Excite, 1.0f, false });
                expect(same(a.render(at(0.2)), b.render(at(0.2), [&b](int n) { if (n == 10) b.engine.aftertouch(BOW_NOTE, 0.4f); })),
                       "model " + juce::String(model));
            }
        }

        beginTest("Bow: more pressure is louder and brighter");
        {
            double lastLevel = 0.0, lastBright = 0.0, firstLevel = 0.0, firstBright = 0.0;
            for (float pressure : { 0.15f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const auto y = bowAt(pressure);
                const double level = rms(y, at(0.8), at(1.4)), bright = brightness(y, at(0.8), at(1.4));
                logMessage("bow at pressure " + juce::String(pressure, 2) + ": level " + juce::String(level, 4) + ", brightness " + juce::String(bright, 4));
                expect(level > lastLevel * 1.1, "louder at " + juce::String(pressure));
                expect(bright > lastBright, "brighter at " + juce::String(pressure));
                if (firstLevel == 0.0) { firstLevel = level; firstBright = bright; }
                lastLevel = level;
                lastBright = bright;
            }
            expect(lastLevel > 6.0 * firstLevel, "from 0.15 to 1: more than 15 dB");
            expect(lastBright > 1.3 * firstBright, "and a third brighter");
            expect(finiteAndUnder(bowAt(1.0f), 2.0f));

            // The route's depth scales it: depth 0.5 at full pressure is pressure 0.5.
            MRig half;
            bowSound(half);
            half.router.setRoute(0, { AftertouchRouter::Destination::Excite, 0.5f, false });
            half.base().noteOn(BOW_NOTE, 0.8f);
            expect(same(half.render(at(0.5), [&half](int b) { if (b == 1) half.engine.aftertouch(BOW_NOTE, 1.0f); }), [] {
                       auto y = bowAt(0.5f, 0.5);
                       return y; }()), "depth x pressure");
        }

        beginTest("Bow: pressure 0 lifts the bow and the string rings out; pressure again and it is bowed again");
        {
            MRig r;
            bowSound(r);
            r.base().noteOn(BOW_NOTE, 0.8f);
            const int lift = at(0.6) / BLOCK, back = at(2.0) / BLOCK;
            const auto y = r.render(at(2.8), [&](int b)
            {
                if (b == 1) r.engine.aftertouch(BOW_NOTE, 0.8f);
                if (b == lift) r.engine.aftertouch(BOW_NOTE, 0.0f);
                if (b == back) r.engine.aftertouch(BOW_NOTE, 0.8f);
            });
            const double bowed = rms(y, at(0.5), at(0.6));
            const double soon = rms(y, at(0.65), at(0.7)), later = rms(y, at(0.9), at(1.0)), late = rms(y, at(1.8), at(2.0));
            const double again = rms(y, at(2.6), at(2.8));
            logMessage("bow lifted: " + juce::String(bowed, 4) + " bowed, " + juce::String(soon, 4) + " 50-100 ms after, " + juce::String(later, 4)
                       + " 300-400 ms after, " + juce::String(late, 6) + " 1.2 s after; bowed again " + juce::String(again, 4));
            expect(bowed > 0.05);
            expect(soon > 0.2 * bowed, "it rings on: not cut");
            expect(soon < 0.9 * bowed && later < 0.5 * soon, "and dies away");
            expect(late < 0.01 * bowed, "to nothing");
            expectWithinAbsoluteError(again / bowed, 1.0, 0.15, "pressure again: bowed as before");
            expect(r.engine.getVoice(0).isActive(), "the note is still held all the while");
            expect(finiteAndUnder(y, 2.0f));

            // Against the keys: a held key goes on at its level over the same time.
            MRig keys;
            bowSound(keys);
            keys.base().noteOn(BOW_NOTE, 0.8f);
            const auto k = keys.render(at(2.0));
            expect(rms(k, at(1.8), at(2.0)) > 0.9 * rms(k, at(0.5), at(0.6)), "from the keys the bow never lifts");
        }

        beginTest("Bow: per voice, and no jump when pressure first arrives or the route goes");
        {
            MRig r;
            bowSound(r);
            r.base().noteOn(BOW_NOTE, 0.8f);
            r.base().noteOn(55, 0.8f);
            const auto y = r.render(at(0.5), [&](int b) { if (b == 100) r.engine.aftertouch(55, 0.3f); });
            int pressed = -1, other = -1;
            for (int v = 0; v < 8; ++v)
                if (r.engine.getVoice(v).isActive())
                    (std::abs(r.engine.getVoice(v).getBowEnergy() - 0.3f) < 0.01f ? pressed : other) = v;
            expect(pressed >= 0 && other >= 0, "one voice follows its pad");
            if (other >= 0)
                expectEquals(r.engine.getVoice(other).getBowEnergy(), 1.0f, "the other keeps its own bow");
            // The energy is moved to (15 ms), so the sound takes no step when pressure arrives ...
            const float scale = maxStep(y, at(0.05), 100 * BLOCK);
            expect(maxStep(y, 100 * BLOCK, 100 * BLOCK + at(0.03)) < 1.3f * scale, "pressure arrives");
            // ... nor when the route is taken away under the note (the bow goes back to its own,
            // and the string swings up to its full level again within a few periods).
            r.router.clear();
            const auto z = r.render(at(0.3));
            logMessage("bow: largest step " + juce::String(scale, 4) + " before, " + juce::String(maxStep(z, 0, at(0.03)), 4) + " as the route goes");
            expect(maxStep(z, 0, at(0.03)) < 2.0f * scale, "the route goes");
            if (pressed >= 0)
                expectEquals(r.engine.getVoice(pressed).getBowEnergy(), 1.0f, "back at the model's own bow");
            // A new note on the same pad starts from the model's own bow again.
            r.router.setRoute(0, { AftertouchRouter::Destination::Excite, 1.0f, false });
            r.base().noteOff(55);
            r.render(at(0.5));
            r.base().noteOn(55, 0.8f);
            r.render(at(0.2));
            for (int v = 0; v < 8; ++v)
                if (r.engine.getVoice(v).isActive() && v != other)
                    expectEquals(r.engine.getVoice(v).getBowEnergy(), 1.0f, "a new note is not held down by the last one's pressure");
        }

        beginTest("Bow: any pressure, rate, note and cell: finite and bounded");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            const float pressures[] = { nan, inf, -inf, 1.0e30f, -1.0f, 0.0f, 1.0f, 0.01f };
            int runs = 0;
            for (double sr : { 22050.0, 48000.0, 192000.0 })
                for (int block : { 1, 64, 4096 })
                    for (int note : { 0, 60, 127 })
                    {
                        MRig r(sr, block);
                        bowSound(r);
                        r.router.setRoute(1, { AftertouchRouter::Destination::Excite, (runs & 1) ? 1.0f : -1.0f, (runs & 2) != 0 });
                        r.router.setRoute(2, { AftertouchRouter::Destination::Vibrato, 1.0f, false });
                        for (int id : { (int) M::TimbreParam, (int) M::ColorParam, (int) M::HarmonicsParam, (int) M::MorphParam })
                            r.set(id, ((runs >> (id - 1)) & 1) ? 1.0f : 0.0f);
                        r.base().noteOn(note, 1.0f);
                        const int total = std::max(block * 12, (int) (0.08 * sr));
                        const auto y = r.render(total, [&](int b) { r.engine.aftertouch(note, pressures[(size_t) ((b * (block >= 64 ? 1 : 0) + b / 37 + runs) & 7)]); });
                        expect(finiteAndUnder(y, 8.0f), "rate " + juce::String(sr) + ", block " + juce::String(block) + ", note " + juce::String(note));
                        ++runs;
                    }
            expectEquals(runs, 27);
        }

        beginTest("Review: the bow's energy follows the pressure with a lag, it is not jumped to");
        {
            MRig r;
            bowSound(r);
            r.base().noteOn(BOW_NOTE, 0.8f);
            r.render(at(0.2));
            r.engine.aftertouch(BOW_NOTE, 0.3f);
            r.render(BLOCK);            // 1.3 ms of a 15 ms lag
            const float soon = r.engine.getVoice(0).getBowEnergy();
            expect(soon < 0.99f && soon > 0.85f, "one block after the pressure: " + juce::String(soon, 4));
            r.render(at(0.015) - BLOCK);
            const float oneLag = r.engine.getVoice(0).getBowEnergy();
            expectWithinAbsoluteError(oneLag, 0.3f + 0.7f * std::exp(-1.0f), 0.03f, "after 15 ms: 1 / e of the way left");
            r.render(at(0.2));
            expectWithinAbsoluteError(r.engine.getVoice(0).getBowEnergy(), 0.3f, 1.0e-3f);
            // The same lag at another rate.
            MRig fast(192000.0, 64);
            bowSound(fast);
            fast.base().noteOn(BOW_NOTE, 0.8f);
            fast.render(19200);
            fast.engine.aftertouch(BOW_NOTE, 0.3f);
            fast.render(2880);          // 15 ms
            expectWithinAbsoluteError(fast.engine.getVoice(0).getBowEnergy(), 0.3f + 0.7f * std::exp(-1.0f), 0.03f, "15 ms at 192 kHz");
        }
    }
};

static XinWavetableTests xinWavetableTests;
