#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"
#include "EqBand.h"
#include "../Saturation.h"
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
// Every filter is an RBJ cookbook biquad (FxBlocks.h). The tone stack is two fxb::EqBand (EqBand.h,
// the equalisers' band): at TONE 0 it is not run, and a turn of TONE glides. The oversampling filters are 4th-order
// Butterworth low-passes (two biquads, Q 0.541 and 1.307) at 0.45 of the original sample rate.
//
// GAIN  0..1 -> 0..40 dB of pre-gain into the clipping stages
// TONE  -1..+1: dark (bass up, treble down) .. bright (bass down, treble up), +/- 9 dB
// CAB   speaker roll-off frequency
// LEVL  output level in dB
// SAT   (second page) the curve of both clipping stages: any of src/dsp/Saturation.h, TANH to start with
class AmpSim : public EffectBase
{
public:
    enum Param { Gain = 0, Tone, Cab, Level };

    // (AMP has no mix of its own: it gives its wet sound alone, so a chain's mix is its WET.)
    AmpSim() { setWetOnly(true); stage2Curve.set(sat::Tanh, BIAS); prepare(44100.0, 0); }

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

    ParamSpec getExtraSpec(int index) const override
    {
        if (index != 0) return {};
        return { "SAT", 0, 0, (float) (sat::Count - 1), (float) sat::Tanh, ParamKind::Choice, "", sat::names(), sat::Count };
    }
    void setExtra(int index, float value) override
    {
        if (index == 0 && std::isfinite(value)) stage2Curve.set((int) std::lround(value), BIAS);
    }
    float getExtra(int index) const override { return index == 0 ? (float) stage2Curve.curve : 0.0f; }

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
        toneLow.setShape(fxb::EqBand::LowShelf);
        toneHigh.setShape(fxb::EqBand::HighShelf);
        toneLow.prepare(sr);
        toneHigh.prepare(sr);
        toneLow.set(250.0f, 0.707f, -9.0f * tone);
        toneHigh.set(2500.0f, 0.707f, 9.0f * tone);
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
            for (auto& b : c.up) b.reset();
            for (auto& b : c.down) b.reset();
            for (auto& b : c.cabLp) b.reset();
        }
        driveSmooth.snap(targetDrive());
        levelSmooth.snap(dsp::dbToLinear(levelDb));
        toneLow.reset();
        toneHigh.reset();
        cabSmooth.snap(cabHz);
        updateFilters(cabHz);
        controlCounter = 0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        // The tone stack: a tilt, the two shelves moving in opposite directions (they glide by themselves).
        toneLow.setGain(-9.0f * tone);
        toneHigh.setGain(9.0f * tone);

        // The cabinet's coefficients follow their smoothed control at control rate.
        float c = cabSmooth.next(cabHz);
        if (--controlCounter <= 0)
        {
            controlCounter = CONTROL_INTERVAL;
            if (std::abs(c - appliedCab) > 0.5f)
                updateFilters(c);
        }

        float drive = driveSmooth.next(targetDrive());
        float level = levelSmooth.next(dsp::dbToLinear(levelDb));
        float stage2 = 1.0f + 3.0f * gain;

        const sat::Biased curve = stage2Curve;
        left = drivenChannel(ch[0], left, drive, stage2, curve);
        right = drivenChannel(ch[1], right, drive, stage2, curve);
        toneLow.process(left, right);
        toneHigh.process(left, right);
        left = cabinet(ch[0], left) * level;
        right = cabinet(ch[1], right) * level;
    }

private:
    static constexpr int CONTROL_INTERVAL = 16;
    static constexpr float BIAS = 0.4f;
    static constexpr float OUTPUT_TRIM = 0.5f;   // a fully driven signal leaves at about full scale

    struct Channel
    {
        fxb::Biquad inputHp, dcBlock, cabHp;
        std::array<fxb::Biquad, 2> up, down, cabLp;
        fxb::OnePole interstage;
    };

    float targetDrive() const { return dsp::dbToLinear(gain * 40.0f); }

    void updateFilters(float cab)
    {
        appliedCab = cab;
        for (auto& c : ch)
        {
            c.cabLp[0].setLowPass(sampleRate, cab, 0.5412);
            c.cabLp[1].setLowPass(sampleRate, cab, 1.3066);
        }
    }

    static float clipStages(Channel& c, float x, float stage2, const sat::Biased& curve)
    {
        float y = c.interstage.lowPass(sat::shape(curve.curve, x));
        return curve(y * stage2);                       // worked off-centre: asymmetric; exactly 0 for 0 in
    }

    // Up to the tone stack: input high-pass, the clipping stages at twice the rate, DC blocker.
    static float drivenChannel(Channel& c, float x, float drive, float stage2, const sat::Biased& curve)
    {
        x = c.inputHp.process(x) * drive;

        // 2x oversampling: zero-stuff (gain 2 restores the level), low-pass, clip, low-pass, keep
        // every other sample.
        float a = c.up[1].process(c.up[0].process(2.0f * x));
        float b = c.up[1].process(c.up[0].process(0.0f));
        a = clipStages(c, a, stage2, curve);
        b = clipStages(c, b, stage2, curve);
        float y = c.down[1].process(c.down[0].process(a));
        c.down[1].process(c.down[0].process(b));

        return c.dcBlock.process(y);
    }

    // After the tone stack: the speaker.
    static float cabinet(Channel& c, float y)
    {
        y = c.cabHp.process(y);
        y = c.cabLp[1].process(c.cabLp[0].process(y));
        return y * OUTPUT_TRIM;
    }

    float gain = 0.4f;
    sat::Biased stage2Curve;       // SAT, worked at BIAS for the second stage; the first uses it centred
    float tone = 0.0f;
    float cabHz = 4500.0f;
    float levelDb = 0.0f;

    std::array<Channel, 2> ch;
    fxb::EqBand toneLow, toneHigh;
    fxb::Smoother driveSmooth, levelSmooth, cabSmooth;
    float appliedCab = 4500.0f;
    int controlCounter = 0;
};
