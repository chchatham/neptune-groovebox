#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ProjectSlots.h"
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// The Mac's list of projects: the window "Projects".
//
//   P01  Night Drive              NOW   21 Nov 2026 14:05
//   P03  (no name)                      02 Nov 2026 09:12      <- the row SLOT is on is highlighted
//   P07  Other Song                     19 Oct 2026 22:40
//   Click: SLOT goes there.  Double-click or Return: load.  Escape: close.
//
// Every used slot is a row (ProjectSlots::rows()); empty slots are left out, so with SLOT on an
// empty slot no row is highlighted. It is open while SETTINGS -> PROJ is showing on the
// controller and can be opened from the Mac with Cmd+O (main.cpp). The controller needs none of
// it: SLOT and LOAD work as they always did, and the highlight follows SLOT.
//
// ProjectBrowser is the content (what the tests build and look at). It knows nothing about the
// app: a click is `onSelect`, a double-click or Return `onLoad`, Escape `onClose`.
// ProjectBrowserWindow puts it in a window that stays above the others.
// -----------------------------------------------------------------------------
class ProjectBrowser : public juce::Component, private juce::ListBoxModel
{
public:
    static constexpr int WIDTH = 480, HEIGHT = 372, ROW_HEIGHT = 26;

    std::function<void(int slot)> onSelect;      // the player chose a row (not: the highlight followed SLOT)
    std::function<void(int slot)> onLoad;
    std::function<void()> onClose;

    ProjectBrowser()
    {
        setName("Projects");
        setOpaque(true);

        list.setModel(this);
        list.setRowHeight(ROW_HEIGHT);
        list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xFF101418));
        list.setColour(juce::ListBox::outlineColourId, juce::Colour(0xFF8A9499));
        list.setOutlineThickness(1);
        addChildComponent(list);

        hint.setText("Click: SLOT goes there.  Double-click or Return: load.  Escape: close.",
                     juce::dontSendNotification);
        hint.setFont(juce::FontOptions(13.0f));
        hint.setColour(juce::Label::textColourId, juce::Colour(0xFFC8D0D4));
        hint.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(hint);

        setSize(WIDTH, HEIGHT);
    }

    ~ProjectBrowser() override { list.setModel(nullptr); }

    // The rows, and the slot SLOT is on. Neither tells onSelect.
    void setRows(std::vector<ProjectRow> newRows, int selectedSlot)
    {
        rows = std::move(newRows);
        const Quiet q(*this);
        list.setVisible(!rows.empty());          // (none: the words painted in its place)
        list.updateContent();
        list.repaint();
        highlight(selectedSlot);
    }

    void showSlot(int slot)
    {
        if (slot == shownSlot)
            return;
        const Quiet q(*this);
        highlight(slot);
    }

    const std::vector<ProjectRow>& getRows() const   { return rows; }
    int getShownSlot() const                         { return shownSlot; }
    // The slot of the highlighted row; -1 = none (SLOT is on an empty slot).
    int getHighlightedSlot() const
    {
        const int row = list.getSelectedRow();
        return row >= 0 && row < (int) rows.size() ? rows[(size_t) row].slot : -1;
    }
    juce::ListBox& getList()                         { return list; }
    juce::String getHint() const                     { return hint.getText(); }

    // What the mouse and the keyboard do, as the list reports them (the tests call these too).
    void clickRow(int row)
    {
        if (row >= 0 && row < (int) rows.size())
            list.selectRow(row);
    }
    void loadRow(int row)
    {
        if (row >= 0 && row < (int) rows.size() && onLoad)
            onLoad(rows[(size_t) row].slot);
    }

    void focusList() { list.grabKeyboardFocus(); }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress(juce::KeyPress::escapeKey)) { if (onClose) onClose(); return true; }
        if (key == juce::KeyPress(juce::KeyPress::returnKey)) { loadRow(list.getSelectedRow()); return true; }
        return false;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xFF263238));
        if (rows.empty())
        {
            g.setColour(juce::Colour(0xFFC8D0D4));
            g.setFont(juce::FontOptions(14.0f));
            g.drawFittedText("No project has been saved yet.\nSVAS on SETTINGS -> PROJ saves into the slot SLOT shows.",
                             list.getBounds().reduced(16), juce::Justification::centred, 3);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16, 14);
        hint.setBounds(area.removeFromBottom(22));
        area.removeFromBottom(8);
        list.setBounds(area);
    }

    // The window's content as it is drawn, as a PNG (for looking at it).
    bool writePng(const juce::File& file, float scale = 2.0f)
    {
        const auto image = createComponentSnapshot(getLocalBounds(), true, scale);
        file.deleteFile();
        juce::FileOutputStream out(file);
        return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out);
    }

private:
    // While the list is being told something, its "selection changed" is not the player's.
    struct Quiet
    {
        explicit Quiet(ProjectBrowser& b) : browser(b), was(b.quiet) { browser.quiet = true; }
        ~Quiet() { browser.quiet = was; }
        ProjectBrowser& browser;
        bool was;
    };

    void highlight(int slot)
    {
        shownSlot = slot;
        for (int r = 0; r < (int) rows.size(); ++r)
            if (rows[(size_t) r].slot == slot)
            {
                list.selectRow(r);
                return;
            }
        list.deselectAllRows();
    }

    // ---- juce::ListBoxModel -------------------------------------------------------------------
    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;
        const auto& r = rows[(size_t) row];
        if (selected)
            g.fillAll(juce::Colour(0xFF2F6F8C));
        g.setFont(juce::FontOptions(15.0f));
        g.setColour(r.name.isEmpty() && !selected ? juce::Colour(0xFF9AA5AA) : juce::Colours::white);
        g.drawText(r.text(), 10, 0, width - 200, height, juce::Justification::centredLeft, true);
        if (r.current)
        {
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.setColour(selected ? juce::Colours::white : juce::Colour(0xFFFFB04A));
            g.drawText("NOW", width - 186, 0, 40, height, juce::Justification::centredLeft, false);
        }
        g.setFont(juce::FontOptions(13.0f));
        g.setColour(selected ? juce::Colours::white : juce::Colour(0xFFC8D0D4));
        g.drawText(r.dateText(), width - 146, 0, 136, height, juce::Justification::centredRight, false);
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        if (quiet || lastRowSelected < 0 || lastRowSelected >= (int) rows.size())
            return;
        shownSlot = rows[(size_t) lastRowSelected].slot;
        if (onSelect)
            onSelect(shownSlot);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { loadRow(row); }
    void returnKeyPressed(int row) override                                    { loadRow(row); }

    juce::ListBox list;
    juce::Label hint;
    std::vector<ProjectRow> rows;
    int shownSlot = -1;
    bool quiet = false;
};

// The list in its window: above the other windows (so it cannot open behind something), with the
// system's title bar; its close button is Escape.
class ProjectBrowserWindow : public juce::DocumentWindow
{
public:
    ProjectBrowserWindow()
        : DocumentWindow("Projects", juce::Colour(0xFF263238), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&browser, true);
        setResizable(false, false);
        setAlwaysOnTop(true);
    }

    ProjectBrowser& getBrowser() { return browser; }

    // Where it goes: to the right of the main window if the screen has the room, else over it.
    static juce::Rectangle<int> placeBeside(juce::Rectangle<int> mainWindow, juce::Rectangle<int> screen, int width, int height)
    {
        juce::Rectangle<int> r(mainWindow.getRight() + 12, mainWindow.getY(), width, height);
        if (!screen.contains(r))
            r = juce::Rectangle<int>(width, height).withCentre(mainWindow.getCentre()).constrainedWithin(screen);
        return r;
    }

    // Shown beside `main`. From the controller (`takeKeyboard` false) the Mac's keyboard stays
    // where it is - it may be playing the pads of the main window, or be in another app; from the
    // Mac (Cmd+O) the window comes to the front with the keyboard in the list.
    void showBeside(juce::Component* main, bool takeKeyboard)
    {
        if (main != nullptr && !isVisible())
        {
            const auto mainBounds = main->getScreenBounds();
            const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(mainBounds);
            setBounds(placeBeside(mainBounds, display != nullptr ? display->userBounds.toNearestInt() : mainBounds.expanded(2000),
                                  getWidth(), getHeight()));
        }
        keyboardWanted = takeKeyboard;
        setVisible(true);
        if (takeKeyboard)
        {
            juce::Process::makeForegroundProcess();
            toFront(true);
            browser.focusList();
        }
        else
            toFront(false);
    }

    void closeButtonPressed() override { if (browser.onClose) browser.onClose(); }

protected:
    // (A top-level window takes the keyboard when it is shown: only when the Mac asked for it.)
    void visibilityChanged() override
    {
        if (keyboardWanted)
            DocumentWindow::visibilityChanged();
    }

private:
    ProjectBrowser browser;
    bool keyboardWanted = false;
};
