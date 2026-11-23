#pragma once

#include "AftertouchRouter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <functional>

enum class VoiceMode
{
    Mono,
    Duo,
    Poly,
    Unison
};

struct Voice
{
    int note = -1;
    float velocity = 0.0f;
    float pressure = 0.0f;
    bool active = false;
    bool sustained = false;
    int age = 0;
};

class VoiceAllocator
{
public:
    explicit VoiceAllocator(int maxVoices = 8);

    void setVoiceCount(int count);
    int getVoiceCount() const { return (int) voices.size(); }

    // VOX: how many of the voices notes are given to, 1 .. getVoiceCount() (all of them until
    // set; setVoiceCount sets it back to all). Every search for a voice stays within the first
    // `limit`, so no more than that many ever sound once the tails of a lowered limit have rung
    // out: at the limit a new note takes the oldest voice, a note struck again its own. Lowering
    // it releases the held voices past the new limit (a note-off: they ring out, they are not cut).
    void setVoiceLimit(int limit);
    int getVoiceLimit() const { return limit; }
    void setMode(VoiceMode mode);
    VoiceMode getMode() const { return mode; }

    int noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff();

    void setAftertouch(int note, float pressure);
    float getAftertouch(int voiceIndex) const;

    AftertouchRouter& getAftertouchRouter() { return aftertouchRouter; }
    const AftertouchRouter& getAftertouchRouter() const { return aftertouchRouter; }

    AftertouchRouter::ModValues getAftertouchModulation(int voiceIndex) const
    {
        return aftertouchRouter.computeModulation(getAftertouch(voiceIndex));
    }

    const Voice& getVoice(int index) const { return voices[index]; }
    int getActiveVoiceCount() const;

    using VoiceCallback = std::function<void(int voiceIndex, const Voice& voice)>;
    void setNoteOnCallback(VoiceCallback cb) { onNoteOn = std::move(cb); }
    void setNoteOffCallback(VoiceCallback cb) { onNoteOff = std::move(cb); }

    // Optional (poly mode): says whether a released voice can still be heard (its release tail).
    // With it a new note goes to a silent voice while there is one, so a tail rings out instead
    // of being restarted under the new note (a click). Without it nothing changes.
    void setVoiceSoundingQuery(std::function<bool(int voiceIndex)> query) { isSounding = std::move(query); }

private:
    int findFreeVoice() const;
    int findQuietVoice() const;
    int findOldestVoice() const;
    int findVoiceForNote(int note) const;

    std::vector<Voice> voices;
    int limit = 0;              // the voices notes are given to: 0 .. limit - 1
    VoiceMode mode = VoiceMode::Poly;
    int ageCounter = 0;

    VoiceCallback onNoteOn;
    VoiceCallback onNoteOff;
    std::function<bool(int)> isSounding;

    AftertouchRouter aftertouchRouter;
};
