#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"
#include "../filters/SVFilter.h"

// Wah: a resonant band-pass swept by the input's envelope (auto-wah) and / or a sine LFO.
// Structure: Zoelzer, "DAFX", filters chapter (wah-wah = swept band-pass) with a peak envelope
// follower; the filter is the project's trapezoidal state-variable filter (SVFilter.h, Simper /
// Zavalishin), which stays tuned and stable while its cutoff is modulated every sample.
//
// FREQ  resting centre frequency
// RESO  0..1 -> Q 1..12 (output scaled so the peak gain is sqrt(Q): about 0 to +11 dB)
// SENS  envelope amount: a full-level input raises the centre by up to 4 octaves
// RATE  LFO rate; 0 = LFO off, otherwise the centre swings +/- one octave
class Wah : public EffectBase
{
public:
    enum Param { Freq = 0, Reso, Sens, Rate };

    Wah() { prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "FREQ", 0, 150.0f, 2500.0f, 400.0f, K::Log, "Hz" };
            case 1: return { "RESO", 1, 0, 1, 0.5f };
            case 2: return { "SENS", 2, 0, 1, 0.5f };
            case 3: return { "RATE", 3, 0.0f, 10.0f, 0.0f, K::Linear, "Hz" };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Freq: freq = std::clamp(value, 150.0f, 2500.0f); break;
            case Reso: reso = std::clamp(value, 0.0f, 1.0f); break;
            case Sens: sens = std::clamp(value, 0.0f, 1.0f); break;
            case Rate: rate = std::clamp(value, 0.0f, 10.0f); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Freq: return freq;
            case Reso: return reso;
            case Sens: return sens;
            case Rate: return rate;
            default:   return 0.0f;
        }
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        for (auto* f : { &filterL, &filterR })
        {
            f->setSampleRate(sr);
            f->setMode(SVFilter::Mode::BandPass);
        }
        follower.setTimes(sr, 3.0f, 150.0f);
        freqSmooth.setTime(sr, 20.0f);
        lfoDepthSmooth.setTime(sr, 30.0f);
        reset();
    }

    void reset() override
    {
        filterL.reset();
        filterR.reset();
        follower.reset();
        freqSmooth.snap(freq);
        lfoDepthSmooth.snap(rate > LFO_OFF ? 1.0f : 0.0f);
        lfoPhase = 0.0;
        quietCount = 0;
    }

    // Centre frequency the filter is at right now (after envelope and LFO), for meters and tests.
    float getCurrentFrequency() const { return currentHz; }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        float env = std::min(1.0f, 2.0f * follower.process(std::max(std::abs(left), std::abs(right))));

        float octaves = sens * ENV_OCTAVES * env;
        float lfoDepth = lfoDepthSmooth.next(rate > LFO_OFF ? 1.0f : 0.0f);
        if (lfoDepth > 0.0f)
        {
            octaves += lfoDepth * (float) std::sin(lfoPhase * dsp::TWO_PI_D);
            lfoPhase += (double) rate / sampleRate;
            if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        }

        float hz = freqSmooth.next(freq) * std::exp2(octaves);
        hz = std::clamp(hz, 60.0f, std::min(12000.0f, (float) sampleRate * 0.45f));
        currentHz = hz;

        float q = std::pow(12.0f, reso);
        float gain = 1.0f / std::sqrt(q);   // band output peaks at Q; leave sqrt(Q) of it

        filterL.setResonance(q);
        filterR.setResonance(q);
        filterL.setCutoff(hz);
        filterR.setCutoff(hz);

        float inL = left, inR = right;
        left = filterL.process(inL) * gain;
        right = filterR.process(inR) * gain;

        // The filter's states are private: once input and output have both been at nothing for a
        // while, clear them so a ringing tail cannot linger in the denormal range.
        if (std::abs(inL) + std::abs(inR) + std::abs(left) + std::abs(right) < 1.0e-12f)
        {
            if (++quietCount >= QUIET_SAMPLES)
            {
                filterL.reset();
                filterR.reset();
                left = right = 0.0f;
                quietCount = QUIET_SAMPLES;
            }
        }
        else
        {
            quietCount = 0;
        }
    }

private:
    static constexpr float ENV_OCTAVES = 4.0f;
    static constexpr float LFO_OFF = 0.005f;
    static constexpr int QUIET_SAMPLES = 64;

    float freq = 400.0f;
    float reso = 0.5f;
    float sens = 0.5f;
    float rate = 0.0f;

    SVFilter filterL, filterR;
    fxb::EnvelopeFollower follower;
    fxb::Smoother freqSmooth, lfoDepthSmooth;
    double lfoPhase = 0.0;
    float currentHz = 400.0f;
    int quietCount = 0;
};
