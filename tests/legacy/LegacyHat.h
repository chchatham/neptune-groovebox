#pragma once

#include "../../src/dsp/DSPUtils.h"
#include "../../src/dsp/Envelope.h"
#include "../../src/dsp/filters/SVFilter.h"
#include <algorithm>
#include <vector>

// The closed and the open hat as the drum voice made them before they became sounds of the
// oscillator bank (DrumVoice.h at commit b4ee701: processClosedHat, its envelopes and its strike,
// moved here line for line). Kept for the tests that measure the bank's HAT and OHAT against it.
struct LegacyHat
{
    static constexpr float HAT_MAKEUP = 3.0f;
    static constexpr float HAT_CLOSE_SECONDS = 0.045f;

    double sampleRate = 48000.0;
    bool open = false;
    float tune = 8000.0f, decay = 0.04f, tone = 0.5f, level = 0.8f;
    Envelope ampEnv;
    SVFilter filter, hatBandHigh, hatBandLow;
    double hatPhase[6] = {};
    float velocity = 1.0f;

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        filter.setSampleRate(sr);
        for (auto* band : { &hatBandHigh, &hatBandLow })
        {
            band->setSampleRate(sr);
            band->setMode(SVFilter::Mode::BandPass);
            band->setResonance(1.6f);
        }
        hatBandHigh.setCutoff(7100.0f);
        hatBandLow.setCutoff(3440.0f);
    }

    void noteOn(float vel)
    {
        velocity = std::clamp(vel, 0.0f, 1.0f);
        filter.reset();
        static constexpr double hatStart[6] = { 0.0, 0.31, 0.62, 0.17, 0.83, 0.45 };
        for (int i = 0; i < 6; ++i) hatPhase[i] = hatStart[i];
        hatBandHigh.reset();
        hatBandLow.reset();
        Envelope::Params ampP;
        if (! open)
        {
            ampP.attack = 0.0f;
            ampP.decay = 0.03f + decay * 0.25f;
            ampP.sustain = 0.0f;
            ampP.release = 0.005f;
            ampP.decayCurve = -0.3f;
        }
        else
        {
            ampP.attack = 0.0f;
            ampP.decay = 0.1f + decay * 0.9f;
            ampP.sustain = 0.0f;
            ampP.release = 0.02f;
            ampP.decayCurve = -0.3f;
        }
        ampEnv.setParams(ampP);
        ampEnv.noteOn();
        filter.setMode(SVFilter::Mode::HighPass);
        filter.setCutoff(tone * 10000.0f + 500.0f);
        filter.setResonance(0.707f);
    }

    void noteOff()
    {
        if (! open)
            return;
        ampEnv.setRelease(HAT_CLOSE_SECONDS);
        ampEnv.noteOff();
    }

    float process()
    {
        float ampEnvVal = ampEnv.process();
        static constexpr double hz[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
        const double scale = std::clamp((double) tune / 8000.0, 0.125, 1.5);

        float sum = 0.0f;
        for (int i = 0; i < 6; ++i)
        {
            const double inc = hz[i] * scale / sampleRate;
            double& ph = hatPhase[i];
            ph += inc;
            if (ph >= 1.0) ph -= 1.0;
            float sq = ph < 0.5 ? 1.0f : -1.0f;
            sq += dsp::polyBLEP(ph, inc);
            double half = ph + 0.5;
            if (half >= 1.0) half -= 1.0;
            sq -= dsp::polyBLEP(half, inc);
            sum += sq;
        }
        sum *= (1.0f / 6.0f);

        const float metal = hatBandHigh.process(sum) + 0.35f * hatBandLow.process(sum);
        return filter.process(metal) * ampEnvVal * HAT_MAKEUP * level * velocity;
    }
};
