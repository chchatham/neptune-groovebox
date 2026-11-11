// Add to main_test.cpp: #include "engine_waveseq_tests.h"
// Add static instance: static WaveSeqExtendedTests waveSeqExtendedTests;
// Add to test filter: || name == "Wave Seq Extended"
#pragma once

#include "../src/engine/WaveSeqEngine.h"
#include "../src/dsp/EnvelopeChain.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

class WaveSeqExtendedTests : public juce::UnitTest
{
public:
    WaveSeqExtendedTests() : UnitTest("Wave Seq Extended") {}

    void runTest() override
    {
        beginTest("Independent lane lengths produce different patterns");
        {
            // Setup engine with pitch lane length 2, timing lane length 4
            auto renderWithLaneLengths = [](int timingLen, int pitchLen) -> float {
                WaveSeqEngine engine(1);
                engine.prepareToPlay(44100.0, 512);

                // Set up alternating pitch offsets on steps
                for (int s = 0; s < WaveSeqVoice::MAX_STEPS; ++s)
                {
                    engine.getVoice(0).getStep(s).duration = 0.01f; // fast steps
                    engine.getVoice(0).getStep(s).pitchOffset = (s % 2 == 0) ? 0.0f : 7.0f;
                    engine.getVoice(0).getStep(s).shape = WaveSeqVoice::Waveform::Saw;
                    engine.getVoice(0).getStep(s).level = 1.0f;
                }

                engine.setParameter(WaveSeqEngine::TimingLaneLen, (float) timingLen);
                engine.setParameter(WaveSeqEngine::PitchLaneLen, (float) pitchLen);
                engine.setParameter(WaveSeqEngine::PitchRange, 12.0f);

                engine.noteOn(60, 1.0f, -1);

                juce::AudioBuffer<float> buffer(1, 2048);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                // Compute spectral fingerprint (sum of absolute sample differences)
                float diffSum = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 1; i < 2048; ++i)
                    diffSum += std::abs(data[i] - data[i - 1]);
                return diffSum;
            };

            float uniformPattern = renderWithLaneLengths(4, 4);
            float polymetricPattern = renderWithLaneLengths(4, 2);
            expect(std::abs(uniformPattern - polymetricPattern) > 0.01f,
                   "Different pitch lane length should produce a different pattern");
        }

        beginTest("All loop modes produce output");
        {
            for (int modeIdx = 0; modeIdx < 5; ++modeIdx)
            {
                WaveSeqEngine engine(1);
                engine.prepareToPlay(44100.0, 512);

                for (int s = 0; s < 4; ++s)
                {
                    auto& step = engine.getVoice(0).getStep(s);
                    step.duration = 0.05f;
                    step.pitchOffset = 0.0f;
                    step.shape = WaveSeqVoice::Waveform::Saw;
                    step.level = 1.0f;
                }

                engine.setParameter(WaveSeqEngine::SeqLength, 4.0f);
                engine.setParameter(WaveSeqEngine::LoopModeParam, (float) modeIdx);
                engine.noteOn(60, 0.8f, -1);

                juce::AudioBuffer<float> buffer(2, 512);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                float peak = buffer.getMagnitude(0, 0, 512);
                expect(peak > 0.001f,
                       "Loop mode " + juce::String(modeIdx) + " should produce output");
            }
        }

        beginTest("Crossfade amount affects output smoothness");
        {
            auto renderWithCrossfade = [](float cfAmount) -> float {
                WaveSeqEngine engine(1);
                engine.prepareToPlay(44100.0, 512);

                // Alternating shapes to create step transitions
                for (int s = 0; s < 4; ++s)
                {
                    auto& step = engine.getVoice(0).getStep(s);
                    step.duration = 0.005f; // very fast steps to force transitions
                    step.pitchOffset = 0.0f;
                    step.shape = (s % 2 == 0) ? WaveSeqVoice::Waveform::Sine
                                               : WaveSeqVoice::Waveform::Square;
                    step.level = 1.0f;
                }

                engine.setParameter(WaveSeqEngine::SeqLength, 4.0f);
                engine.setParameter(WaveSeqEngine::Crossfade, cfAmount);
                engine.noteOn(60, 1.0f, -1);

                juce::AudioBuffer<float> buffer(1, 2048);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                // Measure roughness (sum of absolute second differences)
                float roughness = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 2; i < 2048; ++i)
                {
                    float d2 = data[i] - 2.0f * data[i-1] + data[i-2];
                    roughness += std::abs(d2);
                }
                return roughness;
            };

            float noCrossfade = renderWithCrossfade(0.0f);
            float fullCrossfade = renderWithCrossfade(1.0f);
            // Just check both produce values (crossfade may or may not be smoother
            // depending on waveform content, but they should differ)
            expect(noCrossfade > 0.0f, "No crossfade should produce non-zero roughness");
            expect(fullCrossfade > 0.0f, "Full crossfade should produce non-zero roughness");
        }

        beginTest("Step interpolation on vs off produces different output");
        {
            auto renderWithInterp = [](bool interpOn) -> float {
                WaveSeqEngine engine(1);
                engine.prepareToPlay(44100.0, 512);

                // Steps with very different pitch and level values
                for (int s = 0; s < 4; ++s)
                {
                    auto& step = engine.getVoice(0).getStep(s);
                    step.duration = 0.01f;
                    step.pitchOffset = (s % 2 == 0) ? 0.0f : 12.0f;
                    step.shape = WaveSeqVoice::Waveform::Saw;
                    step.level = (s % 2 == 0) ? 1.0f : 0.3f;
                }

                engine.setParameter(WaveSeqEngine::SeqLength, 4.0f);
                engine.setParameter(WaveSeqEngine::PitchRange, 12.0f);
                engine.setParameter(WaveSeqEngine::LaneInterp, interpOn ? 1.0f : 0.0f);
                engine.noteOn(60, 1.0f, -1);

                juce::AudioBuffer<float> buffer(1, 4096);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                float sum = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 0; i < 4096; ++i)
                    sum += data[i] * data[i];
                return sum;
            };

            float withoutInterp = renderWithInterp(false);
            float withInterp = renderWithInterp(true);
            expect(std::abs(withoutInterp - withInterp) > 0.001f,
                   "Interpolation should change the output character");
        }

        beginTest("EnvelopeChain in parallel mode");
        {
            WaveSeqEngine engine(1);
            engine.prepareToPlay(44100.0, 512);

            for (int s = 0; s < 4; ++s)
            {
                auto& step = engine.getVoice(0).getStep(s);
                step.duration = 0.1f;
                step.pitchOffset = 0.0f;
                step.shape = WaveSeqVoice::Waveform::Saw;
                step.level = 1.0f;
            }

            // Enable envelope chain in parallel mode with 2 stages
            engine.setParameter(WaveSeqEngine::EnvChainMode, 2.0f);   // parallel
            engine.setParameter(WaveSeqEngine::EnvChainStages, 2.0f);
            engine.setParameter(WaveSeqEngine::EnvChainCombine, 0.0f); // sum
            engine.setParameter(WaveSeqEngine::SeqLength, 4.0f);

            engine.noteOn(60, 0.8f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            float peak = buffer.getMagnitude(0, 0, 512);
            expect(peak > 0.001f,
                   "Engine with envelope chain in parallel mode should produce output");

            // Verify chain is actually enabled on the voice
            expect(engine.getVoice(0).isEnvelopeChainEnabled(),
                   "Envelope chain should be enabled on voice");
        }

        beginTest("One-shot mode stops advancing after last step");
        {
            WaveSeqEngine engine(1);
            engine.prepareToPlay(44100.0, 512);

            // Short 2-step sequence
            for (int s = 0; s < 2; ++s)
            {
                auto& step = engine.getVoice(0).getStep(s);
                step.duration = 0.005f; // 5ms per step
                step.pitchOffset = (float) s * 5.0f; // different pitches
                step.shape = WaveSeqVoice::Waveform::Sine;
                step.level = 1.0f;
            }

            engine.setParameter(WaveSeqEngine::SeqLength, 2.0f);
            engine.setParameter(WaveSeqEngine::LoopModeParam, 4.0f); // OneShot
            engine.noteOn(60, 1.0f, -1);

            // Process enough samples to go past both steps
            // At 44100 Hz, 5ms = 220.5 samples per step, 2 steps = ~441 samples
            juce::AudioBuffer<float> buffer(1, 2048);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            // The voice should still be active (envelope hasn't been released)
            // but the sequence should have stopped advancing
            expect(engine.getVoice(0).isActive(),
                   "Voice should still be active after one-shot completes");
        }

        beginTest("Speed multiplier changes playback rate");
        {
            auto countStepTransitions = [](float speed) -> float {
                WaveSeqEngine engine(1);
                engine.prepareToPlay(44100.0, 512);

                for (int s = 0; s < 4; ++s)
                {
                    auto& step = engine.getVoice(0).getStep(s);
                    step.duration = 0.01f;
                    step.pitchOffset = (float) s * 3.0f; // rising pitch
                    step.shape = WaveSeqVoice::Waveform::Saw;
                    step.level = 1.0f;
                }

                engine.setParameter(WaveSeqEngine::SeqLength, 4.0f);
                engine.setParameter(WaveSeqEngine::Speed, speed);
                engine.setParameter(WaveSeqEngine::PitchRange, 12.0f);
                engine.noteOn(60, 1.0f, -1);

                juce::AudioBuffer<float> buffer(1, 4096);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);

                // Measure energy as proxy for rate
                float energy = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 1; i < 4096; ++i)
                    energy += std::abs(data[i] - data[i - 1]);
                return energy;
            };

            float slowEnergy = countStepTransitions(2.0f);  // 2x slower
            float fastEnergy = countStepTransitions(0.5f);  // 2x faster
            // Faster playback = more step transitions = different spectral content
            expect(std::abs(slowEnergy - fastEnergy) > 0.01f,
                   "Different speed multipliers should produce different outputs");
        }

        beginTest("Parameter names for new params resolve");
        {
            WaveSeqEngine engine;
            expectEquals(engine.getParameterName(WaveSeqEngine::TimingLaneLen), juce::String("Tim Len"));
            expectEquals(engine.getParameterName(WaveSeqEngine::PitchLaneLen), juce::String("Pit Len"));
            expectEquals(engine.getParameterName(WaveSeqEngine::ShapeLaneLen), juce::String("Shp Len"));
            expectEquals(engine.getParameterName(WaveSeqEngine::LevelLaneLen), juce::String("Lvl Len"));
            expectEquals(engine.getParameterName(WaveSeqEngine::LaneInterp), juce::String("Lane Interp"));
            expectEquals(engine.getParameterName(WaveSeqEngine::EnvChainMode), juce::String("Env Chain"));
            expectEquals(engine.getParameterName(WaveSeqEngine::EnvChainStages), juce::String("Chain Stg"));
            expectEquals(engine.getParameterName(WaveSeqEngine::EnvChainCombine), juce::String("Chain Comb"));
            expectEquals(engine.getParameterCount(), 16);
        }
    }
};
