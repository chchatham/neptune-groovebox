#pragma once

#include "DrumKit.h"
#include <vector>

// Kit preset management: save/recall full kit configurations
struct KitPreset
{
    DrumKit kit;
    bool isFactory = false;

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kit", kit.toVar());
        obj->setProperty("isFactory", isFactory);
        return juce::var(obj);
    }

    static KitPreset fromVar(const juce::var& v)
    {
        KitPreset preset;
        if (auto* obj = v.getDynamicObject())
        {
            preset.kit = DrumKit::fromVar(obj->getProperty("kit"));
            preset.isFactory = (bool) obj->getProperty("isFactory");
        }
        return preset;
    }

    juce::String toJSON() const { return juce::JSON::toString(toVar()); }
    static KitPreset fromJSON(const juce::String& json) { return fromVar(juce::JSON::parse(json)); }
};

// Helper to build a DrumSlot with named parameters
inline DrumSlot makeSlot(DrumSynthType type, float tune, float decay, float tone,
                         float drive, float snap, float level, float pan = 0.5f,
                         int chokeGroup = 0, DrumATDest atDest = DrumATDest::Off,
                         float atDepth = 1.0f)
{
    DrumSlot s;
    s.synthType = type;
    s.tuning = 0.0f;
    s.params[0] = tune;
    s.params[1] = decay;
    s.params[2] = tone;
    s.params[3] = drive;
    s.params[4] = snap;
    s.params[5] = level;
    s.params[6] = pan;
    s.params[7] = 0.0f;
    s.chokeGroup = chokeGroup;
    s.aftertouchDest = atDest;
    s.aftertouchDepth = atDepth;
    return s;
}

// Factory kit definitions
namespace FactoryKits
{

inline DrumKit make808()
{
    DrumKit kit;
    kit.name = "808";

    //                              type               tune     dcy   tone  drv   snap  lvl   pan   choke atDest           atDep
    kit.slots[0]  = makeSlot(DrumSynthType::AnalogKick,    45.0f, 0.8f, 0.4f, 0.0f, 0.6f, 0.9f, 0.5f, 0);
    kit.slots[1]  = makeSlot(DrumSynthType::AnalogKick,    52.0f, 0.6f, 0.5f, 0.1f, 0.5f, 0.85f,0.5f, 0);
    kit.slots[2]  = makeSlot(DrumSynthType::NoiseSnare,   180.0f, 0.25f,0.4f, 0.2f, 0.6f, 0.8f, 0.5f, 0, DrumATDest::Decay, 0.5f);
    kit.slots[3]  = makeSlot(DrumSynthType::Clap,        1000.0f, 0.3f, 0.5f, 0.4f, 0.5f, 0.75f,0.55f,0);
    kit.slots[4]  = makeSlot(DrumSynthType::Tom,          120.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.7f, 0.35f,0, DrumATDest::Pitch, 0.3f);
    kit.slots[5]  = makeSlot(DrumSynthType::Tom,           90.0f, 0.35f,0.5f, 0.0f, 0.5f, 0.7f, 0.45f,0, DrumATDest::Pitch, 0.3f);
    kit.slots[6]  = makeSlot(DrumSynthType::Tom,           70.0f, 0.4f, 0.5f, 0.0f, 0.5f, 0.7f, 0.6f, 0, DrumATDest::Pitch, 0.3f);
    kit.slots[7]  = makeSlot(DrumSynthType::Rim,          800.0f, 0.01f,0.3f, 0.0f, 0.5f, 0.65f,0.5f, 0);
    kit.slots[8]  = makeSlot(DrumSynthType::ClosedHat,   8000.0f, 0.04f,0.5f, 0.0f, 0.5f, 0.6f, 0.6f, 1);
    kit.slots[9]  = makeSlot(DrumSynthType::ClosedHat,   8500.0f, 0.08f,0.6f, 0.0f, 0.5f, 0.55f,0.6f, 1);
    kit.slots[10] = makeSlot(DrumSynthType::OpenHat,     8000.0f, 0.5f, 0.5f, 0.0f, 0.5f, 0.6f, 0.6f, 1);
    kit.slots[11] = makeSlot(DrumSynthType::Cymbal,      7000.0f, 1.0f, 0.6f, 0.0f, 0.5f, 0.5f, 0.35f,0);
    kit.slots[12] = makeSlot(DrumSynthType::Cymbal,      9000.0f, 1.5f, 0.7f, 0.0f, 0.5f, 0.5f, 0.65f,0);
    kit.slots[13] = makeSlot(DrumSynthType::Clave,       2500.0f, 0.02f,0.5f, 0.0f, 0.5f, 0.6f, 0.5f, 0);
    kit.slots[14] = makeSlot(DrumSynthType::Shaker,      6000.0f, 0.15f,0.5f, 0.2f, 0.5f, 0.5f, 0.55f,0);
    kit.slots[15] = makeSlot(DrumSynthType::SubBass,       36.0f, 0.1f, 0.5f, 0.0f, 0.5f, 0.85f,0.5f, 0, DrumATDest::Level, 0.7f);

    return kit;
}

inline DrumKit make909()
{
    DrumKit kit;
    kit.name = "909";

    kit.slots[0]  = makeSlot(DrumSynthType::FMKick,        48.0f, 0.7f, 0.5f, 0.3f, 0.7f, 0.9f, 0.5f, 0);
    kit.slots[1]  = makeSlot(DrumSynthType::FMKick,        55.0f, 0.5f, 0.6f, 0.4f, 0.6f, 0.85f,0.5f, 0);
    kit.slots[2]  = makeSlot(DrumSynthType::FMSnare,      200.0f, 0.2f, 0.5f, 0.3f, 0.6f, 0.8f, 0.5f, 0, DrumATDest::Decay, 0.5f);
    kit.slots[3]  = makeSlot(DrumSynthType::Clap,        1200.0f, 0.3f, 0.6f, 0.7f, 0.5f, 0.75f,0.55f,0);
    kit.slots[4]  = makeSlot(DrumSynthType::Tom,          150.0f, 0.25f,0.5f, 0.0f, 0.6f, 0.7f, 0.35f,0, DrumATDest::Pitch, 0.3f);
    kit.slots[5]  = makeSlot(DrumSynthType::Tom,          110.0f, 0.3f, 0.5f, 0.0f, 0.6f, 0.7f, 0.45f,0, DrumATDest::Pitch, 0.3f);
    kit.slots[6]  = makeSlot(DrumSynthType::Tom,           80.0f, 0.35f,0.5f, 0.0f, 0.6f, 0.7f, 0.6f, 0, DrumATDest::Pitch, 0.3f);
    kit.slots[7]  = makeSlot(DrumSynthType::Rim,          900.0f, 0.01f,0.4f, 0.1f, 0.5f, 0.65f,0.5f, 0);
    kit.slots[8]  = makeSlot(DrumSynthType::ClosedHat,   9000.0f, 0.03f,0.6f, 0.0f, 0.5f, 0.6f, 0.6f, 1);
    kit.slots[9]  = makeSlot(DrumSynthType::ClosedHat,   9500.0f, 0.06f,0.7f, 0.0f, 0.5f, 0.55f,0.6f, 1);
    kit.slots[10] = makeSlot(DrumSynthType::OpenHat,     9000.0f, 0.45f,0.6f, 0.0f, 0.5f, 0.6f, 0.6f, 1);
    kit.slots[11] = makeSlot(DrumSynthType::Cymbal,      8000.0f, 1.2f, 0.65f,0.0f, 0.5f, 0.5f, 0.3f, 0);
    kit.slots[12] = makeSlot(DrumSynthType::Cymbal,     10000.0f, 1.8f, 0.75f,0.0f, 0.5f, 0.5f, 0.7f, 0);
    kit.slots[13] = makeSlot(DrumSynthType::Clave,       2500.0f, 0.02f,0.5f, 0.0f, 0.5f, 0.6f, 0.5f, 0);
    kit.slots[14] = makeSlot(DrumSynthType::Shaker,      7000.0f, 0.12f,0.6f, 0.3f, 0.5f, 0.5f, 0.55f,0);
    kit.slots[15] = makeSlot(DrumSynthType::SubBass,       40.0f, 0.1f, 0.5f, 0.0f, 0.5f, 0.8f, 0.5f, 0, DrumATDest::Level, 0.7f);

    return kit;
}

inline DrumKit makeAcoustic()
{
    DrumKit kit;
    kit.name = "Acoustic";

    // Acoustic-style: moderate tuning, longer decays, less drive
    kit.slots[0]  = makeSlot(DrumSynthType::AnalogKick,    55.0f, 0.5f, 0.4f, 0.0f, 0.7f, 0.9f, 0.5f, 0);
    kit.slots[1]  = makeSlot(DrumSynthType::FMKick,        60.0f, 0.4f, 0.3f, 0.1f, 0.8f, 0.85f,0.5f, 0);
    kit.slots[2]  = makeSlot(DrumSynthType::NoiseSnare,   220.0f, 0.2f, 0.6f, 0.1f, 0.7f, 0.8f, 0.5f, 0, DrumATDest::Level, 0.6f);
    kit.slots[3]  = makeSlot(DrumSynthType::NoiseSnare,   280.0f, 0.15f,0.7f, 0.0f, 0.8f, 0.7f, 0.48f,0, DrumATDest::Level, 0.6f);
    kit.slots[4]  = makeSlot(DrumSynthType::Tom,          180.0f, 0.4f, 0.5f, 0.0f, 0.6f, 0.75f,0.3f, 0, DrumATDest::Pitch, 0.2f);
    kit.slots[5]  = makeSlot(DrumSynthType::Tom,          130.0f, 0.45f,0.5f, 0.0f, 0.6f, 0.75f,0.4f, 0, DrumATDest::Pitch, 0.2f);
    kit.slots[6]  = makeSlot(DrumSynthType::Tom,           90.0f, 0.5f, 0.5f, 0.0f, 0.6f, 0.75f,0.6f, 0, DrumATDest::Pitch, 0.2f);
    kit.slots[7]  = makeSlot(DrumSynthType::Rim,          700.0f, 0.01f,0.5f, 0.0f, 0.5f, 0.6f, 0.5f, 0);
    kit.slots[8]  = makeSlot(DrumSynthType::ClosedHat,   7500.0f, 0.05f,0.4f, 0.0f, 0.5f, 0.55f,0.6f, 1);
    kit.slots[9]  = makeSlot(DrumSynthType::ClosedHat,   8000.0f, 0.03f,0.5f, 0.0f, 0.5f, 0.5f, 0.6f, 1);
    kit.slots[10] = makeSlot(DrumSynthType::OpenHat,     7500.0f, 0.6f, 0.4f, 0.0f, 0.5f, 0.55f,0.6f, 1);
    kit.slots[11] = makeSlot(DrumSynthType::Cymbal,      6000.0f, 1.5f, 0.5f, 0.0f, 0.5f, 0.45f,0.3f, 0);
    kit.slots[12] = makeSlot(DrumSynthType::Cymbal,      8000.0f, 2.0f, 0.6f, 0.0f, 0.5f, 0.45f,0.7f, 0);
    kit.slots[13] = makeSlot(DrumSynthType::Clave,       2200.0f, 0.02f,0.4f, 0.0f, 0.5f, 0.55f,0.5f, 0);
    kit.slots[14] = makeSlot(DrumSynthType::Shaker,      5500.0f, 0.18f,0.4f, 0.1f, 0.5f, 0.45f,0.55f,0);
    kit.slots[15] = makeSlot(DrumSynthType::Clap,        1000.0f, 0.25f,0.5f, 0.3f, 0.5f, 0.65f,0.55f,0);

    return kit;
}

inline DrumKit makeElectronic()
{
    DrumKit kit;
    kit.name = "Electronic";

    // Electronic: more drive, FM synthesis, aggressive tones
    kit.slots[0]  = makeSlot(DrumSynthType::ModCyclesKick, 50.0f, 0.7f, 0.6f, 0.5f, 0.6f, 0.9f, 0.5f, 0, DrumATDest::Drive, 0.4f);
    kit.slots[1]  = makeSlot(DrumSynthType::FMKick,        42.0f, 0.9f, 0.7f, 0.6f, 0.5f, 0.85f,0.5f, 0, DrumATDest::Drive, 0.4f);
    kit.slots[2]  = makeSlot(DrumSynthType::FMSnare,      250.0f, 0.18f,0.7f, 0.5f, 0.6f, 0.8f, 0.5f, 0, DrumATDest::Filter, 0.6f);
    kit.slots[3]  = makeSlot(DrumSynthType::Clap,        1500.0f, 0.2f, 0.7f, 0.8f, 0.4f, 0.75f,0.55f,0);
    kit.slots[4]  = makeSlot(DrumSynthType::FMSnare,      300.0f, 0.15f,0.8f, 0.4f, 0.5f, 0.7f, 0.4f, 0, DrumATDest::Pitch, 0.5f);
    kit.slots[5]  = makeSlot(DrumSynthType::Tom,          100.0f, 0.3f, 0.6f, 0.3f, 0.5f, 0.7f, 0.5f, 0, DrumATDest::Pitch, 0.4f);
    kit.slots[6]  = makeSlot(DrumSynthType::ModCyclesKick, 65.0f, 0.5f, 0.8f, 0.7f, 0.4f, 0.7f, 0.6f, 0);
    kit.slots[7]  = makeSlot(DrumSynthType::Rim,         1100.0f, 0.01f,0.6f, 0.3f, 0.5f, 0.65f,0.5f, 0);
    kit.slots[8]  = makeSlot(DrumSynthType::ClosedHat,  10000.0f, 0.03f,0.7f, 0.0f, 0.5f, 0.6f, 0.6f, 1);
    kit.slots[9]  = makeSlot(DrumSynthType::ClosedHat,  11000.0f, 0.06f,0.8f, 0.1f, 0.5f, 0.55f,0.6f, 1);
    kit.slots[10] = makeSlot(DrumSynthType::OpenHat,    10000.0f, 0.4f, 0.7f, 0.1f, 0.5f, 0.6f, 0.6f, 1);
    kit.slots[11] = makeSlot(DrumSynthType::Cymbal,      9000.0f, 1.0f, 0.7f, 0.0f, 0.5f, 0.5f, 0.3f, 0);
    kit.slots[12] = makeSlot(DrumSynthType::Cymbal,     12000.0f, 0.8f, 0.8f, 0.0f, 0.5f, 0.5f, 0.7f, 0);
    kit.slots[13] = makeSlot(DrumSynthType::Clave,       3000.0f, 0.015f,0.6f,0.0f, 0.5f, 0.6f, 0.5f, 0);
    kit.slots[14] = makeSlot(DrumSynthType::Shaker,      8000.0f, 0.1f, 0.7f, 0.4f, 0.5f, 0.5f, 0.55f,0);
    kit.slots[15] = makeSlot(DrumSynthType::SubBass,       32.0f, 0.1f, 0.5f, 0.2f, 0.5f, 0.85f,0.5f, 0, DrumATDest::Level, 0.8f);

    return kit;
}

// Retrieve all factory kits
inline std::vector<DrumKit> getFactoryKits()
{
    return { make808(), make909(), makeAcoustic(), makeElectronic() };
}

// Find a factory kit by name (case-insensitive)
inline DrumKit getFactoryKit(const std::string& name)
{
    auto kits = getFactoryKits();
    for (auto& k : kits)
    {
        if (juce::String(k.name).compareIgnoreCase(juce::String(name)) == 0)
            return k;
    }
    // Return 808 as default fallback
    return make808();
}

} // namespace FactoryKits
