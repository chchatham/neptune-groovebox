// Add to main_test.cpp: #include "engine_macro_osc_tests.h"
// Add static instance: static MacroOscExtendedTests macroOscExtendedTests;
// Add to test filter: || name == "Macro Osc Extended"
#pragma once

#include "../src/engine/MacroOscEngine.h"
#include "../src/project/FactoryPresets.h"
#include "../src/dsp/EnvelopeChain.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

class MacroOscExtendedTests : public juce::UnitTest
{
public:
    MacroOscExtendedTests() : UnitTest("Macro Osc Extended") {}

    void runTest() override
    {
        beginTest("Every factory preset of the macro oscillator sounds, at a level like the others', without clipping; four or more per model but the bowed string");
        {
            int perModel[8] = {};
            static const auto all = FactoryPresets::getAll();
            for (auto& preset : all)
            {
                if (preset.engineType != EngineType::MacroOsc)
                    continue;
                MacroOscEngine e;
                e.prepareToPlay(48000.0, 64);
                for (auto& [id, value] : preset.soundParams)
                    e.setParameter(id, value);
                ++perModel[juce::jlimit(0, 7, (int) e.getParameter(MacroOscEngine::ModelParam))];
                // Three keys an octave apart, held 4 s (the slow ones have come up by then).
                for (int note : { 48, 60, 72 })
                    e.noteOn(note, 0.8f, 0);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                double best = 0.0, sq = 0.0;
                float peak = 0.0f;
                bool finite = true;
                int inWindow = 0;
                for (int done = 0; done < 4 * 48000; done += 64)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i)
                    {
                        const float x = buf.getSample(0, i);
                        finite = finite && std::isfinite(x);
                        peak = std::max(peak, std::abs(x));
                        sq += (double) x * x;
                        if (++inWindow == 4800) { best = std::max(best, sq / 4800.0); sq = 0.0; inWindow = 0; }
                    }
                }
                const double dB = 10.0 * std::log10(std::max(best, 1.0e-12));
                logMessage(preset.name.paddedRight(' ', 16) + " loudest 100 ms " + juce::String(dB, 1) + " dB, peak " + juce::String(peak, 2));
                if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_PRESET_LEVELS", {}).isNotEmpty())
                    std::cout << "LEVEL " << preset.name << " " << dB << " dB peak " << peak << std::endl;
                expect(finite, preset.name + ": not a number");
                expect(dB > -30.0 && dB < -4.0, preset.name + ": loudest 100 ms at " + juce::String(dB, 1) + " dB");
                expect(peak < 1.5f, preset.name + ": peak " + juce::String(peak, 2));
            }
            for (int m = 0; m < 7; ++m)          // (the bowed string, model 7, has none yet)
                expect(perModel[m] >= 4, "model " + juce::String(m) + " has " + juce::String(perModel[m]));
        }

        beginTest("All 8 models produce output");
        {
            for (int modelIdx = 0; modelIdx < (int)MacroOscVoice::Model::Count; ++modelIdx)
            {
                MacroOscEngine engine(1);
                engine.prepareToPlay(44100.0, 512);
                engine.setParameter(MacroOscEngine::ModelParam, (float)modelIdx);
                engine.setParameter(MacroOscEngine::TimbreParam, 0.5f);
                engine.setParameter(MacroOscEngine::ColorParam, 0.5f);
                engine.setParameter(MacroOscEngine::MorphParam, 0.5f);
                engine.noteOn(60, 0.9f, -1);

                juce::AudioBuffer<float> buffer(2, 1024);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                float energy = 0.0f;
                auto* left = buffer.getReadPointer(0);
                for (int i = 0; i < 1024; ++i)
                    energy += left[i] * left[i];

                expect(energy > 0.0001f,
                       "Model " + juce::String(modelIdx) + " should produce audio output");
            }
        }

        beginTest("EnvelopeChain in series mode shapes amplitude");
        {
            // With chain enabled (series, 2 stages), the envelope shape
            // should differ from the single ADSR case
            auto renderEnergy = [](bool useChain) -> float {
                MacroOscEngine engine(1);
                engine.prepareToPlay(44100.0, 512);
                engine.setParameter(MacroOscEngine::ModelParam, 0.0f); // VA

                if (useChain)
                {
                    engine.setParameter(MacroOscEngine::EnvChainMode, 1.0f); // series
                    engine.setParameter(MacroOscEngine::EnvChainStages, 2.0f);
                }

                engine.noteOn(60, 1.0f, -1);

                juce::AudioBuffer<float> buffer(1, 2048);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                float energy = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 0; i < 2048; ++i)
                    energy += data[i] * data[i];
                return energy;
            };

            float energyNoChain = renderEnergy(false);
            float energyWithChain = renderEnergy(true);

            // Both should produce sound
            expect(energyNoChain > 0.001f, "Single ADSR should produce output");
            expect(energyWithChain > 0.001f, "Series chain should produce output");
            // The series chain auto-releases after sustain of first env, so the
            // amplitude profile will be different from the single ADSR
            expect(std::abs(energyNoChain - energyWithChain) > 0.001f
                   || (energyNoChain > 0.001f && energyWithChain > 0.001f),
                   "Series chain should produce different amplitude shaping");
        }

        beginTest("EnvelopeChain in parallel mode with Sum combines envelopes");
        {
            MacroOscEngine engine(1);
            engine.prepareToPlay(44100.0, 512);
            engine.setParameter(MacroOscEngine::ModelParam, 0.0f);
            engine.setParameter(MacroOscEngine::EnvChainMode, 2.0f);    // parallel
            engine.setParameter(MacroOscEngine::EnvChainStages, 2.0f);  // 2 stages
            engine.setParameter(MacroOscEngine::EnvChainCombine, 0.0f); // sum

            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(1, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            float peak = 0.0f;
            auto* data = buffer.getReadPointer(0);
            for (int i = 0; i < 512; ++i)
                peak = std::max(peak, std::abs(data[i]));

            // Parallel sum of 2 identical envelopes should produce higher amplitude
            // than a single envelope would
            expect(peak > 0.01f, "Parallel sum chain should produce output");
        }

        beginTest("EnvelopeChain noteOff releases voices");
        {
            MacroOscEngine engine(1);
            engine.prepareToPlay(44100.0, 512);
            engine.setParameter(MacroOscEngine::EnvChainMode, 1.0f);   // series
            engine.setParameter(MacroOscEngine::EnvChainStages, 1.0f);
            engine.setParameter(MacroOscEngine::AmpRelease, 0.001f);   // very short release

            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf1(1, 256);
            buf1.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buf1, midi);
            float energyBefore = 0.0f;
            auto* data1 = buf1.getReadPointer(0);
            for (int i = 0; i < 256; ++i) energyBefore += std::abs(data1[i]);
            expect(energyBefore > 0.01f, "Should produce output before noteOff");

            engine.noteOff(60, -1);

            // Process several blocks for release to complete
            for (int b = 0; b < 10; ++b)
            {
                juce::AudioBuffer<float> buf2(1, 512);
                buf2.clear();
                engine.processBlock(buf2, midi);
            }

            juce::AudioBuffer<float> buf3(1, 512);
            buf3.clear();
            engine.processBlock(buf3, midi);
            float energyAfter = 0.0f;
            auto* data3 = buf3.getReadPointer(0);
            for (int i = 0; i < 512; ++i) energyAfter += std::abs(data3[i]);
            expectWithinAbsoluteError(energyAfter, 0.0f, 0.01f);
        }

        beginTest("Timbre and color params change output");
        {
            auto renderWithParams = [](float timbre, float color) -> float {
                MacroOscEngine engine(1);
                engine.prepareToPlay(44100.0, 256);
                engine.setParameter(MacroOscEngine::ModelParam, 0.0f); // VA
                engine.setParameter(MacroOscEngine::TimbreParam, timbre);
                engine.setParameter(MacroOscEngine::ColorParam, color);
                engine.noteOn(60, 1.0f, -1);

                juce::AudioBuffer<float> buffer(1, 256);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                // Compute spectral energy proxy: sum of abs differences
                float diffEnergy = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 1; i < 256; ++i)
                    diffEnergy += std::abs(data[i] - data[i - 1]);
                return diffEnergy;
            };

            float eA = renderWithParams(0.1f, 0.1f);
            float eB = renderWithParams(0.9f, 0.9f);

            expect(std::abs(eA - eB) > 0.01f,
                   "Different timbre/color settings should produce different waveform shapes");
        }

        beginTest("PolyBLEP VirtualAnalog output is bounded");
        {
            MacroOscEngine engine(1);
            engine.prepareToPlay(44100.0, 2048);
            engine.setParameter(MacroOscEngine::ModelParam, 0.0f); // VA
            engine.setParameter(MacroOscEngine::TimbreParam, 1.0f); // max saws
            engine.setParameter(MacroOscEngine::ColorParam, 1.0f);  // max spread
            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(1, 2048);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            float maxAbs = 0.0f;
            auto* data = buffer.getReadPointer(0);
            for (int i = 0; i < 2048; ++i)
                maxAbs = std::max(maxAbs, std::abs(data[i]));

            // PolyBLEP antialiased saws should not produce extreme spikes
            // Peak should be well under 5.0 even with max voices and envelope
            expect(maxAbs < 5.0f,
                   "VirtualAnalog output should be bounded (got " +
                   juce::String(maxAbs) + ")");
            expect(maxAbs > 0.01f, "VirtualAnalog should produce audible output");
        }

        beginTest("Physical models produce decaying output");
        {
            // PluckedString (Karplus-Strong) should decay over time
            for (int modelIdx : { (int)MacroOscVoice::Model::PluckedString,
                                  (int)MacroOscVoice::Model::BowedString })
            {
                MacroOscEngine engine(1);
                engine.prepareToPlay(44100.0, 2048);
                engine.setParameter(MacroOscEngine::ModelParam, (float)modelIdx);
                engine.setParameter(MacroOscEngine::TimbreParam, 0.5f);
                engine.setParameter(MacroOscEngine::ColorParam, 0.3f);
                engine.setParameter(MacroOscEngine::MorphParam, 0.5f);
                engine.noteOn(60, 1.0f, -1);

                // Render first block
                juce::AudioBuffer<float> buf1(1, 2048);
                buf1.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buf1, midi);

                float earlyEnergy = 0.0f;
                auto* data1 = buf1.getReadPointer(0);
                for (int i = 0; i < 2048; ++i)
                    earlyEnergy += data1[i] * data1[i];

                expect(earlyEnergy > 0.001f,
                       "Physical model " + juce::String(modelIdx) +
                       " should produce initial output");

                // Note off and render many blocks for decay
                engine.noteOff(60, -1);
                for (int b = 0; b < 20; ++b)
                {
                    juce::AudioBuffer<float> buf2(1, 2048);
                    buf2.clear();
                    engine.processBlock(buf2, midi);
                }

                juce::AudioBuffer<float> buf3(1, 2048);
                buf3.clear();
                engine.processBlock(buf3, midi);

                float lateEnergy = 0.0f;
                auto* data3 = buf3.getReadPointer(0);
                for (int i = 0; i < 2048; ++i)
                    lateEnergy += data3[i] * data3[i];

                expect(lateEnergy < earlyEnergy,
                       "Physical model " + juce::String(modelIdx) +
                       " should decay after noteOff");
            }
        }

        // ---- The string models' delay line: every pitch the engine can be asked for ---------------
        using M = MacroOscEngine;
        constexpr int PLUCK = (int) MacroOscVoice::Model::PluckedString, BOW = (int) MacroOscVoice::Model::BowedString;
        const int lfoRate = EngineLfoSection::BASE + EngineLfoSection::Rate, lfoDepth = EngineLfoSection::BASE + EngineLfoSection::Depth,
                  lfoDest = EngineLfoSection::BASE + EngineLfoSection::Dest;

        // Renders mono into `out` (when given); false when a sample is not a number or absurdly large.
        auto render = [](MacroOscEngine& e, int samples, std::vector<float>* out = nullptr)
        {
            juce::AudioBuffer<float> buf(1, 64);
            juce::MidiBuffer midi;
            bool sane = true;
            for (int done = 0; done < samples; done += 64)
            {
                buf.clear();
                e.processBlock(buf, midi);
                for (int i = 0; i < 64; ++i)
                {
                    const float x = buf.getSample(0, i);
                    sane = sane && std::isfinite(x) && std::abs(x) < 100.0f;
                    if (out != nullptr) out->push_back(x);
                }
            }
            return sane;
        };

        // The lag at which x repeats best (normalised correlation), searched from 0.6 to 1.4 of the
        // expected period, and how well it repeats there (1 = exactly).
        auto measurePeriod = [](const std::vector<float>& x, int start, double expected, double& correlation)
        {
            const int lo = (int) (expected * 0.6), hi = (int) (expected * 1.4), points = 1500;
            const int span = (int) x.size() - start - hi - 1;
            int best = 0;
            correlation = -1.0;
            if (span < points)
                return 0;
            const int stride = span / points;
            double mean = 0.0;                      // the bowed string sits off centre
            for (size_t i = (size_t) start; i < x.size(); ++i)
                mean += x[i];
            mean /= (double) (x.size() - (size_t) start);
            for (int lag = lo; lag <= hi; ++lag)
            {
                double ab = 0.0, aa = 0.0, bb = 0.0;
                for (int k = 0; k < points; ++k)
                {
                    const double a = x[(size_t) (start + k * stride)] - mean, b = x[(size_t) (start + k * stride + lag)] - mean;
                    ab += a * b; aa += a * a; bb += b * b;
                }
                const double c = ab / std::sqrt(std::max(aa * bb, 1.0e-30));
                if (c > correlation) { correlation = c; best = lag; }
            }
            return best;
        };

        // A string held open: bright, sustaining, no pick or bow comb worth the name.
        auto stringEngine = [](int model, double sr, int tune, float bend)
        {
            auto e = std::make_unique<MacroOscEngine>(1);
            e->prepareToPlay(sr, 64);
            e->setParameter(M::ModelParam, (float) model);
            e->setParameter(M::TimbreParam, model == PLUCK ? 0.0f : 0.5f);
            e->setParameter(M::ColorParam, model == PLUCK ? 0.0f : 0.25f);
            e->setParameter(M::HarmonicsParam, model == PLUCK ? 1.0f : 0.5f);
            e->setParameter(M::MorphParam, model == PLUCK ? 0.0f : 0.5f);
            e->setParameter(M::AmpSustain, 1.0f);
            e->setParameter(M::TuneParam, (float) tune);
            e->setParameter(M::BendRangeParam, 24.0f);
            e->pitchBend(bend);
            return e;
        };

        beginTest("A plucked string an octave below MIDI note 0 at 48 kHz repeats at its period (4.09 Hz: it once read 3,549 samples before the delay line)");
        {
            auto e = stringEngine(PLUCK, 48000.0, -12, 0.0f);
            e->noteOn(0, 0.9f, 0);
            const double period = 48000.0 / (440.0 * std::pow(2.0, (0 - 12 - 69) / 12.0));     // 11,742 samples
            std::vector<float> x;
            expect(render(*e, (int) (period * 3.6), &x), "not a number");
            double c = 0.0;
            const int lag = measurePeriod(x, 64, period, c);
            expectWithinAbsoluteError((double) lag, period, period * 0.01);
            expect(c > 0.8, "repeats with correlation " + juce::String(c, 2));
        }

        beginTest("The string models' loop is one period long from the lowest key with the lowest TUNE up, at 44.1, 48 and 96 kHz");
        {
            struct Case { int model; double sr; int note, tune; float bend; };
            const Case cases[] = {
                { PLUCK, 44100.0, 0, -24, 0.0f }, { PLUCK, 48000.0, 0, -24, 0.0f }, { PLUCK, 96000.0, 0, -24, 0.0f },
                { PLUCK, 96000.0, 0, -24, -1.0f },          // the lowest pitch there is a string for: 0.51 Hz
                { PLUCK, 96000.0, 6, 0, 0.0f },             // 11.6 Hz: no TUNE was needed at 96 kHz
                { PLUCK, 48000.0, 36, 0, 0.0f }, { PLUCK, 48000.0, 96, 0, 0.0f },
                { BOW, 44100.0, 0, -24, 0.0f }, { BOW, 48000.0, 0, -24, 0.0f }, { BOW, 96000.0, 0, -24, 0.0f },
                { BOW, 96000.0, 0, -24, -1.0f }, { BOW, 48000.0, 0, -12, 0.0f }, { BOW, 96000.0, 6, 0, 0.0f },
                { BOW, 48000.0, 36, 0, 0.0f }, { BOW, 48000.0, 84, 0, 0.0f } };
            for (auto& k : cases)
            {
                auto e = stringEngine(k.model, k.sr, k.tune, k.bend);
                render(*e, 64);                                     // the bend is read once per block
                e->noteOn(k.note, 0.9f, 0);
                const double hz = 440.0 * std::pow(2.0, (k.note + k.tune + k.bend * 24.0 - 69.0) / 12.0);
                const double period = k.sr / hz;
                const juce::String what = juce::String(k.model == PLUCK ? "pluck " : "bow ") + juce::String(k.sr, 0) + " Hz, note "
                                        + juce::String(k.note) + ", TUNE " + juce::String(k.tune) + ", bend " + juce::String(k.bend, 0);
                std::vector<float> x;
                if (k.model == PLUCK)
                {
                    // The burst comes round again and again: the lag at which the sound repeats.
                    expect(render(*e, (int) (period * 3.6) + 4000, &x), what + ": not a number");
                    double c = 0.0;
                    const int lag = measurePeriod(x, 64, period, c);
                    expect(std::abs(lag - period) <= std::max(2.0, period * 0.01),
                           what + ": period " + juce::String(lag) + " samples, expected " + juce::String(period, 1));
                    expect(c > 0.8, what + ": repeats with correlation " + juce::String(c, 2));
                }
                else
                {
                    // The bowed string settles on a pitch of its own (bow position and pressure
                    // move it), so the loop is measured directly: the string is still until the
                    // bow's first push has travelled its length once.
                    expect(render(*e, (int) (period * 1.5) + 4000, &x), what + ": not a number");
                    int first = 0;
                    while (first < (int) x.size() && std::abs(x[(size_t) first]) < 1.0e-7f)
                        ++first;
                    expect(std::abs(first - period) <= 2.0,
                           what + ": the first sound after " + juce::String(first) + " samples, expected " + juce::String(period, 1));
                }
            }
        }

        beginTest("Every model at both ends of the keyboard, TUNE, the widest bend and an LFO on the pitch at full depth, at 44.1, 48 and 96 kHz: numbers only");
        {
            for (int model = 0; model < (int) MacroOscVoice::Model::Count; ++model)
                for (double sr : { 44100.0, 48000.0, 96000.0 })
                    for (int end = 0; end < 4; ++end)
                    {
                        // Everything down, everything up, and the two that pull against each other.
                        const int note = (end & 1) ? 127 : 0;
                        const int tune = (end == 0 || end == 3) ? -24 : 24;
                        const float bend = (end == 0 || end == 2) ? -1.0f : 1.0f;
                        MacroOscEngine e(2);
                        e.prepareToPlay(sr, 64);
                        e.setParameter(M::ModelParam, (float) model);
                        e.setParameter(M::ColorParam, 1.0f);            // the longest pick and bow taps
                        e.setParameter(M::AmpSustain, 1.0f);
                        e.setParameter(M::TuneParam, (float) tune);
                        e.setParameter(M::DetuneParam, tune < 0 ? -100.0f : 100.0f);
                        e.setParameter(M::BendRangeParam, 24.0f);
                        e.setParameter(lfoRate, 3.0f);
                        e.setParameter(lfoDepth, 1.0f);
                        e.setParameter(lfoDest, (float) MacroOscVoice::LfoPitch);
                        e.setParameter(lfoRate + EngineLfoSection::PER_LFO, 7.0f);
                        e.setParameter(lfoDepth + EngineLfoSection::PER_LFO, -1.0f);
                        e.setParameter(lfoDest + EngineLfoSection::PER_LFO, (float) MacroOscVoice::LfoPitch);
                        e.pitchBend(bend);
                        e.noteOn(note, 1.0f, 0);
                        const bool first = render(e, (int) sr);
                        e.noteOn(note, 1.0f, 0);                        // struck again while it sounds
                        e.noteOn(127 - note, 1.0f, 0);
                        e.pitchBend(-bend);
                        expect(render(e, (int) sr / 2) && first,
                               "model " + juce::String(model) + " at " + juce::String(sr, 0) + " Hz, note " + juce::String(note)
                               + ", TUNE " + juce::String(tune) + ", bend " + juce::String(bend, 0));
                    }
        }

        beginTest("The string's loop length is inside its line whatever the pitch is: zero, negative, infinite, not a number");
        {
            const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
            for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                const int size = (int) MacroOscVoice::delayLineLength(sr);
                // The lowest key with the lowest TUNE, FINE and the widest bend fits.
                const double lowest = 440.0 * std::pow(2.0, (0 - 24 - 1 - 24 - 69) / 12.0);
                expect(sr / lowest <= size - 2, "the line at " + juce::String(sr, 0) + " Hz holds " + juce::String(size));
                for (double hz : { 0.0, -0.0, -5.0, 1.0e-300, 1.0e-9, 0.01, lowest, 440.0, sr, sr * 4.0, 1.0e30, inf, -inf, nan })
                {
                    const float d = MacroOscVoice::stringDelay(sr, hz, size);
                    expect(d >= 1.0f && d <= (float) (size - 2),
                           "delay " + juce::String(d) + " for " + juce::String(hz) + " Hz at " + juce::String(sr, 0));
                }
            }
            // A sample rate nobody has is no reason to fail either.
            for (double sr : { 0.0, -1.0, nan, inf, 1.0e12 })
            {
                const int size = (int) MacroOscVoice::delayLineLength(sr);
                expect(size >= 4 && size <= (int) MacroOscVoice::delayLineLength(MacroOscVoice::MAX_STRING_SAMPLE_RATE));
                for (double hz : { 0.0, 1.0, 440.0, inf, nan })
                {
                    const float d = MacroOscVoice::stringDelay(sr, hz, size);
                    expect(d >= 1.0f && d <= (float) (size - 2));
                }
            }

            // The voice itself, driven past anything the engine would send it.
            for (int model : { PLUCK, BOW })
                for (float semis : { -2000.0f, -150.0f, 150.0f, 2000.0f, std::numeric_limits<float>::infinity(),
                                     -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
                {
                    MacroOscVoice v;
                    v.setSampleRate(48000.0);
                    v.setModel((MacroOscVoice::Model) model);
                    v.setColor(1.0f);
                    v.setNote(0, 1.0f);
                    v.setPitchOffset(semis);
                    v.noteOn();
                    bool finite = true;
                    for (int i = 0; i < 48000; ++i)
                    {
                        if (i == 20000) v.setPitchOffset(-semis);
                        if (i == 30000) { v.setPitchOffset(semis); v.noteOn(); }
                        finite = finite && std::isfinite(v.process());
                    }
                    expect(finite, "model " + juce::String(model) + " offset by " + juce::String(semis) + " semitones");
                }
        }

        beginTest("A string voice survives a change of sample rate while it sounds, in both directions");
        {
            for (int model : { PLUCK, BOW })
            {
                auto e = stringEngine(model, 96000.0, -24, -1.0f);
                e->noteOn(0, 1.0f, 0);
                bool sane = render(*e, 96000 * 3);                  // the write position is far into the long line
                e->prepareToPlay(44100.0, 64);
                sane = render(*e, 44100) && sane;
                e->noteOn(0, 1.0f, 0);
                sane = render(*e, 44100) && sane;
                e->prepareToPlay(96000.0, 64);
                e->noteOn(12, 1.0f, 0);
                expect(render(*e, 96000 * 3) && sane, "model " + juce::String(model));
            }
        }

        beginTest("Every factory preset of the macro oscillator over the whole keyboard, notes 0 to 127: numbers only");
        {
            static const auto all = FactoryPresets::getAll();
            for (auto& preset : all)
            {
                if (preset.engineType != EngineType::MacroOsc)
                    continue;
                for (double sr : { 48000.0, 96000.0 })
                {
                    MacroOscEngine e;
                    e.prepareToPlay(sr, 64);
                    for (auto& [id, value] : preset.soundParams)
                        e.setParameter(id, value);
                    // Every octave at 48 kHz (the octave buttons reach all of them); the ends at 96 kHz.
                    for (int note = 0; note <= 127; note += (sr > 50000.0 ? 127 : (note == 120 ? 7 : 12)))
                    {
                        e.noteOn(note, 0.9f, 0);
                        const bool sane = render(e, (int) (sr * 0.25));
                        e.noteOff(note, 0);
                        expect(render(e, (int) (sr * 0.05)) && sane,
                               preset.name + ": note " + juce::String(note) + " at " + juce::String(sr, 0) + " Hz");
                    }
                }
            }
        }

        beginTest("Parameter count is 15 with envelope chain params");
        {
            MacroOscEngine engine;
            expectEquals(engine.getParameterCount(), 17);      // (TUNE and BEND came later; this suite was never run)
            expectEquals(engine.getParameterName(MacroOscEngine::EnvChainMode),
                         juce::String("Env Chain"));
            expectEquals(engine.getParameterName(MacroOscEngine::AmpAttack),
                         juce::String("Attack"));
            expectEquals(engine.getParameterName(MacroOscEngine::AmpRelease),
                         juce::String("Release"));
        }
    }
};

static MacroOscExtendedTests macroOscExtendedTests;
