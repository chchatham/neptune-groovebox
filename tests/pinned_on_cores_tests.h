#pragma once

#include <juce_core/juce_core.h>
#include "../src/dsp/effects/Delay.h"
#include "../src/dsp/effects/Reverb.h"
#include "../src/dsp/effects/HallReverb.h"
#include "old/OldDelay.h"
#include "old/OldReverb.h"
#include "old/OldHallReverb.h"
#include "p01_keyboard_tests.h"
#include "controller/ChordPads.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

// -----------------------------------------------------------------------------
// spec-unify.md, phase E7: the send delay (DLY), the send reverb (RVB) and the hall
// (HALL) on the cores (DelayCore.h, ReverbParts.h).
//
// tests/old/ holds the three classes as they were at b4ee701 (OldDelay, OldReverb,
// OldHallReverb), compiled into the tests only. They are the reference:
//   - "to the bit": with the new cells at rest the new classes give the samples the
//     old ones gave, bit for bit, for noise, clicks and silence, at 44.1 and 48 kHz,
//     in blocks of any size, alone and one sample at a time;
//   - what differs on purpose is measured against them (a glided TIME / SIZE, lines
//     long enough at 96 kHz and above);
//   - the cost of old and new is measured side by side;
//   - NEPTUNE_E7_RENDER=<folder> renders the recordings of the phase with the old
//     classes put into a real AudioEngine (send buses and insert) and with the new.
// -----------------------------------------------------------------------------
class PinnedOnCoresTests : public juce::UnitTest
{
public:
    PinnedOnCoresTests() : UnitTest("Pinned On Cores") {}

    using Buffer = std::vector<float>;
    struct Stereo { Buffer l, r; };

    static bool same(const Buffer& a, const Buffer& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }
    static bool same(const Stereo& a, const Stereo& b) { return same(a.l, b.l) && same(a.r, b.r); }

    static double rms(const Buffer& x, size_t from, size_t to)
    {
        to = std::min(to, x.size());
        if (to <= from) return 0.0;
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) sum += (double) x[i] * (double) x[i];
        return std::sqrt(sum / (double) (to - from));
    }
    static double rms(const Buffer& x) { return rms(x, 0, x.size()); }
    static double dB(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-30)); }
    static float peak(const Buffer& x)
    {
        float p = 0.0f;
        for (float v : x) p = std::max(p, std::abs(v));
        return p;
    }
    static bool finite(const Buffer& x)
    {
        for (float v : x) if (! std::isfinite(v)) return false;
        return true;
    }
    // NEW minus OLD against OLD, in dB.
    static double differenceDb(const Stereo& fresh, const Stereo& old)
    {
        double d = 0.0, o = 0.0;
        for (size_t i = 0; i < old.l.size(); ++i)
        {
            d += std::pow((double) fresh.l[i] - old.l[i], 2.0) + std::pow((double) fresh.r[i] - old.r[i], 2.0);
            o += std::pow((double) old.l[i], 2.0) + std::pow((double) old.r[i], 2.0);
        }
        return dB(std::sqrt(d / std::max(o, 1.0e-60)));
    }

    // What the effects are played: bursts of noise with silence between (a tail is heard to its
    // end), a click, a low tone; different on the two sides.
    static Stereo material(double sr, double seconds, int seed = 7)
    {
        const size_t n = (size_t) (seconds * sr);
        Stereo s { Buffer(n, 0.0f), Buffer(n, 0.0f) };
        juce::Random rng(seed);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const double inBurst = std::fmod(t, 0.9);
            float l = 0.0f, r = 0.0f;
            if (inBurst < 0.12)
            {
                const float env = (float) std::sin(juce::MathConstants<double>::pi * inBurst / 0.12);
                l = env * 0.5f * (rng.nextFloat() * 2.0f - 1.0f);
                r = env * 0.5f * (rng.nextFloat() * 2.0f - 1.0f);
            }
            else if (inBurst > 0.3 && inBurst < 0.5)
            {
                l = 0.3f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 110.0 * t);
                r = 0.3f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 164.8 * t);
            }
            s.l[i] = l;
            s.r[i] = r;
        }
        if (n > 100) { s.l[50] = 0.9f; s.r[77] = -0.8f; }
        return s;
    }

    // Through an effect in blocks whose size changes as it goes (`blocks` empty: one sample at a time
    // through process()).
    static Stereo through(EffectBase& fx, const Stereo& in, const std::vector<int>& blocks)
    {
        Stereo out = in;
        const int n = (int) in.l.size();
        if (blocks.empty())
        {
            for (int i = 0; i < n; ++i) fx.process(out.l[(size_t) i], out.r[(size_t) i]);
            return out;
        }
        int at = 0, which = 0;
        while (at < n)
        {
            const int len = std::min(n - at, blocks[(size_t) which++ % blocks.size()]);
            fx.processBlock(out.l.data() + at, out.r.data() + at, len);
            at += len;
        }
        return out;
    }

    struct Pair { std::unique_ptr<EffectBase> old, fresh; };
    using Make = std::function<Pair()>;

    // Old and new made, set alike BEFORE prepare (as the app's send buses are) and prepared.
    template <typename Old, typename New, typename Set>
    static Make maker(double sr, Set set)
    {
        return [sr, set]
        {
            auto old = std::make_unique<Old>();
            auto fresh = std::make_unique<New>();
            set(*old);
            set(*fresh);
            old->prepare(sr, 256);
            fresh->prepare(sr, 256);
            return Pair { std::move(old), std::move(fresh) };
        };
    }

    void toTheBit(const juce::String& name, double sr, const Make& make, double seconds = 4.0)
    {
        const Stereo in = material(sr, seconds);
        const std::vector<std::vector<int>> ways = { { 256 }, { 64 }, { 1, 7, 33, 500, 4096, 2 }, {} };
        Stereo reference;
        for (size_t w = 0; w < ways.size(); ++w)
        {
            if (ways[w].empty() && seconds > 2.5) continue;        // (one sample at a time: the short ones)
            Pair p = make();
            const Stereo a = through(*p.old, in, ways[w]);
            const Stereo b = through(*p.fresh, in, ways[w]);
            const juce::String how = ways[w].empty() ? "sample by sample" : "blocks of " + juce::String(ways[w][0]) + (ways[w].size() > 1 ? ".." : "");
            expect(rms(a.l) > 1.0e-3 && rms(a.r) > 1.0e-3, name + ": the old one is heard");
            const bool exact = same(a, b);
            if (exact) expect(true);
            else expect(false, name + " at " + juce::String(sr) + ", " + how + ": NEW is not OLD to the bit (difference "
                               + juce::String(differenceDb(b, a), 1) + " dB)");
            if (w == 0 && ! exact)
                for (size_t i = 0; i < a.l.size(); ++i)
                    if (a.l[i] != b.l[i] || a.r[i] != b.r[i])
                    {
                        logMessage("   first difference at " + juce::String((int) i) + ": old " + juce::String(a.l[i], 12) + " " + juce::String(a.r[i], 12)
                                   + " new " + juce::String(b.l[i], 12) + " " + juce::String(b.r[i], 12) + " in " + juce::String(in.l[i], 12));
                        break;
                    }
            if (w == 0) reference = b;
            else expect(same(reference, b), name + ", " + how + ": the block size changes the sound");
        }
    }

    void runTest() override
    {
        testToTheBit();
        testOnPurpose();
        testCells();
        testDelayCells();
        testReverbCells();
        testTurns();
        testBounds();
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_E7_COST", {}).isNotEmpty())
            measureCost();
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("NEPTUNE_E7_RENDER", {}); folder.isNotEmpty())
            renderRecordings(juce::File(folder));
    }

    // ---- 1. to the bit ---------------------------------------------------------------------------
    void testToTheBit()
    {
        beginTest("DLY, RVB and HALL give the samples the old classes gave, to the bit (new cells at rest)");
        for (double sr : { 48000.0, 44100.0 })
        {
            // The send delay as the app makes it, and as a fresh insert; ping-pong; a short and a long time.
            toTheBit("DLY send", sr, maker<OldDelay, Delay>(sr, [](auto& d) { d.setTime(0.375f); d.setFeedback(0.35f); d.setMix(1.0f); }));
            toTheBit("DLY as made", sr, maker<OldDelay, Delay>(sr, [](auto&) {}));
            toTheBit("DLY PING", sr, maker<OldDelay, Delay>(sr, [](auto& d) { d.setParameter(3, 1.0f); d.setParameter(1, 0.8f); d.setParameter(0, 0.25f); d.setMix(0.5f); }));
            toTheBit("DLY 10 ms", sr, maker<OldDelay, Delay>(sr, [](auto& d) { d.setParameter(0, 0.01f); d.setParameter(1, 0.95f); d.setMix(0.3f); }), 2.0);
            toTheBit("DLY 2 s", sr, maker<OldDelay, Delay>(sr, [](auto& d) { d.setParameter(0, 2.0f); d.setParameter(1, 0.6f); }), 7.0);

            toTheBit("RVB send", sr, maker<OldReverb, Reverb>(sr, [](auto& r) { r.setDecay(0.6f); r.setDamping(0.4f); r.setMix(1.0f); }));
            toTheBit("RVB as made", sr, maker<OldReverb, Reverb>(sr, [](auto&) {}));
            toTheBit("RVB long dark large", sr, maker<OldReverb, Reverb>(sr, [](auto& r) { r.setParameter(0, 0.99f); r.setParameter(1, 0.9f); r.setParameter(2, 2.0f); r.setParameter(3, 0.5f); }));
            toTheBit("RVB short bright small", sr, maker<OldReverb, Reverb>(sr, [](auto& r) { r.setParameter(0, 0.2f); r.setParameter(1, 0.0f); r.setParameter(2, 0.25f); r.setParameter(3, 0.7f); }), 2.0);
            toTheBit("RVB size 1.37", sr, maker<OldReverb, Reverb>(sr, [](auto& r) { r.setParameter(2, 1.37f); r.setPreDelay(63.0f); r.setHighCut(4000.0f); }));

            toTheBit("HALL as made", sr, maker<OldHallReverb, HallReverb>(sr, [](auto&) {}));
            toTheBit("HALL as the screen sets it", sr, maker<OldHallReverb, HallReverb>(sr, [](auto& h)
                     { h.setParameter(0, 2.5f); h.setParameter(1, 1.0f); h.setParameter(2, 0.4f); h.setParameter(3, 10.0f); h.setMix(0.35f); }));
            toTheBit("HALL long large", sr, maker<OldHallReverb, HallReverb>(sr, [](auto& h)
                     { h.setParameter(0, 10.0f); h.setParameter(1, 2.0f); h.setParameter(2, 0.1f); h.setParameter(3, 100.0f); h.setMix(0.5f); }), 6.0);
            toTheBit("HALL short small damped", sr, maker<OldHallReverb, HallReverb>(sr, [](auto& h)
                     { h.setParameter(0, 0.5f); h.setParameter(1, 0.25f); h.setParameter(2, 1.0f); h.setParameter(3, 0.0f); }), 2.0);
            toTheBit("HALL still (MOD 0)", sr, maker<OldHallReverb, HallReverb>(sr, [](auto& h) { h.setParameter(4, 0.0f); h.setParameter(1, 1.61f); }));
            toTheBit("HALL MOD 1", sr, maker<OldHallReverb, HallReverb>(sr, [](auto& h) { h.setParameter(4, 1.0f); h.setParameter(5, 0.8f); }));
        }

        beginTest("Set after prepare and before any sound, a cell is where it is set (nothing glides in an empty line): still to the bit");
        {
            const double sr = 48000.0;
            const Stereo in = material(sr, 3.0);
            OldDelay od; Delay nd;
            od.prepare(sr, 256); nd.prepare(sr, 256);
            for (EffectBase* d : { (EffectBase*) &od, (EffectBase*) &nd }) { d->setParameter(0, 0.11f); d->setParameter(1, 0.7f); d->setParameter(3, 1.0f); d->setMix(0.6f); }
            expect(same(through(od, in, { 256 }), through(nd, in, { 256 })), "DLY");
            OldReverb orv; Reverb nrv;
            orv.prepare(sr, 256); nrv.prepare(sr, 256);
            for (EffectBase* r : { (EffectBase*) &orv, (EffectBase*) &nrv }) { r->setParameter(0, 0.9f); r->setParameter(1, 0.2f); r->setParameter(2, 0.6f); r->setParameter(3, 0.8f); }
            expect(same(through(orv, in, { 256 }), through(nrv, in, { 256 })), "RVB");
            OldHallReverb oh; HallReverb nh;
            oh.prepare(sr, 256); nh.prepare(sr, 256);
            for (EffectBase* h : { (EffectBase*) &oh, (EffectBase*) &nh }) { h->setParameter(0, 6.0f); h->setParameter(1, 0.7f); h->setParameter(2, 0.3f); h->setParameter(3, 35.0f); h->setMix(0.4f); }
            expect(same(through(oh, in, { 256 }), through(nh, in, { 256 })), "HALL");
            // reset() puts them back to nothing sounding, and the same again comes out.
            od.reset(); nd.reset(); orv.reset(); nrv.reset(); oh.reset(); nh.reset();
            // (The old delay's reset() left its loop filter where the last sound had put it, and the
            // first repeats after a reset carried that: the new one starts from nothing.)
            {
                const Stereo a = through(od, in, { 64 }), b = through(nd, in, { 64 });
                expect(differenceDb(b, a) < -40.0, "DLY after reset: " + juce::String(differenceDb(b, a), 1) + " dB");
                logMessage("   DLY after a reset under sound: the old one's leftover filter state is " + juce::String(differenceDb(b, a), 1) + " dB of what follows");
                Delay made;
                for (EffectBase* d : { (EffectBase*) &made }) { d->setParameter(0, 0.11f); d->setParameter(1, 0.7f); d->setParameter(3, 1.0f); d->setMix(0.6f); }
                made.prepare(sr, 256);
                expect(same(through(made, in, { 64 }), b), "DLY after reset is DLY as made");
            }
            // A time between two samples: where its place is rounded to depends on how many samples
            // have been written (Core's ring position), and reset() begins that count again.
            {
                Delay played, made;
                for (Delay* d : { &played, &made }) { d->setParameter(0, 0.1101f); d->setParameter(1, 0.7f); d->prepare(sr, 256); }
                through(played, material(sr, 1.37), { 256 });
                played.reset();
                expect(same(through(played, in, { 64 }), through(made, in, { 64 })), "DLY at a time between samples: after reset it is DLY as made");
            }
            // The loop filter's corners (not cells): both set before prepare; one of them alone
            // after it, before any sound.
            for (int when = 0; when < 3; ++when)
            {
                OldDelay oc; Delay nc;
                for (EffectBase* d : { (EffectBase*) &oc, (EffectBase*) &nc }) d->setParameter(1, 0.8f);
                if (when == 0) { oc.setHighCut(2000.0f); oc.setLowCut(300.0f); nc.setHighCut(2000.0f); nc.setLowCut(300.0f); }
                oc.prepare(sr, 256); nc.prepare(sr, 256);
                if (when == 1) { oc.setHighCut(2000.0f); nc.setHighCut(2000.0f); }
                if (when == 2) { oc.setLowCut(300.0f); nc.setLowCut(300.0f); }
                const Stereo a = through(oc, in, { 256 }), b = through(nc, in, { 256 });
                const char* const how[] = { "both set before prepare", "the high cut set after prepare", "the low cut set after prepare" };
                expect(same(a, b), juce::String("DLY with other corners, ") + how[when]);
                Delay plain;
                plain.setParameter(1, 0.8f);
                plain.prepare(sr, 256);
                expect(differenceDb(through(plain, in, { 256 }), b) > -30.0, juce::String("the corners are heard, ") + how[when]);
            }
            expect(same(through(orv, in, { 64 }), through(nrv, in, { 64 })), "RVB after reset");
            expect(same(through(oh, in, { 64 }), through(nh, in, { 64 })), "HALL after reset");
        }
    }

    // ---- 2. what differs on purpose, measured -----------------------------------------------------
    struct Row { juce::String name, verdict, numbers; };
    std::vector<Row> table;

    void row(const juce::String& name, const juce::String& verdict, const juce::String& numbers)
    {
        table.push_back({ name, verdict, numbers });
        logMessage("   " + name.paddedRight(' ', 34) + verdict.paddedRight(' ', 7) + numbers);
    }

    // Old against new for one setting: the difference in dB, and the pair kept for the atlas.
    template <typename Old, typename New, typename Set>
    double compare(const juce::String& name, double sr, Set set, double seconds = 4.0)
    {
        const Stereo in = material(sr, seconds);
        Pair p = maker<Old, New>(sr, set)();
        const Stereo a = through(*p.old, in, { 256 }), b = through(*p.fresh, in, { 256 });
        const bool exact = same(a, b);
        const double diff = exact ? -1000.0 : differenceDb(b, a);
        const double level = exact ? 0.0 : dB(rms(b.l) / std::max(1.0e-30, rms(a.l)));
        row(name, exact ? "MATCH" : (diff < -60.0 ? "CLOSE" : "MOVED"),
            exact ? "bit-identical" : "difference " + juce::String(diff, 1) + " dB, level " + juce::String(level, 2) + " dB");
        keepPair(name, sr, a, b);
        return diff;
    }

    void testOnPurpose()
    {
        beginTest("Times between samples and 96 kHz, to the bit; what differs from the old classes where their arrays were too short, measured");
        using D = OldDelay; using R = OldReverb; using H = OldHallReverb;
        // The settings of the bit test once more, for the table and the atlas.
        expect(compare<D, Delay>("dly_send_48k", 48000.0, [](auto& d) { d.setTime(0.375f); d.setFeedback(0.35f); d.setMix(1.0f); }) < -900.0);
        expect(compare<D, Delay>("dly_ping_48k", 48000.0, [](auto& d) { d.setParameter(3, 1.0f); d.setParameter(1, 0.8f); d.setParameter(0, 0.25f); d.setMix(0.5f); }) < -900.0);
        expect(compare<D, Delay>("dly_tape_mode_off_48k", 48000.0, [](auto& d) { d.setParameter(0, 0.3f); d.setParameter(1, 0.7f); }) < -900.0);
        expect(compare<R, Reverb>("rvb_send_48k", 48000.0, [](auto& r) { r.setDecay(0.6f); r.setDamping(0.4f); r.setMix(1.0f); }) < -900.0);
        expect(compare<R, Reverb>("rvb_long_dark_large_48k", 48000.0, [](auto& r) { r.setParameter(0, 0.99f); r.setParameter(1, 0.9f); r.setParameter(2, 2.0f); r.setParameter(3, 0.5f); }, 8.0) < -900.0);
        expect(compare<H, HallReverb>("hall_as_made_48k", 48000.0, [](auto&) {}) < -900.0);
        expect(compare<H, HallReverb>("hall_long_large_48k", 48000.0, [](auto& h) { h.setParameter(0, 10.0f); h.setParameter(1, 2.0f); h.setParameter(2, 0.1f); h.setMix(0.5f); }, 8.0) < -900.0);

        // A time that is not a whole or a half sample. The old delay worked its read place out as
        // a float in a ring of 192,000, and which float a place is rounded to depends on the ring:
        // in the line's own ring the new delay was 44 to 74 dB away from the old one for such
        // times (the review's measurement, 20 s of chords, a sweep and noise), from the first
        // echo or from where a ring first came round (2.7 s for the line, 4 s for the old ring at
        // 48 kHz). The place is worked out in the old ring (Delay::RING), so these are long enough
        // to pass both several times.
        expect(compare<D, Delay>("dly_time_0.3333s_48k", 48000.0, [](auto& d) { d.setParameter(0, 0.33333f); d.setParameter(1, 0.7f); }, 12.0) < -900.0);
        expect(compare<D, Delay>("dly_time_0.0707s_44k", 44100.0, [](auto& d) { d.setParameter(0, 0.0707f); d.setParameter(1, 0.9f); }, 12.0) < -900.0);
        // (A dotted eighth at 133 and at 97 bpm, as a delay that follows the tempo is set; ping-pong; the longest time at 96 kHz.)
        expect(compare<D, Delay>("dly_dotted_8th_133bpm_48k", 48000.0, [](auto& d) { d.setParameter(0, 60.0f / 133.0f * 0.75f); d.setParameter(1, 0.6f); d.setMix(0.5f); }, 12.0) < -900.0);
        expect(compare<D, Delay>("dly_dotted_8th_97bpm_ping_44k", 44100.0, [](auto& d) { d.setParameter(0, 60.0f / 97.0f * 0.75f); d.setParameter(1, 0.8f); d.setParameter(3, 1.0f); }, 12.0) < -900.0);
        expect(compare<D, Delay>("dly_1.99999s_96k", 96000.0, [](auto& d) { d.setParameter(0, 1.99999f); d.setParameter(1, 0.9f); }, 9.0) < -900.0);
        expect(compare<D, Delay>("dly_2s_96k", 96000.0, [](auto& d) { d.setParameter(0, 2.0f); d.setParameter(1, 0.9f); }, 9.0) < -900.0);

        // 96 kHz: everything still fits the old fixed arrays at SIZE 1. The hall's lines are twice
        // as long there (room for SIZE 2), but its heads' places are worked out in the old ring of
        // 32,768 while the lines in use fit one (SIZE up to 1.44 at 96 kHz).
        expect(compare<D, Delay>("dly_send_96k", 96000.0, [](auto& d) { d.setTime(0.375f); d.setFeedback(0.35f); }) < -900.0);
        expect(compare<R, Reverb>("rvb_send_96k", 96000.0, [](auto& r) { r.setDecay(0.6f); r.setDamping(0.4f); }) < -900.0);
        expect(compare<H, HallReverb>("hall_as_made_96k", 96000.0, [](auto&) {}) < -900.0);
        expect(compare<H, HallReverb>("hall_size1.44_mod1_96k", 96000.0, [](auto& h) { h.setParameter(1, 1.44f); h.setParameter(4, 1.0f); h.setParameter(0, 8.0f); }) < -900.0);
        expect(compare<H, HallReverb>("hall_small_88k", 88200.0, [](auto& h) { h.setParameter(1, 0.41f); h.setParameter(3, 7.0f); }) < -900.0);
        // Where the old arrays were too short, the old classes held every length to the array (the
        // lines lost their proportions); the new lines are as long as the rate needs. Not the same sound, on purpose.
        compare<D, Delay>("dly_2s_192k", 192000.0, [](auto& d) { d.setParameter(0, 2.0f); d.setParameter(1, 0.5f); }, 6.0);
        compare<R, Reverb>("rvb_size2_96k", 96000.0, [](auto& r) { r.setParameter(2, 2.0f); });
        compare<H, HallReverb>("hall_size2_96k", 96000.0, [](auto& h) { h.setParameter(1, 2.0f); });

        beginTest("DLY at 192 kHz: TIME 2 s is 2 s (the old array held it to 1 s)");
        {
            const double sr = 192000.0;
            Stereo in { Buffer((size_t) (3.0 * sr), 0.0f), Buffer((size_t) (3.0 * sr), 0.0f) };
            in.l[10] = in.r[10] = 1.0f;
            Pair p = maker<OldDelay, Delay>(sr, [](auto& d) { d.setParameter(0, 2.0f); d.setParameter(1, 0.0f); d.setMix(1.0f); })();
            const Stereo a = through(*p.old, in, { 4096 }), b = through(*p.fresh, in, { 4096 });
            auto loudest = [](const Buffer& x) { size_t at = 0; for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > std::abs(x[at])) at = i; return (double) at; };
            expectWithinAbsoluteError(loudest(b.l) / sr, 2.0, 0.001);
            expectWithinAbsoluteError(loudest(a.l) / sr, 1.0, 0.001);
        }
    }

    // ---- 3. the page convention ------------------------------------------------------------------
    void testCells()
    {
        beginTest("Eight cells each, labels that fit, set and read back, held to their range; the names and numbers the app uses");
        Delay d; Reverb r; HallReverb h;
        struct Named { EffectBase* fx; const char* name; const char* labels[8]; };
        const Named all[] = { { &d, "DLY", { "TIME", "FDBK", "MIX", "MODE", "READ", "DRIV", "SAT", "AGE" } },
                              { &r, "RVB", { "DCAY", "DAMP", "SIZE", "MIX", "PDLY", "SHIM", "MODE", "HOLD" } },
                              { &h, "HALL", { "DCAY", "SIZE", "DAMP", "PDLY", "MOD", "SHIM", "MODE", "HOLD" } } };
        for (auto& e : all)
            for (int c = 0; c < 8; ++c)
            {
                const ParamSpec spec = c < 4 ? e.fx->getParamSpec(c) : e.fx->getExtraSpec(c - 4);
                const juce::String what = juce::String(e.name) + " cell " + juce::String(c);
                expect(spec.isValid(), what);
                expectEquals(juce::String(spec.label), juce::String(e.labels[c]), what);
                expectEquals(spec.id, c % 4, what);
                auto set = [&](float v) { if (c < 4) e.fx->setParameter(c, v); else e.fx->setExtra(c - 4, v); };
                auto get = [&] { return c < 4 ? e.fx->getParameter(c) : e.fx->getExtra(c - 4); };
                set(spec.maxVal + 100.0f);
                expectEquals(get(), spec.maxVal, what + " held at the top");
                set(spec.minVal - 100.0f);
                expectEquals(get(), spec.minVal, what + " held at the bottom");
                set(std::numeric_limits<float>::quiet_NaN());
                expectEquals(get(), spec.minVal, what + ": what is not a number is not taken");
                const float mid = spec.isDiscrete() ? spec.maxVal : spec.minVal + 0.37f * (spec.maxVal - spec.minVal);
                set(mid);
                expectWithinAbsoluteError(get(), mid, 1.0e-5f, what);
            }
        expect(d.canSync() && d.getSyncParamIndex() == 0, "DLY's TIME follows the tempo");
        expect(! r.canSync() && ! h.canSync());

        // What a fresh object is, as before: not what its cells call their defaults.
        Delay d2; Reverb r2; HallReverb h2;
        expectEquals(d2.getParameter(0), 0.375f); expectEquals(d2.getParameter(1), 0.4f); expectEquals(d2.getMix(), 1.0f);
        expectEquals(r2.getParameter(0), 0.7f); expectEquals(r2.getParameter(1), 0.5f); expectEquals(r2.getParameter(2), 1.0f); expectEquals(r2.getMix(), 1.0f);
        expectEquals(r2.getExtra(0), 10.0f);
        expectEquals(h2.getParameter(0), 3.0f); expectEquals(h2.getParameter(2), 0.5f); expectEquals(h2.getParameter(3), 20.0f);
        expectEquals(h2.getExtra(0), 0.3f); expectEquals(h2.getMix(), 1.0f);
        // The hall's old numbers 4 and 5.
        h2.setParameter(HallReverb::Modulation, 0.8f);
        expectEquals(h2.getExtra(HallReverb::Mod), 0.8f);
        expectEquals(h2.getParameter(HallReverb::Modulation), 0.8f);
        h2.setParameter(HallReverb::Mix, 0.6f);
        expectEquals(h2.getMix(), 0.6f);
        // The setters by name.
        d2.setTime(0.5f); d2.setFeedback(0.2f); d2.setMode(Delay::Mode::PingPong);
        expectEquals(d2.getParameter(0), 0.5f); expectEquals(d2.getParameter(1), 0.2f); expectEquals(d2.getParameter(3), 1.0f);
        r2.setDecay(0.3f); r2.setDamping(0.9f); r2.setSize(1.5f); r2.setPreDelay(40.0f);
        expectEquals(r2.getParameter(0), 0.3f); expectEquals(r2.getParameter(1), 0.9f); expectEquals(r2.getParameter(2), 1.5f); expectEquals(r2.getExtra(0), 40.0f);

        beginTest("Every cell that is not a list is a modulation destination: an offset sounds as the cell set there");
        {
            const double sr = 48000.0;
            const Stereo in = material(sr, 2.0);
            auto check = [&](const char* name, std::function<std::unique_ptr<EffectBase>()> make, int cell, float from, float to, float lo, float hi)
            {
                auto set = [&](EffectBase& fx, float v) { if (cell < 4) fx.setParameter(cell, v); else fx.setExtra(cell - 4, v); };
                auto a = make(), b = make(), c = make();
                set(*a, to);
                set(*b, from);
                set(*c, from);
                b->modulateCell(cell, (to - from) / (hi - lo));
                for (auto* fx : { a.get(), b.get(), c.get() }) fx->prepare(sr, 256);
                const Stereo sa = through(*a, in, { 256 }), sb = through(*b, in, { 256 }), sc = through(*c, in, { 256 });
                expect(differenceDb(sb, sa) < -80.0, juce::String(name) + ": modulated is not the cell set there (" + juce::String(differenceDb(sb, sa), 1) + " dB)");
                expect(differenceDb(sb, sc) > -40.0, juce::String(name) + ": the offset does nothing");
            };
            check("DLY FDBK", [] { return std::make_unique<Delay>(); }, 1, 0.2f, 0.675f, 0.0f, 0.95f);
            check("DLY AGE", [] { return std::make_unique<Delay>(); }, 7, 0.0f, 0.5f, 0.0f, 1.0f);
            check("RVB DCAY", [] { return std::make_unique<Reverb>(); }, 0, 0.3f, 0.795f, 0.0f, 0.99f);
            check("RVB SIZE", [] { return std::make_unique<Reverb>(); }, 2, 0.6f, 1.475f, 0.25f, 2.0f);
            check("RVB SHIM", [] { return std::make_unique<Reverb>(); }, 5, 0.0f, 0.5f, 0.0f, 1.0f);
            check("HALL DAMP", [] { return std::make_unique<HallReverb>(); }, 2, 0.1f, 0.6f, 0.0f, 1.0f);
            check("HALL MOD", [] { return std::make_unique<HallReverb>(); }, 4, 0.0f, 1.0f, 0.0f, 1.0f);
        }
    }

    // ---- 4. the cells the cores gave them ---------------------------------------------------------
    // How bright: the level of the sample-to-sample difference against the level (it rises with frequency).
    static double brightness(const Buffer& x, size_t from, size_t to)
    {
        to = std::min(to, x.size());
        double d = 0.0, s = 0.0;
        for (size_t i = from + 1; i < to; ++i) { d += std::pow((double) x[i] - x[i - 1], 2.0); s += (double) x[i] * x[i]; }
        return std::sqrt(d / std::max(s, 1.0e-30));
    }
    static Stereo toneBurst(double sr, double seconds, double hz, double burst, float level = 0.4f)
    {
        const size_t n = (size_t) (seconds * sr);
        Stereo s { Buffer(n, 0.0f), Buffer(n, 0.0f) };
        for (size_t i = 0; i < (size_t) (burst * sr) && i < n; ++i)
        {
            const double t = (double) i / sr, fade = std::min(1.0, std::min(t, burst - t) / 0.01);
            s.l[i] = s.r[i] = level * (float) (fade * std::sin(2.0 * juce::MathConstants<double>::pi * hz * t));
        }
        return s;
    }

    void testDelayCells()
    {
        const double sr = 48000.0;
        auto at = [sr](double seconds) { return (size_t) (seconds * sr); };

        beginTest("DLY MODE: STER repeats on its own side, PING changes sides every time, TAPE is an unsteady driven STER");
        {
            Stereo in { Buffer(at(1.5), 0.0f), Buffer(at(1.5), 0.0f) };
            for (size_t i = 0; i < at(0.02); ++i) in.l[i] = 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 500.0 * (double) i / sr);
            auto run = [&](int mode)
            {
                Delay d;
                d.setParameter(0, 0.2f); d.setParameter(1, 0.8f); d.setParameter(3, (float) mode); d.setMix(1.0f);
                d.prepare(sr, 256);
                return through(d, in, { 256 });
            };
            const Stereo ster = run(0), ping = run(1), tape = run(2);
            // Echo 1 at 0.2 s, echo 2 at 0.4 s, echo 3 at 0.6 s.
            auto level = [&](const Buffer& x, int echo) { return rms(x, at(0.2 * echo), at(0.2 * echo + 0.05)); };
            expect(level(ster.l, 1) > 0.05 && level(ster.r, 1) < 1.0e-6 && level(ster.l, 2) > 0.03 && level(ster.r, 2) < 1.0e-6, "STER stays left");
            expect(level(ping.l, 1) > 0.05 && level(ping.r, 1) < 1.0e-6, "PING: the first echo where the sound was");
            expect(level(ping.r, 2) > 0.03 && level(ping.l, 2) < 1.0e-6, "PING: the second on the other side");
            expect(level(ping.l, 3) > 0.02 && level(ping.r, 3) < 1.0e-6, "PING: the third back again");
            expect(differenceDb(tape, ster) > -40.0, "TAPE is not STER any more (" + juce::String(differenceDb(tape, ster), 1) + " dB)");
            expect(level(tape.l, 1) > 0.03 && level(tape.r, 1) < 1.0e-6, "TAPE repeats on its own side");
        }

        beginTest("DLY MODE turned under sound: the feedback crosses over, it does not jump");
        {
            // A steady tone in the loop on the left only: when the sides are exchanged the right
            // side's line gets the left's repeats; the step from one sample to the next stays a tone's.
            Stereo in { Buffer(at(2.0), 0.0f), Buffer(at(2.0), 0.0f) };
            for (size_t i = 0; i < in.l.size(); ++i) in.l[i] = 0.4f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 110.0 * (double) i / sr);
            Delay d;
            d.setParameter(0, 0.1f); d.setParameter(1, 0.9f); d.setMix(1.0f);
            d.prepare(sr, 256);
            Stereo head { Buffer(in.l.begin(), in.l.begin() + (long) at(1.0)), Buffer(in.r.begin(), in.r.begin() + (long) at(1.0)) };
            Stereo rest { Buffer(in.l.begin() + (long) at(1.0), in.l.end()), Buffer(in.r.begin() + (long) at(1.0), in.r.end()) };
            const Stereo before = through(d, head, { 256 });
            d.setParameter(3, 1.0f);
            const Stereo after = through(d, rest, { 256 });
            expect(rms(before.r) < 1.0e-6 && rms(after.r, at(0.5), at(1.0)) > 0.05, "the right side gets the repeats");
            const float steady = largestStep(before.l, at(0.5), at(1.0));
            // (Measured 0.73 times the steady step; with the feedback exchanged at one sample, half way, 2.5 times.)
            expect(largestStep(after.l, 0, at(0.3)) < 1.5f * steady && largestStep(after.r, 0, at(0.3)) < 1.5f * steady,
                   "steps " + juce::String(largestStep(after.l, 0, at(0.3)), 5) + " / " + juce::String(largestStep(after.r, 0, at(0.3)), 5) + " against " + juce::String(steady, 5));
        }

        beginTest("DLY READ: REV plays each stretch backwards, OCT+ an octave up; DRIV holds a loud loop; AGE makes the pitch waver");
        {
            // A burst that rises in pitch: forwards its echo rises, backwards it falls.
            Stereo sweep { Buffer(at(1.2), 0.0f), Buffer(at(1.2), 0.0f) };
            double phase = 0.0;
            for (size_t i = 0; i < at(0.2); ++i)
            {
                const double t = (double) i / sr;
                phase += 2.0 * juce::MathConstants<double>::pi * (200.0 + 4000.0 * t) / sr;
                sweep.l[i] = sweep.r[i] = 0.4f * (float) (std::min(1.0, std::min(t, 0.2 - t) / 0.01) * std::sin(phase));
            }
            auto run = [&](const Stereo& in, std::function<void(Delay&)> set)
            {
                Delay d;
                d.setParameter(0, 0.3f); d.setParameter(1, 0.0f); d.setMix(1.0f);
                set(d);
                d.prepare(sr, 256);
                return through(d, in, { 256 });
            };
            const Stereo fwd = run(sweep, [](Delay&) {}), rev = run(sweep, [](Delay& d) { d.setExtra(Delay::Read, (float) dly::read::Reverse); });
            auto rising = [&](const Stereo& s, double from)
            {
                return brightness(s.l, at(from + 0.11), at(from + 0.19)) / std::max(1.0e-9, brightness(s.l, at(from + 0.01), at(from + 0.09)));
            };
            expect(rising(fwd, 0.3) > 1.5, "forwards the echo rises (" + juce::String(rising(fwd, 0.3), 2) + ")");
            // (Backwards, the stretch 0 .. 0.3 s is heard from 0.3 s on, its end first.)
            expect(rms(rev.l, at(0.3), at(0.65)) > 0.03, "REV is heard");
            const double early = brightness(rev.l, at(0.42), at(0.48)), late = brightness(rev.l, at(0.52), at(0.58));
            expect(early > 1.3 * late, "backwards it falls (" + juce::String(early, 3) + " then " + juce::String(late, 3) + ")");

            const Stereo tone = toneBurst(sr, 1.0, 220.0, 0.25);
            const Stereo same8 = run(tone, [](Delay&) {}), up = run(tone, [](Delay& d) { d.setExtra(Delay::Read, (float) dly::read::OctaveUp); });
            const double plain = brightness(same8.l, at(0.35), at(0.5)), shifted = brightness(up.l, at(0.35), at(0.5));
            expect(shifted > 1.7 * plain && shifted < 2.6 * plain, "OCT+ is about twice as bright (" + juce::String(shifted / plain, 2) + ")");

            // FDBK at the top and a loud sound: clean, the loop carries what it is given; driven, the stage holds it.
            Stereo loud { Buffer(at(3.0), 0.0f), Buffer(at(3.0), 0.0f) };
            for (size_t i = 0; i < at(2.0); ++i) loud.l[i] = loud.r[i] = 0.9f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 400.0 * (double) i / sr);
            auto fb = [](Delay& d) { d.setParameter(0, 0.05f); d.setParameter(1, 0.95f); };
            const Stereo clean = run(loud, fb), driven = run(loud, [&](Delay& d) { fb(d); d.setExtra(Delay::Drive, 1.0f); });
            expect(peak(clean.l) > 3.0f, "clean it adds up (" + juce::String(peak(clean.l), 2) + ")");
            expect(peak(driven.l) < 0.6f * peak(clean.l), "driven it is held (" + juce::String(peak(driven.l), 2) + ")");
            const Stereo other = run(loud, [&](Delay& d) { fb(d); d.setExtra(Delay::Drive, 1.0f); d.setExtra(Delay::Sat, (float) sat::Fold); });
            expect(differenceDb(other, driven) > -30.0, "SAT chooses the curve");

            Stereo steady { Buffer(at(3.0), 0.0f), Buffer(at(3.0), 0.0f) };
            for (size_t i = 0; i < steady.l.size(); ++i) steady.l[i] = steady.r[i] = 0.4f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / sr);
            const Stereo still = run(steady, [](Delay&) {}), aged = run(steady, [](Delay& d) { d.setExtra(Delay::Age, 1.0f); });
            expect(differenceDb(aged, still) > -20.0, "AGE moves the repeat (" + juce::String(differenceDb(aged, still), 1) + " dB)");
            expectWithinAbsoluteError(dB(rms(aged.l, at(1.0), at(3.0)) / rms(still.l, at(1.0), at(3.0))), 0.0, 1.0, "and leaves its level");
        }
    }

    static float largestStep(const Buffer& x, size_t from, size_t to)
    {
        to = std::min(to, x.size());
        float s = 0.0f;
        for (size_t i = from + 1; i < to; ++i) s = std::max(s, std::abs(x[i] - x[i - 1]));
        return s;
    }

    template <typename Fx>
    void reverbCells(const juce::String& name, int shimCell, int holdCell, int dampCell, std::function<void(Fx&, float)> setPreDelay)
    {
        const double sr = 48000.0;
        auto at = [sr](double seconds) { return (size_t) (seconds * sr); };
        auto make = [&](std::function<void(Fx&)> set)
        {
            auto fx = std::make_unique<Fx>();
            fx->setMix(1.0f);
            set(*fx);
            fx->prepare(sr, 256);
            return fx;
        };

        beginTest(name + " PDLY: the reverb answers that much later");
        {
            Stereo click { Buffer(at(0.5), 0.0f), Buffer(at(0.5), 0.0f) };
            click.l[0] = click.r[0] = 1.0f;
            auto first = [&](float ms)
            {
                auto fx = make([&](Fx& f) { setPreDelay(f, ms); });
                const Stereo out = through(*fx, click, { 256 });
                for (size_t i = 0; i < out.l.size(); ++i)
                    if (std::abs(out.l[i]) > 1.0e-6f || std::abs(out.r[i]) > 1.0e-6f) return (double) i / sr;
                return -1.0;
            };
            const double none = first(0.0f), late = first(80.0f);
            expect(none >= 0.0 && none < 0.3, name + " answers (" + juce::String(none, 4) + " s)");
            expectWithinAbsoluteError(late - none, 0.080, 0.0005);
        }

        beginTest(name + " SHIM: what comes round comes round higher (MODE: the interval); at 0 it is not there");
        {
            const Stereo tone = toneBurst(sr, 4.0, 220.0, 1.0);
            auto run = [&](float shim, int mode)
            {
                auto fx = make([&](Fx& f) { f.setExtra(shimCell, shim); f.setExtra(shimCell + 1, (float) mode); });
                return through(*fx, tone, { 256 });
            };
            const Stereo dry = run(0.0f, 0), oct = run(0.8f, rvb::shift::Octave), sub = run(0.8f, rvb::shift::Sub);
            const double b0 = brightness(dry.l, at(1.5), at(3.0)), b1 = brightness(oct.l, at(1.5), at(3.0)), b2 = brightness(sub.l, at(1.5), at(3.0));
            expect(b1 > 1.4 * b0, name + ": an octave up is brighter (" + juce::String(b1 / b0, 2) + ")");
            expect(b2 < 0.85 * b0, name + ": an octave down is darker (" + juce::String(b2 / b0, 2) + ")");
            expect(finite(oct.l) && peak(oct.l) < 8.0f && peak(sub.l) < 8.0f);

            // MODE turned under sound: the shimmer goes out, the heads are exchanged, it comes back at the other interval.
            auto fx = make([&](Fx& f) { f.setExtra(shimCell, 0.8f); f.setExtra(shimCell + 1, (float) rvb::shift::Octave); });
            Stereo turned = tone;
            const int block = 256, n = (int) tone.l.size();
            for (int pos = 0; pos < n; pos += block)
            {
                if (pos == (int) at(0.3) / block * block) fx->setExtra(shimCell + 1, (float) rvb::shift::Sub);
                fx->processBlock(turned.l.data() + pos, turned.r.data() + pos, std::min(block, n - pos));
            }
            const double b3 = brightness(turned.l, at(1.5), at(3.0));
            expect(b3 < 0.5 * b1, name + ": MODE turned to SUB under sound is not the sound of OCT any more (" + juce::String(b3 / b1, 2) + " of its brightness)");
            expect(largestStep(turned.l, at(0.25), at(0.6)) < 4.0f * largestStep(oct.l, at(0.25), at(0.6)), name + ": and it does not click");
        }

        beginTest(name + " HOLD: what is in the reverb stays until it is let go, and nothing new gets in");
        {
            const Stereo in = material(sr, 9.0, 3);
            auto run = [&](bool hold, bool letGo)
            {
                auto fx = make([](Fx&) {});
                Stereo out = in;
                const int block = 256, n = (int) in.l.size();
                for (int pos = 0; pos < n; pos += block)
                {
                    if (hold && pos == (int) at(0.6) / block * block) fx->setExtra(holdCell, 1.0f);
                    if (letGo && pos == (int) at(5.0) / block * block) fx->setExtra(holdCell, 0.0f);
                    fx->processBlock(out.l.data() + pos, out.r.data() + pos, std::min(block, n - pos));
                }
                return out;
            };
            // (The input goes on in bursts all the time: held, none of it is heard.)
            Stereo silentAfter = in;
            for (size_t i = at(0.6); i < silentAfter.l.size(); ++i) silentAfter.l[i] = silentAfter.r[i] = 0.0f;
            const Stereo held = run(true, false), released = run(true, true);
            const double start = rms(held.l, at(0.7), at(1.2)), end = rms(held.l, at(8.0), at(8.9));
            expect(start > 1.0e-3 && end > 0.5 * start && end < 2.0 * start,
                   name + ": held it stays (" + juce::String(start, 5) + " then " + juce::String(end, 5) + ")");
            // Nothing new gets in: the held tail is the same whatever is played afterwards.
            {
                auto fx = make([](Fx&) {});
                Stereo out = silentAfter;
                const int block = 256, n = (int) out.l.size();
                for (int pos = 0; pos < n; pos += block)
                {
                    if (pos == (int) at(0.6) / block * block) fx->setExtra(holdCell, 1.0f);
                    fx->processBlock(out.l.data() + pos, out.r.data() + pos, std::min(block, n - pos));
                }
                Stereo a { Buffer(held.l.begin() + (long) at(1.0), held.l.end()), Buffer(held.r.begin() + (long) at(1.0), held.r.end()) };
                Stereo b { Buffer(out.l.begin() + (long) at(1.0), out.l.end()), Buffer(out.r.begin() + (long) at(1.0), out.r.end()) };
                expect(differenceDb(a, b) < -60.0, name + ": what is played while it holds leaks in (" + juce::String(differenceDb(a, b), 1) + " dB)");
            }
            expect(rms(released.l, at(4.0), at(4.9)) > 0.5 * start, name + ": still held before it is let go");
            // Held, DAMP takes nothing: with DAMP nearly closed the tail is as loud and as bright seven seconds on.
            {
                auto fx = make([&](Fx& f) { f.setParameter(dampCell, 0.9f); });
                // (The hall's moving heads take the top off a held tail by themselves: they stand still here.)
                if (auto* hall = dynamic_cast<HallReverb*> (static_cast<EffectBase*> (fx.get()))) hall->setExtra(HallReverb::Mod, 0.0f);
                Stereo out = in;
                const int block = 256, n = (int) out.l.size();
                for (int pos = 0; pos < n; pos += block)
                {
                    if (pos == (int) at(0.6) / block * block) fx->setExtra(holdCell, 1.0f);
                    fx->processBlock(out.l.data() + pos, out.r.data() + pos, std::min(block, n - pos));
                }
                const double level = rms(out.l, at(8.0), at(8.9)) / std::max(1.0e-12, rms(out.l, at(1.0), at(1.9)));
                const double bright = brightness(out.l, at(8.0), at(8.9)) / std::max(1.0e-12, brightness(out.l, at(1.0), at(1.9)));
                expect(level > 0.7 && level < 1.5 && bright > 0.7,
                       name + ": held with DAMP at 0.9 (level x " + juce::String(level, 2) + ", brightness x " + juce::String(bright, 2) + ")");
            }
            expect(finite(held.l) && peak(held.l) < 8.0f && peak(held.r) < 8.0f, name + ": held, it does not grow");
            logMessage("   " + name + " HOLD: level " + juce::String(dB(end / start), 2) + " dB after 8 s; let go, "
                       + juce::String(dB(rms(released.l, at(8.0), at(8.9)) / start), 1) + " dB 3 s later (new sound comes in again)");
        }
    }

    void testReverbCells()
    {
        reverbCells<Reverb>("RVB", Reverb::Shimmer, Reverb::Hold, Reverb::Damping, [](Reverb& r, float ms) { r.setExtra(Reverb::PreDelay, ms); });
        reverbCells<HallReverb>("HALL", HallReverb::Shimmer, HallReverb::Hold, HallReverb::Damping, [](HallReverb& h, float ms) { h.setParameter(HallReverb::PreDelay, ms); });

        beginTest("HALL MOD: 0 = the heads stand still, up = they move (the tail is chorused)");
        {
            const double sr = 48000.0;
            const Stereo in = material(sr, 3.0);
            auto run = [&](float mod)
            {
                HallReverb h;
                h.setExtra(HallReverb::Mod, mod);
                h.prepare(sr, 256);
                return through(h, in, { 256 });
            };
            const Stereo still = run(0.0f), some = run(0.3f), deep = run(1.0f);
            expect(differenceDb(some, still) > -30.0 && differenceDb(deep, some) > -30.0);
            expectWithinAbsoluteError(dB(rms(deep.l) / rms(still.l)), 0.0, 1.5);
        }
    }

    // ---- 5. turned under sound --------------------------------------------------------------------
    void testTurns()
    {
        beginTest("A cell turned under a steady tone is glided to: no step larger than a few of the tone's own (the old classes jumped)");
        const double sr = 48000.0;
        auto at = [sr](double seconds) { return (size_t) (seconds * sr); };
        Stereo tone { Buffer(at(3.0), 0.0f), Buffer(at(3.0), 0.0f) };
        for (size_t i = 0; i < tone.l.size(); ++i)
            tone.l[i] = tone.r[i] = 0.3f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 110.0 * (double) i / sr);

        auto turn = [&](EffectBase& fx, std::function<void(EffectBase&)> act)
        {
            fx.prepare(sr, 256);
            Stereo out = tone;
            const int block = 256, n = (int) out.l.size();
            for (int pos = 0; pos < n; pos += block)
            {
                if (pos == (int) at(1.5) / block * block) act(fx);
                fx.processBlock(out.l.data() + pos, out.r.data() + pos, std::min(block, n - pos));
            }
            struct Steps { float steady, turned; };
            return Steps { largestStep(out.l, at(1.0), at(1.45)), largestStep(out.l, at(1.45), at(3.0)) };
        };
        auto check = [&](const juce::String& name, EffectBase& old, EffectBase& fresh, std::function<void(EffectBase&)> act, float allowed)
        {
            const auto o = turn(old, act), f = turn(fresh, act);
            logMessage("   " + name.paddedRight(' ', 22) + "steady " + juce::String(f.steady, 5) + "   turned: new " + juce::String(f.turned, 5)
                       + " (x " + juce::String(f.turned / f.steady, 1) + "), old " + juce::String(o.turned, 5) + " (x " + juce::String(o.turned / o.steady, 1) + ")");
            expect(f.turned < allowed * f.steady, name + ": a step of " + juce::String(f.turned / f.steady, 1) + " times the tone's own");
        };
        { OldDelay o; Delay f; o.setMix(1.0f); f.setMix(1.0f);
          check("DLY TIME 0.375 -> 0.2", o, f, [](EffectBase& e) { e.setParameter(0, 0.2f); }, 6.0f); }
        { OldDelay o; Delay f;
          // (30,000 samples further back within a tenth of a second: the line is read backwards at
          // seven times its speed on the way, a tape spooling, and the tone is that much steeper.)
          check("DLY TIME 0.375 -> 1.0", o, f, [](EffectBase& e) { e.setParameter(0, 1.0f); }, 10.0f); }
        { OldReverb o; Reverb f;
          check("RVB SIZE 1 -> 0.5", o, f, [](EffectBase& e) { e.setParameter(2, 0.5f); }, 8.0f); }
        { OldReverb o; Reverb f;
          check("RVB SIZE 1 -> 2", o, f, [](EffectBase& e) { e.setParameter(2, 2.0f); }, 8.0f); }
        { OldReverb o; Reverb f;
          check("RVB DCAY 0.7 -> 0.2", o, f, [](EffectBase& e) { e.setParameter(0, 0.2f); }, 8.0f); }
        { OldHallReverb o; HallReverb f;
          check("HALL SIZE 1 -> 0.4", o, f, [](EffectBase& e) { e.setParameter(1, 0.4f); }, 8.0f); }
        { OldHallReverb o; HallReverb f;
          check("HALL PDLY 20 -> 90", o, f, [](EffectBase& e) { e.setParameter(3, 90.0f); }, 8.0f); }
        { OldHallReverb o; HallReverb f;
          check("HALL DCAY 3 -> 0.5", o, f, [](EffectBase& e) { e.setParameter(0, 0.5f); }, 8.0f); }

        beginTest("HALL MOD turned under sound arrives: in the first samples after the turn the heads are still nearly where they were");
        {
            // (The step test above cannot tell this from a jump: under a low tone a head that jumps
            // a few samples is a small step. So: the same hall turned and not turned under 1 kHz,
            // and how far apart the two are over the first eight samples after the turn, against
            // half a second later. A pre-delay that jumped could not be told from one that glides
            // by any step at the output - the four allpasses spread the jump - so it has no test.)
            Stereo in { Buffer(at(2.0), 0.0f), Buffer(at(2.0), 0.0f) };
            for (size_t i = 0; i < in.l.size(); ++i) in.l[i] = in.r[i] = 0.3f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / sr);
            HallReverb a, b;
            a.prepare(sr, 256); b.prepare(sr, 256);
            Stereo sa = in, sb = in;
            const int block = 256, n = (int) in.l.size(), turnAt = (int) at(1.0) / block * block;
            for (int pos = 0; pos < n; pos += block)
            {
                if (pos == turnAt) b.setExtra(HallReverb::Mod, 1.0f);
                a.processBlock(sa.l.data() + pos, sa.r.data() + pos, std::min(block, n - pos));
                b.processBlock(sb.l.data() + pos, sb.r.data() + pos, std::min(block, n - pos));
            }
            auto gap = [&](size_t from, size_t to)
            {
                double d = 0.0;
                for (size_t i = from; i < to; ++i) d += std::pow((double) sb.l[i] - sa.l[i], 2.0);
                return std::sqrt(d / (double) (to - from)) / std::max(1.0e-12, rms(sa.l, at(0.5), at(1.0)));
            };
            const double first = gap((size_t) turnAt, (size_t) turnAt + 8), later = gap((size_t) turnAt + at(0.5), (size_t) turnAt + at(0.9));
            expect(later > 0.05 && first < 0.03 * later, "apart " + juce::String(first, 4) + " of the tone's level in the first 8 samples, " + juce::String(later, 3) + " half a second later");
        }

        beginTest("After a turn the cells are at rest again: the same sound as an effect made there (to -80 dB a second later)");
        {
            const Stereo in = material(sr, 6.0);
            auto settled = [&](EffectBase& turned, EffectBase& made, std::function<void(EffectBase&)> act, const juce::String& name)
            {
                act(made);
                turned.prepare(sr, 256); made.prepare(sr, 256);
                Stereo a = in, b = in;
                const int block = 256, n = (int) in.l.size();
                for (int pos = 0; pos < n; pos += block)
                {
                    if (pos == (int) at(0.5) / block * block) act(turned);
                    turned.processBlock(a.l.data() + pos, a.r.data() + pos, std::min(block, n - pos));
                    made.processBlock(b.l.data() + pos, b.r.data() + pos, std::min(block, n - pos));
                }
                Stereo ta { Buffer(a.l.begin() + (long) at(4.5), a.l.end()), Buffer(a.r.begin() + (long) at(4.5), a.r.end()) };
                Stereo tb { Buffer(b.l.begin() + (long) at(4.5), b.l.end()), Buffer(b.r.begin() + (long) at(4.5), b.r.end()) };
                expect(differenceDb(ta, tb) < -80.0, name + ": " + juce::String(differenceDb(ta, tb), 1) + " dB");
            };
            { Delay a, b; a.setParameter(1, 0.3f); b.setParameter(1, 0.3f); settled(a, b, [](EffectBase& e) { e.setParameter(0, 0.21f); }, "DLY TIME"); }
            { Reverb a, b; a.setParameter(0, 0.4f); b.setParameter(0, 0.4f); settled(a, b, [](EffectBase& e) { e.setParameter(2, 1.6f); }, "RVB SIZE"); }
            { HallReverb a, b; a.setParameter(0, 0.6f); b.setParameter(0, 0.6f); a.setExtra(0, 0.0f); b.setExtra(0, 0.0f);
              settled(a, b, [](EffectBase& e) { e.setParameter(1, 0.5f); }, "HALL SIZE"); }
        }
    }

    // ---- 6. any input, any rate, any block, every cell at both ends ---------------------------------
    void testBounds()
    {
        beginTest("Finite and bounded for any input (not a number, infinity, 1e30), 22.05 to 192 kHz, blocks of 1 to 4096, every cell at both ends");
        struct Kind { const char* name; std::function<std::unique_ptr<EffectBase>()> make; };
        const Kind kinds[] = { { "DLY", [] { return std::unique_ptr<EffectBase>(new Delay()); } },
                               { "RVB", [] { return std::unique_ptr<EffectBase>(new Reverb()); } },
                               { "HALL", [] { return std::unique_ptr<EffectBase>(new HallReverb()); } } };
        const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
        float worst = 0.0f;
        for (auto& k : kinds)
            for (double sr : { 22050.0, 48000.0, 192000.0 })
                for (int way = 0; way < 4; ++way)           // all at the bottom, all at the top, alternating both ways
                    for (int block : { 1, 4096 })
                    {
                        if (block == 1 && sr > 30000.0) continue;
                        auto fx = k.make();
                        for (int c = 0; c < 8; ++c)
                        {
                            const ParamSpec spec = c < 4 ? fx->getParamSpec(c) : fx->getExtraSpec(c - 4);
                            const bool top = way == 1 || (way == 2 && c % 2 == 0) || (way == 3 && c % 2 == 1);
                            if (c < 4) fx->setParameter(c, top ? spec.maxVal : spec.minVal);
                            else       fx->setExtra(c - 4, top ? spec.maxVal : spec.minVal);
                        }
                        fx->setMix(1.0f);
                        fx->prepare(sr, block);
                        const double seconds = block == 1 ? 0.3 : 1.2;
                        Stereo in = material(sr, seconds, 11);
                        for (auto& v : in.l) v *= 2.0f;
                        for (auto& v : in.r) v *= 2.0f;
                        const size_t n = in.l.size();
                        in.l[n / 3] = nan; in.r[n / 3 + 1] = inf; in.l[n / 3 + 2] = -inf; in.r[n / 3 + 3] = 1.0e30f; in.l[n / 3 + 4] = -1.0e30f;
                        for (size_t i = n / 2; i < n / 2 + 200; ++i) in.l[i] = in.r[i] = (i % 2 == 0) ? 1.0e30f : -1.0e30f;
                        const Stereo out = through(*fx, in, { block });
                        const juce::String what = juce::String(k.name) + " at " + juce::String(sr) + ", cells way " + juce::String(way) + ", block " + juce::String(block);
                        expect(finite(out.l) && finite(out.r), what + ": not a number");
                        worst = std::max(worst, std::max(peak(out.l), peak(out.r)));
                        expect(peak(out.l) < 5000.0f && peak(out.r) < 5000.0f, what + ": peak " + juce::String(std::max(peak(out.l), peak(out.r)), 1));
                    }
        logMessage("   the largest sample of all (the input is held to +-64 by the cell base): " + juce::String(worst, 1));

        // Shimmering or holding, what goes onto the hall's lines passes the ceiling (straight to
        // +-8, never beyond +-16): noise at the input limit comes out at 13, without it at 50 to 65.
        {
            HallReverb h;
            h.setExtra(HallReverb::Shimmer, 1.0f);
            h.setExtra(HallReverb::Mod, 1.0f);
            h.setParameter(HallReverb::DecayTime, 10.0f);
            h.setParameter(HallReverb::Damping, 0.0f);
            h.setMix(1.0f);
            h.prepare(48000.0, 512);
            Stereo loud { Buffer(96000, 0.0f), Buffer(96000, 0.0f) };
            juce::Random rng(21);
            for (size_t i = 0; i < loud.l.size(); ++i) { loud.l[i] = 60.0f * (rng.nextFloat() * 2.0f - 1.0f); loud.r[i] = -0.7f * loud.l[i]; }
            const Stereo out = through(h, loud, { 512 });
            const float most = std::max(peak(out.l), peak(out.r));
            expect(finite(out.l) && most < 20.0f, "HALL shimmering under noise at the input limit: peak " + juce::String(most, 1));
        }
    }

    // ---- 7. cost (NEPTUNE_E7_COST=1, a Release build of the tests) ----------------------------------
    void measureCost()
    {
        beginTest("Cost: microseconds per block of 512 at 48 kHz, old and new run alternately");
        const double sr = 48000.0;
        const int block = 512, blocks = 2000;
        const Stereo in = material(sr, (double) block * blocks / sr, 5);
        auto time = [&](EffectBase& fx)
        {
            Stereo work = in;
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b)
                fx.processBlock(work.l.data() + (size_t) b * block, work.r.data() + (size_t) b * block, block);
            const auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::micro>(t1 - t0).count() / blocks;
        };
        auto pair = [&](const juce::String& name, const Make& make)
        {
            double oldBest = 1.0e9, newBest = 1.0e9;
            for (int round = 0; round < 7; ++round)
            {
                Pair p = make();
                oldBest = std::min(oldBest, time(*p.old));
                newBest = std::min(newBest, time(*p.fresh));
            }
            logMessage("   COST " + name.paddedRight(' ', 30) + "old " + juce::String(oldBest, 2) + " us   new " + juce::String(newBest, 2)
                       + " us   (" + juce::String(100.0 * (newBest / oldBest - 1.0), 0) + " %)");
        };
        pair("DLY send (STER)", maker<OldDelay, Delay>(sr, [](auto& d) { d.setTime(0.375f); d.setFeedback(0.35f); }));
        pair("DLY PING", maker<OldDelay, Delay>(sr, [](auto& d) { d.setParameter(3, 1.0f); }));
        pair("RVB send", maker<OldReverb, Reverb>(sr, [](auto& r) { r.setDecay(0.6f); r.setDamping(0.4f); }));
        pair("HALL as made (MOD 0.3)", maker<OldHallReverb, HallReverb>(sr, [](auto&) {}));
        pair("HALL MOD 0", maker<OldHallReverb, HallReverb>(sr, [](auto& h) { h.setParameter(4, 0.0f); }));
        // The new cells in use (nothing old to set against them: the old figure is the effect without).
        pair("DLY READ REV + DRIV + AGE", [sr] { Pair p { std::make_unique<OldDelay>(), std::make_unique<Delay>() };
             p.fresh->setExtra(0, 1.0f); p.fresh->setExtra(1, 0.5f); p.fresh->setExtra(3, 0.5f); p.old->prepare(sr, 512); p.fresh->prepare(sr, 512); return p; });
        pair("RVB SHIM 0.5", [sr] { Pair p { std::make_unique<OldReverb>(), std::make_unique<Reverb>() };
             p.fresh->setExtra(1, 0.5f); p.old->prepare(sr, 512); p.fresh->prepare(sr, 512); return p; });
        pair("HALL SHIM 0.5", [sr] { Pair p { std::make_unique<OldHallReverb>(), std::make_unique<HallReverb>() };
             p.fresh->setExtra(1, 0.5f); p.old->prepare(sr, 512); p.fresh->prepare(sr, 512); return p; });
        pair("RVB HOLD", [sr] { Pair p { std::make_unique<OldReverb>(), std::make_unique<Reverb>() };
             p.fresh->setExtra(3, 1.0f); p.old->prepare(sr, 512); p.fresh->prepare(sr, 512); return p; });
        // In silence, after sound (what a state that has died down costs).
        {
            Stereo quiet { Buffer((size_t) block * blocks, 0.0f), Buffer((size_t) block * blocks, 0.0f) };
            for (size_t i = 0; i < 4800; ++i) quiet.l[i] = quiet.r[i] = in.l[i];
            auto silent = [&](const juce::String& name, const Make& make)
            {
                double o = 1.0e9, f = 1.0e9;
                for (int round = 0; round < 5; ++round)
                {
                    Pair p = make();
                    for (EffectBase* fx : { p.old.get(), p.fresh.get() })
                    {
                        Stereo work = quiet;
                        // (the first half lets it die down; the second half is timed)
                        fx->processBlock(work.l.data(), work.r.data(), block * blocks / 2);
                        const auto t0 = std::chrono::steady_clock::now();
                        for (int b = blocks / 2; b < blocks; ++b)
                            fx->processBlock(work.l.data() + (size_t) b * block, work.r.data() + (size_t) b * block, block);
                        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (blocks / 2);
                        (fx == p.old.get() ? o : f) = std::min(fx == p.old.get() ? o : f, us);
                    }
                }
                logMessage("   COST " + (name + ", died down").paddedRight(' ', 30) + "old " + juce::String(o, 2) + " us   new " + juce::String(f, 2) + " us");
            };
            silent("DLY", maker<OldDelay, Delay>(sr, [](auto& d) { d.setParameter(0, 0.05f); d.setParameter(1, 0.2f); }));
            silent("RVB", maker<OldReverb, Reverb>(sr, [](auto& r) { r.setParameter(0, 0.3f); }));
            silent("HALL", maker<OldHallReverb, HallReverb>(sr, [](auto& h) { h.setParameter(0, 0.5f); }));
        }
    }

    // ---- 8. the recordings (NEPTUNE_E7_RENDER=<folder>) ---------------------------------------------
    juce::File atlas;      // where pairs are kept, when the recordings are asked for

    static void writeWav(const juce::File& file, const Stereo& s, double sr)
    {
        // 24-bit PCM, two channels.
        const uint32_t frames = (uint32_t) s.l.size(), bytes = frames * 6;
        juce::MemoryOutputStream out;
        out.write("RIFF", 4); out.writeInt((int) (36 + bytes)); out.write("WAVE", 4);
        out.write("fmt ", 4); out.writeInt(16); out.writeShort(1); out.writeShort(2);
        out.writeInt((int) sr); out.writeInt((int) sr * 6); out.writeShort(6); out.writeShort(24);
        out.write("data", 4); out.writeInt((int) bytes);
        for (uint32_t i = 0; i < frames; ++i)
            for (const float v : { s.l[i], s.r[i] })
            {
                const int q = (int) std::lround((double) juce::jlimit(-1.0f, 1.0f, v) * 8388607.0);
                out.writeByte((char) (q & 0xff)); out.writeByte((char) ((q >> 8) & 0xff)); out.writeByte((char) ((q >> 16) & 0xff));
            }
        file.deleteFile();
        file.replaceWithData(out.getData(), out.getDataSize());
    }

    void keepPair(const juce::String& name, double sr, const Stereo& old, const Stereo& fresh)
    {
        if (atlas == juce::File() || sr != 48000.0) return;
        writeWav(atlas.getChildFile(name + "_old.wav"), old, sr);
        writeWav(atlas.getChildFile(name + "_new.wav"), fresh, sr);
    }

    // The old classes put where the new ones are in a loaded instrument, set as those are set.
    static void putOldEffects(P01KeyboardTests::Rig& rig)
    {
        for (int b = 0; b < rig.audio.getSendCount(); ++b)
        {
            auto& chain = rig.audio.getSendBus(b).getChain();
            EffectBase* now = chain.getEffect(0);
            std::unique_ptr<EffectBase> old;
            if (dynamic_cast<Reverb*>(now) != nullptr)      old = std::make_unique<OldReverb>();
            else if (dynamic_cast<Delay*>(now) != nullptr)  old = std::make_unique<OldDelay>();
            else continue;
            for (int c = 0; c < 4; ++c) old->setParameter(c, now->getParameter(c));
            old->setMix(now->getMix());
            chain.replaceEffect(0, std::move(old));
        }
        for (int t = 0; t < 8; ++t)
            if (auto* chain = rig.audio.getInsertChain(t))
                for (int slot = 0; slot < chain->getEffectCount(); ++slot)
                {
                    EffectBase* now = chain->getEffect(slot);
                    std::unique_ptr<EffectBase> old;
                    if (dynamic_cast<HallReverb*>(now) != nullptr)   old = std::make_unique<OldHallReverb>();
                    else if (dynamic_cast<Reverb*>(now) != nullptr)  old = std::make_unique<OldReverb>();
                    else if (dynamic_cast<Delay*>(now) != nullptr)   old = std::make_unique<OldDelay>();
                    else continue;
                    for (int c = 0; c < 4; ++c) old->setParameter(c, now->getParameter(c));
                    old->setMix(now->getMix());
                    old->setEnabled(now->isEnabled());
                    chain->replaceEffect(slot, std::move(old));
                }
    }

    struct Step { std::vector<int> pads; double beats; };
    // `played`: the chord is struck again within its bars (the keyboard sound dies away within a
    // second or two: a hand would not leave it lying); otherwise it is held (the drone; the arpeggiator).
    struct Take { const char* name; const char* title; int track; bool arp; bool played; double bpm; std::vector<Step> steps; double hold; };

    // Pads as the owner numbers them, 1..16 (bank 1 of P01's CHORD screen).
    static std::vector<Take> takes()
    {
        // 1 Cm (C2 C4 Eb4 G4)  2 D5 (D2 D3 A3)  3 Gm (G3 D4 G4 Bb4)  4 Gsus2 (G3 D4 A4)  5 F (F3 C4 A4)  6 C/E (E3 C4 G4)
        // 7 Eb5 (Eb1 Eb4 Bb4)  8 Fsus4 (F1 F4 Bb4 C5)  9 G1  10 F1  11 F3 C4  12 F (F1 F3 A3)  13 C3 F3 C4  14 C1  15 Eb5  16 F7 (F3 A3 Eb4 A4)
        const std::vector<Step> dorian = { { { 1, 14 }, 4 }, { { 5, 10 }, 4 }, { { 4, 9 }, 2 }, { { 3, 9 }, 2 }, { { 1, 14 }, 4 },
                                           { { 7 }, 4 }, { { 8 }, 2 }, { { 12 }, 2 }, { { 4, 9 }, 2 }, { { 3 }, 2 }, { { 1, 14 }, 6 } };
        const std::vector<Step> light = { { { 5, 10 }, 4 }, { { 6 }, 4 }, { { 2 }, 4 }, { { 4, 9 }, 4 }, { { 7 }, 4 }, { { 16, 10 }, 4 },
                                          { { 8 }, 2 }, { { 11, 10 }, 2 }, { { 6, 14 }, 6 } };
        const std::vector<Step> pedal = { { { 1, 14 }, 4 }, { { 13 }, 4 }, { { 1 }, 4 }, { { 15 }, 4 }, { { 5, 10 }, 4 }, { { 3, 9 }, 4 }, { { 1, 14 }, 8 } };
        const std::vector<Step> slow = { { { 1, 14 }, 4 }, { { 7 }, 4 }, { { 12 }, 4 }, { { 4, 9 }, 2 }, { { 3, 9 }, 2 }, { { 1, 14 }, 6 } };
        return {
            { "T3_keys_1_dorian",       "Cm  F  Gsus2 Gm  Cm  Eb5  Fsus4 F  Gsus2 Gm  Cm",        2, false, true,  72.0, dorian, 6.0 },
            { "T3_keys_2_borrowed",     "F  C/E  D5  Gsus2  Eb5  F7  Fsus4 F  C/E",                2, false, true,  66.0, light,  6.0 },
            { "T3_keys_3_pedal",        "Cm  Csus4(C F C)  Cm  Eb5  F  Gm  Cm, over the low C",   2, false, true,  60.0, pedal,  6.0 },
            { "T3_keys_1_dorian_arp",   "the first progression through track 3's arpeggiator as stored (latched)", 2, true, false, 72.0, dorian, 6.0 },
            { "T1_drone_1_dorian",      "the first progression on track 1, every chord held",      0, false, false, 72.0, dorian, 8.0 },
            { "T1_drone_4_slow",        "Cm  Eb5  F  Gsus2 Gm  Cm, slowly, every chord held",      0, false, false, 54.0, slow,   8.0 },
        };
    }

    Stereo renderTake(const Take& take, const ChordPads& chords, bool old, juce::String* played = nullptr)
    {
        int nonFinite = 0;
        Stereo out = renderTakeWith(take, chords, old ? &putOldEffects : nullptr, played, &nonFinite);
        expectEquals(nonFinite, 0, take.name);
        return out;
    }

    // The take, with `before` done to the loaded instrument first (the old classes put in, or
    // anything else a phase compares: tests/one_network_tests.h). `nonFinite`: blocks dropped.
    static Stereo renderTakeWith(const Take& take, const ChordPads& chords, void (*before)(P01KeyboardTests::Rig&),
                                 juce::String* played, int* nonFinite)
    {
        const double sr = 48000.0;
        const int block = 256;
        P01KeyboardTests::Rig rig(sr, block);
        if (! rig.loaded) return {};
        rig.seq.setSelectedTrack(take.track);
        if (before != nullptr) before(rig);

        Arpeggiator& arp = rig.arps[(size_t) take.track];
        if (take.arp)
        {
            // As main.cpp wires and seeds it; the settings are the project's.
            arp.setRandomSeed(0x1234567u + (uint32_t) take.track * 7919u);
            arp.onNoteOn = [&rig, &take](int pitch, float velocity) { if (auto* e = rig.engine(take.track)) e->noteOn(pitch, velocity, -1); };
            arp.onNoteOff = [&rig, &take](int pitch) { if (auto* e = rig.engine(take.track)) e->noteOff(pitch, -1); };
            arp.setEnabled(true);
        }
        auto on = [&](int note, float velocity) { if (take.arp) arp.noteOn(note, velocity); else if (auto* e = rig.engine(take.track)) e->noteOn(note, velocity, -1); };
        auto off = [&](int note) { if (take.arp) arp.noteOff(note); else if (auto* e = rig.engine(take.track)) e->noteOff(note, -1); };

        // The score. Every chord's notes start a little after one another from the lowest up (a
        // hand, not a machine) and are held into the next chord (a note that is struck again is let
        // go just before). A "played" take strikes the chord's upper notes again, a little softer,
        // on the "and" of 2 and on 4 (in a chord of two beats: on 2), the bass once. The last three
        // chords slow down; the last one is struck once and rings.
        struct Strike { double time, stepEnd; int note; float velocity; bool lastStep; };
        std::vector<Strike> strikes;
        const double beat = 60.0 / take.bpm;
        double t = 0.3;
        juce::String log;
        auto name = [](int note) { return juce::MidiMessage::getMidiNoteName(note, false, true, 4); };
        for (size_t s = 0; s < take.steps.size(); ++s)
        {
            const size_t left = take.steps.size() - 1 - s;
            const double slower = left == 2 ? 1.06 : (left == 1 ? 1.14 : 1.0);
            const double beats = take.steps[s].beats, length = beats * beat * slower;
            const bool last = left == 0;
            std::vector<ChordPads::Note> notes;
            for (int pad : take.steps[s].pads)
            {
                const auto& chord = chords.get(0, pad - 1);
                for (int k = 0; k < chord.count; ++k)
                {
                    bool there = false;
                    for (auto& n : notes) there = there || n.pitch == chord.notes[(size_t) k].pitch;
                    if (! there) notes.push_back(chord.notes[(size_t) k]);
                }
            }
            std::sort(notes.begin(), notes.end(), [](const ChordPads::Note& a, const ChordPads::Note& b) { return a.pitch < b.pitch; });
            log << juce::String(t, 2).paddedLeft(' ', 6) << " s  pads";
            for (int pad : take.steps[s].pads) log << " " << pad;
            log << ":";
            for (size_t k = 0; k < notes.size(); ++k)
            {
                strikes.push_back({ t + 0.014 * (double) k, t + length, notes[k].pitch, notes[k].velocity, last });
                log << " " << name(notes[k].pitch) << " (" << juce::String(notes[k].velocity, 2) << ")";
            }
            if (take.played && ! last)
            {
                std::vector<double> again;
                if (beats >= 4.0) again = { 1.5, 3.0 };
                else if (beats >= 2.0) again = { 1.0 };
                for (size_t r = 0; r < again.size(); ++r)
                {
                    int k = 0;
                    for (auto& n : notes)
                        if (n.pitch >= 48)         // (from C3 up: the bass is struck once)
                            strikes.push_back({ t + again[r] * beat * slower + 0.014 * (double) k++, t + length, n.pitch,
                                                juce::jmax(ChordPads::MIN_VELOCITY, n.velocity * (r == 0 ? 0.75f : 0.85f)), false });
                }
                log << (again.size() == 2 ? "   (upper notes again on 2-and and on 4)" : (again.size() == 1 ? "   (upper notes again on 2)" : ""));
            }
            log << "\n";
            t += length;
        }
        std::stable_sort(strikes.begin(), strikes.end(), [](const Strike& a, const Strike& b) { return a.time < b.time; });
        struct Event { double time; bool down; int note; float velocity; };
        std::vector<Event> events;
        for (size_t i = 0; i < strikes.size(); ++i)
        {
            double up = strikes[i].lastStep ? strikes[i].stepEnd : strikes[i].stepEnd + 0.07;
            for (size_t j = i + 1; j < strikes.size(); ++j)
                if (strikes[j].note == strikes[i].note) { up = std::min(up, strikes[j].time - 0.04); break; }
            events.push_back({ strikes[i].time, true, strikes[i].note, strikes[i].velocity });
            events.push_back({ up, false, strikes[i].note, 0.0f });
        }
        if (played != nullptr) *played = log;
        // (At the same moment a release comes before a strike.)
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.time < b.time || (a.time == b.time && ! a.down && b.down); });

        // Seeded as the pin's "seeded" family: old and new draw the same per-note spread.
        P01KeyboardTests::seedSystemRandom((juce::int64) (0x5eed0000 + 16 * take.track + 7));
        const juce::int64 total = (juce::int64) std::llround((t + take.hold) * sr);
        Stereo out { Buffer((size_t) total, 0.0f), Buffer((size_t) total, 0.0f) };
        size_t nextEvent = 0;
        for (juce::int64 pos = 0; pos + block <= total; pos += block)
        {
            while (nextEvent < events.size() && (juce::int64) std::llround(events[nextEvent].time * sr) <= pos)
            {
                const auto& e = events[nextEvent++];
                if (e.down) on(e.note, e.velocity); else off(e.note);
            }
            // (The arpeggiator latches: after the last chord it is switched off, so the tail rings out.)
            if (take.arp && pos >= (juce::int64) std::llround((t + 0.05) * sr) && arp.isEnabled())
                arp.setEnabled(false);
            rig.audio.renderBlock(out.l.data() + pos, out.r.data() + pos, block);
        }
        if (nonFinite != nullptr) *nonFinite = rig.audio.getNonFiniteBlocks();
        return out;
    }

    void renderRecordings(const juce::File& folder)
    {
        beginTest("The recordings: P01's two sounds playing its chord pads, with the old classes and with the new");
        folder.createDirectory();
        atlas = folder;
        const juce::int64 randomBefore = juce::Random::getSystemRandom().getSeed();

        const auto chordFile = juce::File(juce::SystemStats::getEnvironmentVariable(
            "NEPTUNE_E7_CHORDS", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Neptune/Projects/P01/project.json").getFullPathName()));
        expect(chordFile.existsAsFile(), "the project with the chord pads: " + chordFile.getFullPathName());
        if (! chordFile.existsAsFile()) return;
        ChordPads chords;
        chords.fromFlat(ProjectData::fromJSON(chordFile.loadFileAsString()).chordPads);       // (read only)
        int stored = 0;
        for (int p = 0; p < ChordPads::PADS; ++p) stored += chords.get(0, p).empty() ? 0 : 1;
        expect(stored >= 16, "sixteen pads of bank 1 hold a chord (" + juce::String(stored) + ")");

        juce::String numbers, scores;
        numbers << "E7: NEW minus OLD for every recording (48 kHz; levels of the DIFF file against the OLD file)\n\n";
        bool allSame = true;
        for (const auto& take : takes())
        {
            juce::String played;
            const Stereo old = renderTake(take, chords, true, &played), fresh = renderTake(take, chords, false);
            expect(! old.l.empty() && old.l.size() == fresh.l.size(), take.name);
            if (old.l.empty() || old.l.size() != fresh.l.size()) continue;
            Stereo diff = fresh;
            double worst = 0.0; size_t where = 0, differing = 0;
            for (size_t i = 0; i < diff.l.size(); ++i)
            {
                diff.l[i] -= old.l[i];
                diff.r[i] -= old.r[i];
                const double d = std::max(std::abs((double) diff.l[i]), std::abs((double) diff.r[i]));
                if (d > worst) { worst = d; where = i; }
                if (d != 0.0) ++differing;
            }
            const bool identical = same(old, fresh);
            allSame = allSame && identical;
            writeWav(folder.getChildFile(juce::String(take.name) + "_OLD.wav"), old, 48000.0);
            writeWav(folder.getChildFile(juce::String(take.name) + "_NEW.wav"), fresh, 48000.0);
            writeWav(folder.getChildFile(juce::String(take.name) + "_DIFF.wav"), diff, 48000.0);
            const double oldRms = std::sqrt(0.5 * (std::pow(rms(old.l), 2.0) + std::pow(rms(old.r), 2.0)));
            const double diffRms = std::sqrt(0.5 * (std::pow(rms(diff.l), 2.0) + std::pow(rms(diff.r), 2.0)));
            numbers << take.name << "\n"
                    << "   length " << juce::String((double) old.l.size() / 48000.0, 1) << " s, OLD peak " << juce::String(dB(std::max(peak(old.l), peak(old.r))), 1)
                    << " dBFS, OLD RMS " << juce::String(dB(oldRms), 1) << " dBFS\n"
                    << "   DIFF: " << (identical ? juce::String("silence. NEW is OLD, sample for sample, bit for bit (0 of ")
                                                   + juce::String((juce::int64) old.l.size() * 2) + " samples differ)"
                                                 : "RMS " + juce::String(dB(diffRms / std::max(oldRms, 1.0e-30)), 1) + " dB against OLD; largest difference "
                                                   + juce::String(dB(worst), 1) + " dBFS at " + juce::String((double) where / 48000.0, 3) + " s; "
                                                   + juce::String((juce::int64) differing) + " sample pairs differ") << "\n\n";
            scores << take.name << "  (" << (take.track == 2 ? "track 3 \"User 13\"" : "track 1 \"DuneDrone\"") << ", " << juce::String(take.bpm, 0) << " bpm"
                   << (take.arp ? ", arpeggiator on as stored" : ", arpeggiator off") << ")\n   " << take.title << "\n" << played << "\n";
            expect(peak(old.l) > 0.01f, juce::String(take.name) + " is heard");
            expect(identical, juce::String(take.name) + ": NEW is not OLD to the bit");
            logMessage("   " + juce::String(take.name).paddedRight(' ', 26) + (identical ? "bit-identical" : "DIFFERS " + juce::String(dB(diffRms / std::max(oldRms, 1.0e-30)), 1) + " dB")
                       + "   peak " + juce::String(dB(std::max(peak(old.l), peak(old.r))), 1) + " dBFS");
        }
        P01KeyboardTests::seedSystemRandom(randomBefore);

        // The effects alone, old and new, for the table (the pairs go to the same folder).
        table.clear();
        testOnPurpose();
        juce::String alone;
        for (auto& r : table) alone << "   " << r.name.paddedRight(' ', 30) << r.verdict.paddedRight(' ', 7) << r.numbers << "\n";

        folder.getChildFile("E7_numbers.txt").replaceWithText(numbers + "The effects alone (noise bursts, a click, two tones; <name>_old.wav / <name>_new.wav for those at 48 kHz):\n" + alone);
        juce::String readme;
        readme << "E7: the send delay (DLY), the send reverb (RVB) and the hall (HALL) moved onto the shared delay and reverb parts\n"
               << "=========================================================================================================\n\n"
               << "Result: " << (allSame ? "all three came out EXACT. Every NEW file here is the OLD file, bit for bit; every DIFF file is silence.\n"
                                           "There is nothing to decide by ear. The files are here so you can check that for yourself.\n"
                                         : "NOT every recording is identical: read E7_numbers.txt and listen.\n") << "\n"
               << "What the files are\n"
               << "  <take>_OLD.wav   the take with the three effects as they were before (the old classes, kept in tests/old/)\n"
               << "  <take>_NEW.wav   the same take, same notes, same per-note random spread, with the effects on the shared parts\n"
               << "  <take>_DIFF.wav  NEW minus OLD, sample by sample (a null test: silence means no difference at all)\n"
               << "  E7_numbers.txt   for every take the level of DIFF against OLD, the largest difference and where it is\n"
               << "  <name>_old.wav / <name>_new.wav   each effect alone on test sounds (noise bursts, a click, two tones)\n"
               << "All WAV files: 48 kHz, 24-bit, stereo. Levels are what the instrument gives (through the master limiter), not normalised.\n\n"
               << "How they were made\n"
               << "  The sounds are P01's: tests/fixtures/p01_keyboard.json loaded through the real project path, as the pin \"P01 Keyboard\" does.\n"
               << "  Track 3 \"User 13\" (the keyboard sound; sends to the reverb and the delay) and track 1 \"DuneDrone\" (through its HALL insert\n"
               << "  and the delay send), through engine, inserts, both sends and the master section.\n"
               << "  The chords are the ones stored on P01's chord pads (bank 1), read from " << chordFile.getFullPathName() << " (read only),\n"
               << "  each note with the velocity stored on the pad. Notes of a chord start 14 ms apart from the lowest up; a chord is held into\n"
               << "  the next; the last chords slow down and the last one rings out. The keyboard sound dies away within a second or two, so on\n"
               << "  track 3 the upper notes of a chord are struck again within its bar, a little softer; the drone's chords are simply held.\n"
               << "  To make them again: NEPTUNE_E7_RENDER=<folder> NEPTUNE_TEST_ONLY=\"Pinned On Cores\" build/NeptuneTests_artefacts/Debug/NeptuneTests\n\n"
               << "How to compare\n"
               << "  Open <take>_OLD.wav and <take>_NEW.wav in any editor on two tracks and switch between them, or just play DIFF: it is silent.\n"
               << "  (If a later change makes them differ, DIFF is what you would hear of the change, and E7_numbers.txt says how loud it is.)\n\n"
               << "The takes\n\n" << scores
               << "What is new and cannot be heard here (the new cells are at rest in every take)\n"
               << "  DLY  FX2: READ (FWD REV OCT+ 5TH+ OCT-), DRIV, SAT, AGE. MODE TAPE is now an unsteady, lightly driven tape (it was the same as STER).\n"
               << "  RVB  FX2: PDLY, SHIM, MODE (the shimmer's interval), HOLD.\n"
               << "  HALL FX2: MOD, SHIM, MODE, HOLD.\n"
               << "  A turned TIME, SIZE or PDLY is glided to (the pitch bends) where it used to jump.\n";
        folder.getChildFile("README.txt").replaceWithText(readme);
        atlas = juce::File();
    }
};

static PinnedOnCoresTests pinnedOnCoresTests;
