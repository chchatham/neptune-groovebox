#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../controller/DisplayRenderer.h"
#include <functional>

// Action IDs for keyboard-mapped Launchkey functions
enum class MirrorKeyAction
{
    Play,
    Record,
    ArrowLeft,
    ArrowRight,
    Shift,
    Arp,
    Scale,
    EncoderBankUp,
    EncoderBankDown
};

class OLEDMirrorComponent : public juce::Component, private juce::Timer
{
public:
    static constexpr int PIXEL_SCALE = 4;
    static constexpr int BEZEL_PAD = 12;
    static constexpr int DISPLAY_W = DisplayRenderer::WIDTH * PIXEL_SCALE;
    static constexpr int DISPLAY_H = DisplayRenderer::HEIGHT * PIXEL_SCALE;
    static constexpr int TOTAL_W = DISPLAY_W + BEZEL_PAD * 2;
    static constexpr int TOTAL_H = DISPLAY_H + BEZEL_PAD * 2;
    static constexpr int BEZEL_RADIUS = 6;

    // Pad grid layout: 128px wide display / 8 pads = 16px per pad
    // Lower half of display: 64px tall / 2 rows = 32px per row
    // In scaled coordinates: 64px per pad width, 128px per pad row height
    static constexpr int PAD_COLS = 8;
    static constexpr int PAD_ROWS = 2;
    static constexpr int DISPLAY_PAD_W = DisplayRenderer::WIDTH / PAD_COLS;   // 16 logical pixels per pad
    static constexpr int DISPLAY_PAD_H = (DisplayRenderer::HEIGHT / 2) / PAD_ROWS; // 16 logical pixels per pad row
    static constexpr int PAD_AREA_TOP_Y = DisplayRenderer::HEIGHT / 2;        // pad area starts at lower half (row 32)

    // Callbacks
    std::function<void(int padIndex, float velocity)> onPadPress;
    std::function<void(int padIndex)> onPadRelease;
    std::function<void(MirrorKeyAction actionId, bool pressed)> onKeyAction;

    explicit OLEDMirrorComponent(DisplayRenderer& renderer)
        : displayRenderer(renderer)
    {
        setSize(TOTAL_W, TOTAL_H);
        setWantsKeyboardFocus(true);
        startTimerHz(30);
    }

    ~OLEDMirrorComponent() override
    {
        stopTimer();
    }

    // Convert mouse coordinates to a pad index (0-15), or -1 if outside pad area.
    // Pads 0-7 = top row, pads 8-15 = bottom row.
    static int mouseToPadIndex(int mouseX, int mouseY)
    {
        // Convert from component coords to logical display coords
        int displayX = mouseX - BEZEL_PAD;
        int displayY = mouseY - BEZEL_PAD;

        // Check bounds: must be within display area
        if (displayX < 0 || displayX >= DISPLAY_W || displayY < 0 || displayY >= DISPLAY_H)
            return -1;

        // Convert scaled coords to logical pixel coords
        int logicalX = displayX / PIXEL_SCALE;
        int logicalY = displayY / PIXEL_SCALE;

        // Pad area is the lower half of the display (rows 32-63)
        if (logicalY < PAD_AREA_TOP_Y)
            return -1;

        int padCol = logicalX / DISPLAY_PAD_W;
        int padRow = (logicalY - PAD_AREA_TOP_Y) / DISPLAY_PAD_H;

        if (padCol < 0 || padCol >= PAD_COLS || padRow < 0 || padRow >= PAD_ROWS)
            return -1;

        return padRow * PAD_COLS + padCol;
    }

    // Map a JUCE KeyPress to a MirrorKeyAction, or return false if no mapping.
    static bool keyToAction(const juce::KeyPress& key, MirrorKeyAction& outAction)
    {
        int code = key.getKeyCode();

        if (code == juce::KeyPress::spaceKey)       { outAction = MirrorKeyAction::Play;            return true; }
        if (code == 'R' || code == 'r')              { outAction = MirrorKeyAction::Record;          return true; }
        if (code == juce::KeyPress::leftKey)          { outAction = MirrorKeyAction::ArrowLeft;       return true; }
        if (code == juce::KeyPress::rightKey)         { outAction = MirrorKeyAction::ArrowRight;      return true; }
        if (code == juce::KeyPress::tabKey)           { outAction = MirrorKeyAction::Scale;           return true; }
        if (code == 'A' || code == 'a')              { outAction = MirrorKeyAction::Arp;             return true; }
        if (code == juce::KeyPress::upKey || code == '[' || code == '{')
            { outAction = MirrorKeyAction::EncoderBankUp; return true; }
        if (code == juce::KeyPress::downKey || code == ']' || code == '}')
            { outAction = MirrorKeyAction::EncoderBankDown; return true; }

        return false;
    }

    // Map a keyboard key to a pad index for the top row (keys 1-8 = pads 0-7)
    // and bottom row (Q/W/E/R/T/Y/U/I = pads 8-15). Returns -1 if no mapping.
    static int keyToPadIndex(const juce::KeyPress& key)
    {
        int code = key.getKeyCode();

        // Top row: number keys 1-8 map to pads 0-7
        if (code >= '1' && code <= '8')
            return code - '1';

        // Bottom row: QWERTYUI map to pads 8-15
        switch (code)
        {
            case 'Q': case 'q': return 8;
            case 'W': case 'w': return 9;
            case 'E': case 'e': return 10;
            // Note: 'R' is also Record, but pad mapping takes precedence
            // only when no action mapping matches first. In practice we
            // handle actions before pads in keyPressed().
            case 'T': case 't': return 11;
            case 'Y': case 'y': return 12;
            case 'U': case 'u': return 13;
            case 'I': case 'i': return 14;
            default: break;
        }
        return -1;
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        int pad = mouseToPadIndex(event.x, event.y);
        if (pad >= 0 && onPadPress)
            onPadPress(pad, 1.0f);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        int pad = mouseToPadIndex(event.x, event.y);
        if (pad >= 0 && onPadRelease)
            onPadRelease(pad);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        // Check action mapping first
        MirrorKeyAction action;
        if (keyToAction(key, action))
        {
            if (onKeyAction)
                onKeyAction(action, true);
            return true;
        }

        // Check pad mapping
        int pad = keyToPadIndex(key);
        if (pad >= 0)
        {
            if (onPadPress)
                onPadPress(pad, 1.0f);
            return true;
        }

        return false;
    }

    bool keyStateChanged(bool isKeyDown) override
    {
        // Handle Shift key as a modifier (not delivered via keyPressed)
        bool shiftNow = juce::ModifierKeys::currentModifiers.isShiftDown();
        if (shiftNow != shiftHeld)
        {
            shiftHeld = shiftNow;
            if (onKeyAction)
                onKeyAction(MirrorKeyAction::Shift, shiftNow);
            return true;
        }
        return false;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xFF1A1A1A));

        g.setColour(juce::Colour(0xFF2A2A2A));
        g.fillRoundedRectangle(
            (float)(BEZEL_PAD - 4), (float)(BEZEL_PAD - 4),
            (float)(DISPLAY_W + 8), (float)(DISPLAY_H + 8),
            (float)BEZEL_RADIUS);

        g.setColour(juce::Colour(0xFF0A0A0A));
        g.fillRect(BEZEL_PAD, BEZEL_PAD, DISPLAY_W, DISPLAY_H);

        const auto& pixels = displayRenderer.getPixels();
        g.setColour(juce::Colour(0xFFCCDDEE));

        for (int y = 0; y < DisplayRenderer::HEIGHT; ++y)
        {
            int runStart = -1;
            for (int x = 0; x <= DisplayRenderer::WIDTH; ++x)
            {
                bool on = (x < DisplayRenderer::WIDTH) && pixels[y][x];
                if (on && runStart < 0)
                {
                    runStart = x;
                }
                else if (!on && runStart >= 0)
                {
                    g.fillRect(
                        BEZEL_PAD + runStart * PIXEL_SCALE,
                        BEZEL_PAD + y * PIXEL_SCALE,
                        (x - runStart) * PIXEL_SCALE,
                        PIXEL_SCALE);
                    runStart = -1;
                }
            }
        }
    }

    void setRefreshRate(int hz)
    {
        stopTimer();
        if (hz > 0)
            startTimerHz(hz);
    }

    DisplayRenderer& getDisplayRenderer() { return displayRenderer; }

private:
    DisplayRenderer& displayRenderer;
    bool shiftHeld = false;

    void timerCallback() override
    {
        displayRenderer.tickAckTimeout();
        repaint();
    }
};
