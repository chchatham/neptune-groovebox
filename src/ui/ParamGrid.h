#pragma once

#include "../controller/DisplayRenderer.h"
#include "../engine/ParamSpec.h"

// -----------------------------------------------------------------------------
// The standard 4 x 2 cell grid used by every screen that edits eight values:
// encoders 1-4 are the top row, 5-8 the bottom row, left to right. A cell shows
// label, bar and value of what its encoder changes.
// -----------------------------------------------------------------------------
namespace ParamGrid
{
    static constexpr int CELL_W = 32;
    static constexpr int ROW1_Y = 12;
    static constexpr int ROW2_Y = 33;
    static constexpr int STRIP_Y = 57;   // free strip under the grid (page marks, meters)

    inline int cellX(int index) { return (index % 4) * CELL_W; }
    inline int cellY(int index) { return (index / 4 == 0) ? ROW1_Y : ROW2_Y; }

    inline void drawEmptyCell(DisplayRenderer& display, int index)
    {
        display.drawString(cellX(index) + 1, cellY(index), "---");
    }

    inline void drawCell(DisplayRenderer& display, int index, const ParamSpec& spec, float value,
                         const juce::String& label, const juce::String& valueText = {})
    {
        const int x = cellX(index), y = cellY(index);
        display.drawString(x + 1, y, label.substring(0, 4));

        // Bar: 28 x 4 outline at y+8.
        display.drawRect(x + 1, y + 8, 28, 4, false);
        const int inner = 26;
        if (spec.kind == ParamKind::Choice || (spec.kind == ParamKind::Int && spec.maxVal - spec.minVal <= 16.0f))
        {
            // Discrete: a block at the current position.
            int count = juce::jmax(1, (int) std::lround(spec.maxVal - spec.minVal) + 1);
            int pos = juce::jlimit(0, count - 1, (int) std::lround(value - spec.minVal));
            int w = juce::jmax(2, inner / count);
            int px = (count > 1) ? (int) std::lround((float) pos * (float) (inner - w) / (float) (count - 1)) : 0;
            display.drawRect(x + 2 + px, y + 9, w, 2, true);
        }
        else if (spec.isBipolar())
        {
            // From the centre outwards.
            int centre = inner / 2;
            int pos = (int) std::lround(spec.toNorm(value) * (float) inner + 0.001f);
            int from = juce::jmin(centre, pos), to = juce::jmax(centre, pos);
            display.drawRect(x + 2 + from, y + 9, juce::jmax(1, to - from), 2, true);
        }
        else
        {
            // (+0.001: a value that came back a hair low after a click up and down draws the same bar)
            int fill = (int) std::lround(spec.toNorm(value) * (float) inner + 0.001f);
            if (fill > 0)
                display.drawRect(x + 2, y + 9, fill, 2, true);
        }

        // Five characters fill the cell: start them at its very edge, so three dark columns (not
        // two) separate them from the next cell ("-20.0" beside "4.00" read as one number).
        const juce::String text = (valueText.isNotEmpty() ? valueText : spec.format(value)).substring(0, 5);
        display.drawString(text.length() >= 5 ? x : x + 1, y + 13, text);
    }

    // Pressure range of a cell, on the pixel row just above its bar: a line from the low end to the
    // high end with a tick at each end that is set. With one end only, just that tick.
    inline void drawCellRange(DisplayRenderer& display, int index, bool hasLo, float normLo, bool hasHi, float normHi)
    {
        if (!hasLo && !hasHi) return;
        const int x = cellX(index) + 2, y = cellY(index) + 7, inner = 26;
        auto px = [&](float n) { return x + juce::jlimit(0, inner - 1, (int) std::lround(n * (float) (inner - 1))); };
        if (hasLo && hasHi)
        {
            int a = juce::jmin(px(normLo), px(normHi)), b = juce::jmax(px(normLo), px(normHi));
            for (int i = a; i <= b; ++i)
                display.setPixel(i, y, true);
        }
        if (hasLo) { display.setPixel(px(normLo), y, true); display.setPixel(px(normLo), y - 1, true); }
        if (hasHi) { display.setPixel(px(normHi), y, true); display.setPixel(px(normHi), y - 1, true); }
    }

    // Small mark in the top right corner of a cell (parameter lock, modified, ...).
    inline void drawCellMark(DisplayRenderer& display, int index)
    {
        const int x = cellX(index), y = cellY(index);
        display.setPixel(x + 28, y, true);
        display.setPixel(x + 29, y, true);
        display.setPixel(x + 28, y + 1, true);
        display.setPixel(x + 29, y + 1, true);
    }

    // One mark per page in the bottom strip; `breakBefore(i)` says whether page i starts a new group.
    template <typename BreakFn>
    inline void drawPageStrip(DisplayRenderer& display, int pageCount, int current, BreakFn breakBefore)
    {
        if (pageCount <= 1) return;
        int gaps = 0;
        for (int i = 1; i < pageCount; ++i)
            if (breakBefore(i)) ++gaps;

        // Shrink the marks when there are many pages.
        int step = 8, gapW = 5;
        while (step > 3 && pageCount * step + gaps * gapW > 126) --step;
        if (pageCount * step + gaps * gapW > 126) gapW = 2;
        const int markW = juce::jmax(2, step - 2);
        int total = pageCount * step - (step - markW) + gaps * gapW;
        int x = juce::jmax(0, (128 - total) / 2);

        for (int i = 0; i < pageCount; ++i)
        {
            if (i > 0 && breakBefore(i)) x += gapW;
            if (i == current) display.drawRect(x, STRIP_Y, markW, 6, true);
            else              display.drawRect(x, STRIP_Y + 4, markW, 2, true);
            x += step;
        }
    }

    // Horizontal meter across the bottom strip (0..1).
    inline void drawStripMeter(DisplayRenderer& display, const char* label, float norm)
    {
        display.drawString(0, STRIP_Y - 1, label);
        display.drawRect(14, STRIP_Y, 112, 6, false);
        int fill = (int) (juce::jlimit(0.0f, 1.0f, norm) * 110.0f);
        if (fill > 0)
            display.drawRect(15, STRIP_Y + 1, fill, 4, true);
    }

    // Signed click count of an encoder message (two's-complement steps in `value`).
    inline int clicks(int encoderValue) { return encoderValue > 64 ? encoderValue - 128 : encoderValue; }

    // New value for a cell after an encoder message, by the control grammar's step sizes
    // (ParamSpec::turned). Screens pass their own turnStep().
    inline float turned(const ParamSpec& spec, float current, int encoderValue, TurnStep mode)
    {
        return spec.turned(current, clicks(encoderValue), mode);
    }

    // Older form: plain or fine.
    inline float stepped(const ParamSpec& spec, float current, int encoderValue, bool fine = false)
    {
        return turned(spec, current, encoderValue, fine ? TurnStep::Fine : TurnStep::Plain);
    }
}
