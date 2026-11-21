#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "../src/engine/AudioEngine.h"
#include "../src/engine/PerfFx.h"
#include "../src/ui/screens/PerfFxScreen.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/project/ProjectBridge.h"
#include "fast_measure.h"

// PERF: effects played from the pads, on any track or on the whole mix.
class PerfFxTests : public juce::UnitTest
{
public:
    PerfFxTests() : UnitTest("Perf FX") {}

    using Signal = std::vector<float>;

    static int insert(EffectType t) { return PerfFx::INSERT_BASE + (int) t; }

    // The rack by itself: a signal is put through one target, block by block.
    struct Bench
    {
        PerfFxRack rack;
        double sr;
        PerfFxRack::Context ctx;
        juce::int64 at = 0;         // samples rendered so far
        juce::Random random { 7 };

        explicit Bench(double sampleRate = 48000.0) : sr(sampleRate)
        {
            rack.setCreator(&AudioEngine::createPerfEffect, &AudioEngine::perfWays, nullptr);
            rack.prepare(sr, 64);
            ctx.bpm = 120.0f;
            ctx.beatsPerSample = 2.0 / sr;
        }

        int samples(double seconds) const { return (int) std::lround(seconds * sr); }

        // `in` through `target`; the left channel comes back (the right is fed the same).
        Signal through(const Signal& in, int target = 0)
        {
            Signal left = in, right = in;
            for (int done = 0; done < (int) in.size(); done += 64)
            {
                const int n = juce::jmin(64, (int) in.size() - done);
                ctx.beatAtStart = ctx.playing ? (double) at * ctx.beatsPerSample : 0.0;
                rack.beginBlock(ctx, n);
                rack.process(target, left.data() + done, right.data() + done, n);
                at += n;
            }
            return left;
        }

        Signal noise(double seconds, float level = 0.25f)
        {
            Signal s((size_t) samples(seconds));
            for (auto& v : s) v = (random.nextFloat() * 2.0f - 1.0f) * level;
            return s;
        }
        Signal silence(double seconds) const { return Signal((size_t) samples(seconds), 0.0f); }
        Signal sine(double seconds, double hz, float level = 0.5f)
        {
            Signal s((size_t) samples(seconds));
            for (size_t i = 0; i < s.size(); ++i)
                s[i] = level * (float) std::sin(6.283185307179586 * hz * (double) (at + (juce::int64) i) / sr);
            return s;
        }

        // A pad with one effect; what comes back is its layer A (put back with set()).
        PerfFx::Layer pad(int index, int kind)
        {
            rack.setKind(index, 0, kind);
            return rack.get(index, 0);
        }
        void set(int index, const PerfFx::Layer& layer) { rack.setLayer(index, 0, layer); }
        // The pad's own settings (latch, grid, pressure curve).
        void ofPad(int index, const std::function<void(PerfFx::Setting&)>& edit)
        {
            PerfFx::Setting setting = rack.get(index);
            edit(setting);
            rack.set(index, setting);
        }
    };

    static Signal minus(const Signal& a, const Signal& b)
    {
        Signal d(a.size());
        for (size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
        return d;
    }
    static float rms(const Signal& s, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, s.size());
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) sum += (double) s[i] * (double) s[i];
        return to > from ? (float) std::sqrt(sum / (double) (to - from)) : 0.0f;
    }
    static float peak(const Signal& s, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, s.size());
        float p = 0.0f;
        for (size_t i = from; i < to; ++i) p = std::max(p, std::abs(s[i]));
        return p;
    }
    static bool finite(const Signal& s)
    {
        for (float v : s) if (! std::isfinite(v)) return false;
        return true;
    }
    // The largest step from one sample to the next.
    static float step(const Signal& s, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, s.size());
        float p = 0.0f;
        for (size_t i = std::max<size_t>(from, 1); i < to; ++i) p = std::max(p, std::abs(s[i] - s[i - 1]));
        return p;
    }
    // How bright: the level of the sample-to-sample difference against the level.
    static float brightness(const Signal& s, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, s.size());
        double diff = 0.0, sum = 0.0;
        for (size_t i = std::max<size_t>(from, 1); i < to; ++i)
        {
            diff += (double) (s[i] - s[i - 1]) * (double) (s[i] - s[i - 1]);
            sum += (double) s[i] * (double) s[i];
        }
        return sum > 0.0 ? (float) std::sqrt(diff / sum) : 0.0f;
    }
    static float correlation(const Signal& a, const Signal& b, size_t from, size_t to)
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = from; i < to; ++i) { ab += (double) a[i] * b[i]; aa += (double) a[i] * a[i]; bb += (double) b[i] * b[i]; }
        return (aa > 1.0e-12 && bb > 1.0e-12) ? (float) (ab / std::sqrt(aa * bb)) : 0.0f;
    }
    // The largest difference between s[at ..] and reference[refAt ..] over n samples.
    static float apart(const Signal& s, size_t at, const Signal& reference, size_t refAt, size_t n)
    {
        float d = 0.0f;
        for (size_t i = 0; i < n && at + i < s.size() && refAt + i < reference.size(); ++i)
            d = std::max(d, std::abs(s[at + i] - reference[refAt + i]));
        return d;
    }
    static float dB(float ratio) { return 20.0f * std::log10(std::max(ratio, 1.0e-9f)); }

    // The PERF screen inside a ScreenManager, driven by controller messages.
    struct Plain : Screen
    {
        juce::String getName() const override { return "OTHER"; }
        void render(DisplayRenderer&) override {}
    };
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        ScreenManager mgr;
        PerfFxScreen* screen = nullptr;
        juce::int64 now = 1000;

        Rig()
        {
            seq.prepareToPlay(48000.0, 64);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(48000.0, 64);
            mgr.setSequencer(&seq);
            mgr.setClock([this] { return now; });
            mgr.setHelpBusyWindow(0);
            mgr.setEngineHooks(GrammarHooks::make(seq, audio));
            mgr.addScreen(std::make_unique<Plain>());
            auto s = std::make_unique<PerfFxScreen>(audio, [this] { return seq.getSelectedTrack(); },
                                                    [this] { return seq.getPattern().getTrackCount(); });
            screen = s.get();
            mgr.addScreen(std::move(s));
            mgr.setShiftPadScreen(14, 1);           // Shift + bottom pad 7
            mgr.setPinnable(1, true);
            mgr.projectLoaded();
        }

        PerfFxRack& rack() { return audio.getPerfFx(); }
        static int padNote(int pad) { return pad < 8 ? LaunchkeyController::PAD_TOP_ROW_START + pad
                                                     : LaunchkeyController::PAD_BOTTOM_ROW_START + pad - 8; }
        void padOn(int pad, int velocity = 100) { mgr.noteOnReceived(1, padNote(pad), velocity); }
        void padOff(int pad) { mgr.noteOffReceived(1, padNote(pad)); }
        void padPressure(int pad, int value) { mgr.aftertouchReceived(1, padNote(pad), value); }
        void shift(bool on) { mgr.controlChangeReceived(7, 63, on ? 127 : 0); }
        void func(bool on)  { mgr.controlChangeReceived(1, 105, on ? 127 : 0); }
        void tap(int cc)    { mgr.controlChangeReceived(1, cc, 127); mgr.controlChangeReceived(1, cc, 0); }
        void turn(int knob, int clicks) { mgr.controlChangeReceived(16, 0x15 + knob, clicks < 0 ? 128 + clicks : clicks); }
        void edit(int cc) { func(true); tap(cc); func(false); }
        void undo() { shift(true); func(true); tap(115); func(false); shift(false); }

        // The whole instrument's output, left channel.
        Signal render(double seconds)
        {
            Signal out;
            std::vector<float> l(64), r(64);
            for (int done = 0; done < (int) (seconds * 48000.0); done += 64)
            {
                audio.renderBlock(l.data(), r.data(), 64);
                out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        }
    };

    void runTest() override
    {
        using namespace PerfFx;
        const auto& kinds = AudioEngine::perfKinds();

        beginTest("A pad that is not pressed changes nothing, whatever is on it");
        {
            for (int kind : kinds)
            {
                Bench b;
                b.pad(0, kind);
                const auto in = b.noise(0.1);
                expect(b.through(in) == in, juce::String("untouched with ") + AudioEngine::perfKindName(kind));
                expect(! b.rack.isSounding(0));
            }
        }

        beginTest("Every kind can be made, has a name, and one way at least of being pressed");
        {
            expect(kinds.size() >= 30);
            for (int kind : kinds)
            {
                if (kind == NONE) continue;
                expect(AudioEngine::createPerfEffect(kind) != nullptr, "kind " + juce::String(kind));
                expect(juce::String(AudioEngine::perfKindName(kind)) != "--");
                const auto w = AudioEngine::perfWays(kind);
                expect(w.punch || w.thrown);
            }
            expect(AudioEngine::createPerfEffect(insert(EffectType::SidechainEffect)) == nullptr, "DUCK has no place on a pad");
            expect(AudioEngine::createPerfEffect(9999) == nullptr);
        }

        beginTest("PUNCH: the effect is in while the pad is down, and the sound is itself again after");
        {
            Bench b;
            auto p = b.pad(0, insert(EffectType::FilterEffectType));
            expect(p.mode == PUNCH);
            p.cell[0] = 300.0f;
            p.pressure.fill(0.0f);
            b.set(0, p);

            b.rack.press(0, 0);
            const auto in = b.noise(0.3);
            const auto held = b.through(in);
            expect(b.rack.isDown(0) && b.rack.isSounding(0));
            expect(brightness(held, 7200) < 0.4f * brightness(in, 7200),
                   "a 300 Hz low-pass under the finger: " + juce::String(brightness(held, 7200)) + " against " + juce::String(brightness(in, 7200)));

            b.rack.release(0);
            const auto in2 = b.noise(0.2);
            const auto after = b.through(in2);
            expect(Signal(after.begin() + 4800, after.end()) == Signal(in2.begin() + 4800, in2.end()), "let go: the sound itself");
            expect(! b.rack.isSounding(0), "and the pad rests");
        }

        beginTest("THROW: only the effect's own sound is added, it rings on after the pad is let go, then the pad rests");
        {
            for (int kind : kinds)
            {
                const auto w = AudioEngine::perfWays(kind);
                if (! w.thrown) continue;
                const juce::String name = AudioEngine::perfKindName(kind);
                Bench b;
                auto p = b.pad(0, kind);
                expect(p.mode == THROW, name + " starts as a throw");
                p.sync = false;
                p.pressure.fill(0.0f);
                b.set(0, p);

                b.rack.press(0, 0);
                const auto in = b.noise(1.0);
                const auto out = b.through(in);
                const auto added = minus(out, in);
                expect(rms(added, 24000) > 1.0e-4f, name + " adds something while held");
                const float c = correlation(added, in, 24000, 48000);
                expect(std::abs(c) < 0.1f, name + ": what is added holds no copy of the sound (" + juce::String(c) + ")");

                b.rack.release(0);
                const auto tail = b.through(b.silence(0.25));
                expect(rms(tail) > 1.0e-4f, name + " rings on after the release");
                expect(b.rack.isSounding(0));
                double waited = 0.25;
                while (b.rack.isSounding(0) && waited < 30.0)
                {
                    expect(finite(b.through(b.silence(0.5))));
                    waited += 0.5;
                }
                expect(! b.rack.isSounding(0), name + " comes to rest (waited " + juce::String(waited) + " s)");
                const auto in3 = b.noise(0.05);
                expect(b.through(in3) == in3, name + ": at rest the sound is untouched");
            }
        }

        beginTest("DUB: the echo comes at TIME, pressure feeds it back, the loop is held however it is driven");
        {
            auto echoes = [this](float pressure)
            {
                Bench b;
                auto p = b.pad(0, DUB);
                expect(p.sync && p.pressure[1] > 0.0f, "DUB starts in time, with pressure on FDBK");
                p.sync = false;
                p.cell[0] = 0.25f;
                p.cell[1] = 0.4f;
                b.set(0, p);
                b.rack.press(0, 0);
                b.rack.setPressure(0, pressure);
                b.through(b.silence(0.3));          // (pressure arrives)
                auto in = b.silence(1.5);
                for (int i = 0; i < 96; ++i) in[(size_t) i] = (b.random.nextFloat() - 0.5f);
                return minus(b.through(in), in);
            };
            const auto soft = echoes(0.0f), hard = echoes(1.0f);
            // The first echo: 0.25 s = 12000 samples after the burst.
            expect(rms(soft, 6000, 11900) < 1.0e-5f, "nothing before the echo's time");
            expect(rms(soft, 12000, 12200) > 1.0e-3f, "the echo at 0.25 s");
            const float third = rms(soft, 36000, 36400), thirdHard = rms(hard, 36000, 36400);
            expect(thirdHard > 2.0f * third, "pressure feeds the echo back: third repeat " + juce::String(third) + " -> " + juce::String(thirdHard));

            {
                // A short tap into a long echo: the pad must still be listening when the echo comes.
                Bench late;
                auto p = late.pad(0, DUB);
                p.sync = false;
                p.cell[0] = 1.5f;
                late.set(0, p);
                late.rack.press(0, 0);
                late.through(late.noise(0.1));
                late.rack.release(0);
                const auto after = late.through(late.silence(2.0));
                expect(rms(after, 0, 60000) < 1.0e-5f, "nothing yet");
                expect(rms(after, 67200, 76800) > 1.0e-3f, "the echo of a tap, a second and a half later");
            }

            Bench b;
            auto p = b.pad(0, DUB);
            p.sync = false;
            p.cell[1] = 1.0f;       // FDBK
            p.cell[3] = 1.0f;       // DRIV
            p.pressure[1] = 1.0f;
            b.set(0, p);
            b.rack.press(0, 0);
            b.rack.setPressure(0, 1.0f);
            const auto loud = b.through(b.noise(1.0, 1.0f));
            expect(finite(loud) && peak(loud) < 4.0f);
            Signal ringing;
            for (int i = 0; i < 20; ++i) ringing = b.through(b.silence(1.0));
            expect(finite(ringing) && peak(ringing) < 2.0f, "feeding on itself, held: " + juce::String(peak(ringing)));
            expect(rms(ringing) > 1.0e-3f, "FDBK at the top keeps going by itself");
            // Shift + Play's silence is the way out.
            b.rack.silence();
            const auto in = b.noise(0.05);
            expect(b.through(in) == in && ! b.rack.isDown(0) && ! b.rack.isSounding(0), "silence empties the pad");
            expectEquals(b.rack.getNonFiniteCount(), 0);
        }

        beginTest("FRZ: holds what it caught for as long as the pad is down, dies away after, and can spiral away");
        {
            auto frozen = [this](float spiral, Signal& held, Signal& released, float decay = 3.0f)
            {
                Bench b;
                auto p = b.pad(0, FRZ);
                p.cell[2] = spiral;
                p.cell[3] = decay;
                p.pressure.fill(0.0f);
                b.set(0, p);
                b.rack.press(0, 0);
                // A chord for half a second (GRAB is 0.25 s), then nothing: what sounds is the pad's.
                Signal in = b.silence(0.5);
                for (size_t i = 0; i < in.size(); ++i)
                    in[i] = 0.15f * (float) (std::sin(6.2831853 * 220.0 * (double) i / 48000.0) + std::sin(6.2831853 * 277.2 * (double) i / 48000.0)
                                             + std::sin(6.2831853 * 329.6 * (double) i / 48000.0));
                b.through(in);
                held = b.through(b.silence(6.0));
                b.rack.release(0);
                released = b.through(b.silence(decay * 1.4));
                return rms(in);
            };
            Signal held, released, heldSpiral, releasedSpiral;
            const float inLevel = frozen(0.0f, held, released);
            const float early = rms(held, 48000, 96000), late = rms(held, 240000, 288000);
            expect(early > 1.0e-3f, "something is held");
            expect(std::abs(dB(late / early)) < 0.5f, "held, not dying: " + juce::String(dB(late / early)) + " dB over four seconds");
            logMessage("FRZ held level against what it was given: " + juce::String(dB(early / inLevel), 1) + " dB");
            expect(dB(early / inLevel) > -14.0f && dB(early / inLevel) < 6.0f);
            const float gone = rms(released, released.size() - 9600);
            expect(dB(gone / early) < -40.0f, "let go: gone after DEC (" + juce::String(dB(gone / early)) + " dB)");
            expect(dB(rms(released, 0, 9600) / early) > -12.0f, "but not cut off");

            frozen(1.0f, heldSpiral, releasedSpiral, 8.0f);
            frozen(0.0f, held, released, 8.0f);
            const float plain = brightness(released, 48000, 96000), climbing = brightness(releasedSpiral, 48000, 96000);
            expect(climbing > 1.5f * plain, "SPRL: the tail climbs away (" + juce::String(plain) + " -> " + juce::String(climbing) + ")");

            // Everything up, loud, for a long time.
            Bench b;
            auto p = b.pad(0, FRZ);
            p.cell[1] = 1.0f;
            p.cell[0] = 2.0f;
            b.set(0, p);
            b.rack.press(0, 0);
            b.rack.setPressure(0, 1.0f);
            float worst = 0.0f;
            for (int i = 0; i < 20; ++i)
            {
                const auto out = b.through(b.noise(1.0, 1.0f));
                expect(finite(out));
                worst = std::max(worst, peak(out));
            }
            expect(worst < 8.0f, "bounded: " + juce::String(worst));
            expectEquals(b.rack.getNonFiniteCount(), 0);
        }

        beginTest("DUB's tape: flat when quiet; when hot the treble gives way first, the octave appears, the pitch wavers");
        {
            const double sr = 48000.0;
            // The first echo (TIME 0.25 s, no feedback) of one second of a sine: how loud `hz` comes back in it.
            auto echo = [&](double hz, float level, float driv, float toneCell)
            {
                DubDelay dub;
                dub.prepare(sr, 64);
                dub.setParameter(DubDelay::Time, 0.25f);
                dub.setParameter(DubDelay::Feedback, 0.0f);
                dub.setParameter(DubDelay::Tone, toneCell);
                dub.setParameter(DubDelay::DriveAmt, driv);
                dub.reset();
                Signal out((size_t) (1.5 * sr));
                for (size_t i = 0; i < out.size(); ++i)
                {
                    float l = i < (size_t) sr ? level * (float) std::sin(6.283185307179586 * hz * (double) i / sr) : 0.0f, r = l;
                    dub.process(l, r);
                    out[i] = l;
                }
                return out;
            };
            auto level = [&](const Signal& x, double hz) { return std::sqrt(fastmeasure::goertzel(x, 24000, 24000, hz, sr)); };
            // How much of its quiet gain a tone keeps when it is 40 times (32 dB) hotter.
            auto kept = [&](double hz)
            {
                const double quiet = level(echo(hz, 0.01f, 0.5f, 0.7f), hz) / 0.01, hot = level(echo(hz, 0.4f, 0.5f, 0.7f), hz) / 0.4;
                return hot / quiet;
            };
            const double bass = kept(300.0), treble = kept(6000.0);
            logMessage("DUB tape: gain kept when hot, 300 Hz " + juce::String(bass, 3) + ", 6 kHz " + juce::String(treble, 3));
            expect(bass < 0.9, "a hot repeat is squashed: " + juce::String(bass));
            expect(treble < 0.7 * bass, "and its treble gives way first: " + juce::String(treble) + " against " + juce::String(bass));

            {
                // Quiet, the tape is flat: the pre-emphasis is undone on playback, and what is left is TONE's band.
                // (Measured as a level, not in one narrow band: the wow spreads a 6 kHz tone over several hertz.)
                const double a = rms(echo(6000.0, 0.005f, 0.0f, 0.7f), 24000, 48000) / rms(echo(300.0, 0.005f, 0.0f, 0.7f), 24000, 48000);
                const double centre = 150.0 * std::pow(5000.0 / 150.0, 0.7);
                auto band = [&](double hz)
                {
                    const double lp = centre * 2.2, hp = centre / 2.2;
                    return (1.0 / std::sqrt(1.0 + (hz / lp) * (hz / lp))) * ((hz / hp) / std::sqrt(1.0 + (hz / hp) * (hz / hp)));
                };
                const double wanted = band(6000.0) / band(300.0);
                logMessage("DUB tape: quiet 6 kHz against 300 Hz " + juce::String(a, 3) + ", the band alone " + juce::String(wanted, 3));
                expect(a > 0.8 * wanted && a < 1.25 * wanted, "flat when quiet: " + juce::String(a) + " against " + juce::String(wanted));
            }

            {
                // The lopsided curve: a driven 300 Hz tone comes back with its octave, a quiet one without.
                const auto hot = echo(300.0, 0.4f, 0.5f, 0.7f), quiet = echo(300.0, 0.004f, 0.5f, 0.7f);
                const double octaveHot = level(hot, 600.0) / level(hot, 300.0), octaveQuiet = level(quiet, 600.0) / level(quiet, 300.0);
                const double thirdHot = level(hot, 900.0) / level(hot, 300.0);
                logMessage("DUB tape: octave of a hot tone " + juce::String(octaveHot, 4) + " (third harmonic " + juce::String(thirdHot, 4) + "), of a quiet one " + juce::String(octaveQuiet, 5));
                expect(octaveHot > 0.03, "even harmonics when driven: " + juce::String(octaveHot));
                expect(thirdHot > 0.03, "and odd ones: " + juce::String(thirdHot));
                expect(octaveQuiet < 0.005, "clean when quiet: " + juce::String(octaveQuiet));
            }

            for (float driv : { 0.0f, 1.0f })
            {
                // Wow and flutter: the phase of a steady quiet 100 Hz tone's echo, read every 20 ms, wanders.
                const auto out = echo(100.0, 0.01f, driv, 0.0f);
                double lo = 1.0e9, hi = -1.0e9;
                for (size_t from = 14400; from + 960 <= 57600; from += 960)
                {
                    double c = 0.0, q = 0.0;
                    for (size_t i = from; i < from + 960; ++i)
                    {
                        const double w = 6.283185307179586 * 100.0 * (double) i / sr;
                        c += out[i] * std::cos(w); q += out[i] * std::sin(w);
                    }
                    const double phase = std::atan2(c, q);
                    lo = std::min(lo, phase); hi = std::max(hi, phase);
                }
                // The echo time moves by up to 0.36 % (0.9 ms of 250) fully driven, about a third of that at rest.
                logMessage("DUB tape: DRIV " + juce::String(driv, 0) + ", phase of a 100 Hz echo wanders over " + juce::String(hi - lo, 3) + " rad");
                expect(hi - lo > (driv > 0.5f ? 0.3 : 0.1), "the transport is not steady: " + juce::String(hi - lo));
                expect(hi - lo < (driv > 0.5f ? 1.4 : 0.5), "and not seasick: " + juce::String(hi - lo));
            }
        }

        beginTest("DUB and FRZ at every corner of their cells: held, let go, ten seconds on - bounded, and dying unless FDBK is at the top");
        {
            for (int kind : { (int) DUB, (int) FRZ })
                for (int corner = 0; corner < 16; ++corner)
                {
                    Bench b;
                    auto p = b.pad(0, kind);
                    p.sync = false;
                    p.pressure.fill(0.0f);
                    for (int i = 0; i < 4; ++i)
                    {
                        const auto spec = b.rack.cellSpec(0, 0, i);
                        p.cell[(size_t) i] = (corner >> i) & 1 ? spec.maxVal : spec.minVal;
                    }
                    b.set(0, p);
                    b.rack.press(0, 0);
                    const auto held = b.through(b.noise(1.0, 0.7f));
                    b.rack.release(0);
                    const auto first = b.through(b.silence(1.0));
                    Signal last;
                    for (int i = 0; i < 9; ++i) last = b.through(b.silence(1.0));
                    const juce::String where = juce::String(AudioEngine::perfKindName(kind)) + " corner " + juce::String(corner);
                    expect(finite(held) && finite(first) && finite(last), where);
                    expect(peak(held) < 8.0f && peak(first) < 4.0f && peak(last) < 4.0f, where + ": " + juce::String(peak(held)) + " " + juce::String(peak(last)));
                    const bool feedsItself = kind == DUB && (corner & 2);          // FDBK at the top
                    if (! feedsItself)
                        expect(rms(last) <= rms(first) + 1.0e-6f, where + ": not growing (" + juce::String(rms(first)) + " -> " + juce::String(rms(last)) + ")");
                    expectEquals(b.rack.getNonFiniteCount(), 0, where);
                }
        }

        beginTest("RPT: the first slice is the sound, then it comes round again; pressure shortens it; QNT starts it on the grid");
        {
            {
                Bench b;                                // 120 BPM at 48 kHz: 1/16 = 6000 samples
                auto p = b.pad(0, RPT);
                expect(p.mode == PUNCH && p.pressure[0] == 1.0f, "RPT is a punch, with pressure on DIV");
                expectEquals((int) p.cell[0], 3);
                b.set(0, p);
                b.rack.press(0, 0);
                const auto in = b.noise(0.6);
                const auto out = b.through(in);
                expect(apart(out, 0, in, 0, 5800) < 1.0e-6f, "the first slice is the sound itself");
                for (int repeat = 1; repeat <= 3; ++repeat)
                    expect(apart(out, (size_t) repeat * 6000 + 200, in, 200, 5600) < 1.0e-6f,
                           "repeat " + juce::String(repeat) + " is the first slice again: " + juce::String(apart(out, (size_t) repeat * 6000 + 200, in, 200, 5600)));
                expect(step(out, 5000, 20000) < 2.0f * step(in), "the joins are faded");

                // Pressed hard: 1/64, 1500 samples, from the next repeat on.
                b.rack.setPressure(0, 1.0f);
                const auto fast = b.through(b.noise(0.6));
                const size_t from = 18000;
                Signal a(fast.begin() + (long) from, fast.begin() + (long) from + 6000), later(fast.begin() + (long) from + 1500, fast.begin() + (long) from + 7500);
                expect(correlation(a, later, 0, 6000) > 0.9f, "pressed hard the slice is a 1/64: " + juce::String(correlation(a, later, 0, 6000)));
                b.rack.release(0);
                b.through(b.noise(0.1));
                const auto in3 = b.noise(0.05);
                expect(b.through(in3) == in3);
            }
            {
                Bench b;
                b.ctx.playing = true;
                auto p = b.pad(0, RPT);
                b.ofPad(0, [](PerfFx::Setting& s) { s.quantize = Q_4; });      // the beat: every 24000 samples
                const auto lead = b.noise(10048.0 / 48000.0);
                b.through(lead);
                b.rack.press(0, 0);                     // between two beats
                const auto in = b.noise(1.0);
                const auto out = b.through(in);
                const size_t beat = 24000 - 10048;      // where the next beat falls in `in`
                expect(apart(out, 0, in, 0, beat + 5800) < 1.0e-6f, "untouched until the beat, and the first slice from it");
                expect(apart(out, beat + 6200, in, beat + 200, 5600) < 1.0e-6f,
                       "the slice starts on the beat, to the sample: " + juce::String(apart(out, beat + 6200, in, beat + 200, 5600)));
                // Stopped, a press does not wait.
                Bench stopped;
                auto q = stopped.pad(0, RPT);
                stopped.ofPad(0, [](PerfFx::Setting& s) { s.quantize = Q_BAR; });
                juce::ignoreUnused(q);
                stopped.rack.press(0, 0);
                const auto in2 = stopped.noise(0.3);
                const auto out2 = stopped.through(in2);
                expect(apart(out2, 6200, in2, 200, 5600) < 1.0e-6f, "stopped, a press does not wait");
            }
        }

        beginTest("STOP: the pitch falls away to nothing in TIME; let go, the sound is back");
        {
            Bench b;
            auto p = b.pad(0, STOP);
            p.cell[0] = 0.5f;
            b.set(0, p);
            b.rack.press(0, 0);
            const auto in = b.sine(1.0, 440.0);
            const auto out = b.through(in);
            auto crossings = [](const Signal& s, size_t from, size_t to)
            {
                int n = 0;
                for (size_t i = from + 1; i < to; ++i) n += (s[i - 1] < 0.0f) != (s[i] < 0.0f);
                return n;
            };
            expect(crossings(out, 12000, 21600) < crossings(in, 12000, 21600) * 3 / 4, "slower: lower");
            expect(crossings(out, 2400, 7200) > crossings(out, 12000, 16800), "and falling");
            expect(rms(out, 26400) < 1.0e-4f, "stopped: silence (" + juce::String(rms(out, 26400)) + ")");
            b.rack.release(0);
            b.through(b.sine(0.1, 440.0));
            const auto in2 = b.sine(0.05, 440.0);
            expect(b.through(in2) == in2);
        }

        beginTest("Pressure moves the chosen value, either way, with the chosen curve; only while the pad is down");
        {
            auto bright = [this](float pressure, float amount, int curve)
            {
                Bench b;
                auto p = b.pad(0, insert(EffectType::FilterEffectType));
                p.cell[0] = 12000.0f;
                p.pressure.fill(0.0f);
                p.pressure[0] = amount;
                b.set(0, p);
                b.ofPad(0, [curve](PerfFx::Setting& s) { s.pressureCurve = curve; });
                b.rack.press(0, 0);
                b.rack.setPressure(0, pressure);
                const auto out = b.through(b.noise(0.4));
                return brightness(out, 9600);
            };
            const float open = bright(0.0f, -0.7f, LINEAR), closed = bright(1.0f, -0.7f, LINEAR);
            expect(closed < 0.4f * open, "pressed hard the filter closes: " + juce::String(open) + " -> " + juce::String(closed));
            expectWithinAbsoluteError(bright(1.0f, 0.0f, LINEAR), open, 0.02f);
            const float half = bright(0.5f, -0.7f, LINEAR);
            expect(bright(0.5f, -0.7f, SOFT) < half && half < bright(0.5f, -0.7f, HARD), "SOFT answers early, HARD late");

            // On WET, downwards: pressing takes the effect out.
            Bench b;
            auto p = b.pad(0, insert(EffectType::FilterEffectType));
            p.cell[0] = 300.0f;
            p.pressure.fill(0.0f);
            p.pressure[WET_VALUE] = -1.0f;
            b.set(0, p);
            b.rack.press(0, 0);
            b.rack.setPressure(0, 1.0f);
            b.through(b.noise(0.3));
            const auto in = b.noise(0.1);
            expect(rms(minus(b.through(in), in)) < 1.0e-4f, "WET pressed down to nothing");
            b.rack.release(0);
            expectEquals(b.rack.get(0, 0).wet, 1.0f, "what pressure does is not written into the pad");
        }

        beginTest("Latch: a press switches the pad on, the next one off");
        {
            Bench b;
            b.pad(0, DUB);
            b.ofPad(0, [](PerfFx::Setting& s) { s.latch = true; });
            b.rack.press(0, 0);
            b.rack.release(0);
            expect(b.rack.isDown(0), "let go, it stays on");
            b.through(b.noise(0.1));
            expect(b.rack.isSounding(0));
            b.rack.press(0, 0);
            b.rack.release(0);
            expect(! b.rack.isDown(0), "pressed again: off");
        }

        beginTest("Targets: a track by number, the whole mix, or the track that is selected at the press");
        {
            Bench b;
            auto p = b.pad(0, STOP);
            p.cell[0] = 0.05f;
            p.target = TRACK_BASE + 2;
            b.set(0, p);
            b.rack.press(0, 5);
            expectEquals(b.rack.actingOn(0), 2);
            const auto in = b.noise(0.3);
            expect(b.through(in, 0) == in, "another track is left alone");
            expect(b.through(in, ON_MASTER) == in);
            expect(rms(b.through(in, 2), 9600) < 1.0e-4f, "its own track is stopped");
            b.rack.release(0);
            b.through(in, 2);

            p.target = MASTER;
            b.set(0, p);
            b.rack.press(0, 5);
            expectEquals(b.rack.actingOn(0), (int) ON_MASTER);
            expect(b.through(in, 2) == in);
            expect(rms(b.through(in, ON_MASTER), 9600) < 1.0e-4f);
            b.rack.release(0);
            b.through(in, ON_MASTER);
            expectEquals(b.rack.actingOn(0), (int) NOWHERE);

            // SEL: where the selection is at the press, and it stays there while it rings.
            Bench c;
            auto q = c.pad(3, DUB);
            expectEquals(q.target, (int) SEL);
            c.rack.press(3, 4);
            c.through(c.noise(0.2), 4);
            c.rack.release(3);
            expectEquals(c.rack.actingOn(3), 4, "ringing on the track it was pressed for");
            c.rack.press(3, 6);
            c.through(c.noise(0.1), 6);
            expectEquals(c.rack.actingOn(3), 6, "pressed again with another track selected");
        }

        beginTest("In the instrument: a track's pads act on that track, MSTR's on everything; Shift + Play's silence lets go");
        {
            Rig r;
            auto level = [&r] { return rms(r.render(0.3), 4800); };
            r.audio.getEngine(0)->noteOn(48, 1.0f, -1);
            r.render(0.2);
            const float playing = level();
            expect(playing > 1.0e-3f, "a note on track 1 sounds");

            auto set = [&r](int target)
            {
                r.rack().setKind(0, 0, STOP);
                auto p = r.rack().get(0, 0);
                p.cell[0] = 0.05f;
                p.target = target;
                r.rack().setLayer(0, 0, p);
            };
            set(TRACK_BASE + 1);
            r.rack().press(0, 0);
            expect(level() > 0.5f * playing, "a pad on track 2 leaves track 1 alone");
            r.rack().release(0);
            r.render(0.1);

            set(TRACK_BASE);
            r.rack().press(0, 0);
            expect(level() < 1.0e-4f, "a pad on track 1 stops it");
            r.rack().release(0);
            r.render(0.1);
            expect(level() > 0.5f * playing, "let go: back");

            set(MASTER);
            r.rack().press(0, 0);
            expect(level() < 1.0e-4f, "MSTR stops the mix");

            auto p = r.rack().get(0);
            p.latch = true;
            r.rack().set(0, p);
            r.rack().press(0, 0);
            r.rack().release(0);
            r.audio.requestSilence();
            r.render(0.5);
            expect(! r.rack().isDown(0), "the silence lets go of a latched pad");
            expectEquals(r.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("The press itself moves values for as long as the pad is down, and adds to what pressure does");
        {
            auto bright = [this](float pressAmount, float pressureAmount, float pressure)
            {
                Bench b;
                auto p = b.pad(0, insert(EffectType::FilterEffectType));
                p.cell[0] = 12000.0f;
                p.pressure.fill(0.0f);
                p.press[0] = pressAmount;
                p.pressure[0] = pressureAmount;
                b.set(0, p);
                b.rack.press(0, 0);
                b.rack.setPressure(0, pressure);
                const float held = brightness(b.through(b.noise(0.4)), 9600);
                b.rack.release(0);
                b.through(b.noise(0.2));
                expectEquals(b.rack.get(0, 0).cell[0], 12000.0f, "the pad's own value is not changed by playing it");
                const auto in = b.noise(0.05);
                expect(b.through(in) == in);
                return held;
            };
            const float plain = bright(0.0f, 0.0f, 0.0f), pressed = bright(-0.5f, 0.0f, 0.0f);
            expect(pressed < 0.6f * plain, "a press alone closes the filter: " + juce::String(plain) + " -> " + juce::String(pressed));
            expectWithinAbsoluteError(bright(-0.5f, 0.0f, 1.0f), pressed, 0.02f);
            expect(bright(-0.5f, -0.3f, 1.0f) < 0.8f * pressed, "pressure goes on from where the press put it");
            expect(bright(-0.5f, 0.5f, 1.0f) > 0.9f * plain, "or brings it back");
        }

        beginTest("One pad, several effects: each on its own target, each value moved its own way by the one finger");
        {
            Bench b;
            // A: a filter on track 1 that pressure closes. B: a stop on the whole mix.
            // C: a filter on track 3 that the press opens half way and pressure the rest; its WET falls with pressure.
            PerfFx::Setting pad;
            pad.layer[0].kind = insert(EffectType::FilterEffectType);
            pad.layer[1].kind = STOP;
            pad.layer[2].kind = insert(EffectType::FilterEffectType);
            b.rack.set(0, pad);
            pad = b.rack.get(0);
            pad.layer[0].target = TRACK_BASE;
            pad.layer[0].cell[0] = 12000.0f;
            pad.layer[0].pressure = { -0.7f, 0.0f, 0.0f, 0.0f, 0.0f };
            pad.layer[1].target = MASTER;
            pad.layer[1].cell[0] = 0.05f;
            pad.layer[2].target = TRACK_BASE + 2;
            pad.layer[2].cell[0] = 100.0f;
            pad.layer[2].press = { 0.3f, 0.0f, 0.0f, 0.0f, 0.0f };
            pad.layer[2].pressure = { 0.4f, 0.0f, 0.0f, 0.0f, -0.5f };
            b.rack.set(0, pad);
            expectEquals(b.rack.get(0).firstLayer(), 0);
            expect(b.rack.get(0, 3).kind == NONE && ! b.rack.get(0).empty());

            // Three targets through the same blocks.
            struct Out { Signal track1, track3, mix; };
            auto play = [&b](double seconds)
            {
                Out in { b.noise(seconds), b.noise(seconds), b.noise(seconds) }, out = in;
                for (int done = 0; done < (int) in.mix.size(); done += 64)
                {
                    const int n = juce::jmin(64, (int) in.mix.size() - done);
                    b.rack.beginBlock(b.ctx, n);
                    Signal right(64);
                    for (auto* t : { &out.track1, &out.track3, &out.mix })
                    {
                        std::copy(t->begin() + done, t->begin() + done + n, right.begin());
                        b.rack.process(t == &out.track1 ? 0 : (t == &out.track3 ? 2 : (int) ON_MASTER), t->data() + done, right.data(), n);
                    }
                }
                return std::make_pair(in, out);
            };

            auto untouched = play(0.1);
            expect(untouched.first.track1 == untouched.second.track1 && untouched.first.mix == untouched.second.mix, "up: nothing");

            b.rack.press(0, 5);
            expectEquals(b.rack.actingOn(0, 0), 0);
            expectEquals(b.rack.actingOn(0, 1), (int) ON_MASTER);
            expectEquals(b.rack.actingOn(0, 2), 2);
            expectEquals(b.rack.actingOn(0, 3), (int) NOWHERE, "an empty layer acts on nothing");
            const auto light = play(0.4);
            expect(rms(light.second.mix, 9600) < 1.0e-4f, "the one press stops the mix");
            const float open1 = brightness(light.second.track1, 9600), dark3 = brightness(light.second.track3, 9600);
            const float level3 = rms(minus(light.second.track3, light.first.track3), 9600);
            expect(open1 > 0.4f * brightness(light.first.track1, 9600), "track 1's filter is open while the pad is only touched: " + juce::String(open1) + " of " + juce::String(brightness(light.first.track1, 9600)));
            expect(dark3 < 0.3f * brightness(light.first.track3, 9600), "track 3's is nearly shut (the press alone opens it a little)");

            b.rack.setPressure(0, 1.0f);
            play(0.2);
            const auto hard = play(0.4);
            const float closed1 = brightness(hard.second.track1, 9600), bright3 = brightness(hard.second.track3, 9600);
            expect(closed1 < 0.5f * open1, "pressure closes track 1's filter: " + juce::String(open1) + " -> " + juce::String(closed1));
            expect(bright3 > 1.5f * dark3, "and at the same time opens track 3's: " + juce::String(dark3) + " -> " + juce::String(bright3));
            expect(rms(minus(hard.second.track3, hard.first.track3), 9600) < 0.75f * level3, "while taking half of that effect away (its WET)");

            b.rack.release(0);
            play(0.3);
            expect(! b.rack.isSounding(0), "let go: every layer rests");
            const auto after = play(0.1);
            expect(after.first.track1 == after.second.track1 && after.first.track3 == after.second.track3 && after.first.mix == after.second.mix);

            // Two layers on one target run one after the other.
            Bench twice;
            PerfFx::Setting two;
            two.layer[0].kind = two.layer[3].kind = insert(EffectType::FilterEffectType);
            twice.rack.set(1, two);
            two = twice.rack.get(1);
            for (int l : { 0, 3 }) { two.layer[(size_t) l].cell[0] = 800.0f; two.layer[(size_t) l].pressure.fill(0.0f); }
            twice.rack.set(1, two);
            twice.rack.press(1, 0);
            const float both = brightness(twice.through(twice.noise(0.3)), 7200);
            twice.rack.release(1);
            twice.through(twice.noise(0.1));
            twice.rack.setKind(1, 3, NONE);
            twice.rack.press(1, 0);
            const float one = brightness(twice.through(twice.noise(0.3)), 7200);
            expect(both < 0.85f * one, "two filters in a row are darker than one: " + juce::String(one) + " -> " + juce::String(both));
        }

        beginTest("Projects: the pads are saved and come back; a project from before PERF gets the starting layout");
        {
            Rig r;
            PerfFx::Setting pad;
            pad.layer[0].kind = DUB;
            pad.layer[2].kind = insert(EffectType::PhaserEffect);
            r.rack().set(2, pad);
            pad = r.rack().get(2);
            pad.latch = true;
            pad.quantize = Q_8;
            pad.pressureCurve = HARD;
            auto& p = pad.layer[0];
            p.target = TRACK_BASE + 3;
            p.wet = 0.7f;
            p.cell = { 0.2f, 0.8f, 0.1f, 0.9f };
            p.sync = true;
            p.division = 9;
            p.press = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f };
            p.pressure = { -0.5f, 0.4f, -0.3f, 0.2f, -0.1f };
            pad.layer[2].target = MASTER;
            pad.layer[2].pressure[1] = 0.25f;
            r.rack().set(2, pad);
            r.rack().setKind(9, 1, insert(EffectType::HallReverbEffect));

            ProjectBridge::Live live { r.seq, r.audio };
            const auto json = juce::JSON::toString(ProjectBridge::capture(live, "perf").toVar());
            expect(json.contains("perfPads"));

            Rig other;
            ProjectBridge::Live otherLive { other.seq, other.audio };
            ProjectBridge::apply(ProjectData::fromVar(juce::JSON::parse(json)), otherLive);
            expect(other.rack().toFlat() == r.rack().toFlat(), "every pad and layer as it was saved");
            expectEquals(other.rack().get(2, 0).kind, (int) DUB);
            expectEquals(other.rack().get(2, 2).kind, insert(EffectType::PhaserEffect));
            expectEquals(other.rack().get(2, 0).press[4], 0.5f);
            expectEquals(other.rack().get(2, 0).pressure[0], -0.5f);
            expectEquals(other.rack().get(9, 1).kind, insert(EffectType::HallReverbEffect));
            expect(other.rack().get(0).empty() && other.rack().get(2).latch);

            // No pads in the file: the starting layout.
            ProjectData old = ProjectData::fromVar(juce::JSON::parse(json));
            old.perfPads.clear();
            ProjectBridge::apply(old, otherLive);
            expectEquals(other.rack().get(0, 0).kind, (int) DUB);
            expectEquals(other.rack().get(0, 0).target, (int) MASTER);
            expectEquals(other.rack().get(8, 0).target, (int) SEL);
            for (int i = 0; i < PADS; ++i)
                expect(! other.rack().get(i).empty(), "every pad of the starting layout plays");
            expectEquals(other.rack().get(2).quantize, (int) Q_16, "the starting repeat begins on the grid");
            expectEquals(other.rack().get(4, 0).pressure[0], -0.7f, "the starting filter closes under pressure");

            // Lists that are not what they should be.
            PerfFxRack& rack = r.rack();
            const auto good = rack.toFlat();
            const size_t head = 6, perLayer = (size_t) LAYER_FIELDS, perPad = (size_t) PAD_FIELDS + (size_t) LAYERS * perLayer;
            expectEquals((int) good.size(), (int) (head + PADS * perPad));
            expect(! rack.fromFlat({}) && ! rack.fromFlat({ 7.0f, 1.0f, 16.0f, 4.0f, 3.0f, 20.0f }) && rack.toFlat() == good, "not a list of pads: nothing changes");
            auto bad = good;
            for (size_t i = head; i < bad.size(); ++i)
                bad[i] = (i % 3 == 0) ? std::numeric_limits<float>::quiet_NaN() : ((i % 3 == 1) ? 1.0e30f : -1.0e30f);
            expect(rack.fromFlat(bad));
            for (int i = 0; i < PADS; ++i)
                for (auto& l : rack.get(i).layer)
                {
                    expect(std::isfinite(l.wet) && l.wet >= 0.0f && l.wet <= 1.0f);
                    for (float c : l.cell) expect(std::isfinite(c));
                    for (int v = 0; v < VALUES; ++v)
                        expect(std::abs(l.press[(size_t) v]) <= 1.0f && std::abs(l.pressure[(size_t) v]) <= 1.0f);
                }
            auto unknown = good;
            unknown[head + 2 * perPad + PAD_FIELDS] = 77.0f;        // pad 3, layer A: a kind this build does not have
            expect(rack.fromFlat(unknown));
            expectEquals(rack.get(2, 0).kind, (int) NONE);
            expectEquals(rack.get(2, 2).kind, insert(EffectType::PhaserEffect), "its other layers are read");
            auto shortList = good;
            shortList.resize(head + perPad * 3);                    // three pads only
            expect(rack.fromFlat(shortList));
            expectEquals(rack.get(2, 0).kind, (int) DUB);
            expect(rack.get(9).empty());
            // More fields and more layers than this build knows (a later one's file): the known ones are read.
            std::vector<float> wider { 2.0f, 1.0f, 1.0f, 6.0f, 5.0f, 23.0f };
            const auto padAt = good.begin() + (long) (head + 2 * perPad);
            wider.insert(wider.end(), padAt, padAt + PAD_FIELDS);
            wider.insert(wider.end(), { 9.0f, 9.0f });
            for (int l = 0; l < 6; ++l)
            {
                if (l < LAYERS) wider.insert(wider.end(), padAt + PAD_FIELDS + l * (long) perLayer, padAt + PAD_FIELDS + (l + 1) * (long) perLayer);
                else            wider.insert(wider.end(), perLayer, 0.0f);
                wider.insert(wider.end(), { 1.0f, 2.0f, 3.0f });
            }
            expect(rack.fromFlat(wider));
            expectEquals(rack.get(0, 0).kind, (int) DUB);
            expectEquals(rack.get(0, 0).pressure[4], -0.1f);
            expectEquals(rack.get(0, 2).pressure[1], 0.25f);
            expectEquals(rack.get(0).pressureCurve, (int) HARD);

            // A list of another version is not read.
            auto other2 = good;
            other2[0] = 1.0f;
            expect(! rack.fromFlat(other2));
        }

        beginTest("The screen: Shift + bottom pad 7 opens it; pads play, pressure arrives, the pad touched is on the knobs");
        {
            Rig r;
            r.audio.loadFactoryPerfPads();
            expectEquals(r.mgr.getActiveScreen()->getName(), juce::String("OTHER"));
            r.shift(true); r.padOn(14); r.padOff(14); r.shift(false);
            expectEquals(r.mgr.getActiveScreen()->getName(), juce::String("PERF"));
            expectEquals(r.screen->getSubPage(), juce::String("DUB>MSTR"));

            r.seq.setSelectedTrack(2);
            r.padOn(10);
            expect(r.rack().isDown(10));
            expectEquals(r.rack().actingOn(10), 2, "a SEL pad takes the selected track");
            expectEquals(r.screen->getSubPage(), juce::String("RPT>SEL"));
            expectEquals(r.screen->getEncoderName(4), juce::String("DIV"), "the effect's own cells are on knobs 5 to 8");
            r.padOff(10);
            expect(! r.rack().isDown(10));

            // Pad lights: dim waiting, full while down.
            std::array<Screen::PadColour, 16> waiting, down;
            r.screen->getPadColours(waiting);
            r.padOn(0);
            r.screen->getPadColours(down);
            expect(down[0].r > 2 * waiting[0].r && waiting[0].r > 0, "a pad lights up under the finger");
            expect(waiting[0].r != waiting[1].r || waiting[0].b != waiting[1].b, "kinds have their own colours");
            r.padOff(0);

            // An empty pad says so.
            r.rack().clear(5);
            r.padOn(5); r.padOff(5);
            expect(r.screen->getSubPage().startsWith("EMPTY"), r.screen->getSubPage());
            expect(! r.rack().isDown(5));
        }

        beginTest("The screen's knobs: FX page, the PRESS and PRES amounts under the same knobs as their values, MORE; undo");
        {
            Rig r;
            r.mgr.setActiveScreen(1);
            auto lay = [&r]() -> const PerfFx::Layer& { return r.rack().get(0, 0); };
            r.padOn(0); r.padOff(0);                    // an empty pad, shown
            UiClock::advance(2000);
            expectEquals(r.screen->getSubPage(), juce::String("A EMPTY"));
            r.turn(2, 1);
            expectEquals(r.screen->getSubPage(), juce::String("NO EFFECT"));
            UiClock::advance(2000);

            r.turn(1, 1);                               // FX: the first of the list
            expectEquals(lay().kind, (int) DUB);
            r.turn(1, 1);
            expectEquals(lay().kind, (int) FRZ);
            r.turn(0, 1);
            expectEquals(lay().target, (int) MASTER);
            r.turn(0, 3);
            expectEquals(lay().target, TRACK_BASE + 2);
            expectEquals(r.screen->getSubPage(), juce::String("FRZ>T3"));
            r.turn(0, 60);
            expectEquals(lay().target, TRACK_BASE + r.seq.getPattern().getTrackCount() - 1, "no further than the last track");

            r.turn(2, -1);
            expectEquals(r.screen->getSubPage(), juce::String("THROW ONLY"));
            expectEquals(lay().mode, (int) THROW);
            UiClock::advance(2000);

            const float before = lay().cell[1];
            r.turn(5, 5);
            expect(lay().cell[1] > before, "knob 6 is the effect's second cell (SHIM)");
            r.turn(3, -10);
            expect(lay().wet < 0.95f);

            // Undo takes the FX change back (both clicks were one step) - with the pad's old settings.
            r.undo();
            expect(r.rack().get(0).empty(), "undo: the pad as it was before FX was turned");

            // A delay in time with the tempo shows the division on its TIME knob.
            r.rack().setKind(0, 0, DUB);
            expect(lay().sync);
            r.screen->render(dummyDisplay());
            const int division = lay().division;
            r.turn(4, 1);
            expectEquals(lay().division, division + 1, "TIME turns the division while SYNC is on");

            // PRESS: the pad's latch and grid, then an amount for WET (knob 4) and each value (knobs 5 - 8).
            r.tap(52);
            expectEquals(r.screen->getPageIndex(), 1);
            expectEquals(r.screen->getSubPage(), juce::String("PRESS"));
            for (int knob = 3; knob < 8; ++knob)
            {
                static const char* const names[] = { "WET", "TIME", "FDBK", "TONE", "DRIV" };
                expectEquals(r.screen->getEncoderName(knob), juce::String(names[knob - 3]), "the amount sits under its value's knob");
            }
            r.turn(0, 1);
            expect(r.rack().get(0).latch);
            r.turn(1, 2);
            expectEquals(r.rack().get(0).quantize, (int) Q_8);
            r.turn(5, 30);
            expectWithinAbsoluteError(lay().press[1], 0.3f, 0.011f);
            r.turn(7, -20);
            expectWithinAbsoluteError(lay().press[3], -0.2f, 0.011f);
            r.turn(3, -10);
            expectWithinAbsoluteError(lay().press[WET_VALUE], -0.1f, 0.011f);
            r.turn(2, 1);
            expectEquals(r.screen->getSubPage(), juce::String("PRESS"), "a knob with nothing over it does nothing");

            // PRES: the curve, and the same five amounts for pressure. A press and pressure can both move one value.
            r.tap(52);
            expectEquals(r.screen->getSubPage(), juce::String("PRESSURE"));
            expectEquals(r.screen->getEncoderName(5), juce::String("FDBK"));
            r.turn(0, 2);
            expectEquals(r.rack().get(0).pressureCurve, (int) HARD);
            const float fdbk = lay().pressure[1];
            expect(fdbk > 0.0f, "DUB starts with pressure on FDBK");
            r.turn(6, -60); r.turn(6, -60);
            expectEquals(lay().pressure[2], -1.0f);
            r.turn(3, 40);
            expectWithinAbsoluteError(lay().pressure[WET_VALUE], 0.4f, 0.011f);
            expectWithinAbsoluteError(lay().press[1], 0.3f, 0.011f, "the press amounts are their own");
            expectEquals(lay().pressure[1], fdbk);

            // MORE: SYNC and DIV; an effect with no time of its own cannot be put in time.
            r.tap(52);
            expectEquals(r.screen->getSubPage(), juce::String("MORE"));
            r.turn(0, -1);
            expect(! lay().sync);
            r.rack().setKind(0, 0, insert(EffectType::DriveEffect));
            r.screen->render(dummyDisplay());
            r.turn(0, 1);
            expectEquals(r.screen->getSubPage(), juce::String("NO TIME CELL"));
            expect(! lay().sync);
            UiClock::advance(2000);

            // CLR: four clicks, not too fast; one click does nothing.
            r.turn(7, 1);
            expectEquals(lay().kind, insert(EffectType::DriveEffect));
            for (int i = 0; i < 4; ++i) { UiClock::advance(150); r.turn(7, 1); }
            expect(r.rack().get(0).empty(), "four clicks clear the pad");
            r.undo();
            expectEquals(lay().kind, insert(EffectType::DriveEffect), "and undo brings it back");
        }

        beginTest("Layers: Up / Down choose the layer on the knobs; one press plays every layer of the pad");
        {
            Rig r;
            r.audio.loadFactoryPerfPads();
            r.mgr.setActiveScreen(1);
            r.padOn(0); r.padOff(0);                    // DUB on the mix
            expectEquals(r.screen->getShownLayer(), 0);
            r.tap(107);                                 // Down: layer B
            expectEquals(r.screen->getShownLayer(), 1);
            expectEquals(r.screen->getSubPage(), juce::String("B EMPTY"));
            UiClock::advance(2000);
            r.turn(1, 3);                               // FX: RPT on layer B
            expectEquals(r.rack().get(0, 1).kind, (int) RPT);
            expectEquals(r.rack().get(0, 0).kind, (int) DUB, "layer A is as it was");
            r.turn(0, 2);                               // TRK: T1, for this layer alone
            expectEquals(r.rack().get(0, 1).target, (int) TRACK_BASE);
            expectEquals(r.rack().get(0, 0).target, (int) MASTER);
            expectEquals(r.screen->getSubPage(), juce::String("RPT>T1"));
            r.tap(52);                                  // PRESS: this layer's amounts
            expectEquals(r.screen->getEncoderName(4), juce::String("DIV"));
            r.turn(4, 50);
            expectWithinAbsoluteError(r.rack().get(0, 1).press[0], 0.5f, 0.011f);
            expectEquals(r.rack().get(0, 0).press[0], 0.0f);

            r.padOn(0);
            expectEquals(r.rack().actingOn(0, 0), (int) ON_MASTER);
            expectEquals(r.rack().actingOn(0, 1), 0, "the same press plays layer B on track 1");
            expectEquals(r.screen->getShownLayer(), 1, "playing the shown pad keeps its layer on the knobs");
            r.padOff(0);

            r.tap(106);                                 // Up: back to A
            expectEquals(r.screen->getShownLayer(), 0);
            r.tap(106);                                 // and round to D
            expectEquals(r.screen->getShownLayer(), 3);
            // Another pad: its first effect is shown.
            r.rack().clear(6);
            r.rack().setKind(6, 2, FRZ);
            r.padOn(6); r.padOff(6);
            expectEquals(r.screen->getShownLayer(), 2);
        }

        beginTest("Copy, clear and paste act on the shown pad, all its layers, and can be undone");
        {
            Rig r;
            r.audio.loadFactoryPerfPads();
            r.rack().setKind(1, 3, DUB);                // pad 2: FRZ on the mix, and a DUB on layer D
            r.mgr.setActiveScreen(1);
            using Edit = ScreenManager::Edit;
            r.padOn(1); r.padOff(1);
            expect(r.mgr.getEditPlan(Edit::Clear).names.contains("PERF PAD 2"), r.mgr.getEditPlan(Edit::Clear).names.joinIntoString("|"));
            r.edit(117);                                // Func + Record: copy
            UiClock::advance(3000);
            r.padOn(12); r.padOff(12);
            expectEquals(r.rack().get(12, 0).kind, insert(EffectType::FilterEffectType));
            r.edit(104);                                // Func + >: paste
            expectEquals(r.rack().get(12, 0).kind, (int) FRZ);
            expectEquals(r.rack().get(12, 3).kind, (int) DUB, "every layer");
            expectEquals(r.rack().get(12, 0).target, (int) MASTER, "the whole pad, its targets too");
            UiClock::advance(3000);
            r.undo();
            expectEquals(r.rack().get(12, 0).kind, insert(EffectType::FilterEffectType), "undo of the paste");
            expectEquals(r.rack().get(12, 3).kind, (int) NONE);
            UiClock::advance(3000);
            r.edit(115);                                // Func + Play: clear
            expect(r.rack().get(12).empty());
            UiClock::advance(3000);
            r.undo();
            expectEquals(r.rack().get(12, 0).kind, insert(EffectType::FilterEffectType));
        }

        beginTest("Pinned, the pads go on playing from another screen");
        {
            Rig r;
            r.audio.loadFactoryPerfPads();
            r.mgr.setActiveScreen(1);
            r.shift(true);
            r.padOn(14);
            r.now += 700;
            r.padOff(14);
            r.shift(false);
            expectEquals(r.mgr.getPinnedScreen(), 1);
            r.mgr.setActiveScreen(0);
            r.padOn(3);
            expect(r.rack().isDown(3), "a PERF pad from the other screen");
            r.padPressure(3, 127);
            r.padOff(3);
            expect(! r.rack().isDown(3));
        }

        beginTest("Every kind at the ends of its cells, at four sample rates: finite, bounded, back to the sound itself");
        {
            for (double sr : { 8000.0, 44100.0, 96000.0, 384000.0 })
                for (int kind : kinds)
                {
                    if (kind == NONE) continue;
                    for (int end = 0; end < (sr == 44100.0 || sr == 96000.0 ? 3 : 1); ++end)        // 0 as made, 1 all low, 2 all high
                    {
                        Bench b(sr);
                        auto p = b.pad(0, kind);
                        for (int i = 0; i < 4; ++i)
                        {
                            const auto spec = b.rack.cellSpec(0, 0, i);
                            if (spec.isValid() && end > 0)
                                p.cell[(size_t) i] = end == 1 ? spec.minVal : spec.maxVal;
                        }
                        if (end > 0)
                        {
                            // Press and pressure both pushing everything further the same way.
                            p.press.fill(end == 1 ? -1.0f : 1.0f);
                            p.pressure.fill(end == 1 ? -1.0f : 1.0f);
                        }
                        b.set(0, p);
                        b.rack.press(0, 0);
                        b.rack.setPressure(0, 1.0f);
                        const auto held = b.through(b.noise(0.3, 0.5f));
                        b.rack.release(0);
                        const auto after = b.through(b.noise(0.2, 0.5f));
                        const juce::String where = juce::String(AudioEngine::perfKindName(kind)) + " at " + juce::String(sr) + " end " + juce::String(end);
                        expect(finite(held) && finite(after), where + " finite");
                        expect(peak(held) < 16.0f && peak(after) < 16.0f, where + " bounded: " + juce::String(peak(held)));
                        expectEquals(b.rack.getNonFiniteCount(), 0, where);
                    }
                }
        }

        beginTest("No clicks: a press and a release step no harder than the sound and the effect themselves");
        {
            for (int kind : kinds)
            {
                if (kind == NONE) continue;
                Bench b;
                auto p = b.pad(0, kind);
                p.pressure.fill(0.0f);
                b.set(0, p);
                b.through(b.sine(0.1, 220.0));
                const auto dry = b.sine(0.1, 220.0);
                const float sound = step(b.through(dry));
                b.rack.press(0, 0);
                const auto pressed = b.through(b.sine(0.5, 220.0));
                b.rack.release(0);
                const auto released = b.through(b.sine(0.5, 220.0));
                // The effect's own roughness, once it is fully in (a crusher steps, a repeat joins).
                const float own = std::max(sound, step(pressed, 12000));
                const juce::String name = AudioEngine::perfKindName(kind);
                expect(step(pressed, 0, 2400) <= 1.6f * own + 1.0e-3f,
                       name + " press: " + juce::String(step(pressed, 0, 2400)) + " against " + juce::String(own));
                expect(step(released, 0, 4800) <= 1.6f * own + 1.0e-3f,
                       name + " release: " + juce::String(step(released, 0, 4800)) + " against " + juce::String(own));
            }
        }
    }

private:
    static DisplayRenderer& dummyDisplay()
    {
        static DisplayRenderer display;
        return display;
    }
};

static PerfFxTests perfFxTests;
