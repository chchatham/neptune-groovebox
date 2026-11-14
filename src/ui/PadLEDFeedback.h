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

    // Roughly what a palette colour looks like on a pad, 0..255 per channel (for the panel in the Mac window).
    struct RGB { int r, g, b; };
    inline RGB approximateRGB(int palette)
    {
        switch (palette)
        {
            case OFF:        return { 0, 0, 0 };
            case DIM_WHITE:  return { 70, 70, 70 };
            case WHITE:      return { 255, 255, 255 };
            case DIM_RED:    return { 80, 8, 4 };
            case RED:        return { 255, 24, 12 };
            case DIM_ORANGE: return { 84, 34, 0 };
            case ORANGE:     return { 255, 110, 0 };
            case DIM_YELLOW: return { 80, 68, 0 };
            case YELLOW:     return { 255, 220, 0 };
            case DIM_GREEN:  return { 6, 76, 10 };
            case GREEN:      return { 20, 255, 40 };
            case DIM_CYAN:   return { 0, 70, 76 };
            case CYAN:       return { 0, 230, 255 };
            case DIM_BLUE:   return { 6, 20, 90 };
            case BLUE:       return { 20, 70, 255 };
            case DIM_PURPLE: return { 44, 12, 88 };
            case PURPLE:     return { 150, 40, 255 };
            case DIM_PINK:   return { 84, 10, 50 };
            case PINK:       return { 255, 40, 160 };
            default:         return { 90, 90, 90 };
        }
    }

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
            case DrumSynthType::CymbalSynth:   return PadColors::DIM_CYAN;
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
