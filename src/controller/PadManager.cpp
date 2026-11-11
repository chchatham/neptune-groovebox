#include "PadManager.h"

PadManager::PadManager()
{
    padHeld.fill(false);
}

void PadManager::setMode(Mode mode)
{
    if (mode == currentMode)
        return;

    currentMode = mode;
    listeners.call(&Listener::padModeChanged, mode);
    // While full-colour pads are shown the mode's colours are not on the pads; the next
    // refreshPadLEDs() sends them all.
    if (shown != Shown::Rgb)
        updatePadLEDs();
}

void PadManager::onPadPress(int padIndex, int velocity)
{
    if (padIndex < 0 || padIndex >= TOTAL_PADS)
        return;

    padHeld[padIndex] = true;
    listeners.call(&Listener::padPressed, padIndex, velocity, currentMode);
}

void PadManager::onPadRelease(int padIndex)
{
    if (padIndex < 0 || padIndex >= TOTAL_PADS)
        return;

    padHeld[padIndex] = false;
    listeners.call(&Listener::padReleased, padIndex, currentMode);
}

void PadManager::onPadAftertouch(int padIndex, int pressure)
{
    if (padIndex < 0 || padIndex >= TOTAL_PADS)
        return;

    listeners.call(&Listener::padAftertouch, padIndex, pressure);
}

bool PadManager::isPadHeld(int padIndex) const
{
    if (padIndex < 0 || padIndex >= TOTAL_PADS)
        return false;
    return padHeld[padIndex];
}

int PadManager::heldPadCount() const
{
    int count = 0;
    for (bool held : padHeld)
        if (held) ++count;
    return count;
}

void PadManager::updatePadLEDs()
{
    for (int i = 0; i < TOTAL_PADS; ++i)
    {
        int color = 0;
        if (colorCallback)
            color = colorCallback(i, currentMode);

        if (controller)
            controller->setPadColor(padIndexToNote(i), color);
        ++sendCount;
        lastSentColor[(size_t) i] = color;
    }
    shown = Shown::Palette;
}

void PadManager::refreshPadLEDs()
{
    if (shown != Shown::Palette)
    {
        updatePadLEDs();
        return;
    }

    for (int i = 0; i < TOTAL_PADS; ++i)
    {
        int color = colorCallback ? colorCallback(i, currentMode) : 0;
        if (color != lastSentColor[(size_t) i])
        {
            if (controller)
                controller->setPadColor(padIndexToNote(i), color);
            ++sendCount;
            lastSentColor[(size_t) i] = color;
        }
    }
}

void PadManager::showRGB(const std::array<RGB, TOTAL_PADS>& colours)
{
    const bool all = (shown != Shown::Rgb);
    for (int i = 0; i < TOTAL_PADS; ++i)
    {
        const auto& c = colours[(size_t) i];
        if (all || c != lastSentRGB[(size_t) i])
        {
            if (controller)
                controller->setPadRGB(padIndexToNote(i), c.r, c.g, c.b);
            ++sendCount;
            lastSentRGB[(size_t) i] = c;
        }
    }
    shown = Shown::Rgb;
}

int PadManager::padIndexToNote(int padIndex) const
{
    if (padIndex < 8)
        return LaunchkeyController::PAD_TOP_ROW_START + padIndex;
    return LaunchkeyController::PAD_BOTTOM_ROW_START + (padIndex - 8);
}
