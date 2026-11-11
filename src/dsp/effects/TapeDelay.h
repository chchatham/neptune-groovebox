#pragma once

#include "EffectBase.h"
#include "FxBlocks.h"

// Tape-style echo. Structure (the usual tape-echo model, e.g. Zoelzer, "DAFX", delay chapter, and
// Zavalishin-style bounded feedback loops): a delay line read with Hermite interpolation at a
// position that glides when TIME changes (pitch slides like a tape changing speed) and wobbles
// with wow (0.6 Hz) and flutter (7 Hz); the repeats pass a one-pole low-pass and high-pass each
// time round; the signal going onto the "tape" is soft-saturated, which also bounds the loop.
//
// TIME  echo time
// FDBK  0..1 -> loop gain 0..1.1: the top of the range self-oscillates, held by the saturation
// AGE   0..1: more wow and flutter, harder saturation, duller and thinner repeats
// MIX   dry / wet
class TapeDelay : public EffectBase
{
public:
    enum Param { Time = 0, Feedback, Age, Mix };

    static constexpr float MIN_TIME = 0.02f;
    static constexpr float MAX_TIME = 1.5f;

    TapeDelay()
    {
        mix = 0.35f;
        prepare(44100.0, 0);
    }

    ParamSpec getParamSpec(int index) const override
    {
        using K = ParamKind;
        switch (index)
        {
            case 0: return { "TIME", 0, MIN_TIME, MAX_TIME, 0.3f, K::Log, "s" };
            case 1: return { "FDBK", 1, 0, 1, 0.45f };
            case 2: return { "AGE", 2, 0, 1, 0.4f };
            case 3: return { "MIX", 3, 0, 1, 0.35f };
            default: return {};
        }
    }

    void setParameter(int id, float value) override
    {
        switch (id)
        {
            case Time:     timeSec = std::clamp(value, MIN_TIME, MAX_TIME); break;
            case Feedback: feedback = std::clamp(value, 0.0f, 1.0f); break;
            case Age:      age = std::clamp(value, 0.0f, 1.0f); break;
            case Mix:      setMix(value); break;
            default: break;
        }
    }

    float getParameter(int id) const override
    {
        switch (id)
        {
            case Time:     return timeSec;
            case Feedback: return feedback;
            case Age:      return age;
            case Mix:      return mix;
            default:       return 0.0f;
        }
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = sr;
        int maxSamples = (int) ((double) MAX_TIME * 1.02 * sr) + 16;
        lineL.prepare(maxSamples);
        lineR.prepare(maxSamples);
        timeSmooth.setTime(sr, 80.0f);
        appliedAge = -1.0f;
        reset();
    }

    void reset() override
    {
        lineL.clear();
        lineR.clear();
        for (auto* f : { &lpL, &lpR, &hpL, &hpR })
            f->reset();
        timeSmooth.snap(timeSec);
        wowPhase = 0.0;
        flutterPhase = 0.0;
    }

    void process(float& left, float& right) override
    {
        if (!enabled) return;

        if (age != appliedAge)
            updateAge();

        float mod = 0.0f;
        if (age > 0.0f)
        {
            mod = age * (WOW_DEPTH * (float) std::sin(wowPhase * dsp::TWO_PI_D)
                         + FLUTTER_DEPTH * (float) std::sin(flutterPhase * dsp::TWO_PI_D));
            wowPhase += WOW_HZ / sampleRate;
            if (wowPhase >= 1.0) wowPhase -= 1.0;
            flutterPhase += FLUTTER_HZ / sampleRate;
            if (flutterPhase >= 1.0) flutterPhase -= 1.0;
        }

        float d = timeSmooth.next(timeSec) * (float) sampleRate * (1.0f + mod);

        float wetL = hpL.highPass(lpL.lowPass(lineL.readHermite(d)));
        float wetR = hpR.highPass(lpR.lowPass(lineR.readHermite(d)));

        float loopGain = feedback * MAX_LOOP_GAIN;
        lineL.write(fxb::flushDenormal(record(left + wetL * loopGain)));
        lineR.write(fxb::flushDenormal(record(right + wetR * loopGain)));

        left = lerp(left, wetL, mix);
        right = lerp(right, wetR, mix);
    }

private:
    static constexpr float MAX_LOOP_GAIN = 1.1f;
    static constexpr double WOW_HZ = 0.6;
    static constexpr double FLUTTER_HZ = 7.0;
    static constexpr float WOW_DEPTH = 0.003f;       // fraction of the delay time at AGE 1
    static constexpr float FLUTTER_DEPTH = 0.0006f;

    // What goes onto the tape: soft saturation, unity gain for small signals, never beyond
    // +/- 1 / drive.
    float record(float x) const { return fxb::softSat(x * drive) / drive; }

    void updateAge()
    {
        appliedAge = age;
        drive = 1.0f + 2.0f * age;
        float lpHz = 9000.0f * std::pow(2200.0f / 9000.0f, age);
        float hpHz = 40.0f * std::pow(180.0f / 40.0f, age);
        lpL.setCutoff(sampleRate, lpHz);
        lpR.setCutoff(sampleRate, lpHz);
        hpL.setCutoff(sampleRate, hpHz);
        hpR.setCutoff(sampleRate, hpHz);
    }

    float timeSec = 0.3f;
    float feedback = 0.45f;
    float age = 0.4f;
    float appliedAge = -1.0f;
    float drive = 1.0f;

    fxb::DelayLine lineL, lineR;
    fxb::OnePole lpL, lpR, hpL, hpR;
    fxb::Smoother timeSmooth;
    double wowPhase = 0.0;
    double flutterPhase = 0.0;
};
