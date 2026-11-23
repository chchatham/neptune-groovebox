#pragma once

#include "screens/SettingsScreen.h"
#include "../project/ProjectManager.h"
#include <array>
#include <functional>
#include <optional>
#include <vector>

// -----------------------------------------------------------------------------
// The project slots: what SETTINGS -> PROJ does with the folders P01 ... P32 of one root folder
// (~/Music/Neptune/Projects in the app), and what the Mac's two project windows are told.
//
//   LOAD   loads the slot SLOT shows.
//   SVAS   saves into the slot SLOT shows, at once; then the Mac asks for the name (`askName`).
//   SAVE   saves into the slot the project is in, at once, and the Mac asks in the same way: the box
//          offers the name the project has, Return keeps or changes it, Cancel puts back what the
//          slot held. A project that has no slot yet goes to the slot SLOT shows (the screen then
//          calls saveAs).
//
// A save never waits for the Mac. Until the box is answered the save is "pending"
// (ProjectManager::saveProjectAsPending keeps what the slot held beside it); loading, saving
// again or a new project settles it as it stands (`settled`: the box has nothing left to ask).
//
// rows() is the list the Mac shows (ui/ProjectBrowser.h): every used slot, its name, when its file
// was last written, and which one the current project is in.
//
// Everything here is called with the screens' lock held (the knobs come on the MIDI thread, the
// Mac's windows on the message thread). No window is touched from here: the owner's callbacks
// post to the message thread.
// -----------------------------------------------------------------------------

struct ProjectRow
{
    int slot = 0;                // 0-based
    juce::String name;           // empty = nobody has named it
    juce::Time written;          // when its file was last written
    bool current = false;        // the project that is playing came from, or was saved to, this slot

    juce::String slotName() const   { return SettingsScreen::slotName(slot); }
    // "P03  Night Drive", "P04  (no name)"
    juce::String text() const       { return slotName() + "  " + (name.isEmpty() ? juce::String("(no name)") : name); }
    // "21 Nov 2026 14:05"
    juce::String dateText() const   { return written.toMilliseconds() <= 0 ? juce::String() : written.formatted("%d %b %Y %H:%M"); }
};

class ProjectSlots
{
public:
    static constexpr int NUM_SLOTS = SettingsScreen::NUM_SLOTS;

    ProjectSlots(ProjectManager& manager, juce::File rootFolder) : pm(manager), root(std::move(rootFolder)) {}

    // ---- What the app adds --------------------------------------------------------------------
    std::function<void(int slot)> askName;       // a save has just been made into this slot: ask for its name
    std::function<void()> settled;               // the save the box asks about stands: the box can go
    std::function<void(int slot)> loaded;        // LOAD worked
    std::function<void(int slot)> remember;      // the slot the next start opens (-1 = none)
    std::function<bool()> writesNothing;         // the screen audit turns SAVE and SVAS too

    // ---- The slots ----------------------------------------------------------------------------
    juce::File dir(int slot) const      { return root.getChildFile(SettingsScreen::slotName(slot)); }
    bool used(int slot) const           { return dir(slot).getChildFile("project.json").existsAsFile(); }

    // The name of the project in a slot (read once; forgotten when the slot is written).
    juce::String title(int slot)
    {
        auto& t = titles[(size_t) slot];
        if (!t.has_value())
            t = ProjectManager::nameIn(dir(slot));
        return *t;
    }

    // The slot the current project is in; -1 = it has never been saved.
    int currentSlot() const
    {
        if (!pm.hasFile())
            return -1;
        for (int s = 0; s < NUM_SLOTS; ++s)
            if (pm.getCurrentFile() == dir(s))
                return s;
        return -1;
    }

    // Counts every change to what rows() gives.
    int version() const { return changes; }

    std::vector<ProjectRow> rows()
    {
        std::vector<ProjectRow> out;
        const int now = currentSlot();
        for (int s = 0; s < NUM_SLOTS; ++s)
        {
            if (!used(s))
                continue;
            ProjectRow row;
            row.slot = s;
            const auto name = title(s);
            row.name = SettingsScreen::isUnnamed(name) ? juce::String() : name;
            row.written = dir(s).getChildFile("project.json").getLastModificationTime();
            row.current = s == now;
            out.push_back(row);
        }
        return out;
    }

    // ---- The actions --------------------------------------------------------------------------
    bool load(int slot)
    {
        settle();
        const bool ok = pm.loadProject(dir(slot));
        if (ok)
        {
            if (remember) remember(slot);
            changed(slot);
            if (loaded) loaded(slot);
        }
        return ok;
    }

    // SVAS (and SAVE of a project without a slot).
    bool saveAs(int slot)
    {
        if (writesNothing && writesNothing())
            return true;
        const bool ok = pm.saveProjectAsPending(dir(slot));
        changed(slot);
        if (ok)
        {
            if (remember) remember(slot);
            if (askName) askName(slot);
        }
        return ok;
    }

    // SAVE. False = the project has no slot yet.
    bool saveCurrent()
    {
        if (writesNothing && writesNothing())
            return true;
        const int now = currentSlot();
        if (now < 0)
        {
            settle();
            return pm.saveProject();          // (false without a file; a file outside the slots is just saved)
        }
        return saveAs(now);
    }

    // LOAD, NEW or a save elsewhere with the box still open: the save it asks about stays as it is.
    void settle()
    {
        if (!pm.saveAsPending())
            return;
        pm.keepSaveAs();
        if (settled) settled();
    }

    // ---- The answers of the Mac's box ---------------------------------------------------------
    bool pendingIn(int slot) const { return pm.saveAsPending() && pm.pendingSaveDir() == dir(slot); }

    // Save / Return: the save stays, under this name. False = there was nothing left to name
    // (settled meanwhile) or the name is not usable.
    bool nameSave(int slot, const juce::String& typed)
    {
        const bool ok = pendingIn(slot) && pm.nameSaveAs(typed);
        changed(slot);
        return ok;
    }

    // Cancel / Escape: the slot holds what it held, the project is the one it was.
    bool takeBack(int slot)
    {
        if (!pendingIn(slot))
            return false;
        const bool ok = pm.takeBackSaveAs();
        changed(slot);
        if (remember) remember(currentSlot());
        return ok;
    }

    // What the box says for the save pending in a slot.
    struct BoxWords
    {
        juce::String slot;           // "P05"
        juce::String heading, offered, cancelHint;
        bool replaces = false;       // the slot held a project before this save

        juce::String saveHint(const juce::String& typed) const
        {
            const auto name = ProjectManager::cleanName(typed);
            return replaces ? "Return or Save: \"" + name + "\" replaces what " + slot + " held."
                            : "Return or Save: " + slot + " holds this project as \"" + name + "\".";
        }
    };

    BoxWords boxWords(int slot) const
    {
        BoxWords w;
        w.slot = SettingsScreen::slotName(slot);
        w.replaces = ProjectManager::keptIn(dir(slot)).existsAsFile();
        w.heading = "Save the project in " + w.slot;
        w.offered = SettingsScreen::isUnnamed(pm.getProjectName()) ? juce::String("Untitled") : pm.getProjectName();
        w.cancelHint = w.replaces ? "Escape or Cancel: " + w.slot + " keeps the project it held."
                                  : "Escape or Cancel: nothing is saved, " + w.slot + " stays empty.";
        return w;
    }

    // The slots have changed in a way this class did not make (a new project).
    void changed(int slot = -1)
    {
        if (slot >= 0 && slot < NUM_SLOTS)
            titles[(size_t) slot].reset();
        ++changes;
    }

    // The hooks of SETTINGS -> PROJ that are about slots.
    void bind(SettingsScreen::Hooks& h)
    {
        h.slotUsed    = [this](int slot) { return used(slot); };
        h.slotTitle   = [this](int slot) { return title(slot); };
        h.currentSlot = [this] { return currentSlot(); };
        h.loadSlot    = [this](int slot) { return load(slot); };
        h.saveToSlot  = [this](int slot) { return saveAs(slot); };
        h.saveCurrent = [this] { return saveCurrent(); };
    }

private:
    ProjectManager& pm;
    juce::File root;
    std::array<std::optional<juce::String>, NUM_SLOTS> titles;
    int changes = 0;
};
