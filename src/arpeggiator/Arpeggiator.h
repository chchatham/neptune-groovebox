#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "VariationMacros.h"
#include "MutateSystem.h"
#include "RhythmCapture.h"

// -----------------------------------------------------------------------------
// Arpeggiator: turns the held notes into a stream of note-on / note-off calls.
//
// Every step is planned once (planStep): the notes of the step, then a short
// list of "hits" (the note itself, ratchet repeats, inserted notes) with their
// start, end and level as fractions of the step. processBlock only walks that
// list. Everything that shapes a step goes through the plan:
//   mode / octaves / repeats     which sequence position plays
//   mutate                       per-position pitch / level / mute offsets
//   variation macros             skip, octave and interval jumps, ratchets,
//                                inserted notes, gate spread, humanize
//   parameter locks              combined with the macros (MacroInteraction)
//   strum / tapped rhythm        per-note delays inside a chord step
//   pressure                     gate, rate or octave range
//   snap glide                   rate changes glide over a musical division
//   freeze                       the step stream repeats exactly
//   scale                        pitches made by the macros and by mutate snap to it
//   transport                    syncToTransport() puts the step lines on the beat grid
//
// Audio thread: no allocation, no locks; all storage is fixed-size.
// -----------------------------------------------------------------------------
class Arpeggiator
{
public:
    enum class Mode
    {
        Up = 0,
        Down,
        UpDown,
        Random,
        AsPlayed,        // The held keys in the order they were first pressed (one entry per key)
        ChordRepeat,
        Walk,
        PlayOrder,       // Every key press in order, a key pressed twice plays twice, each press at its own velocity
        Converge,
        Diverge,
        PolyArp,         // All held notes simultaneously with velocity accent pattern
        RandomOctave,     // Random note selection + random octave shift +-1-2
        NumModes
    };

    enum class Rate
    {
        Whole = 0,      // 1/1
        Half,           // 1/2
        Quarter,        // 1/4
        Eighth,         // 1/8
        Sixteenth,      // 1/16
        ThirtySecond,   // 1/32
        QuarterT,       // 1/4T
        EighthT,        // 1/8T
        SixteenthT,     // 1/16T
        ThirtySecondT,  // 1/32T    (new rates are appended: projects store the number)
        SixtyFourth,    // 1/64
        SixtyFourthT,   // 1/64T
        NumRates
    };

    // The rates from slowest to fastest, which is the order the RATE knob goes through them
    // (the enum itself is in the order projects store).
    static Rate rateBySpeed(int position)
    {
        static const Rate order[] = { Rate::Whole, Rate::Half, Rate::Quarter, Rate::QuarterT, Rate::Eighth, Rate::EighthT,
                                      Rate::Sixteenth, Rate::SixteenthT, Rate::ThirtySecond, Rate::ThirtySecondT,
                                      Rate::SixtyFourth, Rate::SixtyFourthT };
        return order[juce::jlimit(0, (int) Rate::NumRates - 1, position)];
    }
    static int speedPosition(Rate r)
    {
        for (int i = 0; i < (int) Rate::NumRates; ++i)
            if (rateBySpeed(i) == r) return i;
        return 0;
    }

    // Pressure routing targets for pad aftertouch
    enum class PressureTarget
    {
        Rate = 0,
        OctaveRange,
        GateLength,
        NumTargets
    };

    // Parameter lock + macro interaction modes (per-track configurable)
    enum class MacroInteraction
    {
        Additive = 0,    // lock offset + macro offset (default)
        MacroWins,        // macro value overrides lock
        LockWins,         // lock value overrides macro
        Multiply,         // lock * macro
        NumModes
    };

    // Arp recording output mode
    enum class RecordMode
    {
        Individual = 0,   // Record each individual arp note
        Chord,            // Record held chord at arp timing
        NumModes
    };

    // Arpeggiator values a sequencer step can lock.
    enum class LockParam
    {
        Gate = 0,         // 0..1
        Velocity,         // 0..1
        Ratchet,          // 1..4
        NumParams
    };

    struct StrumNote
    {
        int pitch = 0;
        float velocity = 0.0f;
        double delaySamples = 0.0;
    };

    // Captured note from arp recording
    struct RecordedNote
    {
        int pitch = 0;
        float velocity = 0.0f;
        double sampleTimestamp = 0.0;
    };

    // One step of exportSteps(): plain data for a sequencer pattern.
    struct ExportedStep
    {
        static constexpr int MAX_NOTES = 16;
        int pitches[MAX_NOTES] = {};
        float velocities[MAX_NOTES] = {};   // per note, 0..1
        int noteCount = 0;
        float velocity = 0.0f;              // the step's level: its loudest note
        float gate = 0.0f;                  // sounding length as a fraction of one step at the arp's rate
        int hits = 0;                       // triggers inside the step (ratchets and inserted notes; 1 = a plain note)
        float startSteps = 0.0f;            // where the step starts, in steps from the start of the cycle
        float lengthSteps = 1.0f;           // 1 unless a tapped rhythm sets the step timing
        bool rest = true;
    };

    static constexpr int MAX_RHYTHM_EVENTS = 16;
    static constexpr int MAX_VARIATION = 99;

    // Scale masks for setScale(): bit n set = the pitch class n semitones above the root is allowed.
    static constexpr uint16_t SCALE_CHROMATIC        = 0x0FFF;
    static constexpr uint16_t SCALE_MAJOR            = 0x0AB5;   // 0 2 4 5 7 9 11
    static constexpr uint16_t SCALE_MINOR            = 0x05AD;   // 0 2 3 5 7 8 10
    static constexpr uint16_t SCALE_PENTATONIC_MINOR = 0x04A9;   // 0 3 5 7 10
    static constexpr uint16_t SCALE_PENTATONIC_MAJOR = 0x0295;   // 0 2 4 7 9

    Arpeggiator()
    {
        xorshiftState = 12345u;
        mutateSeedState = 0x9E3779B9u;
        setVariation(1);
    }

    // --- Configuration ---

    void setMode(Mode m) { mode = m; }
    Mode getMode() const { return mode; }

    void setRate(Rate r) { rate = r; }
    Rate getRate() const { return rate; }

    void setOctaveRange(int oct) { octaveRange = juce::jlimit(1, 8, oct); }
    int getOctaveRange() const { return octaveRange; }

    void setGate(float g) { gate = juce::jlimit(0.0f, 1.0f, g); }
    float getGate() const { return gate; }

    void setSwing(float s) { swing = juce::jlimit(0.0f, 1.0f, s); }
    float getSwing() const { return swing; }

    // Latch: released keys keep playing; the first key pressed after all keys
    // were released starts a new chord. Switching latch off lets go of every
    // note whose key is no longer down.
    void setLatch(bool l)
    {
        const bool wasLatched = latch;
        latch = l;
        if (wasLatched && !l && !frozen)
            releaseUnheldNotes();
    }
    bool getLatch() const { return latch; }

    void setEnabled(bool e) { enabled = e; if (!e) reset(); }
    bool isEnabled() const { return enabled; }

    // Seeds every random choice (tests; a fixed seed gives a fixed note stream).
    void setRandomSeed(uint32_t seed)
    {
        xorshiftState = (seed != 0u) ? seed : 1u;
        mutateSeedState = hash3(seed, 0x51ED270Bu, 7u) | 1u;
    }

    // --- Strum ---

    void setStrumEnabled(bool e) { strumEnabled = e; }
    bool isStrumEnabled() const { return strumEnabled; }

    void setStrumTime(float ms) { strumTime = juce::jlimit(0.0f, 100.0f, ms); }
    float getStrumTime() const { return strumTime; }

    // 0 = up, 1 = down, 2 = alternating (up, down, up, ...)
    void setStrumDirection(int dir) { strumDirection = juce::jlimit(0, 2, dir); }
    int getStrumDirection() const { return strumDirection; }

    void setStrumVelCurve(float c) { strumVelCurve = juce::jlimit(0.0f, 1.0f, c); }
    float getStrumVelCurve() const { return strumVelCurve; }

    // --- Tapped rhythm ---
    // Chord modes with strum on: the timing and level of each note in the strum
    // (tap times become positions inside the strum time).
    // Everything else (a normal arpeggio, block chords): the timing and accent
    // of the steps. N taps make a cycle of N steps that lasts N steps at the
    // current rate; inside the cycle the steps start where the taps were (the
    // captured span is stretched over N-1 steps, the last tap gets one step),
    // and each tap's level scales its step. Evenly spaced taps give the plain
    // arpeggio. Swing is not applied while a tapped rhythm sets the step timing.

    // Takes a finished capture (two taps or more) as the rhythm.
    bool setRhythmTemplate(const RhythmCapture& capture)
    {
        const auto& events = capture.getEvents();
        const int count = juce::jmin((int) events.size(), (int) MAX_RHYTHM_EVENTS);
        if (count < 2)
            return false;

        const double first = events[0].timeSamples;
        const double span = events[(size_t) (count - 1)].timeSamples - first;
        float maxVel = 0.0f;
        for (int i = 0; i < count; ++i)
            maxVel = juce::jmax(maxVel, events[(size_t) i].velocity);

        rhythmLength = 0;
        for (int i = 0; i < count; ++i)
        {
            rhythmTime[i] = (span > 0.0) ? (events[(size_t) i].timeSamples - first) / span
                                         : (double) i / (double) (count - 1);
            rhythmVelocity[i] = (maxVel > 0.0f) ? juce::jmax(0.05f, events[(size_t) i].velocity / maxVel) : 1.0f;
        }

        // Step lengths for the step rhythm, in steps; they add up to `count`.
        // Taps closer than an eighth of a step are kept that far apart.
        double total = 0.0;
        for (int i = 0; i < count; ++i)
        {
            rhythmStepScale[i] = (i < count - 1)
                                     ? juce::jmax(0.125, (rhythmTime[i + 1] - rhythmTime[i]) * (double) (count - 1))
                                     : 1.0;
            total += rhythmStepScale[i];
        }
        for (int i = 0; i < count; ++i)
            rhythmStepScale[i] *= (double) count / total;

        rhythmLength = count;
        return true;
    }

    // True when the tapped rhythm sets the step timing (not the strum timing).
    bool isStepRhythmActive() const { return stepRhythmLength() > 0; }

    // Length of step `idx` of the tapped cycle, in steps at the current rate.
    double getRhythmStepLength(int idx) const { return (idx >= 0 && idx < rhythmLength) ? rhythmStepScale[idx] : 1.0; }
    float getRhythmLevel(int idx) const { return (idx >= 0 && idx < rhythmLength) ? rhythmVelocity[idx] : 1.0f; }

    void clearRhythmTemplate() { rhythmLength = 0; rhythmEnabled = false; }
    int getRhythmLength() const { return rhythmLength; }
    double getRhythmTime(int idx) const { return (idx >= 0 && idx < rhythmLength) ? rhythmTime[idx] : 0.0; }

    // The tapped rhythm as plain numbers (for saving): enabled, count, then time / level / step
    // length per tap.
    std::vector<float> getRhythmState() const
    {
        std::vector<float> v { rhythmEnabled ? 1.0f : 0.0f, (float) rhythmLength };
        for (int i = 0; i < rhythmLength; ++i)
        {
            v.push_back((float) rhythmTime[i]);
            v.push_back(rhythmVelocity[i]);
            v.push_back((float) rhythmStepScale[i]);
        }
        return v;
    }
    void setRhythmState(const float* v, int count)
    {
        rhythmLength = 0;
        rhythmEnabled = false;
        if (v == nullptr || count < 2) return;
        const int n = juce::jlimit(0, (int) MAX_RHYTHM_EVENTS, (int) v[1]);
        if (n < 2 || count < 2 + n * 3) return;
        for (int i = 0; i < n; ++i)
        {
            rhythmTime[i] = juce::jlimit(0.0, 1.0, (double) v[2 + i * 3]);
            rhythmVelocity[i] = juce::jlimit(0.05f, 1.0f, v[3 + i * 3]);
            rhythmStepScale[i] = juce::jlimit(0.125, (double) MAX_RHYTHM_EVENTS, (double) v[4 + i * 3]);
        }
        rhythmLength = n;
        rhythmEnabled = v[0] > 0.5f;
    }

    // --- Mod strip on a variation macro ---
    // While this arpeggiator is on, the mod strip can drive one macro instead of the mod wheel.
    enum class StripTarget { Off, Spice, Dice, Density, OctaveScatter, RatchetChance, GateSpread, Gate, NumTargets };
    void setStripTarget(StripTarget t) { stripTarget = t; }
    StripTarget getStripTarget() const { return stripTarget; }
    // Strip position 0..1. Returns false when no macro is assigned.
    bool applyStrip(float v)
    {
        v = juce::jlimit(0.0f, 1.0f, v);
        switch (stripTarget)
        {
            case StripTarget::Spice:         setSpice(v); return true;
            case StripTarget::Dice:          setDice(v); return true;
            case StripTarget::Density:       setDensity(v); return true;
            case StripTarget::OctaveScatter: setOctaveScatter(v); return true;
            case StripTarget::RatchetChance: setRatchetChance(v); return true;
            case StripTarget::GateSpread:    setGateSpread(v); return true;
            case StripTarget::Gate:          setGate(v); return true;
            default: return false;
        }
    }

    void setRhythmEnabled(bool e) { rhythmEnabled = e && rhythmLength >= 2; }
    bool isRhythmEnabled() const { return rhythmEnabled && rhythmLength >= 2; }

    // --- Ratchet ---

    void setRatchetEnabled(bool e) { ratchetEnabled = e; }
    bool isRatchetEnabled() const { return ratchetEnabled; }

    void setRatchetCount(int c) { ratchetCount = juce::jlimit(1, 4, c); }
    int getRatchetCount() const { return ratchetCount; }

    void setRatchetDecay(float d) { ratchetDecay = juce::jlimit(0.0f, 1.0f, d); }
    float getRatchetDecay() const { return ratchetDecay; }

    // --- Repeats per note ---

    void setNoteRepeats(int r) { noteRepeats = juce::jlimit(1, 8, r); }
    int getNoteRepeats() const { return noteRepeats; }

    // --- Snap glide: a rate change glides over a musical division ---

    void setGlideTime(float t) { glideTime = juce::jlimit(0.0f, 1.0f, t); }
    float getGlideTime() const { return glideTime; }

    int getGlideFromPitch() const { return glideFromPitch; }
    bool isGlideActive() const { return glideTime > 0.0f && glideFromPitch >= 0; }

    // Returns glide duration in samples, snapped to a musical division
    double getGlideTimeSamples(double sampleRate, double bpm) const
    {
        if (glideTime <= 0.0f) return 0.0;
        double samplesPerBeat = (60.0 / bpm) * sampleRate;
        double division;
        if      (glideTime <= 0.17f) division = 0.125;   // 1/32
        else if (glideTime <= 0.33f) division = 0.25;    // 1/16
        else if (glideTime <= 0.50f) division = 0.5;     // 1/8
        else if (glideTime <= 0.67f) division = 1.0;     // 1/4
        else if (glideTime <= 0.83f) division = 2.0;     // 1/2
        else                         division = 4.0;     // 1/1
        return samplesPerBeat * division;
    }

    // Returns the snap-quantized division name
    static juce::String getGlideDivisionName(float t)
    {
        if      (t <= 0.0f)  return "OFF";
        else if (t <= 0.17f) return "1/32";
        else if (t <= 0.33f) return "1/16";
        else if (t <= 0.50f) return "1/8";
        else if (t <= 0.67f) return "1/4";
        else if (t <= 0.83f) return "1/2";
        else                 return "1/1";
    }

    // The seven glide settings as an index (0 = off) and back.
    static constexpr int NUM_GLIDE_DIVISIONS = 7;
    static int getGlideDivisionIndex(float t)
    {
        if      (t <= 0.0f)  return 0;
        else if (t <= 0.17f) return 1;
        else if (t <= 0.33f) return 2;
        else if (t <= 0.50f) return 3;
        else if (t <= 0.67f) return 4;
        else if (t <= 0.83f) return 5;
        return 6;
    }
    static float getGlideTimeForIndex(int idx)
    {
        static constexpr float values[NUM_GLIDE_DIVISIONS] = { 0.0f, 0.1f, 0.25f, 0.4f, 0.6f, 0.75f, 0.9f };
        return values[juce::jlimit(0, NUM_GLIDE_DIVISIONS - 1, idx)];
    }

    // Step length in quarter notes that is playing right now (glides after a rate change).
    double getCurrentRateMultiplier() const { return rateCurrent > 0.0 ? rateCurrent : getRateMultiplier(); }

    // --- Humanize / Grid ---
    // One humanize for the whole arpeggiator: notes land a little late by a
    // random amount and their level varies. Off (or amount 0) = on the grid.

    void setHumanize(bool h) { humanizeEnabled = h; }
    bool isHumanized() const { return humanizeEnabled; }

    void setHumanizeAmount(float a) { humanizeAmount = juce::jlimit(0.0f, 1.0f, a); }
    float getHumanizeAmount() const { return humanizeAmount; }

    // --- Variation macros ---

    void setSpice(float v)          { macros.setSpice(v); }           // chance of an inserted note; mutation amount
    void setDice(float v)           { macros.setDice(v); }            // chance of an interval jump
    void setDensity(float v)        { macros.setDensity(v); }         // < 0.5 drops steps, > 0.5 adds ghost notes
    void setOctaveScatter(float v)  { macros.setOctaveScatter(v); }   // chance of an octave jump
    void setRatchetChance(float v)  { macros.setRatchetChance(v); }   // chance of a ratchet
    void setGateSpread(float v)     { macros.setGateSpread(v); }      // staccato / legato variation

    float getSpice() const          { return macros.getSpice(); }
    float getDice() const           { return macros.getDice(); }
    float getDensity() const        { return macros.getDensity(); }
    float getOctaveScatter() const  { return macros.getOctaveScatter(); }
    float getRatchetChance() const  { return macros.getRatchetChance(); }
    float getGateSpread() const     { return macros.getGateSpread(); }

    const VariationMacros& getVariationMacros() const { return macros; }

    // The stored variation: every number is a different roll of the dice.
    void setVariation(int n)
    {
        variation = juce::jlimit(1, (int) MAX_VARIATION, n);
        variationSeed = hash3((uint32_t) variation, 0x2545F491u, 3u) | 1u;
    }
    int getVariation() const { return variation; }

    // Re-roll: move on to the next variation.
    void rollDice() { setVariation(variation >= MAX_VARIATION ? 1 : variation + 1); }

    // --- Freeze ---
    // While frozen the held notes are locked (key releases and new keys do not
    // change the sequence), mutation stops, and the stream of steps, including
    // every random choice and macro variation, repeats exactly.

    void setFrozen(bool f)
    {
        if (f && !frozen)
        {
            // Capture current sequence as a snapshot
            frozenSequenceLength = sequenceLength;
            for (int i = 0; i < sequenceLength && i < MAX_SEQUENCE_NOTES; ++i)
                frozenSequence[i] = sequence[i];
            ++freezeGeneration;
        }
        const bool wasFrozen = frozen;
        frozen = f;
        // When unfreezing, let go of keys released meanwhile and rebuild from the input
        if (!f && wasFrozen)
        {
            if (!latch)
                releaseUnheldNotes();
            else if (inputNoteCount > 0)
                rebuildSequence();
        }
    }
    bool isFrozen() const { return frozen; }

    int getFrozenSequenceLength() const { return frozenSequenceLength; }
    int getFrozenSequencePitch(int idx) const
    {
        if (idx >= 0 && idx < frozenSequenceLength)
            return frozenSequence[idx].pitch;
        return -1;
    }
    float getFrozenSequenceVelocity(int idx) const
    {
        if (idx >= 0 && idx < frozenSequenceLength)
            return frozenSequence[idx].velocity;
        return 0.0f;
    }

    // Number of steps in the frozen loop (0 until the audio side has captured it).
    int getFreezeLoopLength() const { return freezeLoopLength; }

    // --- Mutate: gradual evolution of the played sequence ---
    // Every `interval` bars one round of mutations changes some positions of the
    // sequence (pitch, level, mute). The amount is Spice. Mode and interval are
    // set on getMutate(). The held notes themselves are never overwritten.

    MutateSystem& getMutate() { return mutate; }
    const MutateSystem& getMutate() const { return mutate; }

    void setMutateEnabled(bool e) { mutateEnabled = e; }
    bool isMutateEnabled() const { return mutateEnabled; }

    // The four actions are requests: the audio side carries them out, in the
    // order asked, at the next block or step, so they are safe to call from the UI.
    void mutateNow()          { pushAction(MutateAction::Mutate); }     // one round now
    void returnToAncestor()   { pushAction(MutateAction::Return); }     // drop the current mutation
    void commitToAncestor()   { pushAction(MutateAction::Commit); }     // current mutation becomes the ancestor
    void restoreOriginal()    { pushAction(MutateAction::Original); }   // ancestor and current = the held notes

    // The mutation as it stands (a restore that is still waiting counts as done), and putting one
    // back: like the four actions a request, carried out by the audio side. UI thread only.
    MutateSystem::NoteState captureMutation() const
    {
        MutateSystem::NoteState state;
        if (restoreWaiting.load()) state = restoreState;
        else                       mutate.getNoteState(state);
        return state;
    }
    void restoreMutation(const MutateSystem::NoteState& state)
    {
        restoreWaiting.store(false);        // the audio side leaves a half-written state alone
        restoreState = state;
        restoreWaiting.store(true);
        pushAction(MutateAction::Restore);
    }

    // Rounds on top of the ancestor (0 = the ancestor is playing). Requests
    // that are still waiting are counted in, so a display is right at once.
    int getMutationGeneration() const
    {
        int generation; bool committed;
        mutateStateWithPending(generation, committed);
        return generation;
    }

    // True when the ancestor is a committed mutation, false when it is the held notes.
    bool isMutationCommitted() const
    {
        int generation; bool committed;
        mutateStateWithPending(generation, committed);
        return committed;
    }

    // --- Arp Recording ---
    // Individual: every note the arpeggiator emits. Chord: the held chord, once per step.

    void setRecording(bool r)
    {
        if (r && !recording)
            recordedNoteCount = 0;
        recording = r;
        recordSampleCounter = 0.0;
    }
    bool isRecording() const { return recording; }

    void setRecordMode(RecordMode m) { recordMode = m; }
    RecordMode getRecordMode() const { return recordMode; }

    int getRecordedNoteCount() const { return recordedNoteCount; }
    const RecordedNote& getRecordedNote(int idx) const
    {
        static RecordedNote empty;
        if (idx >= 0 && idx < recordedNoteCount)
            return recordedNotes[idx];
        return empty;
    }
    void clearRecordedNotes() { recordedNoteCount = 0; }

    // Called on the audio thread, while isRecording(), at the moment the note
    // sounds. Individual: once for every note the arpeggiator emits (ratchet
    // hits, inserted notes and late strum notes included), with the emitted
    // velocity. Chord: every note of the held chord, once per step that plays
    // (a step dropped by Density or muted by Mutate records nothing), with the
    // velocity the key was played at. Must be real-time safe.
    std::function<void(int pitch, float velocity)> onRecordNote;

    // --- Scale ---
    // Pitches the arpeggiator makes up (Dice and Octave Scatter jumps, mutated
    // positions, and the inserted notes and ratchets of such steps) snap to the
    // nearest allowed pitch; a tie goes down. A step that plays a held note
    // unchanged is never altered, in or out of the scale.
    // Bit n of the mask allows pitch class (root + n) % 12. SCALE_CHROMATIC
    // (the default) or an empty mask = no constraint.

    void setScale(int rootNote0to11, uint16_t noteMask)
    {
        const uint32_t root = (uint32_t) (((rootNote0to11 % 12) + 12) % 12);
        uint32_t mask = (uint32_t) (noteMask & SCALE_CHROMATIC);
        if (mask == 0u)
            mask = SCALE_CHROMATIC;
        scaleSetting.store((root << 16) | mask);
    }
    int getScaleRoot() const { return (int) (scaleSetting.load() >> 16); }
    uint16_t getScaleMask() const { return (uint16_t) (scaleSetting.load() & 0xFFFFu); }
    bool isScaleConstrained() const { return getScaleMask() != SCALE_CHROMATIC; }

    static bool isPitchInScale(int pitch, int root, uint16_t mask)
    {
        const int pc = (((pitch - root) % 12) + 12) % 12;
        return (mask & (uint16_t) (1u << pc)) != 0;
    }

    // Nearest allowed pitch in 0..127; a tie goes down. Unchanged when nothing is allowed.
    static int snapPitchToScale(int pitch, int root, uint16_t mask)
    {
        mask = (uint16_t) (mask & SCALE_CHROMATIC);
        if (mask == 0 || mask == SCALE_CHROMATIC)
            return pitch;
        for (int d = 0; d < 12; ++d)
        {
            if (pitch - d >= 0 && pitch - d <= 127 && isPitchInScale(pitch - d, root, mask))
                return pitch - d;
            if (pitch + d >= 0 && pitch + d <= 127 && isPitchInScale(pitch + d, root, mask))
                return pitch + d;
        }
        return pitch;
    }

    // --- Transport ---
    // Call syncToTransport once per audio block, before processBlock, while
    // the sequencer runs: `beatsSinceStart` is the transport position in
    // quarter notes at the first sample of that block. The step lines then sit
    // on the transport's grid (multiples of the rate, counted from beat 0;
    // swing pairs and a tapped rhythm cycle are counted from beat 0 too): the
    // step being played is stretched or shortened to end on the grid line
    // nearest its own end, so a key pressed between lines sounds at once and
    // the next note is on the grid. A jump of the transport is taken up the
    // same way. While pressure or a glide is moving the rate there is no grid
    // and the arp runs free. Blocks without the call run free, as before.
    // With the transport, Mutate counts its interval in bars (4 beats) of
    // transport time: one round at every `interval`-th bar line.
    void syncToTransport(double beatsSinceStart)
    {
        transportBeat = beatsSinceStart;
        transportSyncPending = true;
    }
    bool isTransportSynced() const { return transportSynced; }

    // Transport start: the pattern starts again from its first step (frozen:
    // from the start of the loop), on the first sample of the next block. The
    // sounding note is released. The variation stream starts again too.
    // Same thread as noteOn / processBlock.
    void resetPhase()
    {
        sendPendingNoteOff();
        if (freezeLoopActive())
        {
            restorePosition(freezeState);
            freezeLoopIndex = 0;
            advanceOneStep();           // the loop's first step, as at every loop start
        }
        else
        {
            currentStep = 0;
            walkPosition = 0;
            repeatSubStep = 0;
            polyArpAccentIndex = 0;
            upDownGoingUp = true;
            strumAltDown = false;
            variationCounter = 0u;
            stepCounter = 0;
        }
        sampleCounter = 0.0;
        stepLengthOverridden = false;
        hitCount = 0;
        nextHit = 0;
        mutateBeatCounter = 0.0;
        lastMutateRound = -1;
        firstNotePending = (inputNoteCount > 0 && sequenceLength > 0);
    }

    // Step length in quarter notes at the set rate (1/16 = 0.25).
    double getStepLengthBeats() const { return getRateMultiplier(); }

    // --- Export ---
    // Writes one cycle as plain step data, one entry per arp step at the
    // current rate, and returns the number of steps (at most maxSteps).
    // Frozen (and the loop has started playing): the frozen loop exactly as it
    // sounds, every variation included (inserted notes and ratchets only as the
    // `hits` count). Not frozen: one cycle of the current sequence with the
    // macros at rest and mutations applied; the random modes (Random, Walk,
    // Random Octave) give the sequence in order.
    // Message thread only (it reads what the audio thread plays; it changes nothing).
    int exportSteps(ExportedStep* out, int maxSteps) const
    {
        if (out == nullptr || maxSteps <= 0 || sequenceLength <= 0)
            return 0;

        VariationMacros localMacros = macros;
        localMacros.setHumanize(humanizeEnabled ? humanizeAmount : 0.0f);

        const bool loop = freezeLoopActive();
        const int steps = juce::jmin(maxSteps, loop ? freezeLoopLength : cycleLength());
        const int rhythmSteps = stepRhythmLength();

        PositionState p;
        RandomSource rs;
        if (loop)
        {
            p = freezeState;
            if (p.currentStep >= sequenceLength)
                p.currentStep = 0;
            rs.loop = true;
            rs.loopBase = freezeBaseCounter;
        }
        else
        {
            rs.none = true;
        }

        double start = 0.0;
        for (int i = 0; i < steps; ++i)
        {
            rs.loopIndex = i;
            if (loop || i > 0)
            {
                if (noteRepeats > 1 && p.repeatSubStep < noteRepeats - 1)
                    ++p.repeatSubStep;
                else
                {
                    p.repeatSubStep = 0;
                    advancePos(p, rs);
                }
            }
            if (i > 0)
                ++p.stepCounter;

            auto& e = out[i];
            e = ExportedStep();
            e.startSteps = (float) start;
            e.lengthSteps = (rhythmSteps > 0) ? (float) rhythmStepScale[p.stepCounter % rhythmSteps] : 1.0f;
            start += (double) e.lengthSteps;

            VariationMacros::StepModification mod;
            if (loop)
                mod = localMacros.apply((int) ((freezeBaseCounter + (uint32_t) i) & 0x7FFFFFFFu), sequenceLength, variationSeed);
            if (mod.skip)
                continue;

            StrumNote notes[MAX_INPUT_NOTES];
            int count = 0;
            if (!buildStepNotes(p, rs, mod.noteOffset, mod.velocityScale, notes, count))
                continue;

            Hit stepHits[MAX_HITS];
            int stepHitCount = 0;
            const int ratchets = planHits(mod, stepHits, stepHitCount);
            if (stepHitCount <= 0)
                continue;

            e.rest = false;
            e.hits = stepHitCount;
            e.gate = (stepHits[ratchets - 1].offFrac - stepHits[0].onFrac) * e.lengthSteps;
            e.noteCount = juce::jmin(count, (int) ExportedStep::MAX_NOTES);
            for (int n = 0; n < e.noteCount; ++n)
            {
                e.pitches[n] = notes[n].pitch;
                e.velocities[n] = juce::jlimit(0.01f, 1.0f, notes[n].velocity * stepHits[0].velScale);
                e.velocity = juce::jmax(e.velocity, e.velocities[n]);
            }
        }
        return steps;
    }

    // --- Pressure Routing ---

    void setPressureTarget(PressureTarget t) { pressureTarget = t; }
    PressureTarget getPressureTarget() const { return pressureTarget; }

    void setPressureAmount(float a) { pressureAmount = juce::jlimit(0.0f, 1.0f, a); }
    float getPressureAmount() const { return pressureAmount; }

    void applyPressure(float pressure)
    {
        currentPressure = juce::jlimit(0.0f, 1.0f, pressure);
    }
    float getCurrentPressure() const { return currentPressure; }

    // Returns the effective gate length after pressure modulation
    float getEffectiveGate() const
    {
        if (currentPressure <= 0.0f || pressureAmount <= 0.0f)
            return gate;
        if (pressureTarget == PressureTarget::GateLength)
        {
            float mod = currentPressure * pressureAmount * 0.5f;
            return juce::jlimit(0.0f, 1.0f, gate + mod);
        }
        return gate;
    }

    // Returns the effective octave range after pressure modulation
    int getEffectiveOctaveRange() const
    {
        if (currentPressure <= 0.0f || pressureAmount <= 0.0f)
            return octaveRange;
        if (pressureTarget == PressureTarget::OctaveRange)
        {
            int mod = (int)(currentPressure * pressureAmount * 4.0f);
            return juce::jlimit(1, 8, octaveRange + mod);
        }
        return octaveRange;
    }

    // Returns how much faster the arpeggiator runs under pressure (1 = unchanged, up to 2)
    double getEffectiveRateFactor() const
    {
        if (currentPressure > 0.0f && pressureAmount > 0.0f && pressureTarget == PressureTarget::Rate)
            return 1.0 + (double) (currentPressure * pressureAmount);
        return 1.0;
    }

    // --- Parameter locks and their interaction with the macros ---
    // A lock replaces the base value of the step being played; the macros'
    // result for the same value is combined with it by the interaction mode.
    // Locks stay until cleared (the sequencer sets them per step).

    void setParamLock(LockParam p, float value)
    {
        const int i = (int) p;
        if (i < 0 || i >= (int) LockParam::NumParams) return;
        lockValue[i] = value;
        lockActive[i] = true;
    }
    void clearParamLock(LockParam p)
    {
        const int i = (int) p;
        if (i >= 0 && i < (int) LockParam::NumParams) lockActive[i] = false;
    }
    void clearParamLocks()
    {
        for (auto& a : lockActive) a = false;
    }
    bool hasParamLock(LockParam p) const
    {
        const int i = (int) p;
        return i >= 0 && i < (int) LockParam::NumParams && lockActive[i];
    }

    void setMacroInteraction(MacroInteraction m) { macroInteraction = m; }
    MacroInteraction getMacroInteraction() const { return macroInteraction; }

    // Combine a parameter lock value with a macro value given a base value
    static float combineMacroAndLock(MacroInteraction mode, float baseValue,
                                     float lockValue, float macroValue)
    {
        switch (mode)
        {
            case MacroInteraction::Additive:
            {
                float lockDelta = lockValue - baseValue;
                float macroDelta = macroValue - baseValue;
                return baseValue + lockDelta + macroDelta;
            }
            case MacroInteraction::MacroWins:
                return macroValue;
            case MacroInteraction::LockWins:
                return lockValue;
            case MacroInteraction::Multiply:
            {
                // Normalize both to 0-1 range relative to base, multiply, scale back
                if (baseValue == 0.0f) return lockValue * macroValue;
                return (lockValue / baseValue) * macroValue;
            }
            default:
                return baseValue;
        }
    }

    static juce::String getMacroInteractionName(MacroInteraction m)
    {
        switch (m)
        {
            case MacroInteraction::Additive:  return "ADD";
            case MacroInteraction::MacroWins: return "MAC";
            case MacroInteraction::LockWins:  return "LCK";
            case MacroInteraction::Multiply:  return "MUL";
            default:                          return "--";
        }
    }

    // --- Callbacks ---

    std::function<void(int pitch, float velocity)> onNoteOn;
    std::function<void(int pitch)> onNoteOff;

    // --- Note input ---

    void noteOn(int pitch, float velocity)
    {
        // Latch: the first key after every key was released starts a new chord.
        if (latch && !frozen && physicalNoteCount == 0 && inputNoteCount > 0)
        {
            sendPendingNoteOff();
            inputNoteCount = 0;
            sequenceLength = 0;
            playLogCount = 0;
        }

        for (int i = 0; i < inputNoteCount; ++i)
            if (inputNotes[i].pitch == pitch)
            {
                inputNotes[i].velocity = velocity;
                if (!inputNotes[i].held)
                {
                    // A latched key pressed again: one more entry in the play order.
                    inputNotes[i].held = true;
                    ++physicalNoteCount;
                    appendPlayLog(pitch, velocity);
                }
                if (!frozen)
                    rebuildSequence();          // the new velocity counts from the next step
                return;
            }

        if (inputNoteCount >= MAX_INPUT_NOTES)
            return;

        inputNotes[inputNoteCount] = { pitch, velocity, noteOrderCounter++, true };
        ++inputNoteCount;
        ++physicalNoteCount;
        appendPlayLog(pitch, velocity);

        if (inputNoteCount == 1)
        {
            currentStep = 0;
            upDownGoingUp = true;
            sampleCounter = 0.0;
            stepCounter = 0;
            walkPosition = 0;
            repeatSubStep = 0;
            polyArpAccentIndex = 0;
            strumAltDown = false;
            hitCount = 0;
            nextHit = 0;
            stepLengthOverridden = false;
            firstNotePending = true;    // the first note sounds at once, not a step later
        }

        if (!frozen)
            rebuildSequence();
    }

    void noteOff(int pitch)
    {
        int idx = -1;
        for (int i = 0; i < inputNoteCount; ++i)
        {
            if (inputNotes[i].pitch == pitch)
            {
                idx = i;
                break;
            }
        }

        if (idx < 0)
            return;

        if (inputNotes[idx].held)
        {
            inputNotes[idx].held = false;
            physicalNoteCount = juce::jmax(0, physicalNoteCount - 1);
        }

        if (latch)
            return;

        if (frozen)
            return;

        removeInputNote(pitch);

        if (inputNoteCount == 0)
        {
            sendPendingNoteOff();
            reset();
            return;
        }

        rebuildSequence();
    }

    // --- Processing ---

    void processBlock(int numSamples, double sampleRate, float tempo)
    {
        if (sampleRate > 0.0)
            lastSampleRate = sampleRate;

        applyPendingActions();

        // The transport position counts for this block only.
        const bool synced = transportSyncPending;
        transportSyncPending = false;
        if (synced != transportSynced)
        {
            transportSynced = synced;
            lastMutateRound = -1;
        }

        if (!enabled || sequenceLength == 0 || inputNoteCount == 0 || tempo <= 0.0f || sampleRate <= 0.0)
            return;

        const double samplesPerBeat = (60.0 / (double) tempo) * sampleRate;

        // Step length in beats: the rate, made shorter by pressure, reached through the glide.
        setRateTarget(getRateMultiplier() / getEffectiveRateFactor());

        if (synced)
            alignToTransport(samplesPerBeat * rateCurrent);

        double currentStepLength = stepLengthOverridden ? stepLengthOverride
                                                        : stepLengthFor(samplesPerBeat * rateCurrent);

        if (firstNotePending)
        {
            firstNotePending = false;
            planStep();
            fireDueHits(currentStepLength);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            sampleCounter += 1.0;

            // Advance recording time counter
            if (recording)
                recordSampleCounter += 1.0;

            // Notes of a strum that come after the first one
            if (strumActive)
            {
                const double base = strumBaseFrac * currentStepLength;
                bool waiting = false;
                for (int s = 0; s < strumNoteCount; ++s)
                {
                    if (strumNoteSent[s])
                        continue;
                    if (sampleCounter >= base + strumNotes[s].delaySamples)
                        emitStepNote(s, strumVelScale);
                    else
                        waiting = true;
                }
                strumActive = waiting;
            }

            fireDueHits(currentStepLength);

            if (noteIsPlaying && sampleCounter >= currentOffFrac * currentStepLength)
                sendPendingNoteOff();

            if (sampleCounter >= currentStepLength)
            {
                sampleCounter -= currentStepLength;
                mutateBeatCounter += currentStepLength / samplesPerBeat;
                ++stepCounter;
                stepLengthOverridden = false;

                stepBoundary(currentStepLength, getGlideTimeSamples(sampleRate, (double) tempo),
                             transportBeat + (double) (i + 1) / samplesPerBeat);

                currentStepLength = stepLengthFor(samplesPerBeat * rateCurrent);

                planStep();
                fireDueHits(currentStepLength);
            }
        }
    }

    void triggerFirstNote()
    {
        enabled = true;
        firstNotePending = false;
        if (sequenceLength > 0 && inputNoteCount > 0)
        {
            planStep();
            fireDueHits(1.0);      // sampleCounter is 0: only what starts right on the step
        }
    }

    void reset()
    {
        sendPendingNoteOff();
        inputNoteCount = 0;
        physicalNoteCount = 0;
        sequenceLength = 0;
        currentStep = 0;
        stepCounter = 0;
        sampleCounter = 0.0;
        upDownGoingUp = true;
        noteIsPlaying = false;
        lastPlayedPitch = -1;
        walkPosition = 0;
        strumNoteCount = 0;
        strumActive = false;
        hitCount = 0;
        nextHit = 0;
        firstNotePending = false;

        repeatSubStep = 0;
        glideFromPitch = -1;
        polyArpAccentIndex = 0;
        strumAltDown = false;
        recordSampleCounter = 0.0;
        rateCurrent = 0.0;          // the next phrase starts at the set rate
        playLogCount = 0;
        stepLengthOverridden = false;
    }

    bool isRunning() const
    {
        return enabled && inputNoteCount > 0 && sequenceLength > 0;
    }

    int getInputNoteCount() const { return inputNoteCount; }
    int getSequenceLength() const { return sequenceLength; }
    int getCurrentStep() const { return currentStep; }

    int getSequencePitch(int idx) const
    {
        if (idx >= 0 && idx < sequenceLength)
            return sequence[idx].pitch;
        return -1;
    }

    // The notes of the step being played (a chord in the chord modes, else one note).
    int getStrumNoteCount() const { return strumNoteCount; }

    const StrumNote& getStrumNote(int idx) const
    {
        static StrumNote empty;
        if (idx >= 0 && idx < strumNoteCount)
            return strumNotes[idx];
        return empty;
    }

    // --- Static helpers ---

    static juce::String getModeName(Mode m)
    {
        switch (m)
        {
            case Mode::Up:           return "UP";
            case Mode::Down:         return "DN";
            case Mode::UpDown:       return "UD";
            case Mode::Random:       return "RN";
            case Mode::AsPlayed:     return "AP";
            case Mode::ChordRepeat:  return "CH";
            case Mode::Walk:         return "WK";
            case Mode::PlayOrder:    return "PO";
            case Mode::Converge:     return "CV";
            case Mode::Diverge:      return "DV";
            case Mode::PolyArp:      return "PA";
            case Mode::RandomOctave: return "RO";
            default:                 return "--";
        }
    }

    // Up to five characters, for a display cell.
    static juce::String getModeLabel(Mode m)
    {
        switch (m)
        {
            case Mode::Up:           return "UP";
            case Mode::Down:         return "DOWN";
            case Mode::UpDown:       return "UPDN";
            case Mode::Random:       return "RAND";
            case Mode::AsPlayed:     return "PLAYD";
            case Mode::ChordRepeat:  return "CHORD";
            case Mode::Walk:         return "WALK";
            case Mode::PlayOrder:    return "ORDER";
            case Mode::Converge:     return "CONV";
            case Mode::Diverge:      return "DIVG";
            case Mode::PolyArp:      return "POLY";
            case Mode::RandomOctave: return "RNOCT";
            default:                 return "--";
        }
    }

    static juce::String getRateName(Rate r)
    {
        switch (r)
        {
            case Rate::Whole:        return "1/1";
            case Rate::Half:         return "1/2";
            case Rate::Quarter:      return "1/4";
            case Rate::Eighth:       return "1/8";
            case Rate::Sixteenth:    return "1/16";
            case Rate::ThirtySecond: return "1/32";
            case Rate::QuarterT:     return "1/4T";
            case Rate::EighthT:      return "1/8T";
            case Rate::SixteenthT:   return "1/16T";
            case Rate::ThirtySecondT: return "1/32T";
            case Rate::SixtyFourth:  return "1/64";
            case Rate::SixtyFourthT: return "1/64T";
            default:                 return "--";
        }
    }

private:
    static constexpr int MAX_INPUT_NOTES = 16;
    static constexpr int MAX_SEQUENCE_NOTES = 128;
    static constexpr int MAX_RECORDED_NOTES = 256;
    static constexpr int MAX_HITS = 12;             // (1 + 2 inserted notes) x 4 ratchets

    struct InputNote
    {
        int pitch = 0;
        float velocity = 0.0f;
        int order = 0;
        bool held = true;       // key still down (false: kept by latch or freeze)
    };

    struct SeqNote
    {
        int pitch = 0;
        float velocity = 0.0f;
    };

    // One trigger of the step's notes; times are fractions of the step length.
    struct Hit
    {
        float onFrac = 0.0f;
        float offFrac = 0.5f;
        float velScale = 1.0f;
    };

    // Position state restored at the start of every frozen loop.
    struct PositionState
    {
        int currentStep = 0;
        int stepCounter = 0;
        int walkPosition = 0;
        int repeatSubStep = 0;
        int polyArpAccentIndex = 0;
        bool upDownGoingUp = true;
        bool strumAltDown = false;
    };

    // Config
    Mode mode = Mode::Up;
    Rate rate = Rate::Eighth;
    int octaveRange = 1;
    float gate = 0.5f;
    float swing = 0.0f;
    bool latch = false;
    bool enabled = false;

    // Strum config
    bool strumEnabled = false;
    float strumTime = 20.0f;
    int strumDirection = 0;
    float strumVelCurve = 0.0f;

    // Tapped rhythm template
    double rhythmTime[MAX_RHYTHM_EVENTS] = {};
    float rhythmVelocity[MAX_RHYTHM_EVENTS] = {};
    double rhythmStepScale[MAX_RHYTHM_EVENTS] = {};     // step lengths of the tapped cycle, in steps
    int rhythmLength = 0;
    StripTarget stripTarget = StripTarget::Off;
    bool rhythmEnabled = false;

    // Ratchet config
    bool ratchetEnabled = false;
    int ratchetCount = 1;
    float ratchetDecay = 0.0f;

    // Repeats config
    int noteRepeats = 1;

    // Glide config and state
    float glideTime = 0.0f;
    int glideFromPitch = -1;
    double rateCurrent = 0.0;           // step length in beats being played (0 = not started)
    double rateGlideStart = 0.0;
    double rateGlideTarget = 0.0;
    double rateGlideElapsed = 0.0;

    // Humanize config
    bool humanizeEnabled = false;
    float humanizeAmount = 0.5f;

    // Variation macros
    VariationMacros macros;
    int variation = 1;
    uint32_t variationSeed = 1u;
    uint32_t variationCounter = 0u;

    // Freeze config and state
    bool frozen = false;
    SeqNote frozenSequence[MAX_SEQUENCE_NOTES] = {};
    int frozenSequenceLength = 0;
    int freezeGeneration = 0;           // counts freezes (UI side)
    int freezeSeenGeneration = 0;       // the freeze the audio side has captured
    int freezeLoopLength = 0;
    int freezeLoopIndex = 0;
    uint32_t freezeBaseCounter = 0u;
    PositionState freezeState;

    // Mutate
    MutateSystem mutate;
    bool mutateEnabled = false;
    double mutateBeatCounter = 0.0;
    uint32_t mutateSeedState = 1u;

    // Requests from the UI, single writer and single reader, kept in order.
    enum class MutateAction : int { Mutate = 0, Return, Commit, Original, Restore };
    MutateSystem::NoteState restoreState;       // written by the UI before it asks for Restore
    std::atomic<bool> restoreWaiting { false };
    static constexpr int ACTION_QUEUE_SIZE = 32;
    std::atomic<int> actionQueue[ACTION_QUEUE_SIZE] = {};
    std::atomic<unsigned int> actionWriteIndex { 0u };
    std::atomic<unsigned int> actionReadIndex { 0u };

    void pushAction(MutateAction a)
    {
        const unsigned int w = actionWriteIndex.load();
        if (w - actionReadIndex.load() >= (unsigned int) ACTION_QUEUE_SIZE)
            return;                                         // full: drop
        actionQueue[w % (unsigned int) ACTION_QUEUE_SIZE].store((int) a);
        actionWriteIndex.store(w + 1u);
    }

    // The mutate state as it will be once the waiting requests are carried out.
    void mutateStateWithPending(int& generation, bool& committed) const
    {
        generation = mutate.getNoteGeneration();
        committed = mutate.hasCommittedNotes();
        const unsigned int w = actionWriteIndex.load();
        for (unsigned int r = actionReadIndex.load(); r != w; ++r)
        {
            switch ((MutateAction) actionQueue[r % (unsigned int) ACTION_QUEUE_SIZE].load())
            {
                case MutateAction::Mutate:
                    generation = (mutate.getMode() == MutateSystem::Mode::Ancestor) ? 1 : generation + 1;
                    break;
                case MutateAction::Return:   generation = 0; break;
                case MutateAction::Commit:   committed = committed || generation > 0; generation = 0; break;
                case MutateAction::Original: generation = 0; committed = false; break;
                case MutateAction::Restore:
                    if (restoreWaiting.load()) { generation = restoreState.generation; committed = restoreState.committed; }
                    break;
            }
        }
    }

    // Recording config
    bool recording = false;
    RecordMode recordMode = RecordMode::Individual;
    RecordedNote recordedNotes[MAX_RECORDED_NOTES] = {};
    int recordedNoteCount = 0;
    double recordSampleCounter = 0.0;

    // Pressure routing config
    PressureTarget pressureTarget = PressureTarget::GateLength;
    float pressureAmount = 0.0f;
    float currentPressure = 0.0f;

    // Parameter locks and macro interaction
    MacroInteraction macroInteraction = MacroInteraction::Additive;
    float lockValue[(int) LockParam::NumParams] = {};
    bool lockActive[(int) LockParam::NumParams] = {};

    // Scale: root in the high word, 12-bit mask in the low word
    std::atomic<uint32_t> scaleSetting { (uint32_t) SCALE_CHROMATIC };

    // Transport
    double transportBeat = 0.0;             // position at the start of the block being processed
    bool transportSyncPending = false;      // syncToTransport was called for the next block
    bool transportSynced = false;           // the block being processed follows the transport
    double stepLengthOverride = 0.0;        // length of the current step when it was fitted to the grid
    bool stepLengthOverridden = false;
    long long lastMutateRound = -1;         // the mutate interval the transport was in at the last step

    // State
    InputNote inputNotes[MAX_INPUT_NOTES] = {};
    int inputNoteCount = 0;
    int physicalNoteCount = 0;
    int noteOrderCounter = 0;

    // Every key press of the chord, in order (PlayOrder mode)
    struct PlayedKey
    {
        int pitch = 0;
        float velocity = 0.0f;
    };
    static constexpr int MAX_PLAY_LOG = 32;
    PlayedKey playLog[MAX_PLAY_LOG] = {};
    int playLogCount = 0;

    void appendPlayLog(int pitch, float velocity)
    {
        if (playLogCount >= MAX_PLAY_LOG)
        {
            for (int i = 1; i < playLogCount; ++i)      // full: the oldest press goes
                playLog[i - 1] = playLog[i];
            --playLogCount;
        }
        playLog[playLogCount++] = { pitch, velocity };
    }

    void removeFromPlayLog(int pitch)
    {
        int kept = 0;
        for (int i = 0; i < playLogCount; ++i)
            if (playLog[i].pitch != pitch)
                playLog[kept++] = playLog[i];
        playLogCount = kept;
    }

    SeqNote sequence[MAX_SEQUENCE_NOTES] = {};
    int sequenceLength = 0;
    int chordSize = 1;                  // notes per chord step in the built sequence
    Mode builtMode = Mode::Up;
    int builtOctaves = 1;
    int currentStep = 0;
    int stepCounter = 0;
    double sampleCounter = 0.0;
    bool upDownGoingUp = true;
    bool noteIsPlaying = false;
    int lastPlayedPitch = -1;
    bool firstNotePending = false;
    double lastSampleRate = 44100.0;

    int walkPosition = 0;

    // Repeat state
    int repeatSubStep = 0;

    // PolyArp accent state
    int polyArpAccentIndex = 0;
    static constexpr float polyArpAccentPattern[4] = { 1.0f, 0.65f, 0.85f, 0.5f };

    // The notes of the current step and what has been sent of them
    StrumNote strumNotes[MAX_INPUT_NOTES] = {};
    bool strumNoteSent[MAX_INPUT_NOTES] = {};
    int strumNoteCount = 0;
    bool strumActive = false;           // notes of a strum are still to come
    bool strumAltDown = false;          // alternating strum: next one goes down
    float strumBaseFrac = 0.0f;
    float strumVelScale = 1.0f;

    // The hits of the current step
    Hit hits[MAX_HITS];
    int hitCount = 0;
    int nextHit = 0;
    float currentOffFrac = 0.5f;

    // Audio-thread-safe PRNG (xorshift32)
    uint32_t xorshiftState = 12345u;

    static void xorshift32(uint32_t& state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        if (state == 0) state = 1;
    }

    static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
    {
        uint32_t h = (a + 0x9E3779B9u) * 0x85EBCA6Bu;
        h ^= (b + 0x7F4A7C15u) * 0xC2B2AE35u;
        h ^= (c + 0x165667B1u) * 0x27D4EB2Fu;
        h ^= h >> 16; h *= 0x7FEB352Du;
        h ^= h >> 15; h *= 0x846CA68Bu;
        h ^= h >> 16;
        return h;
    }

    bool freezeLoopActive() const { return frozen && freezeSeenGeneration == freezeGeneration && freezeLoopLength > 0; }

    // Where a step's random numbers come from: the running generator, the
    // frozen loop (a fixed function of the loop position), or nowhere (export
    // of the plain sequence).
    struct RandomSource
    {
        bool loop = false;
        uint32_t loopBase = 0u;
        int loopIndex = 0;
        uint32_t state = 1u;
        bool none = false;
    };

    RandomSource liveRandom() const
    {
        RandomSource rs;
        rs.loop = freezeLoopActive();
        rs.loopBase = freezeBaseCounter;
        rs.loopIndex = freezeLoopIndex;
        rs.state = xorshiftState;
        return rs;
    }

    uint32_t randomFrom(RandomSource& rs, uint32_t salt) const
    {
        if (rs.none)
            return 0u;
        if (rs.loop)
            return hash3(variationSeed ^ rs.loopBase, (uint32_t) rs.loopIndex, salt) >> 4;
        xorshift32(rs.state);
        return rs.state >> 4;
    }

    bool isChordMode() const { return mode == Mode::ChordRepeat || mode == Mode::PolyArp; }

    // Number of steps in the tapped cycle when the taps set the step timing, else 0.
    int stepRhythmLength() const
    {
        const int n = rhythmLength;
        if (!rhythmEnabled || n < 2 || n > MAX_RHYTHM_EVENTS || (isChordMode() && strumEnabled))
            return 0;
        return n;
    }

    void removeInputNote(int pitch)
    {
        for (int i = 0; i < inputNoteCount; ++i)
        {
            if (inputNotes[i].pitch == pitch)
            {
                for (int j = i; j < inputNoteCount - 1; ++j)
                    inputNotes[j] = inputNotes[j + 1];
                --inputNoteCount;
                removeFromPlayLog(pitch);
                return;
            }
        }
    }

    // Lets go of every note whose key is up (after latch or freeze ends).
    void releaseUnheldNotes()
    {
        int kept = 0;
        for (int i = 0; i < inputNoteCount; ++i)
        {
            if (inputNotes[i].held)
                inputNotes[kept++] = inputNotes[i];
            else
                removeFromPlayLog(inputNotes[i].pitch);
        }

        if (kept == inputNoteCount)
        {
            if (inputNoteCount > 0)
                rebuildSequence();
            return;
        }

        inputNoteCount = kept;
        physicalNoteCount = kept;
        if (inputNoteCount == 0)
        {
            sendPendingNoteOff();
            reset();
            return;
        }
        rebuildSequence();
    }

    void rebuildSequence()
    {
        if (inputNoteCount == 0)
        {
            sequenceLength = 0;
            return;
        }

        const int octaves = getEffectiveOctaveRange();     // the set range plus pressure
        builtOctaves = octaves;
        builtMode = mode;
        chordSize = inputNoteCount;

        InputNote sorted[MAX_INPUT_NOTES];
        for (int i = 0; i < inputNoteCount; ++i)
            sorted[i] = inputNotes[i];

        // Sorting: PolyArp and RandomOctave sort ascending like Up
        if (mode == Mode::Up || mode == Mode::Down || mode == Mode::UpDown
            || mode == Mode::Walk || mode == Mode::Converge || mode == Mode::Diverge
            || mode == Mode::PolyArp || mode == Mode::RandomOctave)
        {
            for (int i = 1; i < inputNoteCount; ++i)
            {
                InputNote key = sorted[i];
                int j = i - 1;
                while (j >= 0 && sorted[j].pitch > key.pitch)
                {
                    sorted[j + 1] = sorted[j];
                    --j;
                }
                sorted[j + 1] = key;
            }
        }
        else if (mode == Mode::AsPlayed)
        {
            for (int i = 1; i < inputNoteCount; ++i)
            {
                InputNote key = sorted[i];
                int j = i - 1;
                while (j >= 0 && sorted[j].order > key.order)
                {
                    sorted[j + 1] = sorted[j];
                    --j;
                }
                sorted[j + 1] = key;
            }
        }

        sequenceLength = 0;

        if (mode == Mode::PlayOrder && playLogCount > 0)
        {
            // Every key press in order, each at the velocity of that press.
            for (int oct = 0; oct < octaves; ++oct)
            {
                for (int i = 0; i < playLogCount; ++i)
                {
                    if (sequenceLength >= MAX_SEQUENCE_NOTES) break;
                    sequence[sequenceLength].pitch = playLog[i].pitch + oct * 12;
                    sequence[sequenceLength].velocity = playLog[i].velocity;
                    ++sequenceLength;
                }
            }
        }
        else if (mode == Mode::ChordRepeat || mode == Mode::PolyArp)
        {
            // Both chord modes build the same sequence (all notes per octave)
            for (int oct = 0; oct < octaves; ++oct)
            {
                for (int i = 0; i < inputNoteCount; ++i)
                {
                    if (sequenceLength >= MAX_SEQUENCE_NOTES) break;
                    sequence[sequenceLength].pitch = sorted[i].pitch + oct * 12;
                    sequence[sequenceLength].velocity = sorted[i].velocity;
                    ++sequenceLength;
                }
            }
        }
        else if (mode == Mode::Down)
        {
            for (int oct = octaves - 1; oct >= 0; --oct)
            {
                for (int i = inputNoteCount - 1; i >= 0; --i)
                {
                    if (sequenceLength >= MAX_SEQUENCE_NOTES) break;
                    sequence[sequenceLength].pitch = sorted[i].pitch + oct * 12;
                    sequence[sequenceLength].velocity = sorted[i].velocity;
                    ++sequenceLength;
                }
            }
        }
        else if (mode == Mode::Converge)
        {
            for (int oct = 0; oct < octaves; ++oct)
            {
                int lo = 0, hi = inputNoteCount - 1;
                bool fromLow = true;
                while (lo <= hi)
                {
                    if (sequenceLength >= MAX_SEQUENCE_NOTES) break;
                    int idx = fromLow ? lo : hi;
                    sequence[sequenceLength].pitch = sorted[idx].pitch + oct * 12;
                    sequence[sequenceLength].velocity = sorted[idx].velocity;
                    ++sequenceLength;
                    if (fromLow) ++lo; else --hi;
                    fromLow = !fromLow;
                }
            }
        }
        else if (mode == Mode::Diverge)
        {
            for (int oct = 0; oct < octaves; ++oct)
            {
                int mid = inputNoteCount / 2;
                int lo = mid - 1, hi = mid + 1;

                if (sequenceLength < MAX_SEQUENCE_NOTES)
                {
                    sequence[sequenceLength].pitch = sorted[mid].pitch + oct * 12;
                    sequence[sequenceLength].velocity = sorted[mid].velocity;
                    ++sequenceLength;
                }

                while (lo >= 0 || hi < inputNoteCount)
                {
                    if (hi < inputNoteCount && sequenceLength < MAX_SEQUENCE_NOTES)
                    {
                        sequence[sequenceLength].pitch = sorted[hi].pitch + oct * 12;
                        sequence[sequenceLength].velocity = sorted[hi].velocity;
                        ++sequenceLength;
                        ++hi;
                    }
                    if (lo >= 0 && sequenceLength < MAX_SEQUENCE_NOTES)
                    {
                        sequence[sequenceLength].pitch = sorted[lo].pitch + oct * 12;
                        sequence[sequenceLength].velocity = sorted[lo].velocity;
                        ++sequenceLength;
                        --lo;
                    }
                }
            }
        }
        else
        {
            // Up, UpDown, Random, AsPlayed, PlayOrder, RandomOctave
            for (int oct = 0; oct < octaves; ++oct)
            {
                for (int i = 0; i < inputNoteCount; ++i)
                {
                    if (sequenceLength >= MAX_SEQUENCE_NOTES) break;
                    sequence[sequenceLength].pitch = sorted[i].pitch + oct * 12;
                    sequence[sequenceLength].velocity = sorted[i].velocity;
                    ++sequenceLength;
                }
            }
        }

        if (currentStep >= sequenceLength)
            currentStep = 0;

        // Chord modes: a step starts on a whole chord, also after the chord changed size.
        if (isChordMode() && chordSize > 1)
            currentStep -= currentStep % chordSize;
    }

    // Moves a position on by one step of the mode.
    void advancePos(PositionState& p, RandomSource& rs) const
    {
        if (sequenceLength == 0)
            return;

        const bool randomMode = (mode == Mode::Random || mode == Mode::RandomOctave || mode == Mode::Walk);

        if (randomMode && rs.none)
        {
            ++p.currentStep;
            if (p.currentStep >= sequenceLength)
                p.currentStep = 0;
        }
        else if (mode == Mode::Random || mode == Mode::RandomOctave)
        {
            p.currentStep = (int) (randomFrom(rs, 1u) % (uint32_t) sequenceLength);
        }
        else if (mode == Mode::Walk)
        {
            int direction = (randomFrom(rs, 2u) & 1u) == 0u ? -1 : 1;
            p.walkPosition += direction;
            if (p.walkPosition < 0) p.walkPosition = 0;
            if (p.walkPosition >= sequenceLength) p.walkPosition = sequenceLength - 1;
            p.currentStep = p.walkPosition;
        }
        else if (mode == Mode::UpDown)
        {
            if (sequenceLength <= 1)
            {
                p.currentStep = 0;
                return;
            }

            if (p.upDownGoingUp)
            {
                ++p.currentStep;
                if (p.currentStep >= sequenceLength)
                {
                    p.upDownGoingUp = false;
                    p.currentStep = sequenceLength - 2;
                    if (p.currentStep < 0) p.currentStep = 0;
                }
            }
            else
            {
                --p.currentStep;
                if (p.currentStep < 0)
                {
                    p.upDownGoingUp = true;
                    p.currentStep = 1;
                    if (p.currentStep >= sequenceLength) p.currentStep = 0;
                }
            }
        }
        else if (isChordMode())
        {
            p.currentStep += juce::jmax(1, chordSize);
            if (p.currentStep >= sequenceLength)
                p.currentStep = 0;

            // PolyArp: advance accent pattern
            if (mode == Mode::PolyArp)
                p.polyArpAccentIndex = (p.polyArpAccentIndex + 1) % 4;
        }
        else
        {
            ++p.currentStep;
            if (p.currentStep >= sequenceLength)
                p.currentStep = 0;
        }
    }

    void advancePosition()
    {
        PositionState p = capturePosition();
        RandomSource rs = liveRandom();
        advancePos(p, rs);
        currentStep = p.currentStep;
        walkPosition = p.walkPosition;
        upDownGoingUp = p.upDownGoingUp;
        polyArpAccentIndex = p.polyArpAccentIndex;
        xorshiftState = rs.state;
    }

    // Repeats per note: the position only moves on after all repeats are done.
    void advanceOneStep()
    {
        if (noteRepeats > 1 && repeatSubStep < noteRepeats - 1)
        {
            ++repeatSubStep;
        }
        else
        {
            repeatSubStep = 0;
            advancePosition();
        }
    }

    // Steps until the stream of positions repeats (random modes: one pass of the sequence).
    int cycleLength() const
    {
        int positions = sequenceLength;
        if (isChordMode())
            positions = (sequenceLength + juce::jmax(1, chordSize) - 1) / juce::jmax(1, chordSize);
        else if (mode == Mode::UpDown && sequenceLength > 1)
            positions = 2 * (sequenceLength - 1);
        return juce::jmax(1, positions) * noteRepeats;
    }

    // Length of step `index` of the timing cycle, in steps. The cycle is the
    // tapped rhythm (rhythmSteps of them) or, without one, the swing pair: the
    // first step of each pair is longer, so the off-beat comes late.
    double stepScaleAt(int index, int rhythmSteps) const
    {
        if (rhythmSteps > 0)
            return rhythmStepScale[index % rhythmSteps];
        const double swingAmount = (double) swing * 0.5;
        return (index % 2 == 0) ? (1.0 + swingAmount) : (1.0 - swingAmount);
    }

    double stepLengthFor(double samplesPerStep) const
    {
        return samplesPerStep * stepScaleAt(stepCounter, stepRhythmLength());
    }

    // Fits the step being played to the transport: it will end on the grid
    // line nearest to where it would have ended anyway (at once if that line
    // has just gone by), and the step after it is the one the grid has there.
    void alignToTransport(double samplesPerStep)
    {
        if (samplesPerStep <= 0.0 || std::abs(rateCurrent - getRateMultiplier()) > 1.0e-9)
            return;                                     // pressure or a glide is moving the rate: no grid

        const int rhythmSteps = stepRhythmLength();
        const int cycle = (rhythmSteps > 0) ? rhythmSteps : 2;

        double lineAt[MAX_RHYTHM_EVENTS + 1];           // grid lines of one cycle, in steps
        lineAt[0] = 0.0;
        for (int k = 0; k < cycle; ++k)
            lineAt[k + 1] = lineAt[k] + stepScaleAt(k, rhythmSteps);
        const double cycleSteps = lineAt[cycle];

        double now = std::fmod(transportBeat / rateCurrent, cycleSteps);
        if (now < 0.0)
            now += cycleSteps;

        const double length = stepLengthOverridden ? stepLengthOverride : stepLengthFor(samplesPerStep);
        const double naturalEnd = now + (length - sampleCounter) / samplesPerStep;

        double best = 0.0, bestDistance = 1.0e30;
        int bestIndex = 0;
        for (int c = -1; c <= 2; ++c)
            for (int k = 0; k < cycle; ++k)
            {
                const double line = (double) c * cycleSteps + lineAt[k];
                const double distance = std::abs(line - naturalEnd);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = line;
                    bestIndex = k;
                }
            }

        stepLengthOverride = sampleCounter + (best - now) * samplesPerStep;
        stepLengthOverridden = true;

        // The step that starts on that line is step `bestIndex` of the cycle.
        const int want = (bestIndex + cycle - 1) % cycle;
        stepCounter += (want - (stepCounter % cycle) + cycle) % cycle;
    }

    void setRateTarget(double target)
    {
        if (rateCurrent <= 0.0 || glideTime <= 0.0f)
        {
            rateCurrent = rateGlideStart = rateGlideTarget = target;
            return;
        }
        if (std::abs(target - rateGlideTarget) > 1.0e-9)
        {
            rateGlideTarget = target;
            rateGlideStart = rateCurrent;
            rateGlideElapsed = 0.0;
        }
    }

    void advanceRateGlide(double stepLength, double glideSamples)
    {
        if (rateCurrent <= 0.0 || rateCurrent == rateGlideTarget)
            return;
        rateGlideElapsed += stepLength;
        const double t = (glideSamples > 0.0) ? rateGlideElapsed / glideSamples : 1.0;
        if (t >= 1.0 || rateGlideStart <= 0.0)
            rateCurrent = rateGlideTarget;
        else
            rateCurrent = rateGlideStart * std::pow(rateGlideTarget / rateGlideStart, t);
    }

    // Mutate actions asked for from the UI.
    void applyPendingActions()
    {
        const unsigned int w = actionWriteIndex.load();
        unsigned int r = actionReadIndex.load();
        for (; r != w; ++r)
        {
            switch ((MutateAction) actionQueue[r % (unsigned int) ACTION_QUEUE_SIZE].load())
            {
                case MutateAction::Mutate:   runMutation(); break;
                case MutateAction::Return:   mutate.returnNotesToAncestor(); break;
                case MutateAction::Commit:   mutate.commitNotesToAncestor(); break;
                case MutateAction::Original: mutate.resetNotes(); break;
                case MutateAction::Restore:
                    if (restoreWaiting.exchange(false)) mutate.setNoteState(restoreState);
                    break;
            }
        }
        actionReadIndex.store(r);
    }

    void runMutation()
    {
        if (sequenceLength <= 0)
            return;
        mutate.setAmount(macros.getSpice());        // mutation amount is Spice
        xorshift32(mutateSeedState);
        mutate.mutateNotes(sequenceLength, mutateSeedState);
    }

    PositionState capturePosition() const
    {
        PositionState p;
        p.currentStep = currentStep;
        p.stepCounter = stepCounter;
        p.walkPosition = walkPosition;
        p.repeatSubStep = repeatSubStep;
        p.polyArpAccentIndex = polyArpAccentIndex;
        p.upDownGoingUp = upDownGoingUp;
        p.strumAltDown = strumAltDown;
        return p;
    }

    void restorePosition(const PositionState& p)
    {
        currentStep = (p.currentStep < sequenceLength) ? p.currentStep : 0;
        if (!transportSynced)
            stepCounter = p.stepCounter;        // with the transport the step count follows the grid
        walkPosition = p.walkPosition;
        repeatSubStep = p.repeatSubStep;
        polyArpAccentIndex = p.polyArpAccentIndex;
        upDownGoingUp = p.upDownGoingUp;
        strumAltDown = p.strumAltDown;
    }

    // Everything that happens between two steps, before the next one is planned.
    void stepBoundary(double finishedStepLength, double glideSamples, double beatNow)
    {
        applyPendingActions();

        // Freeze: capture the loop once, then restart it every loop length.
        if (frozen)
        {
            if (freezeSeenGeneration != freezeGeneration || freezeLoopLength <= 0)
            {
                freezeState = capturePosition();
                freezeLoopLength = cycleLength();
                freezeLoopIndex = 0;
                freezeBaseCounter = variationCounter;
                freezeSeenGeneration = freezeGeneration;
            }
            else if (++freezeLoopIndex >= freezeLoopLength)
            {
                freezeLoopIndex = 0;
                restorePosition(freezeState);
            }
        }
        else
        {
            freezeLoopLength = 0;
        }

        // Mutate: one round every `interval` bars, of the transport when it
        // runs (at the bar line), else of playing.
        if (mutateEnabled && !frozen)
        {
            const double beatsPerRound = (double) mutate.getInterval() * 4.0;
            if (transportSynced)
            {
                const long long round = (long long) std::floor((beatNow + 1.0e-4) / beatsPerRound);
                if (lastMutateRound >= 0 && round > lastMutateRound)
                    runMutation();
                lastMutateRound = round;
                mutateBeatCounter = 0.0;
            }
            else if (mutateBeatCounter >= beatsPerRound - 1.0e-6)
            {
                mutateBeatCounter = std::fmod(mutateBeatCounter, beatsPerRound);
                if (mutateBeatCounter > beatsPerRound - 1.0e-6)
                    mutateBeatCounter = 0.0;
                runMutation();
            }
        }
        else
        {
            mutateBeatCounter = 0.0;
            lastMutateRound = -1;
        }

        // A changed mode or octave range (also through pressure) takes effect here.
        if (!frozen && (builtMode != mode || builtOctaves != getEffectiveOctaveRange()))
            rebuildSequence();

        advanceOneStep();

        advanceRateGlide(finishedStepLength, glideSamples);
    }

    static int foldPitch(int pitch)
    {
        while (pitch > 127) pitch -= 12;
        while (pitch < 0) pitch += 12;
        return pitch;
    }

    // Fills `out` with the notes of the step at position `p`. Returns false for a rest.
    bool buildStepNotes(PositionState& p, RandomSource& rs, int macroNoteOffset, float macroVelocityScale,
                        StrumNote* out, int& outCount) const
    {
        outCount = 0;
        if (sequenceLength == 0)
            return false;
        if (p.currentStep >= sequenceLength)
            p.currentStep = 0;
        const int stepPos = p.currentStep;

        const bool chordMode = isChordMode();
        int count = 1;
        if (chordMode)
            count = juce::jlimit(1, (int) MAX_INPUT_NOTES, juce::jmin(juce::jmax(1, chordSize), sequenceLength - stepPos));

        const bool strum = strumEnabled && chordMode && count > 1;

        int indices[MAX_INPUT_NOTES];
        for (int i = 0; i < count; ++i)
            indices[i] = i;

        if (strum)
        {
            bool down = (strumDirection == 1);
            if (strumDirection == 2)
            {
                down = p.strumAltDown;
                p.strumAltDown = !p.strumAltDown;
            }
            if (down)
                std::reverse(indices, indices + count);
        }

        const bool useRhythm = strum && isRhythmEnabled();
        const double strumSamples = (double) strumTime * 0.001 * lastSampleRate;

        // RandomOctave: one shift per step, -2..+2 octaves
        int octaveShift = 0;
        if (mode == Mode::RandomOctave && !rs.none)
            octaveShift = ((int) (randomFrom(rs, 3u) % 5u) - 2) * 12;

        float accent = (mode == Mode::PolyArp) ? polyArpAccentPattern[p.polyArpAccentIndex % 4] : 1.0f;

        // Tapped rhythm as step timing: the tap's level is the step's accent.
        const int rhythmSteps = stepRhythmLength();
        if (rhythmSteps > 0)
            accent *= rhythmVelocity[p.stepCounter % rhythmSteps];

        const uint32_t scale = scaleSetting.load();
        const int scaleRoot = (int) (scale >> 16);
        const uint16_t scaleMask = (uint16_t) (scale & 0xFFFFu);

        const bool velocityLocked = lockActive[(int) LockParam::Velocity];
        const float lockedVelocity = lockValue[(int) LockParam::Velocity];

        for (int i = 0; i < count; ++i)
        {
            const int seqIdx = stepPos + indices[i];
            const auto& slot = mutate.getNoteMutation(seqIdx);
            if (slot.muted)
                continue;

            // Octaves that leave the MIDI range fold back, keeping the pitch class.
            // A pitch the macros or mutate changed goes to the scale; a held note as played does not.
            int pitch = foldPitch(sequence[seqIdx].pitch + octaveShift + slot.pitchOffset + macroNoteOffset);
            if (slot.pitchOffset != 0 || macroNoteOffset != 0)
                pitch = snapPitchToScale(pitch, scaleRoot, scaleMask);

            // Level: the played velocity (or its lock) against what the macros make of it.
            const float base = sequence[seqIdx].velocity * slot.velocityScale;
            const float macro = base * macroVelocityScale;
            float vel = velocityLocked ? combineMacroAndLock(macroInteraction, base, lockedVelocity, macro) : macro;

            double delay = 0.0;
            if (strum)
            {
                const float pos = (float) i / (float) (count - 1);
                float velScale = 1.0f;
                if (strumVelCurve <= 0.5f)
                    velScale = 1.0f - (strumVelCurve * 2.0f) * pos * 0.5f;
                else
                    velScale = 1.0f - ((strumVelCurve - 0.5f) * 2.0f) * (1.0f - pos) * 0.5f;
                vel *= velScale;

                if (useRhythm)
                {
                    const int r = juce::jmin(i, rhythmLength - 1);
                    delay = rhythmTime[r] * strumSamples;
                    vel *= rhythmVelocity[r];
                }
                else
                {
                    delay = (double) pos * strumSamples;
                }
            }

            vel *= accent;

            auto& note = out[outCount];
            note.pitch = pitch;
            note.velocity = juce::jlimit(0.0f, 1.0f, vel);
            note.delaySamples = delay;
            ++outCount;
        }

        // The first note of a strum always starts the strum.
        if (strum && outCount > 0)
        {
            double firstDelay = out[0].delaySamples;
            for (int s = 1; s < outCount; ++s)
                firstDelay = juce::jmin(firstDelay, out[s].delaySamples);
            for (int s = 0; s < outCount; ++s)
                out[s].delaySamples -= firstDelay;
        }

        return outCount > 0;
    }

    // Plans the step at the current position: its notes and its hits.
    void planStep()
    {
        sendPendingNoteOff();           // nothing of the last step may outlive its notes
        hitCount = 0;
        nextHit = 0;
        if (sequenceLength == 0)
            return;

        // The variation for this step. Frozen: the same ones come round again.
        macros.setHumanize(humanizeEnabled ? humanizeAmount : 0.0f);
        uint32_t variationIndex;
        if (freezeLoopActive())
            variationIndex = freezeBaseCounter + (uint32_t) freezeLoopIndex;
        else
            variationIndex = variationCounter++;

        const auto mod = macros.apply((int) (variationIndex & 0x7FFFFFFFu), sequenceLength, variationSeed);
        if (mod.skip)
            return;                                     // a dropped step

        PositionState p = capturePosition();
        RandomSource rs = liveRandom();
        const bool sounding = buildStepNotes(p, rs, mod.noteOffset, mod.velocityScale, strumNotes, strumNoteCount);
        currentStep = p.currentStep;
        strumAltDown = p.strumAltDown;
        xorshiftState = rs.state;
        for (int s = 0; s < strumNoteCount; ++s)
            strumNoteSent[s] = false;
        if (!sounding)
            return;                                     // every note of the step is muted

        planHits(mod, hits, hitCount);
    }

    // The hits of a step: the note itself, its ratchet repeats and inserted
    // notes. Returns the number of ratchets per note.
    int planHits(const VariationMacros::StepModification& mod, Hit* outHits, int& outHitCount) const
    {
        outHitCount = 0;

        // Gate: base (with pressure), its lock, and the macros' gate spread.
        const float baseGate = getEffectiveGate();
        const float macroGate = baseGate * mod.gateScale;
        float stepGate = lockActive[(int) LockParam::Gate]
                             ? combineMacroAndLock(macroInteraction, baseGate, lockValue[(int) LockParam::Gate], macroGate)
                             : macroGate;
        stepGate = juce::jlimit(0.02f, 1.0f, stepGate);

        // Ratchets: the set count, its lock, and the macros' ratchet chance.
        const int baseRatchets = (ratchetEnabled ? ratchetCount : 1);
        const int macroRatchets = juce::jmax(baseRatchets, mod.ratchetCount);
        int ratchets = macroRatchets;
        if (lockActive[(int) LockParam::Ratchet])
            ratchets = (int) std::lround(combineMacroAndLock(macroInteraction, (float) baseRatchets,
                                                             lockValue[(int) LockParam::Ratchet], (float) macroRatchets));
        ratchets = juce::jlimit(1, 4, ratchets);

        // Humanize: the step starts a little late (never early, so it cannot pile up).
        float delay = 0.0f;
        if (humanizeEnabled && humanizeAmount > 0.0f)
            delay = juce::jlimit(0.0f, 0.3f, mod.timingOffset + 0.08f * humanizeAmount);

        // Inserted notes share the step evenly with the step's own note.
        const int slots = 1 + juce::jlimit(0, 2, mod.extraNotes);
        const float slotLength = (1.0f - delay) / (float) slots;

        for (int s = 0; s < slots; ++s)
        {
            const float slotStart = delay + (float) s * slotLength;
            const float gateLength = stepGate * slotLength;
            const float slotLevel = (s == 0) ? 1.0f : mod.extraVelocity;

            for (int k = 0; k < ratchets && outHitCount < MAX_HITS; ++k)
            {
                auto& hit = outHits[outHitCount++];
                hit.onFrac = slotStart + gateLength * (float) k / (float) ratchets;
                hit.offFrac = juce::jmin(1.0f, slotStart + gateLength * (float) (k + 1) / (float) ratchets);
                hit.velScale = slotLevel * std::pow(1.0f - ratchetDecay, (float) k);
            }
        }
        return ratchets;
    }

    void fireDueHits(double stepLength)
    {
        while (nextHit < hitCount && sampleCounter >= (double) hits[nextHit].onFrac * stepLength)
        {
            fireHit(nextHit);
            ++nextHit;
        }
    }

    void fireHit(int index)
    {
        sendPendingNoteOff();

        const auto& hit = hits[index];
        const bool firstOfStep = (index == 0);
        bool waiting = false;

        for (int s = 0; s < strumNoteCount; ++s)
        {
            // Only the first hit of a step is strummed; repeats are block chords.
            if (firstOfStep && strumNotes[s].delaySamples > 0.0)
            {
                strumNoteSent[s] = false;
                waiting = true;
                continue;
            }
            emitStepNote(s, hit.velScale);
        }

        strumActive = waiting;
        strumBaseFrac = hit.onFrac;
        strumVelScale = hit.velScale;
        currentOffFrac = hit.offFrac;
        noteIsPlaying = true;

        if (firstOfStep)
        {
            glideFromPitch = lastPlayedPitch;
            lastPlayedPitch = strumNotes[0].pitch;

            // Chord recording: the held chord at the time of the step
            if (recording && recordMode == RecordMode::Chord)
                for (int i = 0; i < inputNoteCount; ++i)
                {
                    recordNote(inputNotes[i].pitch, inputNotes[i].velocity);
                    if (onRecordNote)
                        onRecordNote(inputNotes[i].pitch, inputNotes[i].velocity);
                }
        }
    }

    void emitStepNote(int s, float velScale)
    {
        const int pitch = strumNotes[s].pitch;
        const float vel = juce::jlimit(0.01f, 1.0f, strumNotes[s].velocity * velScale);
        strumNoteSent[s] = true;
        if (pitch < 0 || pitch > 127)
            return;
        if (onNoteOn)
            onNoteOn(pitch, vel);
        if (recording && recordMode == RecordMode::Individual)
        {
            recordNote(pitch, vel);
            if (onRecordNote)
                onRecordNote(pitch, vel);
        }
    }

    void sendPendingNoteOff()
    {
        strumActive = false;
        if (!noteIsPlaying)
            return;

        for (int s = 0; s < strumNoteCount; ++s)
        {
            if (strumNoteSent[s])
            {
                if (onNoteOff && strumNotes[s].pitch >= 0 && strumNotes[s].pitch <= 127)
                    onNoteOff(strumNotes[s].pitch);
                strumNoteSent[s] = false;
            }
        }

        noteIsPlaying = false;
    }

    // Record a note to the recording buffer
    void recordNote(int pitch, float velocity)
    {
        if (!recording || recordedNoteCount >= MAX_RECORDED_NOTES)
            return;

        recordedNotes[recordedNoteCount].pitch = pitch;
        recordedNotes[recordedNoteCount].velocity = velocity;
        recordedNotes[recordedNoteCount].sampleTimestamp = recordSampleCounter;
        ++recordedNoteCount;
    }

    double getRateMultiplier() const
    {
        switch (rate)
        {
            case Rate::Whole:        return 4.0;
            case Rate::Half:         return 2.0;
            case Rate::Quarter:      return 1.0;
            case Rate::Eighth:       return 0.5;
            case Rate::Sixteenth:    return 0.25;
            case Rate::ThirtySecond: return 0.125;
            case Rate::QuarterT:     return 2.0 / 3.0;
            case Rate::EighthT:      return 1.0 / 3.0;
            case Rate::SixteenthT:   return 1.0 / 6.0;
            case Rate::ThirtySecondT: return 1.0 / 12.0;
            case Rate::SixtyFourth:  return 1.0 / 16.0;
            case Rate::SixtyFourthT: return 1.0 / 24.0;
            default:                 return 0.5;
        }
    }
};
