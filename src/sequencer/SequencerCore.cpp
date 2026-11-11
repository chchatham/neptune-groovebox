#include "SequencerCore.h"
#include <cmath>

SequencerCore::SequencerCore()
{
    int trackCount = patterns[activePattern].getTrackCount();
    stepAccumulators.resize(trackCount, 0.0);
    currentSteps.resize(trackCount, 0);
    trigCounts.resize(trackCount, 0);
    prevTrigFired.resize(trackCount, false);
    for (auto& s : firstRecordedSteps) s = -1;
    for (auto& s : recordedStep) s = -1;
    recordStampStep.fill(-1);
    recordStampVisit.fill(-1);
    laneLengthStep.fill(-1);
    laneLengthVisit.fill(-1);
    serviceLanePool();
    resetTrigTiming(false);
}

// No trig is waiting on any track. With arriveAtFirstStep, every track plays the step it stands on
// at the next sample (the transport started, or the pattern changed).
void SequencerCore::resetTrigTiming(bool arriveAtFirstStep)
{
    const size_t count = currentSteps.size();
    lateTrigAt.assign(count, -1.0f);
    earlyTrigAt.assign(count, -1.0f);
    firedEarly.assign(count, false);
    atStepStart.assign(count, arriveAtFirstStep);
    if (stepVisits.size() != count)
        stepVisits.assign(count, 0);
}

void SequencerCore::prepareToPlay(double sr, int /*samplesPerBlock*/)
{
    sampleRate = sr;
    samplesPerStep = (60.0 / tempo.load()) * sampleRate / 4.0;
}

void SequencerCore::processBlock(int numSamples)
{
    if (!playing)
        return;

    auto& pat = patterns[activePattern];
    double currentSamplesPerStep = (60.0 / tempo.load()) * sampleRate / 4.0;
    double samplesPerBeat = currentSamplesPerStep * 4.0;
    cachedSamplesPerStep = currentSamplesPerStep;

    for (int s = 0; s < numSamples; ++s)
    {
        blockSample = s;
        if (activeNoteOffCount > 0)
        {
            for (auto& noff : pendingNoteOffs)
            {
                if (!noff.active)
                    continue;
                noff.remainingSamples -= 1.0;
                if (noff.remainingSamples <= 0.0)
                {
                    noff.active = false;
                    --activeNoteOffCount;
                    if (activeLanePlayers > 0)
                        endLanePlayer(noff.trackIndex, noff.pitch);
                    if (onNoteOff)
                        onNoteOff(noff.trackIndex, noff.pitch);
                }
            }
        }

        beatAccumulator += 1.0;
        bool beatBoundary = beatAccumulator >= samplesPerBeat;
        if (beatBoundary)
        {
            beatAccumulator -= samplesPerBeat;
            beatCount++;

            if (pendingPattern >= 0 && pendingChangeMode == ChangeMode::NextBeat)
                applyPatternChange();
        }

        bool barBoundary = beatBoundary && (beatCount % 4 == 0);
        if (barBoundary && pendingPattern >= 0 && pendingChangeMode == ChangeMode::NextBar)
            applyPatternChange();

        for (int trackIdx = 0; trackIdx < pat.getTrackCount(); ++trackIdx)
        {
            auto& track = pat.getTrack(trackIdx);

            double trackStepLen = currentSamplesPerStep / track.speedMultiplier;

            if (atStepStart[trackIdx])
            {
                atStepStart[trackIdx] = false;
                arriveAtStep(trackIdx);
            }

            stepAccumulators[trackIdx] += 1.0;

            if (stepAccumulators[trackIdx] >= trackStepLen)
            {
                stepAccumulators[trackIdx] -= trackStepLen;
                advanceStep(trackIdx);
            }

            // Trigs off the grid: late in their own step, or early, before their step starts.
            const float late = lateTrigAt[trackIdx];
            const float early = earlyTrigAt[trackIdx];
            if (late >= 0.0f && stepAccumulators[trackIdx] >= (double) late * trackStepLen)
            {
                lateTrigAt[trackIdx] = -1.0f;
                const int stepIdx = currentSteps[trackIdx];
                if (recordedStep[(size_t) trackIdx].load() != stepIdx)
                    fireStep(trackIdx, stepIdx);
            }
            else if (early >= 0.0f && stepAccumulators[trackIdx] >= (double) early * trackStepLen)
            {
                earlyTrigAt[trackIdx] = -1.0f;
                auto& current = patterns[activePattern].getTrack(trackIdx);
                const int nextIdx = (currentSteps[trackIdx] + 1) % current.length;
                if (recordedStep[(size_t) trackIdx].load() != (nextIdx | NEXT_STEP))
                {
                    fireStep(trackIdx, nextIdx);
                    firedEarly[trackIdx] = true;
                }
            }
        }
    }

    blockSample = numSamples;
    if (activeLanePlayers > 0 || activeCaptures > 0)
        advancePressureLanes(numSamples, currentSamplesPerStep);
}

void SequencerCore::start()
{
    if (playing)
        return;

    for (auto& acc : stepAccumulators)
        acc = 0.0;
    for (auto& step : currentSteps)
        step = 0;
    std::fill(prevTrigFired.begin(), prevTrigFired.end(), false);
    beatAccumulator = 0.0;
    beatCount = 0;
    for (auto& s : recordedStep) s = -1;
    resetTrigTiming(true);      // the first step plays as the transport starts
    captureEpoch.fetch_add(1, std::memory_order_relaxed);   // pressure still being recorded belongs to the last run
    blockSample = 0;

    playing = true;
    listeners.call(&Listener::transportStarted);
}

void SequencerCore::stop()
{
    if (!playing)
        return;

    playing = false;

    // Every recorded pressure is let go, as the pads would be.
    for (auto& p : lanePlayers)
        if (p.active)
        {
            p.active = false;
            if (p.lastSent != 0.0f)
                sendLanePressure(p, 0.0f);
        }
    activeLanePlayers = 0;

    for (auto& noff : pendingNoteOffs)
    {
        if (noff.active)
        {
            noff.active = false;
            --activeNoteOffCount;
            if (onNoteOff)
                onNoteOff(noff.trackIndex, noff.pitch);
        }
    }

    listeners.call(&Listener::transportStopped);
}

void SequencerCore::setTempo(float bpm)
{
    tempo = juce::jlimit(20.0f, 300.0f, bpm);
}

void SequencerCore::setSwing(float amount)
{
    patterns[activePattern].swing = juce::jlimit(0.0f, 1.0f, amount);
}

void SequencerCore::selectPattern(int index, ChangeMode mode)
{
    if (index < 0 || index >= MAX_PATTERNS)
        return;

    if (index == activePattern)
    {
        pendingPattern = -1;
        return;
    }

    if (mode == ChangeMode::Immediate)
    {
        pendingPattern = index;
        applyPatternChange();
        return;
    }

    pendingPattern = index;
    pendingChangeMode = mode;
}

void SequencerCore::applyPatternChange()
{
    if (pendingPattern < 0 || pendingPattern >= MAX_PATTERNS)
        return;

    activePattern = pendingPattern;
    pendingPattern = -1;

    auto& pat = patterns[activePattern];
    int trackCount = pat.getTrackCount();

    if ((int) stepAccumulators.size() < trackCount)
    {
        stepAccumulators.resize(trackCount, 0.0);
        currentSteps.resize(trackCount, 0);
        trigCounts.resize(trackCount, 0);
        prevTrigFired.resize(trackCount, false);
    }

    for (int i = 0; i < trackCount; ++i)
    {
        stepAccumulators[i] = 0.0;
        currentSteps[i] = 0;
    }
    // Every track plays the new pattern's first step.
    resetTrigTiming(true);

    listeners.call(&Listener::patternChanged, activePattern);
}

void SequencerCore::recordNote(int trackIndex, int note, float velocity)
{
    if (!playing || !recording)
        return;

    auto& pat = patterns[activePattern];
    if (trackIndex < 0 || trackIndex >= pat.getTrackCount())
        return;

    auto& track = pat.getTrack(trackIndex);
    int stepIdx = currentSteps[trackIndex];

    double currentSamplesPerStep = (60.0 / tempo.load()) * sampleRate / 4.0;
    double trackStepLen = currentSamplesPerStep / track.speedMultiplier;

    float rawOffset = (float)(stepAccumulators[trackIndex] / trackStepLen);
    bool nextStep = false;
    if (rawOffset > 0.5f)
    {
        stepIdx = (stepIdx + 1) % track.length;
        rawOffset -= 1.0f;
        nextStep = true;
    }

    // The same step in the same pass: a chord, or two pads together. Otherwise this pass's note
    // replaces what the step held.
    const size_t t = (size_t) juce::jlimit(0, MAX_TRACKS - 1, trackIndex);
    const int visit = (trackIndex < (int) stepVisits.size() ? stepVisits[(size_t) trackIndex] : 0) + (nextStep ? 1 : 0);
    const bool sameVisit = recordStampStep[t] == stepIdx && recordStampVisit[t] == visit;
    recordStampStep[t] = stepIdx;
    recordStampVisit[t] = visit;

    auto& step = track.getStep(stepIdx);
    if (!sameVisit)
    {
        // The notes go, and their pressure lanes with them (the buffers back to the pool); pressure
        // still being recorded for them has nowhere to go.
        dropCapturesOn(activePattern, trackIndex, stepIdx, -1);
        for (auto& n : step.notes)
            recycleLane(n.lane);
        step.notes.clear();
        step.pressureScale = 1.0f;
        step.microTiming = (recordMode.load() == RecordMode::Quant) ? 0.0f : rawOffset;
    }
    bool present = false;
    for (auto& n : step.notes)
        if (n.pitch == note)
        {
            // Hit again in the same pass (a roll): the latest hit counts, without the earlier one's pressure.
            dropCapturesOn(activePattern, trackIndex, stepIdx, note);
            recycleLane(n.lane);
            n.velocity = velocity;
            present = true;
        }
    if (!present && (int) step.notes.size() < MAX_RECORDED_NOTES_PER_STEP)
    {
        step.notes.push_back({ note, velocity, {} });
        present = true;
    }
    step.trig = true;

    lastRecord = { present, activePattern, trackIndex, stepIdx, note, visit };
    recordSerial.fetch_add(1, std::memory_order_relaxed);

    // It was just heard as it was played: the sequencer leaves it out of this pass.
    recordedStep[t] = nextStep ? (stepIdx | NEXT_STEP) : stepIdx;

    lastRecordedTrack = trackIndex;
    const int first = firstRecordedSteps[t].load();
    if (first < 0 || stepIdx < first)
        firstRecordedSteps[t] = stepIdx;
}

int SequencerCore::getCurrentStep(int trackIndex) const
{
    if (trackIndex < 0 || trackIndex >= (int) currentSteps.size())
        return 0;
    return currentSteps[trackIndex];
}

void SequencerCore::setSelectedTrack(int track)
{
    if (track >= 0 && track < patterns[activePattern].getTrackCount())
        selectedTrack = track;
}

bool SequencerCore::isTrackAudible(int trackIndex) const
{
    const auto& pat = patterns[activePattern];
    if (trackIndex < 0 || trackIndex >= pat.getTrackCount())
        return false;

    const auto& track = pat.getTrack(trackIndex);
    if (track.soloed)
        return true;
    if (track.muted)
        return false;

    for (int i = 0; i < pat.getTrackCount(); ++i)
        if (pat.getTrack(i).soloed)
            return false;
    return true;
}

void SequencerCore::advanceStep(int trackIndex)
{
    auto& track = patterns[activePattern].getTrack(trackIndex);
    int& stepIdx = currentSteps[trackIndex];

    int nextStep = (stepIdx + 1) % track.length;

    if (nextStep == 0 && pendingPattern >= 0 && pendingChangeMode == ChangeMode::NextLoop)
    {
        applyPatternChange();
        atStepStart[trackIndex] = false;    // this track arrives at the new pattern's first step now
    }

    stepIdx = nextStep;
    ++stepVisits[trackIndex];

    listeners.call(&Listener::stepAdvanced, trackIndex, stepIdx);

    // A listener may have changed the pattern at once (the song does, on track 1's wrap): this track
    // arrives at the new pattern's first step here, not a second time at the next sample.
    atStepStart[trackIndex] = false;
    arriveAtStep(trackIndex);
}

// The track's playhead is at the start of its current step: play the step's trig (now, or later in
// the step if it is late), unless it sounded early or was played in by hand a moment ago, and see
// whether the following step's trig is due before this step ends.
void SequencerCore::arriveAtStep(int trackIndex)
{
    auto& track = patterns[activePattern].getTrack(trackIndex);
    const int stepIdx = currentSteps[trackIndex];
    auto& step = track.getStep(stepIdx);

    const bool alreadyFired = firedEarly[trackIndex];
    firedEarly[trackIndex] = false;
    lateTrigAt[trackIndex] = -1.0f;
    earlyTrigAt[trackIndex] = -1.0f;

    // A note recorded for this step before it started; a mark from the step before has done its job.
    const int recorded = recordedStep[(size_t) trackIndex].load();
    const bool justRecorded = (recorded == (stepIdx | NEXT_STEP));
    if (recorded >= 0)
        recordedStep[(size_t) trackIndex] = -1;

    if (!alreadyFired)
    {
        if (step.trig && justRecorded)
            prevTrigFired[trackIndex] = true;
        else if (step.trig)
        {
            const float offset = getEffectiveMicroTiming(step, stepIdx);
            if (offset > 0.0f)
                lateTrigAt[trackIndex] = juce::jmin(offset, 0.95f);
            else
                fireStep(trackIndex, stepIdx);
        }
        else
            prevTrigFired[trackIndex] = false;
    }

    const int nextIdx = (stepIdx + 1) % track.length;
    if (nextIdx != stepIdx && !(nextIdx == 0 && pendingPattern >= 0))
    {
        auto& next = track.getStep(nextIdx);
        if (next.trig)
        {
            const float offset = getEffectiveMicroTiming(next, nextIdx);
            if (offset < 0.0f)
                earlyTrigAt[trackIndex] = 1.0f + juce::jmax(offset, -0.95f);
        }
    }
}

void SequencerCore::fireStep(int trackIndex, int stepIndex)
{
    auto& track = patterns[activePattern].getTrack(trackIndex);
    auto& step = track.getStep(stepIndex);

    if (!evaluateTrigCondition(step.trigCondition, trackIndex, stepIndex))
    {
        prevTrigFired[trackIndex] = false;
        return;
    }

    trigCounts[trackIndex]++;
    prevTrigFired[trackIndex] = true;

    // A muted (or un-soloed) track keeps running so it is still in step when it comes back;
    // it just makes no sound.
    const bool audible = isTrackAudible(trackIndex);

    if (audible && onTrig)
        onTrig(trackIndex, step);

    if (audible && !step.notes.empty())
    {
        double trackStepLen = cachedSamplesPerStep / track.speedMultiplier;
        double durationSamples = step.length * trackStepLen;

        for (auto& note : step.notes)
            scheduleNoteOff(trackIndex, note.pitch, durationSamples);

        startLanePlayers(trackIndex, stepIndex, step);
    }
}

// ================= Pressure lanes ================================================================

// Not the audio thread. Keeps LANE_POOL_READY buffers ready: reserves the missing ones, and frees
// those beyond that number (buffers that came back from replaced notes).
void SequencerCore::serviceLanePool()
{
    int ready = getReadyLaneBuffers();
    for (auto& slot : lanePool)
    {
        if (ready == LANE_POOL_READY)
            break;
        const bool fill = ready < LANE_POOL_READY;
        int expected = fill ? LaneSlot::Empty : LaneSlot::Ready;
        if (!slot.state.compare_exchange_strong(expected, LaneSlot::Busy))
            continue;
        if (fill)
        {
            slot.lane.clear();
            slot.lane.points.reserve((size_t) PressureLane::MAX_POINTS);
            slot.state.store(LaneSlot::Ready);
            ++ready;
        }
        else
        {
            std::vector<uint8_t>().swap(slot.lane.points);
            slot.state.store(LaneSlot::Empty);
            --ready;
        }
    }
}

int SequencerCore::getReadyLaneBuffers() const
{
    int ready = 0;
    for (auto& slot : lanePool)
        if (slot.state.load() == LaneSlot::Ready)
            ++ready;
    return ready;
}

int SequencerCore::claimLaneSlot()
{
    for (int i = 0; i < LANE_POOL_SLOTS; ++i)
    {
        int expected = LaneSlot::Ready;
        if (lanePool[(size_t) i].state.compare_exchange_strong(expected, LaneSlot::Busy))
            return i;
    }
    return -1;
}

// A slot this thread holds goes back: ready if its buffer is big enough, else for the service to fill.
void SequencerCore::releaseLaneSlot(LaneSlot& slot)
{
    slot.lane.clear();
    slot.state.store(slot.lane.points.capacity() >= (size_t) PressureLane::MAX_POINTS ? LaneSlot::Ready
                                                                                       : LaneSlot::Empty);
}

// A lane that is no longer wanted gives its buffer to the pool instead of freeing it here.
void SequencerCore::recycleLane(PressureLane& lane)
{
    if (lane.points.capacity() == 0)
        return;
    for (auto& slot : lanePool)
    {
        int expected = LaneSlot::Empty;
        if (!slot.state.compare_exchange_strong(expected, LaneSlot::Busy))
            continue;
        if (slot.lane.points.capacity() != 0)       // holds a small buffer the service will replace
        {
            slot.state.store(LaneSlot::Empty);
            continue;
        }
        slot.lane.points.swap(lane.points);
        releaseLaneSlot(slot);
        break;
    }
    lane.clear();      // (no free place in the pool: the buffer stays with the note until the note goes)
}

void SequencerCore::dropCapture(LaneCapture& c)
{
    if (!c.active)
        return;
    c.active = false;
    --activeCaptures;
    if (c.slot >= 0)
        releaseLaneSlot(lanePool[(size_t) c.slot]);
    c.slot = -1;
}

void SequencerCore::dropCapturesOn(int patternIndex, int trackIndex, int stepIndex, int pitch)
{
    if (activeCaptures <= 0)
        return;
    for (auto& c : captures)
        if (c.active && c.pattern == patternIndex && c.track == trackIndex && c.step == stepIndex
            && (pitch < 0 || c.pitch == pitch))
            dropCapture(c);
}

bool SequencerCore::beginPressureCapture(int pad, int trackIndex, int note)
{
    if (pad < 0 || pad >= MAX_PRESSURE_CAPTURES)
        return false;
    auto& c = captures[(size_t) pad];
    dropCapture(c);
    if (!lastRecord.valid || lastRecord.track != trackIndex || lastRecord.pitch != note)
        return false;

    const int slot = claimLaneSlot();
    if (slot < 0)
        return false;       // no buffer ready: this note is recorded without its pressure
    lanePool[(size_t) slot].lane.clear();

    c.active = true;
    c.pattern = lastRecord.pattern;
    c.track = lastRecord.track;
    c.step = lastRecord.step;
    c.pitch = lastRecord.pitch;
    c.visit = lastRecord.visit;
    c.slot = slot;
    c.epoch = captureEpoch.load(std::memory_order_relaxed);
    c.elapsed = 0.0;
    c.pressure = 0.0f;
    c.peak = 0.0f;
    ++activeCaptures;
    return true;
}

void SequencerCore::setCapturePressure(int pad, float pressure)
{
    if (pad < 0 || pad >= MAX_PRESSURE_CAPTURES)
        return;
    auto& c = captures[(size_t) pad];
    if (!c.active)
        return;
    c.pressure = juce::jlimit(0.0f, 1.0f, pressure);
    c.peak = juce::jmax(c.peak, c.pressure);
}

void SequencerCore::endPressureCapture(int pad, bool keep)
{
    if (pad < 0 || pad >= MAX_PRESSURE_CAPTURES)
        return;
    auto& c = captures[(size_t) pad];
    if (!c.active)
        return;

    auto& recorded = lanePool[(size_t) c.slot].lane;
    keep = keep && c.peak >= MIN_LANE_PEAK && c.epoch == captureEpoch.load(std::memory_order_relaxed)
           && c.pattern >= 0 && c.pattern < MAX_PATTERNS && c.track >= 0
           && c.track < patterns[(size_t) c.pattern].getTrackCount();
    if (keep)
    {
        auto& track = patterns[(size_t) c.pattern].getTrack(c.track);
        Note* note = nullptr;
        if (c.step >= 0 && c.step < (int) track.steps.size() && track.steps[(size_t) c.step].trig)
            for (auto& n : track.steps[(size_t) c.step].notes)
                if (n.pitch == c.pitch)
                    note = &n;
        if (note != nullptr)
        {
            // The lane ends where the pad was let go.
            if ((double) recorded.lengthInSteps() < c.elapsed || recorded.points.size() < 2)
                recorded.append((uint8_t) std::lround(c.pressure * 255.0f));

            // The note takes the recorded buffer; whatever it had goes back to the pool with the slot.
            note->lane.points.swap(recorded.points);
            note->lane.shift = recorded.shift;

            // The note lasts as long as the pad was held (the longest of the notes that landed on
            // the step together).
            auto& step = track.steps[(size_t) c.step];
            const float held = juce::jlimit(0.25f, (float) track.length, (float) c.elapsed);
            const size_t t = (size_t) juce::jlimit(0, MAX_TRACKS - 1, c.track);
            const bool samePass = laneLengthStep[t] == c.step && laneLengthVisit[t] == c.visit;
            step.length = samePass ? juce::jmax(step.length, held) : held;
            laneLengthStep[t] = c.step;
            laneLengthVisit[t] = c.visit;
        }
    }
    dropCapture(c);
}

void SequencerCore::refreshLanePressure(int trackIndex)
{
    if (activeLanePlayers <= 0)
        return;
    for (auto& p : lanePlayers)
        if (p.active && p.track == trackIndex)
            p.lastSent = -1.0f;
}

void SequencerCore::sendLanePressure(LanePlayer& p, float value)
{
    p.lastSent = value;
    if (onLanePressure)
        onLanePressure(p.track, p.pitch, value);
}

// The note a player follows, if it is still there with its lane.
const Note* SequencerCore::laneNote(const LanePlayer& p, float& scale) const
{
    if (p.pattern < 0 || p.pattern >= MAX_PATTERNS || p.track < 0
        || p.track >= patterns[(size_t) p.pattern].getTrackCount())
        return nullptr;
    const auto& track = patterns[(size_t) p.pattern].getTrack(p.track);
    if (p.step < 0 || p.step >= (int) track.steps.size())
        return nullptr;
    const auto& step = track.steps[(size_t) p.step];
    if (!step.trig || step.pressureScale <= 0.0f)
        return nullptr;
    scale = step.pressureScale;
    for (auto& n : step.notes)
        if (n.pitch == p.pitch && !n.lane.empty())
            return &n;
    return nullptr;
}

// A step has just sounded: each of its notes that carries pressure gets a player, and a player
// still running on one of its pitches (that note was cut short by this one) ends. A step without
// lanes, with no lane playing, costs one test per note.
void SequencerCore::startLanePlayers(int trackIndex, int stepIndex, const Step& step)
{
    for (auto& note : step.notes)
    {
        const bool wanted = !note.lane.empty() && step.pressureScale > 0.0f;
        if (!wanted && activeLanePlayers == 0)
            continue;

        LanePlayer* player = nullptr;
        for (auto& p : lanePlayers)
            if (p.active && p.track == trackIndex && p.pitch == note.pitch)
                player = &p;
        if (!wanted)
        {
            if (player != nullptr)
                endLanePlayer(trackIndex, note.pitch);
            continue;
        }
        if (player == nullptr)
        {
            for (auto& p : lanePlayers)
                if (!p.active) { player = &p; break; }
            if (player == nullptr)
                continue;       // all players busy: this note plays without its pressure
            player->active = true;
            player->lastSent = 0.0f;
            ++activeLanePlayers;
        }
        else if (player->lastSent != 0.0f)
            player->lastSent = -1.0f;       // the new voice has heard nothing yet
        player->pattern = activePattern;
        player->track = trackIndex;
        player->step = stepIndex;
        player->pitch = note.pitch;
        // The rest of this block is added at the block's end.
        const double trackStepLen = cachedSamplesPerStep / patterns[activePattern].getTrack(trackIndex).speedMultiplier;
        player->elapsed = trackStepLen > 0.0 ? -(double) blockSample / trackStepLen : 0.0;
    }
}

void SequencerCore::endLanePlayer(int trackIndex, int pitch)
{
    for (auto& p : lanePlayers)
        if (p.active && p.track == trackIndex && p.pitch == pitch)
        {
            p.active = false;
            --activeLanePlayers;
            if (p.lastSent != 0.0f)
                sendLanePressure(p, 0.0f);
        }
}

// Once per block: recorded lanes move on in musical time and send their value; lanes being
// recorded take one more reading of their pad.
void SequencerCore::advancePressureLanes(int numSamples, double samplesPerStepNow)
{
    auto stepsIn = [&](int patternIndex, int trackIndex) -> double
    {
        if (patternIndex < 0 || patternIndex >= MAX_PATTERNS || trackIndex < 0
            || trackIndex >= patterns[(size_t) patternIndex].getTrackCount() || samplesPerStepNow <= 0.0)
            return 0.0;
        return (double) numSamples * (double) patterns[(size_t) patternIndex].getTrack(trackIndex).speedMultiplier
               / samplesPerStepNow;
    };

    if (activeLanePlayers > 0)
        for (auto& p : lanePlayers)
        {
            if (!p.active)
                continue;
            p.elapsed += stepsIn(p.pattern, p.track);
            float scale = 1.0f;
            const Note* note = laneNote(p, scale);
            if (note == nullptr || !isTrackAudible(p.track))
            {
                p.active = false;
                --activeLanePlayers;
                if (p.lastSent != 0.0f)
                    sendLanePressure(p, 0.0f);
                continue;
            }
            const float value = juce::jlimit(0.0f, 1.0f, note->lane.valueAt((float) p.elapsed) * scale);
            if (p.lastSent < 0.0f || std::abs(value - p.lastSent) >= 1.0f / 254.0f
                || (value != p.lastSent && (value == 0.0f || value == 1.0f)))
                sendLanePressure(p, value);
        }

    if (activeCaptures > 0)
    {
        const uint32_t epoch = captureEpoch.load(std::memory_order_relaxed);
        for (int pad = 0; pad < MAX_PRESSURE_CAPTURES; ++pad)
        {
            auto& c = captures[(size_t) pad];
            if (!c.active)
                continue;
            if (c.epoch != epoch)
            {
                dropCapture(c);
                continue;
            }
            auto& lane = lanePool[(size_t) c.slot].lane;
            const auto value = (uint8_t) std::lround(c.pressure * 255.0f);
            // The point at the note's start, then one for every spacing the note has lived through.
            if (lane.points.empty())
                lane.append(value);
            c.elapsed += stepsIn(c.pattern, c.track);
            while ((double) lane.points.size() * (double) lane.stepsPerPoint() <= c.elapsed)
                if (!lane.append(value))
                    break;

            // One pass of the track is as long as a note can be.
            const int trackLength = (c.track < patterns[(size_t) c.pattern].getTrackCount())
                                        ? patterns[(size_t) c.pattern].getTrack(c.track).length : 1;
            if (c.elapsed >= (double) trackLength)
                endPressureCapture(pad, true);
        }
    }
}

void SequencerCore::scheduleNoteOff(int trackIndex, int pitch, double durationSamples)
{
    for (auto& noff : pendingNoteOffs)
    {
        if (noff.active && noff.trackIndex == trackIndex && noff.pitch == pitch)
        {
            noff.active = false;
            --activeNoteOffCount;
        }
    }

    for (auto& noff : pendingNoteOffs)
    {
        if (!noff.active)
        {
            noff.trackIndex = trackIndex;
            noff.pitch = pitch;
            noff.remainingSamples = durationSamples;
            noff.active = true;
            ++activeNoteOffCount;
            return;
        }
    }
}

float SequencerCore::getEffectiveMicroTiming(const Step& step, int stepIndex) const
{
    float rawMT = step.microTiming;

    float swingTarget = 0.0f;
    if (stepIndex % 2 == 1 && patterns[activePattern].swing > 0.0f)
        swingTarget = patterns[activePattern].swing * 0.5f;

    float quantizedTarget = swingTarget;

    return rawMT + quantizeAmount * (quantizedTarget - rawMT);
}

bool SequencerCore::evaluateTrigCondition(const TrigConditionData& cond, int trackIndex, int stepIndex) const
{
    if (stepIndex < 0)
        stepIndex = currentSteps[trackIndex];

    switch (cond.type)
    {
        case TrigCondition::Always:
            return true;

        case TrigCondition::Probability:
        {
            auto& rng = juce::Random::getSystemRandom();
            return rng.nextFloat() < cond.probability;
        }

        case TrigCondition::A_B:
        {
            if (cond.b <= 0) return true;
            return (trigCounts[trackIndex] % cond.b) < cond.a;
        }

        case TrigCondition::Fill:
            return isFillActiveForTrack(trackIndex);

        case TrigCondition::NotFill:
            return !isFillActiveForTrack(trackIndex);

        case TrigCondition::First:
            return stepIndex == 0;

        case TrigCondition::NotFirst:
            return stepIndex != 0;

        case TrigCondition::PrevTrig:
            return prevTrigFired[trackIndex];

        case TrigCondition::NotPrevTrig:
            return !prevTrigFired[trackIndex];
    }

    return true;
}

bool SequencerCore::isFillActiveForTrack(int trackIndex) const
{
    if (fillActive.load())
        return true;
    if (trackIndex >= 0 && trackIndex < MAX_TRACKS)
        return trackFillFlags[(size_t) trackIndex].load();
    return false;
}

void SequencerCore::setTrackFill(int trackIdx, bool active)
{
    if (trackIdx >= 0 && trackIdx < MAX_TRACKS)
        trackFillFlags[(size_t) trackIdx].store(active);
}

bool SequencerCore::isTrackFillActive(int trackIdx) const
{
    if (trackIdx >= 0 && trackIdx < MAX_TRACKS)
        return trackFillFlags[(size_t) trackIdx].load();
    return false;
}

bool SequencerCore::isStepActiveDuringFill(int trackIdx, int stepIdx) const
{
    auto& pat = patterns[activePattern];
    if (trackIdx < 0 || trackIdx >= pat.getTrackCount())
        return false;

    auto& track = pat.getTrack(trackIdx);
    if (stepIdx < 0 || stepIdx >= track.length)
        return false;

    auto& step = track.getStep(stepIdx);
    if (!step.trig)
        return false;

    return evaluateTrigCondition(step.trigCondition, trackIdx);
}
