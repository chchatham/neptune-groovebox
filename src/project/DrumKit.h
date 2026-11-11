#pragma once

#include "../engine/DrumVoice.h"
#include <juce_core/juce_core.h>
#include <array>
#include <string>

// Aftertouch destination for a drum slot
enum class DrumATDest
{
    Off = 0,
    Pitch,
    Decay,
    Filter,
    Level,
    Drive,
    Snap,
    Pan
};

static inline const char* drumATDestName(DrumATDest d)
{
    switch (d)
    {
        case DrumATDest::Off:    return "OFF";
        case DrumATDest::Pitch:  return "PIT";
        case DrumATDest::Decay:  return "DCY";
        case DrumATDest::Filter: return "FLT";
        case DrumATDest::Level:  return "LVL";
        case DrumATDest::Drive:  return "DRV";
        case DrumATDest::Snap:   return "SNP";
        case DrumATDest::Pan:    return "PAN";
        default:                 return "---";
    }
}

static constexpr int DRUM_AT_DEST_COUNT = 8;

// A single slot in a drum kit (one pad)
struct DrumSlot
{
    DrumSynthType synthType = DrumSynthType::FMKick;
    float tuning = 0.0f;           // semitones offset from default
    float params[8] = {};          // voice-specific: tune, decay, tone, drive, snap, level, pan, (reserved)
    DrumATDest aftertouchDest = DrumATDest::Off;
    float aftertouchDepth = 1.0f;  // 0..1 modulation depth
    int chokeGroup = 0;            // 0 = none, >0 = choke group id

    // Named param accessors (indices match DrumEngine::VoiceParam)
    float getTune() const   { return params[0]; }
    float getDecay() const  { return params[1]; }
    float getTone() const   { return params[2]; }
    float getDrive() const  { return params[3]; }
    float getSnap() const   { return params[4]; }
    float getLevel() const  { return params[5]; }
    float getPan() const    { return params[6]; }

    void setTune(float v)   { params[0] = v; }
    void setDecay(float v)  { params[1] = v; }
    void setTone(float v)   { params[2] = v; }
    void setDrive(float v)  { params[3] = v; }
    void setSnap(float v)   { params[4] = v; }
    void setLevel(float v)  { params[5] = v; }
    void setPan(float v)    { params[6] = v; }

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("synthType", static_cast<int>(synthType));
        obj->setProperty("tuning", (double) tuning);
        obj->setProperty("atDest", static_cast<int>(aftertouchDest));
        obj->setProperty("atDepth", (double) aftertouchDepth);
        obj->setProperty("chokeGroup", chokeGroup);

        juce::Array<juce::var> paramsArray;
        for (int i = 0; i < 8; ++i)
            paramsArray.add((double) params[i]);
        obj->setProperty("params", paramsArray);

        return juce::var(obj);
    }

    static DrumSlot fromVar(const juce::var& v)
    {
        DrumSlot slot;
        if (auto* obj = v.getDynamicObject())
        {
            slot.synthType = static_cast<DrumSynthType>((int) obj->getProperty("synthType"));
            slot.tuning = (float)(double) obj->getProperty("tuning");
            slot.aftertouchDest = static_cast<DrumATDest>((int) obj->getProperty("atDest"));
            slot.aftertouchDepth = (float)(double) obj->getProperty("atDepth");
            slot.chokeGroup = (int) obj->getProperty("chokeGroup");

            if (auto* arr = obj->getProperty("params").getArray())
            {
                for (int i = 0; i < std::min(8, arr->size()); ++i)
                    slot.params[i] = (float)(double) (*arr)[i];
            }
        }
        return slot;
    }
};

// A drum kit: 16 slots with a name
struct DrumKit
{
    static constexpr int NUM_SLOTS = 16;

    std::string name = "Init Kit";
    std::array<DrumSlot, NUM_SLOTS> slots;

    // Access
    DrumSlot& getSlot(int index) { return slots[static_cast<size_t>(index)]; }
    const DrumSlot& getSlot(int index) const { return slots[static_cast<size_t>(index)]; }

    // JSON serialization
    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("name", juce::String(name));

        juce::Array<juce::var> slotsArray;
        for (int i = 0; i < NUM_SLOTS; ++i)
            slotsArray.add(slots[static_cast<size_t>(i)].toVar());
        obj->setProperty("slots", slotsArray);

        return juce::var(obj);
    }

    static DrumKit fromVar(const juce::var& v)
    {
        DrumKit kit;
        if (auto* obj = v.getDynamicObject())
        {
            kit.name = obj->getProperty("name").toString().toStdString();

            if (auto* arr = obj->getProperty("slots").getArray())
            {
                for (int i = 0; i < std::min(NUM_SLOTS, arr->size()); ++i)
                    kit.slots[static_cast<size_t>(i)] = DrumSlot::fromVar((*arr)[i]);
            }
        }
        return kit;
    }

    juce::String toJSON() const
    {
        return juce::JSON::toString(toVar());
    }

    static DrumKit fromJSON(const juce::String& json)
    {
        auto parsed = juce::JSON::parse(json);
        return fromVar(parsed);
    }
};
