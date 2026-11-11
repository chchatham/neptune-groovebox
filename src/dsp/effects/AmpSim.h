#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"
#include <array>

// Guitar-amp style model built from standard stages (the usual amp-simulation chain described in
// Zoelzer, "DAFX", nonlinear processing chapter, and Pakarinen & Yeh, "A Review of Digital
// Techniques for Modeling Vacuum-Tube Guitar Amplifiers", Computer Music Journal 2009):
//
//   input high-pass (tightens the bass before clipping)
//   -> pre-gain -> 2x oversampling
//   -> stage 1: tanh -> interstage one-pole low-pass at 6 kHz -> stage 2: biased tanh
//      (the bias moves the zero crossings, so the wave becomes asymmetric: even harmonics)
//   -> decimation -> DC blocker
//   -> tone: tilt made of a low shelf and a high shelf moving in opposite directions
//   -> cabinet: 2-pole high-pass with a small resonance bump at 90 Hz and a 4-pole low-pass
//   -> fixed -6 dB trim (a fully clipped signal then peaks near full scale) -> output level
//
// The biased second stage gives even harmonics at low and medium gain; at very high gain the
// first stage already squares the signal off and the odd harmonics dominate, as in a real
// high-gain amp.
//
// Every filter is an RBJ cookbook biquad (FxBlocks.h). The oversampling filters are 4th-order
// Butterworth low-passes (two biquads, Q 0.541 and 1.307) at 0.45 of the original sample rate.
//
// GAIN  0..1 -> 0..40 dB of pre-gain into the clipping stages
// TONE  -1..+1: dark (bass up, treble down) .. bright (bass down, treble up), +/- 9 dB
// CAB   speaker roll-off frequency
// LEVL  output level in dB
class AmpSim : public EffectBase
{
public:
    enum Param { Gain = 0, Tone, Cab, Level };

    AmpSim() { prepare(44100.0, 0); }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "GAIN", 0, 0, 1, 0.4f };
            case 1: return { "TONE", 1, -1, 1, 0.0f };
            case 2: return { "CAB", 2, 1500.0f, 10000.0f, 4500.0f, K::Log, "Hz" };
            case 3: return { "LEVL", 3, -24.0f, 12.0f, 0.0f, K::Linear, "dB" };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Gain:  gain = std::clamp(value, 0.0f, 1.0f); break;
            case Tone:  tone = std::clamp(value, -1.0f, 1.0f); break;
            case Cab:   cabHz = std::clamp(value, 1500.0f, 10000.0f); break;
            case Level: levelDb = std::clamp(value, -24.0f, 12.0f); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Gain:  return gain;
            case Tone:  return tone;
            case Cab:   return cabHz;
            case Level: return levelDb;
            default:    return 0.0f;
        }
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        for (auto& c : ch)
        {
            c.inputHp.setHighPass(sr, 80.0, 0.707);
            c.dcBlock.setHighPass(sr, 20.0, 0.707);
            c.cabHp.setHighPass(sr, 90.0, 1.1);
            c.interstage.setCutoff(sr * 2.0, 6000.0f);
            for (auto* stage : { &c.up, &c.down })
            {
                (*stage)[0].setLowPass(sr * 2.0, sr * 0.45, 0.5412);
                (*stage)[1].setLowPass(sr * 2.0, sr * 0.45, 1.3066);
            }
        }
        driveSmooth.setTime(sr, 20.0f);
        levelSmooth.setTime(sr, 20.0f);
        toneSmooth.setTime(sr, 30.0f);
        cabSmooth.setTime(sr, 30.0f);
        reset();
    }

    void reset() override
    {
        for (auto& c : ch)
        {
            c.inputHp.reset();
            c.dcBlock.reset();
            c.cabHp.reset();
            c.interstage.reset();
            c.toneLow.reset();
            c.toneHigh.reset();
            for (auto& b : c.up) b.reset();
            for (auto& b : c.down) b.reset();
            for (auto& b : c.cabLp) b.reset();
        }
        driveSmooth.snap(targetDrive());
        levelSmooth.snap(dsp::dbToLinear(levelDb));
        toneSmooth.snap(tone);
        cabSmooth.snap(cabHz);
        updateFilters(tone, cabHz);
        controlCounter = 0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        // Tone and cabinet coefficients follow their smoothed controls at control rate.
        float t = toneSmooth.next(tone);
        float c = cabSmooth.next(cabHz);
        if (--controlCounter <= 0)
        {
            controlCounter = CONTROL_INTERVAL;
            if (std::abs(t - appliedTone) > 1.0e-4f || std::abs(c - appliedCab) > 0.5f)
                updateFilters(t, c);
        }

        float drive = driveSmooth.next(targetDrive());
        float level = levelSmooth.next(dsp::dbToLinear(levelDb));
        float stage2 = 1.0f + 3.0f * gain;

        left = processChannel(ch[0], left, drive, stage2) * level;
        right = processChannel(ch[1], right, drive, stage2) * level;
    }

private:
    static constexpr int CONTROL_INTERVAL = 16;
    static constexpr float BIAS = 0.4f;
    static constexpr float OUTPUT_TRIM = 0.5f;   // a fully driven signal leaves at about full scale

    struct Channel
    {
        fxb::Biquad inputHp, dcBlock, toneLow, toneHigh, cabHp;
        std::array<fxb::Biquad, 2> up, down, cabLp;
        fxb::OnePole interstage;
    };

    float targetDrive() const { return dsp::dbToLinear(gain * 40.0f); }

    void updateFilters(float t, float cab)
    {
        appliedTone = t;
        appliedCab = cab;
        for (auto& c : ch)
        {
            c.toneLow.setLowShelf(sampleRate, 250.0, 0.707, -9.0 * t);
            c.toneHigh.setHighShelf(sampleRate, 2500.0, 0.707, 9.0 * t);
            c.cabLp[0].setLowPass(sampleRate, cab, 0.5412);
            c.cabLp[1].setLowPass(sampleRate, cab, 1.3066);
        }
    }

    static float clipStages(Channel& c, float x, float stage2)
    {
        static const float biasOut = std::tanh(BIAS);
        float y = c.interstage.lowPass(std::tanh(x));
        return std::tanh(y * stage2 + BIAS) - biasOut;   // asymmetric; exactly 0 for 0 in
    }

    static float processChannel(Channel& c, float x, float drive, float stage2)
    {
        x = c.inputHp.process(x) * drive;

        // 2x oversampling: zero-stuff (gain 2 restores the level), low-pass, clip, low-pass, keep
        // every other sample.
        float a = c.up[1].process(c.up[0].process(2.0f * x));
        float b = c.up[1].process(c.up[0].process(0.0f));
        a = clipStages(c, a, stage2);
        b = clipStages(c, b, stage2);
        float y = c.down[1].process(c.down[0].process(a));
        c.down[1].process(c.down[0].process(b));

        y = c.dcBlock.process(y);
        y = c.toneHigh.process(c.toneLow.process(y));
        y = c.cabHp.process(y);
        y = c.cabLp[1].process(c.cabLp[0].process(y));
        return y * OUTPUT_TRIM;
    }

    float gain = 0.4f;
    float tone = 0.0f;
    float cabHz = 4500.0f;
    float levelDb = 0.0f;

    std::array<Channel, 2> ch;
    fxb::Smoother driveSmooth, levelSmooth, toneSmooth, cabSmooth;
    float appliedTone = 0.0f;
    float appliedCab = 4500.0f;
    int controlCounter = 0;
};
