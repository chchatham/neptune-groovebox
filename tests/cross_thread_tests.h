#pragma once

// Cross-thread safety: the structure lock prevents the audio thread from reading step vectors
// while the MIDI thread modifies them (recordNote, setLength).

#include "../src/sequencer/SequencerCore.h"
#include "../src/engine/AudioEngine.h"
#include <thread>
#include <atomic>

class CrossThreadStepTests : public juce::UnitTest
{
public:
    CrossThreadStepTests() : UnitTest("Cross Thread Steps") {}

    void runTest() override
    {
        beginTest("A second thread editing lengths and recording notes while processBlock runs does not crash");
        {
            AudioEngine engine;
            SequencerCore seq;
            engine.setSequencer(&seq);
            seq.prepareToPlay(44100, 64);
            engine.prepare(44100, 64);

            // Wire the structure lock the same way main.cpp does.
            seq.setStructureLock([&engine](const std::function<void()>& work)
            {
                const AudioEngine::StructureLock lock(engine.structure());
                work();
            });

            seq.setRecording(true);
            seq.start();

            std::atomic<bool> stop { false };
            int edits = 0, blocks = 0;

            // A thread that edits step lengths and records notes, as the MIDI thread would.
            std::thread editor([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    // Vary the track length (causes steps.resize).
                    auto& track = seq.getPattern().getTrack(0);
                    {
                        const AudioEngine::StructureLock lock(engine.structure());
                        track.setLength(juce::jlimit(1, 128, track.length + 1));
                    }
                    // Record a note (pushes into step.notes).
                    seq.recordNote(0, 60 + (edits % 12), 0.8f);
                    ++edits;
                    // Rest so the playing thread is not starved.
                    std::this_thread::sleep_for(std::chrono::microseconds(300));
                }
            });

            // The audio thread plays blocks.
            float left[64], right[64];
            for (int i = 0; i < 5000; ++i)
            {
                std::memset(left, 0, sizeof(left));
                std::memset(right, 0, sizeof(right));
                engine.renderBlock(left, right, 64);
                seq.processBlock(64);
                ++blocks;
                // Rest so the editing thread gets time.
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }

            stop = true;
            editor.join();
            seq.stop();

            expect(edits > 100, "the editing thread ran: " + juce::String(edits) + " edits");
            expect(blocks == 5000, "the audio thread ran all blocks");
        }
    }
};

static CrossThreadStepTests crossThreadStepTests;
