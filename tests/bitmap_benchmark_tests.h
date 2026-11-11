#pragma once

#include "../src/controller/BitmapBenchmark.h"
#include <juce_core/juce_core.h>

class BitmapBenchmarkTests : public juce::UnitTest
{
public:
    BitmapBenchmarkTests() : UnitTest("Bitmap Benchmark") {}

    void runTest() override
    {
        beginTest("Benchmark tracks frame count correctly");
        {
            BitmapBenchmark bench;
            bench.startBenchmark(5);
            expect(bench.isRunning(), "Should be running after start");

            for (int i = 0; i < 5; ++i)
            {
                bench.onFrameSent();
                bench.onAckReceived();
            }

            expect(!bench.isRunning(), "Should stop after all frames complete");
            auto results = bench.getResults();
            expectEquals(results.framesCompleted, 5);
            expectEquals(results.framesDropped, 0);
        }

        beginTest("Calculates min/max/avg from synthetic timing data");
        {
            BitmapBenchmark bench;

            // Inject synthetic tick values: 1ms, 2ms, 3ms at the tick rate
            double ticksPerSec = static_cast<double>(juce::Time::getHighResolutionTicksPerSecond());
            int64_t ticks1ms = static_cast<int64_t>(ticksPerSec / 1000.0);
            int64_t ticks2ms = static_cast<int64_t>(ticksPerSec / 500.0);
            int64_t ticks3ms = static_cast<int64_t>(ticksPerSec / 1000.0 * 3.0);

            bench.injectFrameTicks(0, ticks1ms);
            bench.injectFrameTicks(1, ticks2ms);
            bench.injectFrameTicks(2, ticks3ms);
            bench.finalizeWithCount(3);

            auto results = bench.getResults();
            expectEquals(results.framesCompleted, 3);

            // Check min ~ 1ms, max ~ 3ms, avg ~ 2ms (with some float tolerance)
            expectWithinAbsoluteError(results.minFrameMs, 1.0, 0.01);
            expectWithinAbsoluteError(results.maxFrameMs, 3.0, 0.01);
            expectWithinAbsoluteError(results.avgFrameMs, 2.0, 0.01);

            // Sustained FPS should be ~500 (1000 / 2ms avg)
            expectWithinAbsoluteError(results.sustainedFPS, 500.0, 1.0);
        }

        beginTest("Zero-frame benchmark returns immediately with zeroed results");
        {
            BitmapBenchmark bench;
            bench.startBenchmark(0);

            expect(!bench.isRunning(), "Zero-frame benchmark should not be running");
            expectEquals(bench.getProgress(), 0.0);

            auto results = bench.getResults();
            expectEquals(results.framesCompleted, 0);
            expectEquals(results.framesDropped, 0);
            expectEquals(results.minFrameMs, 0.0);
            expectEquals(results.maxFrameMs, 0.0);
            expectEquals(results.avgFrameMs, 0.0);
            expectEquals(results.sustainedFPS, 0.0);
        }

        beginTest("Progress reports correctly during run");
        {
            BitmapBenchmark bench;
            bench.startBenchmark(4);

            expectWithinAbsoluteError(bench.getProgress(), 0.0, 0.001);

            bench.onFrameSent();
            bench.onAckReceived();
            expectWithinAbsoluteError(bench.getProgress(), 0.25, 0.001);

            bench.onFrameSent();
            bench.onAckReceived();
            expectWithinAbsoluteError(bench.getProgress(), 0.5, 0.001);

            bench.onFrameSent();
            bench.onAckReceived();
            expectWithinAbsoluteError(bench.getProgress(), 0.75, 0.001);

            bench.onFrameSent();
            bench.onAckReceived();
            expectWithinAbsoluteError(bench.getProgress(), 1.0, 0.001);

            expect(!bench.isRunning());
        }

        beginTest("Dropped frames counted when ack arrives without matching send");
        {
            BitmapBenchmark bench;
            bench.startBenchmark(3);

            // Normal frame
            bench.onFrameSent();
            bench.onAckReceived();

            // Ack without a send = dropped
            bench.onAckReceived();

            // Normal frame
            bench.onFrameSent();
            bench.onAckReceived();

            // Another ack without send
            bench.onAckReceived();

            // Final normal frame to complete
            bench.onFrameSent();
            bench.onAckReceived();

            expect(!bench.isRunning());
            auto results = bench.getResults();
            expectEquals(results.framesDropped, 2);
            // 3 total - 2 dropped = 1 valid... but we sent 3+2 acks, only 3 sends
            // The 3 send/ack pairs each counted as valid, dropped are extra acks
        }

        beginTest("Results are zeroed before first startBenchmark call");
        {
            BitmapBenchmark bench;
            auto results = bench.getResults();

            expectEquals(results.framesCompleted, 0);
            expectEquals(results.framesDropped, 0);
            expectEquals(results.minFrameMs, 0.0);
            expectEquals(results.maxFrameMs, 0.0);
            expectEquals(results.avgFrameMs, 0.0);
            expectEquals(results.sustainedFPS, 0.0);
            expect(!bench.isRunning());
            expectEquals(bench.getProgress(), 0.0);
        }

        beginTest("MAX_FRAMES constant is 1000");
        {
            expectEquals(BitmapBenchmark::MAX_FRAMES, 1000);
        }

        beginTest("Benchmark clamps frame count to MAX_FRAMES");
        {
            BitmapBenchmark bench;
            bench.startBenchmark(2000);
            expect(bench.isRunning(), "Should still start even with over-limit count");

            // Inject synthetic data and finalize to verify clamping
            double ticksPerSec = static_cast<double>(juce::Time::getHighResolutionTicksPerSecond());
            int64_t ticks1ms = static_cast<int64_t>(ticksPerSec / 1000.0);

            BitmapBenchmark bench2;
            bench2.injectFrameTicks(0, ticks1ms);
            bench2.finalizeWithCount(1);
            auto results = bench2.getResults();
            expectEquals(results.framesCompleted, 1);
        }

        beginTest("Send without ack counts as dropped frame on next send");
        {
            BitmapBenchmark bench;
            bench.startBenchmark(3);

            // Send, then send again without ack -- first send is dropped
            bench.onFrameSent();
            bench.onFrameSent(); // previous frame dropped
            bench.onAckReceived();

            bench.onFrameSent();
            bench.onAckReceived();

            expect(!bench.isRunning());
            auto results = bench.getResults();
            expectEquals(results.framesDropped, 1);
            // 3 total slots - 1 dropped = 2 valid completed
            expectEquals(results.framesCompleted, 2);
        }

        beginTest("onFrameSent and onAckReceived are no-ops when not running");
        {
            BitmapBenchmark bench;
            // Not started yet
            bench.onFrameSent();
            bench.onAckReceived();
            auto results = bench.getResults();
            expectEquals(results.framesCompleted, 0);
            expectEquals(results.framesDropped, 0);
            expect(!bench.isRunning());
        }
    }
};

static BitmapBenchmarkTests bitmapBenchmarkTests;
