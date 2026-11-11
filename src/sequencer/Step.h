#pragma once

#include "../engine/EngineBase.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

enum class TrigCondition
{
    Always,
    Probability,   // with percentage
    A_B,           // first A of every B
    Fill,
    NotFill,
    First,
    NotFirst,
    PrevTrig,
    NotPrevTrig
};

struct TrigConditionData
{
    TrigCondition type = TrigCondition::Always;
    int a = 1;
    int b = 2;
    float probability = 1.0f;
};

// Pad pressure recorded with one note: points at a fixed musical spacing from the note's start,
// joined by straight lines on playback. Short notes get 8 points per step; when a lane is full
// (MAX_POINTS) its spacing doubles, so any note length fits: up to 32 steps at 1/8 step, 64 at 1/4,
// 128 at 1/2, 256 at one point per step, and so on. An empty lane is a note without pressure and
// owns no memory.
struct PressureLane
{
    static constexpr int MAX_POINTS = 256;
    static constexpr int BASE_POINTS_PER_STEP = 8;
    static constexpr int MAX_SHIFT = 12;

    std::vector<uint8_t> points;     // pressure 0..255; point i is at i * stepsPerPoint() steps
    uint8_t shift = 0;               // spacing: (1 << shift) / BASE_POINTS_PER_STEP steps

    bool empty() const { return points.empty(); }
    float stepsPerPoint() const { return (float) (1 << juce::jmin((int) shift, MAX_SHIFT)) / (float) BASE_POINTS_PER_STEP; }
    // From the first point to the last, in steps.
    float lengthInSteps() const { return points.size() < 2 ? 0.0f : (float) (points.size() - 1) * stepsPerPoint(); }

    // Keeps the memory (nothing is freed): for buffers that are used again.
    void clear() { points.clear(); shift = 0; }

    // Pressure 0..1 at `steps` after the note's start; the last point is held after the lane's end.
    float valueAt(float steps) const
    {
        const size_t n = points.size();
        if (n == 0) return 0.0f;
        const float x = steps / stepsPerPoint();
        if (x <= 0.0f) return (float) points[0] / 255.0f;
        if (x >= (float) (n - 1)) return (float) points[n - 1] / 255.0f;
        const size_t i = (size_t) x;
        const float f = x - (float) i;
        return ((float) points[i] * (1.0f - f) + (float) points[i + 1] * f) / 255.0f;
    }

    // Mean pressure 0..1 between two times (steps from the note's start).
    float meanOver(float fromSteps, float toSteps) const
    {
        if (points.empty()) return 0.0f;
        if (toSteps <= fromSteps) return valueAt(fromSteps);
        const int slices = juce::jlimit(4, 64, (int) std::ceil((toSteps - fromSteps) / stepsPerPoint()) * 2);
        float sum = 0.0f;
        for (int i = 0; i < slices; ++i)
            sum += valueAt(fromSteps + (toSteps - fromSteps) * ((float) i + 0.5f) / (float) slices);
        return sum / (float) slices;
    }

    float peak() const
    {
        uint8_t m = 0;
        for (auto p : points) m = juce::jmax(m, p);
        return (float) m / 255.0f;
    }

    // Adds a point without ever allocating: false when the buffer has no room reserved. A full
    // lane is first thinned to half its points at twice the spacing.
    bool append(uint8_t value)
    {
        if ((int) points.size() >= MAX_POINTS)
            halve();
        if (points.size() >= points.capacity())
            return false;
        points.push_back(value);
        return true;
    }

    void halve()
    {
        const size_t n = points.size();
        if (n < 2 || shift >= MAX_SHIFT) return;
        const size_t half = (n + 1) / 2;
        for (size_t i = 0; i < half; ++i)
        {
            const size_t c = 2 * i;
            const int before = points[c > 0 ? c - 1 : c], after = points[c + 1 < n ? c + 1 : c];
            points[i] = (uint8_t) ((before + 2 * (int) points[c] + after + 2) / 4);
        }
        points.resize(half);
        ++shift;
    }

    bool operator== (const PressureLane& o) const { return shift == o.shift && points == o.points; }
    bool operator!= (const PressureLane& o) const { return !(*this == o); }
};

struct Note
{
    int pitch = 60;
    float velocity = 1.0f;
    PressureLane lane;          // pad pressure over the note's life; empty = none
};

struct Step
{
    bool trig = false;
    std::vector<Note> notes;
    float velocity = 1.0f;
    float length = 1.0f;          // in steps
    float microTiming = 0.0f;     // -0.5 to +0.5 steps
    TrigConditionData trigCondition;
    ParamMap paramLocks;

    // Recorded pad pressure is kept per note (Note::lane). This scales what the step's notes play
    // back: 0 = none (the lanes stay), 1 = as recorded, up to 2.
    float pressureScale = 1.0f;

    int pageChangeLock = -1;      // -1 = no lock

    void clear()
    {
        trig = false;
        notes.clear();
        velocity = 1.0f;
        length = 1.0f;
        microTiming = 0.0f;
        trigCondition = {};
        paramLocks.clear();
        pressureScale = 1.0f;
        pageChangeLock = -1;
    }

    bool hasPressure() const
    {
        for (auto& n : notes)
            if (!n.lane.empty())
                return true;
        return false;
    }

    void clearPressureLanes()
    {
        for (auto& n : notes)
            n.lane = {};
        pressureScale = 1.0f;
    }

    bool hasParamLock(ParamId id) const
    {
        return paramLocks.find(id) != paramLocks.end();
    }

    float getParamLock(ParamId id, float defaultValue) const
    {
        auto it = paramLocks.find(id);
        return it != paramLocks.end() ? it->second : defaultValue;
    }
};
