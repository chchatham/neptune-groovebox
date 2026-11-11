#pragma once

#include "../TurnNotice.h"
#include "../ScreenManager.h"
#include "../ScreenLayout.h"
#include "../../sequencer/SongMode.h"
#include "../../sequencer/SequencerCore.h"
#include "../../engine/ParamSpec.h"
#include <functional>

class SongScreen : public Screen
{
public:
    SongScreen(SongMode& song, SequencerCore& seq)
        : songMode(song), sequencer(seq) {}

    juce::String getName() const override { return "SONG"; }

    TurnNotice notice;

    juce::String getSubPage() const override
    {
        if (notice.showing())
            return notice.shown();
        if (songMode.getEntryCount() == 0)
            return "EMPTY";
        return "ROW " + juce::String(selectedRow + 1) + " OF "
             + juce::String(songMode.getEntryCount());
    }

    // The clock used by the DEL confirmation (tests inject their own).
    void setClock(std::function<juce::int64()> c) { clock = std::move(c); }

    // Left half: the rows, with the three editable columns under encoders 1-3 (pattern,
    // repeats, transpose) and the row number under encoder 4, which selects the row.
    // Right half: loop mode (encoder 5), song on/off (encoder 6) and the play position.
    void render(DisplayRenderer& display) override
    {
        static constexpr int ROW_HEIGHT = 8;
        static constexpr int START_Y = 12;
        static constexpr int VISIBLE_ROWS = 5;

        int entryCount = songMode.getEntryCount();

        display.drawString(66, START_Y, songMode.getLoopMode() ? "LP" : "1X");
        display.drawString(82, START_Y, songMode.isArmed() ? "ON" : "OFF");
        if (songMode.isActive())
        {
            display.drawString(66, START_Y + 12, "ROW " + juce::String(songMode.getCurrentEntryIndex() + 1)
                                                 + "/" + juce::String(entryCount));
            display.drawString(66, START_Y + 22, "REP " + juce::String(songMode.getCurrentRepeat() + 1) + "/"
                                                 + juce::String(songMode.getEntry(songMode.getCurrentEntryIndex()).repeatCount));
        }
        else
        {
            display.drawString(66, START_Y + 12, songMode.isArmed() ? "PLAY=SONG" : "PLAY=PAT");
        }
        if (confirm.knob == 7 && confirm.steps > 0)
            display.drawString(66, START_Y + 32, "DEL " + confirm.progress());

        if (entryCount == 0)
        {
            display.drawString(2, 20, "NO ROWS");
            display.drawString(2, 32, "TURN 1-3");
            return;
        }

        int scrollOffset = 0;
        if (selectedRow >= VISIBLE_ROWS)
            scrollOffset = selectedRow - VISIBLE_ROWS + 1;
        if (scrollOffset + VISIBLE_ROWS > entryCount)
            scrollOffset = std::max(0, entryCount - VISIBLE_ROWS);

        for (int i = 0; i < VISIBLE_ROWS && (scrollOffset + i) < entryCount; ++i)
        {
            int rowIdx = scrollOffset + i;
            const auto& entry = songMode.getEntry(rowIdx);
            int y = START_Y + i * ROW_HEIGHT;

            display.drawString(2, y, juce::String(entry.patternIndex + 1).paddedLeft('0', 2));     // encoder 1
            display.drawString(18, y, juce::String(entry.repeatCount).paddedLeft('x', 2));         // encoder 2
            display.drawString(32, y, (entry.transposeSemitones > 0 ? "+" : "")
                                      + juce::String(entry.transposeSemitones));                   // encoder 3
            display.drawString(51, y, juce::String(rowIdx + 1));                                   // encoder 4

            if (songMode.isActive() && rowIdx == songMode.getCurrentEntryIndex())
                display.drawRect(58, y + 2, 2, 3, true);

            if (rowIdx == selectedRow)
                display.invertRegion(0, y, 60, ROW_HEIGHT);
        }

        ScreenLayout::drawScrollBar(display, 62, START_Y, VISIBLE_ROWS * ROW_HEIGHT, entryCount, VISIBLE_ROWS, scrollOffset);
    }

    // Knobs: pattern, repeats, transpose, row | loop, song on/off, insert a row, delete the row.
    juce::String getEncoderName(int encoderIndex) const override
    {
        static const char* const names[] = { "PA", "RP", "TR", "RW", "LP", "SG", "IN", "DL" };
        return (encoderIndex >= 0 && encoderIndex < 8) ? names[encoderIndex] : "";
    }

    juce::String getEncoderReadout(int encoderIndex) const override
    {
        if (notice.showing())
            return notice.shown();
        const int count = songMode.getEntryCount();
        const bool hasRow = count > 0 && selectedRow < count;
        switch (encoderIndex)
        {
            case 0: return hasRow ? "PATTERN " + juce::String(songMode.getEntry(selectedRow).patternIndex + 1) : juce::String("PATTERN --");
            case 1: return hasRow ? "REPEATS " + juce::String(songMode.getEntry(selectedRow).repeatCount) : juce::String("REPEATS --");
            case 2:
            {
                if (!hasRow) return "TRANSPOSE --";
                const int t = songMode.getEntry(selectedRow).transposeSemitones;
                return "TRANSPOSE " + juce::String(t > 0 ? "+" : "") + juce::String(t);
            }
            case 3: return hasRow ? "ROW " + juce::String(selectedRow + 1) + "/" + juce::String(count) : juce::String("ROW --");
            case 4: return songMode.getLoopMode() ? "LOOP ON" : "LOOP ONCE";
            case 5: return songMode.isArmed() ? "SONG ON" : "SONG OFF";
            case 6: return "INSERT ROW";
            case 7: return "DELETE ROW " + confirm.progress();
            default: return {};
        }
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        const int clicks = (value > 64) ? value - 128 : value;
        if (clicks == 0) return;
        if (encoderIndex != 7) confirm.cancel();

        switch (encoderIndex)
        {
            case 3:        // select row
                if (songMode.getEntryCount() <= 1)
                {
                    // PA, RP, TR or IN make the first row; with one row there is nothing to choose.
                    notice.say(songMode.getEntryCount() == 0 ? "SONG IS EMPTY" : "ONE ROW");
                    return;
                }
                setSelectedRow((int) rowSpec().turned((float) selectedRow, clicks, turnStep()));
                return;
            case 4:        // loop the song / play it once
                songMode.setLoopMode(clicks > 0);
                return;
            case 5:        // song on / off: what Play does
                songMode.setArmed(clicks > 0);
                return;
            case 6:        // INS: one click clockwise inserts a row after the selection
                if (clicks > 0) insertRow();
                return;
            case 7:        // DEL: four clicks clockwise
                if (confirm.step(7, clicks, now()))
                    deleteRow();
                return;
            default: break;
        }
        if (encoderIndex < 0 || encoderIndex > 2) return;

        // An empty song has nothing to edit: the first turn of a row encoder creates row 1
        // (pattern 1, played once) and that same turn then edits it.
        if (songMode.getEntryCount() == 0)
        {
            songMode.addEntry(0);
            selectedRow = 0;
        }

        auto entry = songMode.getEntry(selectedRow);
        switch (encoderIndex)
        {
            case 0:
            {
                const ParamSpec spec { "PAT", 0, 0, (float) SongMode::MAX_PATTERN_INDEX, 0, ParamKind::Int };
                entry.patternIndex = (int) spec.turned((float) entry.patternIndex, clicks, turnStep());
                break;
            }
            case 1:
            {
                const ParamSpec spec { "RPT", 0, 1, 99, 1, ParamKind::Int };
                entry.repeatCount = (int) spec.turned((float) entry.repeatCount, clicks, turnStep());
                break;
            }
            case 2:
            {
                const ParamSpec spec { "TRN", 0, (float) SongMode::MIN_TRANSPOSE, (float) SongMode::MAX_TRANSPOSE, 0,
                                       ParamKind::Int, "st" };
                entry.transposeSemitones = (int) spec.turned((float) entry.transposeSemitones, clicks, turnStep());
                break;
            }
            default: break;
        }
        songMode.setEntry(selectedRow, entry);
    }

    // Pad N selects row N; the pad after the last row adds one.
    void onPadPress(int padIndex, int velocity) override
    {
        juce::ignoreUnused(velocity);
        confirm.cancel();

        if (padIndex >= 0 && padIndex < 16)
        {
            if (padIndex < songMode.getEntryCount())
                selectedRow = padIndex;
            else if (padIndex == songMode.getEntryCount()
                     && padIndex < SongMode::MAX_ENTRIES)
            {
                songMode.addEntry(0);
                selectedRow = padIndex;
            }
        }
    }

    // Up / Down move the row selection. The bank buttons do nothing here (one knob page): no
    // paging button ever adds or removes a row.
    void onPadPagePrev() override { confirm.cancel(); setSelectedRow(selectedRow - 1); }
    void onPadPageNext() override { confirm.cancel(); setSelectedRow(selectedRow + 1); }
    void getHelpFacts(HelpFacts& facts) const override { facts.tapUpDown = "SONG ROW"; }
    void onArrowLeft() override {}
    void onArrowRight() override {}
    void onDeactivate() override { confirm.cancel(); }

    // Pads: the first 16 rows; the selected row bright, the row being played green.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        const int count = songMode.getEntryCount();
        for (int i = 0; i < 16; ++i)
        {
            PadColour c;
            if (i < count)
            {
                c = { 0, 12, 40 };
                if (i == selectedRow) c = { 60, 90, 127 };
                if (songMode.isActive() && i == songMode.getCurrentEntryIndex())
                    c = (i == selectedRow) ? PadColour { 60, 127, 60 } : PadColour { 0, 100, 0 };
            }
            out[(size_t) i] = c;
        }
        return true;
    }

    int getSelectedRow() const { return selectedRow; }
    void setSelectedRow(int r) { selectedRow = juce::jlimit(0, std::max(0, songMode.getEntryCount() - 1), r); }

private:
    juce::int64 now() const { return clock ? clock() : UiClock::nowMs(); }
    ParamSpec rowSpec() const
    {
        return { "ROW", 0, 0, (float) std::max(0, songMode.getEntryCount() - 1), 0, ParamKind::Int };
    }

    void insertRow()
    {
        if (songMode.getEntryCount() >= SongMode::MAX_ENTRIES) return;
        const bool wasEmpty = songMode.getEntryCount() == 0;
        songMode.addEntry(0);
        if (wasEmpty) { selectedRow = 0; return; }
        songMode.moveEntry(songMode.getEntryCount() - 1, selectedRow + 1);
        selectedRow = std::min(selectedRow + 1, songMode.getEntryCount() - 1);
    }

    void deleteRow()
    {
        if (songMode.getEntryCount() == 0) return;
        songMode.removeEntry(selectedRow);
        setSelectedRow(selectedRow);
    }

    std::function<juce::int64()> clock;
    TurnConfirm confirm;
    SongMode& songMode;
    SequencerCore& sequencer;
    int selectedRow = 0;
};
