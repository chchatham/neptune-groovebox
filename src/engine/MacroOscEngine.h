#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "VoiceFilterSection.h"
#include "EngineLfoSection.h"
#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/filters/SVFilter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// MacroOscVoice — Plaits-inspired macro oscillator with multiple models.
//
// Each model is a distinct synthesis algorithm controlled by two macro knobs:
//   timbre (primary tonal control)
//   color  (secondary tonal control)
//
// and two more (harmonics, morph), whose meaning also belongs to the model - every one of the four
// is heard in every model (the "Dead Controls" tests). The third is neutral at 0.5 and the fourth
// at 0 wherever the model had no use for it before, so those values are the sound as it was.
// Amplitude is shaped by a built-in ADSR envelope.
// All DSP is audio-thread safe: no heap allocation, no mutex.
// ---------------------------------------------------------------------------

class MacroOscVoice
{
public:
    enum class Model
    {
        VirtualAnalog = 0, // supersaw / hypersaw
        FM2Op,             // simple 2-operator FM
        WaveFold,          // sine & triangle folding waveshaper
        Formant,           // vowel formant synthesis
        Harmonic,          // harmonic additive
        PluckedString,     // Karplus-Strong
        ParticleNoise,     // dust / crackle
        BowedString,       // bowed string physical model
        Count
    };

    // LFO destinations (EngineLfoSection), in the order of the DEST cell. The four macros are
    // moved by +-1 around their knobs (clamped to 0..1).
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // +-12 semitones, depth squared
        LfoAmp,         // gain 0..2
        LfoPan,         // +-1
        LfoCutoff,      // +-4 octaves
        LfoResonance,   // +-0.5
        LfoTimbre,
        LfoColor,
        LfoHarmonics,
        LfoMorph,
        NumLfoDests
    };

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        envChain.setSampleRate(sr);
        filter.setSampleRate(sr);
        lfos.setSampleRate(sr);
        declick.setSampleRate(sr);
        gainDeclick.setSampleRate(sr);
        shaper.setSampleRate(sr);
        // Clear the KS / bowed-string delay line
        std::fill(delayLine.begin(), delayLine.end(), 0.0f);
        delayWritePos = 0;
    }

    void setNote(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        noteFrequency = dsp::midiNoteToHz(note);
        baseFrequency = noteFrequency * (double) pitchRatio;

        // Reset per-model state for the new note
        resetModelState();
    }

    void noteOn()
    {
        if (active)
        {
            declick.arm();      // struck again while sounding: the restart must not jump,
            gainDeclick.arm();  // nor the step to the new velocity
        }
        // An LFO on a page cell is read before the first sample (the engine sets the cell then).
        if (lfos.keyed()) lfos.noteOnAndSample(); else lfos.noteOn();
        filter.noteOn(currentNote, currentVelocity, active);
        ampEnv.noteOn();
        if (envChainEnabled)
            envChain.noteOn();
        active = true;

        // Karplus-Strong: fill delay line with filtered noise burst on note-on
        if (model == Model::PluckedString)
            initKarplusStrong();

        // Bowed string: init waveguide
        if (model == Model::BowedString)
            initBowedString();
    }

    void noteOff()
    {
        ampEnv.noteOff();
        if (envChainEnabled)
            envChain.noteOff();
        filter.noteOff();
    }

    bool isActive() const { return active; }

    float process()
    {
        if (!active)
            return 0.0f;

        // LFOs: the macros and the pitch are moved for this sample only (the set values stay).
        const bool lfoOn = lfos.anyOn();
        const float setTimbre = timbre, setColor = color, setHarmonics = harmonics, setMorph = morph;
        const double setFrequency = baseFrequency;
        if (lfoOn)
        {
            lfos.tick();
            lfoApplied = true;
            timbre = juce::jlimit(0.0f, 1.0f, timbre + lfos.amount(LfoTimbre));
            color = juce::jlimit(0.0f, 1.0f, color + lfos.amount(LfoColor));
            harmonics = juce::jlimit(0.0f, 1.0f, harmonics + lfos.amount(LfoHarmonics));
            morph = juce::jlimit(0.0f, 1.0f, morph + lfos.amount(LfoMorph));
            const float semis = lfos.pitchSemitones(LfoPitch);
            if (semis != 0.0f)
                baseFrequency *= (double) std::exp2(semis / 12.0f);
            filter.setLfoOctaves(lfos.amount(LfoCutoff) * VoiceLfos::CUTOFF_OCTAVES);
            filter.setResonanceMod(lfos.amount(LfoResonance) * VoiceLfos::RESONANCE_RANGE);
            lfoGain = juce::jlimit(0.0f, 2.0f, 1.0f + lfos.amount(LfoAmp));
            lfoPan = juce::jlimit(-1.0f, 1.0f, lfos.amount(LfoPan));
        }
        else if (lfoApplied)
        {
            filter.setLfoOctaves(0.0f);
            filter.setResonanceMod(0.0f);
            lfoGain = 1.0f;
            lfoPan = 0.0f;
            lfoApplied = false;
        }

        float sample = 0.0f;

        switch (model)
        {
            case Model::VirtualAnalog:    sample = processVirtualAnalog();  break;
            case Model::FM2Op:            sample = processFM2Op();          break;
            case Model::WaveFold:         sample = processWaveFold();       break;
            case Model::Formant:          sample = processFormant();        break;
            case Model::Harmonic:         sample = processHarmonic();       break;
            case Model::PluckedString:    sample = processPluckedString();  break;
            case Model::ParticleNoise:    sample = processParticleNoise();  break;
            case Model::BowedString:      sample = processBowedString();    break;
            default:                      sample = 0.0f;                    break;
        }

        if (lfoOn)
        {
            timbre = setTimbre;
            color = setColor;
            harmonics = setHarmonics;
            morph = setMorph;
            baseFrequency = setFrequency;
        }

        sample = filter.process(declick.process(sample));

        float env;
        if (envChainEnabled && envChain.getStageCount() > 0)
        {
            env = envChain.process();
            // Also advance the basic ampEnv to keep it in sync for noteOff
            ampEnv.process();
            if (!envChain.isActive() && !ampEnv.isActive())
                active = false;
        }
        else
        {
            env = ampEnv.process();
            if (!ampEnv.isActive())
                active = false;
        }

        return gainDeclick.process(sample * env * currentVelocity * level * gainMod * lfoGain);
    }

    void reset()
    {
        declick.reset();
        gainDeclick.reset();
        ampEnv.reset();
        envChain.reset();
        filter.reset();
        active = false;
        phase = 0.0;
        modPhase = 0.0;
        std::fill(delayLine.begin(), delayLine.end(), 0.0f);
        delayWritePos = 0;
        bowForce = 0.0f;
    }

    // --- Parameter setters (called from engine on param change) ---
    void setModel(Model m)          { model = m; }
    Model getModel() const          { return model; }
    void setTimbre(float v)         { timbre = juce::jlimit(0.0f, 1.0f, v); }
    void setColor(float v)          { color = juce::jlimit(0.0f, 1.0f, v); }
    void setHarmonics(float v)      { harmonics = juce::jlimit(0.0f, 1.0f, v); }
    void setMorph(float v)          { morph = juce::jlimit(0.0f, 1.0f, v); }
    void setDetune(float v)         { detune = v; } // in cents
    void setLevel(float v)          { level = juce::jlimit(0.0f, 1.0f, v); }
    void setGainMod(float g)        { gainMod = g; }      // pressure -> volume
    // Tuning, fine tune, bend and vibrato together, in semitones.
    void setPitchOffset(float semitones)
    {
        pitchRatio = std::exp2(semitones / 12.0f);
        baseFrequency = noteFrequency * (double) pitchRatio;
    }
    Envelope& getAmpEnvelope()      { return ampEnv; }
    VoiceFilter& getFilter()        { return filter; }
    VoiceLfos& getLfos()            { return lfos; }
    // -1..1 from LFOs routed to the pan (as of the last processed sample); the engine places the voice.
    float getLfoPan() const         { return lfoPan; }

    // --- EnvelopeChain support ---
    void setEnvelopeChainEnabled(bool enabled) { envChainEnabled = enabled; }
    bool isEnvelopeChainEnabled() const        { return envChainEnabled; }
    EnvelopeChain& getEnvelopeChain()          { return envChain; }

private:
    // ===================================================================
    // Model 0 — Virtual Analog (supersaw / hypersaw)
    //
    // Multiple detuned sawtooth oscillators with polyBLEP antialiasing.
    // timbre  = number of saws (1-7, interpolated)
    // color   = detune spread between saws
    // harmonics = how many harmonics each saw has: 0.5 = a saw; below, every saw fades into a
    //             sine (0 = sines only); above, a saw an octave up is mixed in (to half and half)
    // morph   = saw -> square: a second saw half a cycle behind is subtracted (1 = a square).
    //           Everything is made of band-limited (polyBLEP) saws and sines.
    // ===================================================================
    float processVirtualAnalog()
    {
        int numSaws = 1 + (int)(timbre * 6.0f);
        float spread = color * 0.06f + detune * 0.0001f; // max ~6 cents spread per voice

        const float toSine = harmonics < 0.5f ? 1.0f - harmonics * 2.0f : 0.0f;
        const float octave = harmonics > 0.5f ? harmonics - 0.5f : 0.0f;
        // Saw minus m x shifted saw has the power (1 + m + m^2) / 3: keep the saw's level.
        const float squareGain = morph > 0.0f ? 1.0f / std::sqrt(1.0f + morph + morph * morph) : 1.0f;

        float mix = 0.0f;
        for (int i = 0; i < numSaws; ++i)
        {
            // Symmetric spread around center
            float detuneOffset = (numSaws > 1)
                ? spread * ((float)i / (float)(numSaws - 1) - 0.5f) * 2.0f
                : 0.0f;
            double freq = baseFrequency * std::exp2(detuneOffset / 12.0);
            double phaseInc = freq / sampleRate;

            // Each saw has its own sub-phase stored in sawPhases array
            sawPhases[i] += phaseInc;
            sawPhases[i] = dsp::wrapPhase(sawPhases[i]);

            float p = (float)sawPhases[i];
            float saw = 2.0f * p - 1.0f;

            // polyBLEP correction at discontinuity
            saw -= dsp::polyBLEP(p, phaseInc);

            if (morph > 0.0f)
            {
                const double half = p < 0.5f ? sawPhases[i] + 0.5 : sawPhases[i] - 0.5;
                saw = (saw - morph * blepSaw(half, phaseInc)) * squareGain;
            }
            if (octave > 0.0f)
            {
                const double twice = dsp::wrapPhase(sawPhases[i] * 2.0);
                saw += (blepSaw(twice, phaseInc * 2.0) - saw) * octave;
            }
            else if (toSine > 0.0f)
            {
                // (A saw's fundamental is an inverted sine: the blend stays in phase.)
                saw += (-std::sin(p * dsp::TWO_PI_F) - saw) * toSine;
            }

            mix += saw;
        }

        return mix / (float)numSaws;
    }

    // ===================================================================
    // Model 1 — FM 2-Op (carrier + modulator)
    //
    // timbre  = modulation index (FM depth)
    // color   = modulator ratio (1x-8x)
    // harmonics = modulator detune, +-1 semitone around 0.5: the sidebands leave the harmonic
    //             series and beat (the classic FM detune)
    // morph   = modulator feedback, 0 = none: its sine leans into a saw and, far up, into noise
    //           (the mean of its last two samples is fed back, the usual guard against hunting)
    // ===================================================================
    float processFM2Op()
    {
        float modRatio = 1.0f + color * 7.0f;
        float modIndex = timbre * 8.0f;

        double modFreq = baseFrequency * modRatio;
        if (harmonics != 0.5f)
            modFreq *= (double) std::exp2((harmonics - 0.5f) * (2.0f / 12.0f));
        double modPhaseInc = modFreq / sampleRate;
        modPhase += modPhaseInc;
        modPhase = dsp::wrapPhase(modPhase);

        float modSine;
        if (morph > 0.0f)
        {
            modSine = std::sin((float)modPhase * dsp::TWO_PI_F + morph * FM_FEEDBACK * 0.5f * (fmFeedback[0] + fmFeedback[1]));
            fmFeedback[1] = fmFeedback[0];
            fmFeedback[0] = modSine;
        }
        else
        {
            modSine = std::sin((float)modPhase * dsp::TWO_PI_F);
        }
        float modSignal = modSine * modIndex;

        double carrPhaseInc = baseFrequency / sampleRate;
        phase += carrPhaseInc;
        phase = dsp::wrapPhase(phase);

        return std::sin((float)phase * dsp::TWO_PI_F + modSignal);
    }

    // ===================================================================
    // Model 2 — Wavefold (sine fold / triangle fold / chebyshev)
    //
    // timbre  = fold amount (drive into wavefolder)
    // color   = waveform morph (0 = sine source, 1 = triangle source)
    // harmonics = bias (symmetry), none at 0.5: an offset into the folder, up to a quarter of a
    //             fold either way, which brings in the even harmonics (the offset it leaves in
    //             the output is taken out by a 12 Hz high-pass, the project's SVF)
    // morph   = chebyshev polynomial order blend
    // ===================================================================
    float processWaveFold()
    {
        double phaseInc = baseFrequency / sampleRate;
        phase += phaseInc;
        phase = dsp::wrapPhase(phase);

        float p = (float)phase;

        // Source waveform: morph between sine and triangle
        float sine = std::sin(p * dsp::TWO_PI_F);
        float tri = p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
        float source = sine * (1.0f - color) + tri * color;

        // Apply chebyshev waveshaping based on morph parameter
        // T2(x) = 2x^2 - 1,  T3(x) = 4x^3 - 3x
        float cheb2 = 2.0f * source * source - 1.0f;
        float cheb3 = source * (4.0f * source * source - 3.0f);
        float chebyshev = source * (1.0f - morph) + (cheb2 * 0.5f + cheb3 * 0.5f) * morph;

        // Wavefold: drive and fold
        float drive = 1.0f + timbre * 10.0f;
        float driven = chebyshev * drive;

        // Sine-fold: repeated folding via sin(x)
        if (harmonics == 0.5f && ! shaperOn)
            return std::sin(driven);
        if (! shaperOn)
        {
            shaperOn = true;            // from here to the end of the note, so nothing jumps
            shaper.reset();
            shaper.setMode(SVFilter::Mode::HighPass);
            shaper.setResonance(0.707f);
            shaper.setCutoff(12.0f);
        }
        return shaper.process(std::sin(driven + (harmonics - 0.5f) * dsp::PI_F));
    }

    // ===================================================================
    // Model 3 — Formant / Vowel synthesis
    //
    // Three parallel bandpass resonators at vowel formant frequencies.
    // timbre  = vowel blend (a -> e -> i -> o -> u)
    // color   = formant shift (transpose formants up/down)
    // harmonics = Q of the three resonators
    // morph   = what excites them: 0 = the pulse train (a voice), 1 = noise (a whisper)
    // ===================================================================
    float processFormant()
    {
        double phaseInc = baseFrequency / sampleRate;
        phase += phaseInc;
        phase = dsp::wrapPhase(phase);

        // Pulse train source (rich harmonics for formant filtering)
        float p = (float)phase;
        float pulse = (p < 0.1f) ? 1.0f : -0.1f; // narrow pulse
        if (morph > 0.0f)
            pulse += (dsp::xorshiftNoise(noiseState) * FORMANT_NOISE - pulse) * morph;

        // Formant frequencies for 5 vowels (F1, F2, F3) in Hz
        // a(800,1200,2500) e(400,2200,2800) i(300,2700,3300) o(500,800,2500) u(350,700,2200)
        static constexpr float vowelF1[] = { 800.0f, 400.0f, 300.0f, 500.0f, 350.0f };
        static constexpr float vowelF2[] = { 1200.0f, 2200.0f, 2700.0f, 800.0f, 700.0f };
        static constexpr float vowelF3[] = { 2500.0f, 2800.0f, 3300.0f, 2500.0f, 2200.0f };

        // Interpolate between vowels
        float vowelPos = timbre * 4.0f; // 0..4
        int v0 = juce::jlimit(0, 4, (int)vowelPos);
        int v1 = juce::jlimit(0, 4, v0 + 1);
        float frac = vowelPos - (float)v0;

        float shiftMul = std::exp2f((color - 0.5f) * 2.0f); // +/- 1 octave
        float f1 = (vowelF1[v0] * (1.0f - frac) + vowelF1[v1] * frac) * shiftMul;
        float f2 = (vowelF2[v0] * (1.0f - frac) + vowelF2[v1] * frac) * shiftMul;
        float f3 = (vowelF3[v0] * (1.0f - frac) + vowelF3[v1] * frac) * shiftMul;

        // Simple resonant bandpass (one-pole pair) for each formant
        float q = 8.0f + harmonics * 40.0f; // resonance from harmonics param
        float out1 = processBandpass(formantState[0], pulse, f1, q);
        float out2 = processBandpass(formantState[1], pulse, f2, q);
        float out3 = processBandpass(formantState[2], pulse, f3, q);

        // The resonators have a gain of 1 at their centres, so what comes out is a small part
        // of the pulse train, smaller the narrower they are: bring it up to the other models' level.
        return (out1 * 0.5f + out2 * 0.35f + out3 * 0.15f) * FORMANT_GAIN * std::sqrt(q * 0.125f);
    }

    // ===================================================================
    // Model 4 — Harmonic Additive
    //
    // Up to 32 harmonics with individual amplitude control.
    // timbre  = harmonic tilt (0 = fundamental only, 1 = bright/all harmonics)
    // color   = even/odd balance (0 = odd only [square-like], 1 = all, 0.5 = even only)
    // harmonics = number of active harmonics (1-32)
    // morph   = stretch, 0 = none: partial n moves up to n * sqrt(1 + B (n^2 - 1)), the series
    //           of a stiff string or a bar (B up to 0.012), so the tone turns into a bell. The
    //           fundamental stays where it is. Stretched partials each run on a phase of their own.
    // ===================================================================
    float processHarmonic()
    {
        double phaseInc = baseFrequency / sampleRate;
        phase += phaseInc;
        phase = dsp::wrapPhase(phase);

        int numHarmonics = 1 + (int)(harmonics * 31.0f);

        // The partials' levels and ratios change only when a macro does.
        if (timbre != harmTimbre || color != harmColor || morph != harmMorph)
        {
            harmTimbre = timbre;
            harmColor = color;
            harmMorph = morph;
            const float tiltExp = 1.0f + (1.0f - timbre) * 4.0f; // steeper roll-off when timbre is low
            const float stiffness = HARM_STRETCH * morph * morph;
            for (int h = 1; h <= MAX_HARMONICS; ++h)
            {
                // Even/odd balance
                float evenOddGain = 1.0f;
                if (h % 2 == 0)
                    evenOddGain = color; // even harmonics
                else if (h > 1)
                    evenOddGain = 1.0f - std::abs(color - 0.5f) * 0.5f; // odd (except fundamental)
                harmLevel[(size_t) (h - 1)] = evenOddGain / std::pow((float)h, tiltExp);
                harmRatio[(size_t) (h - 1)] = (float) h * std::sqrt(1.0f + stiffness * (float) (h * h - 1));
            }
        }

        // Once stretched, the partials keep their own phases to the end of the note (so that
        // coming back to 0 does not make them jump).
        if (morph > 0.0f && ! harmStretched)
        {
            harmStretched = true;
            const double before = phase - phaseInc;
            for (int h = 1; h <= MAX_HARMONICS; ++h)
                harmPhase[(size_t) (h - 1)] = (float) dsp::wrapPhase(before * h);
        }

        float mix = 0.0f;
        float normalization = 0.0f;
        const float nyquist = (float) (sampleRate * 0.5);

        for (int h = 1; h <= numHarmonics; ++h)
        {
            const size_t i = (size_t) (h - 1);
            float s;
            if (harmStretched)
            {
                // Frequency guard: skip partials above Nyquist
                if ((float) baseFrequency * harmRatio[i] > nyquist)
                    break;
                float ph = harmPhase[i] + (float) phaseInc * harmRatio[i];
                ph -= (float) (int) ph;
                harmPhase[i] = ph;
                s = std::sin(ph * dsp::TWO_PI_F);
            }
            else
            {
                if (baseFrequency * h > sampleRate * 0.5)
                    break;
                s = std::sin((float)(phase * h) * dsp::TWO_PI_F);
            }
            mix += s * harmLevel[i];
            normalization += harmLevel[i];
        }

        return (normalization > 0.0f) ? mix / normalization : 0.0f;
    }

    // ===================================================================
    // Model 5 — Plucked String (Karplus-Strong)
    //
    // Delay line excited by noise burst, with filtered feedback.
    // timbre  = damping (low-pass filter in feedback loop)
    // color   = pick position (comb filter notch)
    // harmonics = feedback amount (sustain)
    // morph   = string -> drum, 0 = string: the sample fed back is inverted at random, up to
    //           every other time (the drum variant of Karplus & Strong, "Digital Synthesis of
    //           Plucked-String and Drum Timbres", 1983: their blend factor going from 1 to 1/2).
    //           The pitch dissolves into a decaying burst.
    // ===================================================================
    float processPluckedString()
    {
        float delaySamples = (float)(sampleRate / baseFrequency);
        int delayLen = juce::jlimit(1, (int)DELAY_LINE_SIZE - 1, (int)delaySamples);

        // Read from delay line with linear interpolation
        float readPos = (float)delayWritePos - delaySamples;
        if (readPos < 0.0f)
            readPos += DELAY_LINE_SIZE;
        int readIdx0 = (int)readPos;
        int readIdx1 = (readIdx0 + 1) % (int)DELAY_LINE_SIZE;
        float frac = readPos - (float)readIdx0;
        readIdx0 = readIdx0 % (int)DELAY_LINE_SIZE;

        float sample = delayLine[readIdx0] * (1.0f - frac) + delayLine[readIdx1] * frac;

        // Pick position: comb filter (notch at harmonics of the pick position)
        if (color > 0.01f)
        {
            int pickDelay = juce::jlimit(1, delayLen / 2, (int)(color * (float)delayLen * 0.5f));
            float pickReadPos = (float)delayWritePos - (float)pickDelay;
            if (pickReadPos < 0.0f)
                pickReadPos += DELAY_LINE_SIZE;
            int pickIdx = (int)pickReadPos % (int)DELAY_LINE_SIZE;
            sample = (sample + delayLine[pickIdx]) * 0.5f;
        }

        // Low-pass filter in feedback loop (damping from timbre)
        float damping = 0.1f + timbre * 0.85f; // 0.1 (very bright) to 0.95 (very dull)
        ksFilterState = ksFilterState * damping + sample * (1.0f - damping);

        // Write back with feedback
        float feedback = 0.95f + harmonics * 0.049f; // very close to 1 for long sustain
        if (morph > 0.0f && dsp::xorshiftFloat(noiseState) < morph * 0.5f)
            feedback = -feedback;
        delayLine[delayWritePos] = ksFilterState * feedback;
        delayWritePos = (delayWritePos + 1) % (int)DELAY_LINE_SIZE;

        return sample;
    }

    // ===================================================================
    // Model 6 — Particle Noise / Dust
    //
    // Generates sparse random impulses (dust/crackle).
    // timbre  = density (impulses per second)
    // color   = color of each impulse (filtered noise vs click)
    // harmonics = ring, none at 0.5: the dust is sent through a resonator tuned by the key (the
    //             project's SVF). Above 0.5 a band-pass at the note that rings harder as it rises
    //             (Q 1..30): pitched particles. Below 0.5 a resonant high-pass that climbs from
    //             the note to four octaves above it: thinner, hissing dust.
    // morph   = mix of the raw particles and the coloured ones
    // ===================================================================
    float processParticleNoise()
    {
        // Density: impulses per second (1 to ~2000)
        float density = 1.0f + timbre * 2000.0f;
        float probability = density / (float)sampleRate;

        // Simple deterministic-ish random (no heap, uses phase accumulator)
        dustSeed = dustSeed * 1664525u + 1013904223u;
        float r = (float)(dustSeed >> 8) / 16777216.0f; // 0..1

        float sample = 0.0f;
        if (r < probability)
        {
            // Generate impulse
            float impulseSign = (dustSeed & 1) ? 1.0f : -1.0f;
            sample = impulseSign;
        }

        // Color: low-pass the output (0 = raw clicks, 1 = colored noise)
        dustFilterState = dustFilterState * color * 0.99f + sample * (1.0f - color * 0.99f);

        // Mix between raw particle and filtered
        const float dust = dustFilterState * (0.5f + morph * 0.5f) + sample * (1.0f - morph) * 0.5f;
        if (harmonics == 0.5f && ! shaperOn)
            return dust;

        const float amount = std::abs(harmonics - 0.5f) * 2.0f;
        const bool ring = harmonics > 0.5f;
        if (! shaperOn)
        {
            shaperOn = true;            // from here to the end of the note
            shaper.reset();
            ringAmount = -1.0f;
        }
        if (amount != ringAmount || ring != ringBand || (float) baseFrequency != ringHz)
        {
            ringAmount = amount;
            ringBand = ring;
            ringHz = (float) baseFrequency;
            const float q = ring ? std::pow(30.0f, amount) : 0.707f + amount * 4.0f;
            shaper.setMode(ring ? SVFilter::Mode::BandPass : SVFilter::Mode::HighPass);
            shaper.setResonance(q);
            shaper.setCutoff(ring ? ringHz : ringHz * std::pow(16.0f, amount));
            // The band-pass output has a gain of Q at its centre; a band f / Q wide of the dust
            // carries sqrt(pi f / (Q sr)) of its level. Together: about the level of the dust.
            ringGain = ring ? std::min(RING_MAX_GAIN, std::sqrt((float) sampleRate / (dsp::PI_F * std::max(20.0f, ringHz) * q))) : 1.0f;
        }
        const float wet = shaper.process(dust) * ringGain;
        const float mix = std::min(1.0f, amount * 2.0f);
        return dust + (wet - dust) * mix;
    }

    // ===================================================================
    // Model 7 — Bowed String (simplified waveguide)
    //
    // A feedback delay line excited by a bow friction model.
    // timbre  = bow pressure: how far the bow holds on to the string before it slips (the slope
    //           of the friction curve), from a light, glassy bow to a pressed, rough one
    // color   = bow position along the string
    // harmonics = damping of the string (the low-pass in the loop)
    // morph   = bow speed
    // ===================================================================
    float processBowedString()
    {
        float delaySamples = (float)(sampleRate / baseFrequency);

        // Read from delay line with linear interpolation
        float readPos = (float)delayWritePos - delaySamples;
        if (readPos < 0.0f)
            readPos += DELAY_LINE_SIZE;
        int readIdx0 = ((int)readPos) % (int)DELAY_LINE_SIZE;
        int readIdx1 = (readIdx0 + 1) % (int)DELAY_LINE_SIZE;
        float frac = readPos - std::floor(readPos);

        float delayOut = delayLine[readIdx0] * (1.0f - frac) + delayLine[readIdx1] * frac;

        // Bow velocity (from morph parameter — represents bow speed)
        float bowVelocity = 0.2f + morph * 0.6f;

        // Velocity difference between bow and string
        float vDiff = bowVelocity - delayOut;

        // Bow friction curve: f(v) = v * exp(-a * v^2) — stick-slip model. The pressure sets how
        // steep it is, over a range wide enough to be heard at the speeds the string moves at.
        float bowPressure = BOW_PRESSURE_MIN * std::pow(BOW_PRESSURE_RANGE, timbre);
        float friction = vDiff * std::exp(-bowPressure * vDiff * vDiff);

        // Apply friction force
        float excitation = friction * 0.5f;

        // Bow position comb filter (similar to pick position in KS)
        float bowPos = 0.05f + color * 0.45f; // 5%-50% along string
        int bowDelay = juce::jlimit(1, (int)delaySamples / 2,
                                     (int)(bowPos * delaySamples));
        float bowReadPos = (float)delayWritePos - (float)bowDelay;
        if (bowReadPos < 0.0f)
            bowReadPos += DELAY_LINE_SIZE;
        int bowIdx = (int)bowReadPos % (int)DELAY_LINE_SIZE;
        float bowReflection = (delayOut + delayLine[bowIdx]) * 0.5f;

        // One-pole damping filter in the loop
        float dampCoeff = 0.03f + harmonics * harmonics * 0.88f;     // 0.25 at the middle, as before
        bowFilterState = bowFilterState * dampCoeff + (bowReflection + excitation) * (1.0f - dampCoeff);

        // Write back to delay line
        delayLine[delayWritePos] = bowFilterState * 0.995f; // slight loss
        delayWritePos = (delayWritePos + 1) % (int)DELAY_LINE_SIZE;

        return delayOut;
    }

    // --- Helper: a band-limited saw at phase t (0..1) ---
    static float blepSaw(double t, double dt)
    {
        return 2.0f * (float) t - 1.0f - dsp::polyBLEP(t, dt);
    }

    // --- Helper: simple bandpass for formant model ---
    struct BandpassState { float s1 = 0.0f; float s2 = 0.0f; };

    float processBandpass(BandpassState& state, float input, float centerFreq, float q)
    {
        float w0 = dsp::TWO_PI_F * centerFreq / (float)sampleRate;
        w0 = juce::jlimit(0.001f, 3.1f, w0); // stability clamp
        float alpha = std::sin(w0) / (2.0f * q);
        float cosw0 = std::cos(w0);

        // Direct form II transposed biquad
        float a0 = 1.0f + alpha;
        float b0 = alpha / a0;
        float b2 = -alpha / a0;
        float a1 = -2.0f * cosw0 / a0;
        float a2 = (1.0f - alpha) / a0;

        float out = b0 * input + state.s1;
        state.s1 = /* b1=0 */ -a1 * out + state.s2;
        state.s2 = b2 * input - a2 * out;

        return out;
    }

    // --- Karplus-Strong init: fill delay line with noise burst behind writePos ---
    void initKarplusStrong()
    {
        std::fill(delayLine.begin(), delayLine.end(), 0.0f);
        float delaySamples = (float)(sampleRate / baseFrequency);
        int burstLen = juce::jlimit(1, (int)DELAY_LINE_SIZE - 1, (int)delaySamples);

        // Use a simple PRNG seeded from the note for deterministic-ish results
        uint32_t seed = (uint32_t)(currentNote * 7919 + 12345);

        // Fill noise burst one full period behind the write position so the
        // first read in processPluckedString() finds the excitation burst.
        delayWritePos = burstLen;
        for (int i = 0; i < burstLen; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            float noise = ((float)(seed >> 8) / 16777216.0f) * 2.0f - 1.0f;
            delayLine[i] = noise * currentVelocity;
        }
        ksFilterState = 0.0f;
    }

    // --- Bowed string init ---
    void initBowedString()
    {
        std::fill(delayLine.begin(), delayLine.end(), 0.0f);
        bowFilterState = 0.0f;
        bowForce = 0.0f;
    }

    void resetModelState()
    {
        phase = 0.0;
        modPhase = 0.0;
        for (auto& sp : sawPhases)
            sp = 0.0;
        ksFilterState = 0.0f;
        bowFilterState = 0.0f;
        bowForce = 0.0f;
        dustFilterState = 0.0f;
        for (auto& fs : formantState)
            fs = { 0.0f, 0.0f };
        fmFeedback[0] = fmFeedback[1] = 0.0f;
        harmStretched = false;
        shaperOn = false;
        noiseState = (uint32_t) (currentNote * 7919 + 4711) | 1u;      // the same note, the same noise
    }

    // --- Constants ---
    static constexpr size_t DELAY_LINE_SIZE = 8192; // enough for MIDI note 0 at 44100 Hz
    static constexpr int MAX_SAWS = 7;
    static constexpr int MAX_HARMONICS = 32;
    static constexpr float FM_FEEDBACK = 1.6f;          // radians of feedback at the top of the macro
    static constexpr float FORMANT_NOISE = 0.6f;        // level of the whisper against the pulse train
    static constexpr float FORMANT_GAIN = 14.0f;
    static constexpr float HARM_STRETCH = 0.012f;       // stiffness B at the top of the macro
    static constexpr float RING_MAX_GAIN = 8.0f;
    static constexpr float BOW_PRESSURE_MIN = 0.3f;
    static constexpr float BOW_PRESSURE_RANGE = 200.0f;

    // --- State ---
    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    double noteFrequency = 440.0;
    float pitchRatio = 1.0f;
    float gainMod = 1.0f;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool active = false;

    // Parameters
    Model model = Model::VirtualAnalog;
    float timbre = 0.5f;
    float color = 0.5f;
    float harmonics = 0.5f;
    float morph = 0.0f;
    float detune = 0.0f; // cents
    float level = 0.8f;

    // Envelope
    Envelope ampEnv;
    dsp::Declick declick;       // on the oscillator, before the filter (whose state smears a jump)
    dsp::Declick gainDeclick;   // on the output
    EnvelopeChain envChain;
    bool envChainEnabled = false;

    // Filter (after the model, before the amp envelope)
    VoiceFilter filter;

    // LFOs
    VoiceLfos lfos;
    bool lfoApplied = false;
    float lfoGain = 1.0f;
    float lfoPan = 0.0f;

    // Oscillator phases
    double phase = 0.0;
    double modPhase = 0.0;
    std::array<double, MAX_SAWS> sawPhases = {};

    // Delay line for KS and bowed string (pre-allocated, no heap on audio thread)
    std::array<float, DELAY_LINE_SIZE> delayLine = {};
    int delayWritePos = 0;
    float ksFilterState = 0.0f;

    // Bowed string state
    float bowFilterState = 0.0f;
    float bowForce = 0.0f;

    // Formant bandpass states
    std::array<BandpassState, 3> formantState = {};

    // Particle noise state
    uint32_t dustSeed = 42;
    float dustFilterState = 0.0f;

    // FM modulator feedback (its last two samples)
    float fmFeedback[2] = { 0.0f, 0.0f };

    // Harmonic model: levels and ratios (recomputed when a macro changes), stretched phases
    std::array<float, MAX_HARMONICS> harmLevel = {}, harmRatio = {}, harmPhase = {};
    float harmTimbre = -1.0f, harmColor = -1.0f, harmMorph = -1.0f;
    bool harmStretched = false;

    // The wavefolder's offset remover and the dust's resonator (one model at a time)
    SVFilter shaper;
    bool shaperOn = false;
    float ringAmount = -1.0f, ringHz = 0.0f, ringGain = 1.0f;
    bool ringBand = true;

    // Noise for the whisper and the drum blend
    uint32_t noiseState = 12345;
};


// ---------------------------------------------------------------------------
// MacroOscEngine — wraps VoiceAllocator + MacroOscVoice, implements EngineBase.
//
// Parameters:
//   0 = Model       (0-7 selecting the synthesis model)
//   1 = Timbre      (primary macro control, per-model meaning)
//   2 = Color       (secondary macro control, per-model meaning)
//   3 = Harmonics   (harmonic richness / count)
//   4 = Morph       (cross-fade / morphing)
//   5 = Decay       (amp envelope decay time in seconds)
//   6 = Level       (output level 0-1)
//   7 = Detune      (detuning in cents)
// ---------------------------------------------------------------------------

class MacroOscEngine : public EngineBase
{
public:
    enum Param
    {
        ModelParam      = 0,
        TimbreParam     = 1,
        ColorParam      = 2,
        HarmonicsParam  = 3,
        MorphParam      = 4,
        DecayParam      = 5,
        LevelParam      = 6,
        DetuneParam     = 7,
        EnvChainMode    = 8,   // 0=off (single ADSR), 1=series, 2=parallel
        EnvChainStages  = 9,   // number of envelope stages (1-4)
        EnvChainCombine = 10,  // 0=sum, 1=max (for parallel mode)
        AmpAttack       = 11,
        AmpDecay        = 12,
        AmpSustain      = 13,
        AmpRelease      = 14,
        TuneParam       = 15,  // semitones
        BendRangeParam  = 16,  // semitones
        FilterBase      = 100  // 100-115: the per-voice filter (VoiceFilterSection offsets)
    };

    explicit MacroOscEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t)maxVoices);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            voices[(size_t)voiceIdx].setNote(v.note, v.velocity);
            voices[(size_t)voiceIdx].noteOn();
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t)voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out.
        allocator.setVoiceSoundingQuery([this](int voiceIdx) { return voices[(size_t) voiceIdx].isActive(); });

        // Seed every parameter so getParameter is always the true value.
        for (auto& page : getParamPages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
        lfoSection.applyAll(EngineLfoSection::each(voices));
    }

    // The LFO destinations: the original nine, then the other continuous cells of the pages
    // (level, the amp envelope, the filter) and the other LFO. One list per model, because the
    // four macros carry the names they have in each model; they differ in nothing else.
    // Not offered: MODL, CHAN, STGS, COMB (choices and counts), TUNE, FINE and BEND (PTCH is the
    // pitch), and what the filter leaves out.
    static const LfoDestList& lfoDests(int model)
    {
        static const std::array<LfoDestList, 8> lists = []
        {
            std::array<LfoDestList, 8> all;
            for (int m = 0; m < 8; ++m)
            {
                const ParamPages p = pagesFor(m);
                LfoDestList l(lfoDestLabels(m), MacroOscVoice::NumLfoDests);
                l.group();
                l.add(p, LevelParam);
                l.add(p, AmpAttack);
                l.add(p, AmpDecay);
                l.add(p, AmpSustain);
                l.add(p, AmpRelease);
                VoiceFilterSection::addLfoDests(l, p, FilterBase, "FMOR");
                l.addCross();
                // The mod strip until the STRIP page says otherwise: the fourth macro to its top,
                // the first a little higher, and a quarter-tone of vibrato.
                l.stripRoute(0, MacroOscVoice::LfoMorph, 1.0f);
                l.stripRoute(1, MacroOscVoice::LfoTimbre, 0.3f);
                l.stripVibrato(0.25f);
                all[(size_t) m] = l;
            }
            return all;
        }();
        static const bool registered = [] { for (auto& l : lists) l.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return lists[(size_t) juce::jlimit(0, 7, model)];
    }

    // The names of the original destinations in each model.
    static const char* const* lfoDestLabels(int model)
    {
        static const char* const labels[8][MacroOscVoice::NumLfoDests] = {
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "SAWS", "SPRD", "HARM", "MORP" },   // virtual analog
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "INDX", "RATI", "DETN", "FDBK" },   // 2-op FM
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "FOLD", "SHPE", "BIAS", "CHEB" },   // wavefolder
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "VOWL", "SHFT", "Q",    "MORP" },   // formant
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "TILT", "EVOD", "NUM",  "STRC" },   // harmonic
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "DAMP", "PICK", "FDBK", "MORP" },   // plucked string
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "DENS", "COLR", "RING", "MIX"  },   // particle noise
            { "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "PRES", "POS",  "DAMP", "SPD"  } }; // bowed string
        return labels[juce::jlimit(0, 7, model)];
    }

    // MACRO (the model and its four macros, tuning, level) and AMP (envelope and envelope chain).
    // The macro cells are named after what they do in the selected model.
    ParamPages getParamPages() const override
    {
        int m = 0;
        auto it = params.find(ModelParam);
        if (it != params.end())
            m = juce::jlimit(0, 7, (int) it->second);

        ParamPages out = pagesFor(m);
        filterSection.updateModeCell(out);
        lfoSection.appendPages(out, lfoDests(m).labels());
        return out;
    }

    // The pages of model m without the two LFO pages.
    static ParamPages pagesFor(int m)
    {
        static const char* const models[] = { "VA", "FM", "FOLD", "FORM", "HARM", "PLCK", "DUST", "BOW" };
        static const char* const chain[] = { "OFF", "SER", "PAR" };
        static const char* const combine[] = { "SUM", "MAX" };
        //                                       timbre  color   harm    morph
        static const char* const names[8][4] = {
            { "SAWS", "SPRD", "HARM", "MORP" },   // virtual analog
            { "INDX", "RATI", "DETN", "FDBK" },   // 2-op FM
            { "FOLD", "SHPE", "BIAS", "CHEB" },   // wavefolder
            { "VOWL", "SHFT", "Q",    "MORP" },   // formant
            { "TILT", "EVOD", "NUM",  "STRC" },   // harmonic
            { "DAMP", "PICK", "FDBK", "MORP" },   // plucked string
            { "DENS", "COLR", "RING", "MIX"  },   // particle noise
            { "PRES", "POS",  "DAMP", "SPD"  } }; // bowed string
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        m = juce::jlimit(0, 7, m);

        ParamPages out(2);
        out[0].name = "MACRO"; out[0].section = "SRC";
        out[0].params = {{
            { "MODL", ModelParam, 0, 7, 0, C, "", models, 8 },
            { names[m][0], TimbreParam, 0, 1, 0.5f, L },
            { names[m][1], ColorParam, 0, 1, 0.5f, L },
            { names[m][2], HarmonicsParam, 0, 1, 0.5f, L },
            { names[m][3], MorphParam, 0, 1, 0, L },
            { "TUNE", TuneParam, -24, 24, 0, I, "st" },
            { "FINE", DetuneParam, -100, 100, 0, L, "ct" },
            { "LEVL", LevelParam, 0, 1, 0.8f, L } }};
        out[1].name = "AMP"; out[1].section = "AMP";
        out[1].params = {{
            { "ATK",  AmpAttack, 0.001f, 10, 0.01f, G, "s" },
            { "DEC",  AmpDecay, 0.001f, 10, 0.3f, G, "s" },
            { "SUS",  AmpSustain, 0, 1, 0.7f, L },
            { "REL",  AmpRelease, 0.001f, 10, 0.3f, G, "s" },
            { "CHAN", EnvChainMode, 0, 2, 0, C, "", chain, 3 },
            { "STGS", EnvChainStages, 1, 4, 2, I },
            { "COMB", EnvChainCombine, 0, 1, 0, C, "", combine, 2 },
            { "BEND", BendRangeParam, 0, 24, 2, I, "st" } }};
        VoiceFilterSection::appendPages(out, FilterBase);
        return out;
    }

    void setTempo(float bpm) override { lfoSection.setTempo(bpm, EngineLfoSection::each(voices)); }
    void pitchBend(float amount) override { bend = juce::jlimit(-1.0f, 1.0f, amount); }
    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }

    EngineType getType() const override { return EngineType::MacroOsc; }
    juce::String getName() const override { return "Macro Oscillator"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.setSampleRate(sr);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        // Once per block: tuning, bend and vibrato, and each voice's pressure (timbre, volume).
        // The mod strip: the routes of the STRIP page (by default the fourth macro, the first a
        // little, and vibrato), and the cutoff by the filter's WHL amount.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        lfoSection.stripBlock(wheelNow, numSamples, sampleRate, EngineLfoSection::each(voices),
                              [this](int key) { setParameter(key, getParameter(key)); });
        vibratoPhase += 5.5 * numSamples / sampleRate;
        vibratoPhase -= std::floor(vibratoPhase);
        const float vib = std::sin((float) vibratoPhase * dsp::TWO_PI_F);
        const float basePitch = tune + fine / 100.0f + bend * bendRange;
        for (int v = 0; v < (int) voices.size(); ++v)
        {
            float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = (aftertouchRouter ? *aftertouchRouter : allocator.getAftertouchRouter()).computeModulation(pressure);
            auto& voice = voices[(size_t) v];
            voice.setTimbre(baseTimbre + m.timbreMod + m.fmDepthMod);
            voice.setMorph(baseMorph + m.wavePosMod);
            voice.setGainMod(juce::jlimit(0.0f, 2.0f, 1.0f + m.volumeMod));
            voice.setPitchOffset(basePitch + vib * m.vibratoMod);
            voice.getFilter().setExternalOctaves(filterSection.externalOctaves(wheelNow, m.cutoffMod));
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // An LFO routed to PAN moves its own voice between the channels (balance law: the
            // centre is unchanged, so a sound without that routing is exactly as before).
            float sampleL = 0.0f, sampleR = 0.0f;
            for (auto& voice : voices)
            {
                if (! voice.isActive())
                    continue;
                // LFOs on page cells: this voice's own value of each (as of its last sample).
                if (voice.getLfos().keyed())
                    voice.getLfos().forEachKeyed([&voice](int key, float value) { applyVoiceParam(voice, key, value); });
                const float s = voice.process();
                const float pan = voice.getLfoPan();
                sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
            }

            sampleL *= masterLevel;
            sampleR *= masterLevel;

            if (right)
            {
                left[i] += sampleL;
                right[i] += sampleR;
            }
            else
            {
                left[i] += 0.5f * (sampleL + sampleR);
            }
        }

        lfoSection.endBlock(numSamples, EngineLfoSection::each(voices));
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    void aftertouch(int note, float pressure) override
    {
        allocator.setAftertouch(note, pressure);
    }

    // ---- Per-stage envelope curves (StageCurveParam): the amp envelope is envelope slot 0 -------
    int getStageCurveParam(ParamId cellId) const override
    {
        if (cellId == AmpAttack)  return StageCurveParam::id(0, StageCurveParam::Attack);
        if (cellId == AmpDecay)   return StageCurveParam::id(0, StageCurveParam::Decay);
        if (cellId == AmpRelease) return StageCurveParam::id(0, StageCurveParam::Release);
        return filterSection.stageCurveParamFor(cellId);
    }

private:
    StageCurves stageCurves;

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        if (StageCurveParam::envelopeOf(id) != 0)
            return;
        for (auto& v : voices)
        {
            auto p = v.getAmpEnvelope().getParams();
            stageCurves.applyTo(p, 0);
            v.getAmpEnvelope().setParams(p);
        }
        refreshChainStages();
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (filterSection.owns(id))
        {
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            lfoSection.baseChanged(id, filterSection.get(id), EngineLfoSection::each(voices));
            return;
        }
        if (lfoSection.owns(id))
        {
            // A cell an LFO has let go of goes back to its set value on every voice.
            const int released = lfoSection.set(id, value, EngineLfoSection::each(voices),
                                                [this](int key) { return getParameter(key); });
            if (released >= 0)
                setParameter(released, getParameter(released));
            return;
        }
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        params[id] = value;
        // The legacy decay id and the amp decay are the same control.
        if (id == DecayParam) params[AmpDecay] = value;
        if (id == AmpDecay) params[DecayParam] = value;
        applyParameter(id, value);
        lfoSection.baseChanged(id == DecayParam ? (int) AmpDecay : id, value, EngineLfoSection::each(voices));
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (lfoSection.owns(id))
            return lfoSection.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (EngineLfoSection::owns(id))
            return EngineLfoSection::name(id);
        switch (id)
        {
            case TuneParam:       return "Tune";
            case BendRangeParam:  return "Bend Range";
            case ModelParam:      return "Model";
            case TimbreParam:     return "Timbre";
            case ColorParam:      return "Color";
            case HarmonicsParam:  return "Harmonics";
            case MorphParam:      return "Morph";
            case DecayParam:      return "Decay";
            case LevelParam:      return "Level";
            case DetuneParam:     return "Detune";
            case EnvChainMode:    return "Env Chain";
            case EnvChainStages:  return "Env Stages";
            case EnvChainCombine: return "Env Combine";
            case AmpAttack:       return "Attack";
            case AmpDecay:        return "Amp Decay";
            case AmpSustain:      return "Sustain";
            case AmpRelease:      return "Release";
            default:              return "Param " + juce::String(id);
        }
    }

    int getParameterCount() const override { return 17; }

    MacroOscVoice& getVoice(int index) { return voices[(size_t)index]; }
    VoiceAllocator& getAllocator() { return allocator; }

private:
    // One voice's own value of a page cell: what an LFO routed to that cell sets, sample by
    // sample. Envelope times act on the stage that is running (the chain's stages are copies).
    static void applyVoiceParam(MacroOscVoice& voice, ParamId id, float value)
    {
        if (id >= FilterBase)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        if (id == LevelParam)
        {
            voice.setLevel(value);
            return;
        }
        auto setEnvelope = [id, value](Envelope& e)
        {
            switch (id)
            {
                case AmpAttack:  e.setAttack(value); break;
                case AmpDecay:   e.setDecay(value); break;
                case AmpSustain: e.setSustain(value); break;
                case AmpRelease: e.setRelease(value); break;
                default: break;
            }
        };
        setEnvelope(voice.getAmpEnvelope());
        auto& chain = voice.getEnvelopeChain();
        for (int s = 0; s < chain.getStageCount(); ++s)
            setEnvelope(chain.getEnvelopeMut(s));
    }

    void applyParameter(ParamId id, float value)
    {
        switch (id)
        {
            case ModelParam:
            {
                int modelIdx = juce::jlimit(0, (int)MacroOscVoice::Model::Count - 1, (int)value);
                for (auto& v : voices)
                    v.setModel(static_cast<MacroOscVoice::Model>(modelIdx));
                break;
            }
            case TimbreParam:
                baseTimbre = value;
                for (auto& v : voices) v.setTimbre(value);
                break;
            case TuneParam:       tune = value; break;
            case BendRangeParam:  bendRange = value; break;
            case ColorParam:
                for (auto& v : voices) v.setColor(value);
                break;
            case HarmonicsParam:
                for (auto& v : voices) v.setHarmonics(value);
                break;
            case MorphParam:
                baseMorph = value;
                for (auto& v : voices) v.setMorph(value);
                break;
            case DecayParam:
                for (auto& v : voices)
                {
                    auto p = v.getAmpEnvelope().getParams();
                    p.decay = value;
                    v.getAmpEnvelope().setParams(p);
                }
                refreshChainStages();
                break;
            case LevelParam:
                for (auto& v : voices) v.setLevel(value);
                break;
            case DetuneParam:
                fine = value;
                for (auto& v : voices) v.setDetune(value);
                break;
            case EnvChainMode:
            {
                int modeVal = juce::jlimit(0, 2, (int)value);
                if (modeVal == 0)
                {
                    // Off: use single ADSR
                    for (auto& v : voices)
                        v.setEnvelopeChainEnabled(false);
                }
                else
                {
                    auto chainMode = (modeVal == 1)
                        ? EnvelopeChain::ChainMode::Series
                        : EnvelopeChain::ChainMode::Parallel;
                    for (auto& v : voices)
                    {
                        v.setEnvelopeChainEnabled(true);
                        v.getEnvelopeChain().setMode(chainMode);
                    }
                    // A chain needs stages: build them from the current envelope if there are none.
                    if (voices.empty() || voices[0].getEnvelopeChain().getStageCount() == 0)
                        rebuildEnvelopeChains(juce::jmax(1, envChainStageCount));
                }
                break;
            }
            case EnvChainStages:
            {
                int stages = juce::jlimit(1, 4, (int)value);
                rebuildEnvelopeChains(stages);
                break;
            }
            case EnvChainCombine:
            {
                auto cm = ((int)value == 0)
                    ? EnvelopeChain::CombineMode::Sum
                    : EnvelopeChain::CombineMode::Max;
                for (auto& v : voices)
                    v.getEnvelopeChain().setCombineMode(cm);
                break;
            }
            case AmpAttack:
                for (auto& v : voices)
                {
                    auto p = v.getAmpEnvelope().getParams();
                    p.attack = value;
                    v.getAmpEnvelope().setParams(p);
                }
                refreshChainStages();
                break;
            case AmpDecay:
                for (auto& v : voices)
                {
                    auto p = v.getAmpEnvelope().getParams();
                    p.decay = value;
                    v.getAmpEnvelope().setParams(p);
                }
                refreshChainStages();
                break;
            case AmpSustain:
                for (auto& v : voices)
                {
                    auto p = v.getAmpEnvelope().getParams();
                    p.sustain = value;
                    v.getAmpEnvelope().setParams(p);
                }
                refreshChainStages();
                break;
            case AmpRelease:
                for (auto& v : voices)
                {
                    auto p = v.getAmpEnvelope().getParams();
                    p.release = value;
                    v.getAmpEnvelope().setParams(p);
                }
                refreshChainStages();
                break;
            default:
                break;
        }
    }

    // Rebuild envelope chains for all voices with the given number of stages.
    // Uses the current amp envelope params as the template for each stage.
    // Called from applyParameter (not audio thread — param changes are UI-thread).
    void rebuildEnvelopeChains(int stages)
    {
        envChainStageCount = stages;
        for (auto& v : voices)
        {
            auto& chain = v.getEnvelopeChain();
            chain.reset();
            // Clear and rebuild: EnvelopeChain uses a fixed-size array,
            // so we create a fresh chain object and copy the mode settings.
            auto savedMode = chain.getMode();
            auto savedCombine = chain.getCombineMode();

            // We need to reconstruct — EnvelopeChain has no removeEnvelope,
            // so we create a new one and copy settings.
            EnvelopeChain newChain;
            newChain.setSampleRate(sampleRate);
            newChain.setMode(savedMode);
            newChain.setCombineMode(savedCombine);

            auto envParams = v.getAmpEnvelope().getParams();
            for (int s = 0; s < stages; ++s)
                newChain.addEnvelope(envParams);

            v.getEnvelopeChain() = newChain;
        }
    }

    // Chain stages are copies of the amp envelope: keep them in step with it.
    void refreshChainStages()
    {
        for (auto& v : voices)
        {
            auto& chain = v.getEnvelopeChain();
            auto p = v.getAmpEnvelope().getParams();
            for (int s = 0; s < chain.getStageCount(); ++s)
                chain.getEnvelopeMut(s).setParams(p);
        }
    }

    double sampleRate = 44100.0;
    float masterLevel = 0.8f;
    float baseTimbre = 0.5f, baseMorph = 0.0f;
    float tune = 0.0f, fine = 0.0f, bend = 0.0f, bendRange = 2.0f, wheel = 0.0f;
    float wheelNow = 0.0f;                              // the strip, smoothed per block
    double vibratoPhase = 0.0;
    int envChainStageCount = 2;
    VoiceFilterSection filterSection { FilterBase };
    EngineLfoSection lfoSection { lfoDests(0) };
    VoiceAllocator allocator;
    std::vector<MacroOscVoice> voices;
    ParamMap params;
};
