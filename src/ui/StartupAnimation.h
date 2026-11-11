#pragma once

#include "../controller/LaunchkeyController.h"
#include <juce_events/juce_events.h>
#include <cmath>
#include <functional>

class StartupAnimation : private juce::Timer
{
public:
    explicit StartupAnimation(LaunchkeyController& ctrl)
        : controller(ctrl) {}

    ~StartupAnimation() override { stopTimer(); }

    std::function<void()> onComplete;

    void start(int durationMs = 3000, int frameRateHz = 20)
    {
        totalFrames = (durationMs * frameRateHz) / 1000;
        frameCount = 0;
        running = true;
        startTimer(1000 / frameRateHz);
    }

    void stop()
    {
        stopTimer();
        running = false;
    }

    bool isRunning() const { return running; }

private:
    void timerCallback() override
    {
        if (frameCount >= totalFrames)
        {
            stopTimer();
            running = false;
            if (onComplete)
                onComplete();
            return;
        }

        float phase = static_cast<float>(frameCount) / static_cast<float>(totalFrames);

        for (int i = 0; i < 16; ++i)
        {
            float hue = std::fmod(phase * 2.0f + static_cast<float>(i) / 16.0f, 1.0f);
            int r, g, b;
            hsvToRgb(hue, 1.0f, 1.0f, r, g, b);

            int note = (i < 8)
                ? LaunchkeyController::PAD_TOP_ROW_START + i
                : LaunchkeyController::PAD_BOTTOM_ROW_START + (i - 8);
            controller.setPadRGB(note, r, g, b);
        }

        ++frameCount;
    }

    static void hsvToRgb(float h, float s, float v, int& r, int& g, int& b)
    {
        float c = v * s;
        float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
        float m = v - c;
        float rf, gf, bf;

        int sector = static_cast<int>(h * 6.0f) % 6;
        switch (sector)
        {
            case 0: rf = c; gf = x; bf = 0; break;
            case 1: rf = x; gf = c; bf = 0; break;
            case 2: rf = 0; gf = c; bf = x; break;
            case 3: rf = 0; gf = x; bf = c; break;
            case 4: rf = x; gf = 0; bf = c; break;
            default: rf = c; gf = 0; bf = x; break;
        }

        r = static_cast<int>((rf + m) * 127.0f);
        g = static_cast<int>((gf + m) * 127.0f);
        b = static_cast<int>((bf + m) * 127.0f);
    }

    LaunchkeyController& controller;
    int totalFrames = 60;
    int frameCount = 0;
    bool running = false;
};
