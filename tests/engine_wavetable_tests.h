// Add to main_test.cpp: #include "engine_wavetable_tests.h"
// Add static instance: static WavetableExtendedTests wavetableExtendedTests;
// Add to test filter: || name == "Wavetable Extended"
#pragma once

#include "../src/engine/WavetableEngine.h"
#include "../src/dsp/EnvelopeChain.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

class WavetableExtendedTests : public juce::UnitTest
{
public:
    WavetableExtendedTests() : UnitTest("Wavetable Extended") {}

    void runTest() override
    {
        beginTest("All 4 interpolation modes produce output");
        {
            for (int mode = 0; mode < 4; ++mode)
            {
                WavetableEngine engine(1);
                engine.prepareToPlay(44100.0, 256);
                engine.setParameter(WavetableEngine::InterpMode, (float) mode);
                engine.setParameter(WavetableEngine::Position, 0.5f);
                engine.noteOn(60, 1.0f, -1);
                juce::AudioBuffer<float> buffer(1, 256);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);
                float energy = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 0; i < 256; ++i)
                    energy += data[i] * data[i];
                expect(energy > 0.001f,
                       "Interpolation mode " + juce::String(mode) + " should produce output");
            }
        }

        beginTest("Different interpolation modes produce different output");
        {
            std::array<float, 4> energies = {};
            for (int mode = 0; mode < 4; ++mode)
            {
                WavetableEngine engine(1);
                engine.prepareToPlay(44100.0, 512);
                // Use a table with many frames so interpolation differences show
                engine.setParameter(WavetableEngine::TableSelect, 0.0f); // Saw
                engine.setParameter(WavetableEngine::InterpMode, (float) mode);
                engine.setParameter(WavetableEngine::Position, 0.3f);
                engine.noteOn(60, 1.0f, -1);
                juce::AudioBuffer<float> buffer(1, 512);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);
                float diffEnergy = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 1; i < 512; ++i)
                    diffEnergy += std::abs(data[i] - data[i - 1]);
                energies[(size_t) mode] = diffEnergy;
            }
            // Stepped and Vintage should differ from Linear
            bool steppedDiffers = std::abs(energies[0] - energies[2]) > 0.001f
                               || std::abs(energies[0] - energies[3]) > 0.001f;
            expect(steppedDiffers,
                   "Stepped/Vintage modes should differ from Linear");
        }

        beginTest("Mip-mapping produces correct number of levels");
        {
            // Saw table with 64 frames, kFrameSize=256 => 128 harmonics max
            // levels: 128 -> 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1 = 8 levels
            Wavetable wt = WavetableFactory::createSaw(4);
            expect(wt.getMipLevelCount() >= 2,
                   "Mip-mapped table should have at least 2 levels");
            expect(wt.getMipLevelCount() <= Wavetable::kMaxMipLevels,
                   "Mip levels should not exceed maximum");
            // Each level should have the same number of frames
            for (int level = 0; level < wt.getMipLevelCount(); ++level)
            {
                expectEquals((int) wt.frames[(size_t) level].size(), wt.numFrames,
                             "Each mip level should have numFrames frames");
            }
        }

        beginTest("Factory tables are valid and normalized");
        {
            auto tables = WavetableFactory::createAllFactoryTables();
            expectEquals((int) tables.size(), 9, "Should have 9 factory tables");   // the engine adds USER as a tenth

            for (int t = 0; t < (int) tables.size(); ++t)
            {
                const auto& wt = tables[(size_t) t];
                expect(wt.numFrames > 0,
                       "Table " + juce::String(t) + " should have frames");
                expect(!wt.frames.empty(),
                       "Table " + juce::String(t) + " should have mip data");

                // Check base level frames are non-empty and roughly normalized
                for (int f = 0; f < wt.numFrames; ++f)
                {
                    float peak = 0.0f;
                    const auto& frame = wt.frames[0][(size_t) f];
                    for (int n = 0; n < Wavetable::kFrameSize; ++n)
                        peak = std::max(peak, std::abs(frame[(size_t) n]));
                    expect(peak > 0.0f || wt.numFrames == 1,
                           "Frame " + juce::String(f) + " of table "
                           + juce::String(t) + " should have non-zero samples");
                    expect(peak <= 1.01f,
                           "Frame should be normalized (peak <= 1.0)");
                }
            }
        }

        beginTest("Sample-to-wavetable conversion");
        {
            // Create a sine wave buffer as source
            const int srcLen = 4096;
            std::vector<float> srcSamples((size_t) srcLen);
            for (int i = 0; i < srcLen; ++i)
                srcSamples[(size_t) i] = std::sin(dsp::TWO_PI_F * (float) i / 256.0f);

            Wavetable wt = WavetableFactory::createFromSamples(
                srcSamples.data(), srcLen, 16);

            expectEquals(wt.numFrames, 16,
                         "Converted table should have requested frame count");
            expect(wt.getMipLevelCount() >= 2,
                   "Converted table should be mip-mapped");

            // Each frame should be normalized
            for (int f = 0; f < wt.numFrames; ++f)
            {
                float peak = 0.0f;
                const auto& frame = wt.frames[0][(size_t) f];
                for (int n = 0; n < Wavetable::kFrameSize; ++n)
                    peak = std::max(peak, std::abs(frame[(size_t) n]));
                expect(peak > 0.5f,
                       "Converted frame should have significant amplitude");
                expect(peak <= 1.01f,
                       "Converted frame should be normalized");
            }
        }

        beginTest("EnvelopeChain in series mode shapes output");
        {
            WavetableEngine engine(1);
            engine.prepareToPlay(44100.0, 256);

            // Enable series envelope chain with 2 stages
            engine.setParameter(WavetableEngine::EnvChainMode, 1.0f);   // series
            engine.setParameter(WavetableEngine::EnvChainStages, 2.0f); // 2 stages

            engine.noteOn(60, 1.0f, -1);
            juce::AudioBuffer<float> buffer(1, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            float energy = 0.0f;
            auto* data = buffer.getReadPointer(0);
            for (int i = 0; i < 512; ++i)
                energy += data[i] * data[i];
            expect(energy > 0.001f,
                   "Engine with envelope chain should produce output");

            // Verify the chain is actually being used
            expect(engine.getVoice(0).isEnvelopeChainEnabled(),
                   "Envelope chain should be enabled");
            expect(engine.getVoice(0).getEnvelopeChain().getStageCount() == 2,
                   "Envelope chain should have 2 stages");
        }

        beginTest("Sub oscillator adds output when enabled");
        {
            auto renderRMS = [](float subLevel) -> float {
                WavetableEngine engine(1);
                engine.prepareToPlay(44100.0, 512);
                engine.setParameter(WavetableEngine::SubLevel, subLevel);
                engine.noteOn(60, 1.0f, -1);
                juce::AudioBuffer<float> buffer(1, 512);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);
                float sum = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 0; i < 512; ++i)
                    sum += data[i] * data[i];
                return std::sqrt(sum / 512.0f);
            };

            float rmsNoSub = renderRMS(0.0f);
            float rmsWithSub = renderRMS(1.0f);
            expect(rmsWithSub > rmsNoSub,
                   "Adding sub oscillator should increase RMS level");
        }

        beginTest("Morph speed modulates position over time");
        {
            WavetableEngine engine(1);
            engine.prepareToPlay(44100.0, 4096);
            engine.setParameter(WavetableEngine::TableSelect, 0.0f); // Saw (many frames)
            engine.setParameter(WavetableEngine::Position, 0.5f);
            engine.setParameter(WavetableEngine::MorphSpeed, 10.0f); // 10 Hz LFO

            engine.noteOn(60, 1.0f, -1);

            // Render two consecutive blocks and compare
            juce::AudioBuffer<float> buf1(1, 4096);
            buf1.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buf1, midi);

            juce::AudioBuffer<float> buf2(1, 4096);
            buf2.clear();
            engine.processBlock(buf2, midi);

            // The morph LFO should cause timbral variation over time.
            // Compare spectral difference (sum of abs differences) between blocks.
            float diffSum = 0.0f;
            auto* d1 = buf1.getReadPointer(0);
            auto* d2 = buf2.getReadPointer(0);
            for (int i = 0; i < 4096; ++i)
                diffSum += std::abs(d1[i] - d2[i]);
            expect(diffSum > 0.1f,
                   "Morph speed should cause timbral variation between blocks");
        }

        beginTest("Table selection changes timbre");
        {
            auto renderWithTable = [](int tableIdx) -> float {
                WavetableEngine engine(1);
                engine.prepareToPlay(44100.0, 512);
                engine.setParameter(WavetableEngine::TableSelect, (float) tableIdx);
                engine.setParameter(WavetableEngine::Position, 0.5f);
                engine.noteOn(60, 1.0f, -1);
                juce::AudioBuffer<float> buffer(1, 512);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);
                float diffEnergy = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 1; i < 512; ++i)
                    diffEnergy += std::abs(data[i] - data[i - 1]);
                return diffEnergy;
            };

            float e0 = renderWithTable(0); // Saw
            float e1 = renderWithTable(1); // Square
            float e3 = renderWithTable(3); // Sine
            bool anyDiffer = (std::abs(e0 - e1) > 0.01f)
                          || (std::abs(e0 - e3) > 0.01f);
            expect(anyDiffer,
                   "Different wavetables should produce different timbres");
        }

        beginTest("Parameter count and new param names");
        {
            WavetableEngine engine;
            expectEquals(engine.getParameterCount(), 16);
            expectEquals(engine.getParameterName(WavetableEngine::EnvChainMode),
                         juce::String("Env Chain"));
            expectEquals(engine.getParameterName(WavetableEngine::EnvChainStages),
                         juce::String("Chain Stages"));
            expectEquals(engine.getParameterName(WavetableEngine::EnvChainCombine),
                         juce::String("Chain Combine"));
        }

        beginTest("EnvelopeChain combine mode parameter");
        {
            WavetableEngine engine(1);
            engine.prepareToPlay(44100.0, 256);

            // Enable parallel chain with max combine
            engine.setParameter(WavetableEngine::EnvChainMode, 2.0f);    // parallel
            engine.setParameter(WavetableEngine::EnvChainStages, 2.0f);
            engine.setParameter(WavetableEngine::EnvChainCombine, 1.0f);  // max

            auto& chain = engine.getVoice(0).getEnvelopeChain();
            expect(chain.getCombineMode() == EnvelopeChain::CombineMode::Max,
                   "Chain combine mode should be Max");
        }

        beginTest("Disabling envelope chain reverts to simple ADSR");
        {
            WavetableEngine engine(1);
            engine.prepareToPlay(44100.0, 256);

            // Enable then disable
            engine.setParameter(WavetableEngine::EnvChainMode, 1.0f);
            expect(engine.getVoice(0).isEnvelopeChainEnabled(),
                   "Chain should be enabled");
            engine.setParameter(WavetableEngine::EnvChainMode, 0.0f);
            expect(!engine.getVoice(0).isEnvelopeChainEnabled(),
                   "Chain should be disabled after setting mode to 0");

            // Should still produce output using simple ADSR
            engine.noteOn(60, 1.0f, -1);
            juce::AudioBuffer<float> buffer(1, 256);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);
            float energy = 0.0f;
            auto* data = buffer.getReadPointer(0);
            for (int i = 0; i < 256; ++i)
                energy += data[i] * data[i];
            expect(energy > 0.001f,
                   "Engine should produce output with ADSR after disabling chain");
        }
    }
};
