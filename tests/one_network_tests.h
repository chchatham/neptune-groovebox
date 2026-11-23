#pragma once

#include <juce_core/juce_core.h>
#include "../src/dsp/effects/ReverbParts.h"
#include "../src/dsp/effects/Reverb.h"
#include "../src/dsp/effects/HallReverb.h"
#include "../src/dsp/effects/SpringReverb.h"
#include "../src/dsp/effects/ShimmerReverb.h"
#include "../src/dsp/effects/PerfEffects.h"
#include "old/OldReverbCores.h"
#include "pinned_on_cores_tests.h"
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// -----------------------------------------------------------------------------
// One reverb network (rvb::Net, ReverbParts.h): the plate, the hall, the spring, the shimmer
// tank and PERF's frozen field are settings of it (rvb::setting::).
//
//   - old against new: tests/old/OldReverbCores.h holds the loops the network replaced
//     (rvb::Core with SPRG and SHIM on it, rvb::Field with FRZ), and RVB and HALL keep their
//     pinned loops beside the network's plate and hall (rvb::network::forPinned). Every pair is
//     measured (level, decay time, brightness early and late, echo density, the null) and
//     judged MATCH / CLOSE / LOST;
//   - every setting dies away (noise in, 30 s of silence, every cell at both ends), stays
//     finite and bounded for any input, rate and block, and glides where a cell is turned;
//   - NEPTUNE_NET_RENDER=<folder> writes the pairs as WAV with the table, and P01's takes with
//     RVB and HALL on their pinned loops (OLD) and on the network (NEW), for the owner's ears;
//   - NEPTUNE_NET_COST=1 measures old against new, run alternately.
// -----------------------------------------------------------------------------
class OneNetworkTests : public juce::UnitTest
{
public:
    OneNetworkTests() : UnitTest("One Network") {}

    using Buffer = std::vector<float>;
    using Stereo = PinnedOnCoresTests::Stereo;
    static constexpr double SR = 48000.0;

    // RVB and HALL on the network (true) or their pinned loops, for as long as it stands.
    struct OnNetwork
    {
        bool was;
        explicit OnNetwork(bool on) : was(rvb::network::forPinned) { rvb::network::forPinned = on; }
        ~OnNetwork() { rvb::network::forPinned = was; }
    };

    using Make = std::function<std::unique_ptr<EffectBase>()>;

    // Made, set and prepared at 48 kHz (RVB and HALL on the path asked for).
    template <typename Fx>
    static Make make(std::function<void(Fx&)> set, bool onNetwork = false)
    {
        return [set, onNetwork]
        {
            OnNetwork scope(onNetwork);
            auto fx = std::make_unique<Fx>();
            set(*fx);
            fx->prepare(SR, 256);
            return std::unique_ptr<EffectBase>(std::move(fx));
        };
    }

    // ---- what is played and measured ------------------------------------------------------------
    // A burst of noise (0.12 s), then a chord of three tones (0.25 to 0.6 s), then silence: the tail
    // is heard to its end. Different on the two sides.
    static constexpr double TAIL_FROM = 0.62;
    static Stereo stimulus(double seconds, int seed = 11)
    {
        const size_t n = (size_t) (seconds * SR);
        Stereo s { Buffer(n, 0.0f), Buffer(n, 0.0f) };
        juce::Random rng(seed);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / SR;
            float l = 0.0f, r = 0.0f;
            if (t < 0.12)
            {
                const float env = (float) std::sin(juce::MathConstants<double>::pi * t / 0.12);
                l = env * 0.4f * (rng.nextFloat() * 2.0f - 1.0f);
                r = env * 0.4f * (rng.nextFloat() * 2.0f - 1.0f);
            }
            else if (t > 0.25 && t < 0.6)
            {
                const double w = 2.0 * juce::MathConstants<double>::pi * t;
                const float env = (float) std::sin(juce::MathConstants<double>::pi * (t - 0.25) / 0.35);
                l = env * 0.12f * (float) (std::sin(w * 220.0) + std::sin(w * 277.18) + std::sin(w * 329.63));
                r = env * 0.12f * (float) (std::sin(w * 220.0) + std::sin(w * 293.66) + std::sin(w * 349.23));
            }
            s.l[i] = l;
            s.r[i] = r;
        }
        return s;
    }

    static Stereo play(EffectBase& fx, const Stereo& in, int block = 256)
    {
        Stereo out = in;
        const int n = (int) in.l.size();
        for (int at = 0; at < n; at += block)
            fx.processBlock(out.l.data() + at, out.r.data() + at, std::min(block, n - at));
        return out;
    }

    static double energy(const Stereo& s, size_t from, size_t to)
    {
        to = std::min(to, s.l.size());
        double e = 0.0;
        for (size_t i = from; i < to; ++i) e += (double) s.l[i] * s.l[i] + (double) s.r[i] * s.r[i];
        return e;
    }
    static double rmsOf(const Stereo& s, double from, double to)
    {
        const size_t a = (size_t) (from * SR), b = std::min(s.l.size(), (size_t) (to * SR));
        return b > a ? std::sqrt(energy(s, a, b) / (2.0 * (double) (b - a))) : 0.0;
    }
    static double dB(double x) { return 20.0 * std::log10(std::max(x, 1.0e-30)); }
    static double brightness(const Stereo& s, double from, double to)
    {
        const size_t a = (size_t) (from * SR), b = std::min(s.l.size(), (size_t) (to * SR));
        double d = 0.0, e = 0.0;
        for (const Buffer* x : { &s.l, &s.r })
            for (size_t i = a + 1; i < b; ++i) { d += std::pow((double) (*x)[i] - (*x)[i - 1], 2.0); e += (double) (*x)[i] * (*x)[i]; }
        return std::sqrt(d / std::max(e, 1.0e-30));
    }

    struct Features
    {
        double level = 0, rt = 0, brightEarly = 0, brightLate = 0, density = 0, lateAt = 0;
    };

    // The tail after the input stops: its level over the first second, the decay time to -60 dB
    // (from the backward-integrated energy between -5 and -25 dB, or as far as it falls), the
    // brightness over its first 0.3 s and where it has fallen by 20 dB, and the echo density over
    // its first 0.3 s (Abel and Huang 2006: the share of samples beyond one deviation in 20 ms
    // windows, against a gaussian's; 1 = as dense as noise).
    static Features measure(const Stereo& s, double from = TAIL_FROM)
    {
        Features f;
        f.level = dB(rmsOf(s, from, from + 1.0));
        const size_t a = (size_t) (from * SR), n = s.l.size();
        std::vector<double> edc(n - a + 1, 0.0);
        for (size_t i = n; i-- > a;)
            edc[i - a] = edc[i - a + 1] + (double) s.l[i] * s.l[i] + (double) s.r[i] * s.r[i];
        const double total = std::max(edc[0], 1.0e-30);
        double t5 = -1, t25 = -1, t20 = -1, deepest = 0, tDeep = 0;
        for (size_t i = 0; i < edc.size(); i += 16)
        {
            const double level = 10.0 * std::log10(std::max(edc[i] / total, 1.0e-30)), t = (double) i / SR;
            if (t5 < 0 && level <= -5.0) t5 = t;
            if (t20 < 0 && level <= -20.0) t20 = t;
            if (t25 < 0 && level <= -25.0) t25 = t;
            if (level > -40.0) { deepest = level; tDeep = t; }
        }
        if (t5 >= 0 && t25 > t5)                 f.rt = 3.0 * (t25 - t5);
        else if (t5 >= 0 && deepest < -10.0)     f.rt = 60.0 * (tDeep - t5) / (-5.0 - deepest);
        else                                     f.rt = 1.0e9;      // (it does not fall: held)
        f.brightEarly = brightness(s, from, from + 0.3);
        f.lateAt = t20 >= 0 ? t20 : 1.0;
        f.brightLate = brightness(s, from + f.lateAt, from + f.lateAt + 0.3);
        double dens = 0.0;
        int windows = 0;
        const size_t win = (size_t) (0.02 * SR);
        for (size_t w = a; w + win <= std::min(n, a + (size_t) (0.3 * SR)); w += win)
        {
            double sq = 0.0;
            for (size_t i = w; i < w + win; ++i) sq += (double) s.l[i] * s.l[i];
            const double sd = std::sqrt(sq / (double) win);
            int beyond = 0;
            for (size_t i = w; i < w + win; ++i) beyond += std::abs((double) s.l[i]) > sd ? 1 : 0;
            dens += (double) beyond / (double) win / 0.3173;
            ++windows;
        }
        f.density = windows > 0 ? dens / windows : 0.0;
        return f;
    }

    static double nullDb(const Stereo& fresh, const Stereo& old)
    {
        double d = 0.0, o = 0.0;
        for (size_t i = 0; i < old.l.size(); ++i)
        {
            d += std::pow((double) fresh.l[i] - old.l[i], 2.0) + std::pow((double) fresh.r[i] - old.r[i], 2.0);
            o += (double) old.l[i] * old.l[i] + (double) old.r[i] * old.r[i];
        }
        return 10.0 * std::log10(std::max(d, 1.0e-30) / std::max(o, 1.0e-30));
    }

    // MATCH: the null below -50 dB, or every measure within a hair (level 0.5 dB, decay 5 %,
    // brightness 5 %, density 0.05). CLOSE: level 2 dB, decay 20 %, brightness 20 %, density 0.15.
    // Otherwise LOST.
    static juce::String verdict(const Features& o, const Features& n, double nul)
    {
        const double dl = std::abs(n.level - o.level);
        const double drt = (o.rt > 1.0e8 && n.rt > 1.0e8) ? 0.0 : std::abs(n.rt / std::max(o.rt, 1.0e-6) - 1.0);
        const double dbE = std::abs(n.brightEarly / std::max(o.brightEarly, 1.0e-9) - 1.0);
        const double dbL = std::abs(n.brightLate / std::max(o.brightLate, 1.0e-9) - 1.0);
        const double dd = std::abs(n.density - o.density);
        if (nul < -50.0 || (dl <= 0.5 && drt <= 0.05 && dbE <= 0.05 && dbL <= 0.05 && dd <= 0.05)) return "MATCH";
        if (dl <= 2.0 && drt <= 0.2 && dbE <= 0.2 && dbL <= 0.2 && dd <= 0.15) return "CLOSE";
        return "LOST";
    }

    static juce::String numbers(const Features& o, const Features& n, double nul)
    {
        auto pc = [](double a, double b) { return juce::String(100.0 * (b / std::max(a, 1.0e-9) - 1.0), 0) + " %"; };
        juce::String rt = (o.rt > 1.0e8 ? juce::String("held") : juce::String(o.rt, 2) + " s") + " -> "
                        + (n.rt > 1.0e8 ? juce::String("held") : juce::String(n.rt, 2) + " s");
        return "null " + juce::String(nul, 1) + " dB | level " + juce::String(n.level - o.level, 2) + " dB | RT60 " + rt
             + " | bright early " + pc(o.brightEarly, n.brightEarly) + ", late " + pc(o.brightLate, n.brightLate)
             + " | density " + juce::String(o.density, 2) + " -> " + juce::String(n.density, 2);
    }

    struct Row { juce::String name, verdict, numbers; };
    std::vector<Row> table;
    juce::File atlas;

    void keepPair(const juce::String& name, const Stereo& old, const Stereo& fresh)
    {
        if (atlas == juce::File()) return;
        PinnedOnCoresTests::writeWav(atlas.getChildFile(name + "_old.wav"), old, SR);
        PinnedOnCoresTests::writeWav(atlas.getChildFile(name + "_new.wav"), fresh, SR);
    }

    // One pair: played the same, measured, judged; `lostOk`: the verdict is reported, not required.
    juce::String compare(const juce::String& name, const Make& older, const Make& newer, double seconds, bool lostOk = false)
    {
        const Stereo in = stimulus(seconds);
        auto a = older(), b = newer();
        const Stereo o = play(*a, in), n = play(*b, in);
        expect(PinnedOnCoresTests::finite(n.l) && PinnedOnCoresTests::finite(n.r), name + ": finite");
        const Features fo = measure(o), fn = measure(n);
        const double nul = nullDb(n, o);
        const juce::String v = verdict(fo, fn, nul), nums = numbers(fo, fn, nul);
        table.push_back({ name, v, nums });
        logMessage("   " + name.paddedRight(' ', 30) + v.paddedRight(' ', 7) + nums);
        if (! lostOk) expect(v != "LOST", name + " is LOST: " + nums);
        keepPair(name, o, n);
        return v;
    }

    void runTest() override
    {
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("NEPTUNE_NET_RENDER", {}); folder.isNotEmpty())
        {
            atlas = juce::File(folder);
            atlas.createDirectory();
        }
        testPinnedPathStays();
        testOldAgainstNew();
        testFreezeOldAgainstNew();
        testSettingsShareEverything();
        testDieAway();
        testBounds();
        testTurns();
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_NET_COST", {}).isNotEmpty())
            measureCost();
        if (atlas != juce::File())
        {
            renderTakes();
            writeTable();
        }
    }

    // ---- 1. the pinned path is the pinned path; the switch puts RVB and HALL on the network ----
    void testPinnedPathStays()
    {
        beginTest("RVB and HALL play their pinned loops unless switched; switched, they play the network's plate and hall");
        const Stereo in = stimulus(3.0);
        for (int which = 0; which < 2; ++which)
        {
            auto build = [which](bool net) -> std::unique_ptr<EffectBase>
            {
                OnNetwork scope(net);
                std::unique_ptr<EffectBase> fx;
                if (which == 0) fx = std::make_unique<Reverb>(); else fx = std::make_unique<HallReverb>();
                fx->prepare(SR, 256);
                return fx;
            };
            auto pinnedA = build(false), pinnedB = build(false), net = build(true);
            const Stereo a = play(*pinnedA, in), b = play(*pinnedB, in), c = play(*net, in);
            expect(PinnedOnCoresTests::same(a, b), "the pinned path is repeatable");
            const double nul = nullDb(c, a);
            expect(nul > -40.0, juce::String(which == 0 ? "RVB" : "HALL") + ": the network is another loop (" + juce::String(nul, 1) + " dB)");
            expect(rmsOf(c, TAIL_FROM, TAIL_FROM + 1.0) > 1.0e-3, "the network's tail is heard");
        }
        expect(! rvb::network::forPinned, "the switch is off by default");
    }

    // ---- 2. old against new: the effects alone ---------------------------------------------------
    void testOldAgainstNew()
    {
        beginTest("Old against new: SPRG and SHIM (rvb::Core) and RVB and HALL (pinned loops) on the network, measured and judged");
        using SP = SpringReverb; using OSP = OldSpringReverb;
        auto spring = [this](const juce::String& name, std::function<void(EffectBase&)> set, double seconds = 5.0)
        {
            compare("SPRG " + name, make<OSP>([set](OSP& f) { set(f); }), make<SP>([set](SP& f) { set(f); }), seconds);
        };
        spring("as made", [](EffectBase&) {});
        spring("DISP 0", [](EffectBase& f) { f.setParameter(SP::Dispersion, 0.0f); });
        spring("DISP 1 TENS 0", [](EffectBase& f) { f.setParameter(SP::Dispersion, 1.0f); f.setParameter(SP::Tension, 0.0f); });
        spring("TENS 1 DAMP 0", [](EffectBase& f) { f.setParameter(SP::Tension, 1.0f); f.setParameter(SP::Damping, 0.0f); });
        spring("DCAY 5 DAMP 1", [](EffectBase& f) { f.setParameter(SP::Decay, 5.0f); f.setParameter(SP::Damping, 1.0f); }, 9.0);
        spring("MOD 1", [](EffectBase& f) { f.setExtra(SP::Modulation, 1.0f); });
        spring("SHIM 0.5 OCT+", [](EffectBase& f) { f.setExtra(SP::Shimmer, 0.5f); });
        spring("PDLY 0.1 s", [](EffectBase& f) { f.setExtra(SP::PreDelay, 0.1f); });

        using SH = ShimmerReverb; using OSH = OldShimmerReverb;
        auto shimmer = [this](const juce::String& name, std::function<void(EffectBase&)> set, double seconds = 8.0)
        {
            compare("SHIM " + name, make<OSH>([set](OSH& f) { set(f); }), make<SH>([set](SH& f) { set(f); }), seconds);
        };
        shimmer("as made", [](EffectBase&) {});
        shimmer("SHIM 0 MOD 0", [](EffectBase& f) { f.setParameter(SH::Shimmer, 0.0f); f.setExtra(SH::Modulation, 0.0f); });
        shimmer("OCT- DAMP 0", [](EffectBase& f) { f.setParameter(SH::ShiftMode, (float) rvb::shift::Sub); f.setParameter(SH::Damping, 0.0f); });
        shimmer("DISP 1", [](EffectBase& f) { f.setExtra(SH::Dispersion, 1.0f); });
        shimmer("SIZE 0.25 DCAY 1", [](EffectBase& f) { f.setExtra(SH::Size, 0.25f); f.setParameter(SH::Decay, 1.0f); }, 5.0);
        shimmer("SIZE 2 DCAY 10", [](EffectBase& f) { f.setExtra(SH::Size, 2.0f); f.setParameter(SH::Decay, 10.0f); }, 14.0);

        // RVB and HALL: the pinned loop (old) against the network's plate and hall (new). Not
        // required to be CLOSE: the owner judges these by ear (the recordings).
        auto plate = [this](const juce::String& name, std::function<void(Reverb&)> set, double seconds = 6.0)
        {
            compare("RVB " + name, make<Reverb>(set, false), make<Reverb>(set, true), seconds, true);
        };
        plate("as P01 sends (0.6 0.4)", [](Reverb& f) { f.setDecay(0.6f); f.setDamping(0.4f); f.setMix(1.0f); });
        plate("as made (0.7 0.5)", [](Reverb&) {});
        plate("long dark large", [](Reverb& f) { f.setDecay(0.99f); f.setDamping(0.9f); f.setSize(2.0f); f.setMix(1.0f); }, 14.0);
        plate("short bright small", [](Reverb& f) { f.setDecay(0.2f); f.setDamping(0.0f); f.setSize(0.25f); f.setMix(1.0f); }, 3.0);
        plate("SHIM 0.5", [](Reverb& f) { f.setExtra(Reverb::Shimmer, 0.5f); f.setMix(1.0f); });

        auto hall = [this](const juce::String& name, std::function<void(HallReverb&)> set, double seconds = 10.0)
        {
            compare("HALL " + name, make<HallReverb>(set, false), make<HallReverb>(set, true), seconds, true);
        };
        auto p01 = [](HallReverb& f) { f.setParameter(HallReverb::DecayTime, 3.0f); f.setParameter(HallReverb::Size, 1.0f);
                                       f.setParameter(HallReverb::Damping, 0.5f); f.setParameter(HallReverb::PreDelay, 40.0f); f.setMix(1.0f); };
        hall("as P01's track 1 (3 s 0.5)", p01);
        hall("as made", [](HallReverb& f) { f.setMix(1.0f); });
        hall("long large", [](HallReverb& f) { f.setParameter(HallReverb::DecayTime, 10.0f); f.setParameter(HallReverb::Size, 2.0f); f.setMix(1.0f); }, 30.0);
        hall("short small damped", [](HallReverb& f) { f.setParameter(HallReverb::DecayTime, 0.5f); f.setParameter(HallReverb::Size, 0.25f);
                                                      f.setParameter(HallReverb::Damping, 0.5f); f.setMix(1.0f); }, 4.0);
        hall("MOD 0", [p01](HallReverb& f) { p01(f); f.setExtra(HallReverb::Mod, 0.0f); });
        hall("MOD 1", [p01](HallReverb& f) { p01(f); f.setExtra(HallReverb::Mod, 1.0f); });
        hall("DAMP 0.2", [p01](HallReverb& f) { p01(f); f.setParameter(HallReverb::Damping, 0.2f); });
        // The old hall's DAMP 1 is no damping at all (its law: d (low-pass with corner a = d) +
        // (1 - d) x, so 0 and 1 both pass everything): its sound is the network's DAMP 0.
        compare("HALL DAMP 1 (old) = DAMP 0 (new)", make<HallReverb>([p01](HallReverb& f) { p01(f); f.setParameter(HallReverb::Damping, 1.0f); }, false),
                make<HallReverb>([p01](HallReverb& f) { p01(f); f.setParameter(HallReverb::Damping, 0.0f); }, true), 10.0, true);

        // Is the old hall's DAMP reachable on the network's law at another value? (Its old law peaks
        // near 0.5 and gives none at 0 and 1; the network's is monotonic.) The nearest by brightness.
        for (float old : { 0.2f, 0.35f, 0.8f })
        {
            const Stereo in = stimulus(10.0);
            auto a = make<HallReverb>([p01, old](HallReverb& f) { p01(f); f.setParameter(HallReverb::Damping, old); }, false)();
            const Stereo o = play(*a, in);
            const Features fo = measure(o);
            float best = 0.0f;
            double bestCost = 1.0e9;
            for (int step = 0; step <= 20; ++step)
            {
                const float d = 0.05f * (float) step;
                auto b = make<HallReverb>([p01, d](HallReverb& f) { p01(f); f.setParameter(HallReverb::Damping, d); }, true)();
                const Features fn = measure(play(*b, in));
                const double cost = std::abs(std::log(fn.brightEarly / fo.brightEarly)) + std::abs(std::log(fn.brightLate / fo.brightLate));
                if (cost < bestCost) { bestCost = cost; best = d; }
            }
            const juce::String v = compare("HALL DAMP " + juce::String(old, 2) + " (old) at DAMP " + juce::String(best, 2) + " (new)",
                                           make<HallReverb>([p01, old](HallReverb& f) { p01(f); f.setParameter(HallReverb::Damping, old); }, false),
                                           make<HallReverb>([p01, best](HallReverb& f) { p01(f); f.setParameter(HallReverb::Damping, best); }, true), 10.0, true);
            // (Below about 0.3 the old law was a dark corner at a shallow depth, which the network's
            // hall shelf does not reach: reported, not required, while HALL keeps its pinned loop.)
            if (old >= 0.3f) expect(v != "LOST", "the old hall's DAMP " + juce::String(old, 2) + " is reachable on the network");
        }
    }

    // ---- 3. FRZ, old against new -------------------------------------------------------------------
    struct Frz { double level = 0, held = 0, fall = 0, bright = 0, climb = 0, climbLate = 0; };

    // A chord for 8 s; the pad pressed at `press` and let go at 4 s.
    static Stereo pressed(EffectBase& fx, double press)
    {
        Stereo in { Buffer((size_t) (8.0 * SR), 0.0f), Buffer((size_t) (8.0 * SR), 0.0f) };
        juce::Random rng(3);
        for (size_t i = 0; i < in.l.size(); ++i)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * (double) i / SR;
            in.l[i] = in.r[i] = 0.2f * (float) std::sin(w * 220.0) + 0.15f * (float) (std::sin(w * 277.18) + std::sin(w * 329.63))
                              + 0.05f * (rng.nextFloat() * 2.0f - 1.0f);
        }
        auto& gesture = dynamic_cast<PerfGesture&>(fx);
        Stereo out = in;
        const int block = 64;
        bool down = false, up = false;
        for (size_t at = 0; at < in.l.size(); at += block)
        {
            const double t = (double) at / SR;
            if (! down && t >= press) { gesture.padGate(true); down = true; }
            if (! up && t >= 4.0) { gesture.padGate(false); up = true; }
            fx.processBlock(out.l.data() + at, out.r.data() + at, block);
        }
        return out;
    }

    static Frz frzMeasure(const Stereo& s)
    {
        Frz f;
        f.level = dB(rmsOf(s, 1.4, 1.9));
        f.held = dB(rmsOf(s, 3.4, 3.9) / std::max(1.0e-9, rmsOf(s, 1.4, 1.9)));
        f.fall = dB(rmsOf(s, 4.4, 4.9) / std::max(1.0e-9, rmsOf(s, 3.4, 3.9)));
        f.bright = brightness(s, 1.4, 1.9);
        f.climb = brightness(s, 4.1, 4.6) / std::max(1.0e-9, brightness(s, 3.0, 3.9));
        f.climbLate = brightness(s, 5.0, 6.0) / std::max(1.0e-9, brightness(s, 3.0, 3.9));
        return f;
    }

    void testFreezeOldAgainstNew()
    {

        beginTest("FRZ, old (rvb::Field) against new (setting::field): held level, steadiness, fall after the release, brightness, the climb");
        struct Case { const char* name; float grab, shim, sprl, dec; };
        const Case cases[] = { { "hold then spiral", 0.25f, 0.0f, 0.5f, 3.0f }, { "plain decay", 0.25f, 0.0f, 0.0f, 1.5f },
                               { "shimmer held", 0.5f, 0.6f, 0.0f, 3.0f }, { "short grab, full spiral", 0.05f, 0.0f, 1.0f, 6.0f } };
        for (const auto& c : cases)
        {
            auto set = [&c](EffectBase& fx) { fx.setParameter(0, c.grab); fx.setParameter(1, c.shim); fx.setParameter(2, c.sprl); fx.setParameter(3, c.dec); };
            // (What is shifted round a held loop goes its own way from every press: five presses, and their means.)
            Frz was, is;
            Stereo firstOld, firstNew;
            const double presses[] = { 0.5, 0.507, 0.513, 0.52, 0.55 };
            for (double press : presses)
            {
                OldFreezeReverb older; FreezeReverb newer;
                set(older); set(newer);
                older.prepare(SR, 64); newer.prepare(SR, 64);
                const Stereo a = pressed(older, press), b = pressed(newer, press);
                expect(PinnedOnCoresTests::finite(b.l), "finite");
                if (press == presses[0]) { firstOld = a; firstNew = b; }
                for (auto* m : { &was, &is })
                {
                    const Frz f = frzMeasure(m == &was ? a : b);
                    m->level += f.level / 5.0; m->held += f.held / 5.0; m->fall += f.fall / 5.0; m->bright += f.bright / 5.0; m->climb += f.climb / 5.0; m->climbLate += f.climbLate / 5.0;
                }
            }
            const double dl = std::abs(is.level - was.level), dh = std::abs(is.held - was.held), df = std::abs(is.fall - was.fall);
            const double db = std::abs(is.bright / was.bright - 1.0), dc = std::abs(is.climb / was.climb - 1.0);
            const juce::String v = (dl <= 0.5 && dh <= 0.5 && df <= 1.0 && db <= 0.05 && dc <= 0.05) ? "MATCH"
                                 : (dl <= 2.0 && dh <= std::max(2.0, 0.25 * std::abs(was.held)) && df <= std::max(2.0, 0.25 * std::abs(was.fall)) && db <= 0.25 && dc <= 0.25) ? "CLOSE" : "LOST";
            const juce::String nums = juce::String::formatted("held level %+.2f dB | held over 2 s %+.1f -> %+.1f dB | fall after release %+.1f -> %+.1f dB | brightness held %+.0f %% | climb x%.2f -> x%.2f, 1-2 s after x%.2f -> x%.2f",
                                                              is.level - was.level, was.held, is.held, was.fall, is.fall, 100.0 * (is.bright / was.bright - 1.0), was.climb, is.climb, was.climbLate, is.climbLate);
            const juce::String name = juce::String("FRZ ") + c.name;
            table.push_back({ name, v, nums });
            logMessage("   " + name.paddedRight(' ', 30) + v.paddedRight(' ', 7) + nums);
            expect(v != "LOST", name + " is LOST: " + nums);
            keepPair(name, firstOld, firstNew);
        }
    }

    // ---- 4. every setting has what the others have -----------------------------------------------
    void testSettingsShareEverything()
    {
        beginTest("Every setting of the network: dispersion, sway, shimmer, HOLD and pre-delay work in each");
        struct Named { const char* name; rvb::Net::Setup s; };
        const Named all[] = { { "plate", rvb::setting::plate() }, { "hall", rvb::setting::hall() }, { "spring", rvb::setting::spring() },
                              { "shimmer", rvb::setting::shimmer() }, { "field", rvb::setting::field() } };
        const Stereo in = stimulus(2.5);
        for (const auto& n : all)
        {
            auto run = [&](std::function<void(rvb::Net&)> set)
            {
                rvb::Net net;
                net.prepare(SR, n.s);
                net.setDecay(2.0f);
                net.setDamping(0.3f);
                set(net);
                net.reset();
                Stereo out = in;
                for (size_t at = 0; at < out.l.size(); at += 32)
                {
                    net.control();
                    net.run(out.l.data() + at, out.r.data() + at, (int) std::min<size_t>(32, out.l.size() - at), 1.0f, 1.0f);
                }
                return out;
            };
            const Stereo plain = run([](rvb::Net&) {});
            {
                // At rest it coasts: nothing on its way, so a block is run at one go.
                rvb::Net net;
                net.prepare(SR, n.s);
                net.setDecay(2.0f);
                net.setDamping(0.3f);
                net.reset();
                std::vector<float> l(32, 0.1f), r(32, 0.1f);
                for (int b = 0; b < 3000; ++b) { net.control(); net.run(l.data(), r.data(), 32, 1.0f, 1.0f); }
                expect(net.resting(), juce::String(n.name) + " comes to rest");
            }
            const double level = rmsOf(plain, TAIL_FROM, TAIL_FROM + 1.0);
            expect(level > 1.0e-3, juce::String(n.name) + " is heard");
            const double dispersed = nullDb(run([](rvb::Net& x) { x.setDispersion(1.0f); }), plain);
            const double swayed = nullDb(run([](rvb::Net& x) { x.setModulation(1.0f); }), plain);
            const double shifted = nullDb(run([](rvb::Net& x) { x.setShimmer(0.7f, 2.0f); }), plain);
            const Stereo late = run([](rvb::Net& x) { x.setPreDelay(200.0f); });
            const Stereo held = run([](rvb::Net& x) { x.setHold(true); });
            logMessage(juce::String("   ") + n.name + ": DISP " + juce::String(dispersed, 1) + " dB, MOD " + juce::String(swayed, 1) + " dB, SHIM "
                       + juce::String(shifted, 1) + " dB from the plain sound");
            expect(dispersed > -30.0, juce::String(n.name) + ": dispersion changes the sound");
            if (n.s.sway != rvb::Net::Sway::None) expect(swayed > -40.0, juce::String(n.name) + ": the sway changes the sound");
            expect(shifted > -30.0, juce::String(n.name) + ": the shimmer changes the sound");
            // Pre-delay: nothing for the first 0.19 s.
            expect(rmsOf(late, 0.0, 0.19) < 0.05 * rmsOf(plain, 0.0, 0.19) + 1.0e-6, juce::String(n.name) + ": the pre-delay holds it back");
            // HOLD: the way in is shut from the start, so nothing gets in at all.
            expect(rmsOf(held, 0.0, 2.5) < 1.0e-4, juce::String(n.name) + ": held from the start, nothing comes in");
        }

        beginTest("HOLD on the network keeps what is in it at its level and brightness, and lets it go");
        for (const auto& n : all)
        {
            rvb::Net net;
            net.prepare(SR, n.s);
            net.setDecay(1.5f);
            net.setDamping(0.9f);
            net.reset();
            const Stereo in = stimulus(12.0);
            Stereo out = in;
            for (size_t at = 0; at < out.l.size(); at += 32)
            {
                if (at == (size_t) (0.7 * SR) / 32 * 32) net.setHold(true);
                if (at == (size_t) (9.0 * SR) / 32 * 32) net.setHold(false);
                net.control();
                net.run(out.l.data() + at, out.r.data() + at, 32, 1.0f, 1.0f);
            }
            const double a = rmsOf(out, 1.5, 2.5), b = rmsOf(out, 7.5, 8.5), gone = rmsOf(out, 11.5, 12.0);
            const double ba = brightness(out, 1.5, 2.5), bb = brightness(out, 7.5, 8.5);
            logMessage(juce::String("   ") + n.name + ": held " + juce::String(dB(b / a), 2) + " dB and brightness x" + juce::String(bb / ba, 2)
                       + " over 6 s; let go " + juce::String(dB(gone / a), 1) + " dB 2.5 s later");
            expect(a > 1.0e-3 && std::abs(dB(b / a)) < 1.5, juce::String(n.name) + ": held at its level");
            expect(bb / ba > 0.6 && bb / ba < 1.5, juce::String(n.name) + ": held at its brightness");
            expect(dB(gone / a) < -20.0, juce::String(n.name) + ": let go, it dies away");
        }
    }

    // ---- 5. every setting dies away (the "Send Reverb" row, with FX2 too) ------------------------
    struct Kind { const char* name; std::function<std::unique_ptr<EffectBase>()> make; };
    static std::vector<Kind> kinds()
    {
        auto onNet = [](auto* raw) { return [raw] { OnNetwork s(true); std::unique_ptr<EffectBase> fx(raw()); return fx; }; };
        return { { "SPRG", [] { return std::unique_ptr<EffectBase>(new SpringReverb()); } },
                 { "SHIM", [] { return std::unique_ptr<EffectBase>(new ShimmerReverb()); } },
                 { "RVB on the network", onNet(+[]() -> EffectBase* { return new Reverb(); }) },
                 { "HALL on the network", onNet(+[]() -> EffectBase* { return new HallReverb(); }) },
                 { "FRZ", [] { return std::unique_ptr<EffectBase>(new FreezeReverb()); } } };
    }

    // Every cell at its end (`top`), the lists that end in HOLD at the entry before it.
    static void allAt(EffectBase& fx, bool top)
    {
        for (int i = 0; i < 4 + EffectBase::MAX_EXTRAS; ++i)
        {
            const ParamSpec spec = i < 4 ? fx.getParamSpec(i) : fx.getExtraSpec(i - 4);
            if (! spec.isValid()) continue;
            float v = top ? spec.maxVal : spec.minVal;
            const juce::String label(spec.label);
            if (label == "HOLD") v = 0.0f;
            if (label == "MODE" && (int) v == (int) rvb::shift::Hold) v = (float) rvb::shift::Hold - 1.0f;
            if (i < 4) fx.setParameter(spec.id, v); else fx.setExtra(i - 4, v);
        }
    }

    void testDieAway()
    {
        beginTest("Every setting falls silent after its input stops: noise in, 30 s of silence, every cell at both ends (HOLD off)");
        for (const auto& k : kinds())
            for (int setting = 0; setting < 3; ++setting)
            {
                std::unique_ptr<EffectBase> fx;
                {
                    OnNetwork scope(juce::String(k.name).contains("network"));
                    fx = k.make();
                    if (setting > 0) allAt(*fx, setting == 1);
                    fx->prepare(48000.0, 64);
                }
                auto* gesture = dynamic_cast<PerfGesture*>(fx.get());
                if (gesture != nullptr) gesture->padGate(true);
                juce::Random rng(1234);
                bool finite = true;
                auto run = [&](int samples, bool noise)
                {
                    double sq = 0.0, sum = 0.0;
                    for (int n = 0; n < samples; ++n)
                    {
                        float l = noise ? (rng.nextFloat() * 2.0f - 1.0f) * 0.25f : 0.0f, r = l;
                        fx->process(l, r);
                        finite = finite && std::isfinite(l) && std::isfinite(r);
                        sq += (double) l * l + (double) r * r;
                        sum += (double) l + (double) r;
                    }
                    const double mean = sum / (2.0 * samples);
                    return std::sqrt(std::max(0.0, sq / (2.0 * samples) - mean * mean));
                };
                const double playing = run(48000, true);
                if (gesture != nullptr) gesture->padGate(false);
                double loudest = run(48000, false);
                for (int w = 0; w < 9; ++w) loudest = std::max(loudest, run(48000 * 3, false));
                const double late = run(48000 * 2, false);
                const juce::String what = juce::String(k.name) + (setting == 0 ? " as made" : setting == 1 ? " every cell at its top" : " every cell at its bottom");
                logMessage("   " + what + ": playing " + juce::String(playing, 4) + ", 30 s after " + juce::String(late, 7));
                expect(finite, what + " is not finite");
                expect(late <= loudest * 0.5 + 1.0e-5, what + " does not die away");
                expect(late < 0.05, what + " still sounds after 30 s");
            }
    }

    // ---- 6. bounds ---------------------------------------------------------------------------------
    void testBounds()
    {
        beginTest("Finite and bounded for any input (not a number, infinity, 1e30), 22.05 to 192 kHz, blocks of 1 to 4096, every cell at both ends, HOLD on");
        const float nasty[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0e30f, 64.0f, -64.0f };
        for (const auto& k : kinds())
            for (double sr : { 22050.0, 44100.0, 96000.0, 192000.0 })
                for (int setting = 0; setting < 3; ++setting)
                {
                    std::unique_ptr<EffectBase> fx;
                    {
                        OnNetwork scope(juce::String(k.name).contains("network"));
                        fx = k.make();
                        if (setting > 0) allAt(*fx, setting == 1);
                        if (setting == 1)
                            for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
                                if (juce::String(fx->getExtraSpec(i).label) == "HOLD") fx->setExtra(i, 1.0f);
                        fx->prepare(sr, 4096);
                    }
                    if (auto* g = dynamic_cast<PerfGesture*>(fx.get())) g->padGate(true);
                    std::vector<float> l(4096), r(4096);
                    float most = 0.0f;
                    bool finite = true;
                    juce::Random rng(5);
                    const int blocks[] = { 1, 7, 64, 333, 4096, 2, 1000 };
                    for (int pass = 0; pass < 40; ++pass)
                    {
                        const int n = blocks[pass % 7];
                        for (int i = 0; i < n; ++i)
                        {
                            l[(size_t) i] = pass < 20 ? (rng.nextInt(50) == 0 ? nasty[rng.nextInt(5)] : 8.0f * (rng.nextFloat() - 0.5f)) : 0.0f;
                            r[(size_t) i] = pass < 20 ? 8.0f * (rng.nextFloat() - 0.5f) : 0.0f;
                        }
                        fx->processBlock(l.data(), r.data(), n);
                        for (int i = 0; i < n; ++i)
                        {
                            finite = finite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
                            most = std::max({ most, std::abs(l[(size_t) i]), std::abs(r[(size_t) i]) });
                        }
                    }
                    const juce::String what = juce::String(k.name) + " at " + juce::String(sr) + (setting == 1 ? " top, HOLD on" : setting == 2 ? " bottom" : " as made");
                    expect(finite, what + " is not finite");
                    expect(most < 1000.0f, what + " runs away: " + juce::String(most));
                }
    }

    // ---- 7. turned under sound: glided ------------------------------------------------------------
    static float largestStep(const Buffer& x, size_t from, size_t to)
    {
        float most = 0.0f;
        for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i) most = std::max(most, std::abs(x[i] - x[i - 1]));
        return most;
    }

    void testTurns()
    {
        beginTest("A cell turned under a steady tone is glided to: no step larger than twice the sound's own before and after, or than the old loop's own turn");
        // `old`: the loop it replaces turned alike (a turned length bends the pitch in both: the step
        // of a read that moves faster is not a click).
        struct Turn { const char* name; std::function<std::unique_ptr<EffectBase>()> make, old; int cell; float from, to; bool onNet; };
        auto spring = [] { return std::unique_ptr<EffectBase>(new SpringReverb()); };
        auto oldSpring = [] { return std::unique_ptr<EffectBase>(new OldSpringReverb()); };
        auto shim = [] { return std::unique_ptr<EffectBase>(new ShimmerReverb()); };
        auto oldShim = [] { return std::unique_ptr<EffectBase>(new OldShimmerReverb()); };
        auto plate = [] { return std::unique_ptr<EffectBase>(new Reverb()); };
        auto hall = [] { return std::unique_ptr<EffectBase>(new HallReverb()); };
        const Turn turns[] = {
            { "SPRG TENS", spring, oldSpring, SpringReverb::Tension, 0.2f, 0.9f, false }, { "SPRG DISP", spring, oldSpring, SpringReverb::Dispersion, 0.0f, 1.0f, false },
            { "SPRG DAMP", spring, oldSpring, SpringReverb::Damping, 0.0f, 1.0f, false }, { "SPRG MOD", spring, oldSpring, 4 + SpringReverb::Modulation, 0.0f, 1.0f, false },
            { "SHIM SIZE", shim, oldShim, 4 + ShimmerReverb::Size, 1.0f, 0.4f, false }, { "SHIM DCAY", shim, oldShim, ShimmerReverb::Decay, 3.0f, 0.5f, false },
            { "SHIM SHIM", shim, oldShim, ShimmerReverb::Shimmer, 0.0f, 1.0f, false }, { "SHIM DISP", shim, oldShim, 4 + ShimmerReverb::Dispersion, 0.0f, 1.0f, false },
            { "RVB(net) SIZE", plate, plate, Reverb::Size, 1.0f, 0.5f, true }, { "RVB(net) DCAY", plate, plate, Reverb::Decay, 0.7f, 0.2f, true },
            { "RVB(net) HOLD", plate, plate, 4 + Reverb::Hold, 0.0f, 1.0f, true }, { "HALL(net) SIZE", hall, hall, HallReverb::Size, 1.0f, 0.4f, true },
            { "HALL(net) MOD", hall, hall, 4 + HallReverb::Mod, 0.0f, 1.0f, true }, { "HALL(net) PDLY", hall, hall, HallReverb::PreDelay, 10.0f, 90.0f, true } };
        const size_t n = (size_t) (2.0 * SR), at = (size_t) (1.0 * SR) / 256 * 256;
        Stereo tone { Buffer(n), Buffer(n) };
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / SR;
            tone.l[i] = tone.r[i] = 0.3f * (float) (std::sin(2.0 * juce::MathConstants<double>::pi * 110.0 * t) + 0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * t));
        }
        for (const auto& t : turns)
        {
            auto set = [&t](EffectBase& fx, float v) { if (t.cell < 4) fx.setParameter(fx.getParamSpec(t.cell).id, v); else fx.setExtra(t.cell - 4, v); };
            auto runWith = [&](const std::function<std::unique_ptr<EffectBase>()>& make, bool net, bool turn, float startAt)
            {
                std::unique_ptr<EffectBase> fx;
                {
                    OnNetwork s(net);
                    fx = make();
                    fx->setMix(0.5f);
                    set(*fx, startAt);
                    fx->prepare(SR, 256);
                }
                Stereo out = tone;
                for (size_t b = 0; b < n; b += 256)
                {
                    if (turn && b == at) set(*fx, t.to);
                    fx->processBlock(out.l.data() + b, out.r.data() + b, 256);
                }
                return out;
            };
            const Stereo turned = runWith(t.make, t.onNet, true, t.from), before = runWith(t.make, t.onNet, false, t.from), after = runWith(t.make, t.onNet, false, t.to);
            const Stereo oldTurned = runWith(t.old, false, true, t.from);
            const float own = std::max(largestStep(before.l, at - 4800, at + 4800), largestStep(after.l, at - 4800, at + 4800));
            const float step = largestStep(turned.l, at - 64, at + 4800), oldStep = largestStep(oldTurned.l, at - 64, at + 4800);
            logMessage(juce::String("   ") + t.name + ": largest step x" + juce::String(step / std::max(own, 1.0e-9f), 2) + " the sound's own, x"
                       + juce::String(step / std::max(oldStep, 1.0e-9f), 2) + " the old loop's turn");
            expect(step < 2.0f * own || step < 1.25f * oldStep, juce::String(t.name) + " jumps: x" + juce::String(step / std::max(own, 1.0e-9f), 2));
        }
    }

    // ---- 8. cost -----------------------------------------------------------------------------------
    static double microseconds(EffectBase& fx)
    {
        std::vector<float> l(512), r(512);
        juce::Random rng(9);
        double best = 1.0e9;
        for (int round = 0; round < 7; ++round)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < 200; ++b)
            {
                for (int i = 0; i < 512; ++i) { l[(size_t) i] = 0.2f * (rng.nextFloat() - 0.5f); r[(size_t) i] = l[(size_t) i]; }
                fx.processBlock(l.data(), r.data(), 512);
            }
            best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 200.0);
        }
        return best;
    }

    void measureCost()
    {
        beginTest("Cost: microseconds per block of 512 at 48 kHz, old and new alternately (best of 7, three rounds)");
        struct Two { const char* name; Make older, newer; };
        auto gate = [](Make m) { return Make([m] { auto fx = m(); if (auto* g = dynamic_cast<PerfGesture*>(fx.get())) g->padGate(true); return fx; }); };
        const Two pairs[] = {
            { "SPRG as made", make<OldSpringReverb>([](OldSpringReverb&) {}), make<SpringReverb>([](SpringReverb&) {}) },
            { "SPRG MOD 1 SHIM 0.5", make<OldSpringReverb>([](OldSpringReverb& f) { f.setExtra(3, 1.0f); f.setExtra(1, 0.5f); }),
                                     make<SpringReverb>([](SpringReverb& f) { f.setExtra(3, 1.0f); f.setExtra(1, 0.5f); }) },
            { "SHIM as made", make<OldShimmerReverb>([](OldShimmerReverb&) {}), make<ShimmerReverb>([](ShimmerReverb&) {}) },
            { "SHIM DISP 1", make<OldShimmerReverb>([](OldShimmerReverb& f) { f.setExtra(2, 1.0f); }), make<ShimmerReverb>([](ShimmerReverb& f) { f.setExtra(2, 1.0f); }) },
            { "FRZ held", gate(make<OldFreezeReverb>([](OldFreezeReverb&) {})), gate(make<FreezeReverb>([](FreezeReverb&) {})) },
            { "FRZ held SHIM 0.5", gate(make<OldFreezeReverb>([](OldFreezeReverb& f) { f.setParameter(1, 0.5f); })),
                                   gate(make<FreezeReverb>([](FreezeReverb& f) { f.setParameter(1, 0.5f); })) },
            { "RVB pinned / network", make<Reverb>([](Reverb&) {}, false), make<Reverb>([](Reverb&) {}, true) },
            { "HALL pinned / network", make<HallReverb>([](HallReverb&) {}, false), make<HallReverb>([](HallReverb&) {}, true) },
            { "HALL MOD 0 pinned / network", make<HallReverb>([](HallReverb& f) { f.setExtra(0, 0.0f); }, false), make<HallReverb>([](HallReverb& f) { f.setExtra(0, 0.0f); }, true) } };
        for (const auto& p : pairs)
        {
            double o = 1.0e9, n = 1.0e9;
            for (int round = 0; round < 3; ++round)
            {
                auto a = p.older(); o = std::min(o, microseconds(*a));
                auto b = p.newer(); n = std::min(n, microseconds(*b));
            }
            const juce::String line = juce::String(p.name).paddedRight(' ', 30) + juce::String(o, 2).paddedLeft(' ', 8) + juce::String(n, 2).paddedLeft(' ', 8)
                                    + juce::String(100.0 * (n / o - 1.0), 0).paddedLeft(' ', 6) + " %";
            logMessage("   " + line);
            costs << line << "\n";
        }
        expect(true);
    }
    juce::String costs;

    // ---- 9. the recordings -----------------------------------------------------------------------

    void renderTakes()
    {
        beginTest("The recordings: P01's two sounds playing its chord pads, RVB and HALL on their pinned loops (OLD) and on the network (NEW)");
        const juce::int64 randomBefore = juce::Random::getSystemRandom().getSeed();
        const auto chordFile = juce::File(juce::SystemStats::getEnvironmentVariable(
            "NEPTUNE_E7_CHORDS", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Neptune/Projects/P01/project.json").getFullPathName()));
        expect(chordFile.existsAsFile(), "the project with the chord pads: " + chordFile.getFullPathName());
        if (! chordFile.existsAsFile()) return;
        ChordPads chords;
        chords.fromFlat(ProjectData::fromJSON(chordFile.loadFileAsString()).chordPads);       // (read only)

        juce::String nums, scores;
        for (const auto& take : PinnedOnCoresTests::takes())
        {
            juce::String played;
            int badOld = 0, badNew = 0;
            Stereo old, fresh;
            { OnNetwork s(false); old = PinnedOnCoresTests::renderTakeWith(take, chords, nullptr, &played, &badOld); }
            { OnNetwork s(true);  fresh = PinnedOnCoresTests::renderTakeWith(take, chords, nullptr, nullptr, &badNew); }
            expectEquals(badOld + badNew, 0, take.name);
            if (old.l.empty() || old.l.size() != fresh.l.size()) { expect(false, take.name); continue; }
            Stereo diff = fresh;
            double worst = 0.0;
            for (size_t i = 0; i < diff.l.size(); ++i)
            {
                diff.l[i] -= old.l[i];
                diff.r[i] -= old.r[i];
                worst = std::max({ worst, std::abs((double) diff.l[i]), std::abs((double) diff.r[i]) });
            }
            PinnedOnCoresTests::writeWav(atlas.getChildFile(juce::String(take.name) + "_OLD.wav"), old, SR);
            PinnedOnCoresTests::writeWav(atlas.getChildFile(juce::String(take.name) + "_NEW.wav"), fresh, SR);
            PinnedOnCoresTests::writeWav(atlas.getChildFile(juce::String(take.name) + "_DIFF.wav"), diff, SR);
            const double end = (double) old.l.size() / SR;
            const double oldRms = rmsOf(old, 0.0, end), newRms = rmsOf(fresh, 0.0, end), diffRms = rmsOf(diff, 0.0, end);
            // Brightness and level over the whole take and over its last ring-out.
            const double ring = std::max(0.0, end - 5.0);
            nums << take.name << "\n   length " << juce::String(end, 1) << " s | DIFF " << juce::String(dB(diffRms / std::max(oldRms, 1.0e-30)), 1)
                 << " dB against OLD (largest " << juce::String(dB(worst), 1) << " dBFS) | level NEW against OLD " << juce::String(dB(newRms / oldRms), 2)
                 << " dB | brightness x" << juce::String(brightness(fresh, 0.0, end) / brightness(old, 0.0, end), 2)
                 << " | the last 5 s: level " << juce::String(dB(rmsOf(fresh, ring, end) / std::max(1.0e-12, rmsOf(old, ring, end))), 2)
                 << " dB, brightness x" << juce::String(brightness(fresh, ring, end) / std::max(1.0e-12, brightness(old, ring, end)), 2) << "\n\n";
            scores << take.name << "  (" << (take.track == 2 ? "track 3 \"User 13\": RVB on its send" : "track 1 \"DuneDrone\": HALL in its insert") << ", "
                   << juce::String(take.bpm, 0) << " bpm)\n   " << take.title << "\n" << played << "\n";
            logMessage("   " + juce::String(take.name).paddedRight(' ', 26) + "DIFF " + juce::String(dB(diffRms / std::max(oldRms, 1.0e-30)), 1) + " dB, level "
                       + juce::String(dB(newRms / oldRms), 2) + " dB");
        }
        P01KeyboardTests::seedSystemRandom(randomBefore);
        takeNumbers = nums;
        takeScores = scores;
    }
    juce::String takeNumbers, takeScores;

    void writeTable()
    {
        juce::String t;
        for (auto& r : table) t << "   " << r.name.paddedRight(' ', 34) << r.verdict.paddedRight(' ', 7) << r.numbers << "\n";
        atlas.getChildFile("numbers.txt").replaceWithText("One reverb network: OLD against NEW\n\nThe effects alone (<name>_old.wav / <name>_new.wav):\n" + t
                                                         + "\nP01's takes (<take>_OLD / _NEW / _DIFF.wav):\n" + takeNumbers
                                                         + (costs.isNotEmpty() ? "\nCost, microseconds per block of 512 at 48 kHz (old, new, change):\n" + costs : juce::String()));
        juce::String readme;
        readme << "One reverb network: the send reverb (RVB) and the hall (HALL) on the network, for your ears\n"
               << "=========================================================================================\n\n"
               << "Every reverb is now a setting of one network of delay lines (the plate, the hall, the spring, the shimmer\n"
               << "tank, PERF's freeze). RVB and HALL as P01 uses them are held to the bit, and the network does not give their\n"
               << "samples (one damping law for all, the shared LFO on the hall's heads, a gain per line). So they still play\n"
               << "their old loops, and these recordings put the two side by side. If NEW is good enough, say so: then RVB\n"
               << "and HALL move onto the network, their old loops go, and the pin is made again once.\n\n"
               << "What the files are\n"
               << "  <take>_OLD.wav   P01 as it plays today (RVB and HALL on their own loops)\n"
               << "  <take>_NEW.wav   the same take, same notes, same per-note spread, with RVB and HALL on the network\n"
               << "  <take>_DIFF.wav  NEW minus OLD, sample by sample\n"
               << "  numbers.txt      the level of each DIFF, the level and brightness of NEW against OLD, and the table of\n"
               << "                   every effect alone (MATCH / CLOSE / LOST with the numbers)\n"
               << "  <name>_old.wav / <name>_new.wav   each reverb alone on a burst of noise and a chord, old and new\n"
               << "All WAV files: 48 kHz, 24-bit, stereo, not normalised.\n\n"
               << "Only RVB (the send reverb track 3 sends to) and HALL (track 1's insert) differ; DLY is the same in both.\n"
               << "To make them again: NEPTUNE_NET_RENDER=<folder> NEPTUNE_TEST_ONLY=\"One Network\" build/NeptuneTests_artefacts/Debug/NeptuneTests\n\n"
               << "The takes\n\n" << takeScores;
        atlas.getChildFile("README.txt").replaceWithText(readme);
    }
};

static OneNetworkTests oneNetworkTests;
