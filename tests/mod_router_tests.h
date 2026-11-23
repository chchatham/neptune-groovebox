#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <chrono>
#include <cstdio>
#include <functional>
#include <set>
#include "../src/engine/MacroOscEngine.h"

// What the macro oscillator costs with its modulation idle and in use: NEPTUNE_ROUTER_BENCH=1
// NEPTUNE_TEST_ONLY="Mod Router" on a Release build of the test binary. Eight voices held, 512-sample
// blocks, 48 kHz; the figure is the share of real time on one core, the best of five runs.
// (Ids are written as numbers so that the same file measures the engine as it was before the
// router, in a tree of that commit: NEPTUNE_ROUTER_BENCH=old leaves out what did not exist.)
namespace ModRouterBench
{
    struct Case { const char* name; std::function<void(MacroOscEngine&)> setUp; bool input = false; float strip = 0.0f; bool pressure = false; };

    inline double measure(const Case& c, int model)
    {
        constexpr double SR = 48000.0;
        constexpr int BLOCK = 512, BLOCKS = 940;        // 10 s
        double best = 1.0e9;
        for (int run = 0; run < 5; ++run)
        {
            MacroOscEngine e(8);
            e.prepareToPlay(SR, BLOCK);
            e.setParameter(MacroOscEngine::ModelParam, (float) model);
            e.setParameter(MacroOscEngine::AmpSustain, 1.0f);
            c.setUp(e);
            e.setModWheel(c.strip);
            static const int notes[8] = { 48, 52, 55, 59, 62, 65, 69, 72 };
            for (int n : notes)
                e.noteOn(n, 0.8f, -1);
            if (c.pressure)
                for (int n : notes)
                    e.aftertouch(n, 0.6f);
            juce::AudioBuffer<float> buf(2, BLOCK);
            juce::MidiBuffer midi;
            std::vector<float> in((size_t) BLOCK);
            double phase = 0.0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < BLOCKS; ++b)
            {
                if (c.input)
                {
                    for (auto& s : in)
                    {
                        s = 0.7f * (float) std::sin(phase);
                        phase += 2.0 * 3.141592653589793 * 330.0 / SR;
                    }
                    AudioInput ai;
                    ai.left = ai.right = in.data();
                    ai.frames = BLOCK;
                    ai.envelope = 0.5f;
                    e.setAudioInput(ai);
                }
                buf.clear();
                e.processBlock(buf, midi);
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            best = std::min(best, 100.0 * seconds / (BLOCKS * BLOCK / SR));
        }
        return best;
    }

    inline std::vector<Case> cases()
    {
        using M = MacroOscEngine;
        const bool old = juce::SystemStats::getEnvironmentVariable("NEPTUNE_ROUTER_BENCH", {}) == "old";
        auto lfo = [](M& e, int i, float hz, float depth, int dest)
        {
            e.setParameter(158 + i * 8, hz);
            e.setParameter(159 + i * 8, depth);
            e.setParameter(165 + i * 8, (float) dest);
        };
        auto slot = [](M& e, int s, int src, int dest, float amt, int via = 0)
        {
            e.setParameter(700 + s * 4, (float) src);
            e.setParameter(701 + s * 4, (float) dest);
            e.setParameter(702 + s * 4, amt);
            e.setParameter(703 + s * 4, (float) via);
        };
        std::vector<Case> c;
        c.push_back({ "idle (nothing routed)", [](M&) {} });
        c.push_back({ "pads pressed, default route (VOL)", [](M&) {}, false, 0.0f, true });
        c.push_back({ "LFO1 > PTCH", [lfo](M& e) { lfo(e, 0, 5.0f, 0.3f, 1); } });
        c.push_back({ "LFO1 > macro 1, LFO2 > CUT", [lfo](M& e) { lfo(e, 0, 5.0f, 0.3f, 6); lfo(e, 1, 0.7f, 0.4f, 4); e.setParameter(M::FilterBase + 2, 2000.0f); } });
        c.push_back({ "LFO1 > ATK (a page cell)", [lfo](M& e) { lfo(e, 0, 5.0f, 0.3f, 11); } });
        c.push_back({ "four slots (ENV, PRES, LFO via STRP, RAND)", [slot](M& e)
        {
            slot(e, 0, 3, 2, 0.5f);         // ENV > AMP
            slot(e, 1, 6, 4, 0.4f);         // PRES > CUT
            slot(e, 2, 1, 9, 0.6f, 7);      // LFO1 > macro 4 via STRP
            slot(e, 3, 9, 3, 0.8f);         // RAND > PAN
            e.setParameter(M::FilterBase + 2, 2000.0f);
        }, false, 0.8f, true });
        c.push_back({ "strip up, default routes", [](M&) {}, false, 1.0f });
        c.push_back({ "input connected, nothing routed", [](M&) {}, true });
        if (old)
            return c;
        // The XIN page: 182 DST1, 183 AMT1, 186 DST2, 187 AMT2; source 11 = XIN.
        c.push_back({ "XIN > PTCH (audio rate)", [](M& e) { e.setParameter(182, 1.0f); e.setParameter(183, 0.5f); }, true });
        c.push_back({ "XIN > PTCH, macro 1, AMP (audio rate)", [slot](M& e)
        {
            e.setParameter(182, 1.0f); e.setParameter(183, 0.5f);
            e.setParameter(186, 6.0f); e.setParameter(187, 0.5f);
            slot(e, 0, 11, 2, 0.7f);
        }, true });
        // What phase U4-0 added (places in the macro oscillator's source list: 20 ONST, 24 BND4,
        // 37 OSC, 18 RND3; destinations 27 ETIM; TRIG 5 = XKEY).
        c.push_back({ "OSC > CUT (the voice's own source, audio rate)", [slot](M& e) { slot(e, 0, 37, 4, 0.5f); e.setParameter(M::FilterBase + 2, 2000.0f); } });
        c.push_back({ "ONST > AMP (the input's onsets)", [slot](M& e) { slot(e, 0, 20, 2, 0.5f); }, true });
        c.push_back({ "BND4 > macro 1 (the input's bands)", [slot](M& e) { slot(e, 0, 24, 6, 0.5f); }, true });
        c.push_back({ "LFO1 at XKEY > PTCH (restarted by the input)", [lfo](M& e) { lfo(e, 0, 5.0f, 0.3f, 1); e.setParameter(161, 5.0f); }, true });
        c.push_back({ "RND3 > ETIM", [slot](M& e) { slot(e, 0, 18, 27, 0.5f); } });
        return c;
    }

    inline void run()
    {
        static const char* const models[] = { "VA", "FM", "FOLD", "FORM", "HARM", "PLCK", "DUST", "BOW" };
        std::printf("Macro oscillator, 8 voices, 512-sample blocks, 48 kHz: %% of one core (best of 5)\n");
        for (int model : { 0, 1, 4 })
            for (auto& c : cases())
                std::printf("  %-5s %-46s %6.3f %%\n", models[model], c.name, measure(c, model));
        std::printf("  (");
        for (int i = 0; i < 4; ++i)     // the first row again: how steady the machine was
            std::printf("VA idle again %6.3f %%%s", measure(cases()[0], 0), i < 3 ? ", " : ")\n");
        std::fflush(stdout);
    }
}

// Every malloc / free of this thread while `counting` (macOS: libmalloc calls malloc_logger).
extern "C" { extern void (*malloc_logger)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t); }
namespace ModRouterAllocations
{
    inline thread_local bool counting = false;
    inline thread_local int events = 0;
    inline void count(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t) { if (counting) ++events; }
}

// -----------------------------------------------------------------------------
// "Mod Router": the modulation router (src/engine/ModRouter.h) by itself, on a small table of its
// own, and in the macro oscillator, the first engine on it.
//
// By itself: the table; nothing routed costs a compare; the scale of each kind of destination;
// every source; VIA; a slot on another slot's amount and on an LFO's rate and depth; ramps and
// leads; a slot taken away is carried off; the track input per sample; pressure per voice; the
// LFOs of all voices in step; the pressure routes as slots; extremes.
// In the macro oscillator: the XIN page; the input at audio rate on the pitch (FM sidebands where
// and as strong as the law says), on the level (amplitude and ring modulation), on a macro; the
// input's filters; a pressure route per voice; at rest the same samples as with nothing connected;
// every rate and block size with every slot in use and an input that is not a number.
// Levels of tones are read with a Hann-windowed correlation (steady, unmodulated partials).
// -----------------------------------------------------------------------------
class ModRouterTests : public juce::UnitTest
{
public:
    ModRouterTests() : UnitTest("Mod Router") {}

    using R = mod::Router;
    using M = MacroOscEngine;
    using V = MacroOscVoice;
    enum { PTCH = 1, AMP, CUT, RESO, LIN, LOG, LRAT, LDPT, VISIBLE, HID = VISIBLE, L1RT = VISIBLE, L1DP, L2RT, L2DP, AMT1 };
    static constexpr int LIN_ID = 10, LOG_ID = 11;
    static constexpr double SR = 48000.0;

    // A small table: every kind of destination once.
    static const mod::DestTable& table()
    {
        static const mod::DestTable t = []
        {
            mod::DestTable d;
            d.group();
            d.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);
            d.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample).audioFloor(-4.0f);
            d.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);
            d.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
            d.group();
            d.add(ParamSpec { "LIN", LIN_ID, 0.0f, 10.0f, 5.0f, ParamKind::Linear }, nullptr, mod::PerSample);
            d.add(ParamSpec { "LOG", LOG_ID, 20.0f, 20000.0f, 1000.0f, ParamKind::Log, "Hz" });
            d.addCross();
            d.hidden("HID", 0.0f, 1.0f, 0.0f, 1.0f);
            d.stripRoute(0, "CUT", 0.5f);
            d.stripScale(CUT, 2.0f);
            d.stripVibrato(0.25f);
            d.finish(2);
            return d;
        }();
        return t;
    }

    struct Rig
    {
        R r { table(), 2 };
        std::array<mod::Voice, 2> v;
        double sr;
        float strip = 0.0f, follower = 0.0f;
        std::array<float, 2> env { { 1.0f, 1.0f } }, fenv { { 0.0f, 0.0f } }, pressure { { 0.0f, 0.0f } };
        std::vector<float> input;       // one sub-block of input, the same every time (empty: none)
        int n = 16;

        explicit Rig(double sampleRate = SR) : sr(sampleRate)
        {
            r.prepare(sr);
            for (auto& voice : v)
                voice.prepare(sr);
            // (The table's strip defaults are for the tests that want them.)
            r.set(R::stripAmountId(0), 0.0f);
            r.set(R::STRIP_VIBRATO, 0.0f);
        }
        void slot(int k, int src, int dest, float amount, int via = 0)
        {
            r.set(ModSlotParam::id(k, ModSlotParam::SRC), (float) src);
            r.set(ModSlotParam::id(k, ModSlotParam::DEST), (float) dest);
            r.set(ModSlotParam::id(k, ModSlotParam::AMT), amount);
            r.set(ModSlotParam::id(k, ModSlotParam::VIA), (float) via);
        }
        void lfo(int i, int dest, float depth, int shape = 2, int trigger = 1, float hz = 0.02f)
        {
            r.set(R::lfoId(i, R::Rate), hz);
            r.set(R::lfoId(i, R::Shape), (float) shape);
            r.set(R::lfoId(i, R::Trigger), (float) trigger);
            r.set(R::lfoId(i, R::Depth), depth);
            r.set(R::lfoId(i, R::DestCell), (float) dest);
        }
        void note(int which = 0, int note = 60, float velocity = 1.0f) { v[(size_t) which].noteOn(r, note, velocity); }
        // The next sub-block for the router, then for one voice.
        void chunk()
        {
            const int frames = input.empty() ? 16 : (int) input.size();
            r.beginBlock(input.empty() ? nullptr : input.data(), input.empty() ? nullptr : input.data(), input.empty() ? 0 : frames, follower, strip, sr);
            n = r.beginChunk(0, frames);
        }
        bool run(int which = 0) { return v[(size_t) which].tick(r, n, env[(size_t) which], fenv[(size_t) which], pressure[(size_t) which]); }
        bool tick(int which = 0) { chunk(); return run(which); }
        void settle(int which = 0, int ticks = 80) { for (int i = 0; i < ticks; ++i) tick(which); }
        // A per-sample destination at the end of the last sub-block (its set value when nothing moves it).
        float at(int dest, int which = 0) const
        {
            const float* b = v[(size_t) which].buffer(dest);
            return b != nullptr ? b[n - 1] : r.baseOf(dest);
        }
        // A control-rate destination as the voice was last told (-1000: not told this sub-block).
        float told(int dest, int which = 0) const
        {
            float out = -1000.0f;
            v[(size_t) which].forEachControl([&](int d, float value) { if (d == dest) out = value; });
            return out;
        }
    };

    // ---- measuring ---------------------------------------------------------------------------
    // The amplitude of the partial at `hz` in y[from, to): a Hann-windowed correlation.
    static double partial(const std::vector<float>& y, int from, int to, double hz, double sr = SR)
    {
        double re = 0.0, im = 0.0, sum = 0.0;
        const int len = to - from;
        for (int i = 0; i < len; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * (double) i / (double) (len - 1));
            const double ph = 2.0 * juce::MathConstants<double>::pi * hz * (double) i / sr;
            re += w * y[(size_t) (from + i)] * std::cos(ph);
            im += w * y[(size_t) (from + i)] * std::sin(ph);
            sum += w;
        }
        return 2.0 * std::sqrt(re * re + im * im) / sum;
    }
    static double rms(const std::vector<float>& y, int from, int to)
    {
        double s = 0.0;
        for (int i = from; i < to; ++i)
            s += (double) y[(size_t) i] * y[(size_t) i];
        return std::sqrt(s / std::max(1, to - from));
    }

    // The macro oscillator as one sine (VA, one saw, HARM at 0: sines only), held at full level.
    static std::unique_ptr<M> sine(double sr = SR, int block = 128)
    {
        auto e = std::make_unique<M>(8);
        e->prepareToPlay(sr, block);
        e->setParameter(M::TimbreParam, 0.0f);
        e->setParameter(M::HarmonicsParam, 0.0f);
        e->setParameter(M::AmpAttack, 0.001f);
        e->setParameter(M::AmpSustain, 1.0f);
        return e;
    }
    // Left channel of `seconds`; input(i): the track input at sample i (nullptr: not connected).
    static std::vector<float> render(M& e, double seconds, const std::function<float(int)>& input = nullptr,
                                     double sr = SR, int block = 128, const std::function<void(int)>& perBlock = nullptr)
    {
        std::vector<float> out, in((size_t) block);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        const int total = (int) (seconds * sr);
        for (int done = 0, b = 0; done < total; done += block, ++b)
        {
            if (perBlock)
                perBlock(b);
            if (input)
            {
                for (int i = 0; i < block; ++i)
                    in[(size_t) i] = input(done + i);
                e.setAudioInput({ in.data(), in.data(), block, 0.5f });
            }
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
                out.push_back(buf.getSample(0, i));
        }
        return out;
    }
    static std::function<float(int)> tone(double hz, float level, double sr = SR)
    {
        return [=](int i) { return level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * hz * (double) i / sr); };
    }
    static void freeSlot(EngineBase& e, int k, int src, int dest, float amount, int via = 0)
    {
        e.setParameter(ModSlotParam::id(k, ModSlotParam::SRC), (float) src);
        e.setParameter(ModSlotParam::id(k, ModSlotParam::DEST), (float) dest);
        e.setParameter(ModSlotParam::id(k, ModSlotParam::AMT), amount);
        e.setParameter(ModSlotParam::id(k, ModSlotParam::VIA), (float) via);
    }
    static double besselJ(int order, double x)
    {
        double sum = 0.0, term = std::pow(x / 2.0, order);
        for (int k = 1; k <= order; ++k)
            term /= (double) k;
        for (int k = 0; k < 20; ++k)
        {
            sum += term;
            term *= -(x * x / 4.0) / ((double) (k + 1) * (double) (k + 1 + order));
        }
        return sum;
    }
    static double besselI(int order, double x)
    {
        double sum = 0.0, term = std::pow(x / 2.0, order);
        for (int k = 1; k <= order; ++k)
            term /= (double) k;
        for (int k = 0; k < 20; ++k)
        {
            sum += term;
            term *= (x * x / 4.0) / ((double) (k + 1) * (double) (k + 1 + order));
        }
        return sum;
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_ROUTER_BENCH", {}).isNotEmpty())
        {
            beginTest("Bench");
            ModRouterBench::run();
            expect(true);
            return;
        }
        routerTests();
        macroTests();
    }

    // =============================================================================================
    void routerTests()
    {
        const float near = 1.0e-4f;

        beginTest("The table: OFF first, the visible entries, what a free slot adds, the hidden one, ids");
        {
            const auto& t = table();
            expectEquals(juce::String(t[0].label), juce::String("OFF"));
            expectEquals(t.visible(), (int) VISIBLE);
            expectEquals(t.size(), VISIBLE + 1, "one hidden entry");
            expectEquals(t.slotChoices(), VISIBLE + 4 + 16 + mod::TrackDest::COUNT, "L1RT L1DP L2RT L2DP, AMT1 .. AM16 and the track's effect cells (E4)");
            expectEquals(t.trackSlotChoice(), VISIBLE + 4 + 16);
            expectEquals(t.cellChoices(), VISIBLE + mod::TrackDest::COUNT, "an LFO's DEST: the visible entries and the track's effect cells");
            expectEquals(t.trackCell(), (int) VISIBLE);
            expectEquals(juce::String(t.slotLabels()[L1RT]), juce::String("L1RT"));
            expectEquals(juce::String(t.slotLabels()[L2DP]), juce::String("L2DP"));
            expectEquals(juce::String(t.slotLabels()[AMT1]), juce::String("AMT1"));
            expectEquals(juce::String(t.slotLabels()[t.trackSlotChoice() - 1]), juce::String("AM16"));
            expectEquals(juce::String(t.slotLabels()[t.slotChoices() - 1]), juce::String("RO.D"));
            for (int i = 0; i < VISIBLE; ++i)
                expectEquals(juce::String(t.slotLabels()[i]), juce::String(t.labels()[i]), "a free slot's list begins with the table");
            expectEquals(t.indexOfLabel("HID"), (int) HID);
            expectEquals(t.indexOfId(LIN_ID), (int) LIN);
            expectEquals(t.indexOfId(LOG_ID), (int) LOG);
            expectEquals(t.indexOfId(99), -1);
            expectEquals(t.pitch(), (int) PTCH);
            expectEquals(t.buffers(), 4, "PTCH, AMP, CUT and LIN are per sample");
            expect(t[LOG].exp && ! t[LIN].exp, "a logarithmic cell moves in octaves");
            expectWithinAbsoluteError(t[LOG].span, 0.5f * std::log2(1000.0f), near);
            expectEquals(t[LIN].span, 10.0f);
            expect(ChoiceGroups::find(M::lfoDests(0).labels()) != nullptr && ChoiceGroups::find(M::lfoDests(0).slotLabels()) != nullptr,
                   "Func + turn knows both lists of a registered table");
            // A hidden entry is not a choice of any cell; a free slot's index past the table is no entry of it.
            Rig g;
            g.r.set(R::lfoId(0, R::DestCell), 99.0f);
            expectEquals(g.r.get(R::lfoId(0, R::DestCell)), (float) (t.cellChoices() - 1));
            g.r.set(ModSlotParam::id(0, ModSlotParam::DEST), 999.0f);
            expectEquals(g.r.get(ModSlotParam::id(0, ModSlotParam::DEST)), (float) (t.slotChoices() - 1));
        }

        beginTest("Nothing routed: tick() says so and no destination has a buffer; the strip's and the pads' routes cost nothing at rest");
        {
            Rig g;
            g.note();
            expect(! g.tick(), "an empty router");
            expect(g.r.idle());
            for (int d = 1; d <= HID; ++d)
                expect(g.v[0].buffer(d) == nullptr && ! g.v[0].moved(d));

            // A strip route with the strip down, a pressure route with the pad up: still nothing.
            g.r.set(R::stripAmountId(0), 0.5f);
            g.r.set(R::STRIP_VIBRATO, 0.25f);
            // Pressure slot 0: destination AMP, amount 1.0 (the old default for Volume).
            g.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) AMP);
            g.r.set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 1.0f);
            g.note();
            for (int i = 0; i < 5; ++i)
                expect(! g.tick(), "strip down, pad up: nothing to do");
            expect(! g.r.idle(), "the routes are there");
            expect(g.v[0].buffer(AMP) == nullptr && g.v[0].buffer(CUT) == nullptr);
            // Slots that are set but off: SRC OFF, AMT 0, DEST OFF.
            g.slot(0, mod::Src::Off, AMP, 1.0f);
            g.slot(1, mod::Src::Velocity, AMP, 0.0f);
            g.slot(2, mod::Src::Velocity, 0, 1.0f);
            g.lfo(0, AMP, 0.0f);
            g.lfo(1, 0, 1.0f);
            g.tick();
            expect(! g.tick(), "slots that are off are not in use");
            // And in use: the pad pressed, the strip up.
            g.pressure[0] = 0.5f;
            expect(g.tick() && g.v[0].buffer(AMP) != nullptr && g.v[0].buffer(CUT) == nullptr, "the pad: its route only");
            g.strip = 1.0f;
            expect(g.tick() && g.v[0].buffer(CUT) != nullptr && g.v[0].buffer(PTCH) != nullptr, "the strip: its route and the vibrato");
        }

        beginTest("Scale: pitch by the amount squared, level 1 + sum, cutoff x 4 octaves, a linear cell by its range, a logarithmic one by half of it");
        {
            // VEL at 1 is a source of exactly 1; a note-on takes everything as it stands.
            auto value = [](int dest, float amount)
            {
                Rig g;
                g.slot(0, mod::Src::Velocity, dest, amount);
                g.note();
                g.tick();
                return std::pair<float, float> { g.at(dest), g.v[0].buffer(dest) != nullptr ? g.v[0].buffer(dest)[0] : -1.0f };
            };
            expectWithinAbsoluteError(value(PTCH, 1.0f).first, 2.0f, near, "full amount: an octave (12 semitones)");
            expectWithinAbsoluteError(value(PTCH, 0.5f).first, std::exp2(0.25f), near, "50 %: 3 semitones");
            expectWithinAbsoluteError(value(PTCH, 0.1f).first, std::exp2(0.01f), near, "10 %: 12 cents");
            expectWithinAbsoluteError(value(PTCH, -0.5f).first, std::exp2(-0.25f), near, "and down");
            expectWithinAbsoluteError(value(AMP, 0.5f).first, 1.5f, near);
            expectWithinAbsoluteError(value(AMP, -1.0f).first, 0.0f, near);
            expectWithinAbsoluteError(value(CUT, 0.5f).first, 2.0f, near, "octaves");
            expectWithinAbsoluteError(value(CUT, -1.0f).first, -4.0f, near);
            expectWithinAbsoluteError(value(LIN, 0.3f).first, 8.0f, near, "5 + 0.3 x the range of 10");
            expectWithinAbsoluteError(value(LIN, 1.0f).first, 10.0f, near, "held to the cell's range");
            expectWithinAbsoluteError(value(LIN, -1.0f).first, 0.0f, near);
            expectWithinAbsoluteError(value(AMP, 0.5f).second, 1.5f, near, "the first sample of a note has it already");

            // Control rate: handed over by forEachControl, in the cell's own scale; no buffer.
            Rig g;
            g.slot(0, mod::Src::Velocity, RESO, 0.6f);
            g.slot(1, mod::Src::Velocity, LOG, 0.2f);
            g.note();
            expect(g.tick());
            expectWithinAbsoluteError(g.told(RESO), 0.3f, near);
            expectWithinAbsoluteError(g.told(LOG), 1000.0f * std::exp2(0.2f * 0.5f * std::log2(1000.0f)), 0.5f, "a fifth of half the range, in octaves");
            expect(g.v[0].buffer(RESO) == nullptr && g.v[0].buffer(LOG) == nullptr);
            expectWithinAbsoluteError(g.v[0].value(RESO), 0.3f, near);
            g.slot(1, mod::Src::Velocity, LOG, 1.0f);
            g.settle();
            expectEquals(g.told(LOG), 20000.0f, "held to the range");

            // A routed cell follows its knob: the sum moves around the new set value.
            Rig k;
            k.slot(0, mod::Src::Velocity, LIN, 0.3f);
            k.note();
            k.tick();
            k.r.setBase(LIN_ID, 2.0f);
            k.tick();
            expectWithinAbsoluteError(k.at(LIN), 5.0f, near, "2 + 3");
            // From the strip's own routes a destination may count more (here the cutoff, twice);
            // a free slot that reads the strip is in the plain scale.
            Rig own;
            own.r.set(R::stripAmountId(0), 0.5f);
            own.strip = 1.0f;
            own.note();
            own.tick();
            expectWithinAbsoluteError(own.at(CUT), 0.5f * 2.0f * 4.0f, near, "a STRIP route");
            Rig plain;
            plain.slot(0, mod::Src::Strip, CUT, 0.5f);
            plain.strip = 1.0f;
            plain.note();
            plain.tick();
            expectWithinAbsoluteError(plain.at(CUT), 0.5f * 4.0f, near, "STRP in a free slot");

            // Sums: two slots add; sixteen on the level are held at 4.
            Rig s;
            for (int i = 0; i < 16; ++i)
                s.slot(i, mod::Src::Velocity, AMP, i < 2 ? 0.25f : 0.0f);
            s.note();
            s.tick();
            expectWithinAbsoluteError(s.at(AMP), 1.5f, near);
            for (int i = 0; i < 16; ++i)
                s.slot(i, mod::Src::Velocity, AMP, 1.0f);
            s.settle();
            expectEquals(s.at(AMP), 4.0f);
        }

        beginTest("Every source reaches a destination: the level is 1 + amount x source");
        {
            struct Case { int src; float expected; const char* name; };
            // The rig: LFOs as a square that has just begun (+1), envelopes 0.6 / 0.3, velocity 0.8,
            // the pad at 0.5, the strip at 0.7, note 90 (KEY = 0.5), the follower at 0.4, the input a
            // constant 0.5 (the low-pass lets it through, the high-pass takes it away).
            const Case cases[] = {
                { mod::Src::Lfo1, 1.0f, "LFO1" }, { mod::Src::Lfo2, 1.0f, "LFO2" }, { mod::Src::Env, 0.6f, "ENV" },
                { mod::Src::FilterEnv, 0.3f, "FENV" }, { mod::Src::Velocity, 0.8f, "VEL" }, { mod::Src::Pressure, 0.5f, "PRES" },
                { mod::Src::Strip, 0.7f, "STRP" }, { mod::Src::Key, 0.5f, "KEY" }, { mod::Src::Input, 0.4f, "IN" },
                { mod::Src::Xin, 0.5f, "XIN" }, { mod::Src::XinLow, 0.5f, "XLP" }, { mod::Src::XinHigh, 0.0f, "XHP" },
                { mod::Src::XinBand, 0.0f, "XBP" }, { mod::Src::One, 1.0f, "ONE" } };
            for (auto& c : cases)
                for (int dest : { (int) AMP, (int) RESO })      // per sample, and at control rate
                {
                    Rig g;
                    g.lfo(0, 0, 0.0f);
                    g.lfo(1, 0, 0.0f);
                    g.env[0] = 0.6f;
                    g.fenv[0] = 0.3f;
                    g.pressure[0] = 0.5f;
                    g.strip = 0.7f;
                    g.follower = 0.4f;
                    g.input.assign(16, 0.5f);
                    g.slot(3, c.src, dest, 0.5f);
                    g.note(0, 90, 0.8f);
                    g.settle();
                    const float got = dest == AMP ? g.at(AMP) - 1.0f : g.v[0].value(RESO) / 0.5f;
                    expectWithinAbsoluteError(got, 0.5f * c.expected, 2.0e-3f, juce::String(c.name) + (dest == AMP ? " > AMP" : " > RESO"));
                    expectEquals(juce::String(mod::Src::labels()[c.src]), juce::String(c.name));
                }

            // RAND: one value a note, -1..1, another on the next note and on another voice.
            Rig g;
            g.slot(0, mod::Src::Random, AMP, 0.5f);
            std::set<int> seen;
            for (int note = 0; note < 12; ++note)
            {
                g.note(note % 2, 60 + note);
                g.tick(note % 2);
                const float random = g.v[(size_t) (note % 2)].source(mod::Src::Random);
                expect(random >= -1.0f && random <= 1.0f);
                expectWithinAbsoluteError(g.at(AMP, note % 2), 1.0f + 0.5f * random, near);
                g.tick(note % 2);
                expectWithinAbsoluteError(g.at(AMP, note % 2), 1.0f + 0.5f * random, near, "held for the note");
                seen.insert((int) std::lround(random * 1000.0f));
            }
            expect(seen.size() >= 10, "a new value for every note");

            // VIB: the engine's vibrato sine at VRAT, the same in every voice.
            Rig vib;
            vib.r.set(R::STRIP_VIBRATO_RATE, 6.0f);
            vib.slot(0, mod::Src::Vib, AMP, 0.5f);
            vib.note(0);
            vib.note(1);
            float low = 9.0f, high = -9.0f;
            int crossings = 0;
            float last = 0.0f;
            for (int i = 0; i < 3000; ++i)      // 1 s
            {
                vib.chunk();
                vib.run(0);
                const float a = vib.at(AMP, 0) - 1.0f;
                vib.run(1);
                expectWithinAbsoluteError(vib.at(AMP, 1) - 1.0f, a, near, "one phase for all voices");
                low = std::min(low, a);
                high = std::max(high, a);
                if (last <= 0.0f && a > 0.0f)
                    ++crossings;
                last = a;
            }
            expectWithinAbsoluteError(high, 0.5f, 5.0e-3f);
            expectWithinAbsoluteError(low, -0.5f, 5.0e-3f);
            expect(crossings >= 5 && crossings <= 7, "6 Hz: " + juce::String(crossings) + " cycles in a second");

            // The strip's own vibrato (WVIB) is that sine on the pitch, in semitones at the top of the strip.
            Rig w;
            w.r.set(R::STRIP_VIBRATO, 1.0f);
            w.strip = 1.0f;
            w.note();
            float top = 0.0f, bottom = 9.0f;
            for (int i = 0; i < 3000; ++i)
            {
                w.tick();
                top = std::max(top, w.at(PTCH));
                bottom = std::min(bottom, w.at(PTCH));
            }
            expectWithinAbsoluteError(12.0f * std::log2(top), 1.0f, 0.01f, "one semitone up");
            expectWithinAbsoluteError(12.0f * std::log2(bottom), -1.0f, 0.01f, "and down");
        }

        beginTest("VIA scales the amount as 0..1: nothing at 0, all of it at 1; a source that swings both ways is moved up");
        {
            for (float strip : { 0.0f, 0.25f, 1.0f })
            {
                Rig g;
                g.strip = strip;
                g.slot(0, mod::Src::Velocity, AMP, 0.8f, mod::Src::Strip);
                g.note();
                g.settle();
                expectWithinAbsoluteError(g.at(AMP), 1.0f + 0.8f * strip, near, "via the strip at " + juce::String(strip));
            }
            for (int note : { 0, 60, 120 })     // KEY = -1, 0, 1  ->  0, 0.5, 1
            {
                Rig g;
                g.slot(0, mod::Src::Velocity, AMP, 0.8f, mod::Src::Key);
                g.note(0, note);
                g.tick();
                expectWithinAbsoluteError(g.at(AMP), 1.0f + 0.8f * (float) note / 120.0f, near, "via KEY at note " + juce::String(note));
            }
            // An LFO as VIA: a square at -1 closes the slot, at +1 opens it.
            Rig g;
            g.lfo(0, 0, 0.0f, 2, 1, 0.02f);
            g.slot(0, mod::Src::Velocity, AMP, 0.8f, mod::Src::Lfo1);
            g.note();
            g.tick();
            expectWithinAbsoluteError(g.at(AMP), 1.8f, near);
            // An LFO whose shape only goes one way (ExpUp: 0 at the start of its cycle) is taken as it is.
            Rig u;
            u.lfo(0, 0, 0.0f, (int) LFO::Shape::ExpUp, 1, 0.02f);
            u.slot(0, mod::Src::Velocity, AMP, 0.8f, mod::Src::Lfo1);
            u.note();
            u.tick();
            expect(LFO::isUnipolar(LFO::Shape::ExpUp));
            expectWithinAbsoluteError(u.at(AMP), 1.0f, 0.01f, "closed at the bottom of a shape that has no negative half");
        }

        beginTest("Modulation of modulation: a slot on another slot's amount, on one LFO's rate and depth, LRAT / LDPT on the others");
        {
            // AMT1: slot 2 adds strip x 0.5 to slot 1's amount (heard one sub-block later).
            Rig g;
            g.slot(0, mod::Src::Velocity, AMP, 0.2f);
            g.slot(1, mod::Src::Strip, AMT1, 0.5f);
            g.strip = 1.0f;
            g.note();
            g.tick();
            g.tick();
            expectWithinAbsoluteError(g.at(AMP), 1.7f, near, "0.2 + 0.5");
            g.slot(1, mod::Src::Strip, AMT1, 1.0f);
            g.settle();
            expectWithinAbsoluteError(g.at(AMP), 2.0f, near, "the amount is held to -1..1");
            // A slot whose amount is 0 but is moved by another is in use.
            Rig z;
            z.slot(0, mod::Src::Velocity, AMP, 0.0f);
            z.slot(1, mod::Src::Strip, AMT1, 0.5f);
            z.strip = 1.0f;
            z.note();
            z.tick();
            z.tick();
            expectWithinAbsoluteError(z.at(AMP), 1.5f, near);
            // Aimed at its own amount: nothing.
            Rig own;
            own.slot(0, mod::Src::Velocity, AMT1, 1.0f);
            own.note();
            expect(! own.tick() && own.r.idle(), "a slot on its own amount is not in use");

            // L1RT: +0.5 = an octave: LFO 1 runs twice as fast, LFO 2 as it was.
            auto phases = [](int dest, float amount, int lfoWithSlot)
            {
                Rig r;
                r.lfo(0, AMP, 0.1f, 3, 0, 5.0f);       // a saw, free, 5 Hz
                r.lfo(1, CUT, 0.1f, 3, 0, 5.0f);
                if (lfoWithSlot < 0)
                    r.slot(0, mod::Src::Velocity, dest, amount);
                else
                    r.r.set(R::lfoId(lfoWithSlot, R::DestCell), (float) dest), r.r.set(R::lfoId(lfoWithSlot, R::Depth), amount);
                r.note();
                for (int i = 0; i < 301; ++i)       // 0.1 s (and the sub-block the rate is late by)
                    r.tick();
                return std::pair<double, double> { r.v[0].getLfo(0).getPhase(), r.v[0].getLfo(1).getPhase() };
            };
            const auto plain = phases(0, 0.0f, -1);
            expectWithinAbsoluteError(plain.first, 0.5017, 2.0e-3, "5 Hz for 0.1 s");
            const auto faster = phases(L1RT, 0.5f, -1);
            expectWithinAbsoluteError(faster.first, 2.0 * plain.first - 1.0, 4.0e-3, "LFO 1 an octave faster");
            expectWithinAbsoluteError(faster.second, plain.second, 1.0e-6, "LFO 2 untouched");
            const auto both = phases(LRAT, 0.5f, -1);
            expectWithinAbsoluteError(both.first, 2.0 * plain.first - 1.0, 4.0e-3, "LRAT from a slot: every LFO");
            expectWithinAbsoluteError(both.second, 2.0 * plain.second - 1.0, 4.0e-3);

            // LRAT from LFO 1 itself (a square at +1, full depth): the other LFO four times as fast, not itself.
            Rig c;
            c.lfo(0, LRAT, 1.0f, 2, 1, 0.02f);
            c.lfo(1, CUT, 0.1f, 3, 0, 5.0f);
            c.note();
            for (int i = 0; i < 301; ++i)
                c.tick();
            expectWithinAbsoluteError(c.v[0].getLfo(1).getPhase(), 0.0067, 4.0e-3, "20 Hz for 0.1 s: two cycles");
            expect(c.v[0].getLfo(0).getPhase() < 0.01, "LFO 1 at its own 0.02 Hz");

            // L1DP: the depth of LFO 1's own slot x (1 + sum), 0..2; a free slot reading LFO 1 is not moved.
            Rig d;
            d.lfo(0, AMP, 0.4f, 2, 1, 0.02f);
            d.slot(0, mod::Src::Velocity, L1DP, 0.5f);
            d.slot(1, mod::Src::Lfo1, CUT, 0.5f);
            d.note();
            d.tick();
            d.tick();
            expectWithinAbsoluteError(d.at(AMP), 1.0f + 0.4f * 1.5f, near);
            expectWithinAbsoluteError(d.at(CUT), 2.0f, near, "the slot's own amount is its depth");
            d.slot(0, mod::Src::Velocity, L1DP, -1.0f);
            d.settle();
            expectWithinAbsoluteError(d.at(AMP), 1.0f, near, "depth x 0");
        }

        beginTest("Nothing is jumped to: ramps inside a sub-block, leads on the sources, pitch as it is");
        {
            // The strip is given once a block: 5 ms to cross its range, a straight line all the way.
            Rig g;
            g.slot(0, mod::Src::Strip, AMP, 1.0f);
            g.note();
            g.tick();
            g.strip = 1.0f;
            float last = 1.0f, largest = 0.0f;
            int samples = 0;
            for (int t = 0; t < 30 && last < 2.0f; ++t)
            {
                g.tick();
                const float* b = g.v[0].buffer(AMP);
                for (int i = 0; i < g.n; ++i)
                {
                    largest = std::max(largest, std::abs(b[i] - last));
                    last = b[i];
                    ++samples;
                }
            }
            expectEquals(last, 2.0f, "arrived exactly");
            expectWithinAbsoluteError((float) samples, 240.0f, 16.0f, "5 ms at 48 kHz");
            expectWithinAbsoluteError(largest, 1.0f / 240.0f, 1.0e-4f, "one even step a sample");

            // An LFO's edge: the level is led over 4 ms, the pitch takes it within the sub-block.
            Rig e;
            e.lfo(0, AMP, 1.0f, 2, 1, 50.0f);       // a square, 50 Hz... held to the cell's 40 Hz
            e.lfo(1, 0, 0.0f, 2, 1, 40.0f);
            e.slot(0, mod::Src::Lfo2, PTCH, 1.0f);
            e.note();
            float ampStep = 0.0f, pitchStep = 0.0f, lastAmp = 2.0f, lastPitch = 2.0f, lowest = 9.0f;
            for (int t = 0; t < 200; ++t)
            {
                e.tick();
                const float* a = e.v[0].buffer(AMP);
                const float* p = e.v[0].buffer(PTCH);
                for (int i = 0; i < e.n; ++i)
                {
                    ampStep = std::max(ampStep, std::abs(a[i] - lastAmp));
                    pitchStep = std::max(pitchStep, std::abs(p[i] - lastPitch));
                    lastAmp = a[i];
                    lastPitch = p[i];
                    lowest = std::min(lowest, a[i]);
                }
            }
            expectWithinAbsoluteError(ampStep, 2.0f / 192.0f, 1.0e-3f, "the whole swing in 4 ms, no faster");
            expect(lowest < 0.01f, "and it gets there");
            expect(pitchStep > 1.5f / 16.0f * 0.9f, "the pitch: from 2 to 0.5 inside one sub-block (" + juce::String(pitchStep, 4) + " a sample)");
        }

        beginTest("A slot taken away under a sounding voice is carried off its destination; a control-rate cell gets its set value back, once");
        {
            for (int how = 0; how < 3; ++how)       // DEST to OFF, AMT to 0, DEST elsewhere
            {
                Rig g;
                g.slot(0, mod::Src::Velocity, AMP, 0.8f);
                g.slot(1, mod::Src::Velocity, LOG, 0.2f);
                g.note();
                g.tick();
                expectWithinAbsoluteError(g.at(AMP), 1.8f, near);
                if (how == 0)       { g.slot(0, mod::Src::Velocity, 0, 0.8f); g.slot(1, mod::Src::Velocity, 0, 0.2f); }
                else if (how == 1)  { g.slot(0, mod::Src::Velocity, AMP, 0.0f); g.slot(1, mod::Src::Velocity, LOG, 0.0f); }
                else                { g.slot(0, mod::Src::Velocity, CUT, 0.8f); g.slot(1, mod::Src::Velocity, RESO, 0.2f); }
                float last = 1.8f, largest = 0.0f, lastLog = 0.0f;
                int samples = 0, toldBase = 0;
                bool gone = false;
                for (int t = 0; t < 40; ++t)
                {
                    g.tick();
                    const float* b = g.v[0].buffer(AMP);
                    if (b == nullptr)
                    {
                        gone = true;
                        if (g.told(LOG) == 1000.0f)
                            ++toldBase;
                        continue;
                    }
                    expect(! gone, "once let go it stays let go");
                    for (int i = 0; i < g.n; ++i)
                    {
                        largest = std::max(largest, std::abs(b[i] - last));
                        last = b[i];
                        ++samples;
                    }
                    const float log = g.told(LOG);
                    if (log == 1000.0f) ++toldBase;
                    else if (log > 0.0f) lastLog = log;
                }
                const juce::String name = how == 0 ? "DEST to OFF" : (how == 1 ? "AMT to 0" : "another DEST");
                expect(gone, name + ": the destination is free again");
                expectEquals(last, 1.0f, name + ": back at the set value");
                expect(largest < 0.012f, name + ": no step (" + juce::String(largest, 4) + ")");
                expect(samples > 60 && samples < 120, name + ": 0.8 of the swing: " + juce::String(samples) + " samples");
                expectEquals(toldBase, 1, name + ": the cell is given its set value once");
                expect(lastLog > 1000.0f && lastLog < 1300.0f, name + ": after coming down to it (" + juce::String(lastLog, 1) + ")");
                if (how == 2)
                    expectWithinAbsoluteError(g.at(CUT), 3.2f, near, "and the new destination has arrived");
            }

            // A voice that stopped sounding with a cell off its value: idle() says so, restore() puts it back.
            Rig g;
            g.slot(0, mod::Src::Velocity, LOG, 0.2f);
            g.note();
            g.tick();
            g.slot(0, mod::Src::Off, LOG, 0.2f);
            g.chunk();
            expect(g.v[0].idle(g.r, g.n), "it holds a cell");
            int restored = 0;
            g.v[0].restore(g.r, [&](int dest, float value) { if (dest == LOG && value == 1000.0f) ++restored; });
            expectEquals(restored, 1);
            expect(! g.v[0].idle(g.r, g.n), "and no longer");

            // A voice struck again just after the slot went: the new note takes everything as it
            // stands, and the cell the old note held is given back with it.
            Rig again;
            again.slot(0, mod::Src::Velocity, LOG, 0.2f);
            again.note();
            again.tick();
            expect(again.told(LOG) > 1500.0f);
            again.slot(0, mod::Src::Off, LOG, 0.2f);
            again.note();
            expect(again.tick(), "something to hand over");
            expectEquals(again.told(LOG), 1000.0f, "the set value");
            expect(! again.tick(), "once");
        }

        beginTest("The track input at audio rate: per sample on a per-sample destination, in the destination's law; a control-rate one samples it");
        {
            Rig g;
            g.input.resize(16);
            for (int i = 0; i < 16; ++i)
                g.input[(size_t) i] = (i % 2 == 0 ? 0.9f : -0.7f) * (float) (i + 1) / 16.0f;      // nothing a ramp could follow
            g.r.set(R::xinId(R::XinDest1), (float) AMP);
            g.r.set(R::xinId(R::XinAmount1), 0.5f);
            g.slot(0, mod::Src::Xin, PTCH, 1.0f);
            g.slot(1, mod::Src::Xin, LIN, 0.2f);
            g.slot(2, mod::Src::Xin, RESO, 1.0f);
            g.note();
            for (int t = 0; t < 3; ++t)
            {
                expect(g.tick());
                const float* a = g.v[0].buffer(AMP);
                const float* p = g.v[0].buffer(PTCH);
                const float* l = g.v[0].buffer(LIN);
                for (int i = 0; i < 16; ++i)
                {
                    const float x = g.input[(size_t) i];
                    expectWithinAbsoluteError(a[i], 1.0f + 0.5f * x, near, "the level, sample " + juce::String(i));
                    expectWithinAbsoluteError(p[i], std::exp2(x), 2.0e-4f, "the pitch: 2 ^ input");
                    expectWithinAbsoluteError(l[i], 5.0f + 0.2f * 10.0f * x, 1.0e-3f, "a cell");
                }
                expectWithinAbsoluteError(g.told(RESO), 0.5f * g.input[15], near, "control rate: the last sample of the sub-block");
            }
            // With a control-rate slot on the same destination: the two add (ramp + audio).
            g.slot(3, mod::Src::Strip, AMP, 1.0f);
            g.strip = 1.0f;
            g.settle();
            expectWithinAbsoluteError(g.v[0].buffer(AMP)[7], 2.0f + 0.5f * g.input[7], near);
            // The amount under an audio slot is led to, like a source: its whole swing in 4 ms.
            g.r.set(R::xinId(R::XinAmount1), 1.0f);
            g.tick();
            const float* a = g.v[0].buffer(AMP);
            const float perSample = 2.0f / (0.004f * (float) SR);
            expectWithinAbsoluteError(a[0] - 2.0f, (0.5f + perSample) * g.input[0], 1.0e-3f, "the first sample is still near the old amount");
            expectWithinAbsoluteError(a[15] - 2.0f, (0.5f + 16.0f * perSample) * g.input[15], 1.0e-3f, "the last a sub-block further");
            g.settle(0, 4);
            expectWithinAbsoluteError(g.v[0].buffer(AMP)[15] - 2.0f, g.input[15], 1.0e-3f, "and a millisecond later it has the new one");
            // The level has an audio floor: the control-rate sum is held at 0, the input goes below.
            Rig ring;
            ring.input = g.input;
            ring.slot(0, mod::Src::One, AMP, -1.0f);
            ring.slot(1, mod::Src::One, AMP, -1.0f);
            ring.note();
            ring.tick();
            expectEquals(ring.at(AMP), 0.0f, "two slots at -100: silence, not the sound upside down");
            ring.slot(2, mod::Src::Xin, AMP, 1.0f);
            ring.settle();
            for (int i = 0; i < 16; ++i)
                expectWithinAbsoluteError(ring.v[0].buffer(AMP)[i], ring.input[(size_t) i], near, "the gain is the input: ring modulation");
            // A cell has none: it stays inside its range.
            Rig cell;
            cell.input.assign(16, -1.0f);
            cell.slot(0, mod::Src::Xin, LIN, 1.0f);
            cell.note();
            cell.tick();
            expectEquals(cell.at(LIN), 0.0f);

            // No input connected: the slots give nothing.
            Rig none;
            none.slot(0, mod::Src::Xin, AMP, 1.0f);
            none.note();
            none.tick();
            expectEquals(none.at(AMP), 1.0f);

            // XLP and XHP: the same input through a one-pole low-pass at LPF / high-pass at HPF;
            // XBP: through both.
            for (int filter = 1; filter <= 3; ++filter)
            {
                auto level = [filter](double hz)
                {
                    Rig r;
                    r.r.set(R::xinId(R::XinLowPass), 500.0f);
                    r.r.set(R::xinId(R::XinHighPass), 500.0f);
                    r.r.set(R::xinId(R::XinDest1), (float) AMP);
                    r.r.set(R::xinId(R::XinAmount1), 1.0f);
                    r.r.set(R::xinId(R::XinFilter1), (float) filter);
                    r.note();
                    r.input.resize(16);
                    double sum = 0.0;
                    int count = 0;
                    for (int t = 0; t < 600; ++t)
                    {
                        for (int i = 0; i < 16; ++i)
                            r.input[(size_t) i] = (float) std::sin(2.0 * juce::MathConstants<double>::pi * hz * (double) (t * 16 + i) / SR);
                        r.tick();
                        if (t >= 300)
                            for (int i = 0; i < 16; ++i, ++count)
                                sum += (double) (r.v[0].buffer(AMP)[i] - 1.0f) * (r.v[0].buffer(AMP)[i] - 1.0f);
                    }
                    return std::sqrt(2.0 * sum / count);        // the amplitude that comes through
                };
                const double low = level(50.0), at = level(500.0), high = level(5000.0);
                if (filter == 3)
                {
                    expectWithinAbsoluteError(at, 0.5, 0.04, "XBP at the two corners: -6 dB");
                    expectWithinAbsoluteError(low, 0.1, 0.02, "XBP: a decade below, a tenth");
                    expectWithinAbsoluteError(high, 0.1, 0.02, "XBP: and a decade above");
                    continue;
                }
                const juce::String name = filter == 1 ? "XLP" : "XHP";
                expectWithinAbsoluteError(at, 0.707, 0.04, name + " at its corner: -3 dB");
                expectWithinAbsoluteError(filter == 1 ? low : high, 1.0, filter == 1 ? 0.02 : 0.05, name + ": the pass band");
                expectWithinAbsoluteError(filter == 1 ? high : low, 0.1, 0.02, name + ": a decade away, a tenth");
            }
        }

        beginTest("An audio-rate slot set, turned, re-aimed or taken away under a note is not jumped to: its gain moves over 2 ms and what it leaves is faded");
        {
            // (A steady input: whatever the destination does is the slot's doing.)
            for (int how = 0; how < 4; ++how)       // AMT to 0, DEST to OFF, DEST elsewhere, FLT (another source)
            {
                Rig g;
                g.input.assign(16, 0.9f);
                g.r.set(R::xinId(R::XinDest1), (float) AMP);
                g.note();
                g.tick();
                float last = 1.0f, largest = 0.0f;
                auto follow = [&](int ticks)
                {
                    for (int t = 0; t < ticks; ++t)
                    {
                        g.tick();
                        for (int i = 0; i < g.n; ++i)
                        {
                            const float* b = g.v[0].buffer(AMP);
                            const float now = b != nullptr ? b[i] : g.r.baseOf(AMP);
                            largest = std::max(largest, std::abs(now - last));
                            last = now;
                        }
                    }
                };
                g.r.set(R::xinId(R::XinAmount1), 1.0f);     // turned up under the note
                follow(3);
                expect(last > 1.2f && last < 1.6f, "half a millisecond on: on its way (" + juce::String(last, 3) + ")");
                follow(12);
                expectWithinAbsoluteError(last, 1.9f, near, "arrived: 1 + 0.9");
                const juce::String name = how == 0 ? "AMT to 0" : (how == 1 ? "DEST to OFF" : (how == 2 ? "another DEST" : "another FLT"));
                if (how == 0)       g.r.set(R::xinId(R::XinAmount1), 0.0f);
                else if (how == 1)  g.r.set(R::xinId(R::XinDest1), 0.0f);
                else if (how == 2)  g.r.set(R::xinId(R::XinDest1), (float) LIN);
                else                g.r.set(R::xinId(R::XinFilter1), 2.0f);     // the high-pass: nothing of a steady input
                follow(3);
                expect(last > 1.3f && last < 1.7f, name + ": half a millisecond on, still leaving (" + juce::String(last, 3) + ")");
                follow(40);
                expectWithinAbsoluteError(last, 1.0f, 0.002f, name + ": gone");
                expect(largest < 0.012f, name + ": no step (" + juce::String(largest, 4) + ")");
                if (how == 2)
                    expectWithinAbsoluteError(g.at(LIN), 10.0f, near, "and the new destination has it (5 + 0.9 x 10, held to 10)");
                if (how != 3)
                {
                    g.settle();
                    expect(how == 2 || ! g.tick(), name + ": and the voice is at rest again");
                }
            }
            // What a slot leaves is faded on the input as it goes on (the input is kept running
            // after its last slot), not on what it last was.
            Rig tail;
            tail.input.assign(16, 0.9f);
            tail.r.set(R::xinId(R::XinDest1), (float) AMP);
            tail.r.set(R::xinId(R::XinAmount1), 1.0f);
            tail.note();
            tail.tick();
            expectWithinAbsoluteError(tail.at(AMP), 1.9f, near);
            tail.r.set(R::xinId(R::XinDest1), 0.0f);
            tail.input.assign(16, 0.3f);
            for (int t = 0; t < 3; ++t)
                tail.tick();
            expectWithinAbsoluteError(tail.at(AMP), 1.15f, 0.01f, "a millisecond on: half the gain x the input as it is now");
            // A destination that comes in under a note starts from its set value as it is now, not
            // from where the voice last had it (the pad pressed, let go, the cell turned, pressed again).
            {
                Rig g;
                // Pressure slot 0: destination LIN, amount 0.2 (as the old MacroTimbre route).
                g.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) LIN);
                g.r.set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 0.2f);
                g.note();
                g.pressure[0] = 1.0f;
                g.settle();
                expectWithinAbsoluteError(g.at(LIN), 7.0f, near);
                g.pressure[0] = 0.0f;
                g.settle();
                expect(! g.tick(), "the pad is up: the voice reads the cell");
                g.r.setBase(LIN_ID, 8.0f);
                g.pressure[0] = 1.0f;
                expect(g.tick());
                expect(g.v[0].buffer(LIN)[0] > 7.99f && g.v[0].buffer(LIN)[0] < 8.05f, "from 8, where the cell is now (" + juce::String(g.v[0].buffer(LIN)[0], 3) + ")");
            }
            // A note struck meanwhile takes everything as it stands: nothing of the old slot.
            Rig struck;
            struck.input.assign(16, 0.9f);
            struck.r.set(R::xinId(R::XinDest1), (float) AMP);
            struck.r.set(R::xinId(R::XinAmount1), 1.0f);
            struck.note();
            struck.tick();
            struck.r.set(R::xinId(R::XinDest1), 0.0f);
            struck.note();
            expect(! struck.tick() && struck.v[0].buffer(AMP) == nullptr, "a new note: no fade of the last one's slot");
            struck.r.set(R::xinId(R::XinDest1), (float) AMP);
            expect(struck.tick() && struck.v[0].buffer(AMP) != nullptr);
            expect(struck.v[0].buffer(AMP)[0] < 1.02f, "and when the slot comes back it comes from nothing (" + juce::String(struck.v[0].buffer(AMP)[0], 3) + ")");
        }

        beginTest("Pressure is each voice's own; the pressure routes (AT ROUTE) are slots: depth, bipolar, vibrato, a hidden destination");
        {
            Rig g;
            g.slot(0, mod::Src::Pressure, CUT, 1.0f);
            g.note(0);
            g.note(1);
            g.pressure = { { 0.5f, 0.0f } };
            for (int t = 0; t < 40; ++t)
            {
                g.chunk();
                g.run(0);
                const float pressed = g.at(CUT, 0);
                g.run(1);
                expectEquals(g.at(CUT, 1), 0.0f, "the other voice is not moved");
                if (t == 39)
                    expectWithinAbsoluteError(pressed, 2.0f, near, "half the pad: two octaves");
            }

            // Set up four pressure slots on a rig: AMP, CUT (bipolar), LIN, HID.
            Rig a;
            a.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) AMP);
            a.r.set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 0.6f);
            a.r.set(PressureSlotParam::id(1, PressureSlotParam::DEST_OFFSET), (float) CUT);
            a.r.set(PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET), 0.5f);
            a.r.set(PressureSlotParam::id(1, PressureSlotParam::BIPOLAR_OFFSET), 1.0f);
            a.r.set(PressureSlotParam::id(2, PressureSlotParam::DEST_OFFSET), (float) LIN);
            a.r.set(PressureSlotParam::id(2, PressureSlotParam::AMT_OFFSET), 0.25f);
            a.r.set(PressureSlotParam::id(3, PressureSlotParam::DEST_OFFSET), (float) HID);
            a.r.set(PressureSlotParam::id(3, PressureSlotParam::AMT_OFFSET), 1.0f);
            a.note(0);
            a.note(1);
            a.pressure = { { 0.75f, 0.0f } };
            a.settle(0);
            expectWithinAbsoluteError(a.at(AMP), 1.0f + 0.6f * 0.75f, near, "the level: 1 + depth x pressure");
            expectWithinAbsoluteError(a.at(CUT), 0.5f * 0.5f * 4.0f, near, "bipolar: (2 p - 1) x depth x 4 octaves");
            expectWithinAbsoluteError(a.at(LIN), 5.0f + 0.25f * 0.75f * 10.0f, 1.0e-3f, "a cell: depth 1 = its whole range");
            expectWithinAbsoluteError(a.v[0].value(HID), 0.75f, near, "the hidden destination");
            expect(a.v[0].moved(HID));
            a.chunk();
            expect(! a.run(1), "the voice whose pad is up: nothing at all (a bipolar route at rest is no pressure)");
            expect(! a.v[1].moved(HID));
            // The pad let go: the routes fall away with it, and then cost nothing again.
            a.pressure[0] = 0.0f;
            a.settle(0);
            expect(! a.tick(0), "pad up again: at rest");
            // No destination, and a depth of 0: no slot. (A value past the list is its last entry.)
            Rig none;
            none.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), 0.0f);
            none.r.set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 1.0f);
            none.r.set(PressureSlotParam::id(1, PressureSlotParam::DEST_OFFSET), (float) AMP);
            none.r.set(PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET), 0.0f);
            none.chunk();
            expect(none.r.idle());
            none.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), 1.0e6f);
            expectEquals((int) none.r.get(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET)), table().pressureChoices() - 1);
            // The amount has the range a route's depth always had: -8 .. 8.
            none.r.set(PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET), 2.0f);
            expectEquals(none.r.get(PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET)), 2.0f);
            none.r.set(PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET), -99.0f);
            expectEquals(none.r.get(PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET)), -8.0f);
            // The hidden destination at depth 2: twice the pad, as far as the destination goes.
            {
                Rig deep;
                deep.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) HID);
                deep.r.set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 2.0f);
                deep.note();
                deep.pressure[0] = 0.3f;
                deep.settle();
                expectWithinAbsoluteError(deep.v[0].value(HID), std::min(0.6f, table()[HID].hi), near, "depth 2: twice the pad");
            }
            // VIB: the vibrato sine through the pad on the pitch, +-depth semitones at full pressure
            // (the value after the table's last entry in a pressure slot's list).
            {
                expectEquals(juce::String(table().pressureLabels()[table().pressureVibrato()]), juce::String("VIB"));
                expectEquals(table().pressureVibrato(), table().size());
                Rig vib;
                vib.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) table().pressureVibrato());
                vib.r.set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 1.0f);
                vib.note();
                vib.pressure[0] = 1.0f;
                float top = 0.0f, bottom = 9.0f;
                for (int i = 0; i < 3000; ++i)
                {
                    vib.tick();
                    top = std::max(top, vib.at(PTCH));
                    bottom = std::min(bottom, vib.at(PTCH));
                }
                expectWithinAbsoluteError(12.0f * std::log2(top), 1.0f, 0.01f, "VIB: a semitone up at full pressure");
                expectWithinAbsoluteError(12.0f * std::log2(bottom), -1.0f, 0.01f, "VIB: and a semitone down");
                vib.pressure[0] = 0.0f;
                vib.settle();
                expect(! vib.tick(), "pad up: no vibrato, and nothing to do");
            }
            // An engine that plays a destination by itself: the router leaves that slot out.
            {
                Rig own;
                mod::Mask mine;
                mine.set(AMP);
                own.r.setOwnPressure(mine, true, 2);
                for (int p = 0; p < 3; ++p)
                {
                    own.r.set(PressureSlotParam::id(p, PressureSlotParam::DEST_OFFSET), (float) AMP);
                    own.r.set(PressureSlotParam::id(p, PressureSlotParam::AMT_OFFSET), 0.5f);
                }
                own.r.set(PressureSlotParam::id(3, PressureSlotParam::DEST_OFFSET), (float) table().pressureVibrato());
                expect(own.r.pressureIsOwn(0) && own.r.pressureIsOwn(1), "the first two slots, aimed at the engine's own");
                expect(! own.r.pressureIsOwn(2), "the third is the router's whatever it is aimed at");
                expect(! own.r.pressureIsOwn(3));
                own.note();
                own.pressure[0] = 1.0f;
                own.settle();
                expectWithinAbsoluteError(own.at(AMP), 1.5f, near, "only the third slot's half is the router's");
            }
            // Pressure on the pitch itself (PTCH): at full pressure and amount 1 the pitch's whole
            // span, an octave up (a route's depth is never squared).
            Rig pres;
            pres.r.set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) PTCH);
            pres.r.set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 1.0f);
            pres.note();
            pres.pressure[0] = 1.0f;
            pres.settle();
            expectWithinAbsoluteError(12.0f * std::log2(pres.at(PTCH)), 12.0f, 0.1f, "full pressure on pitch: 12 semitones up");
            pres.pressure[0] = 0.0f;
            pres.settle();
            expectWithinAbsoluteError(pres.at(PTCH), 1.0f, 0.01f, "pad up: pitch at base");
        }

        beginTest("LFOs: FREE is one LFO for all voices (sounding or not), KEY restarts per note, a new TRIG puts all in step, fade-in, the custom shape");
        {
            Rig g;
            g.lfo(0, AMP, 0.5f, 0, 0, 3.0f);        // a sine, free
            g.note(0);
            for (int t = 0; t < 500; ++t)
            {
                g.chunk();
                g.run(0);
                g.v[1].idle(g.r, g.n);              // the other voice does not sound
            }
            expectWithinAbsoluteError(g.v[0].getLfo(0).getPhase(), g.v[1].getLfo(0).getPhase(), 1.0e-9, "FREE: in step");
            expect(g.v[0].getLfo(0).getPhase() > 0.0);
            g.note(1);
            g.tick(1);
            g.r.set(R::lfoId(0, R::Trigger), 1.0f);     // KEY
            g.chunk();
            g.run(0);
            g.run(1);
            expectWithinAbsoluteError(g.v[0].getLfo(0).getPhase(), g.v[1].getLfo(0).getPhase(), 1.0e-9, "a new trigger mode: both begin again");
            expect(g.v[0].getLfo(0).getPhase() < 0.01);
            for (int t = 0; t < 100; ++t)
                g.tick(0);
            g.note(1);
            g.tick(1);
            expect(g.v[1].getLfo(0).getPhase() < 0.01 && g.v[0].getLfo(0).getPhase() > 0.05, "KEY: the note's own cycle");

            // An LFO nobody reads does not run.
            Rig idle;
            idle.lfo(0, AMP, 0.0f, 0, 0, 3.0f);
            idle.note();
            idle.settle();
            expectEquals(idle.v[0].getLfo(0).getPhase(), 0.0);

            // FADE: the depth rises in a straight line over the time.
            Rig f;
            f.lfo(0, AMP, 1.0f, 2, 1, 0.02f);
            f.r.set(R::lfoId(0, R::Fade), 0.1f);
            f.note();
            for (int t = 0; t < 150; ++t)       // 0.05 s
                f.tick();
            expectWithinAbsoluteError(f.at(AMP), 1.5f, 0.01f, "half way up after half the time");
            f.settle(0, 200);
            expectWithinAbsoluteError(f.at(AMP), 2.0f, near);

            // USR: the eight levels of the LSHP page, an eighth of the cycle each.
            Rig u;
            u.lfo(0, CUT, 1.0f, (int) LFO::Shape::User, 1, 10.0f);
            for (int i = 0; i < 8; ++i)
                u.r.set(LfoShapeParam::BASE + i, i % 2 == 0 ? 0.5f : -0.25f);
            u.note();
            u.tick();
            expectWithinAbsoluteError(u.at(CUT), 2.0f, near, "the first level");
            for (int t = 0; t < 37; ++t)        // into the second eighth (600 samples each)
                u.tick();
            for (int t = 0; t < 30; ++t)
                u.tick();
            expectWithinAbsoluteError(u.at(CUT), -1.0f, near, "the second level");

            // Tempo sync: a quarter note at 120 is 2 Hz.
            Rig s;
            s.lfo(0, AMP, 0.5f, 3, 1, 9.0f);
            s.r.set(R::lfoId(0, R::Sync), 1.0f);
            s.r.setTempo(120.0f);
            s.note();
            for (int t = 0; t < 750; ++t)       // 0.25 s
                s.tick();
            expectWithinAbsoluteError(s.v[0].getLfo(0).getPhase(), 0.5, 2.0e-3);
        }

        beginTest("For the other engines: a voice's own set values, further envelopes as sources, one LFO's rate and depth as table entries, pages at other ids");
        {
            // A voice with set values of its own (a drum of a kit): one router, each voice moved around its own.
            {
                Rig g;
                std::array<float, mod::DestTable::MAX> own {};
                for (int d = 0; d < table().size(); ++d)
                    own[(size_t) d] = table()[d].base;
                own[LIN] = 2.0f;
                own[LOG] = 200.0f;
                g.v[1].useBase(own.data());
                g.slot(0, mod::Src::Velocity, LIN, 0.2f);
                g.slot(1, mod::Src::Velocity, LOG, 0.2f);
                g.note(0);
                g.note(1);
                g.chunk();
                g.run(0);
                expectWithinAbsoluteError(g.at(LIN, 0), 7.0f, near, "the router's set value, 5, + 0.2 x 10");
                const float shared = g.told(LOG, 0);
                g.run(1);
                expectWithinAbsoluteError(g.at(LIN, 1), 4.0f, near, "its own, 2, + 0.2 x 10");
                expectWithinAbsoluteError(g.told(LOG, 1) / shared, 0.2f, 1.0e-4f, "a cell: around its own 200 Hz, not the router's 1000");
                // Let go, each is handed its own set value.
                g.slot(1, mod::Src::Off, LOG, 0.2f);
                float back[2] = { 0.0f, 0.0f };
                for (int t = 0; t < 60; ++t)
                {
                    g.chunk();
                    for (int which = 0; which < 2; ++which)
                    {
                        g.run(which);
                        if (g.told(LOG, which) == (which == 0 ? 1000.0f : 200.0f))
                            back[which] = g.told(LOG, which);
                    }
                }
                expectEquals(back[0], 1000.0f);
                expectEquals(back[1], 200.0f, "its own value back");
                g.slot(1, mod::Src::Velocity, LOG, 0.2f);
                g.settle(1);
                g.slot(1, mod::Src::Off, LOG, 0.2f);
                g.chunk();
                expect(g.v[1].idle(g.r, g.n));
                g.v[1].restore(g.r, [&](int dest, float value) { expect(dest == LOG && value == 200.0f, "restore(): its own too"); });
            }

            // Further envelopes (the unified voice's MENV): sources ENV2 .. ENV5 on a router made for them.
            {
                R r(table(), 2, 4);
                mod::Voice v;
                r.prepare(SR);
                v.prepare(SR);
                r.set(R::stripAmountId(0), 0.0f);
                r.set(R::STRIP_VIBRATO, 0.0f);
                // (OFF .. ENV5 as before, then RND2 RND3 RND4, ONST and the sixteen bands: Router Sources.)
                expectEquals(r.numSources(), (int) mod::Src::Env5 + 1 + 3 + 1 + mod::Src::NUM_BANDS);
                expectEquals(r.sourceIndex(mod::Src::Env5), (int) mod::Src::Env5, "the places up to ENV5 are the enum's");
                r.set(ModSlotParam::id(0, ModSlotParam::SRC), (float) mod::Src::Env3);
                r.set(ModSlotParam::id(0, ModSlotParam::DEST), (float) AMP);
                r.set(ModSlotParam::id(0, ModSlotParam::AMT), 1.0f);
                r.set(ModSlotParam::id(1, ModSlotParam::SRC), (float) mod::Src::Velocity);
                r.set(ModSlotParam::id(1, ModSlotParam::DEST), (float) CUT);
                r.set(ModSlotParam::id(1, ModSlotParam::AMT), 1.0f);
                r.set(ModSlotParam::id(1, ModSlotParam::VIA), (float) mod::Src::Env5);
                expectEquals(r.get(ModSlotParam::id(0, ModSlotParam::SRC)), (float) mod::Src::Env3);
                v.noteOn(r, 60, 1.0f);
                v.setEnvelope(0, 1.0f);         // ENV2: nobody reads it
                v.setEnvelope(1, 0.5f);
                v.setEnvelope(3, 0.25f);
                auto tick = [&]
                {
                    r.beginBlock(nullptr, nullptr, 0, 0.0f, 0.0f, SR);
                    const int n = r.beginChunk(0, 16);
                    expect(v.tick(r, n, 1.0f, 0.0f, 0.0f));
                    return n;
                };
                int n = tick();
                expectWithinAbsoluteError(v.buffer(AMP)[n - 1], 1.5f, near, "ENV3 at 0.5 on the level");
                expectWithinAbsoluteError(v.buffer(CUT)[n - 1], 1.0f, near, "ENV5 at 0.25 as a VIA: a quarter of 4 octaves");
                v.setEnvelope(1, 1.0f);
                n = tick();
                expect(v.buffer(AMP)[n - 1] > 1.5f && v.buffer(AMP)[n - 1] < 1.8f, "led to, like the other envelopes");
                v.setEnvelope(1, std::numeric_limits<float>::quiet_NaN());
                v.setEnvelope(7, 1.0f);         // (no such envelope)
                for (int t = 0; t < 40; ++t)
                    n = tick();
                expectWithinAbsoluteError(v.buffer(AMP)[n - 1], 1.0f, near, "not a number: 0");
                ParamPages pages;
                r.appendPages(pages);
                expectEquals(pages[1].params[0].numChoices, r.numSources(), "SRC offers them");
                expect(juce::String(pages[1].params[0].choices[mod::Src::Env2]) == "ENV2" && juce::String(pages[1].params[3].choices[mod::Src::Env5]) == "ENV5");
                // A router made without them does not have the source: its list goes on with RND2
                // where this one has LFO3, and a place past the end of it is the last one.
                Rig plain;
                expectEquals(plain.r.sourceIndex(mod::Src::Env3), -1);
                expectEquals(plain.r.sourceIndex(mod::Src::Lfo3), -1);
                expectEquals(plain.r.sourceIndex(mod::Src::Random2), mod::Src::offered(2));
                plain.r.set(ModSlotParam::id(0, ModSlotParam::SRC), 1000.0f);
                expectEquals(plain.r.get(ModSlotParam::id(0, ModSlotParam::SRC)), (float) (plain.r.numSources() - 1));
                expectEquals(mod::Src::offered(2), 17);
                expectEquals(mod::Src::offered(4), 19);
                expectEquals(mod::Src::offered(4, 4), 23);
            }

            // One LFO's rate and depth as entries of the table (the unified voice's RAT1 .. DPT4):
            // an LFO's own DEST reaches one other LFO, or itself.
            {
                static const mod::DestTable t = []
                {
                    mod::DestTable d;
                    d.group();
                    d.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample);
                    d.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);
                    d.lfoCell("RAT1", 0, false).lfoCell("RAT2", 1, false).lfoCell("DPT1", 0, true).lfoCell("RAT4", 3, false);
                    d.finish(2);
                    return d;
                }();
                enum { A = 1, C, RAT1, RAT2, DPT1, RAT4 };
                struct Out { double phase1, phase2; float amp; };
                auto run = [&](int dest1, float depth1, int dest2, float depth2)
                {
                    R r(t, 2);
                    mod::Voice v;
                    r.prepare(SR);
                    v.prepare(SR);
                    const float cells[2][5] = { { 5.0f, 3.0f, 0.0f, depth1, (float) dest1 },        // a saw, free, 5 Hz
                                                { 0.02f, 2.0f, 1.0f, depth2, (float) dest2 } };     // a square at +1
                    for (int l = 0; l < 2; ++l)
                    {
                        r.setLfo(l, R::Rate, cells[l][0]);
                        r.setLfo(l, R::Shape, cells[l][1]);
                        r.setLfo(l, R::Trigger, cells[l][2]);
                        r.setLfo(l, R::Depth, cells[l][3]);
                        r.setLfo(l, R::DestCell, cells[l][4]);
                    }
                    r.set(ModSlotParam::id(0, ModSlotParam::SRC), (float) mod::Src::Lfo2);     // (LFO 2 heard on the level through a slot)
                    r.set(ModSlotParam::id(0, ModSlotParam::DEST), (float) A);
                    r.set(ModSlotParam::id(0, ModSlotParam::AMT), 0.4f);
                    v.noteOn(r, 60, 1.0f);
                    Out out {};
                    for (int i = 0; i < 301; ++i)
                    {
                        r.beginBlock(nullptr, nullptr, 0, 0.0f, 0.0f, SR);
                        const int n = r.beginChunk(0, 16);
                        v.tick(r, n, 1.0f, 0.0f, 0.0f);
                        out.amp = v.buffer(A) != nullptr ? v.buffer(A)[n - 1] : 1.0f;
                    }
                    out.phase1 = v.getLfo(0).getPhase();
                    out.phase2 = v.getLfo(1).getPhase();
                    return out;
                };
                const Out plain = run(C, 0.1f, C, 0.1f);
                expectWithinAbsoluteError(plain.phase1, 0.5017, 2.0e-3, "5 Hz for 0.1 s");
                expectWithinAbsoluteError(plain.amp, 1.4f, near);
                const Out faster = run(C, 0.1f, RAT1, 0.5f);
                expectWithinAbsoluteError(faster.phase1, 2.0 * plain.phase1 - 1.0, 4.0e-3, "RAT1 from LFO 2: LFO 1 an octave faster");
                expectWithinAbsoluteError(faster.phase2, plain.phase2, 1.0e-6, "and LFO 2 as it was");
                const Out self = run(C, 0.1f, RAT2, 0.5f);
                expectWithinAbsoluteError(self.phase2, 2.0 * plain.phase2, 1.0e-4, "RAT2 from LFO 2: its own rate");
                expectWithinAbsoluteError(self.phase1, plain.phase1, 1.0e-6);
                const Out none = run(C, 0.1f, RAT4, 1.0f);
                expectWithinAbsoluteError(none.phase1, plain.phase1, 1.0e-6, "an LFO the engine does not have: no slot");
                // DPT1 from LFO 2 (+1 x 0.5): LFO 1's own slot x 1.5. LFO 1 as a square on the level.
                R r(t, 2);
                mod::Voice v;
                r.prepare(SR);
                v.prepare(SR);
                for (int l = 0; l < 2; ++l)
                {
                    r.setLfo(l, R::Rate, 0.02f);
                    r.setLfo(l, R::Shape, 2.0f);
                    r.setLfo(l, R::Trigger, 1.0f);
                }
                r.setLfo(0, R::Depth, 0.4f);
                r.setLfo(0, R::DestCell, (float) A);
                r.setLfo(1, R::Depth, 0.5f);
                r.setLfo(1, R::DestCell, (float) DPT1);
                v.noteOn(r, 60, 1.0f);
                int n = 0;
                for (int i = 0; i < 3; ++i)
                {
                    r.beginBlock(nullptr, nullptr, 0, 0.0f, 0.0f, SR);
                    n = r.beginChunk(0, 16);
                    v.tick(r, n, 1.0f, 0.0f, 0.0f);
                }
                expectWithinAbsoluteError(v.buffer(A)[n - 1], 1.0f + 0.4f * 1.5f, near, "DPT1");
            }

            // The STRIP and XIN pages shown at other ids, without the LFO pages (the unified voice:
            // its own cells are at 174-189, its STRIP page at 424).
            {
                Rig g;
                ParamPages pages;
                R::PageIds ids;
                ids.strip = 424;
                ids.xin = 440;
                ids.lfoPages = false;
                g.r.appendPages(pages, ids);
                juce::StringArray names;
                for (auto& p : pages)
                    names.add(p.name);
                expectEquals(names.joinIntoString(" "), juce::String("XIN SLT1 LSHP STRIP"));
                for (int c = 0; c < 8; ++c)
                {
                    expectEquals(pages[0].params[(size_t) c].id, 440 + c);
                    expectEquals(pages[3].params[(size_t) c].id, 424 + c);
                }
                ParamPages usual;
                g.r.appendPages(usual);
                for (int c = 0; c < 8; ++c)
                    expect(juce::String(usual[0].params[(size_t) c].label) == pages[0].params[(size_t) c].label
                           && juce::String(usual[3].params[(size_t) c].label) == pages[3].params[(size_t) c].label, "the same cells");
            }

            // Cells are set from another thread than the one that renders: the slots are resolved
            // from them at the next beginBlock(), never under a voice that is running.
            {
                Rig g;
                g.note();
                g.tick();
                g.slot(0, mod::Src::Velocity, AMP, 0.5f);
                expect(! g.r.routed(AMP) && g.r.idle(), "set inside a block: the block goes on as it was");
                expect(! g.run() && g.v[0].buffer(AMP) == nullptr);
                expect(g.tick() && g.r.routed(AMP) && ! g.r.idle(), "the next block has it");
                expectEquals(g.r.get(ModSlotParam::id(0, ModSlotParam::AMT)), 0.5f, "(the cell itself reads back at once)");
            }
        }

        beginTest("Cells: ids, round trip, ranges, names, pages");
        {
            Rig g;
            ParamPages pages;
            g.r.appendPages(pages);
            juce::StringArray names;
            for (auto& p : pages)
                names.add(p.name);
            expectEquals(names.joinIntoString(" "), juce::String("XIN SLT1 LSHP STRIP LFO1 LFO2"));
            std::set<int> ids;
            for (auto& page : pages)
                for (auto& c : page.params)
                {
                    expect(c.isValid() && R::owns(c.id), page.name + ": eight working cells of the router");
                    expect(ids.insert(c.id).second, "id " + juce::String(c.id) + " twice");
                    expect(R::name(c.id).isNotEmpty(), "a name for " + juce::String(c.id));
                    if (c.kind == ParamKind::Choice)
                        expect(c.choices != nullptr && c.numChoices == (int) c.maxVal + 1, page.name + " " + c.label);
                    // Both ends are taken and read back; beyond them the value is held.
                    for (float value : { c.minVal, c.maxVal })
                    {
                        g.r.set(c.id, value);
                        expectEquals(g.r.get(c.id), value, page.name + " " + c.label);
                    }
                    g.r.set(c.id, 1.0e9f);
                    expectEquals(g.r.get(c.id), c.maxVal, page.name + " " + c.label + " held at its top");
                    g.r.set(c.id, std::numeric_limits<float>::quiet_NaN());
                    expect(std::isfinite(g.r.get(c.id)));
                }
            expectEquals((int) ids.size(), 48);
            expectEquals(R::lfoId(0, R::Rate), 158);
            expectEquals(R::lfoId(1, R::DestCell), 173);
            expectEquals(R::stripDestId(0), 174);
            expectEquals((int) R::STRIP_VIBRATO_RATE, 181);
            expectEquals(R::xinId(R::XinDest1), 182);
            expectEquals(R::xinId(R::XinHighPass), 189);
            expect(! R::owns(157) && ! R::owns(190) && R::owns(700) && R::owns(772) && R::owns(773) && R::owns(784) && ! R::owns(785));
            // The strip's defaults are the table's.
            R fresh(table(), 2);
            expectEquals(fresh.get(R::stripDestId(0)), (float) CUT);
            expectEquals(fresh.get(R::stripAmountId(0)), 0.5f);
            expectEquals(fresh.get(R::STRIP_VIBRATO), 0.25f);
            expectEquals(fresh.get(R::STRIP_VIBRATO_RATE), 5.5f);
            // A tempo that is no tempo is not taken: a synced LFO keeps a finite rate.
            {
                Rig t;
                t.lfo(0, AMP, 0.5f, (int) LFO::Shape::User, 0, 5.0f);
                t.r.set(R::lfoId(0, R::Sync), 1.0f);
                t.note();
                for (float bpm : { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 1.0e30f, -5.0f, 0.0f })
                {
                    t.r.setTempo(bpm);
                    for (int i = 0; i < 4; ++i)
                        t.tick();
                    const double phase = t.v[0].getLfo(0).getPhase();
                    expect(std::isfinite(phase) && phase >= 0.0 && phase < 1.0, "tempo " + juce::String(bpm) + ": phase " + juce::String(phase));
                    expect(std::isfinite(t.v[0].getLfo(0).getTempo()) && t.v[0].getLfo(0).getTempo() <= 1000.0f);
                }
            }
            // The slot pages follow use.
            Rig p;
            expectEquals(p.r.slotPagesShown(), 1);
            p.r.set(ModSlotParam::id(5, ModSlotParam::AMT), 0.5f);
            expectEquals(p.r.slotPagesShown(), 4, "slot 6 in use: its page and one more");
            p.r.set(ModSlotParam::id(5, ModSlotParam::AMT), 0.0f);
            p.r.set(ModSlotParam::COUNT, 16.0f);
            expectEquals(p.r.slotPagesShown(), 8);
        }

        beginTest("Extremes: every slot in use, sources that are not numbers, 22.05 to 192 kHz: every value finite and inside its destination's limits");
        {
            const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, 0.3f };
            for (double sr : { 22050.0, 44100.0, 96000.0, 192000.0 })
            {
                Rig g(sr);
                expectEquals(g.r.beginChunk(0, 4096), sr <= 48000.0 ? 16 : (sr <= 96000.0 ? 32 : 64), "the sub-block follows the rate");
                const int choices = table().trackSlotChoice();
                for (int k = 0; k < 16; ++k)
                    g.slot(k, 1 + (k * 5) % (mod::Src::offered(2) - 1), 1 + (k * 7) % (choices - 1), k % 2 == 0 ? 1.0f : -1.0f, (k * 3) % mod::Src::offered(2));
                g.lfo(0, PTCH, 1.0f, 4, 0, 40.0f);
                g.lfo(1, LRAT, -1.0f, 9, 1, 40.0f);
                g.r.set(R::xinId(R::XinDest1), (float) PTCH);
                g.r.set(R::xinId(R::XinAmount1), 1.0f);
                g.r.set(R::xinId(R::XinDest2), (float) LIN);
                g.r.set(R::xinId(R::XinAmount2), -1.0f);
                g.r.set(R::xinId(R::XinFilter2), 2.0f);
                g.r.set(R::stripAmountId(0), 1.0f);
                g.r.set(R::STRIP_VIBRATO, 1.0f);
                g.r.setBase(LIN_ID, std::numeric_limits<float>::quiet_NaN());      // (not taken)
                g.input.resize(64);
                g.note(0, 0);
                bool ok = true;
                for (int t = 0; t < 400; ++t)
                {
                    for (int i = 0; i < 64; ++i)
                        g.input[(size_t) i] = bad[(size_t) ((t + i) % 6)];
                    g.env[0] = bad[(size_t) (t % 6)];
                    g.fenv[0] = bad[(size_t) ((t + 1) % 6)];
                    g.pressure[0] = bad[(size_t) ((t + 2) % 6)];
                    g.strip = bad[(size_t) ((t + 3) % 6)];
                    g.follower = bad[(size_t) ((t + 4) % 6)];
                    if (t == 200)
                        g.note(0, 127);
                    if (t % 50 == 49)
                        g.slot(t % 16, mod::Src::Xin, 1 + t % (choices - 1), 1.0f);
                    g.tick();
                    for (int d = 1; d <= HID; ++d)
                    {
                        const auto& dest = table()[d];
                        if (const float* b = g.v[0].buffer(d))
                            for (int i = 0; i < g.n; ++i)
                                ok = ok && std::isfinite(b[i]) && b[i] >= dest.lo && b[i] <= dest.hi;
                    }
                    g.v[0].forEachControl([&](int d, float value)
                    {
                        ok = ok && std::isfinite(value) && value >= table()[d].lo && value <= table()[d].hi;
                    });
                }
                expect(ok, juce::String(sr) + " Hz");
            }
        }
    }

    // =============================================================================================
    void macroTests()
    {
        beginTest("Macro oscillator: the XIN page after the filter's, ids 182-189; a free slot's SRC offers XIN, XLP, XHP, XBP, VIB and ONE");
        {
            M e;
            const auto pages = e.getParamPages();
            juce::StringArray names;
            for (auto& p : pages)
                names.add(p.name);
            // (U4-MACRO: ANLG after MACRO, VOICE before FILTR, BODY+BDY2 after the filter's)
            expectEquals(names.joinIntoString(" "), juce::String("MACRO ANLG AMP VOICE FILTR F ENV BODY BDY2 XIN SLT1 LSHP STRIP LFO1 LFO2"));
            const auto& xin = pages[8];     // +2: VOICE and BDY2 added
            expectEquals(xin.section, juce::String("XIN"));
            static const char* const labels[8] = { "DST1", "AMT1", "FLT1", "LPF", "DST2", "AMT2", "FLT2", "HPF" };
            static const float defaults[8] = { 0.0f, 0.0f, 0.0f, 2000.0f, 0.0f, 0.0f, 0.0f, 200.0f };
            for (int c = 0; c < 8; ++c)
            {
                expectEquals(juce::String(xin.params[(size_t) c].label), juce::String(labels[c]));
                expectEquals(xin.params[(size_t) c].id, 182 + c);
                expectEquals(xin.params[(size_t) c].defaultVal, defaults[c]);
                expectEquals(e.getParameter(182 + c), defaults[c], "seeded");
                expect(e.getParameterName(182 + c).isNotEmpty());
            }
            expectEquals(xin.params[0].numChoices, 45 + mod::TrackDest::COUNT, "the LFOs' destinations (27 + ETIM FTIM + BODY 16: U4-MACRO+BDY2), then the track's effect cells (E4)");
            expectEquals(juce::String(xin.params[0].choices[V::LfoPitch]), juce::String("PTCH"));
            expectEquals(xin.params[2].numChoices, 4);
            expectEquals(juce::String(xin.params[2].choices[1]), juce::String("LP"));
            const auto& slot = pages[9];     // +2: VOICE and BDY2
            // (The 17 there were, RND2 RND3 RND4, ONST and sixteen bands, and the engine's own OSC.)
            expectEquals(slot.params[0].numChoices, 17 + 3 + 1 + 16 + 1);
            expectEquals(juce::String(slot.params[0].choices[mod::Src::XinBand]), juce::String("XBP"));
            expectEquals(juce::String(slot.params[0].choices[mod::Src::One]), juce::String("ONE"));
            expectEquals(juce::String(slot.params[0].choices[mod::Src::Xin]), juce::String("XIN"));
            expectEquals(juce::String(slot.params[0].choices[mod::Src::Vib]), juce::String("VIB"));
            expectEquals(slot.params[1].numChoices, 45 + 4 + 16 + mod::TrackDest::COUNT);  // +8 BDY2
            // The table: what the voice reads by name is where the enum says, and what is per sample.
            const auto& t = M::lfoDests(0);
            expectEquals(t.visible(), 45);  // +8 BDY2
            expectEquals(t.indexOfLabel("FTIM"), 28, "ETIM and FTIM where they were (BODY's eight after them)");
            expectEquals(t.indexOfLabel("LDPT"), 26, "what was there before ETIM and FTIM is where it was");
            expectEquals(t.indexOfLabel("BOW"), (int) V::LfoBow);
            expectEquals(t.indexOfId(M::TimbreParam), (int) V::LfoTimbre);
            expectEquals(t.indexOfId(M::MorphParam), (int) V::LfoMorph);
            for (int d : { (int) V::LfoPitch, (int) V::LfoAmp, (int) V::LfoPan, (int) V::LfoCutoff, (int) V::LfoTimbre,
                           (int) V::LfoColor, (int) V::LfoHarmonics, (int) V::LfoMorph })
                expect(t[d].perSample, juce::String(t[d].label) + " is reached at audio rate");
            expect(! t[V::LfoResonance].perSample && ! t[t.indexOfLabel("LEVL")].perSample);
        }

        beginTest("Macro oscillator: the input on the pitch at audio rate is FM: sidebands at carrier +- modulator, as strong as the law says");
        {
            // A sine at A6 (1760 Hz), the input a 2 kHz sine at half scale, XIN > PTCH at 80 %:
            // the pitch swings by 0.8^2 x 0.5 octaves, exponentially, 2000 times a second (far above
            // the control rate). The carrier moves up to 1760 x I0(z), z = ln 2 x the swing; the
            // deviation at the modulator's frequency is 1760 x 2 I1(z): sidebands J1(b) / J0(b).
            const double fm = 2000.0, swing = 0.8 * 0.8 * 0.5, z = std::log(2.0) * swing;
            const double carrier = 1760.0 * besselI(0, z), beta = 1760.0 * 2.0 * besselI(1, z) / fm;
            auto e = sine();
            e->setParameter(R::xinId(R::XinDest1), (float) V::LfoPitch);
            e->setParameter(R::xinId(R::XinAmount1), 0.8f);
            e->noteOn(93, 1.0f, -1);
            const auto y = render(*e, 1.2, tone(fm, 0.5f));
            const int a = 9600, b = 57600;
            const double c = partial(y, a, b, carrier);
            const double upper = partial(y, a, b, carrier + fm) / c, lower = partial(y, a, b, std::abs(carrier - fm)) / c;
            const double expected = besselJ(1, beta) / besselJ(0, beta);
            logMessage("FM: carrier at " + juce::String(carrier, 1) + " Hz, sidebands " + juce::String(upper, 4) + " / " + juce::String(lower, 4)
                       + " of it, expected " + juce::String(expected, 4));
            expect(c > 0.3, "the carrier is where the exponential law puts it (" + juce::String(c, 3) + ")");
            expect(partial(y, a, b, 1760.0) < 0.2 * c, "and no longer at the note's own frequency");
            expectWithinAbsoluteError(upper, expected, 0.1 * expected, "upper sideband");
            expectWithinAbsoluteError(lower, expected, 0.1 * expected, "lower sideband");
            for (double off : { carrier + 0.5 * fm, carrier + 1.5 * fm, carrier - 0.37 * fm })
                expect(partial(y, a, b, off) < 0.004 * c, "nothing between them (" + juce::String(off, 0) + " Hz)");
            // Without the route the input changes nothing: the same samples as with nothing connected.
            auto plain = sine(), connected = sine();
            plain->noteOn(93, 1.0f, -1);
            connected->noteOn(93, 1.0f, -1);
            expect(render(*plain, 0.2) == render(*connected, 0.2, tone(fm, 0.5f)), "an input nobody routes is not heard");
        }

        beginTest("Macro oscillator: the input on the level is amplitude modulation; with the 1 taken away, ring modulation");
        {
            const double fc = 1760.0, fm = 2000.0;
            auto e = sine();
            freeSlot(*e, 0, mod::Src::Xin, V::LfoAmp, 0.6f);
            e->noteOn(93, 1.0f, -1);
            const auto y = render(*e, 1.2, tone(fm, 0.5f));
            const int a = 9600, b = 57600;
            const double c = partial(y, a, b, fc);
            expectWithinAbsoluteError(partial(y, a, b, fc + fm) / c, 0.15, 0.006, "sum: amount x input / 2");
            expectWithinAbsoluteError(partial(y, a, b, fm - fc) / c, 0.15, 0.006, "difference");
            expect(partial(y, a, b, fc + 2.0 * fm) < 0.003 * c, "and no others");

            // ONE > AMP at -100 takes the 1 away: the carrier is gone, the products stay.
            auto ring = sine();
            freeSlot(*ring, 0, mod::Src::Xin, V::LfoAmp, 0.6f);
            freeSlot(*ring, 1, mod::Src::One, V::LfoAmp, -1.0f);
            ring->noteOn(93, 1.0f, -1);
            const auto r = render(*ring, 1.2, tone(fm, 0.5f));
            const double sum = partial(r, a, b, fc + fm), difference = partial(r, a, b, fm - fc);
            expectWithinAbsoluteError(sum / c, 0.15, 0.006, "the same products");
            expectWithinAbsoluteError(difference / c, 0.15, 0.006);
            expect(partial(r, a, b, fc) < 0.02 * sum, "no carrier: " + juce::String(partial(r, a, b, fc) / sum, 4) + " of a product");
        }

        beginTest("Macro oscillator: the input on a macro at audio rate (the FM model's index), and through its filters");
        {
            // Two-operator FM at index 2, the input a 2 kHz sine on the index: partials that are
            // no multiple of the note's 440 Hz (2000 - 440 = 1560 Hz).
            auto make = [](float amount, int filter, float lowPass)
            {
                auto e = std::make_unique<M>(8);
                e->prepareToPlay(SR, 128);
                e->setParameter(M::ModelParam, 1.0f);
                e->setParameter(M::TimbreParam, 0.25f);
                e->setParameter(M::ColorParam, 0.0f);
                e->setParameter(M::AmpSustain, 1.0f);
                e->setParameter(R::xinId(R::XinDest1), (float) V::LfoTimbre);
                e->setParameter(R::xinId(R::XinAmount1), amount);
                e->setParameter(R::xinId(R::XinFilter1), (float) filter);
                e->setParameter(R::xinId(R::XinLowPass), lowPass);
                e->noteOn(69, 1.0f, -1);
                const auto y = render(*e, 1.2, tone(2000.0, 0.5f));
                return partial(y, 9600, 57600, 1560.0) / partial(y, 9600, 57600, 440.0);
            };
            const double none = make(0.0f, 0, 2000.0f), raw = make(0.2f, 0, 2000.0f), low = make(0.2f, 1, 100.0f);
            logMessage("index at audio rate: 1560 Hz at " + juce::String(none, 5) + " of the fundamental without, " + juce::String(raw, 4)
                       + " with, " + juce::String(low, 5) + " through a 100 Hz low-pass");
            expect(none < 0.001, "without the route there is nothing there");
            expect(raw > 0.02, "with it there is");
            expect(low < 0.1 * raw, "the low-pass at 100 Hz takes a 2 kHz input away (a twentieth)");
        }

        beginTest("Macro oscillator: the harmonic model follows a macro that moves all the time (its partials are refreshed every eighth sample)");
        {
            // TILT swept by a 2 Hz sine over its whole range: the eighth partial (1760 Hz on the A
            // below middle C) comes and goes, from 8^-5 of the fundamental to 8^-1.
            M e(8);
            e.prepareToPlay(SR, 128);
            e.setParameter(M::ModelParam, 4.0f);
            e.setParameter(M::ColorParam, 1.0f);
            e.setParameter(M::HarmonicsParam, 1.0f);
            e.setParameter(M::AmpSustain, 1.0f);
            e.setParameter(R::lfoId(0, R::Rate), 2.0f);
            e.setParameter(R::lfoId(0, R::Depth), 0.5f);
            e.setParameter(R::lfoId(0, R::DestCell), (float) V::LfoTimbre);
            e.noteOn(57, 1.0f, -1);
            const auto y = render(e, 1.2);
            double low = 1.0e9, high = 0.0;
            for (int from = 4800; from + 2400 <= 52800; from += 1200)
            {
                const double level = partial(y, from, from + 2400, 1760.0);
                low = std::min(low, level);
                high = std::max(high, level);
            }
            logMessage("harmonic model, TILT under an LFO: the eighth partial from " + juce::String(low, 6) + " to " + juce::String(high, 4));
            expect(high > 0.01, "it is there at the top of the sweep");
            expect(high > 50.0 * low, "and all but gone at the bottom");
        }

        beginTest("Macro oscillator: a pressure route moves the pressed note only; at rest the engine renders the samples it renders with nothing routed");
        {
            // Set a pressure route: slot 0 to AMP at depth 1 (the old default).
            auto e = sine();
            const int ampDest = e->modRouter()->dests().indexOfLabel("AMP");
            e->modRouter()->set(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) ampDest);
            e->modRouter()->set(PressureSlotParam::id(0, PressureSlotParam::AMT_OFFSET), 1.0f);
            e->noteOn(69, 1.0f, -1);
            e->noteOn(88, 1.0f, -1);        // 1318.5 Hz
            e->aftertouch(69, 1.0f);
            const auto y = render(*e, 0.6);
            auto plain = sine();
            plain->noteOn(69, 1.0f, -1);
            plain->noteOn(88, 1.0f, -1);
            const auto p = render(*plain, 0.6);
            const double hz = 440.0 * std::pow(2.0, 19.0 / 12.0);
            expectWithinAbsoluteError(partial(y, 9600, 28800, 440.0) / partial(p, 9600, 28800, 440.0), 2.0, 0.02, "the pressed note");
            expectWithinAbsoluteError(partial(y, 9600, 28800, hz) / partial(p, 9600, 28800, hz), 1.0, 0.01, "the other one");

            // Routes that have nothing to do (strip down, pads up, an input nobody reads, slots that
            // are off) leave the default sound sample for sample what it is.
            auto idle = std::make_unique<M>(8);
            idle->prepareToPlay(SR, 128);
            idle->noteOn(60, 0.8f, -1);
            idle->noteOn(67, 0.6f, -1);
            const auto reference = render(*idle, 0.3);
            expect(rms(reference, 4800, 14400) > 0.01, "it sounds");
            auto busy = std::make_unique<M>(8);
            busy->prepareToPlay(SR, 128);
            busy->setParameter(R::stripAmountId(2), 1.0f);
            busy->setParameter(R::stripDestId(2), (float) V::LfoCutoff);
            freeSlot(*busy, 0, mod::Src::Off, V::LfoAmp, 1.0f, mod::Src::Xin);
            freeSlot(*busy, 1, mod::Src::Xin, V::LfoPitch, 0.0f);
            busy->setParameter(R::xinId(R::XinDest1), (float) V::LfoPitch);     // (amount 0)
            busy->setParameter(R::lfoId(0, R::DestCell), (float) V::LfoPan);    // (depth 0)
            busy->noteOn(60, 0.8f, -1);
            busy->noteOn(67, 0.6f, -1);
            expect(render(*busy, 0.3, tone(700.0, 0.9f)) == reference, "nothing in use: the same samples");
        }

        beginTest("Macro oscillator, extremes: every slot in use, an input that is not a number, notes 0 and 127, 22.05 to 192 kHz, blocks of 1 to 4096: finite and held");
        {
            const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, 0.9f, -0.9f };
            struct Run { double sr; int block; int model; };
            const Run runs[] = { { 22050.0, 1, 0 }, { 44100.0, 7, 1 }, { 48000.0, 64, 5 }, { 96000.0, 512, 7 }, { 192000.0, 4096, 4 },
                                 { 48000.0, 333, 2 }, { 44100.0, 4096, 3 }, { 96000.0, 1, 6 } };
            for (auto& run : runs)
            {
                M e(8);
                e.prepareToPlay(run.sr, run.block);
                e.setParameter(M::ModelParam, (float) run.model);
                e.setParameter(M::FilterBase + VoiceFilterSection::Resonance, 0.9f);
                const int choices = M::lfoDests(0).trackSlotChoice();
                for (int k = 0; k < 16; ++k)
                    freeSlot(e, k, 1 + (k * 4 + run.model) % (mod::Src::offered(2) - 1), 1 + (k * 5 + run.model * 3) % (choices - 1),
                             k % 2 == 0 ? 1.0f : -1.0f, (k * 3) % mod::Src::offered(2));
                freeSlot(e, run.model, mod::Src::Xin, V::LfoPitch, 1.0f);
                e.setParameter(R::xinId(R::XinDest1), (float) V::LfoCutoff);
                e.setParameter(R::xinId(R::XinAmount1), 1.0f);
                e.setParameter(R::xinId(R::XinDest2), (float) (V::LfoTimbre + run.model % 4));
                e.setParameter(R::xinId(R::XinAmount2), -1.0f);
                e.setParameter(R::xinId(R::XinFilter2), 2.0f);
                e.setParameter(R::lfoId(0, R::DestCell), (float) V::LfoPitch);
                e.setParameter(R::lfoId(0, R::Depth), 1.0f);
                e.setParameter(R::lfoId(0, R::Rate), 40.0f);
                e.setParameter(R::lfoId(1, R::DestCell), 25.0f);        // LRAT
                e.setParameter(R::lfoId(1, R::Depth), 1.0f);
                e.setModWheel(1.0f);
                e.noteOn(0, 1.0f, -1);
                e.noteOn(127, 1.0f, -1);
                e.aftertouch(127, 1.0f);
                float top = 0.0f;
                bool ok = true;
                const double seconds = run.block == 1 ? 0.03 : 0.12;
                const auto y = render(e, seconds, [&](int i) { return bad[(size_t) ((i / 7) % 6)]; }, run.sr, run.block, [&](int b)
                {
                    if (b == 3) e.noteOn(127, 0.5f, -1);
                    if (b == 5) e.setParameter(ModSlotParam::id(2, ModSlotParam::DEST), 2.0f);
                });
                for (float s : y)
                {
                    ok = ok && std::isfinite(s);
                    top = std::max(top, std::abs(s));
                }
                expect(ok, "model " + juce::String(run.model) + " at " + juce::String(run.sr) + " in blocks of " + juce::String(run.block) + ": not finite");
                expect(top < 60.0f, "model " + juce::String(run.model) + ": peak " + juce::String(top, 1));
            }
        }

        beginTest("Macro oscillator: a voice that fell silent while something moved it keeps nothing of it: a cell goes back to its set value, the filter's cutoff offset is dropped");
        {
            // LFO 1 (a square at +1) on a destination while a note sounds and dies away; then the
            // LFO is taken off, with every voice silent.
            auto played = [](int dest, bool lfo)
            {
                auto e = std::make_unique<M>(8);
                e->prepareToPlay(SR, 128);
                e->setParameter(M::AmpRelease, 0.01f);
                e->setParameter(M::FilterBase + VoiceFilterSection::Cutoff, 300.0f);
                if (lfo)
                {
                    e->setParameter(R::lfoId(0, R::Shape), 2.0f);
                    e->setParameter(R::lfoId(0, R::Trigger), 1.0f);
                    e->setParameter(R::lfoId(0, R::Rate), 0.02f);
                    e->setParameter(R::lfoId(0, R::Depth), 0.5f);
                    e->setParameter(R::lfoId(0, R::DestCell), (float) dest);
                }
                e->noteOn(60, 1.0f, -1);
                render(*e, 0.1);
                e->noteOff(60, -1);
                render(*e, 0.3);
                e->setParameter(R::lfoId(0, R::DestCell), 0.0f);
                render(*e, 0.05);
                return e;
            };
            // A page cell (the attack: moved to 0.001 x 2 ^ (0.5 x half its range in octaves)).
            auto cell = played(11, true);       // ATK
            expectEquals(juce::String(M::lfoDests(0).labels()[11]), juce::String("ATK"));
            const float attack = cell->getParameter(M::AmpAttack);
            for (int v = 0; v < 8; ++v)
                expectWithinAbsoluteError(cell->getVoice(v).getAmpEnvelope().getParams().attack, attack, 1.0e-6f, "voice " + juce::String(v) + ": the attack as it is set");
            // The cutoff (two octaves up while it sounded): the next note is the plain one.
            auto cut = played(V::LfoCutoff, true), plain = played(V::LfoCutoff, false);
            cut->noteOn(48, 1.0f, -1);
            plain->noteOn(48, 1.0f, -1);
            const auto a = render(*cut, 0.2), b = render(*plain, 0.2);
            expect(rms(b, 2400, 9600) > 0.01, "it sounds");
            expectWithinAbsoluteError(rms(a, 2400, 9600) / rms(b, 2400, 9600), 1.0, 0.01, "as loud as a note that never had the LFO");
            // (... and while the LFO was on it, it was heard.)
            auto on = played(V::LfoCutoff, true);
            on->setParameter(R::lfoId(0, R::DestCell), (float) V::LfoCutoff);
            on->noteOn(48, 1.0f, -1);
            expect(rms(render(*on, 0.2), 2400, 9600) > 1.15 * rms(b, 2400, 9600), "two octaves more of the saws");
        }

        beginTest("Macro oscillator: nothing is allocated or freed while it renders with every kind of slot in use and cells set from the audio thread");
        {
            M e(8);
            e.prepareToPlay(SR, 256);
            const int choices = M::lfoDests(0).trackSlotChoice();
            for (int k = 0; k < 16; ++k)
                freeSlot(e, k, 1 + (k * 4) % (mod::Src::offered(2) - 1), 1 + (k * 5) % (choices - 1), k % 2 == 0 ? 0.6f : -0.6f, (k * 3) % mod::Src::offered(2));
            e.setParameter(R::xinId(R::XinDest1), (float) V::LfoPitch);
            e.setParameter(R::xinId(R::XinAmount1), 0.5f);
            e.setParameter(R::lfoId(0, R::DestCell), (float) V::LfoCutoff);
            e.setParameter(R::lfoId(0, R::Depth), 0.5f);
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            std::vector<float> in(256, 0.5f);
            auto blocks = [&](int count)
            {
                for (int b = 0; b < count; ++b)
                {
                    // What the sequencer does on the audio thread: notes, step locks, the strip, a pad.
                    if (b % 8 == 0) e.noteOn(48 + b % 24, 0.9f, -1);
                    if (b % 8 == 5) e.noteOff(48 + (b - 5) % 24, -1);
                    if (b % 3 == 0) e.setParameter(R::xinId(R::XinAmount1), b % 6 == 0 ? 0.5f : 0.0f);
                    if (b % 4 == 0) e.setParameter(ModSlotParam::id(b % 16, ModSlotParam::DEST), (float) (1 + b % (choices - 1)));
                    if (b % 5 == 0) e.setParameter(M::TimbreParam, 0.01f * (float) (b % 100));
                    e.setModWheel(b % 7 < 3 ? 1.0f : 0.0f);
                    e.aftertouch(48 + b % 24, b % 2 == 0 ? 0.8f : 0.0f);
                    e.setAudioInput({ in.data(), in.data(), 256, 0.5f });
                    buf.clear();
                    e.processBlock(buf, midi);
                }
            };
            blocks(48);     // (everything has been through once: maps have their keys)
           #if defined(__has_feature) && __has_feature(address_sanitizer)
            logMessage("(a sanitizer's allocator is not seen by malloc_logger: not counted in this build)");
            expect(true);
           #else
            auto* const before = malloc_logger;
            malloc_logger = ModRouterAllocations::count;
            // (The count works: an allocation and its release are seen.)
            ModRouterAllocations::events = 0;
            ModRouterAllocations::counting = true;
            { auto probe = std::make_unique<std::array<float, 4096>>(); (*probe)[0] = buf.getSample(0, 0); }
            ModRouterAllocations::counting = false;
            expect(ModRouterAllocations::events >= 2, "the counter counts");
            ModRouterAllocations::events = 0;
            ModRouterAllocations::counting = true;
            blocks(96);
            ModRouterAllocations::counting = false;
            malloc_logger = before;
            expectEquals(ModRouterAllocations::events, 0, "mallocs and frees in 96 blocks");
           #endif
        }

        beginTest("Macro oscillator: every cell of every page at both ends under full modulation, strip and pads moving, both channels: finite and held");
        {
            const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, 0.9f, -0.9f };
            struct Run { double sr; int block; int model; int blocksPerEnd; };
            for (auto& run : { Run { 22050.0, 1, 0, 24 }, Run { 48000.0, 512, 7, 1 }, Run { 192000.0, 4096, 1, 1 }, Run { 44100.0, 64, 4, 2 } })
            {
                M e(8);
                e.prepareToPlay(run.sr, run.block);
                e.setParameter(M::ModelParam, (float) run.model);
                const int choices = M::lfoDests(0).trackSlotChoice();
                for (int k = 0; k < 16; ++k)
                    freeSlot(e, k, 1 + (k * 4 + run.model) % (mod::Src::offered(2) - 1), 1 + (k * 5 + run.model * 3) % (choices - 1),
                             k % 2 == 0 ? 1.0f : -1.0f, (k * 3) % mod::Src::offered(2));
                e.setModWheel(1.0f);
                for (int note : { 0, 60, 127 })
                    e.noteOn(note, 1.0f, -1);
                juce::AudioBuffer<float> buf(2, run.block);
                juce::MidiBuffer midi;
                std::vector<float> in((size_t) run.block);
                float top = 0.0f;
                bool finite = true;
                int done = 0, block = 0;
                juce::String worst;
                const ParamPages pages = e.getParamPages();
                for (auto& page : pages)
                    for (auto& cell : page.params)
                    {
                        if (! cell.isValid() || e.isRebuildParam(cell.id))
                            continue;
                        const float before = e.getParameter(cell.id);
                        for (float value : { cell.minVal, cell.maxVal })
                        {
                            e.setParameter(cell.id, value);
                            for (int b = 0; b < run.blocksPerEnd; ++b, ++block)
                            {
                                // The strip and a pad from end to end, a note struck again, every few blocks.
                                if (block % 3 == 0) e.setModWheel(block % 6 == 0 ? 1.0f : 0.0f);
                                if (block % 5 == 0) e.aftertouch(127, block % 10 == 0 ? 1.0f : 0.0f);
                                if (block % 7 == 0) e.noteOn(block % 14 == 0 ? 0 : 127, 1.0f, -1);
                                if (block % 4 == 0) e.setTempo(block % 8 == 0 ? bad[(size_t) ((block / 8) % 4)] : (block % 16 == 4 ? 0.0f : 300.0f));
                                if (block % 9 == 0) e.pitchBend(block % 18 == 0 ? 1.0f : -1.0f);
                                for (int i = 0; i < run.block; ++i, ++done)
                                    in[(size_t) i] = bad[(size_t) ((done / 5) % 6)];
                                e.setAudioInput({ in.data(), in.data(), run.block, bad[(size_t) (block % 6)] });
                                buf.clear();
                                e.processBlock(buf, midi);
                                for (int ch = 0; ch < 2; ++ch)
                                    for (int i = 0; i < run.block; ++i)
                                    {
                                        const float y = buf.getSample(ch, i);
                                        finite = finite && std::isfinite(y);
                                        if (std::abs(y) > top)
                                        {
                                            top = std::abs(y);
                                            worst = page.name + " " + cell.label + " at " + juce::String(value);
                                        }
                                    }
                            }
                        }
                        // (The model, the slots' SRC / DEST and the like stay moved: every combination comes by.)
                        if (cell.id == M::LevelParam || cell.id == M::AmpSustain)
                            e.setParameter(cell.id, before);
                    }
                const juce::String where = "model " + juce::String(run.model) + " at " + juce::String(run.sr) + " in blocks of " + juce::String(run.block);
                expect(finite, where + ": not finite");
                expect(top < 60.0f, where + ": peak " + juce::String(top, 1) + " (" + worst + ")");
            }
        }
    }
};

static ModRouterTests modRouterTests;
