#pragma once

#include "EngineBase.h"
#include "VoiceAllocator.h"
#include "VoiceFilterSection.h"
#include "EngineLfoSection.h"
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
// Oscillators use polyBLEP antialiasing (same pattern as UnifiedVoice).
// All state is pre-allocated — no heap allocation on audio thread.
// ============================================================================
class WaveSeqVoice
{
public:
    static constexpr int MAX_STEPS = 16;
    static constexpr int NUM_LANES = 4;

    enum class Waveform { Sine, Saw, Square, Triangle, Pulse, Noise };

    enum class LoopMode { Forward, Reverse, PingPong, Random, OneShot };

    // Lane indices
    enum Lane { TimingLane = 0, PitchLane = 1, ShapeLane = 2, LevelLane = 3 };

    // LFO destinations (EngineLfoSection), in the order of the DEST cell.
    enum LfoDest
    {
        LfoOff = 0,
        LfoPitch,       // +-12 semitones, depth squared
        LfoAmp,         // gain 0..2
        LfoPan,         // +-1
        LfoCutoff,      // +-4 octaves
        LfoResonance,   // +-0.5
        LfoRate,        // how fast the sequence runs, +-2 octaves around RATE
        LfoCrossfade,   // +-1 around XFAD (0..1)
        LfoPitchRange,  // +-1 around PRNG (0..2): how far the pitch lane reaches
        NumLfoDests
    };

    static const char* const* lfoDestLabels()
    {
        static const char* const names[NumLfoDests] = {
            "OFF", "PTCH", "AMP", "PAN", "CUT", "RESO", "RATE", "XFAD", "PRNG" };
        return names;
    }

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
        lfos.setSampleRate(sr);
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

    void noteOn()
    {
        if (active)
        {
            declick.arm();      // struck again while sounding: the restart must not jump,
            gainDeclick.arm();  // nor the step to the new velocity
        }
        else
            lfos.snapSlews();
        // An LFO on a page cell is read before the first sample (the engine sets the cell then).
        if (lfos.keyed()) lfos.noteOnAndSample(); else lfos.noteOn();
        filter.noteOn(currentNote, velocity, active);
        if (envChainEnabled && envChain.getStageCount() > 0)
            envChain.noteOn();
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
        oscPhase = 0.0;
        prevOscPhase = 0.0;
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

    bool isActive() const { return active; }

    float process()
    {
        if (!active)
            return 0.0f;

        if (! lfos.anyOn() && ! lfoApplied)
            return processCore();

        // LFOs: rate, crossfade and pitch range are moved for this sample only (the set values stay).
        if (! lfos.anyOn())
        {
            lfoSemis = 0.0f;
            filter.setLfoOctaves(0.0f);
            filter.setResonanceMod(0.0f);
            lfoGain = 1.0f;
            lfoPan = 0.0f;
            lfoApplied = false;
            lfos.snapSlews();
            return processCore();
        }

        lfos.tick();
        lfoApplied = true;
        const float setSpeed = speedMultiplier, setCrossfade = crossfadeAmount, setRange = pitchRange;
        const float rate = lfos.amount(LfoRate);
        if (rate != 0.0f)
            speedMultiplier = std::max(0.01f, speedMultiplier * std::exp2(-2.0f * rate));
        // Only what is routed is limited to its cell's range: a stored sound may hold a value
        // beyond it (factory presets have PRNG up to 12), and a route elsewhere must leave it alone.
        if (lfos.routed(LfoCrossfade))
            crossfadeAmount = juce::jlimit(0.0f, 1.0f, crossfadeAmount + lfos.amount(LfoCrossfade));
        if (lfos.routed(LfoPitchRange))
            pitchRange = juce::jlimit(0.0f, 2.0f, pitchRange + lfos.amount(LfoPitchRange));
        lfoSemis = lfos.pitchSemitones(LfoPitch);
        filter.setLfoOctaves(lfos.amount(LfoCutoff) * VoiceLfos::CUTOFF_OCTAVES);
        filter.setResonanceMod(lfos.amount(LfoResonance) * VoiceLfos::RESONANCE_RANGE);
        lfoGain = lfos.gain(LfoAmp);
        lfoPan = juce::jlimit(-1.0f, 1.0f, lfos.panAmount(LfoPan));

        const float out = processCore();

        speedMultiplier = setSpeed;
        crossfadeAmount = setCrossfade;
        pitchRange = setRange;
        return out;
    }

    VoiceLfos& getLfos() { return lfos; }
    // -1..1 from LFOs routed to the pan (as of the last processed sample); the engine places the voice.
    float getLfoPan() const { return lfoPan; }

private:
    float processCore()
    {
        float ampVal;
        if (envChainEnabled && envChain.getStageCount() > 0)
        {
            ampVal = envChain.process();
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

        double freq = baseFrequency * std::exp2((pitchSemitones + pitchOffsetSemis + lfoSemis) / 12.0);
        double phaseInc = freq / sampleRate;

        // Generate current step waveform
        float currentSample = generateWaveform(shape, oscPhase, phaseInc);

        // Crossfade with previous step if active
        float output;
        if (crossfadeAmount > 0.001f && crossfadePhase < 1.0f
            && prevTimingStep != timingIdx)
        {
            // Use previous step's shape and pitch for crossfade source
            float prevPitch = steps[(size_t) prevTimingStep].pitchOffset * pitchRange;
            double prevFreq = baseFrequency * std::exp2((prevPitch + pitchOffsetSemis + lfoSemis) / 12.0);
            double prevPhaseInc = prevFreq / sampleRate;
            Waveform prevShape = steps[(size_t) prevTimingStep].shape;

            float prevSample = generateWaveform(prevShape, prevOscPhase, prevPhaseInc);

            // Advance previous oscillator phase
            prevOscPhase += prevPhaseInc;
            prevOscPhase = dsp::wrapPhase(prevOscPhase);

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
        oscPhase += phaseInc;
        oscPhase = dsp::wrapPhase(oscPhase);

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

        // Velocity's share, LEVL and pressure's share of the level are moved to over a few
        // milliseconds under a sounding note; the first sample of a note takes them as they stand.
        const float velGain = 1.0f - velSens + velSens * velocity;
        if (struck)
        {
            struck = false;
            velGainNow = velGain;
            masterLevelNow = masterLevel;
            gainModNow = gainMod;
        }
        else
        {
            if (velGainNow != velGain)         velGainNow = dsp::towards(velGainNow, velGain, levelStep);
            if (masterLevelNow != masterLevel) masterLevelNow = dsp::towards(masterLevelNow, masterLevel, levelStep);
            if (gainModNow != gainMod)         gainModNow = dsp::towards(gainModNow, gainMod, 2.0f * levelStep);
        }
        return gainDeclick.process(filter.process(declick.process(output)) * ampVal * velGainNow * masterLevelNow * gainModNow * lfoGain);
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
    void setGainMod(float g) { gainMod = g; }                                // pressure
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
        active = false;
        oscPhase = 0.0;
        prevOscPhase = 0.0;
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
                rngState ^= rngState << 13;
                rngState ^= rngState >> 17;
                rngState ^= rngState << 5;
                return (float) (rngState & 0x7FFFFFFF) / (float) 0x7FFFFFFF * 2.0f - 1.0f;
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
                // LCG — no heap, deterministic per-voice
                rngState = rngState * 1664525u + 1013904223u;
                lane.currentStep = (int) (rngState % (uint32_t) lane.length);
                break;
            }

            case LoopMode::OneShot:
                if (lane.currentStep < lane.length - 1)
                    ++lane.currentStep;
                else
                    lane.finished = true;
                break;
        }
    }

    // Advance all lanes when the timing lane triggers a step change
    void advanceAllLanes()
    {
        if (sequenceFinished)
            return;

        prevTimingStep = lanes[TimingLane].currentStep;
        prevOscPhase = oscPhase;
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

    // Oscillator state (current and previous for crossfade)
    double oscPhase = 0.0;
    double prevOscPhase = 0.0;

    // Parameters
    float speedMultiplier = 1.0f;
    float pitchRange = 1.0f;
    float masterLevel = 0.8f;
    float gateLength = 1.0f;
    float pitchOffsetSemis = 0.0f;
    float velSens = 1.0f;
    float gainMod = 1.0f;
    float velGainNow = 1.0f, masterLevelNow = 0.8f, gainModNow = 1.0f, stepLevelNow = 1.0f, levelStep = 0.005f;
    bool struck = true, shapeSwitched = false;
    float tempoScale = 1.0f;
    LoopMode loopMode = LoopMode::Forward;

    // DSP
    Envelope ampEnv;
    dsp::Declick declick;       // on the oscillator, before the filter (whose state smears a jump)
    dsp::Declick gainDeclick;   // on the output
    VoiceLfos lfos;
    bool lfoApplied = false;
    float lfoSemis = 0.0f;
    float lfoGain = 1.0f;
    float lfoPan = 0.0f;
    VoiceFilter filter;          // after the wave mix, before the amp envelope
    EnvelopeChain envChain;
    bool envChainEnabled = false;
    mutable uint32_t rngState = 12345u; // mutable for const generateWaveform
};


// ============================================================================
// WaveSeqEngine — wave sequencing engine using VoiceAllocator
//
// Parameters:
//   0  = Seq Length       (1-16 steps, sets all lanes)
//   1  = Crossfade        (0-100%)
//   2  = Loop Mode        (0=Forward, 1=Reverse, 2=PingPong, 3=Random, 4=OneShot)
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
        // Per-step data, + step index 0..15
        StepPitch      = 32,   // semitones (scaled by PitchRange)
        StepShape      = 48,   // waveform
        StepLevel      = 64,
        StepTime       = 80,   // seconds
        FilterBase     = 100   // 100-115: the per-voice filter (VoiceFilterSection offsets)
    };

    explicit WaveSeqEngine(int maxVoices = 8)
        : allocator(maxVoices)
    {
        voices.resize((size_t) maxVoices);

        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            auto& voice = voices[(size_t) voiceIdx];
            voice.setNote(v.note, v.velocity);
            voice.noteOn();
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            voices[(size_t) voiceIdx].noteOff();
        });

        // A new note takes a silent voice while there is one: release tails ring out.
        allocator.setVoiceSoundingQuery([this](int voiceIdx) { return voices[(size_t) voiceIdx].isActive(); });

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
                    if (p.id >= StepPitch && p.id < StepPitch + 16) v = defPitch[p.id - StepPitch];
                    else if (p.id >= StepShape && p.id < StepShape + 16) v = defShape[p.id - StepShape];
                    else if (p.id >= StepLevel && p.id < StepLevel + 16) v = defLevel[p.id - StepLevel];
                    setParameter(p.id, v);
                }
        filterSection.applyAll(VoiceFilterSection::each(voices));
        lfoSection.applyAll(EngineLfoSection::each(voices));
    }

    // SEQ and LANES (how the sequence runs), then two pages per lane with one cell per step
    // (encoder N = step N, or step N + 8 on the second page), then AMP.
    static const ParamPages& pages()
    {
        static const ParamPages list = build();
        return list;
    }

    // The LFO destinations: the original eight, then level, the amp envelope, the filter and the
    // other LFO, then the lanes step by step (P1-P16 pitch, L1-L16 level, T1-T16 time: each voice
    // has its own copy of the sequence, so an LFO bends one voice's step).
    // Not offered: LEN, LOOP, the lane lengths, SYNC, SLID, SHP*, the shape lane, CHAN, STGS,
    // COMB (choices and counts), TUNE, FINE and BEND (PTCH is the pitch), and what the filter
    // leaves out.
    static const LfoDestList& lfoDests(const ParamPages* enginePages = nullptr)
    {
        if (enginePages == nullptr)
            pages();                    // the list is made on the way (build())
        static const LfoDestList list = makeLfoDests(*enginePages);
        static const bool registered = (list.registerGroups(), true);
        juce::ignoreUnused(registered);
        return list;
    }

    static LfoDestList makeLfoDests(const ParamPages& p)
    {
        LfoDestList l(WaveSeqVoice::lfoDestLabels(), WaveSeqVoice::NumLfoDests);
        l.group();
        for (int id : { (int) Level, (int) AmpAttack, (int) AmpDecay, (int) AmpSustain, (int) AmpRelease, (int) VelSens })
            l.add(p, id);
        VoiceFilterSection::addLfoDests(l, p, FilterBase);
        l.addCross();
        for (int base : { (int) StepPitch, (int) StepLevel, (int) StepTime })
            for (int half = 0; half < 2; ++half)
            {
                l.group();
                for (int i = 0; i < 8; ++i)
                    l.add(p, base + half * 8 + i);
            }
        // The mod strip until the STRIP page says otherwise: the sequence up to twice as fast
        // (RATE +50 = one octave of speed, so the top of the strip is in time again).
        l.stripRoute(0, WaveSeqVoice::LfoRate, 0.5f);
        return l;
    }

    ParamPages getParamPages() const override
    {
        ParamPages out = pages();
        filterSection.updateModeCell(out);
        return out;
    }

    void pitchBend(float amount) override { bend = juce::jlimit(-1.0f, 1.0f, amount); }
    void modWheel(float value) override { wheel = juce::jlimit(0.0f, 1.0f, value); }
    void setTempo(float bpm) override
    {
        tempoBpm = bpm;
        updateTempoScale();
        lfoSection.setTempo(bpm, EngineLfoSection::each(voices));
    }

    EngineType getType() const override { return EngineType::WaveSeq; }
    juce::String getName() const override { return "Wave Sequencer"; }

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

        // Once per block: tuning and bend, and each voice's pressure (volume). The mod strip: the
        // routes of the STRIP page (by default the sequence runs up to twice as fast), and the
        // cutoff by the filter's WHL amount.
        wheelNow = smoothWheel(wheelNow, wheel, numSamples, sampleRate);
        lfoSection.stripBlock(wheelNow, numSamples, sampleRate, EngineLfoSection::each(voices),
                              [this](int key) { setParameter(key, getParameter(key)); });
        for (int v = 0; v < (int) voices.size(); ++v)
        {
            float pressure = v < allocator.getVoiceCount() ? allocator.getAftertouch(v) : 0.0f;
            AftertouchRouter::ModValues m;
            if (pressure > 0.0f)
                m = (aftertouchRouter ? *aftertouchRouter : allocator.getAftertouchRouter()).computeModulation(pressure);
            voices[(size_t) v].setGainMod(juce::jlimit(0.0f, 2.0f, 1.0f + m.volumeMod));
            voices[(size_t) v].setPitchOffset(tune + fine / 100.0f + bend * bendRange);
            voices[(size_t) v].getFilter().setExternalOctaves(filterSection.externalOctaves(wheelNow, m.cutoffMod));
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

            sampleL *= 0.7f;
            sampleR *= 0.7f;

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
        lfoSection.afterLetGo([this](int key) { setParameter(key, getParameter(key)); });
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
        if (filterSection.owns(id))
        {
            if (filterSection.swaps(id, value))
                soundSwapped();
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
            stageCurves.set(id, value);
            if (StageCurveParam::envelopeOf(id) == 0)
                applyAmpEnvelope();
            return;
        }
        if ((id == EnvChainMode || id == EnvChainStages || id == EnvChainCombine) && params[id] != value)
            soundSwapped();
        params[id] = value;
        applyParameter(id, value);
        lfoSection.baseChanged(id, value, EngineLfoSection::each(voices));
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
        static const char* const loops[] = { "FWD", "REV", "PING", "RAND", "ONCE" };
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
                { "LOOP", LoopModeParam, 0, 4, 0, C, "", loops, 5 },
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
        VoiceFilterSection::appendPages(out, FilterBase);
        EngineLfoSection(lfoDests(&out)).appendPages(out);
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

    // One voice's own value of a page cell: what an LFO routed to that cell sets, sample by
    // sample. Envelope times act on the stage that is running (the chain's stages are copies).
    static void applyVoiceParam(WaveSeqVoice& voice, ParamId id, float value)
    {
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
                int modeIdx = juce::jlimit(0, 4, (int) value);
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
    EngineLfoSection lfoSection { lfoDests() };
    float tempoBpm = 120.0f;
    bool syncOn = false;
    VoiceAllocator allocator;
    std::vector<WaveSeqVoice> voices;
    ParamMap params;
};
