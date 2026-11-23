#pragma once

#include "FxLfo.h"

// Ring modulator: the input is multiplied by a carrier, which replaces every input partial f by
// the pair (f - carrier) and (f + carrier) for each partial of the carrier. Textbook structure
// (Zoelzer, "DAFX", modulators chapter). Both oscillators are dsp/LFO.h: the carrier one of them run
// at audio rate, the other sweeping the carrier's frequency by up to +/- one octave.
//
// FREQ carrier frequency, RATE the sweep's rate, DPTH its depth (1 = +/- one octave), MIX; on the
// second page WAVE (the carrier's shape: SIN is the ring modulator of the books; SQR chops, SAW and
// RAMP add a ladder of side bands, their steps rounded so they do not alias; RND and SRND multiply
// by stepped or gliding noise at FREQ; USR and USRS are the track's own eight-step wave; EXPU and
// EXPD only go one way, so the dry sound stays in) and SHPE (the sweep's shape).
class RingMod : public CellEffect
{
public:
    enum Param { Freq = 0, Rate, Depth, Mix };
    enum Extra { Wave = 0, Shape };

    RingMod() { seed(); }

    void setUserShape(const float* levels) override
    {
        carrier.setUserShape(levels);
        sweep.setUserShape(levels);
    }

protected:
    const ParamSpec* cells() const override
    {
        using K = ParamKind;
        static const ParamSpec c[CELLS] = {
            { "FREQ", 0, 20.0f, 5000.0f, 220.0f, K::Log, "Hz" },
            { "RATE", 1, 0.05f, 20.0f, 1.0f, K::Log, "Hz" },
            { "DPTH", 2, 0, 1, 0.0f },
            { "MIX", 3, 0, 1, 1.0f },
            fxm::Lfo::shapeSpec("WAVE", Wave),
            fxm::Lfo::shapeSpec("SHPE", Shape),
            {}, {} };
        return c;
    }
    int mixCell() const override { return Mix; }

    void prepareCore() override
    {
        carrier.setSampleRate(sampleRate);
        sweep.prepare(sampleRate);
        shiftDecay = std::exp(-1.0f / (0.0005f * (float) sampleRate));
    }

    void resetCore() override
    {
        carrier.reset();
        carrier.drawRandom();
        sweep.reset();
        untilTune = 0;
        carrier.setShape(wanted);
        waveChanged = false;
        shift = lastCarrier = 0.0f;
    }

    void configure(const float* v) override
    {
        freq = v[Freq];
        depth = v[Depth];
        sweep.setRate(v[Rate]);
        sweep.setShape((int) v[4 + Shape]);
        wanted = fxm::Lfo::shapeOf((int) v[4 + Wave]);
        untilTune = 0;
    }

    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): the sweep counts its own steps)
    void runCore(float* left, float* right, int numSamples, float mixFrom, float mixTo) override
    {
        const float mixSlope = (mixTo - mixFrom) / (float) numSamples;
        float m = mixFrom;
        if (wanted != carrier.getShape())
        {
            carrier.setShape(wanted);
            waveChanged = true;
        }
        const LFO::Shape wave = wanted;
        for (int i = 0; i < numSamples; ++i)
        {
            const float moved = sweep.next();
            if (--untilTune <= 0)
            {
                // The carrier's frequency, set every few samples (its phase runs on unbroken).
                const float f = std::min(depth > 0.0f ? freq * std::exp2(depth * moved) : freq, (float) sampleRate * 0.45f);
                carrier.setRate(f);
                step = (double) f / sampleRate;
                untilTune = TUNE_EVERY;
            }

            float c = carrier.process();
            // The shapes that jump are rounded over the step they jump in (polyBLEP).
            // (The place is taken as the LFO took it, in single precision: a step that falls on
            // a sample must be seen on the same side by both.)
            if (wave == LFO::Shape::Square || wave == LFO::Shape::Saw || wave == LFO::Shape::RampDown)
            {
                const double t = (double) (float) carrier.getPhase();
                if (wave == LFO::Shape::Square)   c += dsp::polyBLEP(t, step) - dsp::polyBLEP(t < 0.5 ? t + 0.5 : t - 0.5, step);
                else if (wave == LFO::Shape::Saw) c -= dsp::polyBLEP(t, step);
                else                              c += dsp::polyBLEP(t, step);
            }

            // A new WAVE starts where the old one was and the difference dies away over 2 ms.
            if (waveChanged)
            {
                waveChanged = false;
                shift = lastCarrier - c;
            }
            if (shift != 0.0f)
            {
                c += shift;
                shift = std::abs(shift) < 1.0e-5f ? 0.0f : shift * shiftDecay;
            }
            lastCarrier = c;

            const float gain = 1.0f + (m += mixSlope) * (c - 1.0f);
            left[i] *= gain;
            right[i] *= gain;
        }
    }

private:
    static constexpr int TUNE_EVERY = 8;

    LFO carrier;
    fxm::Lfo sweep;
    float freq = 220.0f, depth = 0.0f;
    LFO::Shape wanted = LFO::Shape::Sine;
    float shift = 0.0f, shiftDecay = 0.0f, lastCarrier = 0.0f;
    bool waveChanged = false;
    double step = 0.0;
    int untilTune = 0;
};
