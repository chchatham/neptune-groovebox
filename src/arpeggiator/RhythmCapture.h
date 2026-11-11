#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>

class RhythmCapture
{
public:
    struct RhythmEvent
    {
        double timeSamples;
        float  velocity;
    };

    void startCapture(double sampleRate)
    {
        events.clear();
        capturing      = true;
        sampleRate_    = sampleRate;
        sampleCounter_ = 0.0;
    }

    void tapEvent(float velocity)
    {
        if (!capturing)
            return;

        events.push_back({ sampleCounter_, std::max(0.0f, std::min(1.0f, velocity)) });
    }

    void advanceSamples(double samples)
    {
        if (capturing)
            sampleCounter_ += samples;
    }

    void stopCapture()
    {
        capturing = false;
    }

    bool isCapturing() const { return capturing; }
    bool hasCapture() const { return !events.empty(); }
    int getEventCount() const { return static_cast<int>(events.size()); }
    const std::vector<RhythmEvent>& getEvents() const { return events; }

    void normalizeToStepLength(double stepLengthSamples)
    {
        if (events.size() < 2 || stepLengthSamples <= 0.0)
            return;

        double first = events.front().timeSamples;
        double last  = events.back().timeSamples;
        double span  = last - first;

        if (span <= 0.0)
        {
            for (auto& e : events)
                e.timeSamples = 0.0;
            return;
        }

        double scale = stepLengthSamples / span;
        for (auto& e : events)
            e.timeSamples = (e.timeSamples - first) * scale;
    }

private:
    std::vector<RhythmEvent> events;
    bool   capturing      = false;
    double sampleRate_    = 44100.0;
    double sampleCounter_ = 0.0;
};
