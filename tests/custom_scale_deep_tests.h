#pragma once

// Custom Scale Deep Features tests: open-ended pages, keyboard page selection,
// edit mode, page-change transpose, copy/paste/clear, serialization, preset sharing.
//
// To include in the test suite, add to main_test.cpp:
//   #include "custom_scale_deep_tests.h"

#include "../src/controller/CustomScaleManager.h"
#include "../src/ui/screens/CustomScaleScreen.h"
#include "../src/sequencer/Step.h"
#include <juce_core/juce_core.h>

class CustomScaleDeepTests : public juce::UnitTest
{
public:
    CustomScaleDeepTests() : UnitTest("Custom Scale Deep") {}

    void runTest() override
    {
        // ================================================================
        // Task 1: Open-ended page count
        // ================================================================

        beginTest("Default page count is 12");
        {
            CustomScaleManager mgr;
            expectEquals(mgr.getPageCount(), 12);
        }

        beginTest("MAX_PAGES is 32");
        {
            expectEquals(CustomScaleManager::MAX_PAGES, 32);
        }

        beginTest("Factory pages are marked as factory");
        {
            CustomScaleManager mgr;
            for (int i = 0; i < 12; ++i)
                expect(mgr.isPageFactory(i), "Page " + juce::String(i) + " should be factory");
        }

        beginTest("Default pages have correct names");
        {
            CustomScaleManager mgr;
            expectEquals(juce::String(mgr.getPageName(0)), juce::String("Chrom"));
            expectEquals(juce::String(mgr.getPageName(1)), juce::String("Major"));
            expectEquals(juce::String(mgr.getPageName(2)), juce::String("Minor"));
            expectEquals(juce::String(mgr.getPageName(3)), juce::String("Penta"));
            expectEquals(juce::String(mgr.getPageName(4)), juce::String("Blues"));
            expectEquals(juce::String(mgr.getPageName(5)), juce::String("Whole"));
            expectEquals(juce::String(mgr.getPageName(6)), juce::String("Dorian"));
            expectEquals(juce::String(mgr.getPageName(7)), juce::String("Mixo"));
            expectEquals(juce::String(mgr.getPageName(8)), juce::String("HrMin"));
            expectEquals(juce::String(mgr.getPageName(9)), juce::String("mPent"));
            expectEquals(juce::String(mgr.getPageName(10)), juce::String("Phryg"));
            expectEquals(juce::String(mgr.getPageName(11)), juce::String("Lydia"));
        }

        beginTest("Can add user pages up to MAX_PAGES");
        {
            CustomScaleManager mgr;
            int initial = mgr.getPageCount();

            for (int i = initial; i < CustomScaleManager::MAX_PAGES; ++i)
            {
                int idx = mgr.addPage("User " + std::to_string(i));
                expect(idx >= 0, "Should be able to add page " + juce::String(i));
            }

            expectEquals(mgr.getPageCount(), CustomScaleManager::MAX_PAGES);

            // Adding beyond max should fail
            int overflow = mgr.addPage("Overflow");
            expectEquals(overflow, -1);
        }

        beginTest("Added pages are not factory");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("MyScale");
            expect(idx >= 0);
            expect(!mgr.isPageFactory(idx));
        }

        beginTest("Can remove non-factory pages");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("Removable");
            int countBefore = mgr.getPageCount();

            bool removed = mgr.removePage(idx);
            expect(removed);
            expectEquals(mgr.getPageCount(), countBefore - 1);
        }

        beginTest("Cannot remove factory pages");
        {
            CustomScaleManager mgr;
            bool removed = mgr.removePage(0); // Chromatic is factory
            expect(!removed);
            expectEquals(mgr.getPageCount(), 12);
        }

        beginTest("Page name can be changed");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("OldName");
            mgr.setPageName(idx, "NewName");
            expectEquals(juce::String(mgr.getPageName(idx)), juce::String("NewName"));
        }

        // ================================================================
        // Task 2: Keyboard page selection
        // ================================================================

        beginTest("Key select mode defaults to Off");
        {
            CustomScaleManager mgr;
            expect(mgr.getKeySelectMode() == CustomScaleManager::KeySelectMode::Off);
        }

        beginTest("Latched mode: key press selects page by root note");
        {
            CustomScaleManager mgr;
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Latched);

            // Page 0 has root C (60), page 6 (Dorian) has root D (62)
            // Press D key -- should select Dorian page
            int result = mgr.onKeyboardNoteForPageSelect(62);
            expectEquals(result, 6);
            expectEquals(mgr.getActivePage(), 6);
        }

        beginTest("Latched mode: page stays after key release");
        {
            CustomScaleManager mgr;
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Latched);

            mgr.onKeyboardNoteForPageSelect(62);  // D -> Dorian
            expectEquals(mgr.getActivePage(), 6);

            mgr.onKeyboardNoteReleasedForPageSelect(62);
            expectEquals(mgr.getActivePage(), 6); // still Dorian
        }

        beginTest("Momentary mode: returns to previous page on release");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(0);  // start on Chromatic
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Momentary);

            mgr.onKeyboardNoteForPageSelect(62);  // D -> Dorian
            expectEquals(mgr.getActivePage(), 6);

            mgr.onKeyboardNoteReleasedForPageSelect(62);
            expectEquals(mgr.getActivePage(), 0);  // back to Chromatic
        }

        beginTest("Key by page number: C3=page 0, C#3=page 1, etc");
        {
            CustomScaleManager mgr;
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Latched);
            mgr.setKeyByPageNumber(true);

            // MIDI 60 (C3) = page 0
            int result = mgr.onKeyboardNoteForPageSelect(60);
            expectEquals(result, 0);

            // MIDI 63 (D#3) = page 3
            result = mgr.onKeyboardNoteForPageSelect(63);
            expectEquals(result, 3);
        }

        beginTest("Key select returns -1 for unmapped notes");
        {
            CustomScaleManager mgr;
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Latched);
            mgr.setKeyByPageNumber(true);

            // MIDI 120 = page 60, which doesn't exist
            int result = mgr.onKeyboardNoteForPageSelect(120);
            expectEquals(result, -1);
        }

        beginTest("Off mode: key press does nothing");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(0);
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Off);

            int result = mgr.onKeyboardNoteForPageSelect(62);
            expectEquals(result, -1);
            expectEquals(mgr.getActivePage(), 0);
        }

        // ================================================================
        // Task 3: Page edit mode
        // ================================================================

        beginTest("Edit mode defaults to off");
        {
            CustomScaleManager mgr;
            expect(!mgr.isEditMode());
        }

        beginTest("Can enter and exit edit mode");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            expect(mgr.isEditMode());

            mgr.setEditMode(false);
            expect(!mgr.isEditMode());
        }

        beginTest("Edit mode: select pad then assign note");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            mgr.setActivePage(0);

            // Select pad 3
            mgr.setEditPadIndex(3);
            expectEquals(mgr.getEditPadIndex(), 3);

            // Assign note A4 (69) to pad 3
            mgr.assignNoteToEditPad(69);

            // In relative mode, stored as offset from root (60)
            expectEquals(mgr.getPage(0).notes[3], 9);
            expectEquals(mgr.getNoteForPad(3), 69);
        }

        beginTest("Edit mode: assign note in absolute mode");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("AbsTest");
            mgr.setActivePage(idx);
            mgr.setPageMode(idx, CustomScaleManager::Mode::Absolute);
            mgr.setEditMode(true);

            mgr.setEditPadIndex(0);
            mgr.assignNoteToEditPad(72);

            expectEquals(mgr.getPage(idx).notes[0], 72);
            expectEquals(mgr.getNoteForPad(0), 72);
        }

        beginTest("Edit mode: exiting clears pad selection");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            mgr.setEditPadIndex(5);
            expectEquals(mgr.getEditPadIndex(), 5);

            mgr.setEditMode(false);
            expectEquals(mgr.getEditPadIndex(), -1);
        }

        beginTest("Shift octave shifts all notes on page");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("OctTest");
            mgr.setActivePage(idx);
            mgr.setPageMode(idx, CustomScaleManager::Mode::Absolute);

            // Set notes
            for (int i = 0; i < CustomScaleManager::PADS_PER_PAGE; ++i)
                mgr.setNoteForPad(idx, i, 60 + i);

            mgr.shiftPageOctave(1);

            // All should be shifted up by 12
            for (int i = 0; i < CustomScaleManager::PADS_PER_PAGE; ++i)
                expectEquals(mgr.getPage(idx).notes[i], juce::jlimit(0, 127, 72 + i));
        }

        // ================================================================
        // Task 3 continued: Copy/paste/clear
        // ================================================================

        beginTest("Copy and paste page");
        {
            CustomScaleManager mgr;
            // Copy Major (page 1)
            mgr.copyPage(1);
            expect(mgr.hasClipboardData());

            // Paste to a new page
            int idx = mgr.addPage("PasteTarget");
            bool ok = mgr.pastePage(idx);
            expect(ok);

            // Verify notes match Major scale
            expectEquals(mgr.getPage(idx).notes[0], 0);
            expectEquals(mgr.getPage(idx).notes[1], 2);
            expectEquals(mgr.getPage(idx).notes[2], 4);

            // Pasted page should not be factory
            expect(!mgr.isPageFactory(idx));
        }

        beginTest("Paste fails without copy");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("NoPaste");
            bool ok = mgr.pastePage(idx);
            expect(!ok);
        }

        beginTest("Clear page resets to chromatic default");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("ToClear");
            mgr.setNoteForPad(idx, 0, 42);
            mgr.setNoteForPad(idx, 1, 99);

            mgr.clearPage(idx);

            expectEquals(juce::String(mgr.getPageName(idx)), juce::String("Empty"));
            expectEquals(mgr.getPage(idx).notes[0], 0);
            expectEquals(mgr.getPage(idx).notes[1], 1);
            expectEquals(mgr.getPage(idx).rootNote, 60);
        }

        // ================================================================
        // Task 4: Page change as global transpose
        // ================================================================

        beginTest("Transpose callback fires on page change with different roots");
        {
            CustomScaleManager mgr;
            int transposeSemitones = 0;
            mgr.setTransposeCallback([&](int semis) { transposeSemitones = semis; });

            mgr.setActivePage(0);  // C3 (root=60)
            transposeSemitones = 0;

            // Switch to Dorian (page 6, root=62/D)
            mgr.setActivePage(6);
            expectEquals(transposeSemitones, 2);  // D - C = +2
        }

        beginTest("Transpose callback not fired when roots are the same");
        {
            CustomScaleManager mgr;
            int callCount = 0;
            mgr.setTransposeCallback([&](int) { ++callCount; });

            mgr.setActivePage(0);  // C3, root=60
            callCount = 0;

            // Switch to Minor (page 2, also root=60)
            mgr.setActivePage(2);
            expectEquals(callCount, 0);
        }

        beginTest("getTransposeInterval computes interval between pages");
        {
            CustomScaleManager mgr;
            // Chromatic (root=60) to Dorian (root=62)
            expectEquals(mgr.getTransposeInterval(0, 6), 2);
            // Dorian (root=62) to Chromatic (root=60)
            expectEquals(mgr.getTransposeInterval(6, 0), -2);
            // Same page
            expectEquals(mgr.getTransposeInterval(0, 0), 0);
        }

        beginTest("Per-step page change lock field exists in Step");
        {
            Step s;
            expectEquals(s.pageChangeLock, -1);  // default is no lock

            s.pageChangeLock = 3;
            expectEquals(s.pageChangeLock, 3);

            s.clear();
            expectEquals(s.pageChangeLock, -1);
        }

        beginTest("getNoteForPadOnPage works for page-change locks");
        {
            CustomScaleManager mgr;
            // Page 0 (Chromatic, root 60), pad 0 should be 60
            expectEquals(mgr.getNoteForPadOnPage(0, 0), 60);

            // Page 1 (Major, root 60), pad 1 should be 62 (offset 2)
            expectEquals(mgr.getNoteForPadOnPage(1, 1), 62);

            // Invalid page
            expectEquals(mgr.getNoteForPadOnPage(-1, 0), -1);
            expectEquals(mgr.getNoteForPadOnPage(99, 0), -1);
        }

        // ================================================================
        // Task 5: CustomScaleScreen UI
        // ================================================================

        beginTest("Screen name is SCALE");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            expectEquals(screen.getName(), juce::String("SCALE"));
        }

        beginTest("Encoder names: normal mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            expectEquals(screen.getEncoderName(0), juce::String("PG"));
            expectEquals(screen.getEncoderName(1), juce::String("RT"));
            expectEquals(screen.getEncoderName(2), juce::String("MD"));
            expectEquals(screen.getEncoderName(3), juce::String("ED"));
            expectEquals(screen.getEncoderName(4), juce::String("KS"));
            expectEquals(screen.getEncoderName(5), juce::String("KM"));
        }

        beginTest("Encoder names: edit mode");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            CustomScaleScreen screen(mgr);

            expectEquals(screen.getEncoderName(0), juce::String("PG"));
            expectEquals(screen.getEncoderName(1), juce::String("RT"));
            expectEquals(screen.getEncoderName(2), juce::String("MD"));
            expectEquals(screen.getEncoderName(3), juce::String("ED"));
            expectEquals(screen.getEncoderName(4), juce::String("NT"));
            expectEquals(screen.getEncoderName(5), juce::String("PD"));
        }

        beginTest("Encoder 0 changes page");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            screen.onEncoderChange(0, 1);  // delta +1
            expectEquals(mgr.getActivePage(), 1);

            screen.onEncoderChange(0, 1);  // delta +1
            expectEquals(mgr.getActivePage(), 2);
        }

        beginTest("Encoder 1 changes root note");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            int initialRoot = mgr.getRootNote();
            screen.onEncoderChange(1, 2);  // delta +2
            expectEquals(mgr.getRootNote(), initialRoot + 2);
        }

        beginTest("Encoder 2 changes mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            // Start relative, turn right -> absolute
            screen.onEncoderChange(2, 1);
            expect(mgr.getPageMode(0) == CustomScaleManager::Mode::Absolute);

            // Turn left -> relative
            screen.onEncoderChange(2, 127);  // negative delta
            expect(mgr.getPageMode(0) == CustomScaleManager::Mode::Relative);
        }

        beginTest("Encoder 3 toggles edit mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            screen.onEncoderChange(3, 1);  // delta +1 -> enter edit
            expect(mgr.isEditMode());

            screen.onEncoderChange(3, 127);  // delta -1 -> exit edit
            expect(!mgr.isEditMode());
        }

        beginTest("Encoder 4 changes key select mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            expect(mgr.getKeySelectMode() == CustomScaleManager::KeySelectMode::Off);

            screen.onEncoderChange(4, 1);  // Off -> Latched
            expect(mgr.getKeySelectMode() == CustomScaleManager::KeySelectMode::Latched);

            screen.onEncoderChange(4, 1);  // Latched -> Momentary
            expect(mgr.getKeySelectMode() == CustomScaleManager::KeySelectMode::Momentary);

            screen.onEncoderChange(4, 127);  // Momentary -> Latched
            expect(mgr.getKeySelectMode() == CustomScaleManager::KeySelectMode::Latched);
        }

        beginTest("Pad press in edit mode selects pad");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            CustomScaleScreen screen(mgr);

            screen.onPadPress(7, 100);
            expectEquals(mgr.getEditPadIndex(), 7);
            expectEquals(screen.getSelectedPad(), 7);
        }

        beginTest("Pad release in edit mode keeps selection");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            CustomScaleScreen screen(mgr);

            screen.onPadPress(5, 100);
            screen.onPadRelease(5);
            expectEquals(screen.getSelectedPad(), 5);  // stays selected
        }

        beginTest("Keyboard note assigns in edit mode");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            mgr.setEditPadIndex(2);
            CustomScaleScreen screen(mgr);

            screen.onKeyboardNoteOn(72, 100);  // C4
            // In relative mode, stored as offset: 72 - 60 = 12
            expectEquals(mgr.getPage(0).notes[2], 12);
        }

        beginTest("Screen wants keyboard events in edit mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            expect(!screen.wantsKeyboardEvents());  // off by default

            mgr.setEditMode(true);
            expect(screen.wantsKeyboardEvents());
        }

        beginTest("Screen wants keyboard events when key select active");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Latched);
            expect(screen.wantsKeyboardEvents());
        }

        beginTest("Arrow buttons shift octave in edit mode");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("ArrowTest");
            mgr.setActivePage(idx);
            mgr.setPageMode(idx, CustomScaleManager::Mode::Absolute);
            for (int i = 0; i < CustomScaleManager::PADS_PER_PAGE; ++i)
                mgr.setNoteForPad(idx, i, 60 + i);

            mgr.setEditMode(true);
            CustomScaleScreen screen(mgr);

            // Control grammar: the bank buttons no longer shift the octave (that is the OC knob only),
            // and Up / Down change the scale page in edit mode too.
            screen.onArrowRight();
            screen.onArrowLeft();
            expectEquals(mgr.getPage(idx).notes[0], 60);
            expectEquals(mgr.getActivePage(), idx);
            screen.onPadPagePrev();
            expectEquals(mgr.getActivePage(), idx - 1);
            screen.onPadPageNext();
            expectEquals(mgr.getActivePage(), idx);
        }

        beginTest("Arrow buttons navigate pages in normal mode");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(2);
            CustomScaleScreen screen(mgr);

            // Up / Down (pad paging) change the scale page; the bank buttons do not.
            screen.onArrowRight();
            expectEquals(mgr.getActivePage(), 2);
            screen.onPadPageNext();
            expectEquals(mgr.getActivePage(), 3);

            screen.onPadPagePrev();
            expectEquals(mgr.getActivePage(), 2);
        }

        beginTest("Edit mode is on the ED knob only (the unreachable Shift+Scale handler is gone)");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            screen.setModifiers(true, false);
            screen.onButtonPress(0x4A);  // CC_SCALE
            expect(!mgr.isEditMode());
            screen.setModifiers(false, false);

            screen.onEncoderChange(3, 1);
            expect(mgr.isEditMode());
            screen.onEncoderChange(3, 127);
            expect(!mgr.isEditMode());
        }

        beginTest("SubPage shows edit indicator");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            juce::String normal = screen.getSubPage();
            expect(!normal.startsWithChar('*'));

            mgr.setEditMode(true);
            juce::String edited = screen.getSubPage();
            expect(edited.startsWithChar('*'));
        }

        beginTest("Copy/paste through screen helpers");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            mgr.setActivePage(1);  // Major
            screen.copyCurrentPage();

            int idx = mgr.addPage("Dest");
            mgr.setActivePage(idx);
            bool ok = screen.pasteToCurrentPage();
            expect(ok);

            expectEquals(mgr.getPage(idx).notes[1], 2);  // Major scale offset
        }

        beginTest("Clear through screen helper");
        {
            CustomScaleManager mgr;
            int idx = mgr.addPage("ToClear");
            mgr.setActivePage(idx);
            mgr.setNoteForPad(idx, 0, 42);

            CustomScaleScreen screen(mgr);
            screen.clearCurrentPage();

            expectEquals(mgr.getPage(idx).notes[0], 0);
            expectEquals(juce::String(mgr.getPageName(idx)), juce::String("Empty"));
        }

        beginTest("Pad colors: normal mode");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(0);  // Chromatic, root C
            CustomScaleScreen screen(mgr);

            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            // Pad 0 is the root (C): blue. Pad 1 is C#: green.
            expect(c[0].b > c[0].g && c[0].b > c[0].r);
            expect(c[1].g > c[1].b && c[1].g > c[1].r);
        }

        beginTest("Pad colors: edit mode");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            mgr.setEditPadIndex(3);
            CustomScaleScreen screen(mgr);

            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            // The pad being edited is white, the others dim red.
            expect(c[3].r == 127 && c[3].g == 127 && c[3].b == 127);
            expect(c[0].r > 0 && c[0].g == 0 && c[0].b == 0);
            expect(c[7].r > 0 && c[7].g == 0 && c[7].b == 0);
        }

        beginTest("Edit mode encoder: NT fine-tunes note");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            int idx = mgr.addPage("FineTune");
            mgr.setActivePage(idx);
            mgr.setEditPadIndex(0);

            CustomScaleScreen screen(mgr);

            int initialNote = mgr.getPage(idx).notes[0];
            screen.onEncoderChange(4, 3);  // NT, delta +3
            expectEquals(mgr.getPage(idx).notes[0], initialNote + 3);
        }

        beginTest("Edit mode encoder: PD selects pad");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            CustomScaleScreen screen(mgr);

            screen.onEncoderChange(5, 5);  // PD, delta +5 from -1 -> clamp to 4
            expectEquals(mgr.getEditPadIndex(), 5);
        }

        beginTest("Render does not crash in normal mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            DisplayRenderer display;

            // Should not throw
            screen.render(display);
            expect(true);
        }

        beginTest("Render does not crash in edit mode");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            mgr.setEditPadIndex(3);
            CustomScaleScreen screen(mgr);
            DisplayRenderer display;

            screen.render(display);
            expect(true);
        }

        beginTest("Render does not crash in browse mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            screen.setBrowseMode(true);
            DisplayRenderer display;

            screen.render(display);
            expect(true);
        }

        // ================================================================
        // Task 6: Data model / serialization
        // ================================================================

        beginTest("ScalePage struct default values");
        {
            ScalePage sp;
            expectEquals(sp.rootNote, 60);
            expect(!sp.isFactory);
            expect(sp.name.empty());
        }

        beginTest("ScalePage round-trips through var");
        {
            ScalePage original;
            original.name = "TestScale";
            original.rootNote = 72;
            original.isFactory = false;
            for (int i = 0; i < 16; ++i)
                original.padNotes[static_cast<size_t>(i)] = i * 2;

            juce::var v = original.toVar();
            ScalePage restored = ScalePage::fromVar(v);

            expectEquals(juce::String(restored.name), juce::String("TestScale"));
            expectEquals(restored.rootNote, 72);
            expect(!restored.isFactory);
            for (int i = 0; i < 16; ++i)
                expectEquals(restored.padNotes[static_cast<size_t>(i)], i * 2);
        }

        beginTest("PageChangeLock struct default");
        {
            PageChangeLock lock;
            expectEquals(lock.pageIndex, -1);
        }

        beginTest("CustomScaleManager serialization round-trip");
        {
            CustomScaleManager original;
            original.addPage("UserPage1");
            original.setNoteForPad(12, 0, 42);
            original.setRootNote(12, 72);
            original.setActivePage(3);

            juce::var v = original.toVar();

            CustomScaleManager restored;
            restored.fromVar(v);

            expectEquals(restored.getPageCount(), original.getPageCount());
            expectEquals(restored.getActivePage(), 3);
            expectEquals(juce::String(restored.getPageName(12)), juce::String("UserPage1"));
            expectEquals(restored.getPage(12).notes[0], 42);
            expectEquals(restored.getRootNote(12), 72);
        }

        beginTest("Export and import page preset");
        {
            CustomScaleManager mgr;
            // Export Major page
            juce::var preset = mgr.exportPagePreset(1);
            expect(!preset.isVoid());

            // Import into a fresh manager
            CustomScaleManager mgr2;
            int idx = mgr2.importPagePreset(preset);
            expect(idx >= 0);

            // Verify Major scale notes
            expectEquals(mgr2.getPage(idx).notes[0], 0);
            expectEquals(mgr2.getPage(idx).notes[1], 2);
            expectEquals(mgr2.getPage(idx).notes[2], 4);
            expectEquals(mgr2.getPage(idx).notes[3], 5);
        }

        beginTest("Import rejects invalid preset");
        {
            CustomScaleManager mgr;

            // Wrong type
            auto* obj = new juce::DynamicObject();
            obj->setProperty("type", "NotAScalePage");
            int idx = mgr.importPagePreset(juce::var(obj));
            expectEquals(idx, -1);
        }

        beginTest("Export returns empty for invalid index");
        {
            CustomScaleManager mgr;
            juce::var v = mgr.exportPagePreset(99);
            expect(v.isVoid());
        }

        // ================================================================
        // Additional edge cases
        // ================================================================

        beginTest("Active page stays valid after removing pages");
        {
            CustomScaleManager mgr;
            // Add pages then remove some
            for (int i = 0; i < 5; ++i)
                mgr.addPage("Temp" + std::to_string(i));

            int lastIdx = mgr.getPageCount() - 1;
            mgr.setActivePage(lastIdx);

            mgr.removePage(lastIdx);
            expect(mgr.getActivePage() < mgr.getPageCount());
        }

        beginTest("getNoteForPad clamps to 0-127 in relative mode");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(0);  // Chromatic
            mgr.setRootNote(0, 120);  // high root

            // Pad 15 has offset 15, so note = 120 + 15 = 135 -> clamped to 127
            int note = mgr.getNoteForPad(15);
            expect(note <= 127);
        }

        beginTest("Note callback fires on normal pad press");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);

            int receivedNote = -1;
            int receivedVel = -1;
            screen.setNoteCallback([&](int note, int vel) {
                receivedNote = note;
                receivedVel = vel;
            });

            screen.onPadPress(0, 100);
            expectEquals(receivedNote, 60);  // Chromatic pad 0 = root C3
            expectEquals(receivedVel, 100);
        }

        beginTest("Note callback does not fire in edit mode");
        {
            CustomScaleManager mgr;
            mgr.setEditMode(true);
            CustomScaleScreen screen(mgr);

            bool callbackFired = false;
            screen.setNoteCallback([&](int, int) { callbackFired = true; });

            screen.onPadPress(0, 100);
            expect(!callbackFired);
        }

        beginTest("Dorian scale intervals are correct");
        {
            CustomScaleManager mgr;
            // Dorian: 0 2 3 5 7 9 10 12 ...
            auto& page = mgr.getPage(6);
            expectEquals(page.notes[0], 0);
            expectEquals(page.notes[1], 2);
            expectEquals(page.notes[2], 3);
            expectEquals(page.notes[3], 5);
            expectEquals(page.notes[4], 7);
            expectEquals(page.notes[5], 9);
            expectEquals(page.notes[6], 10);
            expectEquals(page.notes[7], 12);
        }

        beginTest("Harmonic minor scale intervals are correct");
        {
            CustomScaleManager mgr;
            // HrMin: 0 2 3 5 7 8 11 12 ...
            auto& page = mgr.getPage(8);
            expectEquals(page.notes[0], 0);
            expectEquals(page.notes[1], 2);
            expectEquals(page.notes[2], 3);
            expectEquals(page.notes[3], 5);
            expectEquals(page.notes[4], 7);
            expectEquals(page.notes[5], 8);
            expectEquals(page.notes[6], 11);
            expectEquals(page.notes[7], 12);
        }
    }
};

static CustomScaleDeepTests customScaleDeepTests;
