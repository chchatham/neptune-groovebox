#pragma once

#include "Pattern.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <functional>
#include <vector>
#include <atomic>
#include <array>

class SequencerCore
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void stepAdvanced(int trackIndex, int stepIndex) { juce::ignoreUnused(trackIndex, stepIndex); }
        virtual void transportStarted() {}
        virtual void transportStopped() {}
        virtual void patternChanged(int patternIndex) { juce::ignoreUnused(patternIndex); }
    };

    SequencerCore();

    void prepareToPlay(double sampleRate, int samplesPerBlock);

    void processBlock(int numSamples);

    void start();
    void stop();
    bool isPlaying() const { return playing; }

    // ---- Count-in and beat ticks (the metronome's clock) ----------------------------------------
    //
    // requestStart() is what Play does: start(), or - when recording is armed and a count-in is
    // set - count the bars in first. During a count-in nothing plays, nothing is recorded and no
    // step or pattern moves on: isPlaying() stays false, and the transport starts (from the top,
    // exactly as start() does, listeners told) on the sample the count-in ends, inside
    // processBlock. stop() during a count-in calls it off; start() skips what is left of it.
    // The count follows tempo changes.
    //
    // Every processBlock leaves the beats that fell in it in getBeatTick(): the count-in's, then
    // the running transport's (the first beat on the sample the transport starts; every fourth
    // beat is a bar's first). They say when, not whether anything should sound.
    static constexpr int MAX_COUNT_IN_BARS = 2;
    static constexpr int BEATS_PER_BAR = 4;
    void setCountInBars(int bars) { countInBars = juce::jlimit(0, MAX_COUNT_IN_BARS, bars); }
    int getCountInBars() const { return countInBars.load(); }
    void requestStart();
    bool isCountingIn() const { return countingIn.load(); }
    // Playing or counting in: what a press of Play would stop.
    bool isRunning() const { return playing.load() || countingIn.load(); }
    // Beats of the count-in still to come (0 when none is running).
    int getCountInBeatsLeft() const { return countingIn.load() ? countInBeatsLeft.load() : 0; }
    // True once after a count-in has started the transport (the audio thread starts the MIDI clock on it).
    bool consumeCountInStart() { return countInStarted.exchange(false); }

    struct BeatTick { int sample = 0; bool accent = false; bool countIn = false; };
    static constexpr int MAX_BEAT_TICKS = 8;
    int getBeatTickCount() const { return beatTickCount; }
    const BeatTick& getBeatTick(int i) const { return beatTicks[(size_t) juce::jlimit(0, MAX_BEAT_TICKS - 1, i)]; }
    // Back to the first step of every track (Shift + Play). Only while stopped; start() does the
    // same on its own.
    void resetPosition()
    {
        if (playing)
            return;
        for (auto& acc : stepAccumulators) acc = 0.0;
        for (auto& step : currentSteps) step = 0;
        for (auto& count : trigCounts) count = 0;
        std::fill(prevTrigFired.begin(), prevTrigFired.end(), false);
        beatAccumulator = 0.0;
        beatCount = 0;
        resetTrigTiming(false);
    }

    // True when the track should sound: soloed, or not muted while no other track is soloed.
    bool isTrackAudible(int trackIndex) const;

    // Switching recording on starts a new take: getLastRecordedTrack() / getFirstRecordedStep()
    // then report what was recorded since.
    void setRecording(bool rec)
    {
        if (rec && !recording)
        {
            lastRecordedTrack = -1;
            for (auto& s : firstRecordedSteps) s = -1;
            serviceLanePool();
        }
        recording = rec;
    }
    bool isRecording() const { return recording; }

    // How a played note is written. Free: on the nearest step with its distance from the grid kept
    // as micro-timing (played back as played; QN pulls it to the grid without changing it).
    // Quant: on the nearest step, exactly on the grid.
    enum class RecordMode { Free, Quant };
    void setRecordMode(RecordMode mode) { recordMode = mode; }
    RecordMode getRecordMode() const { return recordMode.load(); }

    // Writes a note at the playhead. Notes that land on the same step in the same pass (a chord,
    // two drum pads together) are kept together; a later pass over the step replaces its notes.
    void recordNote(int trackIndex, int note, float velocity);

    // The current take (since recording was switched on): the track that received a note last
    // (-1 = nothing was recorded) and the lowest step written on a track (-1 = none).
    int getLastRecordedTrack() const { return lastRecordedTrack.load(); }
    int getFirstRecordedStep(int trackIndex) const
    {
        return (trackIndex >= 0 && trackIndex < MAX_TRACKS) ? firstRecordedSteps[(size_t) trackIndex].load() : -1;
    }

    // Quarter notes since the transport started (fractional), for things that follow the grid.
    double getBeatPosition() const
    {
        double perBeat = cachedSamplesPerStep * 4.0;
        return (double) beatCount + (perBeat > 0.0 ? beatAccumulator / perBeat : 0.0);
    }

    void setTempo(float bpm);
    float getTempo() const { return tempo; }

    void setSwing(float amount);

    void setQuantizeAmount(float amount) { quantizeAmount = juce::jlimit(0.0f, 1.0f, amount); }
    float getQuantizeAmount() const { return quantizeAmount; }

    // Fill mode — global
    void setFillActive(bool active) { fillActive = active; }
    bool isFillActive() const { return fillActive.load(); }

    // Momentary fill helpers (call setFillActive(false) on release unless latched)
    void setFillLatch(bool latch) { fillLatch = latch; }
    bool isFillLatched() const { return fillLatch.load(); }

    // Per-track fill (OR'd with global fill)
    void setTrackFill(int trackIdx, bool active);
    bool isTrackFillActive(int trackIdx) const;

    // Query: would this step trigger given current fill state?
    bool isStepActiveDuringFill(int trackIdx, int stepIdx) const;

    // Legacy alias kept for compatibility
    void setFillMode(bool fill) { setFillActive(fill); }
    bool isFillMode() const { return isFillActive(); }

    // Pattern bank
    static constexpr int MAX_PATTERNS = 16;
    Pattern& getPattern() { return patterns[activePattern]; }
    const Pattern& getPattern() const { return patterns[activePattern]; }
    Pattern& getPattern(int index) { return patterns[juce::jlimit(0, MAX_PATTERNS - 1, index)]; }
    const Pattern& getPattern(int index) const { return patterns[juce::jlimit(0, MAX_PATTERNS - 1, index)]; }
    int getActivePatternIndex() const { return activePattern; }
    int getPatternCount() const { return MAX_PATTERNS; }

    enum class ChangeMode { Immediate, NextBeat, NextBar, NextLoop };
    void selectPattern(int index, ChangeMode mode = ChangeMode::NextLoop);
    int getPendingPattern() const { return pendingPattern; }

    int getCurrentStep(int trackIndex) const;
    int getSelectedTrack() const { return selectedTrack; }
    void setSelectedTrack(int track);

    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

    using TrigCallback = std::function<void(int trackIndex, const Step& step)>;
    void setTrigCallback(TrigCallback cb) { onTrig = std::move(cb); }

    using NoteOffCallback = std::function<void(int trackIndex, int note)>;
    void setNoteOffCallback(NoteOffCallback cb) { onNoteOff = std::move(cb); }

    float getEffectiveMicroTiming(const Step& step, int stepIndex) const;

    // ---- Pressure lanes: pad pressure recorded with a note and played back to its voice ----------
    //
    // RECORDING (all on the audio thread, driven by AudioEngine's pad path). A pad note written by
    // recordNote() can be followed by beginPressureCapture(); the pad's pressure is then sampled
    // once per processBlock, in musical time (tempo and track speed), until endPressureCapture().
    // If the pressure ever reached MIN_LANE_PEAK the lane is stored in the note and the step's
    // length becomes the time the pad was held; otherwise nothing is stored. A capture ends by
    // itself after one pass of the track (its length in steps), and is dropped when a later hit
    // replaces the step or the transport is started again.
    //
    // PLAYBACK. A note with a lane, on an audible track, gets a player (MAX_LANE_PLAYERS at once;
    // beyond that a note plays without its pressure). Once per processBlock each player sends the
    // lane's value at the note's current musical age through the pressure callback, when it has
    // moved by at least half a MIDI step; 0 is sent before the note's note-off, when its track is
    // muted, when the step is edited away, and on stop.
    //
    // MEMORY. Lanes are recorded into buffers from a fixed pool (LANE_POOL_SLOTS places, of which
    // LANE_POOL_READY are kept ready, MAX_POINTS bytes each) and swapped into the note, so the
    // audio thread never allocates; buffers of replaced notes go back to the pool. serviceLanePool()
    // (any other thread; the app calls it 30 times a second, and arming recording calls it) tops
    // the pool up. With no ready buffer a note is recorded without pressure.
    static constexpr int MAX_PRESSURE_CAPTURES = 16;       // one per pad
    static constexpr int MAX_LANE_PLAYERS = 32;
    static constexpr int LANE_POOL_SLOTS = 256;
    static constexpr int LANE_POOL_READY = 64;
    static constexpr float MIN_LANE_PEAK = 0.03f;          // below this a note has "no pressure"

    using PressureCallback = std::function<void(int trackIndex, int note, float pressure)>;
    void setPressureCallback(PressureCallback cb) { onLanePressure = std::move(cb); }

    // Changes every time recordNote() writes a note.
    uint32_t getRecordSerial() const { return recordSerial.load(std::memory_order_relaxed); }
    // Starts recording pressure for the note recordNote() has just written (it must be this track
    // and note). False if that is not the case or no buffer is ready.
    bool beginPressureCapture(int pad, int trackIndex, int note);
    void setCapturePressure(int pad, float pressure);
    void endPressureCapture(int pad, bool keep);
    bool isCapturingPressure(int pad) const
    {
        return pad >= 0 && pad < MAX_PRESSURE_CAPTURES && captures[(size_t) pad].active;
    }

    // The lanes playing on a track send their value again at the next block (a live pad that
    // overrode one has been let go).
    void refreshLanePressure(int trackIndex);
    int getActiveLanePlayers() const { return activeLanePlayers; }

    void serviceLanePool();
    int getReadyLaneBuffers() const;

private:
    void beginPlaying();
    int processCountIn(int numSamples, double samplesPerBeat);
    void addBeatTick(int sample, bool accent, bool countIn)
    {
        if (beatTickCount < MAX_BEAT_TICKS)
            beatTicks[(size_t) beatTickCount++] = { sample, accent, countIn };
    }

    std::atomic<int> countInBars { 0 };
    std::atomic<bool> countingIn { false };
    std::atomic<bool> countInFresh { false };      // a count-in was asked for; the audio thread begins it
    std::atomic<int> countInBeatsLeft { 0 };
    std::atomic<bool> countInStarted { false };
    int countInBeatsTotal = 0;                     // audio thread from here
    double countInAccumulator = 0.0;
    bool countInDue = false;
    bool firstBeatDue = false;                     // the transport has started and its first beat is to be ticked
    std::array<BeatTick, MAX_BEAT_TICKS> beatTicks {};
    int beatTickCount = 0;

    void advanceStep(int trackIndex);
    void arriveAtStep(int trackIndex);
    void fireStep(int trackIndex, int stepIndex);
    void resetTrigTiming(bool arriveAtFirstStep);
    void scheduleNoteOff(int trackIndex, int pitch, double durationSamples);
    bool evaluateTrigCondition(const TrigConditionData& cond, int trackIndex, int stepIndex = -1) const;
    bool isFillActiveForTrack(int trackIndex) const;
    void applyPatternChange();

    std::array<Pattern, MAX_PATTERNS> patterns;
    int activePattern = 0;
    int pendingPattern = -1;
    ChangeMode pendingChangeMode = ChangeMode::NextLoop;
    int selectedTrack = 0;

    double sampleRate = 44100.0;
    std::atomic<float> tempo { 120.0f };
    std::atomic<bool> playing { false };
    std::atomic<bool> recording { false };
    std::atomic<RecordMode> recordMode { RecordMode::Free };
    std::atomic<bool> fillActive { false };
    std::atomic<bool> fillLatch { false };
    static constexpr int MAX_TRACKS = 64;
    std::array<std::atomic<bool>, MAX_TRACKS> trackFillFlags {};

    // The take being recorded, and the step a note was just played into (so the sequencer does not
    // play it a second time in the pass it was recorded in). NEXT_STEP marks "the step after the
    // playhead" (a note played a little early).
    static constexpr int NEXT_STEP = 1 << 20;
    static constexpr int MAX_RECORDED_NOTES_PER_STEP = 16;
    std::atomic<int> lastRecordedTrack { -1 };
    std::array<std::atomic<int>, MAX_TRACKS> firstRecordedSteps {};
    std::array<std::atomic<int>, MAX_TRACKS> recordedStep {};
    std::array<int, MAX_TRACKS> recordStampStep {};
    std::array<int, MAX_TRACKS> recordStampVisit {};
    float quantizeAmount = 0.0f;

    double samplesPerStep = 0.0;
    double beatAccumulator = 0.0;
    int beatCount = 0;
    std::vector<double> stepAccumulators;
    std::vector<int> currentSteps;
    std::vector<int> trigCounts;
    std::vector<bool> prevTrigFired;

    // Micro-timing: a trig sounds at its step's start plus its offset. Per track: the fraction of
    // the current step at which its trig is still due (late, offset > 0) and at which the next
    // step's trig is due (early, offset < 0); -1 = none. firedEarly = the step now starting has
    // sounded already. atStepStart = the track was put on a step without advancing to it (start,
    // pattern change) and has yet to play it.
    std::vector<float> lateTrigAt, earlyTrigAt;
    std::vector<bool> firedEarly, atStepStart;
    std::vector<int> stepVisits;

    static constexpr int MAX_PENDING_NOTEOFFS = 128;
    struct PendingNoteOff
    {
        int trackIndex = -1;
        int pitch = 0;
        double remainingSamples = 0.0;
        bool active = false;
    };
    std::array<PendingNoteOff, MAX_PENDING_NOTEOFFS> pendingNoteOffs {};
    int activeNoteOffCount = 0;
    double cachedSamplesPerStep = 0.0;

    // ---- Pressure lanes ---------------------------------------------------------------------------
    struct LaneSlot
    {
        enum : int { Empty = 0, Ready = 1, Busy = 2 };
        std::atomic<int> state { Empty };
        PressureLane lane;
    };
    struct LaneCapture
    {
        bool active = false;
        int pattern = 0, track = 0, step = 0, pitch = 0, visit = 0, slot = -1;
        uint32_t epoch = 0;
        double elapsed = 0.0;       // steps since the note began
        float pressure = 0.0f;      // the pad's latest pressure
        float peak = 0.0f;
    };
    struct LanePlayer
    {
        bool active = false;
        int pattern = 0, track = 0, step = 0, pitch = 0;
        double elapsed = 0.0;
        float lastSent = 0.0f;      // -1 = send whatever the value is
    };
    struct LastRecord { bool valid = false; int pattern = 0, track = 0, step = 0, pitch = 0, visit = 0; };

    int claimLaneSlot();
    void releaseLaneSlot(LaneSlot& slot);
    void recycleLane(PressureLane& lane);
    void dropCapture(LaneCapture& c);
    void dropCapturesOn(int patternIndex, int trackIndex, int stepIndex, int pitch);
    void advancePressureLanes(int numSamples, double samplesPerStepNow);
    void startLanePlayers(int trackIndex, int stepIndex, const Step& step);
    void endLanePlayer(int trackIndex, int pitch);
    void sendLanePressure(LanePlayer& p, float value);
    const Note* laneNote(const LanePlayer& p, float& scale) const;

    std::array<LaneSlot, LANE_POOL_SLOTS> lanePool;
    std::array<LaneCapture, MAX_PRESSURE_CAPTURES> captures {};
    std::array<LanePlayer, MAX_LANE_PLAYERS> lanePlayers {};
    int activeLanePlayers = 0, activeCaptures = 0;
    LastRecord lastRecord;
    std::atomic<uint32_t> recordSerial { 0 };
    std::atomic<uint32_t> captureEpoch { 0 };      // a new one at every start: older captures are dropped
    std::array<int, MAX_TRACKS> laneLengthStep {}, laneLengthVisit {};
    int blockSample = 0;                           // the sample processBlock is at

    TrigCallback onTrig;
    NoteOffCallback onNoteOff;
    PressureCallback onLanePressure;
    juce::ListenerList<Listener> listeners;
};
