#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "VoiceFilterSection.h"
#include "VoiceSpread.h"
#include "ModRouter.h"
#include "SampleBank.h"
#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/Wavetable.h"
#include "../dsp/PhaseOsc.h"
#include "../dsp/filters/SVFilter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// ============================================================================
// WavetableVoice — a single voice that reads from a wavetable.
//
// Supports optional EnvelopeChain for multi-stage envelope architectures.
// When envChainEnabled is true, the chain output replaces the simple ADSR.
// Each voice ends in the shared filter section (VoiceFilter: two filters, routing, filter FM
// from the table's own sound) and takes its note's share of the per-note spread (VoiceSpread).
//
// Its oscillators are dsp/PhaseOsc.h's: the table and its copy a little off pitch (SPRD) read one
// phaseosc::Wave (the table with its INTP, the vintage read-outs included: they are modes of the
// one read-out, WavetableRead) at the phases of two PhaseOsc, the sine an octave below (SUB) is
// the sine of that file. Under phase modulation (the destination PM) the table is read at the
// band-limited copy for the PITCH, as this engine always read it, not with PhaseOsc's swing rule
// (a duller copy the deeper the modulation): that rule takes brightness away from what PM on
// this engine sounded like (measured on the PWM table at full depth: the mean frequency of the
// sound falls to 0.83 of what it was), so it is not taken here. To take it: osc.readCycles(wave,
// phaseInc, pm, readPosition) in process(), and copyFor(.., osc.getSwing()) for the copy.
// Everything that moves the voice comes from the modulation router (mod::Voice): the voice is
// rendered a sub-block at a time and reads, per sample, the pitch, the level, the pan, the
// cutoff, the table position, the phase (PM) and three cells of the second filter; the rest at
// control rate. A voice nothing moves runs the plain loop, which is the engine as it was.
// ============================================================================

class WavetableVoice
{
public:
    // The destinations the voice reads by name: indices of the engine's table
    // (WavetableEngine::lfoDests), in the order of the DEST cells. The first eight are what the
    // engine always had, at the numbers they always had (a stored DEST keeps its meaning).
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // per sample: a frequency ratio (+-12 semitones, amount squared)
        LfoAmp,         // per sample: a gain, 0..4 (an audio-rate slot may take it to -4)
        LfoPan,         // per sample: +-1
        LfoCutoff,      // per sample: octaves (+-4 at full depth)
        LfoResonance,   // +-0.5
        LfoPosition,    // per sample: the scan position, +-1 (the whole table) around where it stands
        LfoSpread,      // the SPRD cell
        LfoSub,         // the SUB cell
        NumLfoDests
    };

    // The ones further down the table, which the engine looks up once (WavetableEngine::dests()).
    struct Dests
    {
        int balance = -1, filterFm = -1, offsetB = -1;     // the second filter's three per-sample cells
        int envTime = -1, filterTime = -1;                  // ETIM, FTIM
        int phase = -1;                                     // PM: added to the read phase, in cycles, per sample
    };

    // The voice's own sources, in the order the engine publishes them (WavetableEngine::sources).
    enum Published
    {
        PubScan = 0,    // SCAN: where the timed scan is between POS (0) and the end of the table (1)
        PubRect,        // XRCT: the track input (as XFLT has it) rectified, at audio rate
        PubFiltered     // XFLT: the track input through the engine's two-pole filters, at audio rate
    };
    // The engine's samples of the two audio-rate sources for a sub-block (nullptr: nobody reads it).
    struct InputSources { const float* rect = nullptr; const float* filtered = nullptr; };

    // PM at full depth: the phase swings this many cycles each way (4 pi radians); the amount
    // counts squared, so the low half of the cell is fine.
    static constexpr float PM_CYCLES = 2.0f;

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        envChain.setSampleRate(sr);
        filter.setSampleRate(sr);
        mods.prepare(sr);
        osc.setSampleRate(sr);
        osc2.setSampleRate(sr);
        subOsc.setSampleRate(sr);
        declick.setSampleRate(sr);
        gainDeclick.setSampleRate(sr);
        positionStep = (float) (1.0 / (phaseosc::POSITION_GLIDE_SECONDS * sr));     // the whole table in 3 ms (a STEP of a sixteenth: 0.2 ms)
        levelStep = (float) (1.0 / (0.004 * sr));       // a level's whole range in 4 ms
    }

    void setWavetable(const Wavetable* wt) { wavetable = wt; wave.table = wt; }

    void setNote(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        targetFrequency = dsp::midiNoteToHz(note);
        // Glide from the previous pitch only if this voice is still sounding.
        if (glideCoeff >= 1.0f || !active)
            baseFrequency = targetFrequency;
    }

    // `own`: what this note takes of the per-note spread (VoiceSpread, the ANLG page): with every
    // amount at 0 it is nothing, and the note is sample for sample what it was without the page.
    // The voice's generator has been advanced to this note (VoiceSpread::next): the router's
    // randoms (RAND, RND2 ..) and the RAND scan's places are draws of it, so SEED repeats them.
    void noteOn(const mod::Router& router, const VoiceSpread::Note& own = {})
    {
        const bool wasActive = active;
        if (active)
        {
            declick.arm();      // struck again while sounding: the phase restart must not jump,
            gainDeclick.arm();  // nor the step to the new velocity
        }
        // (Whatever is routed is read before the note's first sample: mod::Voice::tick.)
        mods.noteOn(router, currentNote, currentVelocity, spreadGenerator);
        // EVAR: this note's envelopes (amp, the chain's stages, the filter's) are all longer or
        // shorter by the same factor.
        if (own.timeScale != envScale)
        {
            envScale = own.timeScale;
            applyEnvTimeScale();
            filter.setEnvTimeScale(filterTime * envScale);
        }
        filter.noteOn(currentNote, currentVelocity, active, own.cutoffOctaves);
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOn();
        else
            ampEnv.noteOn();
        active = true;
        struck = true;          // the new note takes every glided value as it stands
        positionCarry = 0.0f;
        spreadSemis = own.semitones;
        spreadPosition = own.wave * SPREAD_POSITION;
        // (A voice struck again while it sounds is carried to its new place: the two declicks
        // above work on the voice's one signal, before the engine places it.)
        spreadPan = own.pan;
        if (! wasActive)
            spreadPanNow = spreadPan;
        spreadGain = own.gain;
        osc.reset((double) own.phase(0));
        osc2.reset(0.25 + (double) own.phase(1));
        subOsc.reset((double) own.phase(2));
        // Every note-on starts this voice's own scan run again (like its envelope and its phase).
        scanPhase = 0.0;
        scanHeldStep = -1;
        scanDraws.start(spreadGenerator, spread::Scan);
        scanRun = scanMode != WavetableScanMode::Off && scanReverse ? 1.0f : 0.0f;
    }

    void noteOff()
    {
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOff();
        else
            ampEnv.noteOff();
        filter.noteOff();
    }

    bool isActive() const { return active; }

    void setPosition(float pos) { position = juce::jlimit(0.0f, 1.0f, pos); }
    void setInterpMode(WavetableInterpMode m) { interpMode = m; wave.interp = m; }
    void setDetune(float cents) { detuneCents = cents; }
    void setSubLevel(float lvl) { subLevel = juce::jlimit(0.0f, 1.0f, lvl); }
    void setMorphSpeed(float speed) { morphSpeed = speed; }
    void setOctave(int oct) { octaveShift = juce::jlimit(-4, 4, oct); }
    void setSpread(float s) { spread = juce::jlimit(0.0f, 1.0f, s); }          // second, detuned copy
    void setPitchOffset(float semitones) { pitchOffset = semitones; }           // tune, bend, the VIBR cell's vibrato
    void setLevel(float l) { level = l; }                                       // the engine's LEVL
    void setMorphDepth(float d) { morphDepth = juce::jlimit(0.0f, 1.0f, d); }
    void setEnvToPosition(float a) { envToPos = juce::jlimit(-1.0f, 1.0f, a); }
    void setVelToPosition(float a) { velToPos = juce::jlimit(0.0f, 1.0f, a); }
    void setVelocitySensitivity(float s) { velSens = juce::jlimit(0.0f, 1.0f, s); }
    void setGlideTime(float seconds)
    {
        glideCoeff = seconds > 0.0005f ? 1.0f - std::exp(-3.0f / (seconds * (float) sampleRate)) : 1.0f;
    }
    // This voice's own value of three cells the engine works out per voice (a slot on VIBR, TIME
    // or SPD moves them): the VIBR cell's depth, and the scan's time and speed.
    void setVibrato(float depth) { vibratoDepth = depth; }
    float getVibrato() const { return vibratoDepth; }
    void setScanTimeCell(float seconds) { scanTimeCell = seconds; }
    void setScanSpeedCell(float speed) { scanSpeedCell = speed; }
    float getScanTimeCell() const { return scanTimeCell; }
    float getScanSpeedCell() const { return scanSpeedCell; }

    // The position the table was read at in the last sample (with what the router adds).
    float getReadPosition() const { return readPosition; }

    // A position beyond an end of the table comes back in from that end (mirrored): bounded, and
    // without the corner a hard stop would cut into the wave.
    static float foldPosition(float p)
    {
        if (! std::isfinite(p))
            return 0.0f;
        p = std::abs(p);
        p -= 2.0f * std::floor(p * 0.5f);
        return p > 1.0f ? 2.0f - p : p;
    }

    // --- Timed scan ---
    void setScanMode(WavetableScanMode m) { scanMode = m; }
    void setScanPassSeconds(double seconds) { scanInc = 1.0 / (juce::jmax(0.001, seconds) * sampleRate); }
    void setScanSteps(int n) { scanSteps = juce::jlimit(2, 256, n); }
    void setScanReverse(bool r) { scanReverse = r; }
    double getScanPhase() const { return scanPhase; }                   // passes since the note-on
    float getScanRun() const { return scanRun; }                        // the source SCAN, as of the last sample
    float getEffectivePosition() const { return effectivePosition; }    // as of the last sample

    // --- EnvelopeChain support ---
    void setEnvelopeChainEnabled(bool enabled) { envChainEnabled = enabled; }
    bool isEnvelopeChainEnabled() const { return envChainEnabled; }
    EnvelopeChain& getEnvelopeChain() { return envChain; }

    // One sub-block of n samples, added to left / right. pressure: this voice's pad. input: the
    // engine's two audio-rate sources for these n samples. foldAtEnds: an audio-rate slot is aimed
    // at POS and its input is heard (see process()). setCell(voice, dest, value) sets a page cell of this voice (a
    // control-rate destination of the router).
    template <typename SetCell>
    void render(const mod::Router& router, const Dests& d, float* left, float* right, int n, float pressure,
                const InputSources& input, bool foldAtEnds, const SetCell& setCell)
    {
        // The voice's own sources, only while a slot reads them. SCAN: where the run stood at the
        // last sample. XRCT and XFLT: the input's samples for THIS sub-block (they exist before
        // the voice renders, so the slots hear them with no delay).
        if (router.wants(PubScan))
            mods.publish(PubScan, scanMode != WavetableScanMode::Off ? scanRun : 0.0f);   // (0 while MODE is OFF, under a held note too)
        if (input.rect != nullptr || input.filtered != nullptr)
        {
            mods.publishAudio(PubRect, input.rect, n);
            mods.publishAudio(PubFiltered, input.filtered, n);
            inputHanded = true;
        }
        else if (inputHanded)
        {
            mods.publishAudio(PubRect, nullptr, 0);
            mods.publishAudio(PubFiltered, nullptr, 0);
            inputHanded = false;
        }

        const float envNow = envChainEnabled && envChain.getStageCount() > 0 ? lastEnv : ampEnv.getCurrentLevel();
        const bool moving = mods.tick(router, n, envNow, filter.getEnvelope().getCurrentLevel(), pressure);
        if (! moving)
        {
            // Nothing moves this voice: the engine as it is without any modulation.
            if (filterMoved)
                letFilterGo(router, d, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < n && active; ++i)
            {
                const float s = process();
                const float pan = spreadPanNow;
                left[i] += pan > 0.0f ? s * (1.0f - pan) : s;
                right[i] += pan < 0.0f ? s * (1.0f + pan) : s;
            }
            return;
        }

        mods.forEachControl([&](int dest, float value) { setControl(d, dest, value, setCell); });
        const float* pitch = mods.buffer(LfoPitch);
        const float* amp = mods.buffer(LfoAmp);
        const float* pan = mods.buffer(LfoPan);
        const float* cutoff = mods.buffer(LfoCutoff);
        const float* pos = mods.buffer(LfoPosition);
        const float* pm = mods.buffer(d.phase);
        const float* bal = mods.buffer(d.balance);
        const float* ffm = mods.buffer(d.filterFm);
        const float* ofsb = mods.buffer(d.offsetB);
        if (filterMoved || cutoff || bal || ffm || ofsb)
            letFilterGo(router, d, cutoff, bal, ffm, ofsb);

        // (PM is a phase, mod::Phase: the router brings it in and lets it go without a step.)
        for (int i = 0; i < n && active; ++i)
        {
            if (cutoff) filter.setLfoOctaves(cutoff[i]);
            if (bal)    filter.setBalanceNow(bal[i]);
            if (ffm)    filter.setFmNow(ffm[i]);
            if (ofsb)   filter.setOffsetBNow(ofsb[i]);
            const double phaseMod = pm ? (double) pm[i] : 0.0;
            const float s = process(phaseMod, pos ? pos[i] : 0.0f, foldAtEnds,
                                    pitch ? (double) pitch[i] : 1.0, amp ? amp[i] : 1.0f, true);
            // PAN moves its own voice between the channels (balance law: the centre is unchanged,
            // so a sound without that routing is exactly as before), with the note's own place.
            float p = pan ? pan[i] : 0.0f;
            if (spreadPanNow != 0.0f)
                p = juce::jlimit(-1.0f, 1.0f, p + spreadPanNow);
            left[i] += p > 0.0f ? s * (1.0f - p) : s;
            right[i] += p < 0.0f ? s * (1.0f + p) : s;
        }
    }

    // A control-rate destination of the router: the resonance, an envelope's time, or one of this
    // voice's page cells.
    template <typename SetCell>
    void setControl(const Dests& d, int dest, float value, const SetCell& setCell)
    {
        if (dest == LfoResonance)
            filter.setResonanceMod(value);
        else if (dest == d.envTime)
        {
            envTime = value;
            applyEnvTimeScale();
        }
        else if (dest == d.filterTime)
        {
            filterTime = value;
            filter.setEnvTimeScale(filterTime * envScale);
        }
        else
            setCell(*this, dest, value);
    }

    // One sample.
    //   pm        added to the read phase, in cycles (the destination PM: the track input on it
    //             is phase modulation)
    //   posMod    added to the table position (the destination POS). Without `fold` it is one
    //             more term of the sum that stops at the ends of the table, as an LFO on POS
    //             always was. With `fold` (an audio-rate slot is aimed at POS) it is added after
    //             the position's glide, which would hold an audio-rate movement back, and past an
    //             end of the table it comes back in from that end, so the wave is not cut by a
    //             corner (as XIN POS always was)
    //   pitch     a frequency ratio (the destination PTCH)
    //   amp       a gain (the destination AMP)
    //   routed    the router moves this voice (render()'s second loop): only then can `fold`
    //             change under a note with something on POS, and the position is carried across
    // With the defaults it is the voice without any modulation.
    float process(double pm = 0.0, float posMod = 0.0f, bool fold = false, double pitch = 1.0, float amp = 1.0f, bool routed = false)
    {
        if (!active || wavetable == nullptr || wavetable->numFrames == 0)
            return 0.0f;

        // The timed scan sets the base position (the knob's value when it is off).
        const bool scanning = scanMode != WavetableScanMode::Off;
        const float basePosition = scanning ? advanceScan() : position;

        // Auto-morph position
        if (morphSpeed > 0.0f)
        {
            morphPhase += morphSpeed / (float) sampleRate;
            if (morphPhase >= 1.0f)
                morphPhase = (float) dsp::wrapPhase((double) morphPhase);
            // Sinusoidal LFO modulation of position
            effectivePosition = basePosition + 0.5f * morphDepth * std::sin(morphPhase * 2.0f * juce::MathConstants<float>::pi);
            if (! scanning)
                effectivePosition = effectivePosition - std::floor(effectivePosition); // wrap 0-1
        }
        else
        {
            effectivePosition = basePosition;
        }
        // Envelope, velocity, the router and the note's own share (WVAR) move the scan position.
        // (The sum in the order and shape it always had, so that it rounds as it always did:
        // `none` stands where the pad's pressure was, which comes through the router now.)
        const float none = 0.0f, slow = fold ? 0.0f : posMod;
        float posOffset = none + envToPos * lastEnv + velToPos * currentVelocity + slow + spreadPosition;
        if (posOffset != 0.0f || scanning)
            effectivePosition = juce::jlimit(0.0f, 1.0f, effectivePosition + posOffset);

        // Nothing that is set or stepped under a sounding note is jumped to (a jump of the frame
        // read, or of a level, is a click): the position, the sub level, the spread coming in or
        // going out, velocity's share and the LEVL cell move on at a limited rate: the whole
        // table in three milliseconds, a level's whole range in four. A change that is slower
        // than that - any knob sweep, a running scan - passes exactly. STEP and RAND still step
        // to the ear. The first sample of a note takes everything as it stands.
        const float velGain = (1.0f - velSens + velSens * currentVelocity) * spreadGain;
        if (spreadPanNow != spreadPan)
            spreadPanNow = dsp::towards(spreadPanNow, spreadPan, levelStep);
        const bool first = struck;
        const float readBefore = readPosition;
        if (struck)
        {
            struck = false;
            positionNow = effectivePosition;
            subNow = subLevel;
            spreadMix = spread > 0.0f ? 1.0f : 0.0f;
            velGainNow = velGain;
            levelNow = level;
        }
        else
        {
            positionNow = dsp::towards(positionNow, effectivePosition, positionStep);
            subNow = dsp::towards(subNow, subLevel, levelStep);
            spreadMix = dsp::towards(spreadMix, spread > 0.0f ? 1.0f : 0.0f, levelStep);
            velGainNow = dsp::towards(velGainNow, velGain, levelStep);
            levelNow = dsp::towards(levelNow, level, levelStep);
        }
        effectivePosition = positionNow;
        readPosition = positionNow;
        // An audio-rate movement of the position: after the glide, mirrored at the ends.
        if (fold && posMod != 0.0f)
            readPosition = foldPosition(positionNow + posMod);
        // An audio-rate slot on POS that comes or goes changes how the rest of POS's modulation
        // is taken (inside the glided sum that stops at the ends, or after it and mirrored): with
        // an LFO, the strip or a pad on POS that is another place in the table from one sample
        // to the next. The difference is carried and let go at the position's own rate.
        if (routed)
        {
            if (first)
            {
                foldWas = fold;
                positionCarry = 0.0f;
            }
            else if (fold != foldWas)
            {
                foldWas = fold;
                positionCarry = readBefore - readPosition;
            }
            if (positionCarry != 0.0f)
            {
                positionCarry = dsp::towards(positionCarry, 0.0f, positionStep);
                readPosition = juce::jlimit(0.0f, 1.0f, readPosition + positionCarry);
            }
        }

        if (baseFrequency != targetFrequency)
        {
            baseFrequency += (targetFrequency - baseFrequency) * (double) glideCoeff;
            if (std::abs(targetFrequency - baseFrequency) < 0.01)
                baseFrequency = targetFrequency;
        }

        // Compute frequency with detune, octave and pitch offset
        double freq = baseFrequency * std::exp2((double) octaveShift + detuneCents / 1200.0 + (pitchOffset + spreadSemis) / 12.0);
        if (pitch != 1.0)
            freq *= pitch;
        double phaseInc = freq / sampleRate;

        // The table at this position, phase and INTP: the band-limited copy for the pitch
        const phaseosc::Copy copy = phaseosc::copyFor(*wavetable, phaseInc);
        float sample = phaseosc::readAt(wave, copy, osc.at(pm), readPosition);

        // Spread: a second copy up to 30 cents away, mixed equally (faded in and out where the
        // spread starts or ends under a sounding note). It reads the same band-limited copy.
        if (spread > 0.0f || spreadMix > 0.0f)
        {
            const float both = (sample + phaseosc::readAt(wave, copy, osc2.at(pm), readPosition)) * 0.7071f;
            sample = spreadMix >= 1.0f ? both : sample + spreadMix * (both - sample);
            osc2.advance(phaseInc * std::exp2((double) spread * 30.0 / 1200.0));
        }

        // Sub oscillator: a sine one octave below
        float sub = 0.0f;
        if (subNow > 0.0f)
            sub = subNow * subOsc.sine(0.0f);
        subOsc.advance(phaseInc * 0.5);

        // Advance phase
        osc.advance(phaseInc);          // (never a loop: a huge or infinite step must not hang)

        // Envelope: use chain if enabled, otherwise simple ADSR
        float envVal;
        if (envChainEnabled && envChain.getStageCount() > 0)
        {
            envVal = envChain.process();
            if (!envChain.isActive())
                active = false;
        }
        else
        {
            envVal = ampEnv.process();
            if (!ampEnv.isActive())
                active = false;
        }

        lastEnv = envVal;
        // (FFM: the table's own sound moves the cutoff; the sine below it does not.)
        return gainDeclick.process(filter.process(declick.process(sample + sub), sample) * envVal * velGainNow * amp * levelNow);
    }

    void reset()
    {
        declick.reset();
        gainDeclick.reset();
        lastEnv = 0.0f;         // ENVP must not start a note from the note before's envelope
        ampEnv.reset();
        envChain.reset();
        filter.reset();
        active = false;
        osc.reset();
        morphPhase = 0.0f;
        scanPhase = 0.0;
        scanHeldStep = -1;
        scanRun = 0.0f;
    }

    Envelope& getAmpEnvelope() { return ampEnv; }
    // The amp envelope as set (the chain's stages are copies of it). A note's EVAR and the
    // router's ETIM are a time scale of the envelope (Envelope::setTimeScale): what is set stays.
    const Envelope::Params& getAmpParams() const { return ampEnv.getParams(); }
    void setAmpParams(const Envelope::Params& p)
    {
        ampEnv.setParams(p);
        for (int s = 0; s < envChain.getStageCount(); ++s)
            envChain.getEnvelopeMut(s).setParams(p);
    }
    // One value of it under a running note (a slot on ATK ...): 0 attack, 1 decay, 2 sustain, 3 release.
    void setAmpStage(int stage, float value)
    {
        auto set = [stage, value](Envelope& e)
        {
            if (stage == 0)      e.setAttack(value);
            else if (stage == 1) e.setDecay(value);
            else if (stage == 2) e.setSustain(value);
            else                 e.setRelease(value);
        };
        set(ampEnv);
        for (int s = 0; s < envChain.getStageCount(); ++s)
            set(envChain.getEnvelopeMut(s));
    }
    // ETIM x the note's EVAR on the amp envelope and on every stage of the chain (call after the
    // chain was rebuilt: its new stages start at 1).
    void applyEnvTimeScale()
    {
        const float scale = envTime * envScale;
        ampEnv.setTimeScale(scale);
        for (int s = 0; s < envChain.getStageCount(); ++s)
            envChain.getEnvelopeMut(s).setTimeScale(scale);
    }
    VoiceFilter& getFilter() { return filter; }
    mod::Voice& getMod() { return mods; }
    spread::Generator& getSpreadGenerator() { return spreadGenerator; }
    // -1..1: the note's own place (PVAR), as of the last processed sample.
    float getPan() const { return spreadPanNow; }
    // WVAR at its top moves a note's table position by this much of the table either way.
    static constexpr float SPREAD_POSITION = 0.5f;

private:
    // The filter's per-sample destinations as the router has them for this sub-block: one that
    // is no longer moved goes back to its set value (the router hands back control-rate cells
    // only). cutoff / bal / ffm / ofsb: the buffers, nullptr where nothing moves the cell.
    void letFilterGo(const mod::Router& router, const Dests& d, const float* cutoff, const float* bal, const float* ffm, const float* ofsb)
    {
        if (cutoff == nullptr && (filterMoved & 1))  filter.setLfoOctaves(0.0f);
        if (bal == nullptr && (filterMoved & 2))     filter.setBalanceNow(router.baseOf(d.balance));
        if (ffm == nullptr && (filterMoved & 4))     filter.setFmNow(router.baseOf(d.filterFm));
        if (ofsb == nullptr && (filterMoved & 8))    filter.setOffsetBNow(router.baseOf(d.offsetB));
        filterMoved = (cutoff ? 1 : 0) | (bal ? 2 : 0) | (ffm ? 4 : 0) | (ofsb ? 8 : 0);
    }

    // One sample of the scan run: where the run is between the POS knob (0) and the end of the
    // table (1), then the base position. The first sample of a note is exactly at the start.
    float advanceScan()
    {
        const double u = scanPhase;
        scanPhase += scanInc;
        float t = 0.0f;
        switch (scanMode)
        {
            case WavetableScanMode::Run:
                t = u >= 1.0 ? 1.0f : (float) u;
                break;
            case WavetableScanMode::Loop:
                t = (float) (u - std::floor(u));
                break;
            case WavetableScanMode::Ping:
            {
                const double v = u - 2.0 * std::floor(u * 0.5);
                t = (float) (v <= 1.0 ? v : 2.0 - v);
                break;
            }
            case WavetableScanMode::Step:
            {
                const int k = u >= 1.0 ? scanSteps - 1 : juce::jmin(scanSteps - 1, (int) (u * scanSteps));
                t = (float) k / (float) (scanSteps - 1);
                break;
            }
            case WavetableScanMode::Rand:
            {
                const juce::int64 k = (juce::int64) (u * scanSteps);
                if (k != scanHeldStep)
                {
                    scanHeldStep = k;
                    // The next draw of this note's run (spread::Sequence over the voice's
                    // generator): the same SEED and the same notes give the same places.
                    scanHeld = scanDraws.unipolar();
                }
                t = scanHeld;
                break;
            }
            case WavetableScanMode::Off:
            case WavetableScanMode::Count:
                break;
        }
        if (scanReverse)
            t = 1.0f - t;
        scanRun = t;
        float pos = position + t * (1.0f - position);
        if (scanMode == WavetableScanMode::Rand && wavetable->numFrames > 1)
        {
            const float last = (float) (wavetable->numFrames - 1);      // whole frames only
            pos = std::round(pos * last) / last;
        }
        return pos;
    }

    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool active = false;

    // The oscillators (dsp/PhaseOsc.h): the table, its copy a little off pitch, the sine below
    phaseosc::PhaseOsc osc, osc2, subOsc;
    phaseosc::Wave wave;            // the table and its INTP

    const Wavetable* wavetable = nullptr;
    float position = 0.0f;          // 0.0 - 1.0 scan through frames
    float effectivePosition = 0.0f; // after morph modulation
    float readPosition = 0.0f;      // where the table is read: effectivePosition, moved by the router's POS
    WavetableInterpMode interpMode = WavetableInterpMode::Linear;
    float detuneCents = 0.0f;
    float subLevel = 0.0f;
    float morphSpeed = 0.0f;        // Hz, 0 = manual position only
    float morphPhase = 0.0f;
    int octaveShift = 0;
    float spread = 0.0f;
    float pitchOffset = 0.0f;
    float level = 1.0f;
    float vibratoDepth = 0.0f, scanTimeCell = 2.0f, scanSpeedCell = 1.0f;
    // The values above as they are heard: moved to, never jumped to, under a sounding note.
    bool struck = true;
    float positionNow = 0.0f, subNow = 0.0f, spreadMix = 0.0f, velGainNow = 1.0f, levelNow = 1.0f;
    float positionStep = 0.007f, levelStep = 0.005f;
    float morphDepth = 1.0f;
    float envToPos = 0.0f;
    float velToPos = 0.0f;
    float velSens = 1.0f;
    float lastEnv = 0.0f;
    float glideCoeff = 1.0f;
    double targetFrequency = 440.0;

    // Timed scan
    WavetableScanMode scanMode = WavetableScanMode::Off;
    double scanPhase = 0.0;         // passes since the note-on
    double scanInc = 0.0;           // passes per sample
    int scanSteps = 16;
    bool scanReverse = false;
    juce::int64 scanHeldStep = -1;
    float scanHeld = 0.0f;
    float scanRun = 0.0f;           // the run, 0 (POS) .. 1 (the end of the table): the source SCAN
    spread::Sequence scanDraws;     // RAND's places for this note

    Envelope ampEnv;
    // The per-note spread (VoiceSpread): this voice's generator, and what its present note took
    spread::Generator spreadGenerator;
    float envScale = 1.0f, spreadSemis = 0.0f, spreadPosition = 0.0f, spreadPan = 0.0f, spreadPanNow = 0.0f, spreadGain = 1.0f;
    float envTime = 1.0f, filterTime = 1.0f;    // the router's ETIM and FTIM for this voice
    dsp::Declick declick;       // on the oscillators, before the filter (whose state smears a jump)
    dsp::Declick gainDeclick;   // on the output

    // Filter (after the oscillators and sub, before the amp envelope)
    VoiceFilter filter;

    // This voice's side of the modulation router
    mod::Voice mods;
    int filterMoved = 0;        // which of the filter's per-sample cells the router holds (letFilterGo)
    // POS across a change of `fold` (process()): how it was taken, the difference still carried
    bool foldWas = false;
    float positionCarry = 0.0f;
    bool inputHanded = false;   // the sources XRCT / XFLT are handed over

    // EnvelopeChain support — replaces ampEnv when enabled
    EnvelopeChain envChain;
    bool envChainEnabled = false;
};

// ============================================================================
// WavetableEngine — EngineBase implementation wrapping VoiceAllocator and
// WavetableVoice, with built-in factory tables and parameter interface.
// ============================================================================

class WavetableEngine : public EngineBase
{
public:
    enum Param
    {
        Position       = 0,
        InterpMode     = 1,
        Detune         = 2,
        UnisonSpread   = 3,
        SubLevel       = 4,
        MorphSpeed     = 5,
        Level          = 6,
        Octave         = 7,
        TableSelect    = 8,
        AmpAttack      = 9,
        AmpDecay       = 10,
        AmpSustain     = 11,
        AmpRelease     = 12,
        EnvChainMode   = 13,   // 0=off, 1=series, 2=parallel
        EnvChainStages = 14,   // number of envelope stages in the chain
        EnvChainCombine = 15,  // 0=sum, 1=max (for parallel mode)
        TuneParam = 16,        // semitones
        BendRangeParam = 17,   // semitones
        MorphDepth = 18,       // depth of the position motion
        EnvToPosition = 19,    // amp envelope -> scan position, -1..1
        VelToPosition = 20,    // velocity -> scan position
        VelSens = 21,          // velocity -> level
        Glide = 22,            // seconds
        Vibrato = 23,          // constant vibrato depth (the mod strip adds to it)
        UserSample = 24,       // which SampleBank entry the USER table is built from
        // The SCAN page: a timed run of the position through the table (see pages()).
        ScanMode = 25,         // WavetableScanMode index
        ScanTime = 26,         // seconds for one pass (SYNC = OFF)
        ScanSync = 27,         // 0 = TIME, 1 = LEN at the tempo, 2 = NAT (the USER sample's own length)
        ScanLength = 28,       // index into scanLengthBeats (SYNC = ON)
        ScanSteps = 29,        // index into scanStepCounts (STEP and RAND)
        ScanDirection = 30,    // 0 = forward, 1 = reverse
        ScanSpeed = 31,        // 0.25 .. 4: the pass is this many times faster (1 = 100 %)
        UserFrames = 32,       // how the USER table is cut: index into userFrameCounts, +3 = period-aligned
        // The KEYS table (WavetableKeys): pages KEYS, K SMP, K POS and K PLC. Rebuild parameters all.
        KeyAlgo = 33,          // wavemorph::Algo index: how the frames between two keys are computed
        KeyFrames = 34,        // index into WavetableKeys::frameCount
        KeyBias = 35,          // -1 .. 1
        KeyWrap = 36,          // 0 / 1
        KeyAlign = 37,         // 0 / 1
        KeyNorm = 38,          // WavetableKeys::Norm
        KeyTilt = 39,          // dB per octave
        KeyHarmonics = 40,     // highest harmonic of a key, 1 .. 127
        KeySampleBase = 41,    // 41-48: key n's sample, 0 = OFF, else SampleBank index + 1
        KeyPositionBase = 49,  // 49-56: where in its sample key n is taken, 0 .. 1
        KeyPlaceBase = 57,     // 57-64: where along the table key n sits, 0 .. 1
        // (65-72 were the XIN page's own cells until phase U4: the track input is the router's now,
        // its XIN page and slots. The numbers are left free.)
        VoiceCountParam = 73,  // VOX: polyphony 1..16 (default 8); 65-72 reserved (old XIN cells, must be inert)
        FilterBase = 100,      // 100-115: the per-voice filter (VoiceFilterSection offsets)
        FilterDualBase = 116,  // 116-124: its second filter, routing, balance and FM (the section's dual block)
        SpreadBase = 125       // 125-132: the per-note spread (VoiceSpread offsets; the ANLG page)
        // 158-189 and 700-772 are the modulation router's (mod::Router: LFO1, LFO2, STRIP, XIN,
        // the slots, the custom LFO shape): everything that moves this engine's sound goes
        // through it, the pad's pressure routes and the track input at audio rate included.
    };

    static constexpr int MAX_VOICES = 16;

    explicit WavetableEngine(int maxVoices = 8)
        : allocator(MAX_VOICES)
    {
        voices.resize((size_t) MAX_VOICES);

        // The built-in tables are shared by every engine (built once, read only); the engine owns
        // the two made from samples. USER (a recording made in the app, or a file, cut into frames)
        // and KEYS (key frames) are copies of the sine table until something is chosen.
        const auto& builtIn = WavetableFactory::builtInTables();
        for (int i = 0; i < WavetableFactory::NUM_FIRST_TABLES; ++i)
            tables.push_back(builtIn[(size_t) i]);
        userTable = *builtIn[3];
        userTable.name = "USER";
        keyTable = *builtIn[3];
        keyTable.name = "KEYS";
        tables.push_back(&userTable);
        tables.push_back(&keyTable);
        for (size_t i = (size_t) WavetableFactory::NUM_FIRST_TABLES; i < builtIn.size(); ++i)
            tables.push_back(builtIn[i]);
        jassert((int) tables.size() == NUM_TABLES);

        // Point all voices at the first table
        for (auto& v : voices)
        {
            v.setWavetable(tables[0]);
            v.setLevel(masterLevel);
        }
        seedSpread();

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            voices[(size_t) voiceIdx].setNote(v.note, v.velocity);
            auto& voice = voices[(size_t) voiceIdx];
            // (The spread first: it advances the voice's generator to this note, and the router's
            // randoms and the RAND scan are draws of it.)
            voice.noteOn(router, spreadSection.next(voice.getSpreadGenerator()));
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t) voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out. Not with
        // GLID on: a glide starts from the pitch of a voice that still sounds, so it takes that one.
        allocator.setVoiceSoundingQuery([this](int voiceIdx)
        {
            return voices[(size_t) voiceIdx].isActive() && getParameter(Glide) <= 0.0f;
        });

        // Seed every parameter so getParameter is always the true value.
        for (auto& page : getParamPages())
            for (auto& p : page.params)
                if (p.isValid())
                    setParameter(p.id, p.id == VoiceCountParam ? (float) maxVoices : p.defaultVal);
        filterSection.applyAll(VoiceFilterSection::each(voices));
    }

    // ---- The SCAN page ---------------------------------------------------------------------
    // MODE  OFF / RUN / LOOP / PING / STEP / RAND. Each voice starts its own run at its note-on
    //       (every note-on restarts it, exactly as it restarts the voice's envelope) and travels
    //       from the POS knob's value to the end of the table: POS is the start offset.
    //         RUN   once through, then holds the last frame
    //         LOOP  jumps back to POS and goes again
    //         PING  to the end and back
    //         STEP  once through in STEP equal plateaus (first = POS, last = the end), then holds
    //         RAND  a random whole frame between POS and the end, a new one STEP times per pass
    // TIME  seconds for one pass (SYNC = OFF).
    // SYNC  OFF: TIME.  ON: LEN at the tempo, followed live.  NAT: the natural length — one pass
    //       over the whole USER table lasts exactly as long as the sample it was built from, so RUN
    //       replays the recording at its original speed at any pitch. With POS above 0 the pass is
    //       shorter in proportion (the rest of the recording, still at its own speed). On any
    //       other table NAT has no source and uses TIME.
    // LEN   1/16 note ... 16 bars of 4 beats.
    // STEP  4 / 8 / 16 / 32 / 64 steps per pass (STEP and RAND).
    // DIR   FWD: POS -> end.  REV: end -> POS.
    // SPD   x0.25 ... x4 (25 % - 400 %): the pass is that much faster, in every SYNC setting
    //       (NAT x 0.5 = half speed at the original pitch of the key).
    // FRMS  how the USER table is cut from its sample: 64 / 128 / 256 equal pieces, or A64 / A128 /
    //       A256 = one phase-aligned period per frame (WavetableFactory::createFromSamplesEx).
    //       The table is rebuilt when this changes. 64 is the default: it is what every USER table
    //       made so far uses, so saved sounds do not change.
    //
    // How the run adds up with everything else that moves the position, per sample:
    //     base     = POS knob                       (MODE = OFF)
    //              = POS + run * (1 - POS)          (run = 0..1 from the mode and DIR)
    //     position = clamp(clamp(base + MSPD/MDPT motion + ENVP + VELP, 0, 1) + the router's POS, 0, 1)
    // The run sets the base and the others add to it; the sum stops at the two ends of the table
    // (with an audio-rate slot on POS it comes back in from the end instead: WavetableVoice::process).
    // (With MODE = OFF the MSPD motion wraps around the table as it always has; under a run it is
    // clamped like the rest, so a held last frame does not jump to the first.)
    // ---- The table list ---------------------------------------------------------------------
    // TABL is an index and is stored in projects: 0-8 the first built-in tables, 9 USER, 10 KEYS,
    // 11 on the newer built-in tables in the order of wavetables::definitions(). New ones go at
    // the end.
    static constexpr int USER_TABLE = 9;
    static constexpr int KEY_TABLE = 10;
    static constexpr int FIRST_MORE_TABLE = 11;
    static constexpr int NUM_MORE_TABLES = 14;
    static constexpr int NUM_TABLES = FIRST_MORE_TABLE + NUM_MORE_TABLES;
    static const char* const* tableLabels()
    {
        static const std::array<const char*, NUM_TABLES> names = []
        {
            std::array<const char*, NUM_TABLES> n {};
            const char* const* builtIn = WavetableFactory::builtInLabels();
            jassert(WavetableFactory::builtInCount() == NUM_TABLES - 2);
            for (int i = 0; i < USER_TABLE; ++i)
                n[(size_t) i] = builtIn[i];
            n[(size_t) USER_TABLE] = "USER";
            n[(size_t) KEY_TABLE] = "KEYS";
            for (int i = USER_TABLE; i < WavetableFactory::builtInCount() && i + 2 < NUM_TABLES; ++i)
                n[(size_t) (i + 2)] = builtIn[i];
            return n;
        }();
        return names.data();
    }

    // ---- The key-frame pages ------------------------------------------------------------------
    // KEYS   ALGO  how the frames between two keys are made: LIN crossfade, STEP none (the nearer
    //              key), CLSC crossfade stored as 128 samples of 8 bits, PHAS level and phase of
    //              each harmonic separately, TRNS the energy glides along the harmonics (optimal
    //              transport), VOC envelope first, fine structure after (vocoder cross-synthesis
    //              half way). See wavemorph::morph().
    //        KFRM  frames in the table: 8 .. 256.
    //        BIAS  the frames between lean towards the earlier (-) or the later (+) key.
    //        WRAP  ON: after the last key the table returns to the first (for LOOP scans).
    //        ALGN  ON: every key is turned so that its fundamental starts like a sine.
    //        NORM  OFF as computed / PEAK every frame to full scale / RMS every frame equally loud.
    //        TILT  dB per octave on every key.  HARM  the highest harmonic of a key.
    // K SMP  SMP1-8  the sample key n is taken from; OFF = the key is not used.
    // K POS  POS1-8  where in that sample.
    // K PLC  PLC1-8  where along the table. Only the order and the spacing count: the earliest key
    //                in use is the first frame, the latest the last.
    static const char* const* keySampleNames()
    {
        static std::array<const char*, SampleBank::MAX_ENTRIES + 1> names {};
        auto& bank = SampleBank::get();
        names[0] = "OFF";
        for (int i = 0; i < bank.size() && i < SampleBank::MAX_ENTRIES; ++i)
            names[(size_t) i + 1] = bank.names()[i];
        return names.data();
    }
    static bool isKeyParam(ParamId id) { return id >= KeyAlgo && id < KeyPlaceBase + WavetableKeys::MAX_KEYS; }

    static constexpr int NUM_SCAN_LENGTHS = 9;
    static constexpr int NUM_SCAN_STEPS = 5;
    static constexpr int NUM_USER_FRAME_OPTIONS = 6;
    static float scanLengthBeats(int index)
    {
        static const float beats[NUM_SCAN_LENGTHS] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
        return beats[(size_t) juce::jlimit(0, NUM_SCAN_LENGTHS - 1, index)];
    }
    static int scanStepCount(int index)
    {
        static const int counts[NUM_SCAN_STEPS] = { 4, 8, 16, 32, 64 };
        return counts[(size_t) juce::jlimit(0, NUM_SCAN_STEPS - 1, index)];
    }
    static int userFrameCount(int option)
    {
        static const int counts[3] = { 64, 128, 256 };
        return counts[(size_t) (juce::jlimit(0, NUM_USER_FRAME_OPTIONS - 1, option) % 3)];
    }
    static bool userFramesAligned(int option) { return option >= 3; }

    // ---- The track input at audio rate ---------------------------------------------------------
    // The engine had a page of its own for it (XIN: OFF / PM / POS, XDPT, XENV, XVEL, XWHL, XRCT,
    // XLP, XHP, ids 65-72). It is the router's now, as on every engine: the XIN page (two routes:
    // DST AMT FLT, and the LPF / HPF the filtered copies use) or any free slot with the source XIN
    // / XLP / XHP / XBP. What each old cell is there:
    //   XIN PM    a route to the destination PM: the input is added to the phase the table is
    //             read at, up to two cycles each way, the amount squared (as XDPT was)
    //   XIN POS   a route to POS: the input moves the table position around where it stands, up
    //             to the whole table each way; past an end it comes back in from that end
    //   XDPT      the route's AMT
    //   XENV      on a free slot n: ENV > AMTn (AMT = XDPT x (1 - XENV), the slot ENV > AMTn at
    //             XDPT x XENV), or simply VIA = ENV for all of it
    //   XVEL      the same with VEL          XWHL   STRP > AMTn at XWHL
    //   XLP XHP   the source XFLT, which this engine publishes: the input through the engine's
    //             own two-pole high-pass and low-pass, at the XIN page's HPF and LPF (which are
    //             therefore open on this engine until set: 20 Hz and 20 kHz, the old defaults, not
    //             the router's 200 Hz and 2 kHz). The router's own XLP / XHP / XBP (FLT = LP / HP /
    //             BP) read the same two cells through one-pole filters: half as steep.
    //   XRCT      the source XRCT, which this engine publishes: the absolute value of XFLT (of
    //             the input as it is while LPF and HPF are open)
    // ---- The engine's own sources (SRC and VIA show them after the router's) ---------------------
    //   SCAN      where the timed scan is, 0 (at POS) .. 1 (the end of the table), with DIR; 0
    //             while MODE is OFF. Control rate: any cell can follow the scan (SCAN > CUT: the
    //             filter opens as the table is run through).
    //   XRCT      the track input rectified, at audio rate (after XFLT's filters).
    //   XFLT      the track input through a two-pole high-pass at HPF and a two-pole low-pass at
    //             LPF (the XIN page's cells), at audio rate.
    // Both are heard by the slots in the same sub-block (no delay): the input exists before the
    // voices render.

    // WAVE (table, scan position, motion), SCAN (the timed run), OSC (pitch, thickness), ANLG (the
    // per-note spread: VoiceSpread.h) and AMP (envelope, chain, level); the key-frame pages; FILTR,
    // FLT B and F ENV (the shared filter section with its dual block: VoiceFilterSection.h); XIN.
    // The router's pages (XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2) follow in getParamPages(). The
    // filter's KTRK / EVEL / VEL / WHL stay cells of the filter pages, as on the macro oscillator
    // (VoiceFilterSection's Routed layout, where they are default slots, is not used yet).
    static const ParamPages& pages()
    {
        static const char* const scanModes[] = { "OFF", "RUN", "LOOP", "PING", "STEP", "RAND" };
        static const char* const scanSync[] = { "OFF", "ON", "NAT" };
        static const char* const scanLengths[] = { "1/16", "1/8", "1/4", "1/2", "1BAR", "2BAR", "4BAR", "8BAR", "16BR" };
        static const char* const scanSteps[] = { "4", "8", "16", "32", "64" };
        static const char* const scanDirs[] = { "FWD", "REV" };
        static const char* const userFrames[] = { "64", "128", "256", "A64", "A128", "A256" };
        static const char* const interp[] = { "LIN", "SPEC", "STEP", "VINT", "CLSC" };
        static const char* const keyAlgos[] = { "LIN", "STEP", "CLSC", "PHAS", "TRNS", "VOC" };
        static const char* const keyFrames[] = { "8", "16", "32", "64", "128", "256" };
        static const char* const offOn[] = { "OFF", "ON" };
        static const char* const keyNorms[] = { "OFF", "PEAK", "RMS" };
        static const char* const smpLabels[] = { "SMP1", "SMP2", "SMP3", "SMP4", "SMP5", "SMP6", "SMP7", "SMP8" };
        static const char* const posLabels[] = { "POS1", "POS2", "POS3", "POS4", "POS5", "POS6", "POS7", "POS8" };
        static const char* const plcLabels[] = { "PLC1", "PLC2", "PLC3", "PLC4", "PLC5", "PLC6", "PLC7", "PLC8" };
        static const char* const chain[] = { "OFF", "SER", "PAR" };
        static const char* const combine[] = { "SUM", "MAX" };
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        static const ParamPages list = [&]
        {
          ParamPages l = {
            { "WAVE", "SRC", {{
                { "TABL", TableSelect, 0, NUM_TABLES - 1, 0, C, "", tableLabels(), NUM_TABLES },
                { "POS",  Position, 0, 1, 0, L },
                { "MSPD", MorphSpeed, 0, 10, 0, L, "Hz" },
                { "MDPT", MorphDepth, 0, 1, 1, L },
                { "ENVP", EnvToPosition, -1, 1, 0, L },
                { "VELP", VelToPosition, 0, 1, 0, L },
                { "INTP", InterpMode, 0, (float) ((int) WavetableInterpMode::Count - 1), 0, C, "", interp, (int) WavetableInterpMode::Count },
                { "LEVL", Level, 0, 1, 0.8f, L } }} },
            { "SCAN", "SRC", {{
                { "MODE", ScanMode, 0, (float) ((int) WavetableScanMode::Count - 1), 0, C, "", scanModes, (int) WavetableScanMode::Count },
                { "TIME", ScanTime, 0.05f, 60, 2, G, "s" },
                { "SYNC", ScanSync, 0, 2, 0, C, "", scanSync, 3 },
                { "LEN",  ScanLength, 0, NUM_SCAN_LENGTHS - 1, 4, C, "", scanLengths, NUM_SCAN_LENGTHS },
                { "STEP", ScanSteps, 0, NUM_SCAN_STEPS - 1, 2, C, "", scanSteps, NUM_SCAN_STEPS },
                { "DIR",  ScanDirection, 0, 1, 0, C, "", scanDirs, 2 },
                { "SPD",  ScanSpeed, 0.25f, 4, 1, G, "x" },
                { "FRMS", UserFrames, 0, NUM_USER_FRAME_OPTIONS - 1, 0, C, "", userFrames, NUM_USER_FRAME_OPTIONS } }} },
            { "OSC", "SRC", {{
                { "OCT",  Octave, -4, 4, 0, I },
                { "TUNE", TuneParam, -12, 12, 0, I, "st" },
                { "FINE", Detune, -100, 100, 0, L, "ct" },
                { "BEND", BendRangeParam, 0, 24, 2, I, "st" },
                { "GLID", Glide, 0, 2, 0, L, "s" },
                { "VIBR", Vibrato, 0, 1, 0, L },
                { "SPRD", UnisonSpread, 0, 1, 0, L },
                { "SUB",  SubLevel, 0, 1, 0, L } }} },
            VoiceSpread::makePage(SpreadBase),
            { "AMP", "AMP", {{
                { "ATK",  AmpAttack, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  AmpDecay, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  AmpSustain, 0, 1, 0.7f, L },
                { "REL",  AmpRelease, 0.001f, 10, 0.3f, G, "s" },
                { "VEL",  VelSens, 0, 1, 0.7f, L },
                { "CHAN", EnvChainMode, 0, 2, 0, C, "", chain, 3 },
                { "STGS", EnvChainStages, 1, 8, 2, I },
                { "COMB", EnvChainCombine, 0, 1, 0, C, "", combine, 2 } }} },
            { "KEYS", "KEYS", {{
                { "ALGO", KeyAlgo, 0, (float) ((int) wavemorph::Algo::Count - 1), 0, C, "", keyAlgos, (int) wavemorph::Algo::Count },
                { "KFRM", KeyFrames, 0, WavetableKeys::NUM_FRAME_OPTIONS - 1, 3, C, "", keyFrames, WavetableKeys::NUM_FRAME_OPTIONS },
                { "BIAS", KeyBias, -1, 1, 0, L },
                { "WRAP", KeyWrap, 0, 1, 0, C, "", offOn, 2 },
                { "ALGN", KeyAlign, 0, 1, 1, C, "", offOn, 2 },
                { "NORM", KeyNorm, 0, WavetableKeys::NumNorms - 1, WavetableKeys::NormRms, C, "", keyNorms, WavetableKeys::NumNorms },
                { "TILT", KeyTilt, -6, 6, 0, L },
                { "HARM", KeyHarmonics, 1, wavemorph::H, wavemorph::H, I } }} },
          };
          {
              ParamPage smp { "K SMP", "KEYS", {} }, pos { "K POS", "KEYS", {} }, plc { "K PLC", "KEYS", {} };
              const int bank = SampleBank::get().size();
              for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
              {
                  smp.params[(size_t) k] = { smpLabels[k], KeySampleBase + k, 0, (float) bank, 0, C, "", keySampleNames(), bank + 1 };
                  pos.params[(size_t) k] = { posLabels[k], KeyPositionBase + k, 0, 1, defaultKeyPosition(k), L };
                  plc.params[(size_t) k] = { plcLabels[k], KeyPlaceBase + k, 0, 1, defaultKeyPlace(k), L };
              }
              l.push_back(smp);
              l.push_back(pos);
              l.push_back(plc);
          }
          // VOICE: polyphony. Before the filter pages (before STRIP / LFO pages).
          l.push_back({ "VOICE", "VOX", {{ { "VOX", VoiceCountParam, 1, MAX_VOICES, 8, I }, {}, {}, {}, {}, {}, {}, {} }} });
          VoiceFilterSection::appendPages(l, FilterBase, FilterDualBase);
          return l;
        }();
        return list;
    }

    // What can be moved (the modulation router's table). The original eight (pitch, level, pan,
    // cutoff, resonance and the table position as inputs of the voice; SPRD and SUB, which are
    // page cells), then the other continuous cells of the pages in the order they always had (a
    // stored DEST keeps its meaning: nothing was moved or taken out), LRAT / LDPT, the second
    // filter's cells, and new at the end: ETIM / FTIM (the envelopes' times) and PM (the read
    // phase).
    // Per sample (so the track input reaches them at audio rate): PTCH, AMP, PAN, CUT, POS, PM,
    // BAL, FFM, OFSB. The rest once a sub-block.
    // POS is an input of the voice, an offset of up to the whole table either way around where
    // the knob, the scan, the motion, ENVP and VELP have brought the position - not the POS cell
    // with a range of 0..1: the router would hold that to the table before the voice saw it, and
    // an LFO could then not move a scan that has reached the end, nor the input swing past an
    // end and come back (the mirror the voice makes).
    // Not offered: TABL, INTP, the scan's MODE / SYNC / LEN / STEP / DIR / FRMS, CHAN, STGS, COMB
    // (choices and counts), OCT, TUNE, FINE and BEND (PTCH is the pitch), LEVL (the level of the
    // mixed voices; AMP is each voice's), the ANLG page (read once, at the note-on, before the
    // router's first tick) and what the filter leaves out.
    static const mod::DestTable& lfoDests()
    {
        static const mod::DestTable table = []
        {
            const ParamPages& p = pages();
            mod::DestTable t;
            t.group();
            t.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);      // a ratio: one octave x the amount squared
            t.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample).audioFloor(-4.0f);  // (below 0: ring modulation)
            t.input("PAN", 0.0f, 1.0f, -1.0f, 1.0f, mod::PerSample);
            VoiceFilterSection::addRouterInputs(t);                                 // CUT (octaves, per sample), RESO
            t.input("POS", 0.0f, 1.0f, -2.0f, 2.0f, mod::PerSample);
            t.add(p, UnisonSpread);
            t.add(p, SubLevel);
            jassert(t.indexOfLabel("CUT") == WavetableVoice::LfoCutoff && t.indexOfLabel("SUB") == WavetableVoice::LfoSub);
            t.group();
            for (int id : { (int) MorphSpeed, (int) MorphDepth, (int) EnvToPosition, (int) VelToPosition })
                t.add(p, id);
            t.group();
            t.add(p, ScanTime);
            t.add(p, ScanSpeed);
            t.group();
            t.add(p, Glide);
            t.add(p, Vibrato);
            t.group();
            for (int id : { (int) AmpAttack, (int) AmpDecay, (int) AmpSustain, (int) AmpRelease, (int) VelSens })
                t.add(p, id);
            VoiceFilterSection::addLfoDests(t, p, FilterBase);
            t.addCross();
            // (After everything that was there before: a stored destination keeps its number.)
            VoiceFilterSection::addDualDests(t, p, FilterDualBase);
            t.group();
            t.envTimes();
            t.input("PM", 0.0f, WavetableVoice::PM_CYCLES, -8.0f, 8.0f, mod::PerSample | mod::Squared | mod::Phase);    // cycles (a phase: a cycle is no change)
            // The mod strip until the STRIP page says otherwise: 60 % of the table further on, and a
            // quarter-tone of vibrato.
            t.stripRoute(0, WavetableVoice::LfoPosition, 0.6f);
            t.stripVibrato(0.25f);
            // Pad pressure until AT ROUTE says otherwise: the level, a gain of 1 + pressure.
            t.pressureRoute(0, "AMP", 1.0f);
            t.finish(2);
            return t;
        }();
        static const bool registered = (table.registerGroups(), true);
        juce::ignoreUnused(registered);
        return table;
    }

    // The destinations the voice reads by number beyond the first eight.
    static const WavetableVoice::Dests& dests()
    {
        static const WavetableVoice::Dests found = []
        {
            const auto& t = lfoDests();
            const auto dual = [&t](VoiceFilterSection::Offset o) { return t.indexOfId(FilterDualBase + (int) o - (int) VoiceFilterSection::Count); };
            WavetableVoice::Dests d;
            d.balance = dual(VoiceFilterSection::Balance);
            d.filterFm = dual(VoiceFilterSection::FilterFM);
            d.offsetB = dual(VoiceFilterSection::OffsetB);
            d.envTime = t.ampTime();
            d.filterTime = t.filterTime();
            d.phase = t.indexOfLabel("PM");
            jassert(d.balance > 0 && d.filterFm > 0 && d.offsetB > 0 && d.envTime > 0 && d.filterTime > 0 && d.phase > 0);
            return d;
        }();
        return found;
    }

    // What a slot can read here beside the router's own sources (see "The engine's own sources").
    static const mod::SourceTable& sources()
    {
        static const mod::SourceTable table = []
        {
            mod::SourceTable t(2);
            t.publish("SCAN", mod::Unipolar);                       // WavetableVoice::PubScan
            t.publish("XRCT", mod::AudioRate | mod::Unipolar | mod::Immediate);    // WavetableVoice::PubRect
            t.publish("XFLT", mod::AudioRate | mod::Immediate);                    // WavetableVoice::PubFiltered
            return t;
        }();
        static const bool registered = [] { table.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return table;
    }

    // What an old pressure route (AftertouchRouter::Destination) is here, as a slot's DEST.
    // TIMBRE and WAVE are both the place in the table (POS).
    int pressureDestOf(AftertouchRouter::Destination d) const override
    {
        using D = AftertouchRouter::Destination;
        switch (d)
        {
            case D::Volume:         return WavetableVoice::LfoAmp;
            case D::FilterCutoff:   return WavetableVoice::LfoCutoff;
            case D::MacroTimbre:    return WavetableVoice::LfoPosition;
            case D::WavePosition:   return WavetableVoice::LfoPosition;
            case D::PanSpread:      return WavetableVoice::LfoPan;
            default:                return 0;
        }
    }

    // A key's first place in its sample and along the table: eight keys spread over both, so
    // that choosing one sample for several keys already gives a table that moves.
    static float defaultKeyPosition(int key) { return ((float) key + 0.5f) / (float) WavetableKeys::MAX_KEYS; }
    static float defaultKeyPlace(int key) { return (float) key / (float) (WavetableKeys::MAX_KEYS - 1); }

    bool isRebuildParam(ParamId id) const override { return id == UserSample || id == UserFrames || isKeyParam(id); }

    mod::Router* modRouter() override { return &router; }
    const mod::Router* modRouter() const override { return &router; }

    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        filterSection.updateModeCell(out);
        router.appendPages(out);        // XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2
        // The input's filters are open until set (as this engine's XLP and XHP were).
        for (auto& page : out)
            for (auto& p : page.params)
            {
                if (p.id == mod::Router::xinId(mod::Router::XinLowPass))   p.defaultVal = mod::Router::MAX_XIN_HZ;
                if (p.id == mod::Router::xinId(mod::Router::XinHighPass))  p.defaultVal = mod::Router::MIN_XIN_HZ;
            }
        // The sample bank grows while the app runs (recordings).
        const int bank = SampleBank::get().size();
        for (auto& page : out)
            for (auto& p : page.params)
                if (p.id >= KeySampleBase && p.id < KeySampleBase + WavetableKeys::MAX_KEYS)
                {
                    p.choices = keySampleNames();
                    p.numChoices = bank + 1;
                    p.maxVal = (float) bank;
                }
        return out;
    }

    void pitchBend(float amount) override { bend = juce::jlimit(-1.0f, 1.0f, amount); }
    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }
    void setTempo(float bpm) override
    {
        if (bpm > 0.0f)
            tempo = bpm;        // a synced scan follows it from the next block on
        router.setTempo(bpm);
    }

    EngineType getType() const override { return EngineType::Wavetable; }
    juce::String getName() const override { return "Wavetable"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        for (auto& v : voices)
            v.setSampleRate(sr);
        router.prepare(sr);
        applyParameter(Glide, getParameter(Glide));
        for (auto* f : { &inputLowPass, &inputHighPass })
        {
            f->setSampleRate(sr);
            f->setResonance(0.707f);
            f->reset();
        }
        inputLowPass.setMode(SVFilter::Mode::LowPass);
        inputHighPass.setMode(SVFilter::Mode::HighPass);
        inputLowHz = inputHighHz = -1.0f;
        inputRan = false;
        foldLeft = 0;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        if (userReady.load(std::memory_order_relaxed) || keyReady.load(std::memory_order_relaxed))
        {
            const juce::SpinLock::ScopedTryLockType lock(userLock);
            if (lock.isLocked())
            {
                if (userReady.load(std::memory_order_relaxed))
                {
                    std::swap(userTable, userPending);      // the voices' pointers stay valid
                    userSeconds = userPendingSeconds;
                    userReady.store(false, std::memory_order_relaxed);
                }
                if (keyReady.load(std::memory_order_relaxed))
                {
                    std::swap(keyTable, keyPending);
                    keyReady.store(false, std::memory_order_relaxed);
                }
            }
        }

        // The timed scan: one pass length for the block (so SYNC follows the tempo live), from
        // each voice's own TIME and SPD (a slot on them moves one voice's).
        if (scanMode != WavetableScanMode::Off)
            for (auto& voice : voices)
                voice.setScanPassSeconds(scanPassSeconds(voice.getScanTimeCell(), voice.getScanSpeedCell()));

        // Once per block: tuning and bend, the strip (the cutoff by the filter's WHL amount; its
        // routes are the router's), the track's pressure routes and the track input.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        const float basePitch = tune + bend * bendRange;
        const float wheelOctaves = filterSection.externalOctaves(wheelNow, 0.0f);
        for (auto& voice : voices)
            voice.getFilter().setExternalOctaves(wheelOctaves);
        // (The router fades an input that ends or is shorter than the block.)
        router.beginBlock(audioInput.left, audioInput.right != nullptr ? audioInput.right : audioInput.left,
                          audioInput.connected() ? audioInput.frames : 0, audioInput.envelope, wheelNow, sampleRate);
        const WavetableVoice::Dests& d = dests();
        // An audio-rate slot on POS whose input is heard: the position's modulation is then taken
        // after its glide and mirrored at the ends of the table; and for as long as the router
        // fades such a slot out after it went or its input fell silent (a slot's gain is led to
        // nothing over 4 ms), so that nothing jumps. A slot that moves nothing - its input silent,
        // or its two sides cancelling - leaves the rest of POS as it was: stopping at the ends.
        if (audioOnPosition() && inputHeard())
            foldLeft = (int) (0.004 * sampleRate) + 2 * mod::Router::MAX_CHUNK + numSamples;
        const bool foldPosition = foldLeft > 0;
        foldLeft = std::max(0, foldLeft - numSamples);
        // The VIBR cell: a constant vibrato from the router's vibrato sine (the STRIP page's VRAT).
        const bool vibratoOn = vibrato != 0.0f || router.routed(router.dests().indexOfId(Vibrato));
        const auto setCell = [this](WavetableVoice& voice, int dest, float value)
        {
            applyVoiceParam(voice, router.dests()[dest].id, value);
        };

        // Sub-blocks: the router's control rate. Inside one the voices are rendered one after
        // the other (they share the router's per-sample buffers).
        for (int offset = 0; offset < numSamples;)
        {
            router.readsInput(router.wants(WavetableVoice::PubRect) || router.wants(WavetableVoice::PubFiltered));
            const int n = router.beginChunk(offset, numSamples);
            std::fill(mixL.begin(), mixL.begin() + n, 0.0f);
            std::fill(mixR.begin(), mixR.begin() + n, 0.0f);
            const float vib = vibratoOn ? router.vibrato() : 0.0f;
            // The sources XFLT and XRCT: the input as the router takes it (the mean of its sides,
            // held to +-1), through the two filters where they are not open, and rectified.
            // Worked out only while a slot reads one of them.
            WavetableVoice::InputSources input;
            if (router.wants(WavetableVoice::PubRect) || router.wants(WavetableVoice::PubFiltered))
            {
                filterInput(n);
                input.filtered = filteredInput.data();
                input.rect = rectifiedInput.data();
            }
            else
                inputRan = false;
            for (int v = 0; v < (int) voices.size(); ++v)
            {
                auto& voice = voices[(size_t) v];
                if (! voice.isActive())
                {
                    // Its LFOs keep time; a cell it was left holding goes back to its set value.
                    if (voice.getMod().idle(router, n))
                        voice.getMod().restore(router, [&](int dest, float value) { voice.setControl(d, dest, value, setCell); });
                    continue;
                }
                const float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
                voice.setPitchOffset(basePitch + vib * (voice.getVibrato() * 0.5f));
                voice.render(router, d, mixL.data(), mixR.data(), n, pressure, input, foldPosition, setCell);
            }
            for (int i = 0; i < n; ++i)
            {
                const float sampleL = mixL[(size_t) i] * 0.7f;      // LEVL is applied by each voice (so that a note takes it whole)
                const float sampleR = mixR[(size_t) i] * 0.7f;
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
            auto p = v.getAmpParams();
            stageCurves.applyTo(p, 0);
            v.setAmpParams(p);      // (and the chain's stages, which are copies of this envelope)
        }
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (id == VoiceCountParam)
        {
            // (The allocator keeps to it: no more than VOX voices are given notes, and a lowered
            // VOX releases the voices past it - they ring out.)
            const int n = juce::jlimit(1, MAX_VOICES, (int) std::lround(value));
            params[id] = (float) n;
            allocator.setVoiceLimit(n);
            return;
        }
        if (filterSection.owns(id))
        {
            if (filterSection.swaps(id, value))
                soundSwapped();
            filterSection.set(id, value, VoiceFilterSection::each(voices));
            router.setBase(id, filterSection.get(id));
            return;
        }
        if (spreadSection.owns(id))
        {
            spreadSection.set(id, value);
            if (spreadSection.reseeds(id))
                seedSpread();
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
        if ((id == TableSelect || id == InterpMode || id == EnvChainMode || id == EnvChainStages || id == EnvChainCombine)
            && params[id] != value)
            soundSwapped();
        params[id] = value;
        applyParameter(id, value);
        router.setBase(id, value);
    }

    float getParameter(ParamId id) const override
    {
        if (filterSection.owns(id))
            return filterSection.get(id);
        if (spreadSection.owns(id))
            return spreadSection.get(id);
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
            return VoiceFilterSection::name(filterSection.offsetOf(id));
        if (spreadSection.owns(id))
            return VoiceSpread::name(id - SpreadBase);
        if (mod::Router::owns(id))
            return mod::Router::name(id);
        switch (id)
        {
            case Position:       return "Position";
            case InterpMode:     return "Interp";
            case Detune:         return "Detune";
            case UnisonSpread:   return "Spread";
            case SubLevel:       return "Sub";
            case MorphSpeed:     return "Morph";
            case Level:          return "Level";
            case Octave:         return "Octave";
            case TableSelect:    return "Table";
            case AmpAttack:      return "Attack";
            case AmpDecay:       return "Decay";
            case AmpSustain:     return "Sustain";
            case AmpRelease:     return "Release";
            case EnvChainMode:   return "Env Chain";
            case EnvChainStages: return "Chain Stages";
            case EnvChainCombine: return "Chain Combine";
            case ScanMode:       return "Scan Mode";
            case ScanTime:       return "Scan Time";
            case ScanSync:       return "Scan Sync";
            case ScanLength:     return "Scan Length";
            case ScanSteps:      return "Scan Steps";
            case ScanDirection:  return "Scan Direction";
            case ScanSpeed:      return "Scan Speed";
            case UserFrames:     return "User Frames";
            case KeyAlgo:        return "Key Algorithm";
            case KeyFrames:      return "Key Table Frames";
            case KeyBias:        return "Key Bias";
            case KeyWrap:        return "Key Wrap";
            case KeyAlign:       return "Key Align";
            case KeyNorm:        return "Key Level";
            case KeyTilt:        return "Key Tilt";
            case KeyHarmonics:   return "Key Harmonics";
            default:             break;
        }
        if (id >= KeySampleBase && id < KeySampleBase + WavetableKeys::MAX_KEYS)
            return "Key " + juce::String(id - KeySampleBase + 1) + " Sample";
        if (id >= KeyPositionBase && id < KeyPositionBase + WavetableKeys::MAX_KEYS)
            return "Key " + juce::String(id - KeyPositionBase + 1) + " Position";
        if (id >= KeyPlaceBase && id < KeyPlaceBase + WavetableKeys::MAX_KEYS)
            return "Key " + juce::String(id - KeyPlaceBase + 1) + " Place";
        return "Param " + juce::String(id);
    }

    int getParameterCount() const override { return 24; }

    // Access for tests and advanced use
    VoiceAllocator& getAllocator() { return allocator; }
    WavetableVoice& getVoice(int index) { return voices[(size_t) index]; }
    int getTableCount() const { return (int) tables.size(); }
    const Wavetable& getTable(int index) const { return *tables[(size_t) index]; }

    // The USER table: the sample is cut into frames (FRMS: 64 equal pieces unless set otherwise),
    // so POS scans through the recording. Built on the calling (message) thread; the audio thread
    // swaps it in, with the sample's duration (the natural scan length), at the start of its next
    // block and does nothing else: no allocation, and only a try-lock.
    void buildUserTable(int bankIndex)
    {
        const auto* e = SampleBank::get().entry(bankIndex);
        if (e == nullptr || e->left.size() < 64)
            return;
        std::vector<float> mono(e->left);
        if (e->right.size() == mono.size())
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = 0.5f * (mono[i] + e->right[i]);
        buildUserTableFrom(mono.data(), (int) mono.size(), e->sampleRate);
    }

    // The same from any mono buffer (the bank is not involved).
    void buildUserTableFrom(const float* mono, int numSamples, double sourceRate)
    {
        if (mono == nullptr || numSamples < 64)
            return;
        if (sourceRate <= 0.0)
            sourceRate = 44100.0;
        Wavetable wt = userFrameOption == 0
            ? WavetableFactory::createFromSamples(mono, numSamples, 64)
            : WavetableFactory::createFromSamplesEx(mono, numSamples, sourceRate, userFrameCount(userFrameOption),
                                                    userFramesAligned(userFrameOption));
        wt.name = "USER";
        const juce::SpinLock::ScopedLockType lock(userLock);
        userPending = std::move(wt);
        userPendingSeconds = (double) numSamples / sourceRate;
        userReady.store(true);
    }
    bool userTablePending() const { return userReady.load(); }

    // ---- The KEYS table ---------------------------------------------------------------------
    // Built from the key cells whenever one of them changes while the table is in use, or when
    // the table is chosen after they changed; on the calling (message) thread, and handed to the
    // audio thread exactly as the USER table is. A key's cycle is taken from its sample once and
    // kept until its sample or its position changes, so turning any other cell only recomputes
    // the frames.
    bool keyTablePending() const { return keyReady.load(); }
    int getKeyCount() const
    {
        int n = 0;
        for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
            if (SampleBank::get().entry(keySample[(size_t) k] - 1) != nullptr)
                ++n;
        return n;
    }
    // Whether key n's cycle was taken as one period of a pitched sound (as of the last build).
    bool isKeyPitched(int key) const { return key >= 0 && key < WavetableKeys::MAX_KEYS && keyCache[(size_t) key].pitched; }
    const WavetableKeys::Settings& getKeySettings() const { return keySettings; }

    void buildKeyTable()
    {
        std::vector<WavetableKeys::Key> keys;
        for (int k = 0; k < WavetableKeys::MAX_KEYS; ++k)
        {
            auto& cache = keyCache[(size_t) k];
            const int bankIndex = keySample[(size_t) k] - 1;
            const auto* e = SampleBank::get().entry(bankIndex);
            if (e == nullptr)
            {
                cache.sample = -1;
                cache.pitched = false;
                continue;
            }
            if (cache.sample != bankIndex || cache.position != keyPosition[(size_t) k])
            {
                cache.sample = bankIndex;
                cache.position = keyPosition[(size_t) k];
                cache.valid = WavetableKeys::extract(e->left.data(), e->right.size() == e->left.size() ? e->right.data() : nullptr,
                                                     (int) e->left.size(), e->sampleRate, cache.position,
                                                     cache.spectrum, &cache.pitched);
            }
            if (cache.valid)
                keys.push_back({ cache.spectrum, keyPlace[(size_t) k] });
        }
        setKeyTable(WavetableKeys::build(std::move(keys), keySettings));
    }

    // The same from waves given directly (tests; the bank is not involved).
    void buildKeyTableFrom(std::vector<WavetableKeys::Key> keys)
    {
        setKeyTable(WavetableKeys::build(std::move(keys), keySettings));
    }

    // Duration of the sample behind the USER table that is playing (0 until one has been built).
    double getUserSampleSeconds() const { return userSeconds; }

    // Seconds one scan pass takes right now (TIME, LEN at the tempo or the natural length, over SPD).
    double getScanPassSeconds() const { return scanPassSeconds(scanTime, scanSpeed); }
    // ... with a TIME and SPD of a voice's own (a slot may be moving them).
    double scanPassSeconds(float time, float speed) const
    {
        double seconds = time;
        if (scanSync == 2 && currentTableIndex == USER_TABLE && userSeconds > 0.0)
            seconds = userSeconds * (double) juce::jmax(0.0f, 1.0f - scanStart);
        else if (scanSync == 1)
            seconds = (double) scanLengthBeats(scanLength) * 60.0 / (double) tempo;
        return seconds / (double) speed;
    }

    // The per-note spread, the router's randoms and the RAND scan: every voice's generator starts
    // its sequence again, from the SEED cell and the voice's number.
    void seedSpread()
    {
        for (size_t v = 0; v < voices.size(); ++v)
            voices[v].getSpreadGenerator().seed(spreadSection.seed(), (int) v);
    }

    // (RAND's places are draws of the same generators: spread::Sequence, stream spread::Scan.)

    void selectTable(int index)
    {
        if (index >= 0 && index < (int) tables.size())
        {
            currentTableIndex = index;
            for (auto& v : voices)
                v.setWavetable(tables[(size_t) index]);
            if (index == KEY_TABLE && keyDirty)
                buildKeyTable();
        }
    }

private:
    void setKeyTable(Wavetable wt)
    {
        keyDirty = false;
        const juce::SpinLock::ScopedLockType lock(userLock);
        keyPending = std::move(wt);
        keyReady.store(true);
    }

    // A key cell changed: the table is rebuilt now if it is the one playing, otherwise when it
    // is next chosen (a project that sets thirty key cells on a track playing SAW builds nothing).
    void keysChanged()
    {
        keyDirty = true;
        if (currentTableIndex == KEY_TABLE)
            buildKeyTable();
    }

    template <typename T>
    void setKeyValue(T& member, T value)
    {
        if (member == value)
            return;
        member = value;
        keysChanged();
    }

    void applyKeyParameter(ParamId id, float value)
    {
        const int whole = (int) std::lround(value);
        if (id >= KeySampleBase && id < KeySampleBase + WavetableKeys::MAX_KEYS)
            setKeyValue(keySample[(size_t) (id - KeySampleBase)], juce::jlimit(0, SampleBank::MAX_ENTRIES, whole));
        else if (id >= KeyPositionBase && id < KeyPositionBase + WavetableKeys::MAX_KEYS)
            setKeyValue(keyPosition[(size_t) (id - KeyPositionBase)], juce::jlimit(0.0f, 1.0f, value));
        else if (id >= KeyPlaceBase && id < KeyPlaceBase + WavetableKeys::MAX_KEYS)
            setKeyValue(keyPlace[(size_t) (id - KeyPlaceBase)], juce::jlimit(0.0f, 1.0f, value));
        else switch (id)
        {
            case KeyAlgo:      setKeyValue(keySettings.algo, static_cast<wavemorph::Algo>(juce::jlimit(0, (int) wavemorph::Algo::Count - 1, whole))); break;
            case KeyFrames:    setKeyValue(keySettings.frames, WavetableKeys::frameCount(whole)); break;
            case KeyBias:      setKeyValue(keySettings.bias, juce::jlimit(-1.0f, 1.0f, value)); break;
            case KeyWrap:      setKeyValue(keySettings.wrap, value > 0.5f); break;
            case KeyAlign:     setKeyValue(keySettings.align, value > 0.5f); break;
            case KeyNorm:      setKeyValue(keySettings.norm, juce::jlimit(0, WavetableKeys::NumNorms - 1, whole)); break;
            case KeyTilt:      setKeyValue(keySettings.tilt, juce::jlimit(-6.0f, 6.0f, value)); break;
            case KeyHarmonics: setKeyValue(keySettings.harmonics, juce::jlimit(1, wavemorph::H, whole)); break;
            default: break;
        }
    }

    // One voice's own value of a page cell: what a slot routed to that cell sets, once a
    // sub-block. Envelope times act on the stage that is running (the chain's stages are copies).
    void applyVoiceParam(WavetableVoice& voice, ParamId id, float value) const
    {
        if (id >= FilterBase)      // (the dual block follows the single filter's ids: one offset)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        switch (id)
        {
            case UnisonSpread:  voice.setSpread(value); return;
            case SubLevel:      voice.setSubLevel(value); return;
            case MorphSpeed:    voice.setMorphSpeed(value); return;
            case MorphDepth:    voice.setMorphDepth(value); return;
            case EnvToPosition: voice.setEnvToPosition(value); return;
            case VelToPosition: voice.setVelToPosition(value); return;
            case Glide:         voice.setGlideTime(value); return;
            case Vibrato:       voice.setVibrato(value); return;
            case VelSens:       voice.setVelocitySensitivity(value); return;
            case ScanTime:
            case ScanSpeed:
                if (id == ScanTime) voice.setScanTimeCell(value); else voice.setScanSpeedCell(value);
                voice.setScanPassSeconds(scanPassSeconds(voice.getScanTimeCell(), voice.getScanSpeedCell()));
                return;
            case AmpAttack: case AmpDecay: case AmpSustain: case AmpRelease: break;
            default: return;
        }
        voice.setAmpStage(id - AmpAttack, value);
    }

    // The track input for the n samples of the sub-block, for the sources XFLT and XRCT: as the
    // router reads it (the mean of its sides held to +-1, faded where it ends), through the
    // high-pass and the low-pass (the project's SVF, two poles each; left out while its cell is
    // open), and its absolute value.
    void filterInput(int n)
    {
        const float* in = router.inputNow();
        const float lowHz = router.get(mod::Router::xinId(mod::Router::XinLowPass));
        const float highHz = router.get(mod::Router::xinId(mod::Router::XinHighPass));
        if (lowHz != inputLowHz)   { inputLowHz = lowHz;   inputLowPass.setCutoff(lowHz); }
        if (highHz != inputHighHz) { inputHighHz = highHz; inputHighPass.setCutoff(highHz); }
        if (! inputRan)
        {
            inputLowPass.reset();
            inputHighPass.reset();
            inputRan = true;
        }
        const bool lowOn = lowHz < mod::Router::MAX_XIN_HZ, highOn = highHz > mod::Router::MIN_XIN_HZ;
        for (int i = 0; i < n; ++i)
        {
            float x = in[i];
            if (highOn) x = inputHighPass.process(x);
            if (lowOn)  x = inputLowPass.process(x);
            if (! std::isfinite(x))
            {
                x = 0.0f;
                inputLowPass.reset();
                inputHighPass.reset();
            }
            filteredInput[(size_t) i] = x;
            rectifiedInput[(size_t) i] = std::abs(x);
        }
    }

    // The track input of this block is heard: a sample of it (the mean of its sides) above
    // -120 dB. An input connected but silent, or whose sides cancel, moves nothing.
    bool inputHeard() const
    {
        if (! audioInput.connected())
            return false;
        const float* r = audioInput.right != nullptr ? audioInput.right : audioInput.left;
        for (int i = 0; i < audioInput.frames; ++i)
            if (std::abs(0.5f * (audioInput.left[i] + r[i])) > 1.0e-6f)
                return true;
        return false;
    }

    // True when a slot with an audio-rate source (the track input, XRCT, XFLT) is aimed at POS: the
    // position then swings past the ends of the table and comes back in (WavetableVoice::process)
    // while the input is heard (processBlock: a silent input moves nothing and changes nothing).
    // (A slot whose amount is 0 counts only while another slot moves that amount.)
    bool audioOnPosition() const
    {
        const auto& list = router.sources();
        const int firstAmount = router.dests().visible() + 2 * router.numLfos();
        for (int s = 0; s < mod::Router::NUM_FREE; ++s)
        {
            const auto& c = router.slot(s);
            if (c.dest != WavetableVoice::LfoPosition)
                continue;
            const int source = list.source(c.src);
            if (! (mod::Src::isAudio(source) || (mod::Src::isPublished(source) && list.audio(source - mod::Src::Published1))))
                continue;
            if (c.amount != 0.0f)
                return true;
            for (int other = 0; other < mod::Router::NUM_FREE; ++other)
                if (router.slot(other).dest == firstAmount + s && router.slot(other).src != 0 && router.slot(other).amount != 0.0f)
                    return true;
        }
        for (int cell : { (int) mod::Router::XinDest1, (int) mod::Router::XinDest2 })
            if ((int) router.get(mod::Router::xinId(cell)) == WavetableVoice::LfoPosition && router.get(mod::Router::xinId(cell + 1)) != 0.0f)
                return true;
        return false;
    }

    void applyParameter(ParamId id, float value)
    {
        if (isKeyParam(id))
        {
            applyKeyParameter(id, value);
            return;
        }
        switch (id)
        {
            case Position:
                scanStart = juce::jlimit(0.0f, 1.0f, value);
                for (auto& v : voices)
                    v.setPosition(value);
                break;

            case ScanMode:
                scanMode = static_cast<WavetableScanMode>(juce::jlimit(0, (int) WavetableScanMode::Count - 1, (int) std::lround(value)));
                for (auto& v : voices)
                    v.setScanMode(scanMode);
                break;
            case ScanTime:
                scanTime = juce::jlimit(0.05f, 60.0f, value);
                for (auto& v : voices)
                    v.setScanTimeCell(scanTime);
                break;
            case ScanSync:       scanSync = juce::jlimit(0, 2, (int) std::lround(value)); break;
            case ScanLength:     scanLength = juce::jlimit(0, NUM_SCAN_LENGTHS - 1, (int) std::lround(value)); break;
            case ScanSpeed:
                scanSpeed = juce::jlimit(0.25f, 4.0f, value);
                for (auto& v : voices)
                    v.setScanSpeedCell(scanSpeed);
                break;
            case ScanSteps:
                for (auto& v : voices)
                    v.setScanSteps(scanStepCount((int) std::lround(value)));
                break;
            case ScanDirection:
                for (auto& v : voices)
                    v.setScanReverse(value > 0.5f);
                break;
            case UserFrames:
            {
                const int option = juce::jlimit(0, NUM_USER_FRAME_OPTIONS - 1, (int) std::lround(value));
                if (option != userFrameOption)
                {
                    userFrameOption = option;
                    auto source = params.find(UserSample);       // rebuild the table that is in use
                    if (source != params.end())
                        buildUserTable((int) std::lround(source->second));
                }
                break;
            }

            case InterpMode:
            {
                int mode = juce::jlimit(0, (int) WavetableInterpMode::Count - 1, (int) value);
                for (auto& v : voices)
                    v.setInterpMode(static_cast<WavetableInterpMode>(mode));
                break;
            }

            case Detune:
                for (auto& v : voices)
                    v.setDetune(value);
                break;

            case UnisonSpread:
                unisonSpread = value;
                for (auto& v : voices)
                    v.setSpread(value);
                break;

            case TuneParam:      tune = value; break;
            case BendRangeParam: bendRange = value; break;
            case Vibrato:
                vibrato = value;
                for (auto& v : voices)
                    v.setVibrato(value);
                break;
            case MorphDepth:     for (auto& v : voices) v.setMorphDepth(value); break;
            case EnvToPosition:  for (auto& v : voices) v.setEnvToPosition(value); break;
            case VelToPosition:  for (auto& v : voices) v.setVelToPosition(value); break;
            case VelSens:        for (auto& v : voices) v.setVelocitySensitivity(value); break;
            case Glide:          for (auto& v : voices) v.setGlideTime(value); break;

            case SubLevel:
                for (auto& v : voices)
                    v.setSubLevel(value);
                break;

            case MorphSpeed:
                for (auto& v : voices)
                    v.setMorphSpeed(value);
                break;

            case Level:
                masterLevel = juce::jlimit(0.0f, 1.0f, value);
                for (auto& v : voices)
                    v.setLevel(masterLevel);
                break;

            case Octave:
                for (auto& v : voices)
                    v.setOctave((int) value);
                break;

            case TableSelect:
                selectTable((int) value);
                break;
            case UserSample:
                buildUserTable((int) std::lround(value));
                break;

            case AmpAttack:
            case AmpDecay:
            case AmpSustain:
            case AmpRelease:
            {
                for (auto& v : voices)
                {
                    auto p = v.getAmpParams();
                    if (id == AmpAttack) p.attack = value;
                    else if (id == AmpDecay) p.decay = value;
                    else if (id == AmpSustain) p.sustain = value;
                    else if (id == AmpRelease) p.release = value;
                    v.setAmpParams(p);      // (and the chain's stages, which are copies of this envelope)
                }
                break;
            }

            case EnvChainMode:
            {
                // 0 = off (use simple ADSR), 1 = series, 2 = parallel
                int modeVal = juce::jlimit(0, 2, (int) value);
                for (auto& v : voices)
                {
                    if (modeVal == 0)
                    {
                        v.setEnvelopeChainEnabled(false);
                    }
                    else
                    {
                        v.setEnvelopeChainEnabled(true);
                        auto chainMode = (modeVal == 1)
                            ? EnvelopeChain::ChainMode::Series
                            : EnvelopeChain::ChainMode::Parallel;
                        v.getEnvelopeChain().setMode(chainMode);
                    }
                }
                break;
            }

            case EnvChainStages:
            {
                // Rebuild the chain with N identical envelope stages
                // using the current ADSR parameters
                int stages = juce::jlimit(1, EnvelopeChain::MaxEnvelopes, (int) value);
                for (auto& v : voices)
                {
                    auto& chain = v.getEnvelopeChain();
                    chain.reset();
                    // Reconstruct the chain (reset clears active state, not stages;
                    // we rebuild by creating a new chain)
                    EnvelopeChain newChain;
                    newChain.setSampleRate(sampleRate);
                    newChain.setMode(chain.getMode());
                    newChain.setCombineMode(chain.getCombineMode());
                    auto p = v.getAmpEnvelope().getParams();
                    for (int s = 0; s < stages; ++s)
                        newChain.addEnvelope(p);
                    v.getEnvelopeChain() = newChain;
                    v.applyEnvTimeScale();      // (the new stages take the note's EVAR and the router's ETIM)
                }
                break;
            }

            case EnvChainCombine:
            {
                // 0 = sum, 1 = max
                auto combineMode = ((int) value == 0)
                    ? EnvelopeChain::CombineMode::Sum
                    : EnvelopeChain::CombineMode::Max;
                for (auto& v : voices)
                    v.getEnvelopeChain().setCombineMode(combineMode);
                break;
            }

            default:
                break;
        }
    }

    double sampleRate = 44100.0;
    float masterLevel = 0.8f;
    float unisonSpread = 0.0f;
    float tune = 0.0f, bend = 0.0f, bendRange = 2.0f, wheel = 0.0f, vibrato = 0.0f;
    float wheelNow = 0.0f;                              // the strip, smoothed per block
    int currentTableIndex = 0;

    // Timed scan (SCAN page)
    WavetableScanMode scanMode = WavetableScanMode::Off;
    float scanTime = 2.0f, scanSpeed = 1.0f, scanStart = 0.0f, tempo = 120.0f;
    int scanSync = 0, scanLength = 4, userFrameOption = 0;
    double userSeconds = 0.0, userPendingSeconds = 0.0;

    VoiceFilterSection filterSection { FilterBase, FilterDualBase };
    VoiceSpread spreadSection { SpreadBase };
    mod::Router router { lfoDests(), 2, 0, &sources() };
    std::array<float, mod::Router::MAX_CHUNK> mixL {}, mixR {};
    // The sources XFLT and XRCT for the sub-block, and the filters behind them
    std::array<float, mod::Router::MAX_CHUNK> filteredInput {}, rectifiedInput {};
    SVFilter inputLowPass, inputHighPass;
    float inputLowHz = -1.0f, inputHighHz = -1.0f;
    bool inputRan = false;
    int foldLeft = 0;           // samples for which POS is still taken as an audio-rate movement

    VoiceAllocator allocator;
    std::vector<WavetableVoice> voices;
    std::vector<const Wavetable*> tables;       // TABL order; the built-in ones are shared
    Wavetable userTable, keyTable;              // the two this engine owns (their addresses never change)
    ParamMap params;
    Wavetable userPending, keyPending;
    juce::SpinLock userLock;
    std::atomic<bool> userReady { false }, keyReady { false };

    // Key frames
    struct KeyCache
    {
        int sample = -1;
        float position = -1.0f;
        bool valid = false, pitched = false;
        wavemorph::Spectrum spectrum;
    };
    std::array<int, WavetableKeys::MAX_KEYS> keySample {};
    std::array<float, WavetableKeys::MAX_KEYS> keyPosition {}, keyPlace {};
    std::array<KeyCache, WavetableKeys::MAX_KEYS> keyCache;
    WavetableKeys::Settings keySettings;
    bool keyDirty = true;
};
