#pragma once

#include "ScreenManager.h"
#include "Icons.h"

namespace ScreenLayout
{

static constexpr int NAME_BAR_Y = 0;
static constexpr int NAME_BAR_H = 10;
static constexpr int SEPARATOR_Y = 10;
static constexpr int CONTENT_Y = 11;
static constexpr int CONTENT_H = 42;
static constexpr int CONTENT_BOTTOM = 52;
static constexpr int STATUS_BAR_Y = 53;
static constexpr int STATUS_BAR_H = 11;
static constexpr int COLUMN_W = 16;

inline void drawEncoderBar(DisplayRenderer& display, const Screen& screen)
{
    display.drawHLine(0, STATUS_BAR_Y, DisplayRenderer::WIDTH);

    for (int i = 0; i < 8; ++i)
    {
        auto name = screen.getEncoderName(i);
        if (name.isEmpty())
            continue;

        if (name.length() > 3)
            name = name.substring(0, 3);

        int x = i * COLUMN_W;
        int textW = name.length() * 6;
        int textX = x + (COLUMN_W - textW) / 2;
        display.drawString(textX, STATUS_BAR_Y + 2, name);
    }
}

// Scrolling lists: a 2-pixel track position mark at the given x whenever there are more items than
// fit. The same on every list (presets, song rows).
inline void drawScrollBar(DisplayRenderer& display, int x, int y, int height, int totalItems, int visibleItems, int firstVisible)
{
    if (totalItems <= visibleItems || visibleItems <= 0)
        return;
    int thumbH = juce::jmax(4, height * visibleItems / totalItems);
    int thumbY = y + (height - thumbH) * firstVisible / juce::jmax(1, totalItems - visibleItems);
    display.drawRect(x, thumbY, 2, thumbH, true);
}

inline void drawNavArrows(DisplayRenderer& display, bool canLeft, bool canRight)
{
    if (canLeft)
        display.drawBitmap(1, STATUS_BAR_Y + 3, Icons::ARROW_LEFT,
                           Icons::ARROW_LEFT_W, Icons::ARROW_LEFT_H);

    if (canRight)
        display.drawBitmap(DisplayRenderer::WIDTH - Icons::ARROW_RIGHT_W - 1,
                           STATUS_BAR_Y + 3, Icons::ARROW_RIGHT,
                           Icons::ARROW_RIGHT_W, Icons::ARROW_RIGHT_H);
}

} // namespace ScreenLayout
