#pragma once

// ---------------------------------------------------------------------------
// The Mac's two project windows and what SETTINGS -> PROJ tells them:
//   - SAVE and SVAS save at once and are then asked about (ui/ProjectSlots.h, ui/SaveBox.h);
//   - the list of projects, "Projects" (ui/ProjectBrowser.h).
// Everything is written under a temporary folder, never under ~/Music/Neptune.
// ---------------------------------------------------------------------------

#include "../src/ui/ProjectSlots.h"
#include "../src/ui/ProjectBrowser.h"
#include "../src/ui/SaveBox.h"

class ProjectBoxesTests : public juce::UnitTest
{
public:
    ProjectBoxesTests() : UnitTest("Project Boxes") {}

    // The PROJ page as the app wires it (main.cpp): the screen's hooks are ProjectSlots'.
    struct Rig
    {
        juce::File root;
        ProjectManager pm;
        ProjectSlots slots;
        SettingsScreen screen;
        juce::int64 now = 1000;
        std::vector<int> asked, loadedSlots, remembered;
        int settledCount = 0, rebuilds = 0;
        juce::String rebuiltName;
        bool audit = false;

        explicit Rig(const juce::String& folder)
            : root(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(folder)),
              slots(pm, root)
        {
            root.deleteRecursively();
            pm.onCapture = [this] { return pm.getData(); };
            pm.onRebuild = [this](const ProjectData& d) { ++rebuilds; rebuiltName = d.name; };
            pm.newProject();
            rebuilds = 0;
            slots.askName = [this](int s) { asked.push_back(s); };
            slots.settled = [this] { ++settledCount; };
            slots.loaded = [this](int s) { loadedSlots.push_back(s); };
            slots.remember = [this](int s) { remembered.push_back(s); };
            slots.writesNothing = [this] { return audit; };
            SettingsScreen::Hooks h;
            slots.bind(h);
            h.newProject = [this] { slots.settle(); pm.newProject(); slots.changed(); };
            screen.setHooks(h);
            screen.setClock([this] { return now += 100; });       // 100 ms between clicks
            while (screen.getName() != "PROJ")
                screen.onArrowRight();
        }
        ~Rig() { pm.keepSaveAs(); root.deleteRecursively(); }

        // One turn of an action knob (1 LOAD, 2 SAVE, 3 SVAS, 4 NEW), after a rest.
        void turn(int knob, int clicks = SettingsScreen::ACTION_STEPS)
        {
            now += 2000;
            for (int i = 0; i < clicks; ++i)
                screen.onEncoderChange(knob, 1);
        }
        void toSlot(int slot)
        {
            screen.onEncoderChange(0, 127);       // (any turn of SLOT; then the slot is set)
            screen.selectSlot(slot);
        }
        juce::File file(int slot) const   { return slots.dir(slot).getChildFile("project.json"); }
        juce::String bytes(int slot) const { return file(slot).loadFileAsString(); }
        float bpmIn(int slot) const       { return ProjectData::fromJSON(bytes(slot)).bpm; }

        // A project put in a slot behind the app's back (as files from an earlier session are).
        void plant(int slot, const juce::String& name, float bpm = 120.0f)
        {
            ProjectData d = pm.getData();
            d.name = name;
            d.bpm = bpm;
            slots.dir(slot).createDirectory();
            file(slot).replaceWithText(d.toJSON());
            slots.changed(slot);
        }
    };

    void runTest() override
    {
        beginTest("SAVE of a project that has a slot: saved at once, then asked about; Cancel puts back what the slot held");
        {
            Rig rig("neptune_test_project_boxes_save");
            rig.pm.getData().bpm = 100.0f;
            rig.toSlot(2);
            rig.turn(3);                                         // SVAS into P03
            expect(rig.asked == std::vector<int>{ 2 });
            expect(rig.slots.pendingIn(2));
            {
                const auto w = rig.slots.boxWords(2);
                expectEquals(w.heading, juce::String("Save the project in P03"));
                expectEquals(w.offered, juce::String("Untitled"));
                expect(!w.replaces);
                expectEquals(w.cancelHint, juce::String("Escape or Cancel: nothing is saved, P03 stays empty."));
                expectEquals(w.saveHint(" Night Drive "), juce::String("Return or Save: P03 holds this project as \"Night Drive\"."));
            }
            expect(rig.slots.nameSave(2, "Night Drive"));
            expect(!rig.pm.saveAsPending());
            expectEquals(rig.slots.currentSlot(), 2);
            const auto held = rig.bytes(2);
            expectEquals(rig.bpmIn(2), 100.0f);

            // Played on, then SAVE: in the slot at once (the controller alone is enough) ...
            rig.pm.getData().bpm = 140.0f;
            rig.turn(2);
            expect(rig.asked == (std::vector<int>{ 2, 2 }), "SAVE asks too");
            expectEquals(rig.bpmIn(2), 140.0f, "saved before anything is answered");
            expect(rig.slots.pendingIn(2));
            expect(ProjectManager::keptIn(rig.slots.dir(2)).existsAsFile());
            // ... and the box says which slot, that it replaces what the slot held, and offers the name it has.
            {
                const auto w = rig.slots.boxWords(2);
                expectEquals(w.heading, juce::String("Save the project in P03"));
                expectEquals(w.offered, juce::String("Night Drive"));
                expect(w.replaces);
                expectEquals(w.saveHint("Night Drive"), juce::String("Return or Save: \"Night Drive\" replaces what P03 held."));
                expectEquals(w.cancelHint, juce::String("Escape or Cancel: P03 keeps the project it held."));
            }
            // Cancel: the slot's file is byte for byte what it was; what is playing is not touched.
            expect(rig.slots.takeBack(2));
            expect(rig.bytes(2) == held, "the slot holds what it held");
            expect(!ProjectManager::keptIn(rig.slots.dir(2)).exists());
            expect(!rig.pm.saveAsPending());
            expectEquals(rig.slots.currentSlot(), 2);
            expectEquals(rig.pm.getProjectName(), juce::String("Night Drive"));
            expectEquals(rig.pm.getData().bpm, 140.0f);
            expect(!rig.slots.takeBack(2), "answered: nothing left to take back");

            // SAVE again, Return on the name as offered: the new save stays, under the same name.
            rig.turn(2);
            expectEquals((int) rig.asked.size(), 3);
            expect(rig.slots.nameSave(2, rig.slots.boxWords(2).offered));
            expectEquals(rig.bpmIn(2), 140.0f);
            expectEquals(ProjectManager::nameIn(rig.slots.dir(2)), juce::String("Night Drive"));
            expect(!ProjectManager::keptIn(rig.slots.dir(2)).exists());
            expect(!rig.pm.saveAsPending());

            // ... or under another name.
            rig.turn(2);
            expect(rig.slots.nameSave(2, "Day Drive"));
            expectEquals(rig.slots.title(2), juce::String("Day Drive"));
            expectEquals(rig.pm.getProjectName(), juce::String("Day Drive"));
            expect(!rig.slots.nameSave(2, "Again"), "answered: nothing left to name");

            // A box that is never answered: the save stands (the Mac is never needed).
            rig.pm.getData().bpm = 90.0f;
            rig.turn(2);
            expectEquals(rig.bpmIn(2), 90.0f);

            // The screen audit turns SAVE and SVAS with these hooks: nothing is written, nothing asked.
            rig.pm.keepSaveAs();
            const auto before = rig.bytes(2);
            const auto askedBefore = rig.asked.size();
            rig.audit = true;
            rig.pm.getData().bpm = 60.0f;
            rig.turn(2);
            rig.toSlot(7);
            rig.turn(3);
            expect(rig.bytes(2) == before && !rig.slots.used(7) && rig.asked.size() == askedBefore);
        }

        beginTest("SAVE of a project without a slot goes to the slot SLOT shows; a slot that held another project gets it back on Cancel");
        {
            Rig rig("neptune_test_project_boxes_noslot");
            rig.plant(4, "Old Song", 77.0f);
            const auto held = rig.bytes(4);
            rig.pm.getData().bpm = 133.0f;
            rig.toSlot(4);
            rig.turn(2);                                         // SAVE: no slot yet -> P05, over "Old Song"
            expect(rig.asked == std::vector<int>{ 4 });
            expectEquals(rig.bpmIn(4), 133.0f);
            expect(rig.slots.boxWords(4).replaces);
            expectEquals(rig.slots.currentSlot(), 4);
            expect(rig.slots.takeBack(4));
            expect(rig.bytes(4) == held);
            expectEquals(rig.slots.currentSlot(), -1, "the project is unsaved again");
            expect(!rig.remembered.empty() && rig.remembered.back() == -1);
            expectEquals(rig.slots.title(4), juce::String("Old Song"));
        }

        beginTest("One turn of the knob saves once: Cancel after a long turn still takes the save back");
        {
            // What was wrong: a turn of 12 clicks ran SVAS three times; the second save made the
            // first one "what the slot held", so Cancel left the project in a slot the box called empty.
            Rig rig("neptune_test_project_boxes_turn");
            rig.toSlot(5);
            rig.turn(3, 12);
            expectEquals((int) rig.asked.size(), 1, "one turn, one save");
            expect(!rig.slots.boxWords(5).replaces, "the slot was empty before this turn");
            expect(rig.slots.takeBack(5));
            expect(!rig.slots.dir(5).exists(), "nothing is saved, P06 stays empty");
            expectEquals(rig.slots.currentSlot(), -1);

            // The same for SAVE over a project: a long turn, Cancel, and the slot holds what it held.
            rig.turn(3);
            expect(rig.slots.nameSave(5, "Keeper"));
            const auto held = rig.bytes(5);
            rig.pm.getData().bpm = 150.0f;
            rig.turn(2, 12);
            expectEquals((int) rig.asked.size(), 3);
            expect(rig.slots.takeBack(5));
            expect(rig.bytes(5) == held);

            // After a rest, turning back, or another knob, the knob acts again.
            rig.turn(2);
            expectEquals((int) rig.asked.size(), 4);
            rig.screen.onEncoderChange(2, 127);                  // back
            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                rig.screen.onEncoderChange(2, 1);
            expectEquals((int) rig.asked.size(), 5);
            rig.screen.onEncoderChange(5, 1);                    // any other knob (cancels the confirm)
            for (int i = 0; i < SettingsScreen::ACTION_STEPS; ++i)
                rig.screen.onEncoderChange(2, 1);
            expectEquals((int) rig.asked.size(), 6);
            // LOAD and NEW run once per turn as well.
            rig.pm.keepSaveAs();
            rig.turn(1, 12);
            expectEquals(rig.rebuilds, 1);
        }

        beginTest("Nothing but a project action settles a save before its box opens; a load does, and says so");
        {
            Rig rig("neptune_test_project_boxes_settle");
            rig.plant(0, "First");
            rig.toSlot(3);
            rig.turn(3);
            // (what runs between the save and the box, while the sequencer plays: the autosave check)
            rig.pm.setAutosaveInterval(0);
            rig.pm.markDirty();
            rig.pm.autosaveCheck();
            expect(rig.slots.pendingIn(3), "the box still has its save to ask about");
            expectEquals(rig.settledCount, 0);

            // LOAD with the box open: the save stands, the box is told to go.
            rig.toSlot(0);
            rig.turn(1);
            expectEquals(rig.settledCount, 1);
            expect(!rig.slots.pendingIn(3) && rig.slots.used(3));
            expect(!rig.slots.takeBack(3) && !rig.slots.nameSave(3, "Late"), "the box's answers do nothing now");
            expectEquals(rig.slots.currentSlot(), 0);
            // A save of another slot's project does not answer for this one.
            rig.turn(2);
            expect(rig.slots.pendingIn(0) && !rig.slots.pendingIn(3));
            // NEW settles it too.
            rig.turn(4);
            expectEquals(rig.settledCount, 2);
            expect(!rig.pm.saveAsPending());
        }

        beginTest("The list of projects: every used slot in order, its name, its date, the current one marked");
        {
            Rig rig("neptune_test_project_boxes_rows");
            expect(rig.slots.rows().empty());
            rig.plant(30, "Zed");
            rig.plant(1, "B Song");
            rig.plant(8, "P09");                                 // a slot's own name: nobody named it
            rig.plant(4, "Untitled");
            expect(rig.slots.load(4));
            const auto rows = rig.slots.rows();
            expectEquals((int) rows.size(), 4, "empty slots are left out");
            if (rows.size() == 4)
            {
                const int slotsWanted[4] = { 1, 4, 8, 30 };
                const char* texts[4] = { "P02  B Song", "P05  (no name)", "P09  (no name)", "P31  Zed" };
                for (int i = 0; i < 4; ++i)
                {
                    expectEquals(rows[(size_t) i].slot, slotsWanted[i]);
                    expectEquals(rows[(size_t) i].text(), juce::String(texts[i]));
                    expect(rows[(size_t) i].current == (i == 1), texts[i]);
                    expect(rows[(size_t) i].written == rig.file(slotsWanted[i]).getLastModificationTime());
                    expectEquals(rows[(size_t) i].dateText().length(), 17, rows[(size_t) i].dateText());
                }
            }
            ProjectRow dated;
            dated.written = juce::Time(2026, 10, 21, 14, 5);     // (month 0-based)
            expectEquals(dated.dateText(), juce::String("21 Nov 2026 14:05"));
            expectEquals(ProjectRow().dateText(), juce::String());

            // The list follows what is done: a name, a save into a new slot, a save taken back.
            const int v = rig.slots.version();
            rig.toSlot(4);
            rig.turn(2);
            expect(rig.slots.nameSave(4, "Named Now"));
            expect(rig.slots.version() != v);
            expectEquals(rig.slots.rows()[1].text(), juce::String("P05  Named Now"));
            rig.toSlot(2);
            rig.turn(3);
            expectEquals((int) rig.slots.rows().size(), 5);
            expect(rig.slots.rows()[1].current && !rig.slots.rows()[2].current, "P03 has it now");
            expect(rig.slots.takeBack(2));
            expectEquals((int) rig.slots.rows().size(), 4);
            expect(rig.slots.rows()[1].current);
        }

        beginTest("The Projects window: the highlight follows SLOT, a click moves SLOT, double-click or Return loads as LOAD does");
        {
            const juce::ScopedJuceInitialiser_GUI gui;
            Rig rig("neptune_test_project_boxes_window");
            rig.plant(1, "B Song", 101.0f);
            rig.plant(4, "Untitled");
            rig.plant(8, "Night Drive 13");
            rig.plant(30, "Zed", 99.0f);
            expect(rig.slots.load(4));
            rig.rebuilds = 0;
            rig.loadedSlots.clear();
            rig.toSlot(4);

            ProjectBrowser browser;
            std::vector<int> selected;
            int closes = 0;
            // As the app wires it (main.cpp, openProjectBrowser).
            browser.onSelect = [&](int slot) { selected.push_back(slot); rig.screen.selectSlot(slot); };
            browser.onLoad = [&](int slot) { rig.screen.loadSlot(slot); };
            browser.onClose = [&] { ++closes; };

            browser.setRows(rig.slots.rows(), rig.screen.getSelectedSlot());
            expectEquals(browser.getList().getListBoxModel()->getNumRows(), 4);
            expectEquals(browser.getHighlightedSlot(), 4, "the row of the slot SLOT shows");
            // The knob moves SLOT: the highlight follows, and that is not a choice made on the Mac.
            rig.screen.onEncoderChange(0, 4);                    // P09
            browser.showSlot(rig.screen.getSelectedSlot());
            expectEquals(browser.getHighlightedSlot(), 8);
            rig.screen.onEncoderChange(0, 1);                    // P10: empty
            browser.showSlot(rig.screen.getSelectedSlot());
            expectEquals(browser.getHighlightedSlot(), -1, "an empty slot has no row");
            expect(selected.empty(), "following the knob tells nobody");
            expectEquals(rig.screen.getSelectedSlot(), 9);

            // A click on a row: SLOT goes there, so the controller's display shows it.
            browser.clickRow(0);
            expect(selected == std::vector<int>{ 1 });
            expectEquals(rig.screen.getSelectedSlot(), 1);
            expectEquals(rig.screen.getStatusLine(), juce::String("P02 B Song"));
            expectEquals(rig.rebuilds, 0, "a click loads nothing");

            // The picture, for looking at (rows, the highlight, NOW, the dates, the words below).
            {
                auto file = juce::File(juce::SystemStats::getEnvironmentVariable("NEPTUNE_PROJECTS_PNG", {}));
                if (file.getFullPathName().isEmpty())
                    file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("neptune-projects-window.png");
                expect(browser.writePng(file), "the picture is written to " + file.getFullPathName());
                expect(browser.getLocalBounds().contains(browser.getList().getBounds()));
                expect(browser.getHint().contains("Double-click or Return: load") && browser.getHint().contains("Escape: close"));
            }

            // Double-click with a save still waiting for its answer: settled as LOAD settles it, then loaded.
            rig.turn(2);                                         // SAVE of P05, box unanswered
            expect(rig.slots.pendingIn(4));
            browser.loadRow(3);                                  // P31
            expectEquals(rig.settledCount, 1);
            expect(!rig.pm.saveAsPending());
            expectEquals(rig.slots.currentSlot(), 30);
            expectEquals(rig.rebuilds, 1, "the app rebuilds (and forgets its undo history) as for LOAD");
            expectEquals(rig.rebuiltName, juce::String("Zed"));
            expectEquals(rig.pm.getData().bpm, 99.0f);
            expect(rig.loadedSlots == std::vector<int>{ 30 }, "the app is told: the window closes");
            expectEquals(rig.screen.getSelectedSlot(), 30);
            expect(!rig.remembered.empty() && rig.remembered.back() == 30);
            expectEquals(rig.screen.getActionResult(1), 1, "the LOAD cell says OK");

            // The same from the knob, for comparison: one rebuild, one "loaded".
            rig.toSlot(1);
            rig.turn(1);
            expectEquals(rig.rebuilds, 2);
            expect(rig.loadedSlots == (std::vector<int>{ 30, 1 }));

            // Return loads the highlighted row; Escape closes.
            browser.setRows(rig.slots.rows(), rig.screen.getSelectedSlot());
            expect(rig.slots.rows()[0].current);
            selected.clear();
            browser.clickRow(2);
            expect(selected == std::vector<int>{ 8 });
            expect(browser.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            expectEquals(rig.slots.currentSlot(), 8);
            expectEquals(rig.rebuilds, 3);
            expect(browser.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
            expectEquals(closes, 1);
            expect(!browser.keyPressed(juce::KeyPress('x')), "other keys are left to whoever wants them");

            // Nothing saved yet: no rows, and it says what to do.
            ProjectBrowser empty;
            empty.setRows({}, 0);
            expectEquals(empty.getHighlightedSlot(), -1);
            expect(!empty.getList().isVisible());
            empty.loadRow(0);
            expect(!empty.keyPressed(juce::KeyPress('x')));
            auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("neptune-projects-window-empty.png");
            expect(empty.writePng(file));

            // Where the window goes: beside the main window, or over it when the screen has no room.
            const juce::Rectangle<int> screen(0, 25, 1440, 875);
            const auto beside = ProjectBrowserWindow::placeBeside({ 100, 100, 700, 500 }, screen, ProjectBrowser::WIDTH, 400);
            expectEquals(beside.getX(), 812);
            expectEquals(beside.getY(), 100);
            const auto over = ProjectBrowserWindow::placeBeside({ 700, 100, 700, 500 }, screen, ProjectBrowser::WIDTH, 400);
            expect(screen.contains(over) && over.intersects(juce::Rectangle<int>(700, 100, 700, 500)));
        }

        beginTest("The save box with SAVE's words: all of them fit");
        {
            const juce::ScopedJuceInitialiser_GUI gui;
            Rig rig("neptune_test_project_boxes_words");
            rig.toSlot(31);
            rig.turn(3);
            expect(rig.slots.nameSave(31, "Night Drive 13"));     // (cut to the 13 characters a name has)
            rig.turn(2);
            const auto words = rig.slots.boxWords(31);
            // Built as the app builds it (main.cpp, askProjectName).
            SaveBox box(words.heading, words.offered, ProjectManager::MAX_NAME_LENGTH);
            box.hintFor = [words](SaveBox::NameUse use, const juce::String& typed)
            {
                return use == SaveBox::NameUse::New ? words.saveHint(typed) : juce::String();
            };
            box.setCancelHint(words.cancelHint);
            box.onSave = [&](const juce::String& typed) { rig.slots.nameSave(31, typed); return true; };
            box.onCancel = [&] { rig.slots.takeBack(31); };
            box.refresh();
            expectEquals(box.getNameEditor().getText(), juce::String("Night Drive 1"));
            expectEquals(box.getHint(), juce::String("Return or Save: \"Night Drive 1\" replaces what P32 held."));
            expectEquals(box.getCancelHint(), juce::String("Escape or Cancel: P32 keeps the project it held."));
            const juce::Font font(juce::FontOptions(14.0f));
            const int room = SaveBox::WIDTH - 40 - 10;            // (the box's margins and the label's own)
            for (auto& line : { box.getHint(), box.getCancelHint(), words.saveHint("WWWWWWWWWWWWW") })
                expect(juce::GlyphArrangement::getStringWidthInt(font, line) <= room, line);
            auto file = juce::File(juce::SystemStats::getEnvironmentVariable("NEPTUNE_PROJECT_SAVE_BOX_PNG", {}));
            if (file.getFullPathName().isEmpty())
                file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("neptune-project-save-box.png");
            expect(box.writePng(file));
            // Escape: the slot holds what it held.
            const auto kept = ProjectManager::keptIn(rig.slots.dir(31)).loadFileAsString();
            expect(box.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
            expect(rig.bytes(31) == kept && !rig.pm.saveAsPending());
        }
    }
};

static ProjectBoxesTests projectBoxesTests;
