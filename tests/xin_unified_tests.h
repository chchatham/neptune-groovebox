#pragma once

#include "track_input_tests.h"
#include "fast_measure.h"
#include "../src/engine/UnifiedVoiceEngine.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

// -----------------------------------------------------------------------------
// XIN on the unified voice: the track's input (TRK FX page IN) at audio rate, in
// every voice, sample by sample (UnifiedVoice::XinMode; cells XIN and XDPT on the
// XIN page).
//
//   PM    the input moves oscillator 1's phase on the cross-modulation path
//   RING  the mixer's output times the input, crossfaded by XDPT
//   SYNC  oscillator 1 starts again when the input rises through the threshold
//         (XDPT = sensitivity), with hysteresis
//
// The engine is fed through setAudioInput, as AudioEngine feeds it; one test goes
// through the real routing. "The same sound" is the same samples. Lines of the
// spectrum are Goertzel powers; levels are RMS.
// -----------------------------------------------------------------------------
class XinUnifiedTests : public juce::UnitTest
{
public:
    XinUnifiedTests() : UnitTest("XIN Unified") {}

    using Input = std::function<float (int)>;       // the input's sample n (both sides)

    static constexpr int OFF = UnifiedVoice::XinOff, PM = UnifiedVoice::XinPm, RING = UnifiedVoice::XinRing, SYNC = UnifiedVoice::XinSync;

    // One oscillator, no noise, no drift, the filter wide open, an envelope that holds: what the
    // input does is all there is to see. `wave` is oscillator 1's.
    static std::unique_ptr<UnifiedVoiceEngine> plain(double sr, int block, UnifiedVoice::OscWaveform wave = UnifiedVoice::OscWaveform::Sine)
    {
        using namespace UnifiedParam;
        auto e = std::make_unique<UnifiedVoiceEngine>();
        e->prepareToPlay(sr, block);
        for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
            e->getVoice(v).getAnalogCharacter().enabled = false;
        e->setParameter(WAVE1, (float) (int) wave);
        e->setParameter(MIX, 0.0f);
        e->setParameter(SUB, 0.0f);
        e->setParameter(NOISE, 0.0f);
        e->setParameter(CUTOFF, 20000.0f);
        e->setParameter(RESONANCE, 0.0f);
        e->setParameter(KEYTRACK, 0.0f);
        e->setParameter(AMP_ATTACK, 0.001f);
        e->setParameter(AMP_SUSTAIN, 1.0f);
        e->setParameter(VEL_SENS, 0.0f);
        return e;
    }

    struct Feed
    {
        Feed() : shortBy(0), mono(false) {}
        Feed(Input in, int fewer = 0, bool oneSide = false) : input(std::move(in)), shortBy(fewer), mono(oneSide) {}
        Input input;                // empty = the track has no input
        int shortBy;                // the input's frames are this many fewer than the block's
        bool mono;                  // right == nullptr
    };

    // `seconds` of the engine's left side, block by block, the input handed over as AudioEngine does.
    static std::vector<float> render(EngineBase& e, double sr, int block, double seconds, const Feed& feed = Feed())
    {
        const int total = (int) (seconds * sr);
        std::vector<float> out;
        out.reserve((size_t) (total + block));
        std::vector<float> inL((size_t) block), inR((size_t) block);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        int n = 0;
        while ((int) out.size() < total)
        {
            AudioInput in;
            if (feed.input)
            {
                for (int i = 0; i < block; ++i)
                    inL[(size_t) i] = inR[(size_t) i] = feed.input(n + i);
                in.left = inL.data();
                in.right = feed.mono ? nullptr : inR.data();
                in.frames = std::max(0, block - feed.shortBy);
            }
            e.setAudioInput(in);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
                out.push_back(buf.getSample(0, i));
            n += block;
        }
        out.resize((size_t) total);
        return out;
    }

    static Input sine(double hz, float amp, double sr = 48000.0)
    {
        return [=](int n) { return amp * (float) std::sin(6.283185307179586 * hz * (double) n / sr); };
    }
    static float hashNoise(int n)
    {
        uint32_t x = (uint32_t) n * 2654435761u + 12345u;
        x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
        return (float) (x & 0xffff) / 32768.0f - 1.0f;
    }

    static double rms(const std::vector<float>& x, size_t from = 0)
    {
        double sq = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            sq += (double) x[i] * x[i];
        return std::sqrt(sq / (double) std::max<size_t>(1, x.size() - from));
    }
    static bool same(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }
    static bool bounded(const std::vector<float>& x, float limit)
    {
        for (float s : x)
            if (! std::isfinite(s) || std::abs(s) > limit)
                return false;
        return true;
    }
    // Power of the line at `hz` over the steady part of a 48 kHz render.
    static double line(const std::vector<float>& x, double hz)
    {
        return fastmeasure::goertzel(x, 9600, x.size() - 9600, hz, 48000.0);
    }
    static int period(const std::vector<float>& x, double expected, double& correlation)
    {
        return fastmeasure::bestLag(x, 9600, expected, correlation);
    }

    void runTest() override
    {
        using namespace UnifiedParam;
        const double SR = 48000.0;
        const int BLOCK = 64;

        beginTest("The cells: XIN (OFF PM RING SYNC) and XDPT on the XIN page, ids 148 and 149");
        {
            expectEquals((int) XIN, 148);
            expectEquals((int) XIN_DEPTH, 149);
            expect(EngineParamRange::contains(XIN) && EngineParamRange::contains(XIN_DEPTH));
            expect(XIN_DEPTH < MAX_ID);
            // The stored numbers.
            expectEquals((int) UnifiedVoice::XinOff, 0);
            expectEquals((int) UnifiedVoice::XinPm, 1);
            expectEquals((int) UnifiedVoice::XinRing, 2);
            expectEquals((int) UnifiedVoice::XinSync, 3);

            UnifiedVoiceEngine e;
            const ParamSpec* mode = nullptr;
            const ParamSpec* depth = nullptr;
            int users = 0;
            juce::String pageOf, before, after;
            const auto pages = e.getParamPages();
            for (size_t p = 0; p < pages.size(); ++p)
                for (auto& spec : pages[p].params)
                {
                    if (spec.isValid() && (spec.id == XIN || spec.id == XIN_DEPTH))
                    {
                        ++users;
                        pageOf = pages[p].name;
                        expectEquals(juce::String(pages[p].section), juce::String("SRC"));
                        before = pages[p - 1].name;
                        after = pages[p + 1].name;
                    }
                    if (spec.id == XIN) mode = &spec;
                    if (spec.id == XIN_DEPTH) depth = &spec;
                }
            // U4-UV: the two cells are on no page any more: XIN is the modulation router's XIN page
            // (its routes reach PM, RING and SYNC), and a stored XIN / XDPT becomes its first route
            // (UnifiedVoiceEngine::setLegacyXin). The router's page is checked in "U4 Unified".
            expectEquals(users, 0, "the old cells are on no page");
            juce::ignoreUnused(pageOf, before, after);
            if (mode != nullptr && depth != nullptr)
            {
                expectEquals(juce::String(mode->label), juce::String("XIN"));
                expect(mode->kind == ParamKind::Choice);
                expectEquals(mode->numChoices, 4);
                const char* const names[] = { "OFF", "PM", "RING", "SYNC" };
                for (int i = 0; i < 4; ++i)
                    expectEquals(juce::String(mode->choices[i]), juce::String(names[i]));
                expectEquals(mode->defaultVal, 0.0f);
                expectEquals(juce::String(depth->label), juce::String("XDPT"));
                expect(depth->kind == ParamKind::Linear && depth->minVal == 0.0f && depth->maxVal == 1.0f);
                expectEquals(depth->defaultVal, 0.5f);
            }
            expectEquals(e.getParameter(XIN), 0.0f, "a new sound: off");
            expectEquals(e.getParameter(XIN_DEPTH), 0.5f);
            e.setParameter(XIN, 2.0f);
            e.setParameter(XIN_DEPTH, 0.25f);
            expectEquals(e.getParameter(XIN), 2.0f);
            expectEquals(e.getParameter(XIN_DEPTH), 0.25f);
            e.setParameter(XIN, 99.0f);
            e.setParameter(XIN_DEPTH, 7.0f);
            expectEquals(e.getParameter(XIN), 3.0f, "held to the list");
            expectEquals(e.getParameter(XIN_DEPTH), 1.0f);
            e.setParameter(XIN, -4.0f);
            e.setParameter(XIN_DEPTH, std::numeric_limits<float>::quiet_NaN());
            expectEquals(e.getParameter(XIN), 0.0f);
            {
                // (a depth that is not a number is no depth: the sound stays a sound)
                auto v = plain(SR, BLOCK);
                v->setParameter(XIN, (float) RING);
                v->setParameter(XIN_DEPTH, std::numeric_limits<float>::quiet_NaN());
                v->noteOn(69, 0.8f, -1);
                const auto out = render(*v, SR, BLOCK, 0.1, { sine(100.0, 1.0f) });
                expect(bounded(out, 2.0f) && rms(out) > 0.05);
            }
            expectEquals(e.getParameterName(XIN), juce::String("XIN"));
            expectEquals(e.getParameterName(XIN_DEPTH), juce::String("XDPT"));
            // An architecture leaves the cells alone (they belong to the track's wiring, not to the voice's layout).
            e.setParameter(XIN, 1.0f);
            e.setParameter(XIN_DEPTH, 0.3f);
            e.setParameter(ARCH, 2.0f);
            expectEquals(e.getParameter(XIN), 1.0f);
            expectEquals(e.getParameter(XIN_DEPTH), 0.3f);
        }

        beginTest("XIN OFF, or no input: the same samples as an engine that never heard of the cells");
        {
            const Input loud = [](int n) { return 0.9f * hashNoise(n); };
            // The default sound, and the three architectures (several oscillators, the stack, filters).
            for (int arch = 0; arch <= 3; ++arch)
            {
                auto make = [&]
                {
                    auto e = std::make_unique<UnifiedVoiceEngine>();
                    e->prepareToPlay(SR, BLOCK);
                    if (arch > 0)
                        e->setParameter(ARCH, (float) arch);
                    e->setParameter(CROSS_MOD, 0.3f);       // the path PM shares
                    for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                        e->getVoice(v).getAnalogCharacter().enabled = false;
                    e->noteOn(48, 0.8f, -1);
                    e->noteOn(55, 0.6f, -1);
                    return e;
                };
                const juce::String where = "ARCH " + juce::String(arch);
                const auto reference = render(*make(), SR, BLOCK, 0.3);
                expect(rms(reference) > 0.01, where + " sounds");
                expect(same(reference, render(*make(), SR, BLOCK, 0.3)), where + ": the render repeats");

                auto off = make();
                off->setParameter(XIN_DEPTH, 1.0f);
                expect(same(reference, render(*off, SR, BLOCK, 0.3, { loud })), where + ": OFF with a loud input and XDPT up");
                for (int mode : { PM, RING, SYNC })
                {
                    auto e = make();
                    e->setParameter(XIN, (float) mode);
                    e->setParameter(XIN_DEPTH, 1.0f);
                    expect(same(reference, render(*e, SR, BLOCK, 0.3)), where + ", mode " + juce::String(mode) + ": no input");
                    // An input of no frames is no input.
                    auto none = make();
                    none->setParameter(XIN, (float) mode);
                    none->setParameter(XIN_DEPTH, 1.0f);
                    expect(same(reference, render(*none, SR, BLOCK, 0.3, { loud, BLOCK })), where + ", mode " + juce::String(mode) + ": no frames");
                    // ... and with an input the mode is heard (the test can fail).
                    auto on = make();
                    on->setParameter(XIN, (float) mode);
                    on->setParameter(XIN_DEPTH, 1.0f);
                    expect(! same(reference, render(*on, SR, BLOCK, 0.3, { loud })), where + ", mode " + juce::String(mode) + ": heard with an input");
                }
            }
        }

        beginTest("PM: sidebands at the carrier +- the input's frequency, growing with XDPT");
        {
            // A 440 Hz sine, a 110 Hz input at half scale: index = pi * XDPT * 0.5 * 440 / 110.
            auto run = [&](float depth, const Input& in, float xmod = 0.0f)
            {
                auto e = plain(SR, BLOCK);
                e->setParameter(XIN, (float) PM);
                e->setParameter(XIN_DEPTH, depth);
                e->setParameter(CROSS_MOD, xmod);
                e->noteOn(69, 0.8f, -1);
                return render(*e, SR, BLOCK, 1.0, { in });
            };
            auto dryEngine = plain(SR, BLOCK);
            dryEngine->noteOn(69, 0.8f, -1);
            const auto dry = render(*dryEngine, SR, BLOCK, 1.0);
            const double carrier = line(dry, 440.0);
            expect(carrier > 1.0e-4);
            for (double hz : { 330.0, 550.0, 220.0, 660.0 })
                expect(line(dry, hz) < 1.0e-5 * carrier, "the voice alone has nothing at " + juce::String(hz));

            const auto in = sine(110.0, 0.5f);
            expect(same(dry, run(0.0f, in)), "XDPT 0 is the voice alone");

            // XDPT 20: index 1.257 -> J0 0.642, J1 0.511, J2 0.176 (as amplitudes).
            const auto light = run(0.2f, in);
            const double c1 = line(light, 440.0);
            for (double hz : { 330.0, 550.0 })
            {
                const double ratio = std::sqrt(line(light, hz) / c1);
                expect(ratio > 0.65 && ratio < 0.95, "first sidebands at " + juce::String(hz) + ": J1 / J0 = 0.80, measured " + juce::String(ratio, 3));
            }
            for (double hz : { 220.0, 660.0 })
            {
                const double ratio = std::sqrt(line(light, hz) / c1);
                expect(ratio > 0.2 && ratio < 0.35, "second sidebands at " + juce::String(hz) + ": J2 / J0 = 0.27, measured " + juce::String(ratio, 3));
            }
            expect(std::sqrt(c1 / carrier) > 0.55 && std::sqrt(c1 / carrier) < 0.72, "the carrier gives way: J0 = 0.64, measured " + juce::String(std::sqrt(c1 / carrier), 3));
            // The level is the oscillator's own: phase modulation moves energy, it adds none.
            expect(std::abs(rms(light, 9600) / rms(dry, 9600) - 1.0) < 0.03);

            // Deeper: more in the sidebands, less in the carrier.
            const auto deep = run(0.5f, in);
            expect(line(deep, 440.0) < 0.5 * c1, "XDPT 50: the carrier falls further");
            double far = 0.0, farLight = 0.0;
            for (double hz : { 110.0, 770.0, 880.0 })
            {
                far += line(deep, hz);
                farLight += line(light, hz);
            }
            expect(far > 10.0 * farLight, "... and the far sidebands rise");

            // The path is the cross-modulation's: a steady offset in the input moves the phase and
            // not the pitch, and XMOD from oscillator 2 still works beside it.
            const auto offset = run(1.0f, [](int) { return 0.8f; });
            expect(line(offset, 440.0) > 0.9 * carrier, "an offset in the input leaves the pitch at 440 Hz");
            const auto both = run(0.2f, in, 0.4f);
            expect(! same(both, light) && bounded(both, 2.0f));
            // In every voice: a chord has the sidebands of each note.
            auto chord = plain(SR, BLOCK);
            chord->setParameter(XIN, (float) PM);
            chord->setParameter(XIN_DEPTH, 0.1f);
            chord->noteOn(69, 0.8f, -1);
            chord->noteOn(81, 0.8f, -1);
            const auto two = render(*chord, SR, BLOCK, 1.0, { in });
            expect(line(two, 550.0) > 0.01 * line(two, 440.0), "440 + 110");
            expect(line(two, 990.0) > 0.01 * line(two, 880.0), "880 + 110");
        }

        beginTest("RING: sum and difference tones; silent input = silence at XDPT 100; XDPT crossfades");
        {
            auto run = [&](float depth, const Feed& feed, int notes = 1)
            {
                auto e = plain(SR, BLOCK);
                e->setParameter(XIN, (float) RING);
                e->setParameter(XIN_DEPTH, depth);
                for (int i = 0; i < notes; ++i)
                    e->noteOn(69 + 7 * i, 0.8f, -1);
                return render(*e, SR, BLOCK, 1.0, feed);
            };
            auto dryEngine = plain(SR, BLOCK);
            dryEngine->noteOn(69, 0.8f, -1);
            const auto dry = render(*dryEngine, SR, BLOCK, 1.0);
            const double carrier = line(dry, 440.0), dryRms = rms(dry, 9600);
            expect(dryRms > 0.05);

            const Feed tone { sine(100.0, 1.0f) };
            expect(same(dry, run(0.0f, tone)), "XDPT 0 is the voice alone");

            const auto full = run(1.0f, tone);
            const double lo = line(full, 340.0), hi = line(full, 540.0);
            // Each product is half the carrier's amplitude: a quarter of its power.
            expect(lo > 0.2 * carrier && lo < 0.3 * carrier, "difference tone at 340 Hz: " + juce::String(lo / carrier, 3) + " of the carrier's power");
            expect(hi > 0.2 * carrier && hi < 0.3 * carrier, "sum tone at 540 Hz: " + juce::String(hi / carrier, 3));
            expect(line(full, 440.0) < 1.0e-3 * carrier, "and the carrier is gone");
            expect(std::abs(rms(full, 9600) / dryRms - std::sqrt(0.5)) < 0.03, "level: 3 dB under the voice alone");

            const auto half = run(0.5f, tone);
            const double c = line(half, 440.0) / carrier, s = line(half, 540.0) / carrier;
            expect(c > 0.22 && c < 0.28, "XDPT 50: half of the voice (a quarter of its power): " + juce::String(c, 3));
            expect(s > 0.05 && s < 0.075, "... and half of the product: " + juce::String(s, 3));

            // A silent input (connected, all zeros): nothing. Three notes: every voice.
            const auto silent = run(1.0f, { [](int) { return 0.0f; } }, 3);
            expect(rms(silent) < 1.0e-6, "silent input, XDPT 100: rms " + juce::String(rms(silent), 8));
            const auto partly = run(0.5f, { [](int) { return 0.0f; } });
            expect(std::abs(rms(partly, 9600) / dryRms - 0.5) < 0.02, "silent input, XDPT 50: half the voice");

            // A steady +1 is the voice alone; -1 turns it over.
            const auto plus = run(1.0f, { [](int) { return 1.0f; } });
            expect(std::abs(rms(plus, 9600) / dryRms - 1.0) < 0.01);
            // Both sides are summed to mono: half scale on each is half scale.
            // The input ends before the block does. Since U4-1 the router lets an input that ends go in
            // 4 ms and lets it in again as fast (it never reads silence from the sample it ends): with
            // 32 frames of +1 in every 64-sample block the gain rises 32 / 192 and falls again, so the
            // ring never gets past a sixth of the voice: rms 0.096 of it (0.707 before U4-1, when the
            // input stopped dead half way through every block).
            const auto shortFeed = run(1.0f, { [](int) { return 1.0f; }, BLOCK / 2 });
            expect(std::abs(rms(shortFeed, 9600) / dryRms - 0.096) < 0.01,
                   "an input of half the block's frames, faded in and out by the router: " + juce::String(rms(shortFeed, 9600) / dryRms, 3) + " of the voice");
            // One side only (right == nullptr) is that side.
            const auto mono = run(1.0f, { sine(100.0, 1.0f), 0, true });
            expect(same(mono, full));
            // The noise is part of what is multiplied: the whole mixer.
            auto noisy = plain(SR, BLOCK);
            noisy->setParameter(NOISE, 0.8f);
            noisy->setParameter(XIN, (float) RING);
            noisy->setParameter(XIN_DEPTH, 1.0f);
            noisy->noteOn(69, 0.8f, -1);
            expect(rms(render(*noisy, SR, BLOCK, 0.3, { [](int) { return 0.0f; } })) < 1.0e-6);
        }

        beginTest("SYNC: oscillator 1's period follows the input's frequency; hysteresis; XDPT is the sensitivity");
        {
            // A 220 Hz saw (218.2 samples), an input at 150 Hz (320 samples).
            auto run = [&](float depth, const Feed& feed, int stack = 0, bool on = true)
            {
                auto e = plain(SR, BLOCK, UnifiedVoice::OscWaveform::Saw);
                e->setParameter(STACK, (float) stack);
                e->setParameter(XIN, (float) (on ? SYNC : OFF));
                e->setParameter(XIN_DEPTH, depth);
                e->noteOn(57, 0.8f, -1);
                return render(*e, SR, BLOCK, 1.0, feed);
            };
            double corr = 0.0;
            const auto dry = run(1.0f, {}, 0, false);
            const int dryLag = period(dry, 320.0, corr);
            expect(std::abs(dryLag - 218) <= 1 || std::abs(dryLag - 436) <= 1, "alone: 220 Hz (lag " + juce::String(dryLag) + ")");

            const Feed master { sine(150.0, 0.8f) };
            const auto synced = run(0.8f, master);
            const int lag = period(synced, 320.0, corr);
            expectEquals(lag, 320, "synced: one cycle of the input");
            expect(corr > 0.99, "... exactly (correlation " + juce::String(corr, 4) + ")");
            expect(line(synced, 150.0) > 100.0 * line(dry, 150.0), "the input's pitch is in the sound");
            expect(std::abs(rms(synced, 9600) / rms(dry, 9600) - 1.0) < 0.25, "about as loud");

            // Another input frequency, another period.
            const auto other = run(0.8f, { sine(100.0, 0.8f) });
            expectEquals(period(other, 480.0, corr), 480);
            expect(corr > 0.99);

            // The stack's copies start again together.
            const auto stacked = run(0.8f, master, 2);
            expectEquals(period(stacked, 320.0, corr), 320, "with STCK 4");
            expect(corr > 0.99);

            expect(same(dry, run(0.0f, master)), "XDPT 0: the voice alone");

            // Sensitivity: an input at a tenth of full scale is under the threshold at XDPT 50
            // (0.25) and over it at XDPT 100 (0.005).
            const Feed quiet { sine(150.0, 0.1f) };
            expect(same(dry, run(0.5f, quiet)), "a quiet input does not reach the threshold at XDPT 50");
            expectEquals(period(run(1.0f, quiet), 320.0, corr), 320, "... and does at XDPT 100");
            expect(corr > 0.99);
            expect(std::abs(UnifiedVoice::xinSyncThreshold(0.5f) - 0.25375f) < 1.0e-4f);
            expect(std::abs(UnifiedVoice::xinSyncThreshold(1.0f) - 0.005f) < 1.0e-6f);

            // Hysteresis. Noise under the threshold alone never starts the cycle again ...
            expect(same(dry, run(1.0f, { [](int n) { return 0.004f * hashNoise(n); } })), "noise of 0.004 at XDPT 100: nothing");
            // ... and noise on the input fires once per cycle of the input, not at every wiggle
            // through zero.
            // Counted on a voice whose oscillator barely moves (an 8 Hz saw, no filter): its output
            // climbs by the same small step every sample, and a step the other way is a restart
            // (or the saw's own wrap, seven in the stretch counted: what "alone" counts).
            auto restarts = [&](const Feed& feed, float depth)
            {
                auto e = plain(SR, BLOCK, UnifiedVoice::OscWaveform::Saw);
                e->setParameter(MODEL_A, (float) (FilterSlot::NUM_MODELS - 1));     // no filter
                e->setParameter(XIN, (float) SYNC);
                e->setParameter(XIN_DEPTH, depth);
                e->noteOn(0, 0.8f, -1);
                const auto x = render(*e, SR, BLOCK, 1.0, feed);
                int up = 0, down = 0;
                for (size_t i = 4801; i < x.size(); ++i)
                {
                    if (x[i] > x[i - 1]) ++up;
                    if (x[i] < x[i - 1]) ++down;
                }
                return std::min(up, down);
            };
            const int alone = restarts({}, 0.5f);
            expect(alone >= 7 && alone <= 24, "alone: the saw's own wraps (" + juce::String(alone) + ")");
            const Input noisySine = [](int n) { return 0.6f * (float) std::sin(6.283185307179586 * 150.0 * n / 48000.0) + 0.1f * hashNoise(n); };
            const int clean = restarts({ sine(150.0, 0.6f) }, 0.5f);
            const int noisy = restarts({ noisySine }, 0.5f);
            // 0.9 s of a 150 Hz input: 135 cycles.
            expect(clean >= 134 && clean <= 136, "a clean input: one restart a cycle (" + juce::String(clean) + ")");
            expect(noisy >= 134 && noisy <= 136, "a noisy input: still one a cycle (" + juce::String(noisy) + ")");
            // The same noisy input at XDPT 100, where the threshold is far under the noise: the
            // wiggles around each crossing fire too (the count is the hysteresis's doing).
            const int open = restarts({ noisySine }, 1.0f);
            expect(open > 2 * clean, "... and several a cycle once the threshold is under the noise (" + juce::String(open) + ")");
            // Noise alone at full scale restarts it all the time ...
            const int wild = restarts({ [](int n) { return 0.9f * hashNoise(n); } }, 1.0f);
            expect(wild > 3000, "noise at full scale restarts it all the time (" + juce::String(wild) + ")");
            // ... but an input has to swing from under -threshold to over +threshold to do so.
            const int oneSided = restarts({ [](int n) { return 0.5f + 0.4f * hashNoise(n); } }, 0.5f);
            expectEquals(oneSided, alone, "an input that stays above zero never fires: the saw's own wraps");
        }

        beginTest("Through the instrument: a track's XIN hears the track chosen on TRK FX IN");
        {
            auto run = [&](float src, float level)
            {
                TrackInputTests::Rig r;
                auto* tone = r.add<TrackInputTests::ToneEngine>(0);
                tone->left = tone->right = level;
                auto voice = std::shared_ptr<UnifiedVoiceEngine>(plain(48000.0, 64).release());
                r.audio.addEngine(1, voice);
                r.audio.setTrackParameter(0, MixParam::VOLUME, 0.0f);     // the source feeds from before its fader
                r.audio.setTrackParameter(1, XIN, (float) RING);
                r.audio.setTrackParameter(1, XIN_DEPTH, 1.0f);
                r.audio.setTrackParameter(1, TrackInParam::SRC, src);
                voice->noteOn(69, 0.8f, -1);
                r.blocks(40);
                std::vector<float> out;
                for (int b = 0; b < 40; ++b)
                {
                    r.blocks(1);
                    out.insert(out.end(), r.left.begin(), r.left.end());
                }
                return rms(out);
            };
            const double alone = run(0.0f, 0.0f);
            expect(alone > 0.01, "SRC OFF: the voice alone (" + juce::String(alone, 4) + ")");
            expect(run(1.0f, 0.0f) < 1.0e-5, "SRC T1, a silent track 1, RING at XDPT 100: silence");
            const double fed = run(1.0f, 0.5f);
            // (the input is tanh of the tap: 0.46)
            expect(std::abs(fed / alone - std::tanh(0.5)) < 0.03, "SRC T1 at a steady 0.5: the voice times it (" + juce::String(fed / alone, 3) + ")");
        }

        beginTest("Extremes: any input, 22.05 to 192 kHz, blocks of 1 to 4096, short frames: finite and bounded");
        {
            const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
            const Input nasty = [=](int n)
            {
                switch ((n / 3) % 12)
                {
                    case 0:  return nan;
                    case 1:  return inf;
                    case 2:  return -inf;
                    case 3:  return 1.0e30f;
                    case 4:  return -1.0e30f;
                    case 5:  return 3.0e38f;
                    case 6:  return -1.0f;
                    case 7:  return 1.0f;
                    case 8:  return 1.0e-30f;
                    default: return hashNoise(n);
                }
            };
            int runs = 0;
            for (double sr : { 22050.0, 44100.0, 96000.0, 192000.0 })
                for (int block : { 1, 13, 256, 4096 })
                    for (int mode : { PM, RING, SYNC })
                    {
                        UnifiedVoiceEngine e;
                        e.prepareToPlay(sr, block);
                        e.setParameter(ARCH, (float) (1 + runs % 3));
                        e.setParameter(STACK, (float) (runs % 4));
                        e.setParameter(CROSS_MOD, runs % 2 ? 1.0f : 0.0f);
                        e.setParameter(MIX_DRIVE, runs % 5 == 0 ? 1.0f : 0.0f);
                        e.setParameter(XIN, (float) mode);
                        e.setParameter(XIN_DEPTH, 1.0f);
                        e.noteOn(0, 1.0f, -1);
                        e.noteOn(64, 1.0f, -1);
                        e.noteOn(127, 1.0f, -1);
                        Feed feed { nasty, runs % 3 == 0 ? std::max(1, block / 2) - (block == 1 ? 1 : 0) : 0, runs % 4 == 1 };
                        const double seconds = std::max(0.03, 2.2 * block / sr);
                        const auto out = render(e, sr, block, seconds, feed);
                        const juce::String where = juce::String(sr) + " Hz, block " + juce::String(block) + ", mode " + juce::String(mode);
                        expect(bounded(out, 8.0f), where);
                        // The cells moved under the notes, then a clean input: still sound, still held.
                        e.setParameter(XIN_DEPTH, 0.0f);
                        e.setParameter(XIN, (float) (1 + (mode % 3)));
                        e.setParameter(XIN_DEPTH, 1.0f);
                        const auto after = render(e, sr, block, seconds, { sine(300.0, 1.0f, sr) });
                        expect(bounded(after, 8.0f), where + ", afterwards");
                        ++runs;
                    }
            // What the nasty input leaves is still a sound: PM and SYNC keep the oscillator's level.
            for (int mode : { PM, SYNC })
            {
                auto e = plain(SR, BLOCK);
                e->setParameter(XIN, (float) mode);
                e->setParameter(XIN_DEPTH, 1.0f);
                e->noteOn(69, 0.8f, -1);
                const auto out = render(*e, SR, BLOCK, 0.5, { nasty });
                expect(bounded(out, 2.0f) && rms(out, 4800) > 0.1, "mode " + juce::String(mode) + " under the worst input still sounds");
            }
        }

        beginTest("The input is the mono sum of its two sides");
        {
            // RING at XDPT 100 on a steady input: the level is the input's.
            auto level = [&](float l, float r)
            {
                auto e = plain(SR, BLOCK);
                e->setParameter(XIN, (float) RING);
                e->setParameter(XIN_DEPTH, 1.0f);
                e->noteOn(69, 0.8f, -1);
                const std::vector<float> inL((size_t) BLOCK, l), inR((size_t) BLOCK, r);
                juce::AudioBuffer<float> buf(2, BLOCK);
                juce::MidiBuffer midi;
                std::vector<float> out;
                for (int b = 0; b < 300; ++b)
                {
                    AudioInput in;
                    in.left = inL.data();
                    in.right = inR.data();
                    in.frames = BLOCK;
                    e->setAudioInput(in);
                    buf.clear();
                    e->processBlock(buf, midi);
                    for (int i = 0; i < BLOCK; ++i)
                        out.push_back(buf.getSample(0, i));
                }
                return rms(out, 9600);
            };
            const double whole = level(1.0f, 1.0f);
            expect(whole > 0.05);
            expect(std::abs(level(1.0f, 0.0f) / whole - 0.5) < 0.01, "left only: half");
            expect(std::abs(level(0.0f, 1.0f) / whole - 0.5) < 0.01, "right only: half");
            expect(level(0.7f, -0.7f) < 1.0e-6, "opposite sides cancel");
        }

        beginTest("PM and SYNC act on oscillator 1 alone");
        {
            // Oscillator 2 by itself (MIX 100): the input has nothing to move.
            for (int mode : { PM, SYNC })
            {
                auto make = [&]
                {
                    auto e = plain(SR, BLOCK, UnifiedVoice::OscWaveform::Saw);
                    e->setParameter(WAVE2, (float) (int) UnifiedVoice::OscWaveform::Saw);
                    e->setParameter(MIX, 1.0f);
                    e->setParameter(SUB, 0.5f);
                    e->noteOn(57, 0.8f, -1);
                    return e;
                };
                const auto alone = render(*make(), SR, BLOCK, 0.3);
                expect(rms(alone) > 0.01);
                auto e = make();
                e->setParameter(XIN, (float) mode);
                e->setParameter(XIN_DEPTH, 1.0f);
                expect(same(alone, render(*e, SR, BLOCK, 0.3, { sine(150.0, 0.8f) })), "mode " + juce::String(mode) + ": oscillators 2 and 3 are left alone");
            }
        }

        beginTest("SYNC: the restart is placed inside the sample, where the input crossed the threshold");
        {
            // Blocks of one sample, oscillator 1's phase read after each: on a restart it is one
            // step plus the part of the sample that had passed since the crossing.
            const float depth = 0.8f;
            const float threshold = UnifiedVoice::xinSyncThreshold(depth);
            auto e = plain(SR, 1, UnifiedVoice::OscWaveform::Saw);
            e->setParameter(XIN, (float) SYNC);
            e->setParameter(XIN_DEPTH, depth);
            e->noteOn(57, 0.8f, -1);
            int voice = -1;
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                if (e->getVoice(v).isActive())
                    voice = v;
            expect(voice >= 0);
            const double inc = 220.0 / SR;
            const Input in = sine(149.3, 0.8f);
            juce::AudioBuffer<float> buf(2, 1);
            juce::MidiBuffer midi;
            bool armed = false;
            float prev = 0.0f;
            int fires = 0;
            double least = 1.0, most = 0.0, worst = 0.0;
            for (int n = 0; n < 24000 && voice >= 0; ++n)
            {
                const float x = in(n);
                AudioInput feed;
                feed.left = feed.right = &x;
                feed.frames = 1;
                e->setAudioInput(feed);
                buf.clear();
                e->processBlock(buf, midi);
                if (x <= -threshold)
                    armed = true;
                else if (armed && x >= threshold)
                {
                    armed = false;
                    const double since = (double) ((x - threshold) / (x - prev));
                    const double phase = e->getVoice(voice).getOsc(0).phase;
                    worst = std::max(worst, std::abs(phase - inc * (1.0 + since)));
                    least = std::min(least, since);
                    most = std::max(most, since);
                    ++fires;
                }
                prev = x;
            }
            expect(fires >= 70, juce::String(fires) + " restarts");
            expect(most - least > 0.5, "the crossings fall all over the sample");
            expect(worst < 0.02 * inc, "the phase after a restart is off by " + juce::String(worst / inc, 4) + " of a step at most");
        }

        beginTest("A change of mode starts clean; an input that comes or goes under XIN is a swapped sound");
        {
            // SYNC armed by a low input, then RING, then SYNC again on a high input: nothing fires,
            // as if the input had never been low.
            auto run = [&](float first)
            {
                auto e = plain(SR, BLOCK, UnifiedVoice::OscWaveform::Saw);
                e->setParameter(XIN, (float) SYNC);
                e->setParameter(XIN_DEPTH, 0.5f);
                e->noteOn(57, 0.8f, -1);
                auto out = render(*e, SR, BLOCK, 0.05, { [=](int) { return first; } });
                e->setParameter(XIN, (float) RING);
                const auto ring = render(*e, SR, BLOCK, 0.05, { [](int) { return 0.9f; } });
                e->setParameter(XIN, (float) SYNC);
                const auto again = render(*e, SR, BLOCK, 0.05, { [](int) { return 0.9f; } });
                out.insert(out.end(), ring.begin(), ring.end());
                out.insert(out.end(), again.begin(), again.end());
                return out;
            };
            expect(same(run(-0.9f), run(0.9f)), "the arming of one stretch of SYNC is not carried to the next");

            auto e = plain(SR, BLOCK);
            e->noteOn(69, 0.8f, -1);
            render(*e, SR, BLOCK, 0.01, { sine(100.0, 1.0f) });
            e->takeJump();
            render(*e, SR, BLOCK, 0.01);
            expectEquals(e->takeJump(), (int) EngineBase::NoJump, "XIN OFF: an input that goes is nothing");
            e->setParameter(XIN, (float) RING);
            render(*e, SR, BLOCK, 0.01);
            e->takeJump();
            render(*e, SR, BLOCK, 0.01, { sine(100.0, 1.0f) });
            expectEquals(e->takeJump(), (int) EngineBase::SoundSwapped, "the input comes");
            render(*e, SR, BLOCK, 0.01, { sine(100.0, 1.0f) });
            expectEquals(e->takeJump(), (int) EngineBase::NoJump, "... and stays");
            render(*e, SR, BLOCK, 0.01);
            expectEquals(e->takeJump(), (int) EngineBase::SoundSwapped, "the input goes");
        }

        beginTest("Turning XDPT under a held note makes no step in the sound");
        {
            // RING with a steady input of 0: XDPT is then the level itself, 1 - XDPT.
            auto e = plain(SR, BLOCK);
            e->setParameter(XIN, (float) RING);
            e->setParameter(XIN_DEPTH, 0.0f);
            e->setParameter(WAVE1, (float) (int) UnifiedVoice::OscWaveform::Square);
            e->setParameter(TUNE, -24.0f);
            e->noteOn(0, 0.8f, -1);            // 2 Hz: a level for hundreds of samples
            const Feed zero { [](int) { return 0.0f; } };
            const auto before = render(*e, SR, BLOCK, 0.1, zero);
            e->setParameter(XIN_DEPTH, 1.0f);
            const auto during = render(*e, SR, BLOCK, 0.1, zero);
            const float held = std::abs(before.back());
            expect(held > 0.05f);
            float step = std::abs(during.front() - before.back());
            for (size_t i = 1; i < during.size(); ++i)
                step = std::max(step, std::abs(during[i] - during[i - 1]));
            expect(step < 0.02f * held + 1.0e-4f, "the largest step is " + juce::String(step / held, 4) + " of the level");
            expect(std::abs(during.back()) < 1.0e-4f, "and it arrives");
        }
    }
};

static XinUnifiedTests xinUnifiedTests;
