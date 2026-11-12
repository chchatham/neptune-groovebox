#pragma once

#include "../controller/PadManager.h"
#include "../sequencer/SequencerCore.h"
#include "../engine/DrumEngine.h"
#include "../engine/DrumLane.h"

namespace PadColors
{
    static constexpr int OFF = 0;
    static constexpr int DIM_WHITE = 1;
    static constexpr int WHITE = 3;
    static constexpr int DIM_RED = 5;
    static constexpr int RED = 7;
    static constexpr int DIM_ORANGE = 9;
    static constexpr int ORANGE = 11;
    static constexpr int DIM_YELLOW = 13;
    static constexpr int YELLOW = 15;
    static constexpr int DIM_GREEN = 17;
    static constexpr int GREEN = 19;
    static constexpr int DIM_CYAN = 33;
    static constexpr int CYAN = 35;
    static constexpr int DIM_BLUE = 41;
    static constexpr int BLUE = 43;
    static constexpr int DIM_PURPLE = 49;
    static constexpr int PURPLE = 51;
    static constexpr int DIM_PINK = 53;
    static constexpr int PINK = 55;

    static constexpr int STEP_EMPTY = DIM_WHITE;
    static constexpr int STEP_ACTIVE = YELLOW;
    static constexpr int STEP_LOCKED = CYAN;
    static constexpr int STEP_ACTIVE_LOCKED = GREEN;
    static constexpr int STEP_PLAYHEAD = RED;
    static constexpr int STEP_PLAYHEAD_ACTIVE = ORANGE;

    static constexpr int MUTE_ON = DIM_RED;
    static constexpr int MUTE_OFF = GREEN;
    static constexpr int SOLO_ON = BLUE;

    static constexpr int PLAY_ROOT = PURPLE;
    static constexpr int PLAY_SCALE = DIM_BLUE;
    static constexpr int PLAY_OFF_SCALE = DIM_WHITE;
}

class PadLEDFeedback
{
public:
    void setSequencer(SequencerCore* seq) { sequencer = seq; }
    void setDrumEngine(DrumEngine* engine) { drumEngine = engine; }

    int getStepColor(int padIndex, int pageOffset) const
    {
        if (!sequencer)
            return PadColors::STEP_EMPTY;

        int trackIdx = sequencer->getSelectedTrack();
        auto& track = sequencer->getPattern().getTrack(trackIdx);
        int stepIdx = pageOffset + padIndex;

        if (stepIdx >= track.length)
            return PadColors::OFF;

        auto& step = track.getStep(stepIdx);
        bool isPlayhead = sequencer->isPlaying()
                          && sequencer->getCurrentStep(trackIdx) == stepIdx;

        // A drum track shows the drum selected on DRUMS: lit where that drum hits, dim yellow
        // where only other drums of the kit do.
        const bool drums = track.engineType == EngineType::Drum && drumEngine != nullptr;
        const bool hit = drums ? DrumLane::plays(step, drumEngine->getSelectedVoice()) : step.trig;

        if (isPlayhead)
            return hit ? PadColors::STEP_PLAYHEAD_ACTIVE : PadColors::STEP_PLAYHEAD;

        bool hasLocks = !step.paramLocks.empty();

        if (hit && hasLocks)
            return PadColors::STEP_ACTIVE_LOCKED;
        if (hit)
            return PadColors::STEP_ACTIVE;
        if (hasLocks)
            return PadColors::STEP_LOCKED;
        if (step.trig)
            return PadColors::DIM_YELLOW;

        return PadColors::STEP_EMPTY;
    }

    int getMuteColor(int padIndex) const
    {
        if (!sequencer)
            return PadColors::OFF;

        auto& pattern = sequencer->getPattern();
        if (padIndex >= pattern.getTrackCount())
            return PadColors::OFF;

        auto& track = pattern.getTrack(padIndex);
        if (track.soloed)
            return PadColors::SOLO_ON;
        return track.muted ? PadColors::MUTE_ON : PadColors::MUTE_OFF;
    }

    // TRACKS screen layout: top row selects a track (selected one bright), bottom row shows and
    // toggles each track's mute / solo state.
    int getTracksScreenColor(int padIndex) const
    {
        if (!sequencer || padIndex < 0)
            return PadColors::OFF;

        const int trackCount = sequencer->getPattern().getTrackCount();
        if (padIndex < 8)
        {
            if (padIndex >= trackCount)
                return PadColors::OFF;
            return padIndex == sequencer->getSelectedTrack() ? PadColors::WHITE : PadColors::DIM_WHITE;
        }
        return getMuteColor(padIndex - 8);
    }

    int getDrumColor(int padIndex) const
    {
        if (!drumEngine || padIndex < 0 || padIndex >= DrumEngine::NUM_VOICES)
            return PadColors::DIM_WHITE;

        auto type = drumEngine->getVoice(padIndex).getType();
        switch (type)
        {
            case DrumSynthType::FMKick:       return PadColors::ORANGE;
            case DrumSynthType::AnalogKick:    return PadColors::RED;
            case DrumSynthType::ModCyclesKick: return PadColors::DIM_RED;
            case DrumSynthType::NoiseSnare:    return PadColors::YELLOW;
            case DrumSynthType::FMSnare:       return PadColors::DIM_YELLOW;
            case DrumSynthType::Clap:          return PadColors::PINK;
            case DrumSynthType::ClosedHat:     return PadColors::CYAN;
            case DrumSynthType::OpenHat:       return PadColors::BLUE;
            case DrumSynthType::Cymbal:        return PadColors::DIM_CYAN;
            case DrumSynthType::Tom:           return PadColors::DIM_ORANGE;
            case DrumSynthType::Rim:           return PadColors::WHITE;
            case DrumSynthType::Shaker:        return PadColors::DIM_GREEN;
            case DrumSynthType::Clave:         return PadColors::GREEN;
            case DrumSynthType::SubBass:       return PadColors::PURPLE;
            case DrumSynthType::KickSynth:     return PadColors::RED;
        }
        return PadColors::DIM_WHITE;
    }

    int getColor(int padIndex, PadManager::Mode mode, int pageOffset = 0) const
    {
        switch (mode)
        {
            case PadManager::Mode::Step:
                return getStepColor(padIndex, pageOffset);
            case PadManager::Mode::Mute:
                return getTracksScreenColor(padIndex);
            case PadManager::Mode::Play:
                if (drumEngine) return getDrumColor(padIndex);
                return PadColors::DIM_WHITE;
            case PadManager::Mode::CustomScale:
            case PadManager::Mode::Operator:
                return PadColors::DIM_WHITE;
        }
        return PadColors::OFF;
    }

private:
    SequencerCore* sequencer = nullptr;
    DrumEngine* drumEngine = nullptr;
};
