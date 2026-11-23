#pragma once
#include <juce_core/juce_core.h>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "../src/dsp/effects/SweptFilter.h"
#include "../src/dsp/effects/Wah.h"
#include "../src/dsp/effects/Phaser.h"
#include "../src/dsp/effects/FilterEffect.h"
#include "../src/dsp/effects/Tremolo.h"
#include "../src/dsp/effects/AutoPan.h"
#include "../src/dsp/effects/Vibrato.h"
#include "../src/dsp/effects/TrackFx.h"
#include "../src/dsp/effects/PerfEffects.h"
#include "../src/dsp/effects/TapeDelay.h"
#include "../src/dsp/effects/LCRDelay.h"
#include "../src/dsp/effects/SpringReverb.h"
#include "../src/dsp/effects/ShimmerReverb.h"
#include "../src/dsp/TempoDivisions.h"
#include "../src/dsp/LFO.h"
#include "effect_cores_sweep_old.h"
#include "effect_cores_sweep_oldcores.h"
#include "../src/engine/AudioEngine.h"

// Phase E6 of .ralph/spec-unify.md: one swept filter (swp::Core, SweptFilter.h) of which the wah,
// the phaser, the filter effect and TALK's vowel filter are settings; PERF's freeze, beat repeat
// and tape stop on the reverb parts and the delay core; the LFO's trigger modes, delay and fade on
// the effects' LFO, triggered by the key's onsets; one list of the ways a line is played; a
// time-like flag for tempo sync.
//
// "What must not be lost": every effect that was folded is rendered here OLD (its class as it was
// at b4ee701, effect_cores_sweep_old.h) and NEW with the same gesture and measured. The table is
// printed with NEPTUNE_E6_PRINT=1; NEPTUNE_E6_ATLAS=<folder> also writes each pair as 48 kHz WAV
// (<name>_old.wav, <name>_new.wav). NEPTUNE_E6_TIME=1 prints the cost of old and new.
// Levels are RMS over windows (a tone under a moving filter or delay is smeared for a narrow band).
class EffectCoresSweepTests : public juce::UnitTest
{
public:
    EffectCoresSweepTests() : UnitTest("Effect Cores Sweep") {}

    using Signal = std::vector<float>;
    struct Stereo { Signal l, r; };
    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;

    // ---- signals ---------------------------------------------------------------------------------
    static Signal sine(double seconds, double hz, float amp, double sr = SR)
    {
        Signal s((size_t) (seconds * sr));
        for (size_t i = 0; i < s.size(); ++i) s[i] = amp * (float) std::sin(6.283185307179586 * hz * (double) i / sr);
        return s;
    }
    static Signal silence(double seconds, double sr = SR) { return Signal((size_t) (seconds * sr), 0.0f); }
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
    static void append(Signal& a, const Signal& b) { a.insert(a.end(), b.begin(), b.end()); }

    // A played line: sawtooth notes that are struck and die away (the level an envelope follows),
    // with a rest in the middle.
    static Signal riff(double sr = SR)
    {
        Signal out;
        const double notes[] = { 110.0, 164.81, 220.0, 0.0, 146.83, 110.0, 329.63, 220.0 };
        for (double hz : notes)
        {
            const size_t n = (size_t) (0.3 * sr);
            for (size_t i = 0; i < n; ++i)
            {
                if (hz <= 0.0) { out.push_back(0.0f); continue; }
                const double t = (double) i / sr, ph = std::fmod(hz * t, 1.0);
                const float attack = (float) std::min(1.0, t / 0.003);
                out.push_back(0.6f * attack * (float) std::exp(-t / 0.12) * (float) (2.0 * ph - 1.0));
            }
        }
        return out;
    }

    // Drum hits: short bursts of a falling sine, at the given times.
    static Signal hits(double seconds, std::initializer_list<double> at, float amp = 0.8f, double sr = SR)
    {
        Signal out = silence(seconds, sr);
        for (double t0 : at)
        {
            const size_t start = (size_t) (t0 * sr);
            for (size_t i = 0; i < (size_t) (0.12 * sr) && start + i < out.size(); ++i)
            {
                const double t = (double) i / sr;
                out[start + i] += amp * (float) (std::exp(-t / 0.03) * std::sin(6.283185307179586 * (55.0 * t + 90.0 * 0.03 * (1.0 - std::exp(-t / 0.03)))));
            }
        }
        return out;
    }

    // ---- measurements ----------------------------------------------------------------------------
    static double rms(const Signal& s, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, s.size());
        if (to <= from) return 0.0;
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) sum += (double) s[i] * s[i];
        return std::sqrt(sum / (double) (to - from));
    }
    static double rmsSec(const Signal& s, double from, double to) { return rms(s, (size_t) (from * SR), (size_t) (to * SR)); }
    static double peak(const Signal& s)
    {
        double p = 0.0;
        for (float v : s) p = std::max(p, (double) std::abs(v));
        return p;
    }
    static bool finite(const Signal& s)
    {
        for (float v : s) if (! std::isfinite(v)) return false;
        return true;
    }
    static double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-9)); }
    // Brightness: the level of the sample-to-sample difference against the level (0 = all low, 2 = all at the top).
    static double brightness(const Signal& s, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, s.size());
        double d = 0.0, a = 0.0;
        for (size_t i = from + 1; i < to; ++i) { d += (double) (s[i] - s[i - 1]) * (s[i] - s[i - 1]); a += (double) s[i] * s[i]; }
        return a > 0.0 ? std::sqrt(d / a) : 0.0;
    }
    // Upward zero crossings a second (the pitch of a plain tone).
    static double crossings(const Signal& s, size_t from, size_t to)
    {
        to = std::min(to, s.size());
        int count = 0;
        for (size_t i = from + 1; i < to; ++i) if (s[i - 1] < 0.0f && s[i] >= 0.0f) ++count;
        return to > from ? (double) count * SR / (double) (to - from) : 0.0;
    }

    // Old against new, as numbers.
    struct Compared
    {
        double null = 0.0;          // the difference's level under the old one's, dB (-100 = the same)
        double level = 0.0;         // new level - old level, dB, whole render
        double curve = 0.0;         // the largest difference of the two loudness curves, dB (50 ms windows that sound)
        double bright = 0.0;        // new brightness / old brightness - 1, whole render
        double brightCurve = 0.0;   // the largest difference of brightness over time (250 ms windows that sound), as a share
        double width = 0.0;         // new - old: level of (L - R) against (L + R)
    };
    static double sideShare(const Stereo& s)
    {
        double side = 0.0, mid = 0.0;
        for (size_t i = 0; i < s.l.size(); ++i) { side += (double) (s.l[i] - s.r[i]) * (s.l[i] - s.r[i]); mid += (double) (s.l[i] + s.r[i]) * (s.l[i] + s.r[i]); }
        return mid > 0.0 ? std::sqrt(side / mid) : 0.0;
    }
    static Compared compare(const Stereo& older, const Stereo& newer)
    {
        Compared c;
        Signal diff(older.l.size());
        for (size_t i = 0; i < diff.size(); ++i) diff[i] = newer.l[i] - older.l[i];
        const double oldLevel = rms(older.l), newLevel = rms(newer.l);
        c.null = oldLevel > 0.0 ? std::max(-100.0, db(rms(diff) / oldLevel)) : (newLevel > 0.0 ? 0.0 : -100.0);
        c.level = db(newLevel / std::max(oldLevel, 1.0e-12));
        const size_t win = (size_t) (0.05 * SR);
        for (size_t at = 0; at + win <= older.l.size(); at += win)
        {
            const double a = rms(older.l, at, at + win), b = rms(newer.l, at, at + win);
            if (std::max(a, b) > 0.02 * std::max(oldLevel, newLevel))
                c.curve = std::max(c.curve, std::abs(db(b / std::max(a, 1.0e-9))));
        }
        const double ob = brightness(older.l), nb = brightness(newer.l);
        c.bright = ob > 0.0 ? nb / ob - 1.0 : 0.0;
        const size_t wide = (size_t) (0.25 * SR);
        for (size_t at = 0; at + wide <= older.l.size(); at += wide)
        {
            if (std::min(rms(older.l, at, at + wide), rms(newer.l, at, at + wide)) < 0.05 * oldLevel) continue;
            const double a = brightness(older.l, at, at + wide), b = brightness(newer.l, at, at + wide);
            if (a > 0.0) c.brightCurve = std::max(c.brightCurve, std::abs(b / a - 1.0));
        }
        c.width = sideShare(newer) - sideShare(older);
        return c;
    }
    // MATCH: the difference is 40 dB under the sound, or loudness and brightness agree to within
    // 0.5 dB and 3 % everywhere. CLOSE: within 3 dB and 20 %. Anything else is LOST.
    static const char* verdict(const Compared& c)
    {
        if (c.null <= -40.0) return "MATCH";
        if (c.curve <= 0.5 && c.brightCurve <= 0.03 && std::abs(c.width) <= 0.03) return "MATCH";
        if (c.curve <= 3.0 && c.brightCurve <= 0.20 && std::abs(c.width) <= 0.15) return "CLOSE";
        return "LOST";
    }

    static bool printing() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_E6_PRINT", "").isNotEmpty(); }
    static juce::String atlas() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_E6_ATLAS", ""); }

    static void writeWav(const juce::File& file, const Stereo& s)
    {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (! out.openedOk()) return;
        const int frames = (int) s.l.size(), bytes = frames * 4;
        out.write("RIFF", 4); out.writeInt(36 + bytes); out.write("WAVEfmt ", 8);
        out.writeInt(16); out.writeShort(1); out.writeShort(2); out.writeInt((int) SR); out.writeInt((int) SR * 4);
        out.writeShort(4); out.writeShort(16); out.write("data", 4); out.writeInt(bytes);
        for (int i = 0; i < frames; ++i)
            for (float v : { s.l[(size_t) i], s.r[(size_t) i] })
                out.writeShort((short) std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    Compared report(const juce::String& name, const Stereo& older, const Stereo& newer)
    {
        const Compared c = compare(older, newer);
        if (printing())
            logMessage(juce::String::formatted("E6 | %-26s | %-5s | null %6.1f dB | level %+5.2f dB | curve %4.2f dB | bright %+5.1f %% (over time %4.1f %%) | width %+5.3f",
                                               name.toRawUTF8(), verdict(c), c.null, c.level, c.curve, 100.0 * c.bright, 100.0 * c.brightCurve, c.width));
        if (atlas().isNotEmpty())
        {
            const juce::File folder(atlas());
            folder.createDirectory();
            writeWav(folder.getChildFile(name + "_old.wav"), older);
            writeWav(folder.getChildFile(name + "_new.wav"), newer);
        }
        return c;
    }

    // ---- rendering ---------------------------------------------------------------------------------
    // An effect through its block call; `key` (if any) is handed over before each block; `each`
    // is called before every block with the sample it starts at (a gesture).
    static Stereo render(EffectBase& fx, const Signal& in, const Signal* key = nullptr, int block = BLOCK,
                         const std::function<void(EffectBase&, size_t)>& each = {}, const Signal* inRight = nullptr)
    {
        Stereo out { in, inRight != nullptr ? *inRight : in };
        for (size_t at = 0; at < in.size(); at += (size_t) block)
        {
            const int n = (int) std::min((size_t) block, in.size() - at);
            if (each) each(fx, at);
            if (key != nullptr && fx.wantsInput()) fx.setInput(key->data() + at, key->data() + at, n);
            fx.processBlock(out.l.data() + at, out.r.data() + at, n);
        }
        return out;
    }

    template <typename Fx>
    static std::unique_ptr<Fx> made(const std::function<void(Fx&)>& set = {}, double sr = SR)
    {
        auto fx = std::make_unique<Fx>();
        if (set) set(*fx);
        fx->prepare(sr, 512);
        fx->reset();
        return fx;
    }

    template <typename Block>
    static Stereo renderTalk(Block& talk, const Signal& in, const Signal* inRight = nullptr)
    {
        Stereo out { in, inRight != nullptr ? *inRight : in };
        for (size_t at = 0; at < in.size(); at += (size_t) BLOCK)
        {
            const int n = (int) std::min((size_t) BLOCK, in.size() - at);
            talk.process(out.l.data() + at, out.r.data() + at, n);
        }
        return out;
    }

    void runTest() override
    {
        testNothingLostSweep();
        testNothingLostPerf();
        testSweptFilterGains();
        testTriggers();
        testWaysOfPlaying();
        testTimeLike();
        testAnyInput();
        testCost();
    }

    // =================================================================================================
    // 1. What must not be lost: wah, phaser, filter effect, TALK, vibrato's fade
    // =================================================================================================
    void testNothingLostSweep()
    {
        const Signal line = riff();
        Signal pad = sine(2.4, 110.0, 0.25f);
        { const Signal top = sine(2.4, 331.0, 0.15f), air = noise(2.4, 0.1f); for (size_t i = 0; i < pad.size(); ++i) pad[i] += top[i] + air[i]; }
        const Signal kicks = hits(2.4, { 0.1, 0.6, 1.1, 1.35, 1.6, 2.1 });

        beginTest("Wah, old against new: the envelope, the LFO, both, and the key from the track's input");
        {
            struct Case { const char* name; float freq, reso, sens, rate; int shape; bool keyIn; bool padIn; double nullMost; };
            const Case cases[] = {
                { "wah_default_envelope", 400.0f, 0.5f, 0.5f, 0.0f, 0, false, false, -30.0 },
                { "wah_lfo_only",         500.0f, 0.5f, 0.0f, 2.0f, 0, false, true, -30.0 },
                { "wah_env_and_lfo_sharp", 300.0f, 0.9f, 1.0f, 4.0f, 0, false, false, -24.0 },
                { "wah_square_lfo",       600.0f, 0.7f, 0.2f, 3.0f, 2, false, true, -24.0 },
                { "wah_keyed_from_input", 300.0f, 0.6f, 1.0f, 0.0f, 0, true, true, -30.0 } };
            for (const auto& c : cases)
            {
                auto set = [&c](EffectBase& fx)
                {
                    fx.setParameter(0, c.freq); fx.setParameter(1, c.reso); fx.setParameter(2, c.sens); fx.setParameter(3, c.rate);
                    fx.setExtra(0, (float) c.shape); fx.setExtra(Wah::KEY_EXTRA, c.keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f);
                };
                auto older = made<e6old::Wah>([&](e6old::Wah& fx) { set(fx); });
                auto newer = made<Wah>([&](Wah& fx) { set(fx); });
                const Signal& in = c.padIn ? pad : line;
                const Stereo a = render(*older, in, c.keyIn ? &kicks : nullptr), b = render(*newer, in, c.keyIn ? &kicks : nullptr);
                const Compared r = report(c.name, a, b);
                expect(finite(b.l) && rms(b.l) > 0.001);
                expect(r.null <= c.nullMost, juce::String(c.name) + ": the new wah is the old one (difference " + juce::String(r.null, 1) + " dB)");
                expect(std::abs(r.level) < 0.3 && r.curve < 1.0, juce::String(c.name) + " level");
            }
        }

        beginTest("Phaser, old against new: the same sound, sample for sample near enough");
        {
            struct Case { const char* name; float rate, depth, fb; int stages; int shape; float spread; float mix; };
            const Case cases[] = {
                { "phaser_default",        0.5f, 0.5f, 0.3f, 4, 0, 0.0f, 0.5f },
                { "phaser_deep_six",       2.0f, 1.0f, 0.8f, 6, 0, 0.0f, 0.5f },
                { "phaser_two_negative_spread", 1.0f, 0.8f, -0.9f, 2, 1, 1.0f, 0.5f },
                { "phaser_five_random",    3.0f, 0.7f, 0.5f, 5, 4, 0.5f, 1.0f } };
            const Signal in = noise(2.4, 0.3f, 99);
            for (const auto& c : cases)
            {
                auto set = [&c](EffectBase& fx)
                {
                    fx.setParameter(0, c.rate); fx.setParameter(1, c.depth); fx.setParameter(2, c.fb); fx.setParameter(3, (float) c.stages);
                    fx.setExtra(0, (float) c.shape); fx.setExtra(1, c.spread); fx.setMix(c.mix);
                };
                auto older = made<e6old::Phaser>([&](e6old::Phaser& fx) { set(fx); });
                auto newer = made<Phaser>([&](Phaser& fx) { set(fx); });
                const Stereo a = render(*older, in), b = render(*newer, in);
                const Compared r = report(c.name, a, b);
                expect(r.null <= -60.0, juce::String(c.name) + ": difference " + juce::String(r.null, 1) + " dB");
            }
        }

        beginTest("Filter effect, old against new: every type at a setting of its own");
        {
            struct Case { const char* name; float cut, reso; int type, mode; };
            const Case cases[] = {
                { "filter_svf_lp",     2000.0f, 0.2f, 0, 0 },
                { "filter_svf_notch",   900.0f, 0.6f, 0, 3 },
                { "filter_sem_morph",  1500.0f, 0.5f, 1, 0 },
                { "filter_ladder_lp24", 800.0f, 0.7f, 2, 0 },
                { "filter_ladder_hp",   400.0f, 0.3f, 2, 5 },
                { "filter_diode_bp",   1200.0f, 0.5f, 3, 2 },
                { "filter_comb_pos",    300.0f, 0.8f, 4, 0 },
                { "filter_comb_neg",    150.0f, 0.6f, 4, 1 } };
            for (const auto& c : cases)
            {
                auto set = [&c](EffectBase& fx)
                {
                    fx.setParameter(0, c.cut); fx.setParameter(1, c.reso); fx.setParameter(2, (float) c.type); fx.setParameter(3, (float) c.mode);
                };
                auto older = made<e6old::FilterEffect>([&](e6old::FilterEffect& fx) { set(fx); });
                auto newer = made<FilterEffect>([&](FilterEffect& fx) { set(fx); });
                const Stereo a = render(*older, line), b = render(*newer, line);
                const Compared r = report(c.name, a, b);
                expect(r.null <= -60.0, juce::String(c.name) + ": difference " + juce::String(r.null, 1) + " dB");
            }
        }

        beginTest("TALK, old against new: a vowel by hand, the LFO, the level either way, every cell away from its start");
        {
            struct Case { const char* name; float vowel, shift, reso, mix, rate, depth, env, bite; double curveMost, brightMost; };
            const Case cases[] = {
                { "talk_vowel_a",       0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 0.3f, 0.5, 0.03 },
                { "talk_vowel_i_sharp", 0.5f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.5, 0.03 },
                { "talk_lfo",           0.5f, 0.0f, 0.5f, 1.0f, 4.0f, 1.0f, 0.0f, 0.3f, 3.0, 0.20 },
                { "talk_envelope_up",   0.0f, 0.0f, 0.6f, 1.0f, 1.0f, 0.0f, 1.0f, 0.3f, 1.5, 0.10 },
                { "talk_envelope_down_shifted", 1.0f, 0.5f, 0.8f, 0.7f, 1.0f, 0.0f, -1.0f, 1.0f, 1.5, 0.10 } };
            for (const auto& c : cases)
            {
                auto set = [&c](auto& talk)
                {
                    const float v[8] = { c.vowel, c.shift, c.reso, c.mix, c.rate, c.depth, c.env, c.bite };
                    for (int i = 0; i < 8; ++i) talk.set(i, v[i]);
                    talk.prepare(SR, 512);
                };
                e6old::TalkBlock older; TalkBlock newer;
                set(older); set(newer);
                const Stereo a = renderTalk(older, line), b = renderTalk(newer, line);
                const Compared r = report(c.name, a, b);
                expect(finite(b.l));
                expect(r.curve <= c.curveMost && r.brightCurve <= c.brightMost,
                       juce::String(c.name) + ": loudness " + juce::String(r.curve, 2) + " dB, brightness " + juce::String(100.0 * r.brightCurve, 1) + " %");
            }
        }

        beginTest("Vibrato's delayed onset, old against new: the depth grows from each sound that begins after silence");
        {
            // Two notes with silence between; FADE 0.4 s.
            Signal in = sine(1.0, 440.0, 0.4f);
            append(in, silence(0.5));
            append(in, sine(1.0, 440.0, 0.4f));
            auto set = [](EffectBase& fx) { fx.setParameter(0, 6.0f); fx.setParameter(1, 80.0f); fx.setParameter(3, 0.4f); };
            auto older = made<e6old::Vibrato>([&](e6old::Vibrato& fx) { set(fx); });
            auto newer = made<Vibrato>([&](Vibrato& fx) { set(fx); });
            const Stereo a = render(*older, in), b = render(*newer, in);
            const Compared r = report("vibrato_fade_two_notes", a, b);
            // The pitch swing: how far the pitch (from one upward crossing to the next) wanders, early and late in each note.
            auto swing = [](const Signal& s, double from, double to)
            {
                double lo = 1.0e9, hi = 0.0, last = -1.0;
                for (size_t i = (size_t) (from * SR) + 1; i < (size_t) (to * SR); ++i)
                    if (s[i - 1] < 0.0f && s[i] >= 0.0f)
                    {
                        const double at = (double) i - (double) s[i] / (double) (s[i] - s[i - 1]);
                        if (last >= 0.0) { const double f = SR / (at - last); lo = std::min(lo, f); hi = std::max(hi, f); }
                        last = at;
                    }
                return hi > lo ? hi - lo : 0.0;
            };
            for (double note : { 0.0, 1.5 })
            {
                const double oldEarly = swing(a.l, note + 0.02, note + 0.1), oldLate = swing(a.l, note + 0.6, note + 0.95);
                const double newEarly = swing(b.l, note + 0.02, note + 0.1), newLate = swing(b.l, note + 0.6, note + 0.95);
                if (printing()) logMessage(juce::String::formatted("E6 |   vibrato note at %.1f s: swing early / late, old %.0f / %.0f Hz, new %.0f / %.0f Hz", note, oldEarly, oldLate, newEarly, newLate));
                expect(newEarly < 0.3 * newLate && newLate > 20.0, "the new vibrato fades in on a note that begins");
                expect(std::abs(newLate - oldLate) <= 0.05 * oldLate && std::abs(newEarly - oldEarly) <= 0.05 * oldLate, "as the old one did");
            }
            expect(std::abs(r.level) < 0.3);
        }

        beginTest("As they always listened (review): TALK to the mean level of a stereo sound; the vibrato's FADE to a phrase, at TRIG FREE");
        {
            // TALK's envelope follows the mean of the two sides: a sound on one side only pushes
            // the vowel half as far as the same sound on both.
            for (int which = 0; which < 2; ++which)
            {
                const Signal right = which == 0 ? silence(2.4) : pad;
                auto set = [](auto& talk)
                {
                    const float v[8] = { 0.0f, 0.0f, 0.6f, 1.0f, 1.0f, 0.0f, 1.0f, 0.3f };
                    for (int i = 0; i < 8; ++i) talk.set(i, v[i]);
                    talk.prepare(SR, 512);
                };
                e6old::TalkBlock older; TalkBlock newer;
                set(older); set(newer);
                const Stereo a = renderTalk(older, line, &right), b = renderTalk(newer, line, &right);
                const Compared r = report(which == 0 ? "talk_envelope_one_side" : "talk_envelope_two_sounds", a, b);
                expect(r.null <= -60.0, "TALK, a stereo sound: difference " + juce::String(r.null, 1) + " dB");
            }

            // The vibrato's FADE with TRIG at FREE begins with a sound out of silence (half a second
            // of rest), not with every note: after a short rest and at an accent of 8 dB the vibrato
            // is at its full depth, as it always was. With TRIG at KEY every onset begins it.
            Signal phrase = sine(1.0, 330.0, 0.4f);
            append(phrase, silence(0.3));
            append(phrase, sine(0.8, 440.0, 0.3f));
            append(phrase, sine(0.8, 440.0, 0.75f));
            append(phrase, silence(0.9));
            append(phrase, sine(0.6, 440.0, 0.4f));     // (a new phrase, at 3.8 s)
            auto swing = [](const Signal& s, double from, double to)
            {
                double lo = 1.0e9, hi = 0.0, last = -1.0;
                for (size_t i = (size_t) (from * SR) + 1; i < (size_t) (to * SR); ++i)
                    if (s[i - 1] < 0.0f && s[i] >= 0.0f)
                    {
                        const double at = (double) i - (double) s[i] / (double) (s[i] - s[i - 1]);
                        if (last >= 0.0) { const double f = SR / (at - last); lo = std::min(lo, f); hi = std::max(hi, f); }
                        last = at;
                    }
                return hi > lo ? hi - lo : 0.0;
            };
            auto set = [](EffectBase& fx) { fx.setParameter(0, 6.0f); fx.setParameter(1, 60.0f); fx.setParameter(3, 0.6f); };
            auto older = made<e6old::Vibrato>([&](e6old::Vibrato& fx) { set(fx); });
            auto newer = made<Vibrato>([&](Vibrato& fx) { set(fx); });
            auto perNote = made<Vibrato>([&](Vibrato& fx) { set(fx); fx.setExtra(Vibrato::Trig, (float) (int) LFO::TriggerMode::KeySync); });
            const Stereo a = render(*older, phrase), b = render(*newer, phrase), c = render(*perNote, phrase);
            const Compared r = report("vibrato_fade_phrase", a, b);
            expect(r.null <= -40.0, "the vibrato's FADE over a phrase: difference " + juce::String(r.null, 1) + " dB");
            const double full = swing(a.l, 1.8, 2.05);
            for (double begins : { 1.3, 2.1 })
            {
                const double was = swing(a.l, begins + 0.2, begins + 0.4), is = swing(b.l, begins + 0.2, begins + 0.4), each = swing(c.l, begins + 0.2, begins + 0.4);
                if (printing()) logMessage(juce::String::formatted("E6 |   vibrato, the note at %.1f s, swing 0.2 to 0.4 s on: old %.0f Hz, new at FREE %.0f Hz, new at KEY %.0f Hz (full %.0f)", begins, was, is, each, full));
                expect(was > 0.9 * full && std::abs(is - was) < 0.05 * full, "FREE: at its full depth after a short rest and at an accent, as before");
                expect(each < 0.75 * full, "KEY: every note fades in");
            }
            // After a real rest the depth is back at nothing before the next sound begins: its
            // first moments are at pitch (the depth is not thrown down under the attack).
            const double wasFirst = swing(a.l, 3.8, 3.8 + 0.05), isFirst = swing(b.l, 3.8, 3.8 + 0.05);
            if (printing()) logMessage(juce::String::formatted("E6 |   vibrato, a new phrase at 3.8 s, swing in its first 50 ms: old %.1f Hz, new %.1f Hz", wasFirst, isFirst));
            expect(isFirst < 0.2 * full && isFirst < wasFirst + 0.05 * full, "a new phrase begins at pitch");
        }
    }

    // =================================================================================================
    // 2. What must not be lost: PERF's freeze, beat repeat and tape stop
    // =================================================================================================
    // A pad: reset, then pressed at `press` and let go at `release` (seconds).
    template <typename Fx>
    static Stereo played(Fx& fx, const Signal& in, double press, double release, float bpm = 120.0f)
    {
        fx.setTempo(bpm);
        fx.prepare(SR, 512);
        fx.reset();
        bool down = false, up = false;
        return render(fx, in, nullptr, BLOCK, [&](EffectBase&, size_t at)
        {
            const double t = (double) at / SR;
            if (! down && t >= press) { down = true; fx.padGate(true); }
            if (! up && t >= release) { up = true; fx.padGate(false); }
        });
    }

    // The same sound two samples early.
    static Signal early(const Signal& in)
    {
        Signal out(in.size(), 0.0f);
        for (size_t i = 0; i + 2 < in.size(); ++i) out[i] = in[i + 2];
        return out;
    }

    void testNothingLostPerf()
    {
        Signal chord = sine(8.0, 220.0, 0.2f);
        {
            const Signal b = sine(8.0, 277.18, 0.15f), c = sine(8.0, 329.63, 0.15f), d = noise(8.0, 0.05f, 7);
            for (size_t i = 0; i < chord.size(); ++i) chord[i] += b[i] + c[i] + d[i];
        }
        Signal beat = hits(8.0, { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 5.5, 6.0, 6.5, 7.0, 7.5 });
        { const Signal top = riff(); for (size_t i = 0; i < beat.size(); ++i) beat[i] += 0.5f * top[i % top.size()]; }

        beginTest("FRZ, old against new: what is caught is held at its level, dies away in DEC, and climbs with SPRL");
        {
            struct Case { const char* name; float grab, shim, sprl, dec; };
            const Case cases[] = {
                { "freeze_hold_then_spiral", 0.25f, 0.0f, 0.5f, 3.0f },
                { "freeze_plain_decay",      0.25f, 0.0f, 0.0f, 1.5f },
                { "freeze_shimmer_held",     0.50f, 0.6f, 0.0f, 3.0f },
                { "freeze_short_grab",       0.05f, 0.0f, 1.0f, 6.0f } };
            for (const auto& c : cases)
            {
                auto set = [&c](EffectBase& fx) { fx.setParameter(0, c.grab); fx.setParameter(1, c.shim); fx.setParameter(2, c.sprl); fx.setParameter(3, c.dec); };
                // Held: the level at the end of the hold against its start; let go: the fall over the next second.
                auto held = [](const Stereo& s) { return db(rmsSec(s.l, 3.4, 3.9) / std::max(1.0e-9, rmsSec(s.l, 1.4, 1.9))); };
                auto fall = [](const Stereo& s) { return db(rmsSec(s.l, 4.4, 4.9) / std::max(1.0e-9, rmsSec(s.l, 3.4, 3.9))); };
                // How even the held sound is: the spread of its level in 10 ms windows, as a share of the level.
                auto rough = [](const Stereo& s)
                {
                    double sum = 0.0, sq = 0.0; int n = 0;
                    for (double t = 2.0; t < 3.9; t += 0.01) { const double v = rmsSec(s.l, t, t + 0.01); sum += v; sq += v * v; ++n; }
                    const double m = sum / n;
                    return m > 0.0 ? std::sqrt(std::max(0.0, sq / n - m * m)) / m : 0.0;
                };
                auto late = [](const Stereo& s) { return brightness(s.l, (size_t) (4.1 * SR), (size_t) (4.6 * SR)) / std::max(1.0e-9, brightness(s.l, (size_t) (3.0 * SR), (size_t) (3.9 * SR))); };
                // What is shifted round the loop goes its own way from every press (the heads stand
                // somewhere else in their grain): the same press differs by 15 dB after two seconds,
                // in the old effect as in the new. So five presses each, and their means.
                struct Mean { double held = 0, fall = 0, rough = 0, late = 0, level = 0, heldLow = 1e9, heldHigh = -1e9; };
                Mean was, is;
                const double presses[] = { 0.5, 0.507, 0.513, 0.52, 0.55 };
                Compared first;
                for (double press : presses)
                {
                    e6old::FreezeReverb older; FreezeReverb newer;
                    set(older); set(newer);
                    const Stereo a = played(older, chord, press, 4.0), b = played(newer, chord, press, 4.0);
                    if (press == presses[0]) first = report(c.name, a, b);
                    expect(finite(b.l) && rmsSec(b.l, 1.0, 1.9) > 0.01, juce::String(c.name) + " holds something");
                    for (auto* pair : { &was, &is })
                    {
                        const Stereo& s = pair == &was ? a : b;
                        pair->held += held(s) / 5.0; pair->fall += fall(s) / 5.0; pair->rough += rough(s) / 5.0; pair->late += late(s) / 5.0;
                        pair->level += db(rmsSec(s.l, 1.4, 1.9)) / 5.0;
                        pair->heldLow = std::min(pair->heldLow, held(s)); pair->heldHigh = std::max(pair->heldHigh, held(s));
                    }
                }
                if (printing())
                    logMessage(juce::String::formatted("E6 |   %s, five presses, old / new: held over 2 s %+.2f (%+.1f..%+.1f) / %+.2f (%+.1f..%+.1f) dB, let go %+.1f / %+.1f dB after 0.7 s, unevenness %.3f / %.3f, brightness after against before the release x%.2f / x%.2f",
                                                       c.name, was.held, was.heldLow, was.heldHigh, is.held, is.heldLow, is.heldHigh, was.fall, is.fall, was.rough, is.rough, was.late, is.late));
                const double scatter = std::max(1.0, was.heldHigh - was.heldLow);
                expect(std::abs(is.held - was.held) < 0.75 * scatter, juce::String(c.name) + ": held as steadily as before");
                // (Since the one reverb network FRZ is rvb::setting::field(): a shimmer fed on itself while
                // held falls 3.6 dB less in the 0.7 s after the release, -10.8 against -14.4: re-pinned at 30 %.)
                expect(std::abs(is.fall - was.fall) < std::max(2.0, 0.3 * std::abs(was.fall)), juce::String(c.name) + ": dies away as before");
                expect(std::abs(is.level - was.level) < 1.0, juce::String(c.name) + ": at the old level");
                expect(is.rough < was.rough * 1.3 + 0.03, juce::String(c.name) + ": as even");
                expect(std::abs(is.late / was.late - 1.0) < 0.2, juce::String(c.name) + ": climbs as before");
                expect(first.curve < 6.5 || c.shim > 0.0f);      // (the one network: 6.3 dB at the short grab, one press)
            }
        }

        beginTest("RPT, old against new: the slice comes round at its division, quieter, faster or slower, gated");
        {
            struct Case { const char* name; int div; float dcay, ptch, gate; float bpm; double nullMost; };
            const Case cases[] = {
                { "repeat_sixteenth",           3, 0.0f, 0.0f, 1.0f, 120.0f, -40.0 },
                { "repeat_eighth_decay_gate",   1, 0.5f, 0.0f, 0.5f, 120.0f, -40.0 },
                { "repeat_climbing_octaves",    3, 0.2f, 12.0f, 1.0f, 120.0f, -30.0 },
                { "repeat_sinking_fifths",      2, 0.0f, -7.0f, 0.8f, 97.0f, -30.0 },
                { "repeat_quarter_slow_tempo",  0, 0.3f, 2.0f, 1.0f, 30.0f, -30.0 } };
            for (const auto& c : cases)
            {
                auto set = [&c](EffectBase& fx) { fx.setParameter(0, (float) c.div); fx.setParameter(1, c.dcay); fx.setParameter(2, c.ptch); fx.setParameter(3, c.gate); };
                e6old::BeatRepeat older; BeatRepeat newer;
                set(older); set(newer);
                const Stereo a = played(older, beat, 0.5, 7.5, c.bpm), b = played(newer, beat, 0.5, 7.5, c.bpm);
                const Compared r = report(c.name, a, b);
                expect(finite(b.l) && rms(b.l) > 0.01);
                expect(r.null <= c.nullMost, juce::String(c.name) + ": difference " + juce::String(r.null, 1) + " dB");
                expect(r.curve < 1.5 && std::abs(r.level) < 0.3, juce::String(c.name) + " level");
            }
        }

        beginTest("STOP, old against new: the pitch falls away over TIME, the way CURV says");
        {
            struct Case { const char* name; float time, curve; };
            const Case cases[] = { { "stop_half_second", 0.5f, 0.3f }, { "stop_two_seconds_late_turn", 2.0f, 1.0f }, { "stop_quick_even", 0.1f, 0.0f } };
            for (const auto& c : cases)
            {
                auto set = [&c](EffectBase& fx) { fx.setParameter(0, c.time); fx.setParameter(1, c.curve); };
                e6old::TapeStop older; TapeStop newer;
                set(older); set(newer);
                // (The new head is two samples behind the newest, the core's shortest tap; the old one
                // read the newest itself. So the new one is handed its sound two samples early.)
                const Stereo a = played(older, chord, 0.5, 7.5), b = played(newer, early(chord), 0.5, 7.5);
                Stereo oldCut = a, newCut = b;      // (from just after the press on: the old one's first sample was not the sound's)
                for (auto* s : { &oldCut, &newCut }) { s->l.erase(s->l.begin(), s->l.begin() + (long) (0.5 * SR) + 64); s->r.erase(s->r.begin(), s->r.begin() + (long) (0.5 * SR) + 64); }
                const Compared r = report(c.name, oldCut, newCut);
                expect(finite(b.l));
                expect(r.null <= -30.0, juce::String(c.name) + ": difference " + juce::String(r.null, 1) + " dB");
                expect(rmsSec(b.l, 0.5 + c.time + 0.05, 7.0) < 1.0e-4, juce::String(c.name) + ": a stopped tape gives nothing");
            }
        }
    }

    // =================================================================================================
    // 3. What each swept filter gained from the others
    // =================================================================================================
    // The largest step between two samples in a stretch (a click is a step the sound itself does not make).
    static double largestStep(const Signal& s, double from, double to)
    {
        double step = 0.0;
        for (size_t i = (size_t) (from * SR) + 1; i < std::min(s.size(), (size_t) (to * SR)); ++i)
            step = std::max(step, (double) std::abs(s[i] - s[i - 1]));
        return step;
    }

    void testSweptFilterGains()
    {
        const Signal hiss = noise(1.0, 0.3f, 31);

        beginTest("Wah: FILT sweeps any filter of the set, a phaser, the vowels; the first entry is the wah as it was");
        {
            expectEquals(juce::String(swp::filt::entry(0).name), juce::String("S.BP"));
            expectEquals(swp::filt::entry(0).type, 0);
            expectEquals(swp::filt::entry(0).mode, 1);
            expectEquals((int) Wah().getExtraSpec(Wah::Filt).maxVal, swp::filt::COUNT - 1);
            std::vector<double> bright, level;
            std::vector<Signal> sounds;
            for (int f = 0; f < swp::filt::COUNT; ++f)
            {
                auto fx = made<Wah>([f](Wah& w) { w.setParameter(Wah::Freq, 600.0f); w.setParameter(Wah::Sens, 0.0f); w.setExtra(Wah::Filt, (float) f); });
                const Stereo out = render(*fx, hiss);
                sounds.push_back(out.l);
                expect(finite(out.l) && rms(out.l) > 0.001, juce::String(swp::filt::entry(f).name) + " sounds");
                bright.push_back(brightness(out.l, 4800));
                level.push_back(rms(out.l, 4800));
            }
            const double in = brightness(hiss);
            auto at = [](const char* name) { for (int f = 0; f < swp::filt::COUNT; ++f) if (juce::String(swp::filt::entry(f).name) == name) return (size_t) f; return (size_t) 0; };
            expect(bright[at("L24")] < 0.35 * in, "L24 at 600 Hz is a low-pass");
            expect(bright[at("L24")] < bright[at("L6")] && bright[at("S.LP")] < bright[at("S.BP")] && bright[at("S.BP")] < bright[at("S.HP")], "the modes are themselves");
            expect(bright[at("S.HP")] > 0.9 * in && bright[at("L.HP")] > 0.9 * in, "the high-passes keep the top");
            expect(std::abs(level[at("PH4")] / rms(hiss) - 1.0) > 0.03 && std::abs(level[at("PH2")] - level[at("PH6")]) > 1.0e-4, "a phaser's notches are heard, by stages");
            {
                Signal diff(hiss.size());
                for (size_t i = 0; i < diff.size(); ++i) diff[i] = sounds[at("CMB+")][i] - sounds[at("CMB-")][i];
                expect(rms(diff, 4800) > 0.1 * level[at("CMB+")], "the comb, both ways");
                // No two entries are the same sound.
                for (int a = 0; a < swp::filt::COUNT; ++a)
                    for (int b = a + 1; b < swp::filt::COUNT; ++b)
                    {
                        for (size_t i = 0; i < diff.size(); ++i) diff[i] = sounds[(size_t) a][i] - sounds[(size_t) b][i];
                        expect(rms(diff, 4800) > 0.01 * level[(size_t) a], juce::String(swp::filt::entry(a).name) + " is not " + swp::filt::entry(b).name);
                    }
            }
            // The vowels: FREQ is the vowel (150 Hz = A .. 2.4 kHz = U).
            auto vowel = [&](float hz)
            {
                auto fx = made<Wah>([hz](Wah& w) { w.setParameter(Wah::Freq, hz); w.setParameter(Wah::Sens, 0.0f); w.setExtra(Wah::Filt, (float) (swp::filt::COUNT - 1)); });
                return brightness(render(*fx, hiss).l, 4800);
            };
            expect(std::abs(vowel(150.0f) / vowel(2400.0f) - 1.0) > 0.1 && std::abs(vowel(150.0f) / vowel(600.0f) - 1.0) > 0.05, "FREQ moves through the vowels");
        }

        beginTest("Wah: FILT turned under sound with RESO full - the resonance is the new filter's from its first sample (review)");
        {
            // RESO is a Q of 12 on the band-pass, 0.9 of feedback on the phaser, 1 on a ladder: a
            // filter that came in with the last one's measure would howl for a moment.
            const Signal sound = sine(1.5, 220.0, 0.3f);
            auto turned = [&](int from, int to)
            {
                auto fx = made<Wah>([from](Wah& w) { w.setParameter(Wah::Reso, 1.0f); w.setParameter(Wah::Sens, 0.0f); w.setExtra(Wah::Filt, (float) from); });
                return render(*fx, sound, nullptr, BLOCK, [to](EffectBase& e, size_t at) { if (at >= (size_t) (0.75 * SR)) e.setExtra(Wah::Filt, (float) to); });
            };
            auto steady = [&](int filt)
            {
                auto fx = made<Wah>([filt](Wah& w) { w.setParameter(Wah::Reso, 1.0f); w.setParameter(Wah::Sens, 0.0f); w.setExtra(Wah::Filt, (float) filt); });
                const Stereo out = render(*fx, sound);
                double p = 0.0;
                for (size_t i = (size_t) (1.0 * SR); i < out.l.size(); ++i) p = std::max(p, (double) std::abs(out.l[i]));
                return p;
            };
            const int pairs[][2] = { { 0, 18 }, { 18, 0 }, { 0, 19 }, { 19, 1 }, { 0, 20 }, { 20, 0 }, { 17, 6 }, { 6, 17 }, { 0, 12 } };
            for (const auto& pair : pairs)
            {
                const Stereo out = turned(pair[0], pair[1]);
                double p = 0.0;
                for (size_t i = (size_t) (0.75 * SR); i < (size_t) (1.0 * SR); ++i) p = std::max(p, (double) std::abs(out.l[i]));
                const double most = std::max(steady(pair[0]), steady(pair[1]));
                // (Twice and a half: sharp formants that come in empty under a running sound overshoot by three quarters.)
                expect(finite(out.l) && p < 2.5 * most + 0.02,
                       juce::String(swp::filt::entry(pair[0]).name) + " to " + swp::filt::entry(pair[1]).name + ": peak " + juce::String(p, 3) + " in the exchange, " + juce::String(most, 3) + " at rest");
            }
        }

        beginTest("Wah as a phaser is heard with the dry sound at any WET; the freeze's DAMP darkens what climbs (review)");
        {
            // All-passes alone pass every frequency at its level; with the dry sound, half each,
            // the notches are there: some of the noise is gone (the two notches of four stages,
            // round 2.5 kHz; far above them the two are in step again).
            auto phased = made<Wah>([](Wah& w) { w.setParameter(Wah::Freq, 2500.0f); w.setParameter(Wah::Reso, 0.0f); w.setParameter(Wah::Sens, 0.0f); w.setExtra(Wah::Filt, 18.0f); w.setMix(1.0f); });
            const Stereo out = render(*phased, hiss);
            const double kept = rmsSec(out.l, 0.2, 1.0) / rmsSec(hiss, 0.2, 1.0);
            expect(kept > 0.5 && kept < 0.95, "PH4 at WET 100: " + juce::String(kept, 3) + " of the noise's level is left (1 = no notches)");

            // FRZ let go with SPRL full: what climbs away passes DAMP's low-pass every time round.
            Signal chord = sine(3.0, 220.0, 0.2f);
            { const Signal b = sine(3.0, 329.63, 0.2f), n = noise(3.0, 0.05f, 9); for (size_t i = 0; i < chord.size(); ++i) chord[i] += b[i] + n[i]; }
            auto tail = [&](float damp)
            {
                FreezeReverb fx;
                fx.setParameter(FreezeReverb::Spiral, 1.0f); fx.setParameter(FreezeReverb::Decay, 20.0f); fx.setExtra(FreezeReverb::Damp, damp);
                const Stereo s = played(fx, chord, 0.3, 1.0);
                return brightness(s.l, (size_t) (2.0 * SR), (size_t) (3.0 * SR));
            };
            const double open = tail(0.0f), shut = tail(1.0f);
            expect(shut < 0.7 * open, "DAMP: the tail's brightness " + juce::String(open, 3) + " open, " + juce::String(shut, 3) + " at 100");
        }

        beginTest("Wah with a ladder: the envelope and the LFO sweep it as they swept the band-pass");
        {
            auto fx = made<Wah>([](Wah& w) { w.setParameter(Wah::Freq, 300.0f); w.setParameter(Wah::Sens, 1.0f); w.setExtra(Wah::Filt, 6.0f); });
            expectEquals(juce::String(swp::filt::entry(6).name), juce::String("L24"));
            render(*fx, silence(0.2));
            const float rest = fx->getCurrentFrequency();
            const Stereo quiet = render(*fx, sine(0.3, 220.0, 0.02f));
            const float low = fx->getCurrentFrequency();
            const Stereo loud = render(*fx, sine(0.3, 220.0, 0.5f));
            const float high = fx->getCurrentFrequency();
            expectWithinAbsoluteError(rest, 300.0f, 3.0f);
            expect(high > 4.0f * rest && low < 1.5f * rest, "a loud sound opens the ladder: " + juce::String(low) + " / " + juce::String(high) + " Hz");
            // The 220 Hz tone passes the open ladder and not the shut one.
            expect(rms(loud.l, 9600) / 0.5 > 3.0 * rms(quiet.l, 9600) / 0.02 * 0.2 || rms(loud.l, 9600) > 0.2, "and the sound comes through");
        }

        beginTest("Phaser: ENV and KEY, the sweep follows a level - its own, or another track's");
        {
            const Signal pad = sine(1.0, 330.0, 0.05f), key = hits(1.0, { 0.1, 0.4, 0.7 }, 0.9f);
            auto swept = [&](float env, bool keyIn, const Signal* handed, float depth = 0.0f)
            {
                auto fx = made<Phaser>([=](Phaser& p) { p.setParameter(Phaser::Depth, depth); p.setExtra(Phaser::Env, env); p.setExtra(Phaser::Key, keyIn ? (float) dyn::KEY_CHOICE_IN : 0.0f); });
                float lo = 1.0e9f, hi = 0.0f;
                Stereo out { pad, pad };
                for (size_t at = 0; at < pad.size(); at += BLOCK)
                {
                    if (handed != nullptr && fx->wantsInput()) fx->setInput(handed->data() + at, handed->data() + at, BLOCK);
                    fx->processBlock(out.l.data() + at, out.r.data() + at, (int) std::min((size_t) BLOCK, pad.size() - at));
                    lo = std::min(lo, fx->getCurrentFrequency()); hi = std::max(hi, fx->getCurrentFrequency());
                }
                return std::make_pair(lo, hi);
            };
            const auto still = swept(0.0f, true, &key), up = swept(1.0f, true, &key), down = swept(-1.0f, true, &key), self = swept(1.0f, false, &key), alone = swept(1.0f, true, nullptr);
            expectWithinAbsoluteError(still.first, 2100.0f, 1.0f);
            expectWithinAbsoluteError(still.second, 2100.0f, 1.0f);
            expect(up.second > 4000.0f && up.first < 2400.0f, "the hits push the sweep up: " + juce::String(up.first) + " .. " + juce::String(up.second));
            expect(down.first < 500.0f && down.second <= 2101.0f, "or down");
            expect(self.second < 2600.0f, "KEY at SELF: the quiet pad's own level moves it little (" + juce::String(self.second) + ")");
            expectWithinAbsoluteError(alone.second, self.second, 1.0f);     // IN with nothing handed over is SELF
            expect(Phaser().getExtraSpec(Phaser::Key).numChoices == dyn::KEY_CHOICE_IN + 1 && ! made<Phaser>()->wantsInput() && made<Phaser>([](Phaser& p) { p.setExtra(Phaser::Key, (float) dyn::KEY_CHOICE_IN); })->wantsInput());
        }

        beginTest("Filter effect: RATE and DPTH sweep the cutoff, ENV opens or shuts it, KEY from the track's input");
        {
            auto range = [&](const std::function<void(FilterEffect&)>& set, const Signal& in, const Signal* key)
            {
                auto fx = made<FilterEffect>(set);
                float lo = 1.0e9f, hi = 0.0f;
                Stereo out { in, in };
                for (size_t at = 0; at + BLOCK <= in.size(); at += BLOCK)
                {
                    if (key != nullptr && fx->wantsInput()) fx->setInput(key->data() + at, key->data() + at, BLOCK);
                    fx->processBlock(out.l.data() + at, out.r.data() + at, BLOCK);
                    lo = std::min(lo, fx->getCurrentFrequency()); hi = std::max(hi, fx->getCurrentFrequency());
                }
                return std::make_pair(lo, hi);
            };
            const Signal tone = sine(1.5, 220.0, 0.5f), soft = sine(1.5, 220.0, 0.01f), key = hits(1.5, { 0.2, 0.7, 1.2 }, 0.9f);
            const auto rest = range([](FilterEffect& f) { f.setParameter(0, 1000.0f); }, tone, nullptr);
            expectWithinAbsoluteError(rest.first, 1000.0f, 0.5f);
            expectWithinAbsoluteError(rest.second, 1000.0f, 0.5f);
            const auto lfo = range([](FilterEffect& f) { f.setParameter(0, 1000.0f); f.setExtra(FilterEffect::Rate, 2.0f); f.setExtra(FilterEffect::Depth, 0.5f); }, tone, nullptr);
            expectWithinAbsoluteError(std::log2(lfo.second / lfo.first), 2.0f * 0.5f * FilterEffect::LFO_OCTAVES, 0.15f);
            const auto shut = range([](FilterEffect& f) { f.setParameter(0, 4000.0f); f.setExtra(FilterEffect::Env, -1.0f); }, tone, nullptr);
            expect(shut.first < 4000.0f / 16.0f, "a loud sound shuts the filter with ENV down: " + juce::String(shut.first));
            const auto open = range([](FilterEffect& f) { f.setParameter(0, 200.0f); f.setExtra(FilterEffect::Env, 0.6f); f.setExtra(FilterEffect::Key, (float) dyn::KEY_CHOICE_IN); }, soft, &key);
            const auto notKeyed = range([](FilterEffect& f) { f.setParameter(0, 200.0f); f.setExtra(FilterEffect::Env, 0.6f); }, soft, &key);
            expect(open.second > 200.0f * 6.0f && notKeyed.second < 200.0f * 1.3f, "another track's hits open it: " + juce::String(open.second) + " against " + juce::String(notKeyed.second));
        }

        beginTest("Filter effect: MODE's list is the type's; a type or mode changed under sound is exchanged without a click, CUT glides");
        {
            FilterEffect fx;
            fx.setParameter(FilterEffect::Type, 2.0f);
            expectEquals((int) fx.getParamSpec(3).maxVal, 5);
            fx.setParameter(FilterEffect::ModeParam, 5.0f);
            fx.setParameter(FilterEffect::Type, 1.0f);
            expectEquals((int) fx.getParamSpec(3).maxVal, 1);
            expectEquals((int) fx.getParameter(FilterEffect::ModeParam), 1);
            expectEquals(juce::String(fx.getParamSpec(3).choices[1]), juce::String("BP"));

            const Signal tone = sine(1.0, 220.0, 0.5f);
            const double own = largestStep(tone, 0.0, 1.0);
            struct Move { const char* what; int cell; float from, to; };
            for (const Move& m : { Move { "TYPE SVF to ladder", 2, 0.0f, 2.0f }, Move { "TYPE ladder to comb", 2, 2.0f, 4.0f }, Move { "MODE low-pass to high-pass", 3, 0.0f, 2.0f },
                                   Move { "CUT end to end", 0, 20000.0f, 60.0f }, Move { "RESO end to end", 1, 0.0f, 1.0f } })
            {
                auto f = made<FilterEffect>([&m](FilterEffect& e) { e.setParameter(0, 3000.0f); e.setParameter(m.cell, m.from); });
                const Stereo out = render(*f, tone, nullptr, BLOCK, [&m](EffectBase& e, size_t at) { if (at == 24000) e.setParameter(m.cell, m.to); });
                expect(finite(out.l));
                expect(largestStep(out.l, 0.49, 0.6) < 2.5 * std::max(own, largestStep(out.l, 0.8, 1.0)), juce::String(m.what) + ": step " + juce::String(largestStep(out.l, 0.49, 0.6), 4)
                       + " against the tone's own " + juce::String(own, 4));
                expect(rmsSec(out.l, 0.8, 1.0) > 1.0e-4 || m.cell == 0 || m.cell == 3, juce::String(m.what) + ": the new filter is there");
            }
        }

        beginTest("TALK is on the same core: the shared LFO's shapes reach the vowels");
        {
            auto spoken = [&](int shape)
            {
                TalkBlock t;
                t.set(TalkBlock::Mix, 1.0f); t.set(TalkBlock::Vowel, 0.5f); t.set(TalkBlock::LfoDepth, 1.0f); t.set(TalkBlock::LfoRate, 3.0f);
                t.setLfoShape(shape);
                t.prepare(SR, 512);
                return renderTalk(t, noise(1.0, 0.3f, 5));
            };
            const Stereo sineSwept = spoken(0), squareSwept = spoken(2);
            Signal diff(sineSwept.l.size());
            for (size_t i = 0; i < diff.size(); ++i) diff[i] = squareSwept.l[i] - sineSwept.l[i];
            expect(finite(squareSwept.l) && rms(diff) > 0.05 * rms(sineSwept.l), "SQR: two vowels in turn");
            TalkBlock off;
            Stereo untouched { hiss, hiss };
            off.process(untouched.l.data(), untouched.r.data(), (int) hiss.size());
            expect(untouched.l == hiss, "at MIX 0 the block is not in the path");
        }
    }

    // =================================================================================================
    // 4. The LFO's trigger modes, delay and fade, triggered by the key's onsets
    // =================================================================================================
    static int onsetsIn(const Signal& s)
    {
        fxm::Onset onset;
        dyn::Detector detector;
        onset.prepare(SR);
        detector.prepare(SR);
        int count = 0;
        for (float v : s) count += onset.hears(detector.level(v, v)) ? 1 : 0;
        return count;
    }

    // A tremolo keyed from `key`; the level it leaves on a steady sound, a window of 10 ms from `at` seconds.
    Stereo tremoloUnder(const std::function<void(Tremolo&)>& set, const Signal& in, const Signal* key, int block = BLOCK)
    {
        auto fx = made<Tremolo>(set);
        return render(*fx, in, key, block);
    }
    static double levelAt(const Stereo& s, double at) { return rmsSec(s.l, at, at + 0.01) / (0.5 / std::sqrt(2.0)); }

    void testTriggers()
    {
        beginTest("An onset: a sound out of silence or a jump of 4 dB; not a low note's every period, not legato at one level");
        {
            expectEquals(onsetsIn(sine(2.0, 440.0, 0.3f)), 1);
            expectEquals(onsetsIn(sine(2.0, 40.0, 0.3f)), 1);
            expectEquals(onsetsIn(hits(3.0, { 0.1, 0.6, 1.1, 1.35, 1.6, 2.1 })), 6);
            expectEquals(onsetsIn(hits(3.0, { 0.1, 0.225, 0.35, 0.475, 0.6, 0.725, 0.85, 0.975 })), 8);    // sixteenths at 120 BPM
            {
                // Hits over a held note are onsets, the note is not one more.
                Signal mixed = hits(3.0, { 0.5, 1.0, 1.5, 2.0 });
                const Signal under = sine(3.0, 220.0, 0.15f);
                for (size_t i = 0; i < mixed.size(); ++i) mixed[i] += under[i];
                expectEquals(onsetsIn(mixed), 5);
                // A slow tremolo of 2 dB is one sound.
                Signal wavering = sine(3.0, 440.0, 0.3f);
                for (size_t i = 0; i < wavering.size(); ++i) wavering[i] *= 1.0f + 0.12f * (float) std::sin(6.2831853 * 3.0 * (double) i / SR);
                expectEquals(onsetsIn(wavering), 1);
            }
            Signal legato = sine(0.5, 220.0, 0.3f);
            append(legato, sine(0.5, 330.0, 0.3f));
            expectEquals(onsetsIn(legato), 1);
            Signal accent = sine(0.5, 220.0, 0.1f);
            append(accent, sine(0.5, 330.0, 0.5f));
            expectEquals(onsetsIn(accent), 2);
            Signal twice = sine(0.4, 220.0, 0.3f);
            append(twice, silence(0.6));
            append(twice, sine(0.4, 220.0, 0.3f));
            expectEquals(onsetsIn(twice), 2);
            expectEquals(onsetsIn(silence(1.0)), 0);
            expectEquals(onsetsIn(noise(1.0, 0.0005f)), 0);
        }

        const Signal steady = sine(2.4, 1000.0, 0.5f);
        const std::initializer_list<double> times = { 0.31, 0.83, 1.47 };
        const Signal key = hits(2.4, times, 0.9f);
        auto plucked = [](int trig) { return [trig](Tremolo& t)
        {
            t.setParameter(Tremolo::Rate, 1.0f); t.setParameter(Tremolo::Depth, 1.0f); t.setParameter(Tremolo::Shape, (float) (int) LFO::Shape::ExpDown);
            t.setExtra(Tremolo::Trig, (float) trig); t.setExtra(Tremolo::Key, (float) dyn::KEY_CHOICE_IN);
        }; };

        beginTest("Tremolo, TRIG KEY with KEY at IN: the cycle starts again on every hit of the other track");
        {
            const Stereo keyed = tremoloUnder(plucked((int) LFO::TriggerMode::KeySync), steady, &key);
            const Stereo free = tremoloUnder(plucked((int) LFO::TriggerMode::Free), steady, &key);
            for (double t : times)
            {
                expect(levelAt(keyed, t + 0.012) > 0.8, "full level just after the hit at " + juce::String(t) + ": " + juce::String(levelAt(keyed, t + 0.012), 2));
                expectWithinAbsoluteError(levelAt(keyed, t + 0.4), 0.13, 0.05);     // EXPD two fifths of a second on
            }
            expect(levelAt(free, 0.31 + 0.012) < 0.5, "FREE: the LFO does not know about the hits");
            expectWithinAbsoluteError(levelAt(keyed, 1.47 + 0.8), 0.006, 0.02);     // and it runs on by itself: nearly down at the end of the cycle
            expectWithinAbsoluteError(levelAt(keyed, 1.47 + 0.9), 0.0, 0.01);
        }

        beginTest("Tremolo, TRIG ONE / HALF / HOLD: one cycle and stop; half of one; the level kept until the next hit");
        {
            const Stereo once = tremoloUnder(plucked((int) LFO::TriggerMode::Single), steady, &key);
            const Stereo keyed = tremoloUnder(plucked((int) LFO::TriggerMode::KeySync), steady, &key);
            // 1.1 s after the first hit and its... (the second hit at 0.83 started another): after the last, at 1.47 + 1.2:
            // KEY has begun a new cycle by itself (full level again), ONE has stopped at the cycle's end (shut).
            Signal longKey = key; longKey.resize(steady.size(), 0.0f);
            expect(levelAt(keyed, 1.47 + 0.87) < 0.05 && levelAt(once, 1.47 + 0.87) < 0.05);
            const Signal later = sine(3.4, 1000.0, 0.5f);
            Signal key2 = hits(3.4, times, 0.9f);
            const Stereo once2 = tremoloUnder(plucked((int) LFO::TriggerMode::Single), later, &key2);
            const Stereo keyed2 = tremoloUnder(plucked((int) LFO::TriggerMode::KeySync), later, &key2);
            expect(levelAt(keyed2, 1.47 + 1.05) > 0.5, "KEY: the next cycle begins by itself");
            expect(levelAt(once2, 1.47 + 1.05) < 0.02 && levelAt(once2, 1.47 + 1.6) < 0.02, "ONE: it stays shut until the next hit");

            auto half = [](Tremolo& t)
            {
                t.setParameter(Tremolo::Rate, 1.0f); t.setParameter(Tremolo::Depth, 1.0f); t.setParameter(Tremolo::Shape, (float) (int) LFO::Shape::Saw);
                t.setExtra(Tremolo::Trig, (float) (int) LFO::TriggerMode::Half); t.setExtra(Tremolo::Key, (float) dyn::KEY_CHOICE_IN);
            };
            const Stereo halved = tremoloUnder(half, later, &key2);
            expectWithinAbsoluteError(levelAt(halved, 1.47 + 0.25), 0.25, 0.05);    // the saw a quarter up
            expectWithinAbsoluteError(levelAt(halved, 1.47 + 0.9), 0.5, 0.03);      // and stopped half way
            expectWithinAbsoluteError(levelAt(halved, 1.47 + 1.7), 0.5, 0.03);

            auto hold = [](Tremolo& t)
            {
                t.setParameter(Tremolo::Rate, 3.3f); t.setParameter(Tremolo::Depth, 1.0f); t.setParameter(Tremolo::Shape, 0.0f);
                t.setExtra(Tremolo::Trig, (float) (int) LFO::TriggerMode::Hold); t.setExtra(Tremolo::Key, (float) dyn::KEY_CHOICE_IN);
            };
            const Stereo heldLevel = tremoloUnder(hold, later, &key2);
            const double first = levelAt(heldLevel, 0.31 + 0.05), second = levelAt(heldLevel, 0.83 + 0.05), third = levelAt(heldLevel, 1.47 + 0.05);
            expectWithinAbsoluteError(levelAt(heldLevel, 0.31 + 0.4), first, 0.01);
            expectWithinAbsoluteError(levelAt(heldLevel, 1.47 + 1.5), third, 0.01);
            expect(std::abs(first - second) > 0.02 || std::abs(second - third) > 0.02, "another level from another hit");
        }

        beginTest("Tremolo, DLAY and FADE: after an onset the level is left alone, then the tremolo grows");
        {
            auto delayed = [](Tremolo& t)
            {
                t.setParameter(Tremolo::Rate, 20.0f); t.setParameter(Tremolo::Depth, 1.0f);
                t.setExtra(Tremolo::Delay, 0.3f); t.setExtra(Tremolo::Fade, 0.4f); t.setExtra(Tremolo::Key, (float) dyn::KEY_CHOICE_IN);
            };
            const Stereo out = tremoloUnder(delayed, steady, &key);
            // The level's wander over a stretch: its lowest 10 ms against its highest.
            auto wander = [&](double from, double to)
            {
                double lo = 9.0, hi = 0.0;
                for (double t = from; t < to; t += 0.005) { lo = std::min(lo, levelAt(out, t)); hi = std::max(hi, levelAt(out, t)); }
                return hi - lo;
            };
            expect(wander(1.47 + 0.03, 1.47 + 0.28) < 0.03, "during DLAY nothing moves: " + juce::String(wander(1.47 + 0.03, 1.47 + 0.28), 3));
            const double mid = wander(1.47 + 0.45, 1.47 + 0.55), full = wander(1.47 + 0.75, 1.47 + 0.9);
            expect(mid > 0.1 && mid < 0.75 * full && full > 0.6, "then it grows: " + juce::String(mid, 2) + " then " + juce::String(full, 2));
            // (With SPRD the right side fades with the left.)
            auto fx = made<Tremolo>([&](Tremolo& t) { delayed(t); t.setParameter(Tremolo::StereoOffset, 0.25f); });
            const Stereo wide = render(*fx, steady, &key);
            expect(std::abs(rmsSec(wide.r, 1.47 + 0.05, 1.47 + 0.28) / (0.5 / std::sqrt(2.0)) - 0.5) < 0.03, "the right side waits too");
        }

        beginTest("KEY at IN with nothing handed over is SELF; whatever the block size the same sound");
        {
            Signal gated = sine(0.5, 1000.0, 0.5f);
            append(gated, silence(0.5));
            append(gated, sine(1.0, 1000.0, 0.5f));
            const Stereo self = tremoloUnder(plucked((int) LFO::TriggerMode::Single), gated, nullptr);
            expect(levelAt(self, 1.0 + 0.012) > 0.8 && levelAt(self, 1.0 + 0.5) < 0.12, "its own second note starts the cycle");
            auto fx = made<Tremolo>(plucked((int) LFO::TriggerMode::KeySync));
            expect(fx->wantsInput() && ! fx->keyConnected());
            const Stereo reference = tremoloUnder(plucked((int) LFO::TriggerMode::KeySync), steady, &key, 64);
            for (int block : { 1, 7, 500, 4096 })
            {
                const Stereo other = tremoloUnder(plucked((int) LFO::TriggerMode::KeySync), steady, &key, block);
                double worst = 0.0;
                for (size_t i = 0; i < reference.l.size(); ++i) worst = std::max(worst, (double) std::abs(other.l[i] - reference.l[i]));
                expect(worst < 1.0e-5, "block " + juce::String(block) + ": " + juce::String(worst, 6));
            }
        }

        beginTest("Wah, auto-pan, vibrato: the same trigger");
        {
            // Wah: a saw sweep starts again on every hit (the frequency just after each hit is the same one).
            auto wahAt = [&](int trig)
            {
                auto fx = made<Wah>([trig](Wah& w)
                {
                    w.setParameter(Wah::Sens, 0.0f); w.setParameter(Wah::Rate, 1.3f); w.setExtra(Wah::Shape, 3.0f);
                    w.setExtra(Wah::Trig, (float) trig); w.setExtra(Wah::KEY_EXTRA, (float) dyn::KEY_CHOICE_IN);
                });
                std::vector<float> seen;
                Stereo out { steady, steady };
                size_t next = 0;
                const std::vector<double> when(times);
                for (size_t at = 0; at + BLOCK <= steady.size(); at += BLOCK)
                {
                    fx->setInput(key.data() + at, key.data() + at, BLOCK);
                    fx->processBlock(out.l.data() + at, out.r.data() + at, BLOCK);
                    if (next < when.size() && (double) at / SR >= when[next] + 0.05) { seen.push_back(fx->getCurrentFrequency()); ++next; }
                }
                return seen;
            };
            const auto keyed = wahAt((int) LFO::TriggerMode::KeySync), free = wahAt((int) LFO::TriggerMode::Free);
            expect(keyed.size() == 3 && std::abs(keyed[1] / keyed[0] - 1.0f) < 0.03f && std::abs(keyed[2] / keyed[0] - 1.0f) < 0.03f,
                   "the sweep is at the same place after every hit: " + juce::String(keyed[0]) + " " + juce::String(keyed[1]) + " " + juce::String(keyed[2]));
            expect(std::abs(free[1] / free[0] - 1.0f) > 0.1f || std::abs(free[2] / free[0] - 1.0f) > 0.1f, "FREE: wherever it happens to be");

            // Auto-pan, HOLD: every hit puts the sound at a place that is kept.
            auto pan = made<AutoPan>([](AutoPan& p)
            {
                p.setParameter(AutoPan::Rate, 3.3f); p.setParameter(AutoPan::Depth, 1.0f);
                p.setExtra(AutoPan::Trig, (float) (int) LFO::TriggerMode::Hold); p.setExtra(AutoPan::Key, (float) dyn::KEY_CHOICE_IN);
            });
            const Stereo panned = render(*pan, steady, &key);
            auto side = [&](double at) { return rmsSec(panned.l, at, at + 0.01) / std::max(1.0e-9, rmsSec(panned.r, at, at + 0.01)); };
            expectWithinAbsoluteError(side(0.31 + 0.05) / side(0.31 + 0.45), 1.0, 0.02);
            expectWithinAbsoluteError(side(1.47 + 0.05) / side(1.47 + 0.85), 1.0, 0.02);
            expect(std::abs(side(0.31 + 0.05) / side(1.47 + 0.05) - 1.0) > 0.05 || std::abs(side(0.83 + 0.05) / side(1.47 + 0.05) - 1.0) > 0.05, "another place from another hit");

            // Vibrato, ONE: a single bend after the onset, then the pitch stands.
            auto vib = made<Vibrato>([](Vibrato& v) { v.setParameter(Vibrato::Rate, 4.0f); v.setParameter(Vibrato::Depth, 100.0f); v.setExtra(Vibrato::Trig, (float) (int) LFO::TriggerMode::Single); });
            const Stereo bent = render(*vib, sine(2.0, 440.0, 0.4f));
            const double early = std::abs(crossings(bent.l, (size_t) (0.05 * SR), (size_t) (0.15 * SR)) - crossings(bent.l, (size_t) (0.15 * SR), (size_t) (0.25 * SR)));
            const double late = std::abs(crossings(bent.l, (size_t) (1.0 * SR), (size_t) (1.1 * SR)) - crossings(bent.l, (size_t) (1.12 * SR), (size_t) (1.22 * SR)));
            expect(early >= 10.0 && late < 10.0, "the pitch moves in the first cycle (" + juce::String(early) + " Hz) and not after it (" + juce::String(late) + ")");
            // DLAY: the vibrato waits.
            auto waiting = made<Vibrato>([](Vibrato& v) { v.setParameter(Vibrato::Rate, 6.0f); v.setParameter(Vibrato::Depth, 100.0f); v.setExtra(Vibrato::Delay, 0.5f); });
            const Stereo waited = render(*waiting, sine(2.0, 440.0, 0.4f));
            auto wobble = [&](double from, double to)
            {
                double lo = 1.0e9, hi = 0.0;
                for (double t = from; t + 0.04 <= to; t += 0.02) { const double f = crossings(waited.l, (size_t) (t * SR), (size_t) ((t + 0.04) * SR)); lo = std::min(lo, f); hi = std::max(hi, f); }
                return hi - lo;
            };
            expect(wobble(0.05, 0.45) < 26.0 && wobble(1.0, 1.9) >= 50.0, "DLAY: " + juce::String(wobble(0.05, 0.45)) + " then " + juce::String(wobble(1.0, 1.9)));
        }
    }

    // =================================================================================================
    // 5. One list of the ways a line is played; a tape that stops, a loop that is held
    // =================================================================================================
    void testWaysOfPlaying()
    {
        beginTest("One list: the delays' READ is it, the reverbs' MODE a view of it that keeps its stored numbers");
        {
            const char* const all[] = { "FWD", "REV", "OCT+", "5TH+", "OCT-", "O+5", "SUB", "HOLD", "STOP" };
            expectEquals((int) dly::read::Count, 9);
            for (int i = 0; i < dly::read::Count; ++i)
                expectEquals(juce::String(dly::read::names()[i]), juce::String(all[i]));
            expectEquals((int) TapeDelay().getExtraSpec(TapeDelay::Read).maxVal, 8);
            const float was[] = { 2.0f, 1.4983071f, 2.9966142f, 0.5f };
            for (int i = 0; i < 4; ++i) expectEquals(rvb::shift::speedOf(i), was[i]);
            const char* const view[] = { "OCT+", "5TH+", "O+5", "OCT-", "REV", "SUB", "HOLD" };
            expectEquals((int) rvb::shift::Count, 7);
            for (int i = 0; i < rvb::shift::Count; ++i)
                expectEquals(juce::String(rvb::shift::names()[i]), juce::String(view[i]));
            expectEquals(rvb::shift::speedOf(rvb::shift::Reverse), -1.0f);
            expectEquals(rvb::shift::speedOf(rvb::shift::TwoDown), 0.25f);
            expectEquals(rvb::shift::speedOf(rvb::shift::Hold), 0.0f);
            expectEquals((int) SpringReverb().getExtraSpec(SpringReverb::ShiftMode).maxVal, 6);
        }

        Signal burst = sine(0.25, 440.0, 0.4f);
        for (size_t i = 0; i < burst.size(); ++i) burst[i] *= (float) std::sin(3.14159265 * (double) i / (double) burst.size());
        Signal once = burst;
        append(once, silence(2.75));

        beginTest("O+5 and SUB on a delay: every repeat an octave and a fifth up, two octaves down");
        {
            auto pitchOf = [&](int read)
            {
                auto fx = made<TapeDelay>([read](TapeDelay& t) { t.setParameter(TapeDelay::Time, 0.5f); t.setParameter(TapeDelay::Feedback, 0.5f); t.setParameter(2, 0.0f); t.setExtra(TapeDelay::Read, (float) read); t.setMix(1.0f); });
                const Stereo out = render(*fx, once);
                return crossings(out.l, (size_t) (0.56 * SR), (size_t) (0.70 * SR));
            };
            expectWithinAbsoluteError(pitchOf(dly::read::Forward), 440.0, 10.0);
            expectWithinAbsoluteError(pitchOf(dly::read::OctaveFifth), 1318.5, 90.0);
            expectWithinAbsoluteError(pitchOf(dly::read::Sub), 110.0, 30.0);
        }

        // A delay with its wet sound alone: tone A, then (from 0.6 s) `then`; READ becomes `read` at 0.5 s and FWD again at `back`.
        auto tape = [&](int read, const Signal& then, double back = 99.0)
        {
            Signal in = sine(0.5, 330.0, 0.4f);
            append(in, silence(0.1));
            append(in, then);
            auto fx = made<TapeDelay>([](TapeDelay& t) { t.setParameter(TapeDelay::Time, 0.25f); t.setParameter(TapeDelay::Feedback, 0.8f); t.setParameter(2, 0.0f); t.setMix(1.0f); });
            return render(*fx, in, nullptr, BLOCK, [=](EffectBase& e, size_t at)
            {
                if (at == 24000) e.setExtra(TapeDelay::Read, (float) read);
                if (at == (size_t) (back * SR) / BLOCK * BLOCK) e.setExtra(TapeDelay::Read, 0.0f);
            });
        };

        beginTest("READ at HOLD: the loop is shut and repeats what it holds; nothing new comes in");
        {
            const Signal other = sine(2.4, 1500.0, 0.4f), nothing = silence(2.4);
            const Stereo heldWith = tape(dly::read::Hold, other), heldWithout = tape(dly::read::Hold, nothing), open = tape(dly::read::Forward, other);
            Signal diff(heldWith.l.size());
            for (size_t i = 0; i < diff.size(); ++i) diff[i] = heldWith.l[i] - heldWithout.l[i];
            expect(rmsSec(diff, 0.7, 3.0) < 0.02 * rmsSec(heldWithout.l, 0.7, 3.0), "the new sound is not in the loop: " + juce::String(rmsSec(diff, 0.7, 3.0) / rmsSec(heldWithout.l, 0.7, 3.0), 4));
            expect(rmsSec(heldWith.l, 1.0, 1.5) > 0.02 && rmsSec(heldWith.l, 2.0, 2.5) > 0.3 * rmsSec(heldWith.l, 1.0, 1.5), "and what it held comes round, as FDBK lets it");
            expect(brightness(open.l, (size_t) (2.0 * SR), (size_t) (2.5 * SR)) > 2.0 * brightness(heldWith.l, (size_t) (2.0 * SR), (size_t) (2.5 * SR)), "(read forward, the new sound is in it)");
            // Back to FWD: the new sound comes in again.
            const Stereo backAgain = tape(dly::read::Hold, other, 1.5);
            expect(brightness(backAgain.l, (size_t) (2.5 * SR), (size_t) (3.0 * SR)) > 2.0 * brightness(heldWith.l, (size_t) (2.5 * SR), (size_t) (3.0 * SR)));
            expect(largestStep(heldWith.l, 0.49, 0.56) < 3.0 * largestStep(heldWith.l, 0.3, 0.45), "shut without a click");
        }

        beginTest("READ at STOP: the echoes slow to a standstill, falling in pitch; read another way, the tape runs again");
        {
            const Signal nothing = silence(2.4), again = sine(2.4, 330.0, 0.4f);
            const Stereo stopped = tape(dly::read::Stop, again), running = tape(dly::read::Forward, again);
            expect(finite(stopped.l));
            const double before = crossings(stopped.l, (size_t) (0.42 * SR), (size_t) (0.5 * SR));
            const double falling = crossings(stopped.l, (size_t) (0.7 * SR), (size_t) (0.8 * SR)), low = crossings(stopped.l, (size_t) (0.9 * SR), (size_t) (1.0 * SR));
            expect(std::abs(before - 330.0) < 15.0 && falling < 0.85 * before && low < 0.75 * falling, "the pitch falls: " + juce::String(before) + ", " + juce::String(falling) + ", " + juce::String(low) + " Hz");
            expect(rmsSec(stopped.l, 1.25, 3.0) < 1.0e-4 && rmsSec(running.l, 1.25, 3.0) > 0.05, "a stopped tape gives nothing");
            const Stereo restarted = tape(dly::read::Stop, again, 1.8);
            expect(rmsSec(restarted.l, 2.3, 3.0) > 0.05, "FWD again: the echoes are back");
            expect(std::abs(crossings(restarted.l, (size_t) (2.5 * SR), (size_t) (2.9 * SR)) - 330.0) < 10.0, "at their own pitch");
            expect(largestStep(restarted.l, 1.79, 1.9) < 3.0 * largestStep(running.l, 2.5, 2.9), "started without a click");
            juce::ignoreUnused(nothing);
        }

        beginTest("MODE at HOLD on a reverb: the loops keep what they have, nothing comes in, nothing is lost");
        {
            for (int which = 0; which < 2; ++which)
            {
                const int cell = which == 0 ? 4 + (int) SpringReverb::ShiftMode : (int) ShimmerReverb::ShiftMode;
                auto build = [which]() -> std::unique_ptr<EffectBase>
                {
                    std::unique_ptr<EffectBase> fx;
                    if (which == 0) fx = std::make_unique<SpringReverb>(); else fx = std::make_unique<ShimmerReverb>();
                    fx->setParameter(0, 1.0f);      // DCAY 1 s
                    fx->setMix(1.0f);
                    fx->prepare(SR, 512);
                    fx->reset();
                    return fx;
                };
                auto setMode = [cell](EffectBase& e, float mode) { if (cell >= 4) e.setExtra(cell - 4, mode); else e.setParameter(cell, mode); };
                Signal in = noise(1.0, 0.3f, 77);
                append(in, silence(0.5));
                Signal late = in;
                append(in, silence(8.5));
                append(late, sine(2.0, 1200.0, 0.4f));
                append(late, silence(6.5));
                auto run = [&](const Signal& sound, bool hold, double release = 99.0)
                {
                    auto fx = build();
                    return render(*fx, sound, nullptr, BLOCK, [&](EffectBase& e, size_t at)
                    {
                        if (hold && at == 48000) setMode(e, (float) rvb::shift::Hold);
                        if (at == (size_t) (release * SR) / BLOCK * BLOCK) setMode(e, 0.0f);
                    });
                };
                const juce::String name = which == 0 ? "SPRG" : "SHIM";
                const Stereo held = run(in, true), free = run(in, false), heldLate = run(late, true), released = run(in, true, 5.0);
                const double kept = db(rmsSec(held.l, 8.0, 9.5) / rmsSec(held.l, 2.0, 3.5)), lost = db(rmsSec(free.l, 8.0, 9.5) / std::max(1.0e-9, rmsSec(free.l, 2.0, 3.5)));
                if (printing()) logMessage("E6 |   " + name + " MODE HOLD: 6 s on the held sound is at " + juce::String(kept, 2) + " dB; not held " + juce::String(lost, 1) + " dB");
                expect(finite(held.l) && rmsSec(held.l, 8.0, 9.5) > 0.003, name + " holds");
                expect(kept > -6.0 && kept < 1.0, name + ": held, the level stays (" + juce::String(kept, 2) + " dB after 6 s)");
                expect(lost < -40.0, name + ": not held it is gone");
                Signal diff(held.l.size());
                for (size_t i = 0; i < diff.size(); ++i) diff[i] = heldLate.l[i] - held.l[i];
                expect(rmsSec(diff, 1.5, 9.5) < 0.02 * rmsSec(held.l, 1.5, 9.5), name + ": nothing comes in while it is held");
                expect(db(rmsSec(released.l, 8.5, 9.5) / rmsSec(released.l, 3.5, 4.5)) < -30.0, name + ": let go, it dies away as it is set");
                expect(std::abs(db(rmsSec(held.l, 1.5, 1.6) / rmsSec(free.l, 0.9, 1.0))) < 6.0, name + ": held at the level it had");
            }
        }

        beginTest("MODE at REV and SUB on a reverb, MODE on the freeze: read backwards, two octaves down; they stay reverbs");
        {
            Signal in = burst;
            append(in, silence(11.75));
            auto tail = [&](int mode)
            {
                auto fx = made<ShimmerReverb>([mode](ShimmerReverb& s) { s.setParameter(ShimmerReverb::Decay, 2.0f); s.setParameter(ShimmerReverb::Shimmer, 1.0f); s.setParameter(ShimmerReverb::ShiftMode, (float) mode); s.setMix(1.0f); });
                return render(*fx, in);
            };
            const Stereo up = tail(rvb::shift::Octave), back = tail(rvb::shift::Reverse), down = tail(rvb::shift::TwoDown);
            for (const Stereo* s : { &up, &back, &down })
                expect(finite(s->l) && rmsSec(s->l, 0.3, 1.3) > 0.003 && rmsSec(s->l, 11.0, 12.0) < 0.01 * rmsSec(s->l, 0.3, 1.3), "sounds, and dies away");
            const double bUp = brightness(up.l, (size_t) (0.5 * SR), (size_t) (1.5 * SR)), bBack = brightness(back.l, (size_t) (0.5 * SR), (size_t) (1.5 * SR)),
                         bDown = brightness(down.l, (size_t) (0.5 * SR), (size_t) (1.5 * SR));
            if (printing()) logMessage(juce::String::formatted("E6 |   SHIM reverb, SHIM 100, brightness of the tail: OCT+ %.3f, REV %.3f, SUB %.3f", bUp, bBack, bDown));
            expect(bDown < 0.6 * bBack && bBack < 0.8 * bUp, "SUB sinks, OCT+ climbs, REV stays where the note was");
            {
                // Backwards is not the plain tail: with SHIM at 0 the same reverb gives another sound.
                auto plainFx = made<ShimmerReverb>([](ShimmerReverb& r) { r.setParameter(ShimmerReverb::Decay, 2.0f); r.setParameter(ShimmerReverb::Shimmer, 0.0f); r.setMix(1.0f); });
                const Stereo plain = render(*plainFx, in);
                Signal diff(plain.l.size());
                for (size_t i = 0; i < diff.size(); ++i) diff[i] = back.l[i] - plain.l[i];
                expect(rmsSec(diff, 0.3, 2.0) > 0.5 * rmsSec(plain.l, 0.3, 2.0), "REV is read backwards: " + juce::String(rmsSec(diff, 0.3, 2.0) / rmsSec(plain.l, 0.3, 2.0), 2));
                expect(std::abs(db(rmsSec(back.l, 0.3, 2.0) / rmsSec(plain.l, 0.3, 2.0))) < 6.0, "at the plain tail's level");
            }

            FreezeReverb frz;
            expectEquals((int) frz.getExtraSpec(FreezeReverb::Mode).maxVal, (int) rvb::shift::Hold - 1);
            Signal chord = sine(6.0, 220.0, 0.3f);
            auto caught = [&](int mode)
            {
                FreezeReverb fx;
                fx.setParameter(FreezeReverb::Shimmer, 0.0f); fx.setParameter(FreezeReverb::Spiral, 1.0f); fx.setExtra(FreezeReverb::Mode, (float) mode);
                return played(fx, chord, 0.5, 2.0);
            };
            const Stereo climbs = caught(rvb::shift::Octave), sinks = caught(rvb::shift::Sub), turned = caught(rvb::shift::Reverse);
            const size_t a = (size_t) (2.4 * SR), b = (size_t) (2.9 * SR);
            expect(finite(turned.l) && rmsSec(turned.l, 2.2, 2.7) > 0.001);
            expect(brightness(climbs.l, a, b) > 1.5 * brightness(turned.l, a, b) && brightness(sinks.l, a, b) < 0.7 * brightness(turned.l, a, b),
                   "let go, the frozen sound climbs, sinks, or stays and is turned round");
        }
    }

    // =================================================================================================
    // 6. A time-like cell follows the tempo; one list of divisions
    // =================================================================================================
    // An effect with nothing but a time in milliseconds on its second page.
    struct MillisecondEffect : EffectBase
    {
        float ms = 10.0f;
        void reset() override {}
        void process(float&, float&) override {}
        ParamSpec getExtraSpec(int index) const override { return index == 2 ? ParamSpec { "GAP", 2, 1.0f, 2000.0f, 10.0f, ParamKind::Log, "ms" }.timed() : ParamSpec(); }
        void setExtra(int index, float value) override { if (index == 2) ms = value; }
        float getExtra(int index) const override { return index == 2 ? ms : 0.0f; }
    };

    void testTimeLike()
    {
        beginTest("A time-like cell can be locked to the tempo wherever it is: a rate on the second page, seconds, milliseconds");
        {
            FilterEffect flt;
            expect(flt.getExtraSpec(FilterEffect::Rate).timeLike && ! flt.getParamSpec(0).timeLike);
            expectEquals(flt.getSyncParamIndex(), 4 + (int) FilterEffect::Rate);
            flt.setExtra(FilterEffect::Rate, 0.7f);
            flt.setTempo(120.0f);
            flt.setSyncDivision(FxSync::DEFAULT_DIVISION);
            flt.setSync(true);
            expectWithinAbsoluteError(flt.getExtra(FilterEffect::Rate), 2.0f, 1.0e-4f);     // a quarter note at 120
            flt.setTempo(90.0f);
            expectWithinAbsoluteError(flt.getExtra(FilterEffect::Rate), 1.5f, 1.0e-4f);
            flt.setSyncDivision(FxSync::DEFAULT_DIVISION - 3);                              // 1/8
            expectWithinAbsoluteError(flt.getExtra(FilterEffect::Rate), 3.0f, 1.0e-4f);
            flt.setUserExtra(FilterEffect::Rate, 5.0f);                                     // the knob's own value is kept aside
            expectWithinAbsoluteError(flt.getExtra(FilterEffect::Rate), 3.0f, 1.0e-4f);
            expectWithinAbsoluteError(flt.getUserExtra(FilterEffect::Rate), 5.0f, 1.0e-4f);
            flt.setSync(false);
            expectWithinAbsoluteError(flt.getExtra(FilterEffect::Rate), 5.0f, 1.0e-4f);

            FreezeReverb frz;
            expectEquals(frz.getSyncParamIndex(), (int) FreezeReverb::Grab);
            frz.setTempo(120.0f);
            frz.setSync(true);
            expectWithinAbsoluteError(frz.getParameter(FreezeReverb::Grab), 0.5f, 1.0e-4f);
            frz.setSync(false);
            expectWithinAbsoluteError(frz.getParameter(FreezeReverb::Grab), 0.25f, 1.0e-4f);

            MillisecondEffect gap;
            expectEquals(gap.getSyncParamIndex(), 6);
            gap.setTempo(120.0f);
            gap.setSync(true);
            expectWithinAbsoluteError(gap.ms, 500.0f, 0.01f);
            gap.setSyncDivision(FxSync::NUM_DIVISIONS - 1);         // 16 bars: halved until it fits 2 s
            expectWithinAbsoluteError(gap.ms, 2000.0f, 0.01f);
            gap.setSync(false);
            expectWithinAbsoluteError(gap.ms, 10.0f, 0.01f);

            // The reverbs' pre-delay.
            SpringReverb spring;
            expectEquals(spring.getSyncParamIndex(), 4 + (int) SpringReverb::PreDelay);
            spring.setTempo(120.0f);
            spring.setSyncDivision(FxSync::DEFAULT_DIVISION - 6);    // 1/16 = 0.125 s
            spring.setSync(true);
            expectWithinAbsoluteError(spring.getExtra(SpringReverb::PreDelay), 0.125f, 1.0e-4f);
            expectEquals(ShimmerReverb().getSyncParamIndex(), 4 + (int) ShimmerReverb::PreDelay);

            // What synced before syncs as before: a TIME or RATE among the four comes first.
            TapeDelay tape;
            expectEquals(tape.getSyncParamIndex(), (int) TapeDelay::Time);
            expectEquals(Wah().getSyncParamIndex(), (int) Wah::Rate);
            expectEquals(Vibrato().getSyncParamIndex(), (int) Vibrato::Rate);
            expect(! AudioEngine::createEffect(EffectType::CompressorEffect)->canSync());
        }

        beginTest("A tempo that is not a number, or infinite, is not taken: a synced cell stays where it is (review)");
        {
            // (A division of an infinite tempo has no length: halving or doubling it into the cell's range never ended.)
            for (auto type : { EffectType::TapeDelayEffect, EffectType::ChorusEffect, EffectType::FilterEffectType, EffectType::SpringReverbEffect })
            {
                auto fx = AudioEngine::createEffect(type);
                fx->prepare(SR, 512);
                fx->reset();
                fx->setSync(true);
                const int cell = fx->getSyncParamIndex();
                auto value = [&] { return cell < 4 ? fx->getParameter(cell) : fx->getExtra(cell - 4); };
                const float before = value();
                for (float bpm : { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity(), 0.0f, -120.0f })
                    fx->setTempo(bpm);
                expectEquals(value(), before);
                fx->setTempo(3.0e38f);
                expect(std::isfinite(value()));
                fx->setTempo(1.0e-30f);
                expect(std::isfinite(value()));
                fx->setTempo(97.0f);
                expect(value() != before, "and a real tempo is taken again");
            }
        }

        beginTest("One list of tempo divisions: an effect's are it, an LFO's are entries of it");
        {
            expectEquals((int) FxSync::NUM_DIVISIONS, (int) tempo::COUNT);
            expectEquals(juce::String(FxSync::divisionLabels()[FxSync::DEFAULT_DIVISION]), juce::String("1/4"));
            expectEquals(tempo::beats(tempo::QUARTER), 1.0);
            for (int i = 0; i < LFO::NUM_DIVISIONS; ++i)
            {
                const int entry = tempo::fromLabel(LFO::divisionName(i));
                expect(entry >= 0, juce::String(LFO::divisionName(i)) + " is in the one list");
                expectWithinAbsoluteError((double) LFO::divisionBeats(i), tempo::beats(entry), 1.0e-6);
            }
            for (int i = 1; i < tempo::COUNT; ++i) expect(tempo::beats(i) > tempo::beats(i - 1), "shortest first");
        }
    }

    // =================================================================================================
    // 7. Any input, any rate, any block, every cell at both ends
    // =================================================================================================
    struct Kind { const char* name; std::function<std::unique_ptr<EffectBase>()> make; };
    static std::vector<Kind> kinds()
    {
        return {
            { "WAH", [] { return std::unique_ptr<EffectBase>(new Wah()); } },
            { "PHS", [] { return std::unique_ptr<EffectBase>(new Phaser()); } },
            { "FLT", [] { return std::unique_ptr<EffectBase>(new FilterEffect()); } },
            { "TRM", [] { return std::unique_ptr<EffectBase>(new Tremolo()); } },
            { "PAN", [] { return std::unique_ptr<EffectBase>(new AutoPan()); } },
            { "VIB", [] { return std::unique_ptr<EffectBase>(new Vibrato()); } },
            { "FRZ", [] { return std::unique_ptr<EffectBase>(new FreezeReverb()); } },
            { "RPT", [] { return std::unique_ptr<EffectBase>(new BeatRepeat()); } },
            { "STOP", [] { return std::unique_ptr<EffectBase>(new TapeStop()); } },
            { "TAPE", [] { return std::unique_ptr<EffectBase>(new TapeDelay()); } },
            { "LCR", [] { return std::unique_ptr<EffectBase>(new LCRDelay()); } },
            { "SPRG", [] { return std::unique_ptr<EffectBase>(new SpringReverb()); } },
            { "SHIM", [] { return std::unique_ptr<EffectBase>(new ShimmerReverb()); } } };
    }

    // Every cell at one end (0), the other (1), or as the ends alternate (2, 3).
    static void cellsAt(EffectBase& fx, int corner)
    {
        for (int c = 0; c < 8; ++c)
        {
            const ParamSpec spec = c < 4 ? fx.getParamSpec(c) : fx.getExtraSpec(c - 4);
            if (! spec.isValid()) continue;
            const bool top = corner == 1 || (corner == 2 && c % 2 == 0) || (corner == 3 && c % 2 == 1);
            if (c < 4) fx.setParameter(c, top ? spec.maxVal : spec.minVal); else fx.setExtra(c - 4, top ? spec.maxVal : spec.minVal);
        }
    }

    void testAnyInput()
    {
        beginTest("Whatever comes in (1e30, infinite, not a number), at 22.05 to 192 kHz, in blocks of 1 to 4096, every cell at both ends: finite and held");
        {
            for (const auto& kind : kinds())
                for (double sr : { 22050.0, 48000.0, 192000.0 })
                    for (int corner = 0; corner < 4; ++corner)
                    {
                        if (sr != 48000.0 && corner >= 2) continue;
                        auto fx = kind.make();
                        cellsAt(*fx, corner);
                        fx->setTempo(corner == 0 ? 20.0f : 300.0f);
                        fx->prepare(sr, 4096);
                        fx->reset();
                        auto* gesture = dynamic_cast<PerfGesture*>(fx.get());
                        Signal in = noise(0.25, 1.0f, 11 + (uint32_t) corner, sr);
                        in[100] = 1.0e30f; in[101] = -1.0e30f; in[300] = std::numeric_limits<float>::infinity(); in[301] = -std::numeric_limits<float>::infinity();
                        in[500] = std::numeric_limits<float>::quiet_NaN(); in[2000] = 1.0e30f;
                        append(in, silence(0.25, sr));
                        Signal key = noise(0.5, 1.0f, 3, sr);
                        key[700] = std::numeric_limits<float>::quiet_NaN(); key[900] = 1.0e30f;
                        Stereo out { in, in };
                        const int blocks[] = { 1, 37, 4096, 64 };
                        size_t at = 0;
                        int turn = 0;
                        bool ok = true;
                        double most = 0.0;
                        while (at < in.size())
                        {
                            const int n = (int) std::min((size_t) blocks[turn % 4], in.size() - at);
                            if (turn % 16 == 3 && gesture != nullptr) gesture->padGate(true);
                            if (turn % 16 == 11 && gesture != nullptr) gesture->padGate(false);
                            if (turn % 5 == 2) for (int c = 0; c <= 8; ++c) fx->modulateCell(c, (turn % 2 == 0) ? 1.0f : -1.0f);
                            if (turn % 7 == 4) cellsAt(*fx, (corner + turn) % 4);
                            if (fx->wantsInput()) fx->setInput(key.data() + at, key.data() + at, n);
                            fx->processBlock(out.l.data() + at, out.r.data() + at, n);
                            at += (size_t) n;
                            ++turn;
                        }
                        for (size_t i = 0; i < out.l.size(); ++i)
                        {
                            ok = ok && std::isfinite(out.l[i]) && std::isfinite(out.r[i]);
                            most = std::max(most, (double) std::max(std::abs(out.l[i]), std::abs(out.r[i])));
                        }
                        expect(ok, juce::String(kind.name) + " finite at " + juce::String(sr) + " Hz, corner " + juce::String(corner));
                        expect(most < 1000.0, juce::String(kind.name) + " held: peak " + juce::String(most) + " at " + juce::String(sr) + " Hz, corner " + juce::String(corner));
                    }
        }

        beginTest("Left in silence, the swept filters come to rest at nothing; a filter of each kind, with its tail");
        {
            // (RESO short of the top: at the top the ladder and the diode filter sing by themselves, as they do in a voice.)
            for (int f : { 0, 6, 12, 15, 17, 20 })
            {
                auto fx = made<Wah>([f](Wah& w) { w.setParameter(Wah::Reso, 0.8f); w.setParameter(Wah::Rate, 3.0f); w.setExtra(Wah::Filt, (float) f); });
                Signal in = noise(0.3, 0.5f);
                append(in, silence(4.0));
                const Stereo out = render(*fx, in);
                bool subnormal = false;
                for (size_t i = (size_t) (3.3 * SR); i < out.l.size(); ++i) subnormal = subnormal || (out.l[i] != 0.0f && std::abs(out.l[i]) < 1.0e-30f);
                expect(! subnormal && rmsSec(out.l, 4.0, 4.3) < 1.0e-9, juce::String(swp::filt::entry(f).name) + " rests: " + juce::String(rmsSec(out.l, 4.0, 4.3)));
            }
            auto ph = made<Phaser>([](Phaser& p) { p.setParameter(Phaser::Feedback, -0.9f); p.setParameter(Phaser::Stages, 2.0f); p.setMix(1.0f); });
            Signal in = noise(0.3, 0.5f);
            append(in, silence(3.0));
            const Stereo out = render(*ph, in);
            expect(rmsSec(out.l, 3.0, 3.3) == 0.0, "the phaser's stages are put away");
        }

        beginTest("The same sound whatever the block size: the swept filters with a key, the freeze, the repeat, the stop");
        {
            const Signal in = riff(), key = hits(2.4, { 0.1, 0.6, 1.1, 1.35, 1.6, 2.1 });
            for (const auto& kind : kinds())
            {
                const juce::String name(kind.name);
                if (name == "TAPE" || name == "LCR" || name == "SPRG" || name == "SHIM") continue;     // ("Effect Cores Time" holds those)
                auto play = [&](int block)
                {
                    auto fx = kind.make();
                    if (name == "WAH") { fx->setParameter(Wah::Rate, 2.0f); fx->setExtra(Wah::Trig, 1.0f); fx->setExtra(Wah::KEY_EXTRA, (float) dyn::KEY_CHOICE_IN); }
                    if (name == "PHS") { fx->setExtra(Phaser::Env, 0.7f); fx->setExtra(Phaser::Key, (float) dyn::KEY_CHOICE_IN); }
                    if (name == "FLT") { fx->setExtra(FilterEffect::Depth, 0.5f); fx->setExtra(FilterEffect::Env, 0.5f); fx->setExtra(FilterEffect::Key, (float) dyn::KEY_CHOICE_IN); }
                    if (name == "TRM" || name == "PAN") { fx->setExtra(0, 2.0f); fx->setExtra(2, 0.1f); fx->setExtra(3, 1.0f); }
                    if (name == "VIB") { fx->setParameter(Vibrato::Fade, 0.2f); fx->setExtra(Vibrato::Trig, 1.0f); }
                    fx->setTempo(120.0f);
                    fx->prepare(SR, 4096);
                    fx->reset();
                    auto* gesture = dynamic_cast<PerfGesture*>(fx.get());
                    Stereo out { in, in };
                    size_t at = 0;
                    while (at < in.size())
                    {
                        // (The pad is pressed on a sample, whatever the block.)
                        size_t n = std::min((size_t) block, in.size() - at);
                        for (size_t edge : { (size_t) 9600, (size_t) 76800 })
                            if (at < edge && at + n > edge) n = edge - at;
                        if (at == 9600 && gesture != nullptr) gesture->padGate(true);
                        if (at == 76800 && gesture != nullptr) gesture->padGate(false);
                        if (fx->wantsInput()) fx->setInput(key.data() + at, key.data() + at, (int) n);
                        fx->processBlock(out.l.data() + at, out.r.data() + at, (int) n);
                        at += n;
                    }
                    return out;
                };
                const Stereo reference = play(64);
                expect(rms(reference.l) > 1.0e-4, name + " sounds");
                for (int block : { 1, 7, 4096 })
                {
                    const Stereo other = play(block);
                    double worst = 0.0;
                    for (size_t i = 0; i < reference.l.size(); ++i) worst = std::max(worst, (double) std::abs(other.l[i] - reference.l[i]));
                    expect(worst < 1.0e-4, name + " in blocks of " + juce::String(block) + ": " + juce::String(worst, 6));
                }
            }
        }
    }

    // =================================================================================================
    // 8. Cost (NEPTUNE_E6_TIME=1): old against new in this binary
    // =================================================================================================
    template <typename Run>
    static double microsecondsPerBlock(Run&& run)
    {
        static const Signal in = noise(1.0, 0.3f, 5);
        std::vector<float> l(512), r(512);
        double best = 1.0e9;
        for (int pass = 0; pass < 7; ++pass)
        {
            const auto start = std::chrono::steady_clock::now();
            const int blocks = 400;
            for (int b = 0; b < blocks; ++b)
            {
                const size_t at = (size_t) ((b * 512) % 40000);
                std::copy(in.begin() + (long) at, in.begin() + (long) at + 512, l.begin());
                std::copy(in.begin() + (long) at, in.begin() + (long) at + 512, r.begin());
                run(l.data(), r.data(), 512);
            }
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / blocks;
            best = std::min(best, us);
        }
        return best;
    }

    void testCost()
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_E6_TIME", "").isEmpty())
            return;
        beginTest("Cost: microseconds per 512-sample block at 48 kHz, old against new (lowest of 7 passes, alternated three times)");
        auto row = [this](const juce::String& name, const std::function<double()>& older, const std::function<double()>& newer)
        {
            double a = 1.0e9, b = 1.0e9;
            for (int round = 0; round < 3; ++round) { if (older) a = std::min(a, older()); b = std::min(b, newer()); }
            logMessage(juce::String::formatted("E6 cost | %-34s | old %7.2f | new %7.2f | %+6.1f %%", name.toRawUTF8(), older ? a : 0.0, b, older ? 100.0 * (b / a - 1.0) : 0.0));
        };
        auto fxCost = [](EffectBase& fx) { return microsecondsPerBlock([&fx](float* l, float* r, int n) { fx.processBlock(l, r, n); }); };
        template_pair<e6old::Wah, Wah>(row, fxCost, "WAH default (envelope)", [](EffectBase&) {});
        template_pair<e6old::Wah, Wah>(row, fxCost, "WAH LFO 2 Hz", [](EffectBase& fx) { fx.setParameter(3, 2.0f); });
        template_pair<e6old::Phaser, Phaser>(row, fxCost, "PHS default (4 stages)", [](EffectBase&) {});
        template_pair<e6old::Phaser, Phaser>(row, fxCost, "PHS 6 stages, spread", [](EffectBase& fx) { fx.setParameter(3, 6.0f); fx.setExtra(1, 1.0f); });
        template_pair<e6old::FilterEffect, FilterEffect>(row, fxCost, "FLT SVF LP", [](EffectBase&) {});
        template_pair<e6old::FilterEffect, FilterEffect>(row, fxCost, "FLT ladder LP24", [](EffectBase& fx) { fx.setParameter(2, 2.0f); });
        template_pair<e6old::Vibrato, Vibrato>(row, fxCost, "VIB default", [](EffectBase&) {});
        template_pair<e6old::Vibrato, Vibrato>(row, fxCost, "VIB FADE 0.5", [](EffectBase& fx) { fx.setParameter(3, 0.5f); });
        template_pair<e6old::FreezeReverb, FreezeReverb>(row, fxCost, "FRZ held", [](EffectBase& fx) { dynamic_cast<PerfGesture&>(fx).padGate(true); });
        template_pair<e6old::FreezeReverb, FreezeReverb>(row, fxCost, "FRZ held, SHIM 0.5", [](EffectBase& fx) { fx.setParameter(1, 0.5f); dynamic_cast<PerfGesture&>(fx).padGate(true); });
        template_pair<e6old::BeatRepeat, BeatRepeat>(row, fxCost, "RPT 1/16", [](EffectBase& fx) { dynamic_cast<PerfGesture&>(fx).padGate(true); });
        template_pair<e6old::TapeStop, TapeStop>(row, fxCost, "STOP TIME 4 s", [](EffectBase& fx) { fx.setParameter(0, 4.0f); dynamic_cast<PerfGesture&>(fx).padGate(true); });
        {
            e6old::TalkBlock older; TalkBlock newer;
            for (int which = 0; which < 2; ++which)
            {
                auto set = [which](auto& t) { t.set(3, 1.0f); if (which == 1) { t.set(5, 1.0f); t.set(6, 0.5f); } t.prepare(SR, 512); };
                set(older); set(newer);
                row(which == 0 ? "TALK a vowel" : "TALK LFO and envelope",
                    [&] { return microsecondsPerBlock([&](float* l, float* r, int n) { older.process(l, r, n); }); },
                    [&] { return microsecondsPerBlock([&](float* l, float* r, int n) { newer.process(l, r, n); }); });
            }
        }
        // What was not folded but stands on the cores that grew (the old cores are compiled beside the new: effect_cores_sweep_oldcores.h).
        costOfCores(row);
        // The new things in use.
        auto alone = [&](const juce::String& name, std::unique_ptr<EffectBase> fx) { fx->prepare(SR, 512); fx->reset(); row(name, {}, [&] { return fxCost(*fx); }); };
        { auto fx = std::make_unique<Wah>(); fx->setExtra(Wah::Filt, 6.0f); alone("WAH on a ladder (L24)", std::move(fx)); }
        { auto fx = std::make_unique<Wah>(); fx->setExtra(Wah::Filt, 18.0f); alone("WAH as a phaser (PH4)", std::move(fx)); }
        { auto fx = std::make_unique<Wah>(); fx->setExtra(Wah::Filt, 20.0f); alone("WAH on the vowels", std::move(fx)); }
        { auto fx = std::make_unique<Wah>(); fx->setParameter(3, 2.0f); fx->setExtra(Wah::Trig, 1.0f); alone("WAH LFO, TRIG KEY", std::move(fx)); }
        { auto fx = std::make_unique<Phaser>(); fx->setExtra(Phaser::Env, 0.5f); alone("PHS with ENV", std::move(fx)); }
        { auto fx = std::make_unique<FilterEffect>(); fx->setExtra(FilterEffect::Depth, 0.5f); fx->setExtra(FilterEffect::Env, 0.5f); alone("FLT SVF with LFO and ENV", std::move(fx)); }
        { auto fx = std::make_unique<Tremolo>(); alone("TRM default (TRIG FREE)", std::move(fx)); }
        { auto fx = std::make_unique<Tremolo>(); fx->setExtra(Tremolo::Trig, 1.0f); alone("TRM TRIG KEY (listening)", std::move(fx)); }
        { auto fx = std::make_unique<TapeDelay>(); fx->setExtra(TapeDelay::Read, (float) dly::read::Hold); alone("TAPE READ HOLD", std::move(fx)); }
        { auto fx = std::make_unique<TapeDelay>(); fx->setExtra(TapeDelay::Read, (float) dly::read::OctaveFifth); alone("TAPE READ O+5", std::move(fx)); }
        { auto fx = std::make_unique<ShimmerReverb>(); fx->setParameter(ShimmerReverb::ShiftMode, (float) rvb::shift::Hold); alone("SHIM MODE HOLD", std::move(fx)); }
        { auto fx = std::make_unique<ShimmerReverb>(); fx->setParameter(ShimmerReverb::Shimmer, 0.5f); fx->setParameter(ShimmerReverb::ShiftMode, (float) rvb::shift::Reverse); alone("SHIM MODE REV", std::move(fx)); }
    }

    template <typename Old, typename New, typename Row, typename Cost>
    static void template_pair(Row& row, Cost& cost, const char* name, const std::function<void(EffectBase&)>& set)
    {
        Old older; New newer;
        for (EffectBase* fx : { (EffectBase*) &older, (EffectBase*) &newer }) { fx->setTempo(120.0f); fx->prepare(SR, 512); fx->reset(); set(*fx); }
        row(name, [&] { return cost(older); }, [&] { return cost(newer); });
    }

    template <typename Row>
    void costOfCores(Row& row)
    {
        // A tape (driven, emphasis, wow), three clean taps, moving heads: the delay core as it was and as it is.
        for (int which = 0; which < 3; ++which)
        {
            auto prepare = [which](auto& core, auto setup)
            {
                setup.emphasis = setup.makeup = which == 0;
                setup.stereo = which != 1;
                core.prepare(SR, 2.0f, setup);
                core.setTapCount(which == 1 ? 3 : 1);
                core.setTap(0, 0.3f, 1.0f, which == 1 ? 0.0f : 1.0f, 1.0f);
                if (which == 1) { core.setTap(1, 0.2f, 0.7f, 0.7f, 0.0f); core.setTap(2, 0.1f, 0.0f, 1.0f, 0.0f); }
                core.setFeedback(0.5f);
                core.setFilter(6000.0f, which == 0 ? 80.0f : 0.0f);
                if (which == 0) { core.setDrive(1.0f, 2.0f); core.setTransport(0.4f); }
                if (which == 2) core.setRead(2.0f, 0.015f, 1.0f);
                core.reset();
            };
            e6dly::Core older; dly::Core newer;
            prepare(older, e6dly::Core::Setup());
            prepare(newer, dly::Core::Setup());
            auto time = [](auto& core) { return microsecondsPerBlock([&core](float* l, float* r, int n)
            {
                for (int at = 0; at < n; at += 32) { core.control(); core.run(l + at, r + at, 32, 0.5f, 0.5f); }
            }); };
            row(which == 0 ? "dly::Core as a tape" : which == 1 ? "dly::Core, three clean taps" : "dly::Core, heads an octave up", [&] { return time(older); }, [&] { return time(newer); });
        }
        for (int which = 0; which < 2; ++which)
        {
            auto settle = [which](auto& core)
            {
                core.setDecay(2.0f);
                core.setDamping(0.4f);
                if (which == 0) core.setDispersion(0.6f); else { core.setShimmer(0.5f, 2.0f); core.setModulation(0.25f); }
                core.reset();
            };
            e6rvb::Core older;
            {
                e6rvb::Core::Setup setup;
                if (which == 0) { setup.dispersionAlways = true; setup.crossOut = 0.2f; }
                else { setup.cross = true; setup.diffusers = 3; setup.loopAllpassMs[0] = 22.0f; setup.loopAllpassMs[1] = 31.0f; setup.taps = true; setup.loopMs[0] = 97.0f; setup.loopMs[1] = 113.0f; }
                older.prepare(SR, setup);
                settle(older);
            }
            // (Since the one network, the spring and the shimmer tank are settings of rvb::Net.)
            rvb::Net newer;
            {
                rvb::Net::Setup setup = which == 0 ? rvb::setting::spring() : rvb::setting::shimmer();
                if (which == 1) { setup.allpassMs[0] = 22.0f; setup.allpassMs[1] = 31.0f; setup.lineMs[0] = 97.0f; setup.lineMs[1] = 113.0f; }
                newer.prepare(SR, setup);
                settle(newer);
            }
            auto time = [](auto& core) { return microsecondsPerBlock([&core](float* l, float* r, int n)
            {
                for (int at = 0; at < n; at += 32) { core.control(); core.run(l + at, r + at, 32, 0.5f, 0.5f); }
            }); };
            row(which == 0 ? "rvb::Core as a spring" : "rvb::Core as a shimmer tank", [&] { return time(older); }, [&] { return time(newer); });
        }
    }
};

static EffectCoresSweepTests effectCoresSweepTests;
