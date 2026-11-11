#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

class VariationMacros
{
public:
    void setSpice(float v)          { spice = clamp01(v); }
    void setDice(float v)           { dice = clamp01(v); }
    void setDensity(float v)        { density = clamp01(v); }
    void setOctaveScatter(float v)  { octaveScatter = clamp01(v); }
    void setRatchetChance(float v)  { ratchetChance = clamp01(v); }
    void setGateSpread(float v)     { gateSpread = clamp01(v); }
    void setHumanize(float v)       { humanize = clamp01(v); }

    float getSpice() const          { return spice; }
    float getDice() const           { return dice; }
    float getDensity() const        { return density; }
    float getOctaveScatter() const  { return octaveScatter; }
    float getRatchetChance() const  { return ratchetChance; }
    float getGateSpread() const     { return gateSpread; }
    float getHumanize() const       { return humanize; }

    struct StepModification
    {
        int   noteOffset    = 0;
        float velocityScale = 1.0f;
        float gateScale     = 1.0f;
        float timingOffset  = 0.0f;
        bool  skip          = false;
        int   ratchetCount  = 1;
        int   extraNotes    = 0;
        float extraVelocity = 0.6f;   // level of inserted notes relative to the step's own note
    };

    StepModification apply(int stepIndex, int totalSteps, uint32_t seed) const
    {
        StepModification mod;
        uint32_t rng = mixSeed(seed, static_cast<uint32_t>(stepIndex));

        if (density < 0.5f)
        {
            float keepProb = density * 2.0f;
            int period = 4;
            if (keepProb > 0.75f)
                period = 1;
            else if (keepProb > 0.5f)
                period = 2;

            bool isOnGrid = (stepIndex % period == 0);
            if (!isOnGrid)
            {
                float r = nextFloat(rng);
                if (r > keepProb)
                    mod.skip = true;
            }
        }
        else if (density > 0.5f)
        {
            float ghostProb = (density - 0.5f) * 2.0f;
            float r = nextFloat(rng);
            if (r < ghostProb)
            {
                // A ghost note: quieter than the step's own note, which keeps its level.
                mod.extraNotes = 1;
                mod.extraVelocity = 0.5f;
            }
        }

        if (mod.skip)
            return mod;

        if (spice > 0.0f)
        {
            float r = nextFloat(rng);
            if (r < spice)
                mod.extraNotes += 1;
        }

        if (dice > 0.0f)
        {
            float r = nextFloat(rng);
            if (r < dice)
            {
                static constexpr int intervals[] = { -7, -5, -4, -3, -2, 0, 2, 3, 4, 5, 7 };
                static constexpr int numIntervals = 11;
                int idx = static_cast<int>(nextFloat(rng) * numIntervals);
                if (idx >= numIntervals) idx = numIntervals - 1;
                mod.noteOffset = intervals[idx];
            }
        }

        if (octaveScatter > 0.0f)
        {
            float r = nextFloat(rng);
            if (r < octaveScatter)
            {
                float dir = nextFloat(rng);
                float mag = nextFloat(rng);
                int octaves = (mag < 0.5f) ? 1 : 2;
                int semitones = octaves * 12;
                mod.noteOffset += (dir < 0.5f) ? -semitones : semitones;
            }
        }

        if (ratchetChance > 0.0f)
        {
            float r = nextFloat(rng);
            if (r < ratchetChance)
            {
                float which = nextFloat(rng);
                mod.ratchetCount = (which < 0.6f) ? 2 : 3;
            }
        }

        if (gateSpread > 0.0f)
        {
            float r = nextFloat(rng);
            float spread = (r - 0.5f) * 2.0f;
            float scale = 1.0f + spread * gateSpread * 0.75f;
            mod.gateScale = std::max(0.25f, std::min(2.0f, scale));
        }

        if (humanize > 0.0f)
        {
            float r1 = nextFloat(rng);
            mod.timingOffset = (r1 - 0.5f) * 0.16f * humanize;
            mod.timingOffset = std::max(-0.5f, std::min(0.5f, mod.timingOffset));

            float r2 = nextFloat(rng);
            float velVar = 1.0f + (r2 - 0.5f) * 0.4f * humanize;
            mod.velocityScale *= std::max(0.1f, std::min(1.5f, velVar));
        }

        (void) totalSteps;
        return mod;
    }

    void reset() {}

private:
    float spice         = 0.0f;
    float dice          = 0.0f;
    float density       = 0.5f;
    float octaveScatter = 0.0f;
    float ratchetChance = 0.0f;
    float gateSpread    = 0.0f;
    float humanize      = 0.0f;

    static float clamp01(float v)
    {
        return std::max(0.0f, std::min(1.0f, v));
    }

    static uint32_t mixSeed(uint32_t seed, uint32_t step)
    {
        uint32_t s = seed ^ (step * 2654435761u);
        if (s == 0) s = 1;
        return s;
    }

    static float nextFloat(uint32_t& state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state & 0x7FFFFFFF) / static_cast<float>(0x7FFFFFFF);
    }
};
