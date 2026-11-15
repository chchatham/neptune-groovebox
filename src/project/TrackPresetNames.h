#pragma once

#include "../engine/EngineBase.h"
#include <juce_core/juce_core.h>
#include <map>

// The preset each track's sound came from: set when a preset is kept with LD and when a save is
// named, stored with the track in projects and in the session, and offered by the save box as the
// name to save under. The engine type is kept with it: a track whose engine has changed since no
// longer holds that preset's sound.
// Written on the MIDI thread (PRESET) and read on the message thread (saving a project): it has a
// lock of its own, and nothing is called while it is held.
class TrackPresetNames
{
public:
    struct Origin
    {
        juce::String name;                                  // empty = the sound came from no preset
        EngineType engine = EngineType::UnifiedVoice;
    };

    void set(int track, const juce::String& name, EngineType engine)
    {
        const juce::ScopedLock sl(lock);
        if (name.isEmpty())
            origins.erase(track);
        else
            origins[track] = { name, engine };
    }

    void clear(int track) { set(track, {}, EngineType::UnifiedVoice); }

    Origin get(int track) const
    {
        const juce::ScopedLock sl(lock);
        auto it = origins.find(track);
        return it != origins.end() ? it->second : Origin();
    }

    // The name for a track whose engine is `engine` now ("" when the engine is another one).
    juce::String nameFor(int track, EngineType engine) const
    {
        const auto o = get(track);
        return o.engine == engine ? o.name : juce::String();
    }

private:
    juce::CriticalSection lock;
    std::map<int, Origin> origins;
};
