#pragma once

#include "VoiceFilterSection.h"
#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/Resonator.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

// ---------------------------------------------------------------------------
// The resonator's voice (the RSN engine's, and - the very same - the drum kit's
// resonator drum, DrumVoice.h): a thin shell around dsp/Resonator.h. A voice is one
// resonator (a bank of 64 ringing modes) tuned by its note and one exciter, both
// computed from latent parameters, then the shared voice filter and the amp envelope.
//
//   MRPH  where the body is among STRG TUBE COMB PLAT BAR BELL HEAD GONG TAM CRSH RIDE CUP
//         CHNA SPLS (0..13, anything between)
//   XMPH  where the exciter is among STRK BOW BRTH SCRP BRSH (0..4)
//   BODY  the body's eight latents, BOD2 its second eight (MEMB DROP WIRE MEET SPRD PAIR HAND
//         DIR), EXC the exciter's five: each cell is added to what the position gives (0 = the
//         point as it is), and the sum is held to 0..1
//   CURV  the junction's curve: PHYS (the bow's and the reed's own) or one of dsp/Saturation.h
//   IN    the track's input (EngineBase::audioInput) under the exciter: up to the middle it is
//         added to it, beyond it the exciter fades; at the top the input alone drives the body.
//         It is let in by velocity x envelope (pressure opens it further, up to twice). GRAN
//         roughens it: noise that follows the input's level is added by the exciter's grain, so
//         that a sparse or tonal input (a drum loop) still finds the body's own partials.
//   GRAIN what GRAN's impulses are made of: SMPL OFF = noise (as before), or a sample of the bank,
//         read as grains (dsp/GrainScheduler.h) at GPOS, which moves on at SCAN; SPRY scatters them,
//         SIZE is a grain's length, GPIT and PJIT its pitch, SHAP its window. GRAN is how many: from
//         2 a second to 400. The body is scraped (struck, with CONT down) with fragments of a recording.
// What each latent is: the header of dsp/Resonator.h.
//
// Energy. What drives the exciter: velocity x the amp envelope, or - when the track's pressure
// router has an EXC route and the voice has had pressure from its pad - that pressure x the
// route's depth (a pad let up all the way stops the bow). Smoothed by LAG, times ENRG (50 = as
// played, 100 = twice). A burst takes velocity x ENRG once, at the note-on.
//
// [U2b] CHAN RING (setRingOut): the envelope is the energy alone - what is heard is the body,
// which rings until it has died (no gate on the output): a cymbal, a gong, a bell.
// As a drum (setDrum: the kit's host). A drum is a one-shot at a pitch of its own (setFrequency):
// the envelope runs attack, decay, release whatever the pad does. With CHAN RING the body rings
// until it has died or a hand is laid on it (choke); without, the envelope is the level too,
// as on the keys (a tom or a clave cut to length). A stroke adds to what still rings. The
// pad's pressure is energy beside the stroke's (setPadEnergy: a bow, a brush).
//
// [U4-RSN] What moves the voice is the host's: on the keys the RSN engine's modulation router
// (ResonatorEngine.h) sets its cells, its filter, the pressure's energy (setPressure) and, sample
// by sample, a pitch ratio and a gain (setPitchMod / setAmpMod); the kit sets its cells. The voice
// keeps no LFO of its own. A note's noise starts from setNoiseSeed (a draw of the host's per-note
// generator) or, when none was given, from the note's number (the same note, the same noise).
//
// All of it is audio-thread safe: no allocation after construction, no lock.
// ---------------------------------------------------------------------------
class ResonatorVoice
{
public:
    // The cells a voice has its own value of (so an LFO or a mod slot moves them voice by voice).
    enum Cell { Morph = 0, ExciterMorph, Decay, Energy, Input, VelSense, Lag,
                BodyFirst, ExciteFirst = BodyFirst + res::NumBody, GrainFirst = ExciteFirst + res::NumExcite,
                GrainPos = GrainFirst, GrainScan, GrainSpray, GrainSize, GrainPitch, GrainJitter,
                MoreFirst, NumCells = MoreFirst + res::NumMore };

    static constexpr float CHOKE_SECONDS = 0.06f;   // to -60 dB under the hand
    static constexpr float DRUM_SILENT = 1.0e-10f;  // what the body holds when a drum's voice is let go

    static constexpr float OUT_LIMIT = res::Resonator::OUT_LIMIT;
    static constexpr float MAX_ENERGY = res::Resonator::MAX_ENERGY;

    void setSampleRate(double sr)
    {
        sampleRate = sr >= 1.0 ? sr : 44100.0;
        ampEnv.setSampleRate(sampleRate);
        envChain.setSampleRate(sampleRate);
        filter.setSampleRate(sampleRate);
        declick.setSampleRate(sampleRate);
        gainDeclick.setSampleRate(sampleRate);
        levelStep = (float) (1.0 / (0.004 * sampleRate));
        core.prepare(sampleRate);
        lagSeen = -1.0f;
        dirty = true;
    }

    void setNote(int note, float vel)
    {
        currentNote = note;
        velocity = juce::jlimit(0.0f, 1.0f, vel);
        noteFrequency = dsp::midiNoteToHz(note);
        baseFrequency = noteFrequency * (double) pitchRatio;
        dirty = true;       // (the velocity's share of the energy)
    }

    // --- [U2b] the kit's host ---------------------------------------------------------
    void setDrum(bool on)           { drum = on; }
    // CHAN RING, on the keys: the envelope is the energy alone and the body is heard until it has
    // died, as a drum's is (a key let go ends the energy, not the ringing).
    void setRingOut(bool on)        { ringOut = on; }
    bool isDrum() const             { return drum; }
    // The pitch of a drum: its own, not a note's.
    void setFrequency(double hz)
    {
        noteFrequency = hz >= 1.0 ? std::min(hz, 20000.0) : 1.0;
        baseFrequency = noteFrequency * (double) pitchRatio;
    }
    void setStroke(float vel)
    {
        currentNote = juce::jlimit(0, 127, (int) std::lround(69.0 + 12.0 * std::log2(noteFrequency / 440.0)));
        velocity = juce::jlimit(0.0f, 1.0f, vel);
        dirty = true;
    }
    // The pad's pressure as energy (0..1 x what the sound makes of it), for as long as it is held.
    void setPadEnergy(float e)
    {
        padEnergy = e > 0.0f ? std::min(e, MAX_ENERGY) : 0.0f;
        if (drum && ! active && padEnergy > 0.0f && everStruck)
        {
            active = true;      // a bowed drum that has rung out starts again under the pad
            choked = false;
            dirty = true;
        }
    }
    // A hand on it: nothing is fed any more and the body dies in a twentieth of a second. The
    // next stroke lifts the hand.
    void choke()
    {
        if (active && ! choked)
        {
            choked = true;
            dirty = true;
            core.stopBurst();
            ampEnv.noteOff();       // (its energy is over; where it is the level too, the sound's release ends it)
        }
    }

    void noteOn()
    {
        if (drum)
        {
            const bool ringing = active;
            filter.noteOn(currentNote, velocity, ringing);
            ampEnv.noteOn();
            active = fresh = dirty = everStruck = true;
            choked = false;
            pushIn = 0;
            quietIn = 0;
            if (! ringing)
            {
                // (From silence: the same hit every time. A stroke on what rings adds to it.)
                energyNow = gateNow = 0.0f;
                core.reset();
                noiseState = (uint32_t) (currentNote * 7919 + 4711) | 1u;
                core.seedNoise(noiseState ^ 0x9e3779b9u);
            }
            return;
        }
        if (active)
        {
            declick.arm();      // struck again while sounding: the restart must not jump
            gainDeclick.arm();
        }
        filter.noteOn(currentNote, velocity, active);
        ampEnv.noteOn();
        if (envChainEnabled)
            envChain.noteOn();
        active = true;
        fresh = true;
        pressed = false;
        pushIn = 0;
        energyNow = gateNow = 0.0f;
        core.reset();
        // The exciter's noise: the host's draw for this note, or the same note, the same noise. (The
        // noise that roughens the track input under IN stays the note's: see setNoiseSeed.)
        noiseState = (uint32_t) (currentNote * 7919 + 4711) | 1u;
        core.seedNoise((noteSeed != 0 ? noteSeed : noiseState) ^ 0x9e3779b9u);
    }

    void noteOff()
    {
        ampEnv.noteOff();
        if (envChainEnabled)
            envChain.noteOff();
        filter.noteOff();
    }

    bool isActive() const { return active; }

    // input: the track's input at this sample; inputLevel: its follower, 0..1.
    float process(float input, float inputLevel)
    {
        if (! active)
            return 0.0f;

        // The host's pitch ratio for this sample (the set value stays).
        double frequency = baseFrequency;
        if (pitchMod != 1.0f)
            frequency *= (double) pitchMod;

        // The cells go to the resonator when one moved, as often as it looks at them (it glides
        // to them by itself).
        if (pushIn > 0)
            --pushIn;
        if (pushIn == 0 && dirty)
        {
            dirty = false;
            pushIn = core.controlPeriod();
            res::BodyLatent body;
            res::ExciteLatent excite;
            for (size_t i = 0; i < body.size(); ++i)   body[i] = cell[(size_t) BodyFirst + i];
            for (size_t i = 0; i < excite.size(); ++i) excite[i] = cell[(size_t) ExciteFirst + i];
            core.setBody(cell[Morph], body);
            res::MoreLatent more;
            for (size_t i = 0; i < more.size(); ++i)   more[i] = cell[(size_t) MoreFirst + i];
            core.setMore(more);
            core.setExciter(cell[ExciterMorph], excite);
            core.setDecay(choked ? CHOKE_SECONDS : cell[Decay]);
            res::GrainSet grains;
            grains.position = cell[GrainPos];
            grains.scan = cell[GrainScan];
            grains.scatter = cell[GrainSpray];
            grains.size = cell[GrainSize];
            grains.pitch = cell[GrainPitch];
            grains.pitchJitter = cell[GrainJitter];
            grains.window = grainWindow;
            core.setGrains(grains);
            if (cell[Lag] != lagSeen)
            {
                lagSeen = cell[Lag];
                lagK = (float) (1.0 - std::exp(-1.0 / (juce::jlimit(0.0005, 1.0, (double) lagSeen) * sampleRate)));
            }
            velGain = 1.0f - juce::jlimit(0.0f, 1.0f, cell[VelSense]) * (1.0f - velocity);
            // IN: the input comes up under the exciter, then the exciter goes.
            const float in = juce::jlimit(0.0f, 1.0f, cell[Input]);
            inputGain = std::min(1.0f, 2.0f * in);
            exciterGain = std::min(1.0f, 2.0f - 2.0f * in);
        }
        core.setPitch(frequency);

        float env;
        if (drum)
        {
            // A one-shot: from the decay's end the envelope goes on into its release by itself.
            env = ampEnv.process();
            if (ampEnv.getStage() == Envelope::Stage::Sustain)
                ampEnv.noteOff();
            if (! ringOut)
            {
                if (! ampEnv.isActive() && padEnergy <= 0.0f)
                    active = false;
            }
            else if (--quietIn <= 0)
            {
                quietIn = 64;
                if (! ampEnv.isActive() && padEnergy <= 0.0f && energyNow < 1.0e-4f
                    && core.held() < DRUM_SILENT && core.moving() < 1.0e-5f)
                    active = false;
            }
        }
        else if (ringOut)
        {
            env = ampEnv.process();
            if (--quietIn <= 0)
            {
                quietIn = 64;
                if (! ampEnv.isActive() && energyNow < 1.0e-4f && core.held() < DRUM_SILENT && core.moving() < 1.0e-5f)
                    active = false;
            }
        }
        else if (envChainEnabled && envChain.getStageCount() > 0)
        {
            env = envChain.process();
            ampEnv.process();       // kept in step for the note-off
            if (! envChain.isActive() && ! ampEnv.isActive())
                active = false;
        }
        else
        {
            env = ampEnv.process();
            if (! ampEnv.isActive())
                active = false;
        }

        // The energy: the pad's pressure once it has come (with an EXC route), else velocity x
        // envelope. With an EXC route a note starts from its strike velocity. The input is let
        // in by velocity x envelope always (a latched chord goes on ringing to it when the pad is
        // let up): there pressure opens the gate further, up to twice.
        const float drive = cell[Energy] * 2.0f;
        const float played = velGain * env;
        const float energyTarget = drum ? (choked ? 0.0f : juce::jlimit(0.0f, MAX_ENERGY, (played + padEnergy) * drive))
                                        : juce::jlimit(0.0f, MAX_ENERGY, (pressureDriven() ? pressureEnergy : played) * drive);
        const float gateTarget = juce::jlimit(0.0f, MAX_ENERGY, played * (pressureDriven() ? 1.0f + pressureEnergy : 1.0f) * drive);
        if (fresh)
        {
            const float struck = std::min(MAX_ENERGY, velGain * drive);
            // (A drum's stroke is there at once, as a pressed pad's is: a tap of a brush has no lag.)
            if (drum)
                energyNow = std::max(energyNow, struck);
            else
                energyNow = exciteRouted ? struck : 0.0f;
            core.strike(struck * exciterGain);
        }
        energyNow += (energyTarget - energyNow) * lagK;
        gateNow += (gateTarget - gateNow) * lagK;

        float outside = 0.0f;
        if (inputGain > 0.0f)
            outside = (input + core.exciter()[res::Grain] * dsp::xorshiftNoise(noiseState) * inputLevel) * inputGain * gateNow;
        float y = core.tick(outside, energyNow * exciterGain);
        y = filter.process(declick.process(y));

        // LEVL is moved to over a few milliseconds under a sounding note; the first sample of a
        // note takes it as it stands. (The host's gain is led by the host.)
        if (fresh)
        {
            fresh = false;
            levelNow = level;
        }
        else if (levelNow != level)
            levelNow = dsp::towards(levelNow, level, levelStep);
        // (CHAN RING: the envelope is the energy, not a gate - the body is heard for as long as it rings.)
        return gainDeclick.process(y * (ringOut ? 1.0f : env) * levelNow * ampMod);
    }

    void reset()
    {
        declick.reset();
        gainDeclick.reset();
        ampEnv.reset();
        envChain.reset();
        filter.reset();
        core.reset();
        active = false;
        pressed = false;
        choked = false;
        energyNow = gateNow = padEnergy = 0.0f;
    }

    // --- Set by the engine ---
    void setCell(int c, float v)
    {
        if (c < 0 || c >= NumCells || ! std::isfinite(v) || cell[(size_t) c] == v)
            return;
        cell[(size_t) c] = v;
        dirty = true;
    }
    float getCell(int c) const { return cell[(size_t) juce::jlimit(0, (int) NumCells - 1, c)]; }
    void setCurve(int c)            { core.setCurve(c); }
    // The recording GRAN's grains are read from (nullptr: noise) and their window.
    void setGrainSource(const res::GrainSource* s) { core.setGrainSource(s); }
    void setGrainWindow(int w)      { grainWindow = w; dirty = true; }
    void setLevel(float v)          { level = juce::jlimit(0.0f, 1.0f, v); }
    // [U4-RSN] The host's modulation for the next sample: a ratio of the pitch, a gain (1 = none).
    void setPitchMod(float ratio)   { pitchMod = ratio; }
    void setAmpMod(float gain)      { ampMod = gain; }
    // The seed the next note's exciter noise starts from - its grain, a sample's grains, the wires
    // (0: the note's own; the drum kit's host gives none). The noise GRAN adds to the track input
    // under IN is not the exciter's and keeps the note's seed (what "Track Input Integration"
    // measured the input-driven body with).
    void setNoiseSeed(uint32_t s)   { noteSeed = s; }
    // Tuning, fine tune, bend and vibrato together, in semitones.
    void setPitchOffset(float semitones)
    {
        pitchRatio = std::exp2(juce::jlimit(-96.0f, 96.0f, semitones) / 12.0f);
        baseFrequency = noteFrequency * (double) pitchRatio;
    }
    // Once a block: is there an EXC route, the pad's pressure, and what the route makes of it.
    void setPressure(bool routed, float pressure, float routedEnergy)
    {
        exciteRouted = routed;
        if (routed && pressure > 0.0f)
            pressed = true;
        pressureEnergy = std::isfinite(routedEnergy) ? juce::jlimit(0.0f, MAX_ENERGY, routedEnergy) : 0.0f;
    }
    float getEnergy() const         { return energyNow; }
    res::Resonator& getResonator()  { return core; }
    Envelope& getAmpEnvelope()      { return ampEnv; }
    VoiceFilter& getFilter()        { return filter; }

    void setEnvelopeChainEnabled(bool enabled) { envChainEnabled = enabled; }
    EnvelopeChain& getEnvelopeChain()          { return envChain; }

private:
    bool pressureDriven() const { return exciteRouted && pressed; }

    res::Resonator core;
    double sampleRate = 44100.0;
    double baseFrequency = 440.0, noteFrequency = 440.0;
    float pitchRatio = 1.0f;
    int currentNote = 60;
    float velocity = 1.0f, velGain = 1.0f;
    bool active = false, fresh = true, dirty = true;
    bool drum = false, choked = false, everStruck = false;     // [U2b] the kit's host
    bool ringOut = false;
    float padEnergy = 0.0f;
    int quietIn = 0;
    int pushIn = 0;

    //                                  MRPH  XMPH  DCAY  ENRG  IN    VSEN  LAG     (the latents' offsets: 0)
    std::array<float, NumCells> cell { { 0.0f, 0.0f, 2.0f, 0.5f, 0.0f, 1.0f, 0.015f } };
    float level = 0.8f, pitchMod = 1.0f, ampMod = 1.0f;
    float levelNow = 0.8f, levelStep = 0.005f;
    float inputGain = 0.0f, exciterGain = 1.0f;
    int grainWindow = 0;
    uint32_t noiseState = 12345, noteSeed = 0;

    // Energy
    bool exciteRouted = false, pressed = false;
    float pressureEnergy = 0.0f, energyNow = 0.0f, gateNow = 0.0f, lagK = 0.001f, lagSeen = -1.0f;

    Envelope ampEnv;
    dsp::Declick declick;       // on the resonator, before the filter
    dsp::Declick gainDeclick;   // on the output
    EnvelopeChain envChain;
    bool envChainEnabled = false;
    VoiceFilter filter;
};
