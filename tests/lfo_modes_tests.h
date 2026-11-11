#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <set>
#include <vector>
#include "../src/dsp/LFO.h"
#include "../src/engine/UnifiedVoiceEngine.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/project/ProjectBridge.h"

// The LFO's shapes, tempo divisions, trigger modes, delay / fade and start phase, measured on the
// LFO itself and in the unified voice's audio.
class LfoModesTests : public juce::UnitTest
{
public:
    LfoModesTests() : UnitTest("LFO Modes") {}

    // ---- helpers ---------------------------------------------------------------------------------

    static LFO makeLfo(LFO::Shape shape, float hz, double sr = 44100.0)
    {
        LFO lfo;
        lfo.setSampleRate(sr);
        lfo.setShape(shape);
        lfo.setRate(hz);
        lfo.setDepth(1.0f);
        return lfo;
    }

    // Samples between two wraps of a rising saw (measured after the first wrap).
    static int measureCycleSamples(LFO& lfo)
    {
        float prev = lfo.process();
        int first = -1;
        for (int i = 1; i < 20000000; ++i)
        {
            float v = lfo.process();
            if (v < prev - 1.0f)
            {
                if (first < 0) first = i;
                else return i - first;
            }
            prev = v;
        }
        return -1;
    }

    // One voice, one saw oscillator, open filter, flat envelope, nothing random.
    struct Synth
    {
        UnifiedVoiceEngine engine;
        double sr;

        explicit Synth(double sampleRate = 44100.0, int numVoices = 1) : engine(numVoices), sr(sampleRate)
        {
            engine.prepareToPlay(sr, 64);
            engine.setParameter(UnifiedParam::DRIFT, 0.0f);
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                engine.getVoice(v).getAnalogCharacter().enabled = false;
            engine.setParameter(UnifiedParam::MIX, 0.0f);
            engine.setParameter(UnifiedParam::SUB, 0.0f);
            engine.setParameter(UnifiedParam::CUTOFF, 20000.0f);
            engine.setParameter(UnifiedParam::FILTER_ENV, 0.0f);
            engine.setParameter(UnifiedParam::AMP_ATTACK, 0.001f);
            engine.setParameter(UnifiedParam::AMP_SUSTAIN, 1.0f);
            engine.setParameter(UnifiedParam::VEL_SENS, 0.0f);
        }

        void lfo(int offset, float value, int index = 0) { engine.setParameter(ModParam::lfoParamId(index, offset), value); }
        void ext(int offset, float value, int index = 0) { engine.setParameter(ModParam::lfoExtParamId(index, offset), value); }
        void ext2(int offset, float value, int index = 0) { engine.setParameter(ModParam::lfoExt2ParamId(index, offset), value); }

        // LFO 1 as a square on the level, restarted by the note: the level is x(1 + depth) for the
        // first half cycle and x(1 - depth) for the second.
        void squareTremolo(float hz, float depth)
        {
            lfo(ModParam::LFO_RATE, hz);
            lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square);
            lfo(ModParam::LFO_TRIG, (float) (int) LFO::TriggerMode::KeySync);
            lfo(ModParam::LFO_DEPTH, depth);
            lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Amp);
        }

        // Renders `seconds` and appends to left / right.
        void render(double seconds, std::vector<float>& left, std::vector<float>* right = nullptr)
        {
            juce::MidiBuffer midi;
            int remaining = (int) std::lround(seconds * sr);
            while (remaining > 0)
            {
                const int n = juce::jmin(64, remaining);
                juce::AudioBuffer<float> buf(2, n);
                buf.clear();
                engine.processBlock(buf, midi);
                left.insert(left.end(), buf.getReadPointer(0), buf.getReadPointer(0) + n);
                if (right) right->insert(right->end(), buf.getReadPointer(1), buf.getReadPointer(1) + n);
                remaining -= n;
            }
        }
    };

    static float rms(const std::vector<float>& x, double sr, double fromSec, double toSec)
    {
        const size_t a = (size_t) std::lround(fromSec * sr);
        const size_t b = juce::jmin(x.size(), (size_t) std::lround(toSec * sr));
        if (b <= a) return 0.0f;
        double sum = 0.0;
        for (size_t i = a; i < b; ++i) sum += (double) x[i] * x[i];
        return (float) std::sqrt(sum / (double) (b - a));
    }

    // ---- tests -----------------------------------------------------------------------------------

    void runTest() override
    {
        beginTest("Stored indices: the first six shapes and three trigger modes are where they were");
        {
            expectEquals((int) LFO::Shape::Sine, 0);
            expectEquals((int) LFO::Shape::Triangle, 1);
            expectEquals((int) LFO::Shape::Square, 2);
            expectEquals((int) LFO::Shape::Saw, 3);
            expectEquals((int) LFO::Shape::Random, 4);
            expectEquals((int) LFO::Shape::SmoothRandom, 5);
            expectEquals((int) LFO::Shape::RampDown, 6);
            expectEquals((int) LFO::Shape::ExpUp, 7);
            expectEquals((int) LFO::Shape::ExpDown, 8);
            expectEquals((int) LFO::TriggerMode::Free, 0);
            expectEquals((int) LFO::TriggerMode::KeySync, 1);
            expectEquals((int) LFO::TriggerMode::Single, 2);
            expectEquals((int) LFO::TriggerMode::Half, 3);
            expectEquals((int) LFO::TriggerMode::Hold, 4);

            using D = UnifiedVoice::LfoDest;
            const D old[] = { D::Off, D::Pitch, D::Cutoff, D::Resonance, D::OscMix, D::Amp, D::PulseWidth, D::CrossMod };
            for (int i = 0; i < 8; ++i)
                expectEquals((int) old[i], i);
            const char* oldNames[] = { "OFF", "PTCH", "CUT", "RESO", "MIX", "AMP", "PW", "XMOD" };
            for (int i = 0; i < 8; ++i)
                expectEquals(juce::String(UnifiedVoice::lfoDestLabels()[i]), juce::String(oldNames[i]));
        }

        beginTest("Ramp down falls from +1 to -1 over a cycle and is the mirror of the saw");
        {
            auto ramp = makeLfo(LFO::Shape::RampDown, 1.0f, 1000.0);
            auto saw = makeLfo(LFO::Shape::Saw, 1.0f, 1000.0);
            float prev = 2.0f;
            for (int i = 0; i < 999; ++i)
            {
                float r = ramp.process(), s = saw.process();
                expectWithinAbsoluteError(r, -s, 1.0e-5f);
                expect(r < prev, "falling");
                prev = r;
                if (i == 0)   expectWithinAbsoluteError(r, 1.0f, 0.01f);
                if (i == 499) expectWithinAbsoluteError(r, 0.0f, 0.01f);
                if (i == 998) expectWithinAbsoluteError(r, -1.0f, 0.01f);
            }
        }

        beginTest("Exponential shapes are unipolar: down decays 1 -> 0 fast then slow, up mirrors it in time");
        {
            auto down = makeLfo(LFO::Shape::ExpDown, 1.0f, 1000.0);
            auto up = makeLfo(LFO::Shape::ExpUp, 1.0f, 1000.0);
            std::vector<float> d, u;
            for (int i = 0; i < 999; ++i) { d.push_back(down.process()); u.push_back(up.process()); }

            expectWithinAbsoluteError(d.front(), 1.0f, 0.01f);
            expectWithinAbsoluteError(d.back(), 0.0f, 0.01f);
            expectWithinAbsoluteError(u.front(), 0.0f, 0.01f);
            expectWithinAbsoluteError(u.back(), 1.0f, 0.01f);
            for (size_t i = 1; i < d.size(); ++i)
            {
                expect(d[i] < d[i - 1] && d[i] >= 0.0f && d[i] <= 1.0f, "down: falling, inside 0..1");
                expect(u[i] > u[i - 1] && u[i] >= 0.0f && u[i] <= 1.0f, "up: rising, inside 0..1");
            }
            // Curved, not a straight line: well below the half-way value at mid cycle.
            expect(d[499] < 0.12f, "decay has mostly happened by mid cycle: " + juce::String(d[499]));
            expect(u[499] < 0.12f, "the rise comes late: " + juce::String(u[499]));
            for (size_t i = 0; i < d.size(); ++i)
                expectWithinAbsoluteError(u[i], d[d.size() - 1 - i], 0.002f);

            expect(LFO::isUnipolar(LFO::Shape::ExpDown) && LFO::isUnipolar(LFO::Shape::ExpUp));
            expect(!LFO::isUnipolar(LFO::Shape::RampDown) && !LFO::isUnipolar(LFO::Shape::Sine));
        }

        beginTest("A negative depth inverts");
        {
            auto a = makeLfo(LFO::Shape::ExpDown, 3.0f);
            auto b = makeLfo(LFO::Shape::ExpDown, 3.0f);
            b.setDepth(-0.5f);
            for (int i = 0; i < 5000; ++i)
                expectWithinAbsoluteError(b.process(), -0.5f * a.process(), 1.0e-6f);
        }

        beginTest("Tempo sync: every division lasts its number of beats, at any sample rate");
        {
            expectEquals(LFO::NUM_DIVISIONS, 17);
            const char* names[] = { "8BAR", "4BAR", "2BAR", "1BAR", "1/2D", "1/2", "1/4D", "1/2T", "1/4",
                                    "1/8D", "1/4T", "1/8", "1/16D", "1/8T", "1/16", "1/16T", "1/32" };
            const double beats[] = { 32, 16, 8, 4, 3, 2, 1.5, 4.0 / 3.0, 1, 0.75, 2.0 / 3.0, 0.5, 0.375, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };
            for (int d = 0; d < LFO::NUM_DIVISIONS; ++d)
            {
                expectEquals(juce::String(LFO::divisionName(d)), juce::String(names[d]));
                expect(juce::String(names[d]).length() <= 5, "fits a cell");
                expectWithinAbsoluteError((double) LFO::divisionBeats(d), beats[d], 1.0e-6);
                if (d > 0)
                    expect(LFO::divisionBeats(d) < LFO::divisionBeats(d - 1), "longest first");

                // Measured: skip the very long ones at the high rate to keep the test quick.
                for (double sr : { 44100.0, 96000.0 })
                {
                    if (beats[d] > 4.0 && sr > 50000.0) continue;
                    const float bpm = 132.0f;
                    auto lfo = makeLfo(LFO::Shape::Saw, 7.3f, sr);     // the Hz value must not matter
                    lfo.setTempo(bpm);
                    lfo.setSyncDivision(d);
                    lfo.setTempoSync(true);
                    const double expected = beats[d] * 60.0 / bpm * sr;
                    const int measured = measureCycleSamples(lfo);
                    expect(std::abs(measured - expected) <= 2.0,
                           juce::String(names[d]) + " @ " + juce::String(sr) + ": " + juce::String(measured)
                               + " samples, expected " + juce::String(expected));
                }
            }
        }

        beginTest("Tempo sync: 1/8 at 120 bpm is a quarter second, 1/8T at 90 bpm two ninths; the tempo is followed");
        {
            auto lfo = makeLfo(LFO::Shape::Saw, 5.0f);
            lfo.setTempoSync(true);
            lfo.setTempo(120.0f);
            lfo.setSyncDivision(11);
            expectEquals(juce::String(LFO::divisionName(11)), juce::String("1/8"));
            expect(std::abs(measureCycleSamples(lfo) - 11025) <= 1);

            lfo.setTempo(60.0f);
            expect(std::abs(measureCycleSamples(lfo) - 22050) <= 1);

            lfo.setTempo(90.0f);
            lfo.setSyncDivision(13);
            expectEquals(juce::String(LFO::divisionName(13)), juce::String("1/8T"));
            expect(std::abs(measureCycleSamples(lfo) - 9800) <= 1);

            // Sync off: the Hz value was kept.
            lfo.setTempoSync(false);
            expectWithinAbsoluteError(lfo.getRate(), 5.0f, 1.0e-6f);
            expect(std::abs(measureCycleSamples(lfo) - 8820) <= 1);
        }

        beginTest("ONE runs exactly one cycle after a trigger and then holds the end value");
        {
            for (double sr : { 44100.0, 48000.0 })
            {
                auto lfo = makeLfo(LFO::Shape::Saw, 2.0f, sr);
                lfo.setTriggerMode(LFO::TriggerMode::Single);
                for (int i = 0; i < 3000; ++i) lfo.process();      // somewhere in a cycle
                lfo.trigger();

                const int cycle = (int) (sr / 2.0);
                float prev = -2.0f;
                int stoppedAt = -1;
                for (int i = 0; i < cycle * 3; ++i)
                {
                    float v = lfo.process();
                    expect(v >= prev, "never wraps: one cycle only");
                    if (i == 0) expectWithinAbsoluteError(v, -1.0f, 0.001f);
                    if (stoppedAt < 0 && lfo.hasStopped()) stoppedAt = i;
                    if (i > cycle + 2) expectEquals(v, 1.0f, "holds the end of the saw");
                    prev = v;
                }
                expect(std::abs(stoppedAt - (cycle - 1)) <= 1, "stops after one cycle: " + juce::String(stoppedAt));

                // The next note runs it again.
                lfo.trigger();
                expect(!lfo.hasStopped());
                expectWithinAbsoluteError(lfo.process(), -1.0f, 0.001f);
            }

            // Each shape ends where its cycle ends.
            struct End { LFO::Shape shape; float value; };
            for (End e : { End { LFO::Shape::Sine, 0.0f }, End { LFO::Shape::Triangle, -1.0f }, End { LFO::Shape::Square, -1.0f },
                           End { LFO::Shape::RampDown, -1.0f }, End { LFO::Shape::ExpUp, 1.0f }, End { LFO::Shape::ExpDown, 0.0f } })
            {
                auto lfo = makeLfo(e.shape, 10.0f);
                lfo.setTriggerMode(LFO::TriggerMode::Single);
                lfo.trigger();
                for (int i = 0; i < 6000; ++i) lfo.process();
                expect(lfo.hasStopped());
                expectWithinAbsoluteError(lfo.process(), e.value, 1.0e-4f);
            }
        }

        beginTest("HALF stops at half a cycle");
        {
            auto saw = makeLfo(LFO::Shape::Saw, 2.0f);
            saw.setTriggerMode(LFO::TriggerMode::Half);
            saw.trigger();
            int stoppedAt = -1;
            for (int i = 0; i < 30000; ++i)
            {
                float v = saw.process();
                if (stoppedAt < 0 && saw.hasStopped()) stoppedAt = i;
                if (i > 11030) expectWithinAbsoluteError(v, 0.0f, 1.0e-5f);
            }
            expect(std::abs(stoppedAt - 11024) <= 1, "stops after half a cycle: " + juce::String(stoppedAt));

            // A triangle's half cycle is a single rise from -1 to +1.
            auto tri = makeLfo(LFO::Shape::Triangle, 2.0f);
            tri.setTriggerMode(LFO::TriggerMode::Half);
            tri.trigger();
            float prev = -2.0f, v = 0.0f;
            for (int i = 0; i < 30000; ++i) { v = tri.process(); expect(v >= prev); prev = v; }
            expectWithinAbsoluteError(v, 1.0f, 1.0e-5f);
        }

        beginTest("ONE without a trigger keeps running; setting the same mode again does not restart a finished one-shot");
        {
            auto lfo = makeLfo(LFO::Shape::Saw, 20.0f);
            lfo.setTriggerMode(LFO::TriggerMode::Single);
            for (int i = 0; i < 44100; ++i) lfo.process();
            expect(!lfo.hasStopped());

            lfo.trigger();
            for (int i = 0; i < 4410; ++i) lfo.process();
            expect(lfo.hasStopped());
            lfo.setTriggerMode(LFO::TriggerMode::Single);     // e.g. a parameter restored after a lock
            expect(lfo.hasStopped());
            lfo.setTriggerMode(LFO::TriggerMode::Free);
            expect(!lfo.hasStopped());
            float a = lfo.process(), b = lfo.process();
            expect(b != a, "running again");
        }

        beginTest("FREE ignores triggers, KEY restarts the cycle");
        {
            auto freeLfo = makeLfo(LFO::Shape::Saw, 1.0f);
            auto twin = makeLfo(LFO::Shape::Saw, 1.0f);
            for (int i = 0; i < 10000; ++i) { freeLfo.process(); twin.process(); }
            freeLfo.trigger();
            for (int i = 0; i < 1000; ++i)
                expectEquals(freeLfo.process(), twin.process());

            auto key = makeLfo(LFO::Shape::Saw, 1.0f);
            key.setTriggerMode(LFO::TriggerMode::KeySync);
            for (int i = 0; i < 10000; ++i) key.process();
            key.trigger();
            expectWithinAbsoluteError(key.process(), -1.0f, 0.001f);
            for (int i = 0; i < 44100 * 2; ++i) key.process();
            expect(!key.hasStopped(), "KEY keeps cycling");
        }

        beginTest("HOLD: the running LFO's value at the trigger is held until the next trigger");
        {
            auto hold = makeLfo(LFO::Shape::Sine, 3.0f);
            auto twin = makeLfo(LFO::Shape::Sine, 3.0f);     // the same LFO, left running
            hold.setTriggerMode(LFO::TriggerMode::Hold);

            float heldValues[3] = {};
            int k = 0;
            for (int gap : { 1234, 5000, 7777 })
            {
                float running = 0.0f;
                for (int i = 0; i < gap; ++i) { hold.process(); running = twin.process(); }
                hold.trigger();
                float first = hold.process();
                twin.process();
                expectWithinAbsoluteError(first, running, 1.0e-6f);       // what the free LFO was showing
                for (int i = 0; i < 20000; ++i)
                {
                    expectEquals(hold.process(), first, "constant while held");
                    twin.process();
                }
                heldValues[k++] = first;
            }
            expect(std::abs(heldValues[0] - heldValues[1]) > 0.05f && std::abs(heldValues[1] - heldValues[2]) > 0.05f,
                   "a different value each note");
        }

        beginTest("Delay: silent for the delay time; fade: then a linear rise over the fade time");
        {
            for (double sr : { 44100.0, 96000.0 })
            {
                auto lfo = makeLfo(LFO::Shape::Square, 0.5f, sr);     // +1 for the first second
                lfo.setTriggerMode(LFO::TriggerMode::KeySync);
                lfo.setDelayTime(0.1f);
                lfo.setFadeTime(0.2f);
                lfo.trigger();

                const int delay = (int) std::lround(0.1 * sr), fade = (int) std::lround(0.2 * sr);
                std::vector<float> out;
                for (int i = 0; i < delay + fade * 2; ++i) out.push_back(lfo.process());

                for (int i = 0; i < delay - 1; ++i)
                    expectEquals(out[(size_t) i], 0.0f);
                expectWithinAbsoluteError(out[(size_t) (delay + fade / 4)], 0.25f, 0.01f);
                expectWithinAbsoluteError(out[(size_t) (delay + fade / 2)], 0.5f, 0.01f);
                expectWithinAbsoluteError(out[(size_t) (delay + fade * 3 / 4)], 0.75f, 0.01f);
                for (int i = delay + fade + 2; i < delay + fade * 2; ++i)
                    expectEquals(out[(size_t) i], 1.0f);
                for (int i = delay + 1; i < delay + fade; ++i)
                    expect(out[(size_t) i] >= out[(size_t) (i - 1)], "rising");
            }

            // No delay, no fade: full level from the first sample.
            auto plain = makeLfo(LFO::Shape::Square, 0.5f);
            plain.setTriggerMode(LFO::TriggerMode::KeySync);
            plain.trigger();
            expectEquals(plain.process(), 1.0f);

            // The fade applies in FREE mode too, and again on every note.
            auto freeLfo = makeLfo(LFO::Shape::Square, 0.5f);
            freeLfo.setFadeTime(0.1f);
            for (int n = 0; n < 2; ++n)
            {
                freeLfo.trigger();
                expect(std::abs(freeLfo.process()) < 0.01f);
                for (int i = 0; i < 2205; ++i) freeLfo.process();
                expectWithinAbsoluteError(std::abs(freeLfo.process()), 0.5f, 0.01f);
                for (int i = 0; i < 4410; ++i) freeLfo.process();
                expectEquals(std::abs(freeLfo.process()), 1.0f);
            }
        }

        beginTest("A delayed one-shot plays its whole cycle after the delay");
        {
            auto lfo = makeLfo(LFO::Shape::Saw, 4.0f);
            lfo.setTriggerMode(LFO::TriggerMode::Single);
            lfo.setDelayTime(0.5f);
            lfo.trigger();
            int stoppedAt = -1;
            float firstAfterDelay = 0.0f;
            for (int i = 0; i < 44100; ++i)
            {
                float v = lfo.process();
                if (i == 22050) firstAfterDelay = v;
                if (stoppedAt < 0 && lfo.hasStopped()) stoppedAt = i;
            }
            expectWithinAbsoluteError(firstAfterDelay, -1.0f, 0.001f);
            expect(std::abs(stoppedAt - (22050 + 11025 - 1)) <= 1, "stopped at " + juce::String(stoppedAt));
        }

        beginTest("Start phase: the cycle begins there");
        {
            auto sine = makeLfo(LFO::Shape::Sine, 1.0f);
            sine.setTriggerMode(LFO::TriggerMode::KeySync);
            sine.setStartPhase(0.25f);
            sine.trigger();
            expectWithinAbsoluteError(sine.process(), 1.0f, 0.001f);      // the top of the sine

            sine.setStartPhase(0.5f);
            sine.trigger();
            float a = sine.process(), b = sine.process();
            expect(std::abs(a) < 0.001f && b < a, "half way: crossing zero downwards");

            // A one-shot from a start phase still runs one whole cycle and ends where it began.
            auto saw = makeLfo(LFO::Shape::Saw, 2.0f);
            saw.setTriggerMode(LFO::TriggerMode::Single);
            saw.setStartPhase(0.25f);
            saw.trigger();
            expectWithinAbsoluteError(saw.process(), -0.5f, 0.001f);
            int stoppedAt = -1;
            float v = 0.0f;
            for (int i = 1; i < 40000; ++i)
            {
                v = saw.process();
                if (stoppedAt < 0 && saw.hasStopped()) stoppedAt = i;
            }
            expect(std::abs(stoppedAt - 22049) <= 1);
            expectWithinAbsoluteError(v, -0.5f, 0.001f);

            // Two free LFOs a quarter cycle apart stay a quarter cycle apart.
            auto i0 = makeLfo(LFO::Shape::Sine, 5.0f), q = makeLfo(LFO::Shape::Sine, 5.0f);
            q.setStartPhase(0.25f);
            for (int i = 0; i < 20000; ++i)
            {
                float s = i0.process(), c = q.process();
                expectWithinAbsoluteError(s * s + c * c, 1.0f, 0.001f);
            }
        }

        beginTest("Rate and depth modulation inputs");
        {
            auto lfo = makeLfo(LFO::Shape::Saw, 4.0f);
            lfo.setRateMod(1.0f);                       // one octave up
            expect(std::abs(measureCycleSamples(lfo) - 5512) <= 1);
            lfo.setRateMod(-2.0f);
            expect(std::abs(measureCycleSamples(lfo) - 44100) <= 1);
            lfo.setRateMod(0.0f);
            expect(std::abs(measureCycleSamples(lfo) - 11025) <= 1);

            auto a = makeLfo(LFO::Shape::Sine, 4.0f), b = makeLfo(LFO::Shape::Sine, 4.0f);
            b.setDepthMod(-0.5f);
            for (int i = 0; i < 3000; ++i)
                expectWithinAbsoluteError(b.process(), 0.5f * a.process(), 1.0e-6f);
        }

        beginTest("An idle LFO advanced by blocks is where a processed one is");
        {
            auto a = makeLfo(LFO::Shape::Saw, 3.7f), b = makeLfo(LFO::Shape::Saw, 3.7f);
            for (int i = 0; i < 64 * 500; ++i) a.process();
            for (int i = 0; i < 500; ++i) b.advanceIdle(64);
            expectWithinAbsoluteError(b.getPhase(), a.getPhase(), 1.0e-6);
        }

        // ---- in the unified voice ----------------------------------------------------------------

        beginTest("Ids: the new LFO parameters are below 150, collide with nothing and read back");
        {
            std::set<int> used;
            for (auto& page : UnifiedVoiceEngine::basePages())
                for (auto& p : page.params)
                    if (p.isValid()) used.insert(p.id);
            for (int id = ModParam::LFO_BASE; id <= ModParam::ENV_COUNT; ++id)
                expect(used.insert(id).second, "old modulation id " + juce::String(id) + " is free of engine parameters");

            UnifiedVoiceEngine e(2);
            float value = 0.0f;
            for (int l = 0; l < ModParam::MAX_LFOS; ++l)
                for (int off : { ModParam::LFO_DIV, ModParam::LFO_PHASE, ModParam::LFO_DELAY, ModParam::LFO_FADE })
                {
                    const int id = ModParam::lfoExtParamId(l, off);
                    expect(id >= 130 && id < 150 && id < UnifiedParam::MAX_ID);
                    expect(used.insert(id).second, "id " + juce::String(id) + " is used once");
                    expect(ModParam::isModParam(id));
                    // Defaults: quarter-note division, nothing else.
                    expectEquals(e.getParameter(id), off == ModParam::LFO_DIV ? (float) LFO::DEFAULT_DIVISION : 0.0f);
                    value += 0.01f;
                    const float v = off == ModParam::LFO_DIV ? (float) (l + 2) : value;
                    e.setParameter(id, v);
                    expectEquals(e.getParameter(id), v);
                }
            expectEquals(ModParam::lfoExtParamId(0, ModParam::LFO_DIV), 130);
            expectEquals(ModParam::lfoExtParamId(3, ModParam::LFO_FADE), 145);

            auto& lfo = e.getVoice(1).getLFOBank().getLFO(2);
            expectEquals(lfo.getSyncDivision(), 4);
            expectWithinAbsoluteError(lfo.getStartPhase(), e.getParameter(ModParam::lfoExtParamId(2, ModParam::LFO_PHASE)), 1.0e-6f);
            expectWithinAbsoluteError(lfo.getDelayTime(), e.getParameter(ModParam::lfoExtParamId(2, ModParam::LFO_DELAY)), 1.0e-6f);
            expectWithinAbsoluteError(lfo.getFadeTime(), e.getParameter(ModParam::lfoExtParamId(2, ModParam::LFO_FADE)), 1.0e-6f);
        }

        beginTest("Voice: a synced square on the level gates the sound in time with the tempo");
        {
            for (double sr : { 44100.0, 48000.0 })
                for (float bpm : { 120.0f, 60.0f })
                {
                    Synth s(sr);
                    s.engine.setTempo(bpm);
                    s.lfo(ModParam::LFO_RATE, 13.0f);             // must not matter
                    s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square);
                    s.lfo(ModParam::LFO_TRIG, (float) (int) LFO::TriggerMode::KeySync);
                    s.lfo(ModParam::LFO_DEPTH, 1.0f);
                    s.lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Amp);
                    s.ext(ModParam::LFO_DIV, 11.0f);              // 1/8: on for a sixteenth, off for a sixteenth
                    s.lfo(ModParam::LFO_SYNC, 1.0f);
                    s.engine.noteOn(57, 1.0f, -1);
                    std::vector<float> out;
                    s.render(2.0, out);

                    const double half = 0.25 * 60.0 / bpm;         // a sixteenth, in seconds
                    for (int n = 0; n < 6; ++n)
                    {
                        const float level = rms(out, sr, n * half + 0.01, (n + 1) * half - 0.01);
                        if (n % 2 == 0) expect(level > 0.05f, "on at " + juce::String(n) + ": " + juce::String(level));
                        else            expect(level < 0.001f, "off at " + juce::String(n) + ": " + juce::String(level));
                    }
                }
        }

        beginTest("Voice: SYNC off uses the Hz value again");
        {
            Synth s;
            s.lfo(ModParam::LFO_RATE, 5.0f);
            s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square);
            s.lfo(ModParam::LFO_TRIG, 1.0f);
            s.lfo(ModParam::LFO_DEPTH, 1.0f);
            s.lfo(ModParam::LFO_DEST, 5.0f);
            s.lfo(ModParam::LFO_SYNC, 1.0f);
            s.lfo(ModParam::LFO_SYNC, 0.0f);
            s.engine.noteOn(57, 1.0f, -1);
            std::vector<float> out;
            s.render(0.5, out);
            expect(rms(out, s.sr, 0.01, 0.09) > 0.05f);
            expect(rms(out, s.sr, 0.11, 0.19) < 0.001f);
            expect(rms(out, s.sr, 0.21, 0.29) > 0.05f);
        }

        beginTest("Voice: ONE with a decay shape is a one-off sweep that ends, every note");
        {
            Synth s;
            s.lfo(ModParam::LFO_RATE, 4.0f);                      // a quarter second
            s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::ExpDown);
            s.lfo(ModParam::LFO_TRIG, (float) (int) LFO::TriggerMode::Single);
            s.lfo(ModParam::LFO_DEPTH, 1.0f);
            s.lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Amp);
            for (int note = 0; note < 2; ++note)
            {
                s.engine.noteOn(57, 1.0f, -1);
                std::vector<float> out;
                s.render(1.5, out);
                const float start = rms(out, s.sr, 0.005, 0.03);
                const float mid = rms(out, s.sr, 0.10, 0.14);
                const float after1 = rms(out, s.sr, 0.30, 0.50);
                const float after2 = rms(out, s.sr, 0.60, 0.80);
                const float after3 = rms(out, s.sr, 1.20, 1.40);
                expect(start > after1 * 1.5f, "boosted at the start: " + juce::String(start) + " vs " + juce::String(after1));
                expect(mid > after1 * 1.02f && mid < start, "decaying");
                expectWithinAbsoluteError(after2 / after1, 1.0f, 0.01f);      // it has stopped: no second cycle
                expectWithinAbsoluteError(after3 / after1, 1.0f, 0.01f);
                expect(s.engine.getVoice(0).getLFOBank().getLFO(0).hasStopped());
                s.engine.noteOff(57, -1);
                s.render(0.5, out);
            }

            // The same patch in KEY mode keeps cycling.
            Synth k;
            k.lfo(ModParam::LFO_RATE, 4.0f);
            k.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::ExpDown);
            k.lfo(ModParam::LFO_TRIG, (float) (int) LFO::TriggerMode::KeySync);
            k.lfo(ModParam::LFO_DEPTH, 1.0f);
            k.lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Amp);
            k.engine.noteOn(57, 1.0f, -1);
            std::vector<float> out;
            k.render(1.0, out);
            expect(rms(out, k.sr, 0.505, 0.53) > rms(out, k.sr, 0.40, 0.49) * 1.5f, "a new cycle at half a second");
        }

        beginTest("Voice: delay keeps the LFO away from the start of the note, at any sample rate");
        {
            for (double sr : { 44100.0, 96000.0 })
            {
                Synth s(sr);
                s.lfo(ModParam::LFO_RATE, 1.0f);
                s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square);
                s.lfo(ModParam::LFO_TRIG, (float) (int) LFO::TriggerMode::KeySync);
                s.lfo(ModParam::LFO_DEPTH, -1.0f);                // square high = silence
                s.lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Amp);
                s.ext(ModParam::LFO_DELAY, 0.2f);
                s.engine.noteOn(57, 1.0f, -1);
                std::vector<float> out;
                s.render(0.6, out);
                expect(rms(out, sr, 0.02, 0.19) > 0.05f, "untouched during the delay");
                expect(rms(out, sr, 0.21, 0.55) < 0.001f, "silenced once the LFO starts");
            }
        }

        beginTest("Voice: fade-in brings the modulation in gradually");
        {
            Synth s;
            s.lfo(ModParam::LFO_RATE, 0.5f);
            s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square);
            s.lfo(ModParam::LFO_TRIG, (float) (int) LFO::TriggerMode::KeySync);
            s.lfo(ModParam::LFO_DEPTH, -1.0f);
            s.lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Amp);
            s.ext(ModParam::LFO_FADE, 0.4f);
            s.engine.noteOn(57, 1.0f, -1);
            std::vector<float> out;
            s.render(0.8, out);
            const float full = rms(out, s.sr, 0.005, 0.015);
            const float a = rms(out, s.sr, 0.09, 0.11), b = rms(out, s.sr, 0.19, 0.21), c = rms(out, s.sr, 0.29, 0.31);
            expectWithinAbsoluteError(a / full, 0.75f, 0.06f);
            expectWithinAbsoluteError(b / full, 0.50f, 0.06f);
            expectWithinAbsoluteError(c / full, 0.25f, 0.06f);
            expect(rms(out, s.sr, 0.45, 0.75) < 0.001f);
        }

        beginTest("Voice: HOLD gives each note one steady value; FREE keeps running between notes");
        {
            Synth s;
            s.lfo(ModParam::LFO_RATE, 3.0f);
            s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Triangle);
            s.lfo(ModParam::LFO_TRIG, (float) (int) LFO::TriggerMode::Hold);
            s.lfo(ModParam::LFO_DEPTH, 0.8f);
            s.lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Amp);
            s.engine.setParameter(UnifiedParam::AMP_RELEASE, 0.005f);

            std::vector<float> levels;
            std::vector<float> scratch;
            for (double gap : { 0.07, 0.13, 0.21, 0.05 })
            {
                s.render(gap, scratch);                           // silence: the LFO must keep moving
                s.engine.noteOn(57, 1.0f, -1);
                std::vector<float> out;
                s.render(0.6, out);
                const float l1 = rms(out, s.sr, 0.05, 0.15), l2 = rms(out, s.sr, 0.25, 0.35), l3 = rms(out, s.sr, 0.45, 0.55);
                expectWithinAbsoluteError(l2 / l1, 1.0f, 0.01f);
                expectWithinAbsoluteError(l3 / l1, 1.0f, 0.01f);
                levels.push_back(l1);
                s.engine.noteOff(57, -1);
                s.render(0.1, scratch);
                expect(!s.engine.getVoice(0).isActive(), "the voice has gone silent between notes");
            }
            int different = 0;
            for (size_t i = 1; i < levels.size(); ++i)
                if (std::abs(levels[i] / levels[i - 1] - 1.0f) > 0.03f) ++different;
            expect(different >= 2, "notes at different moments get different values");

            // The phase of a free LFO moves while its voice is silent, as if it had been running.
            Synth f;
            f.lfo(ModParam::LFO_RATE, 1.0f);
            std::vector<float> none;
            f.render(0.25, none);
            expectWithinAbsoluteError(f.engine.getVoice(0).getLFOBank().getLFO(0).getPhase(), 0.25, 0.005);
        }

        beginTest("Voice: every destination changes the sound");
        {
            using D = UnifiedVoice::LfoDest;
            auto render = [](D dest, std::vector<float>& left, std::vector<float>& right)
            {
                Synth s;
                auto& e = s.engine;
                e.setParameter(UnifiedParam::WAVE1, 4.0f);            // pulse, so the width matters
                e.setParameter(UnifiedParam::MIX, 0.4f);
                e.setParameter(UnifiedParam::DETUNE2, 0.0f);
                e.setParameter(UnifiedParam::TUNE2, 7.0f);
                e.setParameter(UnifiedParam::SUB, 0.5f);
                e.setParameter(UnifiedParam::NOISE, 0.3f);
                e.setParameter(UnifiedParam::CUTOFF, 1200.0f);
                e.setParameter(UnifiedParam::RESONANCE, 0.3f);
                e.setParameter(UnifiedParam::MODEL_B, 0.0f);          // a second filter, after the first
                e.setParameter(UnifiedParam::ROUTING, 1.0f);
                e.setParameter(UnifiedParam::CROSS_MOD, 0.2f);
                e.setParameter(UnifiedParam::FILTER_FM, 0.2f);
                s.lfo(ModParam::LFO_RATE, 6.0f);
                s.lfo(ModParam::LFO_TRIG, 1.0f);
                s.lfo(ModParam::LFO_DEPTH, 0.8f);
                s.lfo(ModParam::LFO_DEST, (float) (int) dest);
                e.noteOn(50, 1.0f, -1);
                s.render(0.5, left, &right);
            };

            std::vector<float> dryL, dryR;
            render(D::Off, dryL, dryR);
            expect(rms(dryL, 44100.0, 0.0, 0.5) > 0.01f);
            for (size_t i = 0; i < dryL.size(); ++i)
                if (dryL[i] != dryR[i]) { expect(false, "without a PAN routing both channels are the same"); break; }

            for (int d = 1; d < (int) D::Lfo1Rate; ++d)
            {
                std::vector<float> l, r;
                render(static_cast<D>(d), l, r);
                double diff = 0.0;
                for (size_t i = 0; i < l.size(); ++i)
                    diff += std::abs(l[i] - dryL[i]) + std::abs(r[i] - dryR[i]);
                expect(diff / (double) l.size() > 0.001,
                       juce::String(UnifiedVoice::lfoDestLabels()[d]) + " changes the output: " + juce::String(diff / (double) l.size()));
                float peak = 0.0f;
                bool finite = true;
                for (float v : l) { finite = finite && std::isfinite(v); peak = juce::jmax(peak, std::abs(v)); }
                // (Two resonant filters in series ring loudly when RESO is pushed; a blow-up is far beyond this.)
                expect(finite && peak < 20.0f,
                       juce::String(UnifiedVoice::lfoDestLabels()[d]) + " output stays sane, peak " + juce::String(peak));
            }
        }

        beginTest("Voice: PAN moves the voice between the channels and leaves the centre alone");
        {
            Synth s;
            s.lfo(ModParam::LFO_RATE, 2.0f);
            s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square);
            s.lfo(ModParam::LFO_TRIG, 1.0f);
            s.lfo(ModParam::LFO_DEPTH, 1.0f);
            s.lfo(ModParam::LFO_DEST, (float) (int) UnifiedVoice::LfoDest::Pan);
            s.engine.noteOn(57, 1.0f, -1);
            std::vector<float> l, r;
            s.render(0.5, l, &r);
            // First quarter second: hard right; second: hard left.
            expect(rms(l, s.sr, 0.02, 0.24) < 0.001f && rms(r, s.sr, 0.02, 0.24) > 0.05f);
            expect(rms(r, s.sr, 0.26, 0.49) < 0.001f && rms(l, s.sr, 0.26, 0.49) > 0.05f);

            Synth c;
            c.engine.noteOn(57, 1.0f, -1);
            std::vector<float> cl, cr;
            c.render(0.5, cl, &cr);
            expectWithinAbsoluteError(rms(r, s.sr, 0.02, 0.24) / rms(cr, c.sr, 0.02, 0.24), 1.0f, 0.01f);
        }

        beginTest("Voice: one LFO can drive another LFO's rate and depth");
        {
            using D = UnifiedVoice::LfoDest;
            auto cyclesOfLfo1 = [](D lfo2Dest, float lfo2Depth)
            {
                Synth s;
                s.engine.setParameter(ModParam::LFO_COUNT, 2.0f);
                s.lfo(ModParam::LFO_RATE, 4.0f, 0);
                s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Saw, 0);
                s.lfo(ModParam::LFO_TRIG, 1.0f, 0);
                s.lfo(ModParam::LFO_RATE, 0.25f, 1);                  // high for the first two seconds
                s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square, 1);
                s.lfo(ModParam::LFO_TRIG, 1.0f, 1);
                s.lfo(ModParam::LFO_DEPTH, lfo2Depth, 1);
                s.lfo(ModParam::LFO_DEST, (float) (int) lfo2Dest, 1);
                s.engine.noteOn(57, 1.0f, -1);
                int wraps = 0;
                double prev = 0.0;
                std::vector<float> out;
                for (int b = 0; b < 1380; ++b)                        // just over one second in blocks of 32
                {
                    out.clear();
                    s.render(32.0 / 44100.0, out);
                    double p = s.engine.getVoice(0).getLFOBank().getLFO(0).getPhase();
                    if (p < prev) ++wraps;
                    prev = p;
                }
                return wraps;
            };
            expectEquals(cyclesOfLfo1(D::Off, 1.0f), 4);
            expectEquals(cyclesOfLfo1(D::Lfo1Rate, 1.0f), 16);        // two octaves up
            expectEquals(cyclesOfLfo1(D::Lfo1Rate, 0.5f), 8);
            expectEquals(cyclesOfLfo1(D::Lfo1Rate, -0.5f), 2);
            expectEquals(cyclesOfLfo1(D::Lfo2Rate, 1.0f), 4, "aimed at itself, LFO 1 is untouched");

            // Depth: LFO 2 (square, low half first with a negative depth) closes LFO 1's tremolo.
            auto tremoloSwing = [](float lfo2Depth)
            {
                Synth s;
                s.engine.setParameter(ModParam::LFO_COUNT, 2.0f);
                s.lfo(ModParam::LFO_RATE, 10.0f, 0);
                s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square, 0);
                s.lfo(ModParam::LFO_TRIG, 1.0f, 0);
                s.lfo(ModParam::LFO_DEPTH, 0.5f, 0);
                s.lfo(ModParam::LFO_DEST, (float) (int) D::Amp, 0);
                s.lfo(ModParam::LFO_RATE, 0.25f, 1);
                s.lfo(ModParam::LFO_SHAPE, (float) (int) LFO::Shape::Square, 1);
                s.lfo(ModParam::LFO_TRIG, 1.0f, 1);
                s.lfo(ModParam::LFO_DEPTH, lfo2Depth, 1);
                s.lfo(ModParam::LFO_DEST, (float) (int) D::Lfo1Depth, 1);
                s.engine.noteOn(57, 1.0f, -1);
                std::vector<float> out;
                s.render(0.5, out);
                return rms(out, s.sr, 0.21, 0.24) / rms(out, s.sr, 0.26, 0.29);    // loud half / quiet half
            };
            expectWithinAbsoluteError(tremoloSwing(0.0f), 3.0f, 0.1f);     // 1.5 / 0.5
            expectWithinAbsoluteError(tremoloSwing(-1.0f), 1.0f, 0.02f);   // depth pulled to zero
            expect(tremoloSwing(1.0f) > 50.0f, "depth doubled: the quiet half is silent");
        }

        beginTest("Voice: the cross-mod and filter-FM destinations work with oscillator 2 out of the mix");
        {
            using D = UnifiedVoice::LfoDest;
            for (D dest : { D::CrossMod, D::FilterFM })
            {
                auto render = [dest](bool routed)
                {
                    Synth s;
                    s.engine.setParameter(UnifiedParam::CUTOFF, 1500.0f);
                    s.lfo(ModParam::LFO_RATE, 3.0f);
                    s.lfo(ModParam::LFO_TRIG, 1.0f);
                    s.lfo(ModParam::LFO_DEPTH, 1.0f);
                    s.lfo(ModParam::LFO_DEST, routed ? (float) (int) dest : 0.0f);
                    s.engine.noteOn(50, 1.0f, -1);
                    std::vector<float> out;
                    s.render(0.4, out);
                    return out;
                };
                auto dry = render(false), wet = render(true);
                double diff = 0.0;
                for (size_t i = 0; i < dry.size(); ++i) diff += std::abs(dry[i] - wet[i]);
                expect(diff / (double) dry.size() > 0.001, "audible with MIX at oscillator 1 only");
            }
        }

        // ---- performance controls: wheel, velocity, key, pressure --------------------------------

        // Depth of a square tremolo at 5 Hz, read from the audio: (loud - quiet) / (loud + quiet)
        // with the sign of "first half louder".
        auto measuredDepth = [](Synth& s, int note = 57, float velocity = 1.0f)
        {
            // The engine smooths the mod strip (about 20 ms): let a new position arrive first.
            std::vector<float> settle;
            s.render(0.2, settle);
            s.engine.noteOn(note, velocity, -1);
            std::vector<float> out;
            s.render(0.2, out);
            s.engine.noteOff(note, -1);
            const float first = rms(out, s.sr, 0.02, 0.09), second = rms(out, s.sr, 0.12, 0.19);
            return (first - second) / (first + second);
        };

        beginTest("Ids: the performance controls are 174..189, default to zero and read back");
        {
            expectEquals(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 174);
            expectEquals(ModParam::lfoExt2ParamId(0, ModParam::LFO_VEL), 175);
            expectEquals(ModParam::lfoExt2ParamId(0, ModParam::LFO_KTRK), 176);
            expectEquals(ModParam::lfoExt2ParamId(0, ModParam::LFO_PRES), 177);
            expectEquals(ModParam::lfoExt2ParamId(3, ModParam::LFO_PRES), 189);
            UnifiedVoiceEngine e(2);
            float v = -0.9f;
            for (int id = ModParam::LFO_EXT2_BASE; id < ModParam::LFO_EXT2_END; ++id)
            {
                expect(EngineParamRange::contains(id) && id < UnifiedParam::MAX_ID && ModParam::isModParam(id));
                expectEquals(e.getParameter(id), 0.0f);
                e.setParameter(id, v);
                expectWithinAbsoluteError(e.getParameter(id), v, 1.0e-6f);
                v += 0.11f;
            }
            for (int id = 158; id < 174; ++id)
                expect(!ModParam::isModParam(id), "158..173 belong to someone else");
        }

        beginTest("WHL: the mod strip adds to the depth (DPTH 0 + WHL = vibrato on the wheel); zero changes nothing");
        {
            Synth s;
            s.squareTremolo(5.0f, 0.0f);
            s.engine.modWheel(1.0f);
            expectWithinAbsoluteError(measuredDepth(s), 0.0f, 0.01f);      // WHL 0: the wheel does nothing here

            s.ext2(ModParam::LFO_WHEEL, 0.5f);
            s.engine.modWheel(0.0f);
            expectWithinAbsoluteError(measuredDepth(s), 0.0f, 0.01f);
            s.engine.modWheel(0.5f);
            expectWithinAbsoluteError(measuredDepth(s), 0.25f, 0.02f);
            s.engine.modWheel(1.0f);
            expectWithinAbsoluteError(measuredDepth(s), 0.5f, 0.02f);

            // Negative: the wheel takes the modulation away (and past zero, inverts it).
            s.lfo(ModParam::LFO_DEPTH, 0.5f);
            s.ext2(ModParam::LFO_WHEEL, -0.5f);
            expectWithinAbsoluteError(measuredDepth(s), 0.0f, 0.02f);
            s.ext2(ModParam::LFO_WHEEL, -1.0f);
            expectWithinAbsoluteError(measuredDepth(s), -0.5f, 0.02f);

            // Clamped to the depth range.
            s.lfo(ModParam::LFO_DEPTH, 0.8f);
            s.ext2(ModParam::LFO_WHEEL, 1.0f);
            expectWithinAbsoluteError(measuredDepth(s), 1.0f, 0.02f);
            expectWithinAbsoluteError(s.engine.getVoice(0).getLFOBank().getLFO(0).getEffectiveDepth(), 1.0f, 1.0e-6f);
        }

        beginTest("VEL: harder notes get more (or, negative, less) LFO; taken at note-on");
        {
            Synth s;
            s.squareTremolo(5.0f, 0.0f);
            expectWithinAbsoluteError(measuredDepth(s, 57, 1.0f), 0.0f, 0.01f);
            s.ext2(ModParam::LFO_VEL, 0.8f);
            const float soft = measuredDepth(s, 57, 0.25f), hard = measuredDepth(s, 57, 1.0f);
            expectWithinAbsoluteError(soft, 0.2f, 0.02f);
            expectWithinAbsoluteError(hard, 0.8f, 0.02f);

            s.lfo(ModParam::LFO_DEPTH, 0.6f);
            s.ext2(ModParam::LFO_VEL, -0.6f);
            expectWithinAbsoluteError(measuredDepth(s, 57, 1.0f), 0.0f, 0.02f);
            expectWithinAbsoluteError(measuredDepth(s, 57, 0.5f), 0.3f, 0.02f);

            // Per voice: two notes at different velocities, each voice's LFO has its own depth.
            Synth p(44100.0, 4);
            p.squareTremolo(1.0f, 0.0f);
            p.ext2(ModParam::LFO_VEL, 1.0f);
            p.engine.noteOn(50, 0.25f, -1);
            p.engine.noteOn(62, 0.75f, -1);
            std::vector<float> out;
            p.render(0.1, out);
            std::vector<float> depths;
            for (int v = 0; v < 4; ++v)
                if (p.engine.getVoice(v).isActive())
                    depths.push_back(p.engine.getVoice(v).getLFOBank().getLFO(0).getCurrentValue());
            expectEquals((int) depths.size(), 2);
            if (depths.size() == 2)
            {
                std::sort(depths.begin(), depths.end());
                expectWithinAbsoluteError(depths[0], 0.25f, 0.01f);
                expectWithinAbsoluteError(depths[1], 0.75f, 0.01f);
            }
        }

        beginTest("PRES: a voice's own pressure adds to its LFO depth, not to the other voices'");
        {
            // No AT ROUTE routings, so pressure reaches the sound only through the LFO.
            AftertouchRouter noRoutes;
            noRoutes.setRouteCount(0);

            Synth s;
            s.engine.setAftertouchRouter(&noRoutes);
            s.squareTremolo(5.0f, 0.0f);
            s.engine.noteOn(57, 1.0f, -1);
            s.engine.aftertouch(57, 1.0f);
            std::vector<float> out;
            s.render(0.2, out);
            expectWithinAbsoluteError(rms(out, s.sr, 0.02, 0.09) / rms(out, s.sr, 0.12, 0.19), 1.0f, 0.02f);   // PRES 0

            s.ext2(ModParam::LFO_PRES, 0.5f);
            auto depthAt = [&](float pressure)
            {
                s.engine.noteOff(57, -1);
                s.engine.noteOn(57, 1.0f, -1);
                s.engine.aftertouch(57, pressure);
                std::vector<float> o;
                s.render(0.2, o);
                const float first = rms(o, s.sr, 0.02, 0.09), second = rms(o, s.sr, 0.12, 0.19);
                return (first - second) / (first + second);
            };
            expectWithinAbsoluteError(depthAt(0.0f), 0.0f, 0.02f);
            expectWithinAbsoluteError(depthAt(0.5f), 0.25f, 0.03f);
            expectWithinAbsoluteError(depthAt(1.0f), 0.5f, 0.03f);
            s.lfo(ModParam::LFO_DEPTH, 0.5f);
            s.ext2(ModParam::LFO_PRES, -0.5f);
            expectWithinAbsoluteError(depthAt(1.0f), 0.0f, 0.03f);

            // Polyphonic: only the pressed note's voice.
            Synth p(44100.0, 4);
            p.engine.setAftertouchRouter(&noRoutes);
            p.squareTremolo(1.0f, 0.0f);
            p.ext2(ModParam::LFO_PRES, 1.0f);
            p.engine.noteOn(50, 1.0f, -1);
            p.engine.noteOn(62, 1.0f, -1);
            p.engine.aftertouch(62, 0.8f);
            std::vector<float> o;
            p.render(0.1, o);
            int pressed = 0, unpressed = 0;
            for (int v = 0; v < 4; ++v)
            {
                if (!p.engine.getVoice(v).isActive()) continue;
                const float d = p.engine.getVoice(v).getLFOBank().getLFO(0).getCurrentValue();
                if (std::abs(d - 0.8f) < 0.01f) ++pressed;
                if (std::abs(d) < 0.001f) ++unpressed;
            }
            expectEquals(pressed, 1);
            expectEquals(unpressed, 1);
        }

        beginTest("KTRK: the rate follows the key (x2 per octave at 1), not while SYNC is on");
        {
            // Half cycles of the square in the audio: count level changes in one second.
            auto halfCycles = [](float ktrk, int note, bool sync)
            {
                Synth s;
                s.squareTremolo(4.0f, 1.0f);
                s.ext2(ModParam::LFO_KTRK, ktrk);
                s.engine.setTempo(120.0f);
                s.ext(ModParam::LFO_DIV, 8.0f);               // 1/4 at 120 = 2 Hz
                s.lfo(ModParam::LFO_SYNC, sync ? 1.0f : 0.0f);
                s.engine.noteOn(note, 1.0f, -1);
                std::vector<float> out;
                s.render(1.0, out);
                int changes = 0;
                bool on = true;
                const double win = 0.005;
                for (double t = 0.0; t + win <= 1.0; t += win)
                {
                    const bool now = rms(out, s.sr, t, t + win) > 0.01f;
                    if (now != on) { ++changes; on = now; }
                }
                return changes;
            };
            expectEquals(halfCycles(0.0f, 72, false), 7);         // 4 Hz whatever the key
            expectEquals(halfCycles(0.0f, 36, false), 7);
            expectEquals(halfCycles(1.0f, 60, false), 7);         // middle C: unchanged
            expectEquals(halfCycles(1.0f, 72, false), 15);        // an octave up: 8 Hz
            expectEquals(halfCycles(1.0f, 48, false), 3);         // an octave down: 2 Hz
            expectEquals(halfCycles(0.5f, 84, false), 15);        // half tracking, two octaves up
            expectEquals(halfCycles(-1.0f, 72, false), 3);        // negative: slower going up
            expectEquals(halfCycles(1.0f, 72, true), 3);          // synced: the division, 2 Hz
            expectEquals(halfCycles(1.0f, 36, true), 3);
        }

        beginTest("Project: the new LFO settings come back after save and load");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            auto& track = seq.getPattern().getTrack(0);
            track.engineType = EngineType::UnifiedVoice;

            struct Setting { int id; float value; };
            const Setting settings[] = {
                { ModParam::lfoParamId(0, ModParam::LFO_SHAPE), (float) (int) LFO::Shape::ExpDown },
                { ModParam::lfoParamId(0, ModParam::LFO_TRIG), (float) (int) LFO::TriggerMode::Half },
                { ModParam::lfoParamId(0, ModParam::LFO_SYNC), 1.0f },
                { ModParam::lfoParamId(0, ModParam::LFO_DEST), (float) (int) UnifiedVoice::LfoDest::Pan },
                { ModParam::lfoParamId(0, ModParam::LFO_DEPTH), -0.6f },
                { ModParam::lfoExtParamId(0, ModParam::LFO_DIV), 13.0f },
                { ModParam::lfoExtParamId(0, ModParam::LFO_PHASE), 0.25f },
                { ModParam::lfoExtParamId(0, ModParam::LFO_DELAY), 0.3f },
                { ModParam::lfoExtParamId(0, ModParam::LFO_FADE), 1.25f },
                { ModParam::lfoExtParamId(3, ModParam::LFO_FADE), 0.75f },
                { ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f },
                { ModParam::lfoExt2ParamId(0, ModParam::LFO_VEL), -0.25f },
                { ModParam::lfoExt2ParamId(0, ModParam::LFO_KTRK), 0.8f },
                { ModParam::lfoExt2ParamId(0, ModParam::LFO_PRES), -0.4f },
                { ModParam::lfoExt2ParamId(3, ModParam::LFO_PRES), 1.0f } };
            for (auto& s : settings)       // as the parameter screen does it
            {
                track.soundParams[s.id] = s.value;
                audio.setTrackParameter(0, s.id, s.value);
            }

            auto data = ProjectData::fromJSON(ProjectBridge::capture({ seq, audio }, "L").toJSON());

            SequencerCore seq2;
            AudioEngine audio2;
            ProjectBridge::apply(data, { seq2, audio2 });
            auto* engine = dynamic_cast<UnifiedVoiceEngine*>(audio2.getEngine(0));
            expect(engine != nullptr);
            if (engine != nullptr)
            {
                for (auto& s : settings)
                    expectWithinAbsoluteError(engine->getParameter(s.id), s.value, 1.0e-4f);
                auto& lfo = engine->getVoice(0).getLFOBank().getLFO(0);
                expect(lfo.getShape() == LFO::Shape::ExpDown && lfo.getTriggerMode() == LFO::TriggerMode::Half);
                expect(lfo.isTempoSynced() && lfo.getSyncDivision() == 13);
                expectWithinAbsoluteError(lfo.getStartPhase(), 0.25f, 1.0e-4f);
                expectWithinAbsoluteError(lfo.getDelayTime(), 0.3f, 1.0e-4f);
                expectWithinAbsoluteError(lfo.getFadeTime(), 1.25f, 1.0e-4f);
                expect(engine->getVoice(0).getLfoDest(0) == UnifiedVoice::LfoDest::Pan);
            }
        }
    }
};
static LfoModesTests LfoModesTestsInstance;
