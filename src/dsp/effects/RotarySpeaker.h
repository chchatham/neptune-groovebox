#pragma once

#include "ModDelay.h"

// Rotary speaker: a horn for the highs and a drum for the lows, split by a one-pole crossover.
// The cabinet has one loudspeaker, so the middle of the input goes in. Each rotor is one voice of
// the modulated delay (ModDelay.h): the delay swings as the rotor comes and goes (the pitch
// wobble) and the level follows a quarter turn later, opposite on the two sides (loudest as it
// faces you): the stereo picture is the rotors', not the input's.
//
// SPD   slow (chorale: horn 0.8 Hz, drum 0.4 Hz) to fast (the rates below), any place between
// HORN  the horn's fast rate, DRUM the drum's
// ACCL  how quickly the rotors reach a new speed: 2 s to 0.1 s for the horn, the drum three times
//       as slow (it is the heavier one)
// On the second page: SHPE (the rotors' motion: SIN is a rotor, anything else is not, on purpose),
// DPTH (how far the pitch wobbles: 50 is the cabinet as it was) and XOVR (the crossover).
class RotarySpeaker : public CellEffect
{
public:
    enum Param
    {
        Speed = 0,      // 0 = slow (chorale), 1 = fast (tremolo)
        HornRate,       // fast horn rate: 5 - 10 Hz
        DrumRate,       // fast drum rate: 0.5 - 2 Hz
        Acceleration    // 0 - 1 (spin-up speed)
    };
    enum Extra { Shape = 0, Doppler, Crossover };

    RotarySpeaker() { seed(); }

    void setUserShape(const float* levels) override
    {
        horn.setUserShape(levels);
        drum.setUserShape(levels);
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "SPD", 0, 0, 1, 0 },
            { "HORN", 1, 5, 10, 6.7f, K::Linear, "Hz" },
            { "DRUM", 2, 0.5f, 2, 1.0f, K::Linear, "Hz" },
            { "ACCL", 3, 0, 1, 0.5f },
            fxm::Lfo::shapeSpec("SHPE", Shape),
            { "DPTH", Doppler, 0, 1, 0.5f },
            { "XOVR", Crossover, 500.0f, 2000.0f, 800.0f, K::Log, "Hz" },
            {} };
        return c;
    }

    void prepareCore() override
    {
        for (auto* rotor : { &horn, &drum })
        {
            rotor->setVoices(1);
            rotor->setRange(0.0f, 0.007f);
            rotor->prepare(sampleRate, 0.008, true);
        }
        horn.setLevelFollow(0.3f);
        drum.setLevelFollow(0.2f);
    }

    void resetCore() override
    {
        horn.reset();
        drum.reset();
        hornNow = hornGoal;
        drumNow = drumGoal;
        cross = 0.0f;
        crossNow = crossCoeff;
    }

    void configure(const float* v) override
    {
        const float speed = v[Speed], depth = v[4 + Doppler], sr = (float) sampleRate;
        hornGoal = SLOW_HORN_RATE * std::pow(v[HornRate] / SLOW_HORN_RATE, speed);
        drumGoal = SLOW_DRUM_RATE * std::pow(v[DrumRate] / SLOW_DRUM_RATE, speed);
        const float seconds = 2.0f * std::pow(0.05f, v[Acceleration]);
        hornAccel = 1.0f - std::exp(-1.0f / (seconds * sr));
        drumAccel = 1.0f - std::exp(-1.0f / (3.0f * seconds * sr));
        horn.setDelay(0.0f, 0.002f, 0.002f * depth);
        drum.setDelay(0.0f, 0.003f, 0.003f * depth);
        crossCoeff = 1.0f - std::exp(-dsp::TWO_PI_F * v[4 + Crossover] / sr);
        const int shape = (int) v[4 + Shape];
        horn.setShape(shape);
        drum.setShape(shape);
    }

    // Every look: the rotors run up or down to their speed, a turned XOVR arrives over a few milliseconds.
    void controlCore() override
    {
        hornNow += std::min(1.0f, hornAccel * (float) CONTROL) * (hornGoal - hornNow);
        drumNow += std::min(1.0f, drumAccel * (float) CONTROL) * (drumGoal - drumNow);
        horn.setRate(hornNow);
        drum.setRate(drumNow);
        crossNow += 0.25f * (crossCoeff - crossNow);
    }

    void runCore(float* l, float* r, int n, float mixFrom, float mixTo) override
    {
        float lowL[CONTROL], lowR[CONTROL], highL[CONTROL], highR[CONTROL];
        const float coeff = crossNow;
        float low = cross;
        for (int i = 0; i < n; ++i)
        {
            const float mid = 0.5f * (l[i] + r[i]);
            low = fxb::flushDenormal(low + coeff * (mid - low));
            lowL[i] = lowR[i] = low;
            highL[i] = highR[i] = mid - low;
        }
        cross = low;
        horn.process(highL, highR, n);
        drum.process(lowL, lowR, n);

        // (Each sample from the start of the run, not from the one before: several at once.)
        const float slope = (mixTo - mixFrom) / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const float m = mixFrom + slope * (float) (i + 1);
            l[i] = l[i] * (1.0f - m) + (highL[i] + lowL[i]) * m;
            r[i] = r[i] * (1.0f - m) + (highR[i] + lowR[i]) * m;
        }
    }

private:
    static constexpr float SLOW_HORN_RATE = 0.8f;  // chorale horn speed
    static constexpr float SLOW_DRUM_RATE = 0.4f;  // chorale drum speed

    moddelay::Core horn, drum;
    float hornGoal = SLOW_HORN_RATE, drumGoal = SLOW_DRUM_RATE, hornNow = SLOW_HORN_RATE, drumNow = SLOW_DRUM_RATE;
    float hornAccel = 0.001f, drumAccel = 0.001f;
    float cross = 0.0f, crossCoeff = 0.1f, crossNow = 0.1f;
};
