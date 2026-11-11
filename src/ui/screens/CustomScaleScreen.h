#pragma once
#include <array>
#include <functional>

#include "../ScreenManager.h"
#include "../NoteUtils.h"
#include "../ScreenLayout.h"
#include "../../controller/CustomScaleManager.h"
#include "../../engine/ParamSpec.h"

class CustomScaleScreen : public Screen
{
public:
    using NoteCallback = std::function<void(int midiNote, int velocity)>;

    explicit CustomScaleScreen(CustomScaleManager& mgr)
        : scaleManager(mgr) {}

    juce::String getName() const override { return "SCALE"; }

    juce::String getSubPage() const override
    {
        int page = scaleManager.getActivePage();
        juce::String sub;

        if (scaleManager.isEditMode())
            sub += "*";

        sub += juce::String(scaleManager.getPageName(page)).substring(0, 6);
        sub += " ";
        sub += NoteUtils::noteToString(scaleManager.getRootNote());

        // What encoders 3 and 5 are set to (they have no cell of their own): relative / absolute
        // page, and whether keys select pages (latched or momentary).
        sub += scaleManager.getPageMode(page) == CustomScaleManager::Mode::Absolute ? " AB" : " RL";
        switch (scaleManager.getKeySelectMode())
        {
            case CustomScaleManager::KeySelectMode::Latched:   sub += " KL"; break;
            case CustomScaleManager::KeySelectMode::Momentary: sub += " KM"; break;
            default: break;
        }
        if (getLock && getLock())
            sub += " LK";
        return sub;
    }

    void render(DisplayRenderer& display) override
    {
        if (scaleManager.isEditMode())
            renderEditMode(display);
        else if (browseMode)
            renderBrowser(display);
        else
            renderNormal(display);
    }

    juce::String getEncoderName(int encoderIndex) const override
    {
        if (scaleManager.isEditMode())
        {
            static const char* editNames[] = { "PG", "RT", "MD", "ED", "NT", "PD", "OC", "LK" };
            if (encoderIndex < 0 || encoderIndex >= 8) return {};
            return editNames[encoderIndex];
        }

        static const char* names[] = { "PG", "RT", "MD", "ED", "KS", "KM", "OC", "LK" };
        if (encoderIndex < 0 || encoderIndex >= 8)
            return {};
        return names[encoderIndex];
    }

    // Full name and value of knob i, for the name bar.
    juce::String getEncoderReadout(int encoderIndex) const override
    {
        const int page = scaleManager.getActivePage();
        const bool edit = scaleManager.isEditMode();
        switch (encoderIndex)
        {
            case 0: return "PAGE " + juce::String(page + 1) + " " + juce::String(scaleManager.getPageName(page));
            case 1: return "ROOT " + NoteUtils::noteToString(scaleManager.getRootNote());
            case 2: return scaleManager.getPageMode(page) == CustomScaleManager::Mode::Absolute ? "MODE ABS" : "MODE REL";
            case 3: return edit ? "EDIT ON" : "EDIT OFF";
            case 4:
                if (edit)
                {
                    const int pad = scaleManager.getEditPadIndex();
                    return pad >= 0 ? "NOTE " + NoteUtils::noteToString(scaleManager.getNoteForPad(pad)) : juce::String("NOTE --");
                }
                switch (scaleManager.getKeySelectMode())
                {
                    case CustomScaleManager::KeySelectMode::Latched:   return "KEY SELECT LATCH";
                    case CustomScaleManager::KeySelectMode::Momentary: return "KEY SELECT MOMENT";
                    default:                                           return "KEY SELECT OFF";
                }
            case 5:
                if (edit)
                    return "PAD " + (scaleManager.getEditPadIndex() >= 0 ? juce::String(scaleManager.getEditPadIndex() + 1) : juce::String("--"));
                return scaleManager.getKeyByPageNumber() ? "KEY MAP NUMBER" : "KEY MAP ROOT";
            case 6: return "OCTAVE " + juce::String(scaleManager.getRootNote() / 12 - 1);
            case 7: return (getLock && getLock()) ? "LOCK ON" : "LOCK OFF";
            default: return {};
        }
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        const int delta = (value > 64) ? -(128 - value) : value;
        if (delta == 0) return;
        const int page = scaleManager.getActivePage();
        static const ParamSpec noteSpec { "NOTE", 0, 0, 127, 60, ParamKind::Int, "note" };

        switch (encoderIndex)
        {
            case 0: // PG - page
            {
                const ParamSpec spec { "PG", 0, 0, (float) (scaleManager.getPageCount() - 1), 0, ParamKind::Int };
                scaleManager.setActivePage((int) spec.turned((float) page, delta, turnStep()));
                return;
            }
            case 1: // RT - root note (Func: by octaves)
                scaleManager.setRootNote(page, (int) noteSpec.turned((float) scaleManager.getRootNote(), delta, turnStep()));
                return;
            case 2: // MD - absolute / relative
                scaleManager.setPageMode(page, delta > 0 ? CustomScaleManager::Mode::Absolute
                                                         : CustomScaleManager::Mode::Relative);
                return;
            case 3: // ED - edit mode
                scaleManager.setEditMode(delta > 0);
                return;
            case 6: // OC - the whole page up or down an octave
            {
                int newRoot = scaleManager.getRootNote() + (delta > 0 ? 12 : -12);
                if (newRoot >= 0 && newRoot <= 127)
                    scaleManager.setRootNote(page, newRoot);
                return;
            }
            case 7: // LK - scale lock (same as the Scale button)
                if (setLock) setLock(delta > 0);
                return;
            default: break;
        }

        if (scaleManager.isEditMode())
        {
            if (encoderIndex == 4) // NT - the selected pad's note
            {
                int editPad = scaleManager.getEditPadIndex();
                if (editPad >= 0)
                    scaleManager.setNoteForPad(page, editPad,
                        (int) noteSpec.turned((float) scaleManager.getPage(page).notes[editPad], delta, turnStep()));
            }
            else if (encoderIndex == 5) // PD - which pad
            {
                const ParamSpec spec { "PD", 0, 0, CustomScaleManager::PADS_PER_PAGE - 1, 0, ParamKind::Int };
                int editPad = scaleManager.getEditPadIndex();
                int newPad = (int) spec.turned((float) (editPad < 0 ? 0 : editPad), delta, turnStep());
                scaleManager.setEditPadIndex(newPad);
                selectedPad = newPad;
            }
            return;
        }

        if (encoderIndex == 4) // KS - keys select pages: off, latched, momentary
        {
            static const char* const names[] = { "OFF", "LTCH", "MOMT" };
            const ParamSpec spec { "KS", 0, 0, 2, 0, ParamKind::Choice, "", names, 3 };
            int mode = (int) spec.turned((float) static_cast<int>(scaleManager.getKeySelectMode()), delta > 0 ? 1 : -1, TurnStep::Plain);
            scaleManager.setKeySelectMode(static_cast<CustomScaleManager::KeySelectMode>(mode));
        }
        else if (encoderIndex == 5) // KM - keys choose a page by its root or by its number
            scaleManager.setKeyByPageNumber(delta > 0);
    }

    void onPadPress(int padIndex, int velocity) override
    {
        if (padIndex < 0 || padIndex >= CustomScaleManager::PADS_PER_PAGE)
            return;

        if (scaleManager.isEditMode())
        {
            // In edit mode, tapping a pad selects it for note assignment
            scaleManager.setEditPadIndex(padIndex);
            selectedPad = padIndex;
            return;
        }

        selectedPad = padIndex;
        int midiNote = scaleManager.getNoteForPad(padIndex);
        soundingNote[(size_t) padIndex] = midiNote;

        if (noteCallback && midiNote >= 0)
            noteCallback(midiNote, velocity);
    }

    void onPadAftertouch(int padIndex, int pressure) override
    {
        if (padIndex < 0 || padIndex >= CustomScaleManager::PADS_PER_PAGE)
            return;
        int midiNote = soundingNote[(size_t) padIndex];
        if (aftertouchCallback && midiNote >= 0)
            aftertouchCallback(midiNote, pressure);
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex >= 0 && padIndex < CustomScaleManager::PADS_PER_PAGE)
        {
            // Release the note that was started by this pad, even if the page changed meanwhile.
            int midiNote = soundingNote[(size_t) padIndex];
            soundingNote[(size_t) padIndex] = -1;
            if (noteOffCallback && midiNote >= 0)
                noteOffCallback(midiNote);
        }

        if (scaleManager.isEditMode())
            return; // keep selection in edit mode

        if (padIndex == selectedPad)
            selectedPad = -1;
    }

    // Keyboard events for page selection and edit mode note assignment
    bool wantsKeyboardEvents() const override
    {
        return scaleManager.isEditMode()
            || scaleManager.getKeySelectMode() != CustomScaleManager::KeySelectMode::Off;
    }

    void onKeyboardNoteOn(int note, int velocity) override
    {
        if (scaleManager.isEditMode())
        {
            // In edit mode, key press assigns note to selected pad
            scaleManager.assignNoteToEditPad(note);
            return;
        }

        if (scaleManager.getKeySelectMode() != CustomScaleManager::KeySelectMode::Off)
        {
            scaleManager.onKeyboardNoteForPageSelect(note);
            // The key still plays the track (the same path as the pads).
            if (note >= 0 && note < 128)
            {
                keyPlaying[(size_t) note] = true;
                if (noteCallback) noteCallback(note, velocity);
            }
        }
    }

    void onKeyboardNoteOff(int note) override
    {
        if (scaleManager.getKeySelectMode() == CustomScaleManager::KeySelectMode::Momentary)
            scaleManager.onKeyboardNoteReleasedForPageSelect(note);
        if (note >= 0 && note < 128 && keyPlaying[(size_t) note])
        {
            keyPlaying[(size_t) note] = false;
            if (noteOffCallback) noteOffCallback(note);
        }
    }

    // Up / Down = the scale page, in edit mode too. The bank buttons do nothing here (one knob
    // page); the octave is on the OC knob only.
    void onPadPagePrev() override
    {
        if (scaleManager.getActivePage() > 0)
            scaleManager.setActivePage(scaleManager.getActivePage() - 1);
    }
    void onPadPageNext() override
    {
        if (scaleManager.getActivePage() + 1 < scaleManager.getPageCount())
            scaleManager.setActivePage(scaleManager.getActivePage() + 1);
    }
    void onArrowLeft() override {}
    void onArrowRight() override {}
    void getHelpFacts(HelpFacts& facts) const override { facts.tapUpDown = "SCALE PAGE"; }

    // Copy current page to clipboard
    void copyCurrentPage()
    {
        scaleManager.copyPage(scaleManager.getActivePage());
    }

    // Paste clipboard to current page
    bool pasteToCurrentPage()
    {
        return scaleManager.pastePage(scaleManager.getActivePage());
    }

    // Clear current page
    void clearCurrentPage()
    {
        scaleManager.clearPage(scaleManager.getActivePage());
    }

    void setNoteCallback(NoteCallback cb) { noteCallback = std::move(cb); }
    void setLockHooks(std::function<bool()> get, std::function<void(bool)> set) { getLock = std::move(get); setLock = std::move(set); }
    void setNoteOffCallback(std::function<void(int midiNote)> cb) { noteOffCallback = std::move(cb); }
    void setAftertouchCallback(std::function<void(int midiNote, int pressure)> cb) { aftertouchCallback = std::move(cb); }

    // Pads: the root one colour, the other notes another, a held pad bright. In edit mode the
    // pad being edited is white and the rest dim red.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        const bool edit = scaleManager.isEditMode();
        for (int i = 0; i < CustomScaleManager::PADS_PER_PAGE && i < 16; ++i)
        {
            PadColour c;
            if (edit)
                c = (i == scaleManager.getEditPadIndex()) ? PadColour { 127, 127, 127 } : PadColour { 30, 0, 0 };
            else
            {
                const int midiNote = scaleManager.getNoteForPad(i);
                const bool root = midiNote >= 0 && midiNote % 12 == scaleManager.getRootNote() % 12;
                const bool lit = (i == selectedPad);
                if (midiNote < 0)   c = {};
                else if (root)      c = lit ? PadColour { 60, 90, 127 } : PadColour { 0, 20, 110 };
                else                c = lit ? PadColour { 90, 127, 90 } : PadColour { 0, 45, 8 };
            }
            out[(size_t) i] = c;
        }
        return true;
    }

    CustomScaleManager& getScaleManager() { return scaleManager; }

    // For testing: expose internal state
    int getSelectedPad() const { return selectedPad; }
    bool isBrowseMode() const { return browseMode; }
    void setBrowseMode(bool b) { browseMode = b; }

private:
    void renderNormal(DisplayRenderer& display)
    {
        // 2x8 pad grid in content zone
        for (int row = 0; row < 2; ++row)
        {
            for (int col = 0; col < 8; ++col)
            {
                int padIdx = row * 8 + col;
                int x = col * ScreenLayout::COLUMN_W;
                int y = ScreenLayout::CONTENT_Y + row * 21;

                int midiNote = scaleManager.getNoteForPad(padIdx);
                const juce::String noteName = NoteUtils::noteToString(midiNote);

                // A pad's column is 16 pixels, two characters: the note ("C#") on one line and its
                // octave ("4", "-1") under it. A whole name on one line ran into its neighbours.
                const int split = noteName.indexOfAnyOf("-0123456789");
                const juce::String letter = (split > 0 ? noteName.substring(0, split) : noteName).substring(0, 2);
                const juce::String octave = (split > 0 ? noteName.substring(split) : juce::String()).substring(0, 2);
                auto centred = [&](const juce::String& text, int ty)
                {
                    display.drawString(x + (ScreenLayout::COLUMN_W - text.length() * 6) / 2 + 1, ty, text);
                };
                centred(letter, y + 1);
                centred(octave, y + 9);

                // Mark root note
                if (midiNote >= 0 && midiNote % 12 == scaleManager.getRootNote() % 12)
                    display.drawHLine(x + 2, y + 18, 12);
            }
        }

        // Highlight selected pad
        if (selectedPad >= 0 && selectedPad < CustomScaleManager::PADS_PER_PAGE)
        {
            int row = selectedPad / 8;
            int col = selectedPad % 8;
            display.invertRegion(col * ScreenLayout::COLUMN_W,
                                 ScreenLayout::CONTENT_Y + row * 21,
                                 ScreenLayout::COLUMN_W, 21);
        }
    }

    void renderEditMode(DisplayRenderer& display)
    {
        // Top row: "EDIT" indicator and page name
        juce::String editLabel = "EDIT: ";
        editLabel += scaleManager.getPageName(scaleManager.getActivePage());
        display.drawString(2, ScreenLayout::CONTENT_Y + 1, editLabel);

        // Middle: show selected pad and its note
        int editPad = scaleManager.getEditPadIndex();
        if (editPad >= 0)
        {
            juce::String padInfo = "Pad ";
            padInfo += juce::String(editPad + 1);
            padInfo += ": ";

            int note = scaleManager.getNoteForPad(editPad);
            padInfo += NoteUtils::noteToString(note);

            display.drawString(2, ScreenLayout::CONTENT_Y + 11, padInfo);

            // Show "Press key to assign" hint
            display.drawString(2, ScreenLayout::CONTENT_Y + 21, "Key=assign");
        }
        else
        {
            display.drawString(2, ScreenLayout::CONTENT_Y + 11, "Tap pad...");
        }

        // Bottom: show 2x8 grid compact (just dot indicators)
        for (int col = 0; col < 8; ++col)
        {
            for (int row = 0; row < 2; ++row)
            {
                int padIdx = row * 8 + col;
                int x = col * ScreenLayout::COLUMN_W + 6;
                int y = ScreenLayout::CONTENT_Y + 31 + row * 5;       // rows 42-45 and 47-50: clear of the knob bar

                if (padIdx == editPad)
                    display.drawRect(x, y, 4, 4, true);
                else
                    display.drawRect(x, y, 4, 4, false);
            }
        }
    }

    void renderBrowser(DisplayRenderer& display)
    {
        // Show list of pages, 4 visible at a time
        int current = scaleManager.getActivePage();
        int startIdx = juce::jmax(0, current - 1);
        int endIdx = juce::jmin(scaleManager.getPageCount(), startIdx + 4);

        for (int i = startIdx; i < endIdx; ++i)
        {
            int yPos = ScreenLayout::CONTENT_Y + (i - startIdx) * 10 + 2;
            juce::String entry;
            if (i == current)
                entry += "> ";
            else
                entry += "  ";

            entry += juce::String(i + 1);
            entry += ":";
            entry += scaleManager.getPageName(i);

            juce::String rootStr = " ";
            rootStr += NoteUtils::noteToString(scaleManager.getRootNote(i));

            display.drawString(2, yPos, entry);
            display.drawString(90, yPos, rootStr);
        }
    }

    CustomScaleManager& scaleManager;
    int selectedPad = -1;
    bool browseMode = false;
    std::array<bool, 128> keyPlaying {};
    NoteCallback noteCallback;
    std::function<bool()> getLock;
    std::function<void(bool)> setLock;
    std::function<void(int)> noteOffCallback;
    std::function<void(int, int)> aftertouchCallback;
    std::array<int, 16> soundingNote = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
};
