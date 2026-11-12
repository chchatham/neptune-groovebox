#pragma once

#include "DrumEngine.h"
#include "../sequencer/Step.h"

// A drum track's step holds one note per drum that hits on it. STEP shows one drum at a time (the
// pad selected on DRUMS): these find that drum's note among the step's notes.
namespace DrumLane
{
    // The index of the note that plays this pad, or -1.
    inline int find(const Step& step, int pad)
    {
        for (size_t i = 0; i < step.notes.size(); ++i)
            if (DrumEngine::padForNote(step.notes[i].pitch) == pad)
                return (int) i;
        return -1;
    }

    // The pad hits on this step.
    inline bool plays(const Step& step, int pad) { return step.trig && find(step, pad) >= 0; }

    // Puts the pad on the step (a step that was off loses the notes it was left with). False if it was there.
    inline bool add(Step& step, int pad, float velocity = 1.0f)
    {
        if (!step.trig)
            step.notes.clear();
        const bool had = find(step, pad) >= 0;
        if (!had)
            step.notes.push_back({ pad, velocity });
        step.trig = true;
        return !had;
    }

    // Takes the pad off the step; the step goes off with its last drum.
    inline void remove(Step& step, int pad)
    {
        for (int i = find(step, pad); i >= 0; i = find(step, pad))
            step.notes.erase(step.notes.begin() + i);
        if (step.notes.empty())
            step.trig = false;
    }
}
