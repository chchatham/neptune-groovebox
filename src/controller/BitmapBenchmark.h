#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

/**
 * BitmapBenchmark — measures OLED bitmap round-trip timing.
 *
 * Tracks per-frame send-to-ack latency and computes sustained frame rate
 * over a configurable number of frames (max 1000).
 *
 * Timing uses juce::Time high-resolution ticks internally (int64_t).
 * Millisecond conversion happens only when producing results.
 */
class BitmapBenchmark
{
public:
    static constexpr int MAX_FRAMES = 1000;

    struct BenchmarkResults
    {
        double minFrameMs   = 0.0;
        double maxFrameMs   = 0.0;
        double avgFrameMs   = 0.0;
        double sustainedFPS = 0.0;
        int framesCompleted = 0;
        int framesDropped   = 0;
    };

    BitmapBenchmark() = default;

    /** Begin a benchmark run of numFrames frames (clamped to 0..MAX_FRAMES). */
    void startBenchmark(int numFrames)
    {
        results = {};
        frameIndex = 0;
        totalFrames = juce::jlimit(0, MAX_FRAMES, numFrames);
        hasPendingSend = false;

        if (totalFrames == 0)
        {
            running = false;
            return;
        }

        running = true;
        benchmarkStartTicks = juce::Time::getHighResolutionTicks();
    }

    /** Record the moment a bitmap SysEx is sent. */
    void onFrameSent()
    {
        if (!running)
            return;

        // If there was already a pending send without an ack, that frame was dropped.
        if (hasPendingSend)
        {
            results.framesDropped++;
            // Count it as a completed cycle (dropped, but consumed a slot)
            frameIndex++;
            if (frameIndex >= totalFrames)
            {
                finalize();
                return;
            }
        }

        lastSendTicks = juce::Time::getHighResolutionTicks();
        hasPendingSend = true;
    }

    /** Record the moment an ack is received. */
    void onAckReceived()
    {
        if (!running)
            return;

        // Ack without a matching send — count as a dropped frame
        if (!hasPendingSend)
        {
            results.framesDropped++;
            return;
        }

        int64_t ackTicks = juce::Time::getHighResolutionTicks();
        int64_t elapsed = ackTicks - lastSendTicks;

        // Store the tick duration for this frame
        if (frameIndex < MAX_FRAMES)
            frameTicks[static_cast<size_t>(frameIndex)] = elapsed;

        hasPendingSend = false;
        frameIndex++;

        if (frameIndex >= totalFrames)
            finalize();
    }

    /** True while a benchmark is in progress. */
    bool isRunning() const { return running; }

    /** Returns progress as fraction: framesCompleted / totalFrames. */
    double getProgress() const
    {
        if (totalFrames <= 0)
            return 0.0;
        return static_cast<double>(frameIndex) / static_cast<double>(totalFrames);
    }

    /** Returns the results struct. Valid after isRunning() returns false. */
    BenchmarkResults getResults() const { return results; }

    /** Inject a synthetic frame time for testing (ticks). Does not advance state. */
    void injectFrameTicks(int index, int64_t ticks)
    {
        if (index >= 0 && index < MAX_FRAMES)
            frameTicks[static_cast<size_t>(index)] = ticks;
    }

    /** Force-finalize with a given frame count for testing. */
    void finalizeWithCount(int count)
    {
        totalFrames = juce::jlimit(0, MAX_FRAMES, count);
        frameIndex = totalFrames;
        running = true;
        benchmarkStartTicks = 0;
        finalize();
    }

private:
    void finalize()
    {
        running = false;
        int validFrames = frameIndex - results.framesDropped;
        results.framesCompleted = validFrames;

        if (validFrames <= 0)
        {
            results.minFrameMs = 0.0;
            results.maxFrameMs = 0.0;
            results.avgFrameMs = 0.0;
            results.sustainedFPS = 0.0;
            return;
        }

        double ticksPerSecond = static_cast<double>(juce::Time::getHighResolutionTicksPerSecond());
        double ticksToMs = 1000.0 / ticksPerSecond;

        int64_t minTicks = frameTicks[0];
        int64_t maxTicks = frameTicks[0];
        int64_t totalTicks = 0;
        int counted = 0;

        for (int i = 0; i < frameIndex && counted < validFrames; ++i)
        {
            int64_t t = frameTicks[static_cast<size_t>(i)];
            if (t <= 0)
                continue; // skip dropped frames (they have 0 ticks)
            if (counted == 0)
            {
                minTicks = t;
                maxTicks = t;
            }
            else
            {
                if (t < minTicks) minTicks = t;
                if (t > maxTicks) maxTicks = t;
            }
            totalTicks += t;
            counted++;
        }

        results.minFrameMs = static_cast<double>(minTicks) * ticksToMs;
        results.maxFrameMs = static_cast<double>(maxTicks) * ticksToMs;
        results.avgFrameMs = (static_cast<double>(totalTicks) * ticksToMs) / static_cast<double>(counted);

        if (results.avgFrameMs > 0.0)
            results.sustainedFPS = 1000.0 / results.avgFrameMs;
        else
            results.sustainedFPS = 0.0;
    }

    // Pre-allocated storage for frame tick durations (no heap allocation on measurement path)
    std::array<int64_t, MAX_FRAMES> frameTicks {};

    int64_t benchmarkStartTicks = 0;
    int64_t lastSendTicks = 0;

    int totalFrames = 0;
    int frameIndex = 0;
    bool running = false;
    bool hasPendingSend = false;

    BenchmarkResults results {};
};
