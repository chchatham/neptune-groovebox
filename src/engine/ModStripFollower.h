#pragma once

#include "EngineBase.h"
#include <array>
#include <atomic>

// -----------------------------------------------------------------------------
// The mod strip holds its position (it does not spring back), and there is one
// strip for all the tracks. The rule this class keeps (owner's choice, 2026-11-11):
//
//   the strip moves the selected track only, and every track keeps the value
//   the strip last gave it.
//
// So a track that is left stays as it was set, selecting a track changes nothing
// until the strip is touched, and an engine that is rebuilt (a preset or project
// load, an engine change) is given its track's value again within one refresh.
//
// moved() is called from the MIDI thread when the strip sends a value for the
// engine; follow() from the 30 Hz refresh. Both only store numbers and call
// EngineBase::setModWheel, which engines read once per audio block.
// -----------------------------------------------------------------------------
class ModStripFollower
{
public:
    static constexpr int MAX_TRACKS = 64;   // tracks beyond this are played but not remembered

    // The strip moved, and the value is meant for the selected track's engine (an arpeggiator
    // that takes the strip for one of its macros does not call this).
    void moved(EngineBase* engine, int selectedTrack, float value)
    {
        const float v = juce::jlimit(0.0f, 1.0f, value);
        if (selectedTrack >= 0 && selectedTrack < MAX_TRACKS)
            values[(size_t) selectedTrack].store(v);
        if (engine != nullptr)
            engine->setModWheel(v);
    }

    // engineOf(track) returns that track's engine or null. Gives every engine its track's value
    // back if it has lost it (a rebuilt engine starts at rest).
    template <typename EngineOf>
    void follow(EngineOf&& engineOf)
    {
        for (int t = 0; t < MAX_TRACKS; ++t)
        {
            EngineBase* e = engineOf(t);
            if (e == nullptr)
                continue;
            const float v = values[(size_t) t].load();
            if (e->getModWheelPosition() != v)
                e->setModWheel(v);
        }
    }

    float getValue(int track) const
    {
        return (track >= 0 && track < MAX_TRACKS) ? values[(size_t) track].load() : 0.0f;
    }

    // A new or loaded project starts with every track at rest.
    void reset()
    {
        for (auto& v : values)
            v.store(0.0f);
    }

private:
    std::array<std::atomic<float>, MAX_TRACKS> values {};   // what each track's engine should have
};
