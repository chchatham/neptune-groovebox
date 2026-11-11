#pragma once

#include "../engine/EngineBase.h"
#include <juce_core/juce_core.h>
#include <array>

enum class ModSource
{
    LFO1, LFO2, LFO3, LFO4, LFO5,
    Env1, Env2, Env3,
    Velocity, Aftertouch, ModWheel, PitchBend,
    KeyTrack, Random, Constant
};

struct ModSlot
{
    ModSource source = ModSource::LFO1;
    ParamId destination = 0;
    float depth = 0.0f;
    bool bipolar = true;
    bool active = false;
};

class ModMatrix
{
public:
    static constexpr int MAX_SLOTS = 32;

    void addSlot(const ModSlot& slot)
    {
        if (slotCount >= MAX_SLOTS) return;
        slots[slotCount] = slot;
        slots[slotCount].active = true;
        ++slotCount;
    }

    void removeSlot(int index)
    {
        if (index < 0 || index >= slotCount) return;
        for (int i = index; i < slotCount - 1; ++i)
            slots[i] = slots[i + 1];
        slots[slotCount - 1] = ModSlot{};
        --slotCount;
    }

    void clearAll()
    {
        for (auto& s : slots) s.active = false;
        slotCount = 0;
    }

    int getSlotCount() const { return slotCount; }

    ModSlot& getSlot(int index) { return slots[index]; }
    const ModSlot& getSlot(int index) const { return slots[index]; }

    void setSourceValue(ModSource source, float value)
    {
        sourceValues[(size_t) source] = value;
    }

    float getSourceValue(ModSource source) const
    {
        return sourceValues[(size_t) source];
    }

    float getModulatedValue(ParamId dest, float baseValue) const
    {
        float total = baseValue;
        for (int i = 0; i < slotCount; ++i)
        {
            auto& slot = slots[i];
            if (slot.active && slot.destination == dest)
            {
                float srcVal = getSourceValue(slot.source);
                if (!slot.bipolar)
                    srcVal = (srcVal + 1.0f) * 0.5f;
                total += srcVal * slot.depth;
            }
        }
        return total;
    }

    void process(ParamMap& params) const
    {
        for (int i = 0; i < slotCount; ++i)
        {
            auto& slot = slots[i];
            if (!slot.active) continue;

            float srcVal = getSourceValue(slot.source);
            if (!slot.bipolar)
                srcVal = (srcVal + 1.0f) * 0.5f;

            auto it = params.find(slot.destination);
            if (it != params.end())
                it->second += srcVal * slot.depth;
        }
    }

private:
    std::array<ModSlot, MAX_SLOTS> slots {};
    int slotCount = 0;
    static constexpr int NUM_SOURCES = 15;
    std::array<float, NUM_SOURCES> sourceValues {};
};
