#include "VoiceAllocator.h"

VoiceAllocator::VoiceAllocator(int maxVoices)
{
    setVoiceCount(maxVoices);
}

void VoiceAllocator::setVoiceCount(int count)
{
    voices.resize(count);
    allNotesOff();
}

void VoiceAllocator::setMode(VoiceMode newMode)
{
    if (mode == newMode)
        return;

    mode = newMode;
    allNotesOff();
}

int VoiceAllocator::noteOn(int note, float velocity)
{
    int voiceIndex = -1;

    switch (mode)
    {
        case VoiceMode::Mono:
        {
            voiceIndex = 0;
            if (voices[0].active && onNoteOff)
                onNoteOff(0, voices[0]);
            break;
        }

        case VoiceMode::Duo:
        {
            voiceIndex = findFreeVoice();
            if (voiceIndex < 0)
                voiceIndex = findOldestVoice();
            if (voiceIndex >= 2)
                voiceIndex = findOldestVoice();
            break;
        }

        case VoiceMode::Unison:
        {
            for (int i = 0; i < (int) voices.size(); ++i)
            {
                voices[i].note = note;
                voices[i].velocity = velocity;
                voices[i].pressure = 0.0f;
                voices[i].active = true;
                voices[i].age = ageCounter++;
                if (onNoteOn)
                    onNoteOn(i, voices[i]);
            }
            return 0;
        }

        case VoiceMode::Poly:
        default:
        {
            int existing = findVoiceForNote(note);
            if (existing >= 0 && isSounding && ! voices[existing].active)
                existing = -1;      // released: its tail is left alone, the note gets a voice of its own
            if (existing >= 0)
            {
                voiceIndex = existing;
                if (voices[voiceIndex].active && onNoteOff)
                    onNoteOff(voiceIndex, voices[voiceIndex]);
            }
            else
            {
                voiceIndex = isSounding ? findQuietVoice() : findFreeVoice();
                if (voiceIndex < 0)
                    voiceIndex = findOldestVoice();
            }
            break;
        }
    }

    if (voiceIndex < 0 || voiceIndex >= (int) voices.size())
        return -1;

    if (voices[voiceIndex].active && onNoteOff)
        onNoteOff(voiceIndex, voices[voiceIndex]);

    voices[voiceIndex].note = note;
    voices[voiceIndex].velocity = velocity;
    voices[voiceIndex].pressure = 0.0f;
    voices[voiceIndex].active = true;
    voices[voiceIndex].age = ageCounter++;

    if (onNoteOn)
        onNoteOn(voiceIndex, voices[voiceIndex]);

    return voiceIndex;
}

void VoiceAllocator::noteOff(int note)
{
    if (mode == VoiceMode::Unison)
    {
        for (int i = 0; i < (int) voices.size(); ++i)
        {
            if (voices[i].active && voices[i].note == note)
            {
                voices[i].active = false;
                if (onNoteOff)
                    onNoteOff(i, voices[i]);
            }
        }
        return;
    }

    for (int i = 0; i < (int) voices.size(); ++i)
    {
        if (voices[i].active && voices[i].note == note)
        {
            voices[i].active = false;
            if (onNoteOff)
                onNoteOff(i, voices[i]);
            return;
        }
    }
}

void VoiceAllocator::allNotesOff()
{
    for (int i = 0; i < (int) voices.size(); ++i)
    {
        if (voices[i].active)
        {
            voices[i].active = false;
            if (onNoteOff)
                onNoteOff(i, voices[i]);
        }
        voices[i].note = -1;
        voices[i].velocity = 0.0f;
        voices[i].pressure = 0.0f;
        voices[i].age = 0;
    }
    ageCounter = 0;
}

void VoiceAllocator::setAftertouch(int note, float pressure)
{
    for (auto& v : voices)
    {
        if (v.active && v.note == note)
            v.pressure = pressure;
    }
}

float VoiceAllocator::getAftertouch(int voiceIndex) const
{
    if (voiceIndex < 0 || voiceIndex >= (int) voices.size())
        return 0.0f;
    return voices[voiceIndex].pressure;
}

int VoiceAllocator::getActiveVoiceCount() const
{
    int count = 0;
    for (auto& v : voices)
        if (v.active) ++count;
    return count;
}

int VoiceAllocator::findFreeVoice() const
{
    for (int i = 0; i < (int) voices.size(); ++i)
    {
        if (!voices[i].active)
            return i;
    }
    return -1;
}

// A released voice that is silent; failing that, the released voice that was started longest ago.
int VoiceAllocator::findQuietVoice() const
{
    int tail = -1;
    for (int i = 0; i < (int) voices.size(); ++i)
    {
        if (voices[i].active)
            continue;
        if (! isSounding(i))
            return i;
        if (tail < 0 || voices[i].age < voices[tail].age)
            tail = i;
    }
    return tail;
}

int VoiceAllocator::findOldestVoice() const
{
    int oldest = 0;
    int oldestAge = voices[0].age;

    for (int i = 1; i < (int) voices.size(); ++i)
    {
        if (voices[i].age < oldestAge)
        {
            oldest = i;
            oldestAge = voices[i].age;
        }
    }
    return oldest;
}

int VoiceAllocator::findVoiceForNote(int note) const
{
    for (int i = 0; i < (int) voices.size(); ++i)
    {
        if (voices[i].note == note)
            return i;
    }
    return -1;
}
