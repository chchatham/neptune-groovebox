// The FM engine as it was before it moved onto the modulation router (phase U4-FM, commit c21530a),
// kept so that the suite "U4 FM" can play old against new. Only the class names and the include
// paths were changed. It still uses src/engine/EngineLfoSection.h (and EngineModSection.h through
// it): when those headers go (the last engine ported), this copy and the old-against-new tests go
// with them, or the two headers are copied here first.
#pragma once

#include "../../src/dsp/DSPUtils.h"
#include "../../src/dsp/Envelope.h"
#include "../../src/dsp/PhaseOsc.h"
#include <algorithm>
#include <cmath>

// One FM operator: the shared oscillator (dsp/PhaseOsc.h) at a ratio of the note, under its own
// envelope and level. What it reads is its WAVE: the sine (the classic operator, and the same
// samples as before the oscillator was shared), the cycles of a sample, or any built-in
// wavetable, with a position where the wave has one. Phase modulation, feedback and the track
// input act on every wave alike, so a table can be a carrier or a modulator.
class OldFMOperator
{
public:
    void setSampleRate(double sr)
    {
        sampleRate = sr;
        env.setSampleRate(sr);
        osc.setSampleRate(sr);
        levelStep = (float) (1.0 / (0.004 * sr));
        positionStep = (float) (1.0 / (phaseosc::POSITION_GLIDE_SECONDS * std::max(sr, 1.0)));
    }

    // What the operator reads. A change under a sounding note is a swap (the engine carries the
    // step off: EngineBase::soundSwapped).
    void setWave(const phaseosc::Wave& w)
    {
        if (wave.isSine())
            positionNow = position;     // nothing was read at a position: the new wave starts where it is set
        wave = w;
        if (state != Off)
            state = w.isSine() ? Sine : Table;
    }
    const phaseosc::Wave& getWave() const { return wave; }
    // POS, 0 .. 1: moved to under a sounding note, never jumped to.
    void setPosition(float p) { position = phaseosc::unit(p); }
    // PENV: how far the operator's own envelope moves its position (-1 .. 1), sample by sample.
    void setPositionEnv(float amount) { positionEnv = amount; }

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

    // Phase modulation from outside the voice (the track input, XIN), in radians at full
    // sensitivity: added to what the algorithm's modulators send for the next sample, so pressure
    // and INDX scale it as they scale an operator's. 0 = none, and then nothing is added.
    void setExternal(float radians) { external = radians; }

    float getRatio() const { return ratio; }
    float getLevel() const { return level; }
    Envelope& getEnvelope() { return env; }

    void noteOn(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        baseFrequency = dsp::midiNoteToHz(note);
        osc.reset();
        prevOutput = 0.0f;
        env.noteOn();
        state = wave.isSine() ? Sine : Table;
        struck = true;
    }

    void noteOff()
    {
        env.noteOff();
    }

    // One sample. The two kinds of wave are two whole bodies of the same code (run<>): with the
    // table's read-out as a branch inside one body the sine cost a third more than it did before
    // the oscillator was shared (the compiler then keeps the operator's state in memory around a
    // call it never makes); as its own body it costs what it always did. "Sounding" and "which
    // body" are one variable, so a sine operator is asked one question a sample, as before.
    float process(float modulation = 0.0f)
    {
        if (state == Sine)
            return run<true>(modulation);
        return state == Off ? 0.0f : run<false>(modulation);
    }

    void reset()
    {
        osc.reset();
        prevOutput = 0.0f;
        env.reset();
        state = Off;
    }

    bool isActive() const { return state != Off; }

private:
    template <bool ReadsSine>
    float run(float modulation)
    {
        double freq = fixedFreq ? (double) fixedHz
                                : baseFrequency * (double) ratio * (double) detuneMul * (double) pitchMul;
        if (lfoPitchMul != 1.0f)
            freq *= (double) lfoPitchMul;
        double phaseInc = freq / sampleRate;

        if (feedbackNow != feedback)    // (set under a sounding note: moved to, like the level below)
            feedbackNow = struck ? feedback : dsp::towards(feedbackNow, feedback, levelStep);
        // (Not "no feedback, so do not read the last output": that was tried, and while one
        // operator alone ran a quarter faster, six in a voice ran up to 70 % slower.)
        float fbMod = prevOutput * (lfoFeedback != 0.0f ? std::clamp(feedbackNow + lfoFeedback, 0.0f, 1.0f) : feedbackNow);
        if (external != 0.0f)
            modulation += external;
        float totalMod = modulation * modSensitivity * lfoIndexMul + fbMod;

        // The sine is the operator as it always was; anything else is read at the position.
        float sample;
        if constexpr (ReadsSine)
            sample = osc.sine(totalMod);
        else
        {
            if (positionNow != position)
                positionNow = struck ? position : dsp::towards(positionNow, position, positionStep);
            // (The wave as it stands at this sample, taken once: WAVE may be set from another
            // thread while the note sounds, and the read asks twice whether there is a table.)
            const phaseosc::Wave w = wave;
            sample = osc.read(w, phaseInc, totalMod,
                              positionEnv != 0.0f ? positionNow + positionEnv * env.getCurrentLevel() : positionNow);
        }
        osc.advance(phaseInc);

        float envVal = env.process();
        if (!env.isActive())
            state = Off;

        // A level set under a sounding note is moved to over at most 4 ms, never jumped to (a
        // carrier's level is the sound's, a modulator's its brightness: either jump is a click).
        // The first sample of a note takes it as it stands.
        if (levelNow != level)
        {
            const float d = level - levelNow;
            levelNow = struck || std::abs(d) <= levelStep ? level : levelNow + (d > 0.0f ? levelStep : -levelStep);
        }
        struck = false;

        float output = sample * envVal * levelNow * currentVelocity;
        prevOutput = output;
        return output;
    }

    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    phaseosc::PhaseOsc osc;
    phaseosc::Wave wave;                // the sine until WAVE says otherwise
    float position = 0.0f, positionNow = 0.0f, positionStep = 0.01f, positionEnv = 0.0f;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    enum State : unsigned char { Off, Sine, Table };
    State state = Off;                  // off, or sounding and reading the sine or a table

    float ratio = 1.0f;
    float level = 1.0f;
    float levelNow = 1.0f, levelStep = 0.005f;
    bool struck = true;
    float detuneCents = 0.0f;
    float detuneMul = 1.0f;
    float pitchMul = 1.0f;
    float modSensitivity = 1.0f;
    float feedback = 0.0f, feedbackNow = 0.0f;
    bool fixedFreq = false;
    float fixedHz = 440.0f;
    float prevOutput = 0.0f;
    float lfoPitchMul = 1.0f;
    float lfoIndexMul = 1.0f;
    float lfoFeedback = 0.0f;
    float external = 0.0f;

    Envelope env;
};
