#pragma once

#include <atomic>
#include "SequencerCore.h"
#include "SongMode.h"

// Runs the song: when it is armed, Play starts at the first row and each time the pattern (track 1)
// wraps the song moves on, switching pattern and transposition.
class SongPlayer : public SequencerCore::Listener
{
public:
    SongMode* song = nullptr;
    SequencerCore* sequencer = nullptr;
    std::atomic<int> transpose { 0 };

    void transportStarted() override
    {
        if (!song || !sequencer || !song->isArmed() || song->getEntryCount() == 0)
            return;
        song->start();
        sequencer->selectPattern(song->getCurrentPatternIndex(), SequencerCore::ChangeMode::Immediate);
        transpose = song->getCurrentTranspose();
    }

    void transportStopped() override
    {
        if (song) song->stop();
        transpose = 0;
    }

    void stepAdvanced(int trackIndex, int stepIndex) override
    {
        if (!song || !sequencer || trackIndex != 0 || stepIndex != 0 || !song->isActive())
            return;
        // The sequencer reports a step only when a track moves on to it: the step the transport starts
        // on (and the first step after a pattern change) is not reported, so every report of step 1 on
        // track 1 is a wrap.
        if (song->advancePattern())
        {
            sequencer->selectPattern(song->getCurrentPatternIndex(), SequencerCore::ChangeMode::Immediate);
            transpose = song->getCurrentTranspose();
        }
    }
};
