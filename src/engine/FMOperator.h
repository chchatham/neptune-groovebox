#pragma once

#include "../dsp/DSPUtils.h"
#include "../dsp/Envelope.h"
#include <algorithm>
#include <cmath>

class FMOperator
{
public:
    void setSampleRate(double sr) { sampleRate = sr; env.setSampleRate(sr); }

    void setRatio(float r) { ratio = r; }
    void setLevel(float l) { level = l; }
    void setDetune(float cents) { detuneCents = cents; detuneMul = std::exp2(cents / 1200.0f); }
    void setPitchMul(float m) { pitchMul = m; }          // tuning + pitch bend, as a frequency ratio
    void setModSensitivity(float s) { modSensitivity = s; }   // scales incoming modulation (pressure)
    void setFeedback(float fb) { feedback = fb; }
    void setFixed(bool f) { fixedFreq = f; }
    void setFixedHz(float hz) { fixedHz = hz; }

    // From the voice's LFOs: a frequency ratio, a factor on the incoming modulation and an offset
    // of the feedback. clearLfo() is the state without them.
    void setLfo(float pitchRatio, float indexFactor, float feedbackOffset)
    {
        lfoPitchMul = pitchRatio;
        lfoIndexMul = indexFactor;
        lfoFeedback = feedbackOffset;
    }
    void clearLfo() { setLfo(1.0f, 1.0f, 0.0f); }

    float getRatio() const { return ratio; }
    float getLevel() const { return level; }
    Envelope& getEnvelope() { return env; }

    void noteOn(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        baseFrequency = dsp::midiNoteToHz(note);
        phase = 0.0;
        prevOutput = 0.0f;
        env.noteOn();
        active = true;
    }

    void noteOff()
    {
        env.noteOff();
    }

    float process(float modulation = 0.0f)
    {
        if (!active)
            return 0.0f;

        double freq = fixedFreq ? (double) fixedHz
                                : baseFrequency * (double) ratio * (double) detuneMul * (double) pitchMul;
        if (lfoPitchMul != 1.0f)
            freq *= (double) lfoPitchMul;
        double phaseInc = freq / sampleRate;

        float fbMod = prevOutput * (lfoFeedback != 0.0f ? std::clamp(feedback + lfoFeedback, 0.0f, 1.0f) : feedback);
        float totalMod = modulation * modSensitivity * lfoIndexMul + fbMod;

        float sample = std::sin((float)(phase * dsp::TWO_PI_D) + totalMod);
        phase = dsp::wrapPhase(phase + phaseInc);

        float envVal = env.process();
        if (!env.isActive())
            active = false;

        float output = sample * envVal * level * currentVelocity;
        prevOutput = output;
        return output;
    }

    void reset()
    {
        phase = 0.0;
        prevOutput = 0.0f;
        env.reset();
        active = false;
    }

    bool isActive() const { return active; }

private:
    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    double phase = 0.0;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool active = false;

    float ratio = 1.0f;
    float level = 1.0f;
    float detuneCents = 0.0f;
    float detuneMul = 1.0f;
    float pitchMul = 1.0f;
    float modSensitivity = 1.0f;
    float feedback = 0.0f;
    bool fixedFreq = false;
    float fixedHz = 440.0f;
    float prevOutput = 0.0f;
    float lfoPitchMul = 1.0f;
    float lfoIndexMul = 1.0f;
    float lfoFeedback = 0.0f;

    Envelope env;
};
