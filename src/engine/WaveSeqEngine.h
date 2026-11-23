#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "VoiceFilterSection.h"
#include "VoiceBodySection.h"
#include "ModRouter.h"
#include "VoiceSpread.h"
#include "../dsp/PhaseOsc.h"
#include "../dsp/Envelope.h"
#include "../dsp/EnvelopeChain.h"
#include "../dsp/DSPUtils.h"
#include <array>
#include <cmath>

// ============================================================================
// WaveSeqVoice — a single voice that steps through a wave sequence
//
// Each sequence has up to 16 steps, each with independent lanes:
//   - Timing: duration per step (seconds)
//   - Pitch: semitone offset per step
//   - Shape: waveform per step (sine, saw, square, triangle, pulse, noise)
//   - Level: amplitude per step
//
// Each lane can have an independent length (1-16), allowing polymetric
// patterns. The timing lane controls when all lanes advance.
//
// Steps crossfade smoothly based on the crossfade amount parameter.
// Loop modes: forward, reverse, ping-pong, random, one-shot.
// The waves are computed from the phase (polyBLEP); the phase itself is the shared oscillator's
// (dsp/PhaseOsc.h), so the track input or any slot can modulate it (the destination PM).
// Everything that moves the sound goes through the modulation router (ModRouter.h): the LFOs,
// the strip, the pad's pressure routes, the free slots, the track input at audio rate. The voice
// hands the router its four lanes as sources (PLAN SLAN LLAN TLAN: what the pitch, shape, level
// and time lane stand at), so a lane can step any cell of the engine. A random loop draws from
// the one per-note generator (spread::Generator, stream Lanes): a render repeats.
// Last in the voice, after the filter and the amp envelope: BODY (VoiceBodySection.h), off
// until its MIX is raised; the voice is alive while its source sounds or its body rings.
// All state is pre-allocated — no heap allocation on audio thread.
// ============================================================================
class WaveSeqVoice
{
public:
    static constexpr int MAX_STEPS = 16;
    static constexpr int NUM_LANES = 4;

    enum class Waveform { Sine, Saw, Square, Triangle, Pulse, Noise };

    // Random: any step of the lane each time, drawn from the per-note generator.
    // OldRandom (ORND): the random loop as it was before the unification, kept reachable until the
    // owner has heard both: an LCG's low bits shared by the four lanes and the noise step, never
    // started again. With lane lengths of 2, 4, 8 or 16 it is no random walk but a short cycle
    // (each lane goes round 4 steps of 16, or between 2 of 8), another one from note to note,
    // and the same one in every voice of a chord. tests/u4_waveseq_tests.h measures both.
    enum class LoopMode { Forward, Reverse, PingPong, Random, OneShot, OldRandom };

    // Lane indices
    enum Lane { TimingLane = 0, PitchLane = 1, ShapeLane = 2, LevelLane = 3 };

    // The destinations the voice reads by name: indices of the engine's table (WaveSeqEngine::
    // lfoDests), in the order of the DEST cells.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // per sample: a frequency ratio (+-12 semitones, amount squared)
        LfoAmp,         // per sample: a gain, 0..4 (an audio-rate slot may take it to -4)
        LfoPan,         // per sample: +-1
        LfoCutoff,      // per sample: octaves (+-4 at full depth)
        LfoResonance,   // +-0.5
        LfoRate,        // how fast the sequence runs: a factor on RATE, +-2 octaves
        LfoCrossfade,   // the XFAD cell (+-1 around it, held to 0..1)
        LfoPitchRange,  // +-1 added to PRNG, the sum held to 0..2: how far the pitch lane reaches
        NumLfoDests
    };

    // The ones further down the table (after the lanes' steps and BODY's cells, whose number
    // grows): the engine reads their places from the table once (WaveSeqEngine::dests()), so a
    // section that grows cannot leave them pointing at another entry.
    struct Dests
    {
        int envTime = -1;       // ETIM: the amp envelope's time (and its chain's), a factor
        int filterTime = -1;    // FTIM: the filter envelope's time, a factor
        int phase = -1;         // PM, per sample: cycles added to the oscillator's phase (+-1 at full depth)
    };

    // The voice's own sources, in the order the engine publishes them (WaveSeqEngine::sources):
    // what each lane stands at, at control rate (with SLID on, the glide between two steps).
    enum Published
    {
        PubPitch = 0,   // PLAN: the pitch lane's step, -24..24 semitones as -1..1 (before PRNG)
        PubShape,       // SLAN: the shape lane's step, SIN..NOIS as 0..1
        PubLevel,       // LLAN: the level lane's step, 0..1
        PubTime,        // TLAN: the time lane's step, 30 ms..2 s as 0..1 (by octaves)
        NumPublished
    };

    struct SeqStep
    {
        float duration = 0.25f;       // seconds per step
        float pitchOffset = 0.0f;     // semitones
        Waveform shape = Waveform::Saw;
        float level = 1.0f;
    };

    // Per-lane configuration for independent lengths and positions
    struct LaneConfig
    {
        int length = 4;           // independent lane length (1-16)
        int currentStep = 0;      // current position in this lane
        int direction = 1;        // for ping-pong
        bool finished = false;    // for one-shot
    };

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        ampEnv.setSampleRate(sr);
        envChain.setSampleRate(sr);
        filter.setSampleRate(sr);
        body.setSampleRate(sr);
        mods.prepare(sr);
        osc.setSampleRate(sr);
        prevOsc.setSampleRate(sr);
        declick.setSampleRate(sr);
        gainDeclick.setSampleRate(sr);
        levelStep = (float) (1.0 / (0.004 * sr));
    }

    void setNote(int note, float vel)
    {
        currentNote = note;
        velocity = vel;
        baseFrequency = dsp::midiNoteToHz(note);
    }

    void noteOn(const mod::Router& router)
    {
        if (active)
        {
            declick.arm();      // struck again while sounding: the restart must not jump,
            gainDeclick.arm();  // nor the step to the new velocity
        }
        // The note's randoms: RAND, RND2 .. of the router and what a random loop draws while the
        // note sounds, all of the one generator. (Whatever is routed is read before the note's
        // first sample: mod::Voice::tick.)
        generator.advance();
        mods.noteOn(router, currentNote, velocity, generator);
        laneDraws.start(generator, spread::Lanes);
        filter.noteOn(currentNote, velocity, active);
        body.noteOn(currentNote);
        if (envChainEnabled && envChain.getStageCount() > 0)
        {
            envChain.noteOn();
            chainLevel = 0.0f;  // (the chain begins every note from nothing: so does the source ENV)
        }
        else
            ampEnv.noteOn();
        active = true;

        // Reset all lane positions based on loop mode
        for (int lane = 0; lane < NUM_LANES; ++lane)
        {
            if (loopMode == LoopMode::Reverse)
            {
                lanes[lane].currentStep = lanes[lane].length - 1;
                lanes[lane].direction = -1;
            }
            else
            {
                lanes[lane].currentStep = 0;
                lanes[lane].direction = 1;
            }
            lanes[lane].finished = false;
        }

        // Legacy fields for backward compat
        currentStep = lanes[TimingLane].currentStep;
        direction = lanes[TimingLane].direction;

        stepPhase = 0.0;
        osc.reset();
        prevOsc.reset();
        crossfadePhase = 1.0f; // no crossfade at start
        prevTimingStep = lanes[TimingLane].currentStep;
        sequenceFinished = false;
        interpProgress = 0.0f;
        struck = true;
    }

    void noteOff()
    {
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOff();
        else
            ampEnv.noteOff();
        filter.noteOff();
    }

    // The source sounds, or the body it went through still rings.
    bool isActive() const { return active || body.ringing(); }

    // The per-note generator's seed (the engine: its seed and this voice's number).
    void seedRandom(uint32_t engineSeed, int voice) { generator.seed(engineSeed, voice); }

    // One sub-block of n samples, added to left / right. pressure: this voice's pad;
    // setCell(voice, dest, value) sets a page cell of this voice (a control-rate destination of
    // the router). The source sounds, or only the body it went through still rings.
    template <typename SetCell>
    void render(const mod::Router& router, const Dests& d, float* left, float* right, int n, float pressure, const SetCell& setCell)
    {
        // The lanes as sources: worked out only while a slot reads them.
        if (router.wants(PubPitch)) mods.publish(PubPitch, lanePitch() * (1.0f / 24.0f));
        if (router.wants(PubShape)) mods.publish(PubShape, (float) (int) steps[(size_t) lanes[ShapeLane].currentStep].shape * 0.2f);
        if (router.wants(PubLevel)) mods.publish(PubLevel, laneLevel());
        if (router.wants(PubTime))  mods.publish(PubTime, timeAsUnit(steps[(size_t) lanes[TimingLane].currentStep].duration));

        const float envLevel = envChainEnabled && envChain.getStageCount() > 0 ? chainLevel : ampEnv.getCurrentLevel();
        const bool moved = mods.tick(router, n, active ? envLevel : 0.0f, filter.getEnvelope().getCurrentLevel(), pressure);
        if (! moved)
        {
            // Nothing moves this voice: the sequence as it is without any modulation.
            if (cutMoved)
            {
                filter.setLfoOctaves(0.0f);
                cutMoved = false;
            }
            if (! body.engaged())
            {
                for (int i = 0; i < n && active; ++i)
                {
                    const float s = processCore(1.0f, 1.0, 0.0);
                    left[i] += s;
                    right[i] += s;
                }
                return;
            }
            // (Sample by sample "the source sounds or the body rings": a body that has rung out
            // and was put away is not started again by the samples left of the sub-block.)
            for (int i = 0; i < n && isActive(); ++i)
            {
                const float s = body.process(active ? processCore(1.0f, 1.0, 0.0) : 0.0f, active);
                left[i] += s;
                right[i] += s;
            }
            return;
        }

        mods.forEachControl([&](int dest, float value) { setControl(d, dest, value, setCell); });
        const float* pitch = mods.buffer(LfoPitch);
        const float* amp = mods.buffer(LfoAmp);
        const float* pan = mods.buffer(LfoPan);
        const float* cutoff = mods.buffer(LfoCutoff);
        const float* pm = mods.buffer(d.phase);
        if (cutoff == nullptr && cutMoved)
            filter.setLfoOctaves(0.0f);
        cutMoved = cutoff != nullptr;

        // Rate, crossfade and pitch range are moved for these samples only (the set values stay).
        // Only what is routed is held to its cell's range: a stored sound may hold a value beyond
        // it (factory presets have PRNG up to 12), and a route elsewhere must leave it alone.
        const float setSpeed = speedMultiplier, setCrossfade = crossfadeAmount, setRange = pitchRange;
        if (mods.moved(LfoRate))
            speedMultiplier = std::max(0.01f, speedMultiplier / std::max(1.0e-3f, mods.value(LfoRate)));
        if (mods.moved(LfoCrossfade))
            crossfadeAmount = juce::jlimit(0.0f, 1.0f, mods.value(LfoCrossfade));
        if (mods.moved(LfoPitchRange))
            pitchRange = juce::jlimit(0.0f, 2.0f, pitchRange + mods.value(LfoPitchRange));

        for (int i = 0; i < n && isActive(); ++i)
        {
            float s = 0.0f;
            if (active)
            {
                if (cutoff) filter.setLfoOctaves(cutoff[i]);
                s = processCore(amp ? amp[i] : 1.0f, pitch ? (double) pitch[i] : 1.0, pm ? (double) pm[i] : 0.0);
            }
            if (body.engaged())
                s = body.process(s, active);
            // PAN moves its own voice between the channels (balance law: the centre is unchanged).
            const float p = pan ? pan[i] : 0.0f;
            left[i] += p > 0.0f ? s * (1.0f - p) : s;
            right[i] += p < 0.0f ? s * (1.0f + p) : s;
        }

        speedMultiplier = setSpeed;
        crossfadeAmount = setCrossfade;
        pitchRange = setRange;
    }

    // A control-rate destination of the router: the resonance, an envelope's time, or one of this
    // voice's page cells. (RATE, XFAD and PRNG are read where the sub-block is rendered.)
    template <typename SetCell>
    void setControl(const Dests& d, int dest, float value, const SetCell& setCell)
    {
        if (dest == LfoResonance)
            filter.setResonanceMod(value);
        else if (dest == d.envTime)
            setEnvTimeScale(value);
        else if (dest == d.filterTime)
            filter.setEnvTimeScale(value);
        else if (dest != LfoRate && dest != LfoCrossfade && dest != LfoPitchRange)
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

    VoiceBody& getBody() { return body; }
    mod::Voice& getMod() { return mods; }

    // What the pitch and the level lane stand at (with SLID on: on the way to the next step).
    float lanePitch() const
    {
        float v = steps[(size_t) lanes[PitchLane].currentStep].pitchOffset;
        if (laneInterpolation && interpProgress < 1.0f)
            v = v * (1.0f - interpProgress) + steps[(size_t) getNextLaneStep(PitchLane)].pitchOffset * interpProgress;
        return v;
    }
    float laneLevel() const
    {
        float v = steps[(size_t) lanes[LevelLane].currentStep].level;
        if (laneInterpolation && interpProgress < 1.0f)
            v = v * (1.0f - interpProgress) + steps[(size_t) getNextLaneStep(LevelLane)].level * interpProgress;
        return v;
    }
    // A step time as 0..1 over the T cells' range (30 ms .. 2 s), by octaves.
    static float timeAsUnit(float seconds)
    {
        static const float span = std::log2(2.0f / 0.03f);
        return seconds > 0.03f ? std::min(1.0f, std::log2(seconds / 0.03f) / span) : 0.0f;
    }

private:
    // One sample of the voice's own sound: sequence, filter, envelope, level. amp: the gain the
    // router has for the level (1 = none); ratio: its factor on the pitch (1 = none); pm: cycles
    // it adds to the oscillator's phase (0 = none).
    float processCore(float amp, double ratio, double pm)
    {
        float ampVal;
        if (envChainEnabled && envChain.getStageCount() > 0)
        {
            ampVal = chainLevel = envChain.process();
            if (!envChain.isActive())
            {
                active = false;
                return 0.0f;
            }
        }
        else
        {
            ampVal = ampEnv.process();
            if (!ampEnv.isActive())
            {
                active = false;
                return 0.0f;
            }
        }

        if (shapeSwitched)
        {
            shapeSwitched = false;
            declick.arm();
        }

        // Read each lane from its own current position
        int timingIdx = lanes[TimingLane].currentStep;
        int pitchIdx  = lanes[PitchLane].currentStep;
        int shapeIdx  = lanes[ShapeLane].currentStep;
        int levelIdx  = lanes[LevelLane].currentStep;

        // Get values from each lane's current step
        float stepDuration = steps[(size_t) timingIdx].duration;
        float pitchSemitones = steps[(size_t) pitchIdx].pitchOffset * pitchRange;
        Waveform shape = steps[(size_t) shapeIdx].shape;
        float stepLevel = steps[(size_t) levelIdx].level;

        // Apply lane interpolation if enabled
        if (laneInterpolation && interpProgress < 1.0f)
        {
            // Interpolate pitch (portamento-style)
            int nextPitch = getNextLaneStep(PitchLane);
            float nextPitchVal = steps[(size_t) nextPitch].pitchOffset * pitchRange;
            pitchSemitones = pitchSemitones * (1.0f - interpProgress) + nextPitchVal * interpProgress;

            // Interpolate level (linear)
            int nextLevel = getNextLaneStep(LevelLane);
            float nextLevelVal = steps[(size_t) nextLevel].level;
            stepLevel = stepLevel * (1.0f - interpProgress) + nextLevelVal * interpProgress;

            // Shape does NOT interpolate - snaps
        }

        // (2 ^ (semitones / 12) is worked out again only when the semitones have moved: within
        // a step they stand still.)
        const float semis = pitchSemitones + pitchOffsetSemis;
        if (semis != semisNow)
        {
            semisNow = semis;
            semisRatio = std::exp2(semis / 12.0);
        }
        double freq = baseFrequency * semisRatio * ratio;
        double phaseInc = freq / sampleRate;

        // Generate current step waveform
        float currentSample = generateWaveform(shape, osc.at(pm), phaseInc);

        // Crossfade with previous step if active
        float output;
        if (crossfadeAmount > 0.001f && crossfadePhase < 1.0f
            && prevTimingStep != timingIdx)
        {
            // Use previous step's shape and pitch for crossfade source
            float prevPitch = steps[(size_t) prevTimingStep].pitchOffset * pitchRange;
            const float prevSemis = prevPitch + pitchOffsetSemis;
            if (prevSemis != prevSemisNow)
            {
                prevSemisNow = prevSemis;
                prevSemisRatio = std::exp2(prevSemis / 12.0);
            }
            double prevFreq = baseFrequency * prevSemisRatio * ratio;
            double prevPhaseInc = prevFreq / sampleRate;
            Waveform prevShape = steps[(size_t) prevTimingStep].shape;

            float prevSample = generateWaveform(prevShape, prevOsc.at(pm), prevPhaseInc);

            // Advance previous oscillator phase
            prevOsc.advance(prevPhaseInc);

            // Equal-power crossfade
            float fadeIn = crossfadePhase;
            float fadeOut = 1.0f - crossfadePhase;
            float prevLevel = steps[(size_t) prevTimingStep].level;
            output = prevSample * prevLevel * fadeOut
                   + currentSample * stepLevel * fadeIn;
            stepLevelNow = stepLevel;

            // Advance crossfade phase
            float crossfadeDuration = stepDuration * crossfadeAmount * speedMultiplier * tempoScale;
            if (crossfadeDuration > 0.0001f)
                crossfadePhase += (float) (1.0 / (crossfadeDuration * sampleRate));
            else
                crossfadePhase = 1.0f;
        }
        else
        {
            // A step's level is moved to over at most 4 ms where the sequence steps without a
            // crossfade, or where the level is set under the step that sounds: the step is still
            // heard as a step, without the click of a level that jumps in one sample.
            stepLevelNow = struck ? stepLevel : dsp::towards(stepLevelNow, stepLevel, levelStep);
            output = currentSample * stepLevelNow;
        }

        // Advance oscillator phase
        osc.advance(phaseInc);

        // Advance step timing (driven by timing lane)
        float effectiveDuration = stepDuration * speedMultiplier * tempoScale;
        if (effectiveDuration > 0.0001f)
            stepPhase += 1.0 / (effectiveDuration * sampleRate);

        // Advance interpolation progress within step
        if (laneInterpolation && effectiveDuration > 0.0001f)
        {
            interpProgress = (float) stepPhase;
        }

        if (stepPhase >= 1.0)
        {
            stepPhase -= 1.0;
            advanceAllLanes();
            interpProgress = 0.0f;
        }

        // Velocity's share and LEVL are moved to over a few milliseconds under a sounding note;
        // the first sample of a note takes them as they stand. (What the router has for the
        // level - an LFO, the pad's pressure - comes already led: amp.)
        const float velGain = 1.0f - velSens + velSens * velocity;
        if (struck)
        {
            struck = false;
            velGainNow = velGain;
            masterLevelNow = masterLevel;
        }
        else
        {
            if (velGainNow != velGain)         velGainNow = dsp::towards(velGainNow, velGain, levelStep);
            if (masterLevelNow != masterLevel) masterLevelNow = dsp::towards(masterLevelNow, masterLevel, levelStep);
        }
        return gainDeclick.process(filter.process(declick.process(output)) * ampVal * velGainNow * masterLevelNow * amp);
    }

public:
    // --- Accessors ---

    SeqStep& getStep(int index) { return steps[(size_t) index]; }
    const SeqStep& getStep(int index) const { return steps[(size_t) index]; }

    Envelope& getAmpEnvelope() { return ampEnv; }
    EnvelopeChain& getEnvelopeChain() { return envChain; }
    VoiceFilter& getFilter() { return filter; }

    void setEnvelopeChainEnabled(bool enabled) { envChainEnabled = enabled; }
    bool isEnvelopeChainEnabled() const { return envChainEnabled; }

    // Set all lane lengths at once (backward compat with setSequenceLength)
    void setSequenceLength(int len)
    {
        int clamped = juce::jlimit(1, MAX_STEPS, len);
        sequenceLength = clamped;
        for (int i = 0; i < NUM_LANES; ++i)
            lanes[i].length = clamped;
    }
    int getSequenceLength() const { return sequenceLength; }

    // Set individual lane length
    void setLaneLength(int laneIndex, int len)
    {
        if (laneIndex >= 0 && laneIndex < NUM_LANES)
            lanes[laneIndex].length = juce::jlimit(1, MAX_STEPS, len);
    }
    int getLaneLength(int laneIndex) const
    {
        if (laneIndex >= 0 && laneIndex < NUM_LANES)
            return lanes[laneIndex].length;
        return 4;
    }

    // Lane interpolation
    void setLaneInterpolation(bool enabled) { laneInterpolation = enabled; }
    bool getLaneInterpolation() const { return laneInterpolation; }

    void setCrossfadeAmount(float cf) { crossfadeAmount = juce::jlimit(0.0f, 1.0f, cf); }
    float getCrossfadeAmount() const { return crossfadeAmount; }

    void setLoopMode(LoopMode mode) { loopMode = mode; }
    LoopMode getLoopMode() const { return loopMode; }

    void setSpeedMultiplier(float spd) { speedMultiplier = std::max(0.01f, spd); }
    float getSpeedMultiplier() const { return speedMultiplier; }

    void setPitchRange(float range) { pitchRange = range; }
    float getPitchRange() const { return pitchRange; }

    void setMasterLevel(float lvl) { masterLevel = lvl; }
    float getMasterLevel() const { return masterLevel; }

    void setGateLength(float gl) { gateLength = juce::jlimit(0.0f, 1.0f, gl); }

    void setPitchOffset(float semitones) { pitchOffsetSemis = semitones; }   // tune, bend
    void setVelocitySensitivity(float s) { velSens = juce::jlimit(0.0f, 1.0f, s); }
    void setTempoScale(float s) { tempoScale = std::max(0.01f, s); }         // tempo sync of step times
    float getGateLength() const { return gateLength; }

    // Access lane config (read-only for tests/UI)
    const LaneConfig& getLaneConfig(int laneIndex) const { return lanes[laneIndex]; }

    void reset()
    {
        declick.reset();
        gainDeclick.reset();
        ampEnv.reset();
        envChain.reset();
        filter.reset();
        body.reset();
        active = false;
        chainLevel = 0.0f;
        osc.reset();
        prevOsc.reset();
        stepPhase = 0.0;
        currentStep = 0;
        prevTimingStep = 0;
        crossfadePhase = 1.0f;
        sequenceFinished = false;
        direction = 1;
        interpProgress = 0.0f;
        for (int i = 0; i < NUM_LANES; ++i)
        {
            lanes[i].currentStep = 0;
            lanes[i].direction = 1;
            lanes[i].finished = false;
        }
    }

private:
    // Get the next step index for a given lane (used for interpolation)
    int getNextLaneStep(int laneIndex) const
    {
        const auto& lane = lanes[laneIndex];
        switch (loopMode)
        {
            case LoopMode::Forward:
                return (lane.currentStep + 1) % lane.length;
            case LoopMode::Reverse:
                return (lane.currentStep - 1 + lane.length) % lane.length;
            case LoopMode::PingPong:
            {
                int next = lane.currentStep + lane.direction;
                if (next >= lane.length || next < 0)
                    return lane.currentStep; // at boundary, stay
                return next;
            }
            case LoopMode::Random:
            case LoopMode::OldRandom:
                return lane.currentStep; // can't predict random
            case LoopMode::OneShot:
                if (lane.currentStep < lane.length - 1)
                    return lane.currentStep + 1;
                return lane.currentStep;
        }
        return lane.currentStep;
    }

    float generateWaveform(Waveform shape, double phase, double phaseInc) const
    {
        float p = (float) phase;
        float dt = (float) phaseInc;

        switch (shape)
        {
            case Waveform::Sine:
                return std::sin(p * dsp::TWO_PI_F);

            case Waveform::Saw:
            {
                float saw = 2.0f * p - 1.0f;
                saw -= dsp::polyBLEP(p, dt);
                return saw;
            }

            case Waveform::Square:
            {
                float sq = p < 0.5f ? 1.0f : -1.0f;
                sq += dsp::polyBLEP(p, dt);
                sq -= dsp::polyBLEP(std::fmod(p + 0.5f, 1.0f), dt);
                return sq;
            }

            case Waveform::Triangle:
            {
                float tri = p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
                return tri;
            }

            case Waveform::Pulse:
            {
                float pw = 0.25f;
                float pulse = p < pw ? 1.0f : -1.0f;
                pulse += dsp::polyBLEP(p, dt);
                pulse -= dsp::polyBLEP(std::fmod(p + (1.0f - pw), 1.0f), dt);
                return pulse;
            }

            case Waveform::Noise:
            {
                // Fast deterministic noise using xorshift on mutable seed
                noiseState ^= noiseState << 13;
                noiseState ^= noiseState >> 17;
                noiseState ^= noiseState << 5;
                return (float) (noiseState & 0x7FFFFFFF) / (float) 0x7FFFFFFF * 2.0f - 1.0f;
            }
        }

        return 0.0f;
    }

    // Advance a single lane according to the current loop mode
    void advanceLane(int laneIndex)
    {
        auto& lane = lanes[laneIndex];

        if (lane.finished)
            return;

        switch (loopMode)
        {
            case LoopMode::Forward:
                lane.currentStep = (lane.currentStep + 1) % lane.length;
                break;

            case LoopMode::Reverse:
                lane.currentStep = (lane.currentStep - 1 + lane.length) % lane.length;
                break;

            case LoopMode::PingPong:
                lane.currentStep += lane.direction;
                if (lane.currentStep >= lane.length)
                {
                    lane.direction = -1;
                    lane.currentStep = std::max(0, lane.length - 2);
                }
                else if (lane.currentStep < 0)
                {
                    lane.direction = 1;
                    lane.currentStep = std::min(1, lane.length - 1);
                }
                break;

            case LoopMode::Random:
            {
                // The next draw of this note's run (the per-note generator, stream Lanes): any
                // step of the lane, each as likely as the other.
                lane.currentStep = std::min(lane.length - 1, (int) (laneDraws.unipolar() * (float) lane.length));
                break;
            }

            case LoopMode::OneShot:
                if (lane.currentStep < lane.length - 1)
                    ++lane.currentStep;
                else
                    lane.finished = true;
                break;

            case LoopMode::OldRandom:
                // (As it was, on the state the noise step also turns: see LoopMode.)
                noiseState = noiseState * 1664525u + 1013904223u;
                lane.currentStep = (int) (noiseState % (uint32_t) lane.length);
                break;
        }
    }

    // Advance all lanes when the timing lane triggers a step change
    void advanceAllLanes()
    {
        if (sequenceFinished)
            return;

        prevTimingStep = lanes[TimingLane].currentStep;
        prevOsc = osc;
        crossfadePhase = 0.0f;
        const Waveform shapeBefore = steps[(size_t) lanes[ShapeLane].currentStep].shape;

        // Advance all lanes independently
        for (int i = 0; i < NUM_LANES; ++i)
            advanceLane(i);

        // Another waveform takes over in mid-cycle: without a crossfade that is a jump. The jump is
        // carried off over 2 ms (the step is heard as a step, not as a click).
        // (Armed with the next sample, the first of the new waveform.)
        shapeSwitched = crossfadeAmount <= 0.001f && steps[(size_t) lanes[ShapeLane].currentStep].shape != shapeBefore;

        // Update legacy fields
        currentStep = lanes[TimingLane].currentStep;
        direction = lanes[TimingLane].direction;

        // Check if all lanes are finished (one-shot mode)
        if (loopMode == LoopMode::OneShot)
        {
            bool allFinished = true;
            for (int i = 0; i < NUM_LANES; ++i)
            {
                if (!lanes[i].finished)
                {
                    allFinished = false;
                    break;
                }
            }
            sequenceFinished = allFinished;
        }
    }

    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    int currentNote = 60;
    float velocity = 1.0f;
    bool active = false;

    // Sequence data — pre-allocated, no heap on audio thread
    std::array<SeqStep, MAX_STEPS> steps {};
    int sequenceLength = 4;  // legacy: sets all lane lengths at once
    int currentStep = 0;     // legacy: mirrors timing lane
    int prevTimingStep = 0;
    int direction = 1;       // legacy: mirrors timing lane
    double stepPhase = 0.0;
    bool sequenceFinished = false;

    // Independent lane configuration
    std::array<LaneConfig, NUM_LANES> lanes {};

    // Lane interpolation
    bool laneInterpolation = false;
    float interpProgress = 0.0f;

    // Crossfade
    float crossfadeAmount = 0.5f;
    float crossfadePhase = 1.0f;

    // Oscillator state (current and previous for crossfade): the shared phase (dsp/PhaseOsc.h)
    phaseosc::PhaseOsc osc, prevOsc;
    float semisNow = 0.0f, prevSemisNow = 0.0f;         // the semitones the two ratios below are for
    double semisRatio = 1.0, prevSemisRatio = 1.0;

    // Parameters
    float speedMultiplier = 1.0f;
    float pitchRange = 1.0f;
    float masterLevel = 0.8f;
    float gateLength = 1.0f;
    float pitchOffsetSemis = 0.0f;
    float velSens = 1.0f;
    float velGainNow = 1.0f, masterLevelNow = 0.8f, stepLevelNow = 1.0f, levelStep = 0.005f;
    bool struck = true, shapeSwitched = false;
    float tempoScale = 1.0f;
    LoopMode loopMode = LoopMode::Forward;

    // DSP
    Envelope ampEnv;
    dsp::Declick declick;       // on the oscillator, before the filter (whose state smears a jump)
    dsp::Declick gainDeclick;   // on the output
    mod::Voice mods;
    bool cutMoved = false;       // the router's cutoff octaves are on the filter
    float chainLevel = 0.0f;     // the envelope chain as of its last sample (the source ENV while CHAN is on)
    spread::Generator generator; // the per-note randoms: the router's RAND .., a random loop's steps
    spread::Sequence laneDraws;
    VoiceFilter filter;          // after the wave mix, before the amp envelope
    VoiceBody body;              // last: after the amp envelope (it rings on when the source has stopped)
    EnvelopeChain envChain;
    bool envChainEnabled = false;
    // The noise step's generator (not a draw of the note: it runs on from note to note), and
    // what ORND steps by. mutable for const generateWaveform.
    mutable uint32_t noiseState = 12345u;
};


// ============================================================================
// WaveSeqEngine — wave sequencing engine using VoiceAllocator
//
// Parameters:
//   0  = Seq Length       (1-16 steps, sets all lanes)
//   1  = Crossfade        (0-100%)
//   2  = Loop Mode        (0=Forward, 1=Reverse, 2=PingPong, 3=Random, 4=OneShot, 5=the old random)
//   3  = Speed            (0.1x-10x multiplier)
//   4  = Pitch Range      (0-24 semitones — scales per-step pitch offsets)
//   5  = Shape Mix        (global waveform select, 0-5)
//   6  = Level            (master output 0-1)
//   7  = Gate Length       (0-1, controls amp envelope sustain)
//   8  = Timing Lane Len  (1-16, independent timing lane length)
//   9  = Pitch Lane Len   (1-16, independent pitch lane length)
//   10 = Shape Lane Len   (1-16, independent shape lane length)
//   11 = Level Lane Len   (1-16, independent level lane length)
//   12 = Lane Interp      (0=off, 1=on — interpolate between lane values)
//   13 = Env Chain Mode   (0=off, 1=series, 2=parallel)
//   14 = Env Chain Stages (number of envelope stages in chain)
//   15 = Env Chain Combine(0=sum, 1=max)
// ============================================================================
class WaveSeqEngine : public EngineBase
{
public:
    enum Param
    {
        SeqLength      = 0,
        Crossfade      = 1,
        LoopModeParam  = 2,
        Speed          = 3,
        PitchRange     = 4,
        ShapeMix       = 5,
        Level          = 6,
        GateLength     = 7,
        TimingLaneLen  = 8,
        PitchLaneLen   = 9,
        ShapeLaneLen   = 10,
        LevelLaneLen   = 11,
        LaneInterp     = 12,
        EnvChainMode   = 13,
        EnvChainStages = 14,
        EnvChainCombine = 15,
        AmpAttack      = 16,
        AmpDecay       = 17,
        AmpSustain     = 18,
        AmpRelease     = 19,
        VelSens        = 20,
        TuneParam      = 21,   // semitones
        FineParam      = 22,   // cents
        BendRangeParam = 23,   // semitones
        TempoSync      = 24,   // on: a step time of 250 ms is one sixteenth at the current tempo
        Rate           = 25,   // playback rate of the sequence (higher = faster)
        VoiceCountParam = 26,  // VOX: polyphony 1..16 (default 8)
        // Per-step data, + step index 0..15
        StepPitch      = 32,   // semitones (scaled by PitchRange)
        StepShape      = 48,   // waveform
        StepLevel      = 64,
        StepTime       = 80,   // seconds
        FilterBase     = 100,  // 100-115: the per-voice filter (VoiceFilterSection offsets)
        // 133-140: BODY (VoiceBodySection cells). In the engine's own block: 158-189 are the
        // modulation router's on every engine (the id rule at the top of ModRouter.h).
        BodyBase       = 133
        // 158-189 and 700-772 are the modulation router's (mod::Router: LFO1, LFO2, STRIP, XIN,
        // the slots, the custom LFO shape): everything that moves this engine's sound goes
        // through it, the pad's pressure routes and the track input at audio rate included.
    };

    static constexpr int MAX_VOICES = 16;

    explicit WaveSeqEngine(int maxVoices = 8)
        : allocator(MAX_VOICES)
    {
        voices.resize((size_t) MAX_VOICES);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            auto& voice = voices[(size_t) voiceIdx];
            voice.setNote(v.note, v.velocity);
            voice.noteOn(router);
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t) voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out.
        allocator.setVoiceSoundingQuery([this](int voiceIdx) { return voices[(size_t) voiceIdx].isActive(); });

        // The per-note randoms (RAND, RND2 .., a random loop's steps) repeat from run to run: the
        // same notes in the same order on a new engine draw the same values. (This engine has no
        // ANLG page, so no SEED cell: the seed is fixed until it gets one.)
        for (int v = 0; v < MAX_VOICES; ++v)
            voices[(size_t) v].seedRandom(0, v);

        // Seed every parameter (including a default 8-step sequence that actually moves) so
        // getParameter is always the true value.
        static const float defPitch[16] = { 0, 7, 12, 7, 0, 3, 7, 10, 0, 7, 12, 7, 0, 5, 7, 12 };
        static const float defShape[16] = { 1, 1, 2, 1, 3, 1, 2, 4, 1, 1, 2, 1, 3, 1, 2, 0 };
        static const float defLevel[16] = { 1, 0.7f, 0.8f, 0.6f, 1, 0.7f, 0.8f, 0.6f, 1, 0.7f, 0.8f, 0.6f, 1, 0.7f, 0.8f, 0.6f };
        for (auto& page : pages())
            for (auto& p : page.params)
                if (p.isValid())
                {
                    float v = p.defaultVal;
                    if (p.id == VoiceCountParam) v = (float) maxVoices;      // (held to 1 .. 16 by the cell)
                    else if (p.id >= StepPitch && p.id < StepPitch + 16) v = defPitch[p.id - StepPitch];
                    else if (p.id >= StepShape && p.id < StepShape + 16) v = defShape[p.id - StepShape];
                    else if (p.id >= StepLevel && p.id < StepLevel + 16) v = defLevel[p.id - StepLevel];
                    setParameter(p.id, v);
                }
        filterSection.applyAll(VoiceFilterSection::each(voices));
    }

    // SEQ and LANES (how the sequence runs), then two pages per lane with one cell per step
    // (encoder N = step N, or step N + 8 on the second page), then AMP, the filter's two and BODY:
    // the engine's own pages, without the modulation router's.
    static const ParamPages& pages()
    {
        static const ParamPages list = build();
        return list;
    }

    // What can be moved (the modulation router's table), in the order the DEST cells have always
    // had: the original eight (pitch, level, pan, cutoff, resonance, the sequence's rate and the
    // pitch lane's reach as inputs of the voice; XFAD, a page cell), then level, the amp envelope,
    // the filter and the other LFO, then the lanes step by step (P1-P16 pitch, L1-L16 level,
    // T1-T16 time: each voice has its own copy of the sequence, so a slot bends one voice's
    // step), BODY's eight cells (BMIX ... BKTK), and new at the end: the two envelope times ETIM /
    // FTIM and PM, the oscillator's phase. 86 entries of DestTable::MAX 96.
    // Per sample (so the track input reaches them at audio rate): PTCH, AMP, PAN, CUT, PM.
    // RATE and PRNG are inputs, not the cells: RATE is a factor on the sequence's speed that is
    // not held to the RATE cell's range (the strip's default doubles the speed whatever RATE is),
    // and PRNG an offset of +-1 on a cell of 0..2 whose stored value may lie beyond that range
    // (factory presets hold up to 12): the voice holds the sum to 0..2 only while a slot is
    // aimed at it, and a sound nothing is routed to keeps its value.
    // Not offered: LEN, LOOP, the lane lengths, SYNC, SLID, SHP*, the shape lane, CHAN, STGS,
    // COMB (choices and counts), TUNE, FINE and BEND (PTCH is the pitch), and what the filter
    // leaves out.
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
            t.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);              // octaves
            t.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
            t.input("RATE", 1.0f, 2.0f, 1.0f / 64.0f, 64.0f, mod::Exp);             // a factor: +-2 octaves
            t.add(p, Crossfade);
            t.input("PRNG", 0.0f, 1.0f, -2.0f, 2.0f);                               // an offset
            jassert(t.size() == WaveSeqVoice::NumLfoDests);
            t.group();
            for (int id : { (int) Level, (int) AmpAttack, (int) AmpDecay, (int) AmpSustain, (int) AmpRelease, (int) VelSens })
                t.add(p, id);
            VoiceFilterSection::addLfoDests(t, p, FilterBase);
            t.addCross();
            for (int base : { (int) StepPitch, (int) StepLevel, (int) StepTime })
                for (int half = 0; half < 2; ++half)
                {
                    t.group();
                    for (int i = 0; i < 8; ++i)
                        t.add(p, base + half * 8 + i);
                }
            t.group();
            for (int c = 0; c < (int) VoiceBody::NumCells; ++c)
                t.add(p, BodyBase + c, VoiceBodySection::destLabel(c));
            // New with the router, after what was there before (a stored DEST keeps its meaning).
            t.group();
            t.envTimes();
            t.input("PM", 0.0f, 1.0f, -4.0f, 4.0f, mod::PerSample | mod::Phase);    // cycles (a phase: a cycle is no change)
            // The mod strip until the STRIP page says otherwise: the sequence up to twice as fast
            // (RATE +50 = one octave of speed, so the top of the strip is in time again).
            t.stripRoute(0, WaveSeqVoice::LfoRate, 0.5f);
            // Pad pressure until AT ROUTE says otherwise: the level, a gain of 1 + pressure.
            t.pressureRoute(0, "AMP", 1.0f);
            t.finish(2);
            return t;
        }();
        static const bool registered = [] { table.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return table;
    }

    // The destinations the voice reads by number beyond the first eight: ETIM, FTIM and PM, where
    // the table has them ("U4 Wave Sequencer" checks that each is found and acts).
    static const WaveSeqVoice::Dests& dests()
    {
        static const WaveSeqVoice::Dests found = []
        {
            const auto& t = lfoDests();
            WaveSeqVoice::Dests d;
            d.envTime = t.ampTime();
            d.filterTime = t.filterTime();
            d.phase = t.indexOfLabel("PM");
            return d;
        }();
        return found;
    }

    // What a slot can read here beside the router's own sources: the list SRC and VIA show. The
    // four lanes, each as the step it stands at in this voice (WaveSeqVoice::Published), at
    // control rate: a lane steps the filter, the body, an LFO's rate, another lane's steps.
    //   PLAN  the pitch lane: -24..24 semitones as -1..1 (the step as set, before PRNG)
    //   SLAN  the shape lane: SIN SAW SQR TRI PLS NOIS as 0, 0.2 .. 1
    //   LLAN  the level lane: 0..1
    //   TLAN  the time lane: 30 ms .. 2 s as 0..1, by octaves
    // With SLID on, PLAN and LLAN glide between steps as the sound does. A lane whose length
    // differs from the time lane's is a pattern of its own length (PLEN 3 against TLEN 8).
    static const mod::SourceTable& sources()
    {
        static const mod::SourceTable table = []
        {
            mod::SourceTable t(2);
            t.publish("PLAN");
            t.publish("SLAN", mod::Unipolar);
            t.publish("LLAN", mod::Unipolar);
            t.publish("TLAN", mod::Unipolar);
            jassert(t.published() == WaveSeqVoice::NumPublished);
            return t;
        }();
        static const bool registered = [] { table.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return table;
    }

    // What an old pressure route (AftertouchRouter::Destination) is here, as a slot's DEST.
    int pressureDestOf(AftertouchRouter::Destination d) const override
    {
        using D = AftertouchRouter::Destination;
        switch (d)
        {
            case D::Volume:         return WaveSeqVoice::LfoAmp;
            case D::FilterCutoff:   return WaveSeqVoice::LfoCutoff;
            case D::PanSpread:      return WaveSeqVoice::LfoPan;
            default:                return 0;
        }
    }

    mod::Router* modRouter() override { return &router; }
    const mod::Router* modRouter() const override { return &router; }

    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        filterSection.updateModeCell(out);
        router.appendPages(out);        // XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2
        return out;
    }

    void pitchBend(float amount) override { bend = juce::jlimit(-1.0f, 1.0f, amount); }
    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }
    void setTempo(float bpm) override
    {
        tempoBpm = bpm;
        updateTempoScale();
        router.setTempo(bpm);
    }

    EngineType getType() const override { return EngineType::WaveSeq; }
    juce::String getName() const override { return "Wave Sequencer"; }

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
        // routes are the router's: by default the sequence runs up to twice as fast), the track's
        // pressure routes and the track input.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        const float semitones = tune + fine / 100.0f + bend * bendRange;
        const float wheelOctaves = filterSection.externalOctaves(wheelNow, 0.0f);
        for (auto& voice : voices)
        {
            voice.setPitchOffset(semitones);
            voice.getBody().setPitchOffset(semitones);
            voice.getFilter().setExternalOctaves(wheelOctaves);
        }
        router.beginBlock(audioInput.left, audioInput.right, audioInput.connected() ? audioInput.frames : 0,
                          audioInput.envelope, wheelNow, sampleRate);
        const auto setCell = [this](WaveSeqVoice& voice, int dest, float value)
        {
            applyVoiceParam(voice, router.dests()[dest].id, value);
        };
        const WaveSeqVoice::Dests& d = dests();

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
                        voice.getMod().restore(router, [&](int dest, float value) { voice.setControl(d, dest, value, setCell); });
                    continue;
                }
                const float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
                voice.render(router, d, mixL.data(), mixR.data(), n, pressure, setCell);
            }
            for (int i = 0; i < n; ++i)
            {
                const float sampleL = mixL[(size_t) i] * 0.7f, sampleR = mixR[(size_t) i] * 0.7f;
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
        if (bodySection.owns(id))
        {
            bodySection.set(id, value, VoiceBodySection::each(voices));
            router.setBase(id, bodySection.get(id));
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
        if (mod::Router::owns(id))
        {
            router.set(id, value);
            return;
        }
        if (StageCurveParam::owns(id))
        {
            stageCurves.set(id, value);
            if (StageCurveParam::envelopeOf(id) == 0)
                applyAmpEnvelope();
            return;
        }
        if ((id == EnvChainMode || id == EnvChainStages || id == EnvChainCombine) && params[id] != value)
            soundSwapped();
        params[id] = value;
        applyParameter(id, value);
        router.setBase(id, value);
    }

    float getParameter(ParamId id) const override
    {
        if (bodySection.owns(id))
            return bodySection.get(id);
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
        if (bodySection.owns(id))
            return VoiceBodySection::name(id - BodyBase);
        if (filterSection.owns(id))
            return VoiceFilterSection::name(id - FilterBase);
        if (mod::Router::owns(id))
            return mod::Router::name(id);
        switch (id)
        {
            case SeqLength:       return "Seq Length";
            case Crossfade:       return "Crossfade";
            case LoopModeParam:   return "Loop Mode";
            case Speed:           return "Speed";
            case PitchRange:      return "Pitch Range";
            case ShapeMix:        return "Shape Mix";
            case Level:           return "Level";
            case GateLength:      return "Gate Length";
            case TimingLaneLen:   return "Tim Len";
            case PitchLaneLen:    return "Pit Len";
            case ShapeLaneLen:    return "Shp Len";
            case LevelLaneLen:    return "Lvl Len";
            case LaneInterp:     return "Lane Interp";
            case EnvChainMode:    return "Env Chain";
            case EnvChainStages:  return "Chain Stg";
            case EnvChainCombine: return "Chain Comb";
            default:              return "Param " + juce::String(id);
        }
    }

    int getParameterCount() const override { return 96; }

    // --- Direct access for configuration ---

    WaveSeqVoice& getVoice(int index) { return voices[(size_t) index]; }
    VoiceAllocator& getAllocator() { return allocator; }

    // Set a step's lane data across all voices (for UI / parameter locks)
    void setStepData(int stepIndex, float duration, float pitch,
                     WaveSeqVoice::Waveform shape, float level)
    {
        for (auto& voice : voices)
        {
            auto& step = voice.getStep(stepIndex);
            step.duration = duration;
            step.pitchOffset = pitch;
            step.shape = shape;
            step.level = level;
        }
    }

private:
    static ParamPages build()
    {
        static const char* const loops[] = { "FWD", "REV", "PING", "RAND", "ONCE", "ORND" };
        static const char* const shapes[] = { "SIN", "SAW", "SQR", "TRI", "PLS", "NOIS" };
        static const char* const chain[] = { "OFF", "SER", "PAR" };
        static const char* const combine[] = { "SUM", "MAX" };
        static const char* const stepLabels[4][16] = {
            { "P1","P2","P3","P4","P5","P6","P7","P8","P9","P10","P11","P12","P13","P14","P15","P16" },
            { "S1","S2","S3","S4","S5","S6","S7","S8","S9","S10","S11","S12","S13","S14","S15","S16" },
            { "L1","L2","L3","L4","L5","L6","L7","L8","L9","L10","L11","L12","L13","L14","L15","L16" },
            { "T1","T2","T3","T4","T5","T6","T7","T8","T9","T10","T11","T12","T13","T14","T15","T16" } };
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;

        ParamPages out = {
            { "SEQ", "SEQ", {{
                { "LEN",  SeqLength, 1, 16, 8, I },
                { "XFAD", Crossfade, 0, 1, 0.3f, L },
                { "LOOP", LoopModeParam, 0, 5, 0, C, "", loops, 6 },
                { "RATE", Rate, 0.25f, 4, 1, G, "x" },
                { "PRNG", PitchRange, 0, 2, 1, L, "x" },
                { "TUNE", TuneParam, -24, 24, 0, I, "st" },
                { "FINE", FineParam, -100, 100, 0, L, "ct" },
                { "LEVL", Level, 0, 1, 0.8f, L } }} },
            { "LANES", "SEQ", {{
                { "TLEN", TimingLaneLen, 1, 16, 8, I },
                { "PLEN", PitchLaneLen, 1, 16, 8, I },
                { "SLEN", ShapeLaneLen, 1, 16, 8, I },
                { "LLEN", LevelLaneLen, 1, 16, 8, I },
                { "SYNC", TempoSync, 0, 1, 1, C, "", ParamChoices::offOn, 2 },
                { "SLID", LaneInterp, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
                { "SHP*", ShapeMix, 0, 5, 1, C, "", shapes, 6 },
                { "BEND", BendRangeParam, 0, 24, 2, I, "st" } }} },
        };

        static const char* const laneNames[4][2] = { { "PIT A", "PIT B" }, { "SHP A", "SHP B" },
                                                     { "LVL A", "LVL B" }, { "TIM A", "TIM B" } };
        static const char* const sections[4] = { "PITCH", "SHAPE", "LEVEL", "TIME" };
        for (int lane = 0; lane < 4; ++lane)
        {
            for (int half = 0; half < 2; ++half)
            {
                ParamPage p; p.name = laneNames[lane][half]; p.section = sections[lane];
                for (int i = 0; i < 8; ++i)
                {
                    int step = half * 8 + i;
                    const char* label = stepLabels[lane][step];
                    switch (lane)
                    {
                        case 0: p.params[(size_t) i] = { label, StepPitch + step, -24, 24, 0, I, "st" }; break;
                        case 1: p.params[(size_t) i] = { label, StepShape + step, 0, 5, 1, C, "", shapes, 6 }; break;
                        case 2: p.params[(size_t) i] = { label, StepLevel + step, 0, 1, 1, L }; break;
                        default: p.params[(size_t) i] = { label, StepTime + step, 0.03f, 2, 0.25f, G, "s" }; break;
                    }
                }
                out.push_back(p);
            }
        }

        out.push_back({ "AMP", "AMP", {{
            { "ATK",  AmpAttack, 0.001f, 10, 0.005f, G, "s" },
            { "DEC",  AmpDecay, 0.001f, 10, 0.3f, G, "s" },
            { "SUS",  AmpSustain, 0, 1, 0.8f, L },
            { "REL",  AmpRelease, 0.001f, 10, 0.2f, G, "s" },
            { "VEL",  VelSens, 0, 1, 0.7f, L },
            { "CHAN", EnvChainMode, 0, 2, 0, C, "", chain, 3 },
            { "STGS", EnvChainStages, 1, 8, 2, I },
            { "COMB", EnvChainCombine, 0, 1, 0, C, "", combine, 2 } }} });
        // VOICE: polyphony. Before the filter pages (before STRIP / LFO pages).
        out.push_back({ "VOICE", "VOX", {{ { "VOX", VoiceCountParam, 1, MAX_VOICES, 8, I }, {}, {}, {}, {}, {}, {}, {} }} });
        VoiceFilterSection::appendPages(out, FilterBase);
        VoiceBodySection::appendPage(out, BodyBase);
        return out;
    }

    void updateTempoScale()
    {
        // With sync on, a 250 ms step lasts one sixteenth note.
        float scale = syncOn ? (60.0f / juce::jmax(20.0f, tempoBpm) / 4.0f) / 0.25f : 1.0f;
        for (auto& v : voices)
            v.setTempoScale(scale);
    }

    void applyAmpEnvelope()
    {
        Envelope::Params ep;
        ep.attack = getParameter(AmpAttack);
        ep.decay = getParameter(AmpDecay);
        ep.sustain = getParameter(AmpSustain);
        ep.release = getParameter(AmpRelease);
        stageCurves.applyTo(ep, 0);
        for (auto& v : voices)
        {
            v.getAmpEnvelope().setParams(ep);
            auto& chain = v.getEnvelopeChain();
            for (int s = 0; s < chain.getStageCount(); ++s)
                chain.getEnvelopeMut(s).setParams(ep);
        }
    }

    // One voice's own value of a page cell: what a slot routed to that cell sets, once a
    // sub-block. Envelope times act on the stage that is running (the chain's stages are copies).
    static void applyVoiceParam(WaveSeqVoice& voice, ParamId id, float value)
    {
        if (VoiceBodySection::ownsId(id, BodyBase))
        {
            VoiceBodySection::applyTo(voice.getBody(), id - BodyBase, value);
            return;
        }
        if (id >= FilterBase)
        {
            VoiceFilterSection::applyTo(voice.getFilter(), id - FilterBase, value);
            return;
        }
        if (id >= StepPitch && id < StepTime + 16)
        {
            auto& s = voice.getStep((id - StepPitch) % 16);
            switch ((id - StepPitch) / 16)
            {
                case 0: s.pitchOffset = value; break;
                case 2: s.level = juce::jlimit(0.0f, 1.0f, value); break;
                case 3: s.duration = juce::jmax(0.005f, value); break;
                default: break;
            }
            return;
        }
        switch (id)
        {
            case Level:   voice.setMasterLevel(value); return;
            case VelSens: voice.setVelocitySensitivity(value); return;
            case AmpAttack: case AmpDecay: case AmpSustain: case AmpRelease: break;
            default: return;
        }
        auto setEnvelope = [id, value](Envelope& e)
        {
            if (id == AmpAttack)       e.setAttack(value);
            else if (id == AmpDecay)   e.setDecay(value);
            else if (id == AmpSustain) e.setSustain(value);
            else                       e.setRelease(value);
        };
        setEnvelope(voice.getAmpEnvelope());
        auto& chain = voice.getEnvelopeChain();
        for (int s = 0; s < chain.getStageCount(); ++s)
            setEnvelope(chain.getEnvelopeMut(s));
    }

    void applyParameter(ParamId id, float value)
    {
        if (id >= StepPitch && id < StepTime + 16)
        {
            int lane = (id - StepPitch) / 16;
            int step = (id - StepPitch) % 16;
            for (auto& v : voices)
            {
                auto& s = v.getStep(step);
                switch (lane)
                {
                    case 0: s.pitchOffset = value; break;
                    case 1: s.shape = static_cast<WaveSeqVoice::Waveform>(juce::jlimit(0, 5, (int) std::lround(value))); break;
                    case 2: s.level = juce::jlimit(0.0f, 1.0f, value); break;
                    default: s.duration = juce::jmax(0.005f, value); break;
                }
            }
            return;
        }

        switch (id)
        {
            case AmpAttack: case AmpDecay: case AmpSustain: case AmpRelease:
                applyAmpEnvelope();
                break;
            case VelSens:
                for (auto& v : voices) v.setVelocitySensitivity(value);
                break;
            case TuneParam:      tune = value; break;
            case FineParam:      fine = value; break;
            case BendRangeParam: bendRange = value; break;
            case TempoSync:      syncOn = value > 0.5f; updateTempoScale(); break;
            case Rate:
                for (auto& v : voices) v.setSpeedMultiplier(1.0f / juce::jmax(0.05f, value));
                break;

            case SeqLength:
                for (auto& v : voices)
                    v.setSequenceLength((int) value);
                break;

            case Crossfade:
                for (auto& v : voices)
                    v.setCrossfadeAmount(value);
                break;

            case LoopModeParam:
            {
                int modeIdx = juce::jlimit(0, 5, (int) value);
                auto mode = static_cast<WaveSeqVoice::LoopMode>(modeIdx);
                for (auto& v : voices)
                    v.setLoopMode(mode);
                break;
            }

            case Speed:
                for (auto& v : voices)
                    v.setSpeedMultiplier(value);
                break;

            case PitchRange:
                for (auto& v : voices)
                    v.setPitchRange(value);
                break;

            case ShapeMix:
            {
                // Global waveform override for all steps
                int shapeIdx = juce::jlimit(0, 5, (int) value);
                auto shape = static_cast<WaveSeqVoice::Waveform>(shapeIdx);
                for (auto& v : voices)
                    for (int s = 0; s < WaveSeqVoice::MAX_STEPS; ++s)
                        v.getStep(s).shape = shape;
                // Keep the per-step values in step with what is sounding.
                for (int s = 0; s < 16; ++s)
                    params[StepShape + s] = (float) shapeIdx;
                break;
            }

            case Level:
                for (auto& v : voices)
                    v.setMasterLevel(value);
                break;

            case GateLength:
            {
                for (auto& v : voices)
                    v.setGateLength(value);

                // Map gate length to envelope sustain
                Envelope::Params ep;
                ep.attack = 0.005f;
                ep.decay = 0.1f;
                ep.sustain = value;
                ep.release = 0.05f + (1.0f - value) * 0.3f;
                stageCurves.applyTo(ep, 0);
                for (auto& v : voices)
                    v.getAmpEnvelope().setParams(ep);
                break;
            }

            case TimingLaneLen:
                for (auto& v : voices)
                    v.setLaneLength(WaveSeqVoice::TimingLane, (int) value);
                break;

            case PitchLaneLen:
                for (auto& v : voices)
                    v.setLaneLength(WaveSeqVoice::PitchLane, (int) value);
                break;

            case ShapeLaneLen:
                for (auto& v : voices)
                    v.setLaneLength(WaveSeqVoice::ShapeLane, (int) value);
                break;

            case LevelLaneLen:
                for (auto& v : voices)
                    v.setLaneLength(WaveSeqVoice::LevelLane, (int) value);
                break;

            case LaneInterp:
                for (auto& v : voices)
                    v.setLaneInterpolation(value > 0.5f);
                break;

            case EnvChainMode:
            {
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
                        auto& chain = v.getEnvelopeChain();
                        chain.setMode(modeVal == 1
                            ? EnvelopeChain::ChainMode::Series
                            : EnvelopeChain::ChainMode::Parallel);
                        // A chain with no stages would be ignored: give it the amp envelope twice.
                        while (chain.getStageCount() < 2)
                            chain.addEnvelope(v.getAmpEnvelope().getParams());
                    }
                }
                break;
            }

            case EnvChainStages:
            {
                int numStages = juce::jlimit(0, EnvelopeChain::MaxEnvelopes, (int) value);
                for (auto& v : voices)
                {
                    auto& chain = v.getEnvelopeChain();
                    // Rebuild chain with the requested number of stages
                    chain.reset();
                    while (chain.getStageCount() > numStages)
                        chain.removeLastEnvelope();
                    // Re-add stages (chain retains existing ones; we just note the count)
                    // The chain's addEnvelope is a setup-time operation, not audio-thread
                    while (chain.getStageCount() < numStages)
                    {
                        Envelope::Params ep;
                        ep.attack = 0.01f;
                        ep.decay = 0.1f;
                        ep.sustain = 0.7f;
                        ep.release = 0.2f;
                        chain.addEnvelope(ep);
                    }
                }
                break;
            }

            case EnvChainCombine:
            {
                auto cm = (value > 0.5f)
                    ? EnvelopeChain::CombineMode::Max
                    : EnvelopeChain::CombineMode::Sum;
                for (auto& v : voices)
                    v.getEnvelopeChain().setCombineMode(cm);
                break;
            }

            default:
                break;
        }
    }

    double sampleRate = 44100.0;
    float tune = 0.0f, fine = 0.0f, bend = 0.0f, bendRange = 2.0f, wheel = 0.0f;
    float wheelNow = 0.0f;                                  // the strip, smoothed per block

    VoiceFilterSection filterSection { FilterBase };
    VoiceBodySection bodySection { BodyBase };
    mod::Router router { lfoDests(), 2, 0, &sources() };
    std::array<float, mod::Router::MAX_CHUNK> mixL {}, mixR {};
    float tempoBpm = 120.0f;
    bool syncOn = false;
    VoiceAllocator allocator;
    std::vector<WaveSeqVoice> voices;
    ParamMap params;
};
