#pragma once

#include "../src/engine/SamplerEngine.h"
#include "../src/project/SamplePool.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/controller/DisplayRenderer.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <vector>

// ============================================================================
//  Sampler Engine Full Tests
// ============================================================================
class SamplerFullTests : public juce::UnitTest
{
public:
    SamplerFullTests() : juce::UnitTest("Sampler Full") {}

    void runTest() override
    {
        // -- Stereo playback --
        beginTest("Stereo playback reads both channels");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 128);

            // Create a stereo sample: L = sine, R = cosine
            const int len = 4410;
            std::vector<float> interleaved(len * 2);
            for (int i = 0; i < len; ++i)
            {
                float phase = 2.0f * 3.14159f * 440.0f * (float)i / 44100.0f;
                interleaved[i * 2]     = std::sin(phase);  // L
                interleaved[i * 2 + 1] = std::cos(phase);  // R
            }
            samp.loadSampleStereoInterleaved(interleaved.data(), len, 44100.0);

            expect(samp.getSampleChannels() == 2, "Should report 2 channels");
            expect(samp.getSampleDataR() != nullptr, "Should have R channel data");

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);
            samp.setParameter(SamplerEngine::Pan, 0.0f);

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 128);
            buf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf, midi);

            float maxL = 0.0f, maxR = 0.0f;
            for (int i = 0; i < 128; ++i)
            {
                maxL = std::max(maxL, std::abs(buf.getSample(0, i)));
                maxR = std::max(maxR, std::abs(buf.getSample(1, i)));
            }

            expect(maxL > 0.01f, "Left channel should have audio");
            expect(maxR > 0.01f, "Right channel should have audio");
        }

        // -- Mono sample plays to both channels --
        beginTest("Mono sample plays to both output channels");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 128);

            std::vector<float> mono(4410);
            for (int i = 0; i < 4410; ++i)
                mono[i] = std::sin(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
            samp.loadSample(mono.data(), 4410, 1, 44100.0);

            expect(samp.getSampleChannels() == 1, "Should report 1 channel");

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);
            samp.setParameter(SamplerEngine::Pan, 0.0f);

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 128);
            buf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf, midi);

            float maxL = 0.0f, maxR = 0.0f;
            for (int i = 0; i < 128; ++i)
            {
                maxL = std::max(maxL, std::abs(buf.getSample(0, i)));
                maxR = std::max(maxR, std::abs(buf.getSample(1, i)));
            }
            expect(maxL > 0.01f, "L should have audio");
            expect(maxR > 0.01f, "R should have audio");
        }

        // -- Loop mode --
        beginTest("Loop mode keeps playing past sample end");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 64);

            // Very short sample
            std::vector<float> data(64, 0.5f);
            samp.loadSample(data.data(), 64, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Loop);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::LoopEnd, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);

            samp.noteOn(60, 1.0f, -1);

            // Process several blocks past sample length
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            for (int b = 0; b < 10; ++b)
            {
                buf.clear();
                samp.processBlock(buf, midi);
            }

            // Should still be producing audio after 10 blocks
            buf.clear();
            samp.processBlock(buf, midi);
            float maxVal = 0.0f;
            for (int i = 0; i < 64; ++i)
                maxVal = std::max(maxVal, std::abs(buf.getSample(0, i)));

            expect(maxVal > 0.01f, "Loop mode should keep producing audio");
        }

        // -- Ping-pong loop --
        beginTest("Ping-pong loop reverses direction");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 64);

            // Ascending ramp so direction is audible
            std::vector<float> ramp(128);
            for (int i = 0; i < 128; ++i)
                ramp[i] = (float)i / 128.0f;
            samp.loadSample(ramp.data(), 128, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::PingPong);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 1.0f);

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;

            // Process enough blocks for forward-then-reverse
            for (int b = 0; b < 6; ++b)
            {
                buf.clear();
                samp.processBlock(buf, midi);
            }

            // If still producing audio after many blocks, ping-pong is working
            float maxVal = 0.0f;
            for (int i = 0; i < 64; ++i)
                maxVal = std::max(maxVal, std::abs(buf.getSample(0, i)));
            expect(maxVal > 0.001f, "Ping-pong should still produce audio");
        }

        // -- One-shot mode --
        beginTest("One-shot stops after sample end");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 64);

            std::vector<float> data(128, 0.5f);
            samp.loadSample(data.data(), 128, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::OneShot);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            // Process 4 blocks = 256 samples > 128 sample length
            for (int b = 0; b < 4; ++b)
            {
                buf.clear();
                samp.processBlock(buf, midi);
            }

            buf.clear();
            samp.processBlock(buf, midi);
            float sum = 0.0f;
            for (int i = 0; i < 64; ++i)
                sum += std::abs(buf.getSample(0, i));
            expect(sum < 0.01f, "One-shot should be silent after end");
        }

        // -- Reverse playback --
        beginTest("Reverse playback plays backwards");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 64);

            // Ascending ramp
            std::vector<float> ramp(256);
            for (int i = 0; i < 256; ++i)
                ramp[i] = (float)i / 256.0f;
            samp.loadSample(ramp.data(), 256, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Reverse);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 1.0f);

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf, midi);

            // In reverse, first output samples should be from end of ramp (high values)
            // and should decrease
            float sum = 0.0f;
            for (int i = 0; i < 64; ++i) sum += std::abs(buf.getSample(0, i));
            expect(sum > 0.0f, "Reverse mode should produce non-silent output");
        }

        // -- Pitch shifting via MIDI note --
        beginTest("Pitch shifting via MIDI note changes playback rate");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 128);

            std::vector<float> data(4410);
            for (int i = 0; i < 4410; ++i)
                data[i] = std::sin(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
            samp.loadSample(data.data(), 4410, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);

            // Note 60 = root pitch (1x)
            samp.noteOn(60, 1.0f, -1);
            juce::AudioBuffer<float> buf60(2, 128);
            buf60.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf60, midi);
            samp.allNotesOff();

            // Note 72 = one octave up (2x playback rate)
            samp.noteOn(72, 1.0f, -1);
            juce::AudioBuffer<float> buf72(2, 128);
            buf72.clear();
            samp.processBlock(buf72, midi);

            // At 2x rate, the waveform period should be halved.
            // Count zero crossings as proxy for frequency
            auto countZeroCrossings = [](const juce::AudioBuffer<float>& b, int numSamps) {
                int crossings = 0;
                for (int i = 1; i < numSamps; ++i)
                {
                    if ((b.getSample(0, i-1) >= 0) != (b.getSample(0, i) >= 0))
                        crossings++;
                }
                return crossings;
            };

            int crossings60 = countZeroCrossings(buf60, 128);
            int crossings72 = countZeroCrossings(buf72, 128);

            // Octave up should have roughly twice as many zero crossings
            expect(crossings72 > crossings60, "Higher note should have more zero crossings");
        }

        // -- Slice mode: pad index selects slice --
        beginTest("Slice mode: different slices give different audio");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 64);

            // Create sample with two distinct halves
            std::vector<float> data(1024);
            for (int i = 0; i < 512; ++i)
                data[i] = 0.5f;  // first half: constant
            for (int i = 512; i < 1024; ++i)
                data[i] = -0.5f; // second half: constant negative

            samp.loadSample(data.data(), 1024, 1, 44100.0);
            samp.sliceByGrid(2);

            expectEquals(samp.getSliceCount(), 2);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 1.0f);

            // Slice 0
            samp.setParameter(SamplerEngine::SliceSelect, 0.0f);
            samp.noteOn(60, 1.0f, -1);
            juce::AudioBuffer<float> buf0(2, 32);
            buf0.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf0, midi);
            samp.allNotesOff();

            // Slice 1
            samp.setParameter(SamplerEngine::SliceSelect, 1.0f);
            samp.noteOn(60, 1.0f, -1);
            juce::AudioBuffer<float> buf1(2, 32);
            buf1.clear();
            samp.processBlock(buf1, midi);

            // Output should differ because slice contents differ
            bool differ = false;
            for (int i = 2; i < 32; ++i)
            {
                if (std::abs(buf0.getSample(0, i) - buf1.getSample(0, i)) > 0.01f)
                {
                    differ = true;
                    break;
                }
            }
            expect(differ, "Different slices should produce different audio");
        }

        // -- Velocity to filter cutoff --
        beginTest("Velocity to filter cutoff mapping");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 256);

            // Bright sample (full-range noise approximation)
            std::vector<float> data(4410);
            uint32_t seed = 12345;
            for (int i = 0; i < 4410; ++i)
            {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                data[i] = (float)(seed & 0x7FFFFFFF) / 2147483648.0f * 2.0f - 1.0f;
            }
            samp.loadSample(data.data(), 4410, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);
            samp.setParameter(SamplerEngine::FilterCutoff, 0.3f);
            samp.setParameter(SamplerEngine::VelToFilter, 0.7f);

            // Low velocity => darker sound (lower cutoff)
            samp.noteOn(60, 0.1f, -1);
            juce::AudioBuffer<float> lowVelBuf(2, 256);
            lowVelBuf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(lowVelBuf, midi);
            samp.allNotesOff();

            // High velocity => brighter sound (higher cutoff)
            samp.noteOn(60, 1.0f, -1);
            juce::AudioBuffer<float> highVelBuf(2, 256);
            highVelBuf.clear();
            samp.processBlock(highVelBuf, midi);

            // High velocity output should have more high-frequency energy
            // Compare sum of absolute differences between adjacent samples (rough HF measure)
            float lowVelHF = 0.0f, highVelHF = 0.0f;
            for (int i = 1; i < 256; ++i)
            {
                lowVelHF += std::abs(lowVelBuf.getSample(0, i) - lowVelBuf.getSample(0, i-1));
                highVelHF += std::abs(highVelBuf.getSample(0, i) - highVelBuf.getSample(0, i-1));
            }

            expect(highVelHF > lowVelHF, "High velocity should be brighter (more HF energy)");
        }

        // -- Start/end points --
        beginTest("Start and end points limit playback region");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 32);

            // Ascending ramp from 0 to 1
            std::vector<float> ramp(1024);
            for (int i = 0; i < 1024; ++i)
                ramp[i] = (float)i / 1024.0f;
            samp.loadSample(ramp.data(), 1024, 1, 44100.0);

            // Set start at 50%, end at 75%
            samp.setParameter(SamplerEngine::StartPoint, 0.5f);
            samp.setParameter(SamplerEngine::EndPoint, 0.75f);
            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::Volume, 1.0f);

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 32);
            buf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf, midi);

            float sum = 0.0f;
            for (int i = 0; i < 32; ++i) sum += std::abs(buf.getSample(0, i));
            expect(sum > 0.0f, "Start point should produce non-silent output");
        }

        // -- Playhead normalized --
        beginTest("Playhead normalized returns valid position during playback");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 128);

            std::vector<float> data(4410, 0.5f);
            samp.loadSample(data.data(), 4410, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);

            // Before note on, no active voice
            float ph = samp.getPlayheadNormalized();
            expect(ph < 0.0f, "No playhead before note on");

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 128);
            buf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf, midi);

            ph = samp.getPlayheadNormalized();
            expect(ph >= 0.0f && ph <= 1.0f, "Playhead should be 0..1 during playback");
        }

        // -- Sample pool ID --
        beginTest("Sample pool ID default and setter");
        {
            SamplerEngine samp;
            expectEquals(samp.getSamplePoolId(), -1);
            samp.setSamplePoolId(42);
            expectEquals(samp.getSamplePoolId(), 42);
        }

        // -- hasSample --
        beginTest("hasSample returns correct state");
        {
            SamplerEngine samp;
            // A new sampler starts on the bank's sustaining tone so the track makes sound at once.
            expect(samp.hasSample(), "Factory sample loaded by default");
            expectEquals(samp.getBankIndex(), SampleBank::get().defaultIndex());

            std::vector<float> data(100, 0.5f);
            samp.loadSample(data.data(), 100, 1, 44100.0);
            expect(samp.hasSample(), "Sample should be loaded");
        }
    }
};

static SamplerFullTests samplerFullTests;


// ============================================================================
//  SamplePool Tests
// ============================================================================
class SamplePoolTests : public juce::UnitTest
{
public:
    SamplePoolTests() : juce::UnitTest("Sample Pool") {}

    void runTest() override
    {
        beginTest("Add and retrieve a mono sample");
        {
            SamplePool pool;
            std::vector<float> data(1000, 0.5f);
            int id = pool.addSample("kick", data.data(), 1000, 44100.0);
            expect(id > 0, "Should return valid ID");

            auto* entry = pool.getSample(id);
            expect(entry != nullptr, "Should find sample by ID");
            expectEquals(entry->name, juce::String("kick"));
            expectEquals(entry->lengthInSamples, 1000);
            expectEquals(entry->channels, 1);
            expect(!entry->isStereo(), "Should be mono");
        }

        beginTest("Add stereo sample");
        {
            SamplePool pool;
            std::vector<float> dataL(500, 0.3f);
            std::vector<float> dataR(500, -0.3f);
            int id = pool.addSampleStereo("pad", dataL.data(), dataR.data(), 500, 44100.0);
            expect(id > 0);

            auto* entry = pool.getSample(id);
            expect(entry != nullptr);
            expect(entry->isStereo(), "Should be stereo");
            expectEquals(entry->channels, 2);
        }

        beginTest("Add interleaved stereo sample");
        {
            SamplePool pool;
            std::vector<float> interleaved(200);
            for (int i = 0; i < 100; ++i)
            {
                interleaved[i * 2] = 0.5f;
                interleaved[i * 2 + 1] = -0.5f;
            }
            int id = pool.addSampleStereoInterleaved("hat", interleaved.data(), 100, 48000.0);
            expect(id > 0);
            auto* e = pool.getSample(id);
            expect(e != nullptr);
            expect(e->isStereo());
            expectEquals((int) e->dataL.size(), 100);
        }

        beginTest("Remove sample with no references");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            int id = pool.addSample("temp", data.data(), 100, 44100.0);
            expect(pool.removeSample(id));
            expect(pool.getSample(id) == nullptr, "Should be gone");
        }

        beginTest("Cannot remove referenced sample");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            int id = pool.addSample("used", data.data(), 100, 44100.0);
            pool.addRef(id);
            expect(!pool.removeSample(id), "Should refuse removal while referenced");
            pool.releaseRef(id);
            expect(pool.removeSample(id), "Should remove after releasing ref");
        }

        beginTest("Force remove ignores ref count");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            int id = pool.addSample("forced", data.data(), 100, 44100.0);
            pool.addRef(id);
            expect(pool.forceRemoveSample(id));
            expect(pool.getSample(id) == nullptr);
        }

        beginTest("Reference counting");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            int id = pool.addSample("reftest", data.data(), 100, 44100.0);
            expectEquals(pool.getRefCount(id), 0);
            pool.addRef(id);
            expectEquals(pool.getRefCount(id), 1);
            pool.addRef(id);
            expectEquals(pool.getRefCount(id), 2);
            pool.releaseRef(id);
            expectEquals(pool.getRefCount(id), 1);
        }

        beginTest("Rename sample");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            int id = pool.addSample("old_name", data.data(), 100, 44100.0);
            expect(pool.renameSample(id, "new_name"));
            expectEquals(pool.getSample(id)->name, juce::String("new_name"));
        }

        beginTest("Find by name");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            int id = pool.addSample("findme", data.data(), 100, 44100.0);
            expectEquals(pool.findByName("findme"), id);
            expectEquals(pool.findByName("nope"), -1);
        }

        beginTest("Pool size tracking");
        {
            SamplePool pool;
            expect(pool.getTotalBytes() == 0);

            std::vector<float> data(1000, 0.5f);
            pool.addSample("s1", data.data(), 1000, 44100.0);
            expect(pool.getTotalBytes() == 1000 * sizeof(float));

            std::vector<float> dataL(500, 0.3f);
            std::vector<float> dataR(500, -0.3f);
            pool.addSampleStereo("s2", dataL.data(), dataR.data(), 500, 44100.0);
            expect(pool.getTotalBytes() == (1000 + 500 + 500) * sizeof(float));
        }

        beginTest("Eviction of unreferenced samples");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            int id1 = pool.addSample("first", data.data(), 100, 44100.0);
            int id2 = pool.addSample("second", data.data(), 100, 44100.0);

            // Reference id2
            pool.addRef(id2);

            // Evict to free up space
            int evicted = pool.evictUntilSpace(pool.getMaxBytes());
            expect(evicted >= 1, "Should evict at least one");
            expect(pool.getSample(id1) == nullptr, "Unreferenced should be evicted first");
            expect(pool.getSample(id2) != nullptr, "Referenced should survive");
            pool.releaseRef(id2);
        }

        beginTest("hasSpaceFor reports correctly");
        {
            SamplePool pool;
            expect(pool.hasSpaceFor(1000, 1));
            expect(pool.hasSpaceFor(1000, 2));
        }

        beginTest("Serialization round-trip");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.42f);
            int id = pool.addSample("roundtrip", data.data(), 100, 44100.0);
            pool.addRef(id);

            auto serialized = pool.toVar();

            // Save raw data
            juce::File tmpDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_pool_test");
            tmpDir.createDirectory();
            pool.saveRawData(tmpDir);

            // Load into new pool
            SamplePool pool2;
            expect(pool2.loadFromVar(serialized, tmpDir));
            expect(pool2.getSampleCount() == 1);
            auto* e = pool2.getSample(id);
            expect(e != nullptr);
            expectEquals(e->name, juce::String("roundtrip"));
            expectEquals(e->lengthInSamples, 100);
            expectEquals(e->refCount, 1);

            // Verify data matches
            bool dataMatch = true;
            for (int i = 0; i < 100; ++i)
            {
                if (std::abs(e->dataL[i] - 0.42f) > 0.001f)
                {
                    dataMatch = false;
                    break;
                }
            }
            expect(dataMatch, "Sample data should survive round-trip");

            tmpDir.deleteRecursively();
        }

        beginTest("Clear pool");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            pool.addSample("a", data.data(), 100, 44100.0);
            pool.addSample("b", data.data(), 100, 44100.0);
            expectEquals(pool.getSampleCount(), 2);
            pool.clear();
            expectEquals(pool.getSampleCount(), 0);
            expect(pool.getTotalBytes() == 0);
        }

        beginTest("Duration calculation");
        {
            SamplePool pool;
            std::vector<float> data(44100, 0.5f);
            int id = pool.addSample("onesec", data.data(), 44100, 44100.0);
            auto* e = pool.getSample(id);
            expect(std::abs(e->durationSeconds() - 1.0) < 0.001);
        }

        beginTest("getAllSamples returns all entries");
        {
            SamplePool pool;
            std::vector<float> data(100, 0.5f);
            pool.addSample("a", data.data(), 100, 44100.0);
            pool.addSample("b", data.data(), 100, 44100.0);
            pool.addSample("c", data.data(), 100, 44100.0);
            expectEquals((int) pool.getAllSamples().size(), 3);
        }
    }
};

static SamplePoolTests samplePoolTests;




