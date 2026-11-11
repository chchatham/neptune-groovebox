#pragma once

#include "ScreenManager.h"
#include "ParamGrid.h"
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// Base for screens that are simply pages of eight values in the standard 4 x 2
// grid (encoders 1-4 top row, 5-8 bottom row). A derived screen only declares
// its cells: what each shows and how to read and write it.
//
// Encoder-bank buttons change page; Shift + turn is fine and Func + turn coarse
// (ParamSpec::turned); the strip at the bottom shows the page position.
// -----------------------------------------------------------------------------
class GridScreen : public Screen
{
public:
    struct Cell
    {
        ParamSpec spec;                              // spec.id is ignored; an empty label = unused cell
        std::function<float()> get;
        std::function<void(float)> set;
        std::function<juce::String(float)> text;     // optional custom value text
        bool used() const { return get != nullptr; }
    };

    struct Page
    {
        juce::String name;
        std::array<Cell, 8> cells;
    };

    bool usesEncoderBar() const override { return false; }
    EncoderLayout getEncoderLayout() const override { return EncoderLayout::Grid; }

    void render(DisplayRenderer& display) override
    {
        if (gridPages.empty()) return;
        auto& page = gridPages[(size_t) gridPageIndex];
        for (int i = 0; i < 8; ++i)
        {
            auto& cell = page.cells[(size_t) i];
            if (!cell.used())
            {
                ParamGrid::drawEmptyCell(display, i);
                continue;
            }
            float v = cell.get();
            ParamGrid::drawCell(display, i, cell.spec, v, cell.spec.label,
                                cell.text ? cell.text(v) : juce::String());
        }
        drawStrip(display);
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        if (gridPages.empty() || encoderIndex < 0 || encoderIndex >= 8) return;
        auto& cell = gridPages[(size_t) gridPageIndex].cells[(size_t) encoderIndex];
        if (!cell.used() || !cell.set) return;
        cell.set(ParamGrid::turned(cell.spec, cell.get(), value, turnStep()));
    }

    juce::String getEncoderName(int encoderIndex) const override
    {
        if (gridPages.empty() || encoderIndex < 0 || encoderIndex >= 8) return "---";
        auto& cell = gridPages[(size_t) gridPageIndex].cells[(size_t) encoderIndex];
        return cell.used() ? juce::String(cell.spec.label) : juce::String("---");
    }

    // Bank buttons go round: back from the first page is the last, on from the last is the first.
    void onArrowLeft() override
    {
        const int n = (int) gridPages.size();
        if (n > 0) gridPageIndex = (gridPageIndex + n - 1) % n;
    }
    void onArrowRight() override
    {
        const int n = (int) gridPages.size();
        if (n > 0) gridPageIndex = (gridPageIndex + 1) % n;
    }
    // Func + bank: first / last page.
    void onSectionPrev() override { gridPageIndex = 0; }
    void onSectionNext() override { gridPageIndex = juce::jmax(0, (int) gridPages.size() - 1); }
    void onShiftChanged(bool held) override { gridShiftHeld = held; }
    void getHelpFacts(HelpFacts& facts) const override { facts.bankJump = BankJump::FirstLast; }

    int getPageIndex() const override { return gridPageIndex; }
    int getPageCount() const override { return (int) gridPages.size(); }
    void setPageIndex(int i) { gridPageIndex = juce::jlimit(0, juce::jmax(0, (int) gridPages.size() - 1), i); }
    juce::String getPageName() const
    {
        return gridPages.empty() ? juce::String() : gridPages[(size_t) gridPageIndex].name;
    }

protected:
    // What goes in the bottom strip; the default is the page marks.
    virtual void drawStrip(DisplayRenderer& display)
    {
        ParamGrid::drawPageStrip(display, (int) gridPages.size(), gridPageIndex, [](int) { return false; });
    }

    static Cell cell(ParamSpec spec, std::function<float()> get, std::function<void(float)> set,
                     std::function<juce::String(float)> text = nullptr)
    {
        return { spec, std::move(get), std::move(set), std::move(text) };
    }

    static ParamSpec onOff(const char* label)
    {
        return { label, 0, 0, 1, 0, ParamKind::Choice, "", ParamChoices::offOn, 2 };
    }

    std::vector<Page> gridPages;
    int gridPageIndex = 0;
    bool gridShiftHeld = false;      // unused by the grid itself (turnStep() decides); kept for derived screens
};
