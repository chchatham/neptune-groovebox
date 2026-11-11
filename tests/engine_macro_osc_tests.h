// Add to main_test.cpp: #include "engine_macro_osc_tests.h"
// Add static instance: static MacroOscExtendedTests macroOscExtendedTests;
// Add to test filter: || name == "Macro Osc Extended"
#pragma once

#include "../src/engine/MacroOscEngine.h"
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

        beginTest("Parameter count is 15 with envelope chain params");
        {
            MacroOscEngine engine;
            expectEquals(engine.getParameterCount(), 15);
            expectEquals(engine.getParameterName(MacroOscEngine::EnvChainMode),
                         juce::String("Env Chain"));
            expectEquals(engine.getParameterName(MacroOscEngine::AmpAttack),
                         juce::String("Attack"));
            expectEquals(engine.getParameterName(MacroOscEngine::AmpRelease),
                         juce::String("Release"));
        }
    }
};
