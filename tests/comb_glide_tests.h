#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>
#include "../src/dsp/filters/CombFilter.h"
#include "../src/dsp/filters/FilterSlot.h"

// The comb's cutoff sweep is smooth at low frequencies. Its delay used to jump to each new cutoff:
// one knob click at 20-40 Hz moved the read point by dozens of samples, a click and a step in pitch
// (measured 2026-11-11: output steps 45-55 times the steady ones at 25-40 Hz, 1.6 times at 1 kHz).
class CombGlideTests : public juce::UnitTest
{
public:
    CombGlideTests() : UnitTest("Comb Glide") {}

    static float input(int n)
    {
        const double t = n / 48000.0;
        return 0.3f * (float) (std::sin(6.2831853 * 110.0 * t) + 0.5 * std::sin(6.2831853 * 330.0 * t + 1.0));
    }

    // Largest sample-to-sample step of the output during a sweep of ten knob clicks (2.5 % each,
    // 30 ms apart), over the largest step of the settled sound before it.
    static float sweepRoughness(float startHz, float factor)
    {
        CombFilter c;
        c.setSampleRate(48000.0);
        c.setResonance(0.9f);
        c.setCutoff(startHz);
        float prev = 0.0f, steady = 0.0f, sweeping = 0.0f;
        int n = 0;
        for (; n < 48000; ++n)
        {
            const float y = c.process(input(n));
            if (n > 40000) steady = std::max(steady, std::abs(y - prev));
            prev = y;
        }
        float hz = startHz;
        for (int click = 0; click < 10; ++click)
        {
            hz *= factor;
            c.setCutoff(hz);
            for (int k = 0; k < 1440; ++k, ++n)
            {
                const float y = c.process(input(n));
                sweeping = std::max(sweeping, std::abs(y - prev));
                prev = y;
            }
        }
        return sweeping / steady;
    }

    // Samples from an impulse to its first echo.
    static int echoDelay(CombFilter& c, int maxSamples)
    {
        int best = 0;
        float bestAbs = 0.0f;
        c.process(1.0f);
        for (int n = 1; n < maxSamples; ++n)
        {
            const float y = std::abs(c.process(0.0f));
            if (y > bestAbs) { bestAbs = y; best = n; }
        }
        return best;
    }

    void runTest() override
    {
        beginTest("A slow sweep by knob clicks has no jumps, low or high, up or down");
        {
            for (float hz : { 22.0f, 25.0f, 40.0f, 60.0f, 250.0f, 1000.0f })
            {
                const float up = sweepRoughness(hz, 1.025f), down = sweepRoughness(hz * 1.3f, 1.0f / 1.025f);
                logMessage("comb sweep from " + juce::String(hz, 0) + " Hz: roughness up " + juce::String(up, 2) + ", down " + juce::String(down, 2));
                expect(up < 10.0f, "up from " + juce::String(hz) + " Hz: " + juce::String(up));
                expect(down < 10.0f, "down to " + juce::String(hz) + " Hz: " + juce::String(down));
            }
        }

        beginTest("After the glide the comb is tuned to the new cutoff");
        {
            CombFilter c;
            c.setSampleRate(48000.0);
            c.setResonance(0.5f);
            c.setCutoff(30.0f);
            for (int n = 0; n < 4800; ++n) c.process(input(n));
            c.setCutoff(100.0f);                                // 480 samples
            for (int n = 0; n < 48000; ++n) c.process(0.0f);    // glide over, loop rung out
            expectEquals(echoDelay(c, 2000), 480);
        }

        beginTest("A reset filter takes its cutoff at once: a new note is in tune from its first sample");
        {
            CombFilter c;
            c.setSampleRate(48000.0);
            c.setResonance(0.5f);
            c.setCutoff(30.0f);
            for (int n = 0; n < 4800; ++n) c.process(input(n));
            c.reset();
            c.setCutoff(200.0f);                                // 240 samples
            expectEquals(echoDelay(c, 2000), 240);

            FilterSlot slot;                                    // as a voice holds it
            slot.setSampleRate(48000.0);
            slot.setModel(0, FilterModel::Comb);
            slot.setResonance(0, 0.5f);
            slot.setCutoff(0, 30.0f);
            for (int n = 0; n < 4800; ++n) slot.process(input(n));
            slot.reset();
            slot.setCutoff(0, 400.0f);                          // 120 samples
            int best = 0; float bestAbs = 0.0f;
            slot.process(1.0f);
            for (int n = 1; n < 1000; ++n)
            {
                const float y = std::abs(slot.process(0.0f));
                if (y > bestAbs) { bestAbs = y; best = n; }
            }
            expectEquals(best, 120);
        }

        beginTest("High cutoffs still follow fast modulation: the glide is a millisecond there");
        {
            CombFilter c;
            c.setSampleRate(48000.0);
            c.setResonance(0.5f);
            c.setCutoff(2000.0f);
            for (int n = 0; n < 4800; ++n) c.process(input(n));
            c.setCutoff(4000.0f);                               // 12 samples
            for (int n = 0; n < 480; ++n) c.process(0.0f);      // 10 ms: arrived, and the short loop has rung out
            expectEquals(echoDelay(c, 200), 12);
        }
    }
};

static CombGlideTests combGlideTests;
