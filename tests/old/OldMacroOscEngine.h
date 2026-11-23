#pragma once

// The macro oscillator as it was at 46fedbc (src/engine/MacroOscEngine.h, already on the modulation
// router), before U4-MACRO: PLCK, DUST and BOW as models, no ANLG, no BODY, its randoms seeded (0, voice),
// the envelope chain's plumbing its own. Only the class names and the include paths differ.
// tests/u4_macro_tests.h compares it against the engine as it is.

#include "../../src/engine/EngineBase.h"
#include "../../src/engine/VoiceAllocator.h"
#include "../../src/engine/VoiceFilterSection.h"
#include "../../src/engine/ModRouter.h"
#include "../../src/dsp/Envelope.h"
#include "../../src/dsp/EnvelopeChain.h"
#include "../../src/dsp/DSPUtils.h"
#include "../../src/dsp/filters/SVFilter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// OldMacroOscVoice — Plaits-inspired macro oscillator with multiple models.
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

class OldMacroOscVoice
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

    // The destinations the voice reads by name: indices of the engine's table (OldMacroOscEngine::
    // lfoDests), in the order of the DEST cells. The four macros are page cells (moved by +-1
    // around their knobs, held to 0..1).
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // per sample: a frequency ratio (+-12 semitones, amount squared)
        LfoAmp,         // per sample: a gain, 0..4 (an audio-rate slot may take it to -4)
        LfoPan,         // per sample: +-1
        LfoCutoff,      // per sample: octaves (+-4 at full depth)
        LfoResonance,   // +-0.5
        LfoTimbre,      // per sample, like the three after it
        LfoColor,
        LfoHarmonics,
        LfoMorph,
        NumLfoDests,
        LfoEnvTime = 27,    // ETIM: the amp envelope's time (and its chain's), a factor
        LfoFilterTime,      // FTIM: the filter envelope's time, a factor
        LfoBow              // hidden (the pressure route BOW): the bow's energy, 0..1
    };

    // The voice's own sources, in the order the engine publishes them (OldMacroOscEngine::sources).
    enum Published
    {
        PubOsc = 0      // OSC: the model's output before the filter, at audio rate
    };

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        envChain.setSampleRate(sr);
        filter.setSampleRate(sr);
        mods.prepare(sr);
        declick.setSampleRate(sr);
        gainDeclick.setSampleRate(sr);
        levelStep = (float) (1.0 / (0.004 * sr));
        bowLagK = (float) (1.0 - std::exp(-1.0 / (BOW_ENERGY_LAG * std::max(sr, 1.0))));
        shaper.setSampleRate(sr);
        // The string's line is as long as the lowest pitch needs at this rate: sized here, never
        // while playing.
        const size_t length = delayLineLength(sr);
        if (delayLine.size() != length)
            delayLine.assign(length, 0.0f);
        else
            std::fill(delayLine.begin(), delayLine.end(), 0.0f);
        delayWritePos = 0;
        delayDirty = 0;
    }

    // The lowest pitch the string models have a string for: MIDI note 0 with TUNE and FINE at the
    // bottom and the widest bend all the way down (49 semitones below it, 0.482 Hz). An LFO or
    // pressure pulling further stops there: the pitch is held, never the read position.
    static constexpr double STRING_MIN_HZ = 0.48;
    static constexpr double MAX_STRING_SAMPLE_RATE = 192000.0;     // above it the lowest pitch rises with the rate

    // Slots in the string's line at a sample rate: 400 kB a voice at 48 kHz.
    static size_t delayLineLength(double sr)
    {
        const double rate = sr >= 1.0 ? std::min(sr, MAX_STRING_SAMPLE_RATE) : 44100.0;
        return (size_t) std::ceil(rate / STRING_MIN_HZ) + 4;
    }

    // The string's length in samples for a pitch, always one the line can give: 1 to
    // lineLength - 2, whatever the pitch is (zero and not-a-number are the longest string).
    static float stringDelay(double sr, double hz, int lineLength)
    {
        const double longest = (double) (lineLength - 2);
        const double d = sr / hz;
        if (! (d <= longest))
            return (float) longest;
        return d < 1.0 ? 1.0f : (float) d;
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

    void noteOn(const mod::Router& router)
    {
        if (active)
        {
            declick.arm();      // struck again while sounding: the restart must not jump,
            gainDeclick.arm();  // nor the step to the new velocity
        }
        // (Whatever is routed is read before the note's first sample: mod::Voice::tick.)
        mods.noteOn(router, currentNote, currentVelocity);
        filter.noteOn(currentNote, currentVelocity, active);
        ampEnv.noteOn();
        if (envChainEnabled)
            envChain.noteOn();
        active = true;
        struck = true;
        // The bow is the model's own until this note's pad is pressed (setExcite).
        bowPressed = false;
        bowEnergy = 1.0f;

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

    // One sub-block of n samples, added to left / right. pressure: this voice's pad; bowIsRouted:
    // a slot is aimed at the bow's energy; setCell(voice, dest, value) sets a page cell of this
    // voice (a control-rate destination of the router).
    template <typename SetCell>
    void render(const mod::Router& router, float* left, float* right, int n, float pressure, bool bowIsRouted, const SetCell& setCell)
    {
        const bool moved = mods.tick(router, n, ampEnv.getCurrentLevel(), filter.getEnvelope().getCurrentLevel(), pressure);
        setExcite(bowIsRouted, pressure, mods.moved(LfoBow) ? mods.value(LfoBow) : 0.0f);
        // OSC, the voice's own source: kept and handed over only while a slot reads it. (The
        // router has taken what the last sub-block made; the slots hear these samples in the next.)
        float* tap = nullptr;
        if (router.wants(PubOsc))
        {
            tap = oscOut.data();
            std::fill(tap, tap + n, 0.0f);
            mods.publishAudio(PubOsc, tap, n);
            oscTapped = true;
        }
        else if (oscTapped)
        {
            mods.publishAudio(PubOsc, nullptr, 0);
            oscTapped = false;
        }
        if (! moved)
        {
            // Nothing moves this voice: the oscillator as it is without any modulation.
            if (cutMoved)
            {
                filter.setLfoOctaves(0.0f);
                cutMoved = false;
            }
            for (int i = 0; i < n && active; ++i)
            {
                const float s = processSample(1.0f);
                if (tap) tap[i] = lastOsc;
                left[i] += s;
                right[i] += s;
            }
            return;
        }

        mods.forEachControl([&](int dest, float value) { setControl(dest, value, setCell); });
        const float* pitch = mods.buffer(LfoPitch);
        const float* amp = mods.buffer(LfoAmp);
        const float* pan = mods.buffer(LfoPan);
        const float* cutoff = mods.buffer(LfoCutoff);
        const float* macro[4] = { mods.buffer(LfoTimbre), mods.buffer(LfoColor), mods.buffer(LfoHarmonics), mods.buffer(LfoMorph) };
        if (cutoff == nullptr && cutMoved)
            filter.setLfoOctaves(0.0f);
        cutMoved = cutoff != nullptr;

        if (! (pitch || pan || cutoff || macro[0] || macro[1] || macro[2] || macro[3]))
        {
            // The level alone (a pad's pressure on the volume), or only cells set above.
            for (int i = 0; i < n && active; ++i)
            {
                const float s = processSample(amp ? amp[i] : 1.0f);
                if (tap) tap[i] = lastOsc;
                left[i] += s;
                right[i] += s;
            }
            return;
        }

        // The macros and the pitch are moved for these samples only (the set values stay).
        const float setTimbre = timbre, setColor = color, setHarmonics = harmonics, setMorph = morph;
        const double setFrequency = baseFrequency;
        for (int i = 0; i < n && active; ++i)
        {
            if (macro[0]) timbre = macro[0][i];
            if (macro[1]) color = macro[1][i];
            if (macro[2]) harmonics = macro[2][i];
            if (macro[3]) morph = macro[3][i];
            if (pitch)    baseFrequency = setFrequency * (double) pitch[i];
            if (cutoff)   filter.setLfoOctaves(cutoff[i]);
            const float s = processSample(amp ? amp[i] : 1.0f);
            if (tap) tap[i] = lastOsc;
            // PAN moves its own voice between the channels (balance law: the centre is unchanged).
            const float p = pan ? pan[i] : 0.0f;
            left[i] += p > 0.0f ? s * (1.0f - p) : s;
            right[i] += p < 0.0f ? s * (1.0f + p) : s;
        }
        timbre = setTimbre;
        color = setColor;
        harmonics = setHarmonics;
        morph = setMorph;
        baseFrequency = setFrequency;
    }

    // A control-rate destination of the router: the resonance, or one of this voice's page cells.
    template <typename SetCell>
    void setControl(int dest, float value, const SetCell& setCell)
    {
        if (dest == LfoResonance)
            filter.setResonanceMod(value);
        else if (dest == LfoEnvTime)
            setEnvTimeScale(value);
        else if (dest == LfoFilterTime)
            filter.setEnvTimeScale(value);
        else if (dest != LfoBow)
            setCell(*this, dest, value);
    }

    // ETIM: every stage time of the amp envelope, and of the chain's stages, times this. Under a
    // sounding note the running stage goes on from where it is (Envelope::setTimeScale).
    void setEnvTimeScale(float scale)
    {
        ampEnv.setTimeScale(scale);
        for (int s = 0; s < envChain.getStageCount(); ++s)
            envChain.getEnvelopeMut(s).setTimeScale(scale);
    }

    // One sample. amp: the gain the router has for the level (1 = none).
    float processSample(float amp)
    {
        if (!active)
            return 0.0f;

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

        lastOsc = sample;       // (the source OSC: the model as it is, before the filter)
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

        // LEVL is moved to over a few milliseconds under a sounding note (a jump would be a
        // click); the first sample of a note takes it as it stands.
        if (struck)
        {
            struck = false;
            levelNow = level;
        }
        else if (levelNow != level)
            levelNow = dsp::towards(levelNow, level, levelStep);
        return gainDeclick.process(sample * env * currentVelocity * levelNow * amp);
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
        clearString();
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
    // Pad pressure as the bow's energy (the pressure route BOW; the bowed string only). Once a
    // sub-block: is there such a route, this voice's pressure, and what the route makes of it. A
    // voice that has had pressure is bowed by it from then on - 0 lifts the bow and the string
    // rings out - until its next note. Without the route, or from the keys (no pressure ever
    // comes), the bow is the model's own, exactly as before. The same rule as the resonator's EXC.
    void setExcite(bool routed, float pressure, float routedEnergy)
    {
        bowRouted = routed;
        if (routed && pressure > 0.0f)
            bowPressed = true;
        bowTarget = std::isfinite(routedEnergy) ? juce::jlimit(0.0f, 1.0f, routedEnergy) : 0.0f;
    }
    float getBowEnergy() const      { return bowEnergy; }  // 1 = the model's own bow
    // Tuning, fine tune, bend and vibrato together, in semitones.
    void setPitchOffset(float semitones)
    {
        pitchRatio = std::exp2(semitones / 12.0f);
        baseFrequency = noteFrequency * (double) pitchRatio;
    }
    Envelope& getAmpEnvelope()      { return ampEnv; }
    VoiceFilter& getFilter()        { return filter; }
    mod::Voice& getMod()            { return mods; }

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

        // The partials' levels and ratios change only when a macro does, and under a macro that
        // moves all the time (an LFO, the input) no more often than every eighth sample: 32
        // powers a sample was most of this model's cost under modulation.
        if (harmWait > 0)
            --harmWait;
        else if (timbre != harmTimbre || color != harmColor || morph != harmMorph)
        {
            harmWait = HARM_REFRESH - 1;
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
        const float delaySamples = stringDelay(sampleRate, baseFrequency, (int) delayLine.size());
        const int delayLen = (int) delaySamples;

        float sample = readString(delaySamples);

        // Pick position: comb filter (notch at harmonics of the pick position)
        if (color > 0.01f)
        {
            int pickDelay = juce::jlimit(1, juce::jmax(1, delayLen / 2), (int)(color * (float)delayLen * 0.5f));
            sample = (sample + delayLine[tapIndex(pickDelay)]) * 0.5f;
        }

        // Low-pass filter in feedback loop (damping from timbre)
        float damping = 0.1f + timbre * 0.85f; // 0.1 (very bright) to 0.95 (very dull)
        ksFilterState = ksFilterState * damping + sample * (1.0f - damping);

        // Write back with feedback
        float feedback = 0.95f + harmonics * 0.049f; // very close to 1 for long sustain
        if (morph > 0.0f && dsp::xorshiftFloat(noiseState) < morph * 0.5f)
            feedback = -feedback;
        writeString(ksFilterState * feedback);

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
    //
    // Pad pressure as the bow's energy e (setExcite; 1 = the bow as the cells set it, which is
    // what full pressure gives and what a voice without pressure always has): the bow's speed is
    // SPD x e; below e = 1 the bow also presses less (PRES falls by up to half its range) and the
    // string is damped more (towards a dull 0.9), so more pressure is louder and brighter; under
    // an eighth the bow lets go of the string (its force, and the damping it added, fade to
    // nothing at 0) and the string rings out by itself. e follows the pressure with a lag of 15 ms. The values were chosen by
    // measurement: level and brightness both rise with e from 0.15 to 1 at the default cells.
    // ===================================================================
    float processBowedString()
    {
        // A string in two parts, from the bow to the nut and from the bow to the bridge, each a
        // delay that ends in an inverting reflection; the bow sits between them and lets the
        // string go when it moves too differently from the bow (stick and slip). The two parts
        // share the line, one in the even slots and one in the odd, so each has half of it: the
        // lowest pitch is an octave above the plucked string's (0.96 Hz), and is held there below.
        const int pairs = (int) delayLine.size() / 2;
        if (delayWritePos >= pairs)                     // (the model was changed under a sounding note)
            delayWritePos = 0;
        float dampCoeff = 0.03f + harmonics * harmonics * 0.88f;           // the bridge's low-pass
        float bowVelocity = BOW_SPEED_MIN + morph * BOW_SPEED_RANGE;
        float slope = 5.0f - 4.0f * timbre;
        float bowForce01 = 1.0f;
        // Pressure as the bow's energy: only for a voice whose pad has been pressed with a BOW
        // route (or that is on its way back from there); every other voice skips this whole.
        const float energyTarget = bowRouted && bowPressed ? bowTarget : 1.0f;
        const bool bowed = bowEnergy != energyTarget || energyTarget != 1.0f;
        if (bowed)
        {
            bowEnergy += (energyTarget - bowEnergy) * bowLagK;
            if (std::abs(energyTarget - bowEnergy) < BOW_ENERGY_SNAP)
                bowEnergy = energyTarget;
            const float e = bowEnergy, short01 = 1.0f - e;
            bowForce01 = std::min(1.0f, e * BOW_LIFT);
            bowVelocity *= e;
            slope = 5.0f - 4.0f * juce::jlimit(0.0f, 1.0f, timbre - BOW_ENERGY_PRESSURE * short01);
            // (The damping is the bow's weight on the string: it goes as the bow lets go, and
            // the string rings out as bright and as long as the DAMP cell has it.)
            if (dampCoeff < BOW_SOFT_DAMP)
                dampCoeff += (BOW_SOFT_DAMP - dampCoeff) * BOW_ENERGY_DAMP * short01 * bowForce01;
        }
        bowForce = bowForce01;
        // One period is both parts and what the low-pass holds back.
        const float period = stringDelay(sampleRate, baseFrequency, pairs) - dampCoeff / (1.0f - dampCoeff);
        const float total = juce::jlimit(2.0f, (float) (pairs - 2), period);
        const float bowPos = 0.05f + color * 0.45f;                        // 5 % .. 50 % from the bridge
        const float bridgeLen = juce::jmax(1.0f, total * bowPos);
        const float neckLen = juce::jmax(1.0f, total - bridgeLen);

        bowFilterState = bowFilterState * dampCoeff + readBow(1, bridgeLen) * (1.0f - dampCoeff);
        const float fromBridge = -BOW_BRIDGE_GAIN * bowFilterState;
        const float fromNut = -readBow(0, neckLen);
        const float stringVelocity = fromBridge + fromNut;

        // The bow: its speed (SPD) against the string's. Friction falls off steeply once they
        // differ; more pressure (PRES) holds on over a wider difference.
        const float difference = bowVelocity - stringVelocity;
        const float x = std::abs(difference * slope) + 0.75f;
        const float grip = juce::jlimit(0.01f, 0.98f, 1.0f / (x * x * x * x));
        const float pushed = bowed ? difference * grip * bowForce01 : difference * grip;

        const size_t at = (size_t) (2 * delayWritePos);
        delayLine[at] = fromBridge + pushed;            // towards the nut
        delayLine[at + 1] = fromNut + pushed;           // towards the bridge
        if ((int) at + 2 > delayDirty)
            delayDirty = (int) at + 2;
        if (++delayWritePos >= pairs)
            delayWritePos = 0;

        return fromBridge * BOW_OUTPUT_GAIN;
    }

    // One of the bowed string's two parts (0 = to the nut, 1 = to the bridge), `length` samples
    // ago, between two slots. Always inside the line.
    float readBow(int part, float length) const
    {
        const int pairs = (int) delayLine.size() / 2;
        const int whole = (int) length;
        const float frac = length - (float) whole;
        auto slot = [&](int back)
        {
            int i = delayWritePos - juce::jlimit(1, pairs - 1, back);
            if (i < 0) i += pairs;
            return delayLine[(size_t) (2 * i + part)];
        };
        return slot(whole) * (1.0f - frac) + slot(whole + 1) * frac;
    }

    // --- The string's line (both string models) ---
    // The slot written `back` samples ago (1 = the last one). Held to 1..size - 1, so that one
    // wrap always lands inside the line.
    size_t tapIndex(int back) const
    {
        const int size = (int) delayLine.size();
        const int i = delayWritePos - juce::jlimit(1, size - 1, back);
        return (size_t) (i < 0 ? i + size : i);
    }

    // The string `delaySamples` ago (from stringDelay), between two slots. The whole samples and
    // the fraction are taken apart before the write position comes in: one float holding both
    // has no bits left for the fraction far into a long line.
    float readString(float delaySamples) const
    {
        const int whole = (int) delaySamples;
        const float frac = 1.0f - (delaySamples - (float) whole);      // towards the newer slot
        return delayLine[tapIndex(whole + 1)] * (1.0f - frac) + delayLine[tapIndex(whole)] * frac;
    }

    void writeString(float value)
    {
        delayLine[(size_t) delayWritePos] = value;
        if (delayWritePos >= delayDirty)
            delayDirty = delayWritePos + 1;
        if (++delayWritePos >= (int) delayLine.size())
            delayWritePos = 0;
    }

    // Silence in the line: only the part written since the last time (a note-on must not pay
    // for the whole line of a short note).
    void clearString()
    {
        std::fill(delayLine.begin(), delayLine.begin() + juce::jlimit(0, (int) delayLine.size(), delayDirty), 0.0f);
        delayDirty = 0;
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
        clearString();
        int burstLen = (int) stringDelay(sampleRate, baseFrequency, (int) delayLine.size());

        // Use a simple PRNG seeded from the note for deterministic-ish results
        uint32_t seed = (uint32_t)(currentNote * 7919 + 12345);

        // Fill noise burst one full period behind the write position so the
        // first read in processPluckedString() finds the excitation burst.
        delayWritePos = burstLen;
        for (int i = 0; i < burstLen; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            float noise = ((float)(seed >> 8) / 16777216.0f) * 2.0f - 1.0f;
            delayLine[(size_t) i] = noise * currentVelocity;
        }
        delayDirty = burstLen;
        ksFilterState = 0.0f;
    }

    // --- Bowed string init ---
    void initBowedString()
    {
        clearString();
        delayWritePos = 0;
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
        harmWait = 0;
        shaperOn = false;
        noiseState = (uint32_t) (currentNote * 7919 + 4711) | 1u;      // the same note, the same noise
    }

    // --- Constants ---
    static constexpr int MAX_SAWS = 7;
    static constexpr int MAX_HARMONICS = 32;
    static constexpr float FM_FEEDBACK = 1.6f;          // radians of feedback at the top of the macro
    static constexpr float FORMANT_NOISE = 0.6f;        // level of the whisper against the pulse train
    static constexpr float FORMANT_GAIN = 14.0f;
    static constexpr float HARM_STRETCH = 0.012f;       // stiffness B at the top of the macro
    static constexpr float RING_MAX_GAIN = 8.0f;
    static constexpr float BOW_BRIDGE_GAIN = 0.95f;      // what the bridge gives back
    static constexpr float BOW_SPEED_MIN = 0.05f, BOW_SPEED_RANGE = 0.2f;
    static constexpr float BOW_OUTPUT_GAIN = 2.0f;       // the string moves little: up to the other models' level
    // Pressure as the bow's energy (processBowedString)
    static constexpr double BOW_ENERGY_LAG = 0.015;      // seconds: how the energy follows the pressure
    static constexpr float BOW_ENERGY_PRESSURE = 0.5f;   // how much of PRES is taken away as the energy falls to 0
    static constexpr float BOW_ENERGY_SNAP = 5.0e-4f;    // the energy has arrived (a float stops short of 1 by up to 1e-4)
    static constexpr float BOW_SOFT_DAMP = 0.9f;         // the loop's low-pass under the lightest bow ...
    static constexpr float BOW_ENERGY_DAMP = 0.9f;       // ... and how far towards it the damping goes
    static constexpr float BOW_LIFT = 8.0f;              // under 1 / 8 of the energy the bow's hold on the string fades out

    // --- State ---
    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    double noteFrequency = 440.0;
    float pitchRatio = 1.0f;
    float levelNow = 0.8f, levelStep = 0.005f;
    bool struck = true;
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

    // This voice's side of the modulation router
    mod::Voice mods;
    bool cutMoved = false;      // the filter holds a cutoff offset from the router
    // The source OSC: the model's last sample, and those of the sub-block while a slot reads it
    float lastOsc = 0.0f;
    bool oscTapped = false;
    std::array<float, mod::Router::MAX_CHUNK> oscOut {};

    // Oscillator phases
    double phase = 0.0;
    double modPhase = 0.0;
    std::array<double, MAX_SAWS> sawPhases = {};

    // Delay line for KS and bowed string (sized in setSampleRate, no heap on audio thread)
    std::vector<float> delayLine = std::vector<float>(delayLineLength(44100.0), 0.0f);
    int delayWritePos = 0;      // always inside the line
    int delayDirty = 0;         // slots from the start that may hold sound
    float ksFilterState = 0.0f;

    // Bowed string state
    float bowFilterState = 0.0f;
    float bowForce = 0.0f;          // the bow's hold on the string, 0 (lifted) .. 1, as of the last sample
    // Pad pressure as the bow's energy (setExcite)
    bool bowRouted = false, bowPressed = false;
    float bowTarget = 0.0f;         // what the pressure route asks for, 0..1
    float bowEnergy = 1.0f;         // what the bow has: 1 = the model's own
    float bowLagK = 0.0015f;

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
    static constexpr int HARM_REFRESH = 8;
    int harmWait = 0;
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
// OldMacroOscEngine — wraps VoiceAllocator + OldMacroOscVoice, implements EngineBase.
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

class OldMacroOscEngine : public EngineBase
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
        // 158-189 and 700-772 are the modulation router's (mod::Router: LFO1, LFO2, STRIP, XIN,
        // the slots, the custom LFO shape): everything that moves this engine's sound goes
        // through it, the pad's pressure routes and the track input at audio rate included.
    };

    explicit OldMacroOscEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t)maxVoices);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            voices[(size_t)voiceIdx].setNote(v.note, v.velocity);
            voices[(size_t)voiceIdx].noteOn(router);
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t)voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out.
        allocator.setVoiceSoundingQuery([this](int voiceIdx) { return voices[(size_t) voiceIdx].isActive(); });

        // The per-note randoms (RAND, RND2 ..) repeat from run to run: the same notes in the same
        // order on a new engine draw the same values. (This engine has no ANLG page, so no SEED
        // cell and no spread generator to hand over: the seed is fixed until it gets one.)
        for (int v = 0; v < maxVoices; ++v)
            voices[(size_t) v].getMod().seed(0, v);

        // Seed every parameter so getParameter is always the true value.
        for (auto& page : getParamPages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
    }

    // What can be moved (the modulation router's table): the original nine (pitch, level, pan,
    // cutoff, resonance as inputs of the voice; the four macros, which are page cells), then the
    // other continuous cells of the pages (level, the amp envelope, the filter), LRAT / LDPT, the
    // two envelope times ETIM / FTIM, and hidden, for the pressure route only, the bow's energy.
    // One table per model, because the four macros carry the names they have in each model; they
    // differ in nothing else.
    // Per sample (so the track input reaches them at audio rate): PTCH, AMP, PAN, CUT and the
    // four macros. Not offered: MODL, CHAN, STGS, COMB (choices and counts), TUNE, FINE and BEND
    // (PTCH is the pitch), and what the filter leaves out.
    static const mod::DestTable& lfoDests(int model)
    {
        static const std::array<mod::DestTable, 8> tables = []
        {
            std::array<mod::DestTable, 8> all;
            for (int m = 0; m < 8; ++m)
            {
                const ParamPages p = pagesFor(m);
                mod::DestTable& t = all[(size_t) m];
                t.group();
                t.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);      // a ratio: one octave x the amount squared
                t.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample).audioFloor(-4.0f);  // (below 0: ring modulation)
                t.input("PAN", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);
                t.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);              // octaves
                t.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
                for (int id : { (int) TimbreParam, (int) ColorParam, (int) HarmonicsParam, (int) MorphParam })
                    t.add(p, id, nullptr, mod::PerSample);
                t.group();
                t.add(p, LevelParam);
                t.add(p, AmpAttack);
                t.add(p, AmpDecay);
                t.add(p, AmpSustain);
                t.add(p, AmpRelease);
                VoiceFilterSection::addLfoDests(t, p, FilterBase, "FMOR");
                t.addCross();
                // The envelopes' times (after what was there before: a stored DEST keeps its meaning).
                t.group();
                t.envTimes();
                jassert(t.ampTime() == OldMacroOscVoice::LfoEnvTime && t.filterTime() == OldMacroOscVoice::LfoFilterTime);
                // (The bow follows the pad as it is: it has a lag of its own.)
                t.hidden("BOW", 0.0f, 1.0f, 0.0f, 1.0f, mod::Steps);
                jassert(t.indexOfLabel("BOW") == OldMacroOscVoice::LfoBow);
                // The mod strip until the STRIP page says otherwise: the fourth macro to its top,
                // the first a little higher, and a quarter-tone of vibrato.
                t.stripRoute(0, OldMacroOscVoice::LfoMorph, 1.0f);
                t.stripRoute(1, OldMacroOscVoice::LfoTimbre, 0.3f);
                t.stripVibrato(0.25f);
                t.finish(2);
            }
            return all;
        }();
        static const bool registered = [] { for (auto& t : tables) t.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return tables[(size_t) juce::jlimit(0, 7, model)];
    }

    // What a slot can read here beside the router's own sources: the list SRC and VIA show.
    //   OSC   the model's output before the filter, at audio rate (OldMacroOscVoice::PubOsc): on CUT
    //         it is filter FM by the voice's own oscillator, on AMP it squares the wave, on PAN it
    //         spreads the partials, on PTCH and the macros it is feedback (one sub-block late).
    static const mod::SourceTable& sources()
    {
        static const mod::SourceTable table = []
        {
            mod::SourceTable t(2);
            t.publish("OSC", mod::AudioRate);
            return t;
        }();
        static const bool registered = [] { table.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return table;
    }

    // The track's pressure routes (AT ROUTE) as router slots: what each destination is here.
    static const mod::Router::PressureMap& pressureDests()
    {
        using D = AftertouchRouter::Destination;
        static const mod::Router::PressureMap map = []
        {
            mod::Router::PressureMap m;
            m.fill(-1);
            m[(size_t) D::Volume] = OldMacroOscVoice::LfoAmp;
            m[(size_t) D::FilterCutoff] = OldMacroOscVoice::LfoCutoff;
            m[(size_t) D::FMDepth] = OldMacroOscVoice::LfoTimbre;
            m[(size_t) D::MacroTimbre] = OldMacroOscVoice::LfoTimbre;
            m[(size_t) D::WavePosition] = OldMacroOscVoice::LfoMorph;
            m[(size_t) D::PanSpread] = OldMacroOscVoice::LfoPan;
            m[(size_t) D::Excite] = OldMacroOscVoice::LfoBow;
            return m;
        }();
        return map;
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
        router.appendPages(out);        // XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2
        return out;
    }

    // The pages of model m without the modulation router's.
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

    void setTempo(float bpm) override { router.setTempo(bpm); }
    void pitchBend(float amount) override { bend = juce::jlimit(-1.0f, 1.0f, amount); }
    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }

    EngineType getType() const override { return EngineType::MacroOsc; }
    juce::String getName() const override { return "Macro Oscillator"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        router.prepare(sr);
        for (auto& v : voices)
            v.setSampleRate(sr);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        // Once per block: tuning and bend, the strip (the cutoff by the filter's WHL amount; its
        // routes are the router's), the track's pressure routes and the track input.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        const float basePitch = tune + fine / 100.0f + bend * bendRange;
        const float wheelOctaves = filterSection.externalOctaves(wheelNow, 0.0f);
        for (auto& voice : voices)
        {
            voice.setPitchOffset(basePitch);
            voice.getFilter().setExternalOctaves(wheelOctaves);
        }
        router.setPressureRoutes(aftertouchRouter ? *aftertouchRouter : allocator.getAftertouchRouter(), pressureDests());
        router.beginBlock(audioInput.left, audioInput.right, audioInput.connected() ? audioInput.frames : 0,
                          audioInput.envelope, wheelNow, sampleRate);
        // The pressure route BOW: pad pressure is the bow's energy on the bowed string.
        const bool bowRouted = router.routed(OldMacroOscVoice::LfoBow);
        const auto setCell = [this](OldMacroOscVoice& voice, int dest, float value)
        {
            applyVoiceParam(voice, router.dests()[dest].id, value);
        };

        // Sub-blocks: the router's control rate. Inside one the voices are rendered one after
        // the other (they share the router's per-sample buffers).
        for (int offset = 0; offset < numSamples;)
        {
            const int n = router.beginChunk(offset, numSamples);
            std::fill(mixL.begin(), mixL.begin() + n, 0.0f);
            std::fill(mixR.begin(), mixR.begin() + n, 0.0f);
            for (int v = 0; v < (int) voices.size(); ++v)
            {
                auto& voice = voices[(size_t) v];
                if (! voice.isActive())
                {
                    // Its LFOs keep time; a cell it was left holding goes back to its set value.
                    if (voice.getMod().idle(router, n))
                        voice.getMod().restore(router, [&](int dest, float value) { voice.setControl(dest, value, setCell); });
                    continue;
                }
                const float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
                voice.render(router, mixL.data(), mixR.data(), n, pressure, bowRouted, setCell);
            }
            for (int i = 0; i < n; ++i)
            {
                const float sampleL = mixL[(size_t) i] * masterLevel, sampleR = mixR[(size_t) i] * masterLevel;
                if (right)
                {
                    left[offset + i] += sampleL;
                    right[offset + i] += sampleR;
                }
                else
                {
                    left[offset + i] += 0.5f * (sampleL + sampleR);
                }
            }
            offset += n;
        }
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
        soundCut();
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
            if (filterSection.swaps(id, value))
                soundSwapped();
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            router.setBase(id, filterSection.get(id));
            return;
        }
        if (mod::Router::owns(id))
        {
            router.set(id, value);
            return;
        }
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        if ((id == ModelParam || id == EnvChainMode || id == EnvChainStages || id == EnvChainCombine) && params[id] != value)
            soundSwapped();
        params[id] = value;
        // The legacy decay id and the amp decay are the same control.
        if (id == DecayParam) params[AmpDecay] = value;
        if (id == AmpDecay) params[DecayParam] = value;
        applyParameter(id, value);
        router.setBase(id == DecayParam ? (int) AmpDecay : id, value);
        if (id == ModelParam)
            router.setTable(lfoDests((int) value));     // the macros' names in this model
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (mod::Router::owns(id))
            return router.get(id);
        if (StageCurveParam::owns(id))
            return stageCurves.get(id);
        auto it = params.find(id);
        return it != params.end() ? it->second : 0.0f;
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (mod::Router::owns(id))
            return mod::Router::name(id);
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

    OldMacroOscVoice& getVoice(int index) { return voices[(size_t)index]; }
    VoiceAllocator& getAllocator() { return allocator; }

private:
    // One voice's own value of a page cell: what a slot routed to that cell sets, once a
    // sub-block. Envelope times act on the stage that is running (the chain's stages are copies).
    static void applyVoiceParam(OldMacroOscVoice& voice, ParamId id, float value)
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
                int modelIdx = juce::jlimit(0, (int)OldMacroOscVoice::Model::Count - 1, (int)value);
                for (auto& v : voices)
                    v.setModel(static_cast<OldMacroOscVoice::Model>(modelIdx));
                break;
            }
            case TimbreParam:
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
    float tune = 0.0f, fine = 0.0f, bend = 0.0f, bendRange = 2.0f, wheel = 0.0f;
    float wheelNow = 0.0f;                              // the strip, smoothed per block
    int envChainStageCount = 2;
    VoiceFilterSection filterSection { FilterBase };
    mod::Router router { lfoDests(0), 2, 0, &sources() };
    std::array<float, mod::Router::MAX_CHUNK> mixL {}, mixR {};
    VoiceAllocator allocator;
    std::vector<OldMacroOscVoice> voices;
    ParamMap params;
};
