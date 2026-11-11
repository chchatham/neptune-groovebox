#pragma once

#include "LaunchkeyController.h"
#include <juce_core/juce_core.h>
#include <array>
#include <functional>

class PadManager
{
public:
    enum class Mode
    {
        Step,
        Play,
        CustomScale,
        Mute,
        Operator
    };

    static constexpr int TOTAL_PADS = 16;

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void padModeChanged(Mode /*newMode*/) {}
        virtual void padPressed(int /*padIndex*/, int /*velocity*/, Mode /*currentMode*/) {}
        virtual void padReleased(int /*padIndex*/, Mode /*currentMode*/) {}
        virtual void padAftertouch(int /*padIndex*/, int /*pressure*/) {}
    };

    PadManager();

    void setController(LaunchkeyController* ctrl) { controller = ctrl; }

    Mode getMode() const { return currentMode; }
    void setMode(Mode mode);

    void onPadPress(int padIndex, int velocity);
    void onPadRelease(int padIndex);
    void onPadAftertouch(int padIndex, int pressure);

    bool isPadHeld(int padIndex) const;
    int heldPadCount() const;

    void updatePadLEDs();
    // Send only the pads whose colour changed since the last send. Cheap enough to call on a timer,
    // so step toggles, parameter locks and the playhead show up without each screen pushing LEDs.
    void refreshPadLEDs();

    // Full-colour pads (0..127 per channel), for screens and hold layers that set their own colours.
    // Only pads whose colour changed are sent; the first call after palette colours sends all 16,
    // and the next refreshPadLEDs() / updatePadLEDs() goes back to the mode's palette colours.
    struct RGB
    {
        juce::uint8 r = 0, g = 0, b = 0;
        bool operator== (const RGB& o) const { return r == o.r && g == o.g && b == o.b; }
        bool operator!= (const RGB& o) const { return !(*this == o); }
    };
    void showRGB(const std::array<RGB, TOTAL_PADS>& colours);

    // Number of pad colour messages produced so far (counted with or without a controller).
    int getSendCount() const { return sendCount; }

    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

    using ColorCallback = std::function<int(int padIndex, Mode mode)>;
    void setColorCallback(ColorCallback cb) { colorCallback = std::move(cb); }

private:
    LaunchkeyController* controller = nullptr;
    Mode currentMode = Mode::Step;
    std::array<bool, TOTAL_PADS> padHeld {};
    std::array<int, TOTAL_PADS> lastSentColor {};
    std::array<RGB, TOTAL_PADS> lastSentRGB {};
    enum class Shown { Nothing, Palette, Rgb };
    Shown shown = Shown::Nothing;
    int sendCount = 0;
    juce::ListenerList<Listener> listeners;
    ColorCallback colorCallback;

    int padIndexToNote(int padIndex) const;
};
