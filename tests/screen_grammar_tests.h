#pragma once
#include <juce_core/juce_core.h>
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/ui/screens/SongScreen.h"
#include "../src/ui/screens/PresetScreen.h"
#include "../src/ui/screens/CustomScaleScreen.h"
#include "../src/ui/screens/DrumPadScreen.h"
#include "../src/ui/screens/ArpScreen.h"
#include "../src/ui/screens/SettingsScreen.h"
#include "../src/ui/screens/WaveformScreen.h"
#include "../src/ui/screens/AftertouchScreen.h"
#include "../src/project/ProjectBridge.h"

// The control grammar (docs/launchkey-mk4-control-map.md "CONTROL GRAMMAR") on every screen other than
// PARAM / TRK FX / FLTR / TRACKS, and pad-pressure routing per track. The screens' methods are
// called directly, with setModifiers(shift, func) standing in for the held buttons.
class ScreenGrammarTests : public juce::UnitTest
{
public:
    ScreenGrammarTests() : UnitTest("Screen Grammar") {}

    static int enc(int clicks) { return clicks >= 0 ? clicks : 128 + clicks; }
    static bool same(Screen::PadColour a, Screen::PadColour b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
    static int brightness(Screen::PadColour c) { return (int) c.r + (int) c.g + (int) c.b; }
    static bool dark(Screen::PadColour c) { return brightness(c) == 0; }

    struct StepRig
    {
        SequencerCore seq;
        AudioEngine audio;
        PadManager pads;
        PadLEDFeedback leds;
        std::unique_ptr<ParamPageScreen> params;
        std::unique_ptr<StepViewScreen> steps;
        juce::int64 now = 10000;

        explicit StepRig(bool withLockPages = true)
        {
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            steps = std::make_unique<StepViewScreen>(seq, pads, leds);
            steps->setClock([this] { return now; });
            if (withLockPages)
            {
                params = std::make_unique<ParamPageScreen>(seq, &audio);
                steps->setLockPages(params.get());
            }
        }
        Step& step(int i) { return seq.getPattern().getTrack(0).getStep(i); }
    };

    void runTest() override
    {
        stepTests();
        songTests();
        presetTests();
        scaleTests();
        drumTests();
        arpAndSettingsTests();
        waveTests();
        pressureRoutingTests();
    }

    // ============================================ STEP ============================================
    void stepTests()
    {
        beginTest("STEP: a trig is added on press; only a short tap with nothing edited removes one");
        {
            StepRig rig;
            auto& s = rig.step(2);
            expect(!s.trig);

            rig.steps->onPadPress(2, 100);
            expect(s.trig, "added on press");
            expect(rig.steps->isStepHeld());
            rig.now += 100;
            rig.steps->onPadRelease(2);
            expect(s.trig, "the tap that added it does not remove it");
            expect(!rig.steps->isStepHeld());

            // Held longer than 350 ms: kept.
            rig.steps->onPadPress(2, 100);
            rig.now += 500;
            rig.steps->onPadRelease(2);
            expect(s.trig, "a held step is never removed");

            // Short, but a knob was turned while it was down: kept.
            rig.steps->onPadPress(2, 100);
            rig.now += 50;
            rig.steps->onEncoderChange(2, enc(3));
            rig.now += 50;
            rig.steps->onPadRelease(2);
            expect(s.trig, "hold + turn keeps the step");

            // Short, but a key was played into it: kept.
            rig.steps->onPadPress(2, 100);
            rig.steps->onKeyboardNoteOn(67, 90);
            rig.now += 100;
            rig.steps->onPadRelease(2);
            expect(s.trig);
            expectEquals(s.notes[0].pitch, 67);

            // A short tap with nothing edited removes it.
            rig.steps->onPadPress(2, 100);
            expect(s.trig, "still there while the pad is down");
            rig.now += 200;
            rig.steps->onPadRelease(2);
            expect(!s.trig, "short tap removes");
        }

        beginTest("STEP: Shift changes nothing on a held step; Shift / Func + turn are fine / coarse locks");
        {
            StepRig rig;
            auto& s = rig.step(4);
            rig.steps->onPadPress(4, 100);
            rig.steps->setModifiers(true, false);
            rig.steps->onShiftChanged(true);
            rig.steps->onShiftChanged(false);
            rig.steps->onShiftChanged(true);
            expect(s.trigCondition.type == TrigCondition::Always, "no trig-condition cycling on Shift");
            expect(s.paramLocks.empty());

            // Shift + turn: a fine lock on the shown parameter (RESO on the MAIN page), no raw ids.
            rig.steps->onEncoderChange(1, enc(10));
            expectEquals((int) s.paramLocks.size(), 1);
            expect(s.paramLocks.count(UnifiedParam::RESONANCE) == 1, "the lock is on the parameter the page shows");
            const float fine = s.paramLocks[UnifiedParam::RESONANCE] - 0.2f;
            expectWithinAbsoluteError(fine, 0.01f, 0.002f, "ten fine clicks = 1 %");

            rig.steps->setModifiers(false, false);
            rig.steps->onShiftChanged(false);
            rig.steps->onEncoderChange(1, enc(10));
            expectWithinAbsoluteError(s.paramLocks[UnifiedParam::RESONANCE], 0.2f + fine + 0.1f, 0.002f, "plain: 1 % a click");

            rig.steps->setModifiers(false, true);
            rig.steps->onEncoderChange(1, enc(1));
            expectWithinAbsoluteError(s.paramLocks[UnifiedParam::RESONANCE], 0.5f, 0.002f, "Func snaps to the next quarter");
            rig.steps->setModifiers(false, false);

            // > + turn clears that knob's lock.
            rig.steps->onEncoderLockClear(1);
            expect(s.paramLocks.empty(), "lock cleared");
            rig.now += 1000;
            rig.steps->onPadRelease(4);

            // Without lock pages there is only the TRIG page: still no raw-id locks.
            StepRig bare(false);
            bare.steps->onPadPress(1, 100);
            bare.steps->setModifiers(true, false);
            for (int k = 0; k < 8; ++k) bare.steps->onEncoderChange(k, enc(2));
            expect(bare.step(1).paramLocks.empty(), "no lock written under raw ids");
        }

        beginTest("STEP: bank buttons page the encoders, Up / Down page the steps; TRIG reads ALL");
        {
            StepRig rig;
            rig.seq.getPattern().getTrack(0).setLength(64);

            expectEquals(rig.steps->getPageCount(), 1, "one knob page: no n/N");
            rig.steps->onArrowRight();
            expectEquals(rig.steps->getCurrentPage(), 0, "the bank buttons do not page the steps");
            rig.steps->onPadPageNext();
            expectEquals(rig.steps->getCurrentPage(), 1);
            expectEquals(rig.steps->getSubPage(), juce::String("S 17-32"));
            expectEquals(rig.steps->getPageCount(), 1);
            for (int i = 0; i < 6; ++i) rig.steps->onPadPageNext();
            expectEquals(rig.steps->getCurrentPage(), 3);
            for (int i = 0; i < 6; ++i) rig.steps->onPadPagePrev();
            expectEquals(rig.steps->getCurrentPage(), 0);

            // A held step: the bank buttons page its detail pages (TRIG + the lock pages).
            rig.steps->onPadPress(0, 100);
            rig.steps->onArrowLeft();
            expect(rig.steps->isDetailActive());
            expectEquals(rig.steps->getName(), juce::String("TRIG"));
            expectEquals(rig.steps->getPageIndex(), 0);
            expectEquals(rig.steps->getPageCount(), rig.params->getPageCount() + 1);
            expectEquals(rig.steps->getEncoderReadout(4), juce::String("COND ALL"));
            rig.steps->onArrowRight();
            expectEquals(rig.steps->getPageIndex(), 1);
            expectEquals(rig.steps->getCurrentPage(), 0, "the step page did not move");

            // Paging the steps while holding does not change which step is held.
            const int held = rig.steps->getHeldStepIndex();
            rig.steps->onPadPageNext();
            expectEquals(rig.steps->getHeldStepIndex(), held);
            rig.now += 1000;
            rig.steps->onPadRelease(0);

            std::array<Screen::PadColour, 16> c;
            expect(!rig.steps->getPadColours(c), "STEP keeps the pad manager's drawing");
        }

        beginTest("STEP: copy / clear / paste act on the held step, all of it");
        {
            StepRig rig;
            expect(!rig.steps->onCopy(), "no step held: the app applies its default");
            expect(!rig.steps->onClear());
            expect(!rig.steps->onPaste());

            auto& a = rig.step(1);
            a.trig = true;
            a.notes = { { 72, 0.5f }, { 76, 0.4f } };
            a.length = 2.0f;
            a.microTiming = 0.25f;
            a.trigCondition.type = TrigCondition::Fill;
            a.paramLocks[UnifiedParam::CUTOFF] = 777.0f;

            rig.steps->onPadPress(1, 100);
            expect(rig.steps->onCopy());
            rig.now += 50;
            rig.steps->onPadRelease(1);
            expect(a.trig, "copying counts as an edit: the quick release does not remove the step");

            auto& b = rig.step(9);
            rig.steps->onPadPress(9, 100);
            expect(rig.steps->onPaste());
            expect(b.trig);
            expectEquals((int) b.notes.size(), 2);
            expectEquals(b.notes[1].pitch, 76);
            expectWithinAbsoluteError(b.notes[0].velocity, 0.5f, 0.001f);
            expectWithinAbsoluteError(b.length, 2.0f, 0.001f);
            expect(b.trigCondition.type == TrigCondition::Fill);
            expectWithinAbsoluteError(b.paramLocks[UnifiedParam::CUTOFF], 777.0f, 0.01f);

            expect(rig.steps->onClear());
            expect(!b.trig && b.notes.empty() && b.paramLocks.empty(), "cleared");
            // (Taking a clear back is ScreenManager's undo history: the screen just clears again.)
            expect(rig.steps->onClear());
            expect(!b.trig && b.notes.empty() && b.paramLocks.empty(), "the same again is the same clear");
            rig.now += 1000;
            rig.steps->onPadRelease(9);
        }

        beginTest("STEP: the track settings follow plain / fine / coarse and have readouts");
        {
            StepRig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            expectEquals(rig.steps->getEncoderReadout(0), juce::String("LENGTH 16"));

            rig.steps->onEncoderChange(0, enc(1));
            expectEquals(track.length, 17);
            rig.steps->setModifiers(false, true);
            rig.steps->onEncoderChange(0, enc(1));
            expectEquals(track.length, 20, "Func: counts by 4");
            rig.steps->onEncoderChange(0, enc(-1));
            expectEquals(track.length, 16);

            rig.seq.setTempo(120.0f);
            rig.steps->onEncoderChange(7, enc(1));
            expectWithinAbsoluteError(rig.seq.getTempo(), 130.0f, 0.01f, "Func: BPM +10");
            rig.steps->setModifiers(true, false);
            rig.steps->onEncoderChange(7, enc(1));
            expectWithinAbsoluteError(rig.seq.getTempo(), 130.1f, 0.01f, "Shift: BPM +0.1");
            rig.steps->setModifiers(false, false);
            rig.steps->onEncoderChange(7, enc(-1));
            expectWithinAbsoluteError(rig.seq.getTempo(), 129.0f, 0.11f);
            expect(rig.steps->getEncoderReadout(7).startsWith("BPM 1"));
            expect(rig.steps->getEncoderReadout(1).startsWith("SPEED "));
            expect(rig.steps->getEncoderReadout(2).startsWith("QUANTIZE "));
            expect(!rig.steps->getSubPage().startsWith("T"), "the framework prints the track, not the screen");
        }
    }

    // ============================================ SONG ============================================
    void songTests()
    {
        beginTest("SONG: paging buttons never edit; INS and DEL are on knobs 7 and 8");
        {
            SongMode song;
            SequencerCore seq;
            SongScreen screen(song, seq);
            juce::int64 now = 5000;
            screen.setClock([&] { return now; });
            song.addEntry(0); song.addEntry(1); song.addEntry(2);
            screen.setSelectedRow(0);

            // Up / Down move the selection; the bank buttons do nothing. No row is added or removed.
            screen.onPadPageNext();
            expectEquals(screen.getSelectedRow(), 1);
            screen.onPadPageNext(); screen.onPadPageNext(); screen.onPadPageNext();
            expectEquals(screen.getSelectedRow(), 2);
            screen.onPadPagePrev();
            expectEquals(screen.getSelectedRow(), 1);
            for (int i = 0; i < 5; ++i) { screen.onArrowLeft(); screen.onArrowRight(); }
            expectEquals(song.getEntryCount(), 3, "paging never edits");
            expectEquals(screen.getSelectedRow(), 1);
            expectEquals(song.getEntry(1).patternIndex, 1);

            // INS: one click inserts a row after the selection and selects it.
            screen.onEncoderChange(6, enc(-1));
            expectEquals(song.getEntryCount(), 3, "anticlockwise does nothing");
            screen.onEncoderChange(6, enc(1));
            expectEquals(song.getEntryCount(), 4);
            expectEquals(screen.getSelectedRow(), 2);
            expectEquals(song.getEntry(1).patternIndex, 1);
            expectEquals(song.getEntry(3).patternIndex, 2, "the old row 3 moved down");

            // DEL: four clicks spread over 300 ms; three do nothing, another knob cancels.
            for (int i = 0; i < 3; ++i) { screen.onEncoderChange(7, enc(1)); now += 150; }
            expectEquals(song.getEntryCount(), 4);
            expect(screen.getEncoderReadout(7).contains(">>>"), "progress is shown");
            screen.onEncoderChange(0, enc(1));
            for (int i = 0; i < 3; ++i) { screen.onEncoderChange(7, enc(1)); now += 150; }
            expectEquals(song.getEntryCount(), 4, "cancelled by the other knob");
            screen.onEncoderChange(7, enc(-1));
            for (int i = 0; i < 4; ++i) screen.onEncoderChange(7, enc(1));
            expectEquals(song.getEntryCount(), 4, "four clicks at once are too fast");
            now += 400;
            screen.onEncoderChange(7, enc(1));
            expectEquals(song.getEntryCount(), 3, "deleted");
            expectEquals(song.getEntry(2).patternIndex, 2);
        }

        beginTest("SONG: fine / coarse, readouts, pad colours, OFF");
        {
            SongMode song;
            SequencerCore seq;
            SongScreen screen(song, seq);
            song.addEntry(0); song.addEntry(3);
            screen.setSelectedRow(1);

            screen.onEncoderChange(2, enc(1));
            expectEquals(song.getEntry(1).transposeSemitones, 1);
            screen.setModifiers(false, true);
            screen.onEncoderChange(2, enc(-1));
            expectEquals(song.getEntry(1).transposeSemitones, -11, "Func: transpose by octaves");
            screen.onEncoderChange(1, enc(1));
            expectEquals(song.getEntry(1).repeatCount, 4, "Func: counts by 4");
            screen.setModifiers(false, false);

            expectEquals(screen.getEncoderReadout(0), juce::String("PATTERN 4"));
            expectEquals(screen.getEncoderReadout(1), juce::String("REPEATS 4"));
            expectEquals(screen.getEncoderReadout(2), juce::String("TRANSPOSE -11"));
            expectEquals(screen.getEncoderReadout(3), juce::String("ROW 2/2"));
            expectEquals(screen.getEncoderReadout(5), juce::String("SONG OFF"));
            for (int i = 0; i < 8; ++i) expect(screen.getEncoderReadout(i).isNotEmpty());

            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            expect(!dark(c[0]) && !dark(c[1]) && dark(c[2]), "one pad per row");
            expect(brightness(c[1]) > brightness(c[0]), "the selected row is brighter");
            song.setArmed(true);
            song.start();
            expect(screen.getPadColours(c));
            expect(c[0].g > c[0].b, "the row being played is marked");

            // The on/off text is ON / OFF (it used to read OF).
            DisplayRenderer display;
            song.stop();
            song.setArmed(false);
            screen.render(display);
            expect(textAt(display, 82, 12, "OFF"), "OFF is drawn in full");
        }
    }

    // Renders `text` to a second buffer at the same place and compares the pixels.
    bool textAt(DisplayRenderer& display, int x, int y, const juce::String& text)
    {
        DisplayRenderer ref;
        ref.drawString(x, y, text);
        bool any = false;
        for (int py = y; py < y + 8; ++py)
            for (int px = x; px < x + text.length() * 6; ++px)
            {
                if (ref.getPixel(px, py) != display.getPixel(px, py)) return false;
                any = any || ref.getPixel(px, py);
            }
        return any;
    }

    // =========================================== PRESET ===========================================
    struct PresetRig
    {
        SequencerCore seq;
        AudioEngine audio;
        PresetManager mgr;
        std::unique_ptr<PresetScreen> screen;
        juce::int64 now = 1000;
        std::vector<std::pair<int, int>> on, pressure;
        std::vector<int> off;

        PresetRig()
        {
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            setCutoff(1234.0f);
            screen = std::make_unique<PresetScreen>(seq, &audio, mgr);
            screen->setClock([this] { return now; });
            screen->setNoteCallback([this](int n, int v) { on.push_back({ n, v }); });
            screen->setNoteOffCallback([this](int n) { off.push_back(n); });
            screen->setAftertouchCallback([this](int n, int p) { pressure.push_back({ n, p }); });
            screen->onActivate();
        }
        void setCutoff(float hz)
        {
            audio.setTrackParameter(0, UnifiedParam::CUTOFF, hz);
            seq.getPattern().getTrack(0).soundParams[UnifiedParam::CUTOFF] = hz;
        }
        float cutoff() const { return audio.getTrackParameter(0, UnifiedParam::CUTOFF); }
        // Scroll down until the auditioned preset's sound differs from the 1234 Hz marker.
        bool scrollToDifferentSound()
        {
            for (int i = 0; i < screen->getListSize(); ++i)
            {
                screen->onEncoderChange(0, 1);
                if (std::abs(cutoff() - 1234.0f) > 1.0f) return true;
            }
            return false;
        }
        void confirm(int knob) { for (int i = 0; i < 4; ++i) { screen->onEncoderChange(knob, 1); now += 150; } }
    };

    void presetTests()
    {
        beginTest("PRESET: pads play the track through the note callbacks and never load or save");
        {
            PresetRig rig;
            const int presets = rig.mgr.getPresetCount();
            const auto params = rig.seq.getPattern().getTrack(0).soundParams;
            const int base = rig.screen->getBaseNote();
            expectEquals(base, 60);

            for (int pad = 0; pad < 16; ++pad)
            {
                rig.screen->onPadPress(pad, 90 + pad);
                rig.screen->onPadAftertouch(pad, 50 + pad);
                rig.screen->onPadRelease(pad);
            }
            expectEquals((int) rig.on.size(), 16);
            expectEquals((int) rig.off.size(), 16);
            expectEquals((int) rig.pressure.size(), 16);
            for (int pad = 0; pad < 16; ++pad)
            {
                expectEquals(rig.on[(size_t) pad].first, base + pad, "chromatic from the base note");
                expectEquals(rig.on[(size_t) pad].second, 90 + pad, "with the pad's velocity");
                expectEquals(rig.pressure[(size_t) pad].first, base + pad);
                expectEquals(rig.pressure[(size_t) pad].second, 50 + pad, "and its own pressure");
                expectEquals(rig.off[(size_t) pad], base + pad);
            }
            expectEquals(rig.mgr.getPresetCount(), presets, "no pad saves");
            expect(rig.seq.getPattern().getTrack(0).soundParams == params, "no pad loads");
            expectWithinAbsoluteError(rig.cutoff(), 1234.0f, 0.5f);

            // The base note follows the provider (the track's octave).
            rig.screen->setBaseNoteProvider([] { return 36; });
            rig.screen->onPadPress(2, 100);
            expectEquals(rig.on.back().first, 38);
            rig.screen->onPadRelease(2);

            std::array<Screen::PadColour, 16> c;
            expect(rig.screen->getPadColours(c));
            for (auto& p : c) expect(!dark(p), "every pad plays");
            expect(!same(c[0], c[1]) && same(c[0], c[12]), "the Cs are marked");
        }

        beginTest("PRESET: scrolling auditions - the sound returns on leaving or AU off, unless LD");
        {
            PresetRig rig;
            expect(rig.screen->getListSize() > 3);

            // Audition is on without AU being touched; entering the screen loaded nothing.
            expectEquals(rig.screen->getEncoderReadout(3), juce::String("AUDITION ON"));
            expectWithinAbsoluteError(rig.cutoff(), 1234.0f, 0.5f);
            expect(!rig.screen->isAuditioning());

            // Scrolling loads the highlighted preset...
            expect(rig.scrollToDifferentSound(), "auditioning changes the sound");
            expect(rig.screen->isAuditioning());
            // (Up / Down scroll the list too.)
            const int index = rig.screen->getSelectedIndex();
            rig.screen->onPadPagePrev();
            expectEquals(rig.screen->getSelectedIndex(), index - 1);
            rig.screen->onPadPageNext();
            expectEquals(rig.screen->getSelectedIndex(), index);
            // ...and leaving the screen brings the old sound back.
            rig.screen->onDeactivate();
            expectWithinAbsoluteError(rig.cutoff(), 1234.0f, 0.5f, "restored on leaving");
            expectWithinAbsoluteError(rig.seq.getPattern().getTrack(0).soundParams[UnifiedParam::CUTOFF], 1234.0f, 0.5f);

            // The same when AU is switched off; with AU off, scrolling loads nothing.
            rig.screen->onActivate();
            rig.screen->onPadPagePrev();
            expect(rig.scrollToDifferentSound());
            rig.screen->onEncoderChange(3, 127);
            expectEquals(rig.screen->getEncoderReadout(3), juce::String("AUDITION OFF"));
            expectWithinAbsoluteError(rig.cutoff(), 1234.0f, 0.5f, "restored when AU goes off");
            rig.screen->onPadPagePrev();
            rig.screen->onPadPageNext();
            expectWithinAbsoluteError(rig.cutoff(), 1234.0f, 0.5f, "AU off: scrolling loads nothing");

            // AU back on: the highlighted preset is heard at once. LD (four clicks) makes it the track's sound.
            rig.screen->onEncoderChange(3, 1);
            expect(std::abs(rig.cutoff() - 1234.0f) > 1.0f);
            for (int i = 0; i < 3; ++i) { rig.screen->onEncoderChange(4, 1); rig.now += 150; }
            expect(rig.screen->isAuditioning(), "three clicks are not a load");
            rig.screen->onEncoderChange(4, 1);
            expect(!rig.screen->isAuditioning());
            expectEquals(rig.screen->getSubPage(), juce::String("LOAD OK"));
            const float loaded = rig.cutoff();
            expect(std::abs(loaded - 1234.0f) > 1.0f);
            rig.screen->onDeactivate();
            expectWithinAbsoluteError(rig.cutoff(), loaded, 0.5f, "LD keeps");

            // After LD, a further audition returns to the loaded sound, not the older one.
            rig.screen->onActivate();
            rig.setCutoff(1234.0f);
            rig.screen->onDeactivate();
            rig.screen->onActivate();
            rig.screen->onPadPagePrev();
            expect(rig.scrollToDifferentSound());
            rig.screen->onPadPageNext();
            rig.screen->onDeactivate();
            expectWithinAbsoluteError(rig.cutoff(), 1234.0f, 0.5f);
        }

        beginTest("PRESET: SV and DL need the four-click confirmation; VL is fine / coarse; readouts");
        {
            PresetRig rig;
            const int before = rig.mgr.getPresetCount();
            for (int i = 0; i < 4; ++i) rig.screen->onEncoderChange(5, 1);
            expectEquals(rig.mgr.getPresetCount(), before, "four clicks at once are too fast");
            rig.screen->onEncoderChange(7, 1);
            rig.confirm(5);
            expectEquals(rig.mgr.getPresetCount(), before + 1);

            rig.audio.setTrackParameter(0, MixParam::VOLUME, 0.6f);
            rig.screen->onEncoderChange(7, enc(1));
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, MixParam::VOLUME), 0.61f, 0.0005f);
            rig.screen->setModifiers(true, false);
            rig.screen->onEncoderChange(7, enc(1));
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, MixParam::VOLUME), 0.611f, 0.0005f, "Shift: fine");
            rig.screen->setModifiers(false, true);
            rig.screen->onEncoderChange(7, enc(1));
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, MixParam::VOLUME), 0.75f, 0.0005f, "Func: next quarter");
            rig.screen->setModifiers(false, false);

            for (int i = 0; i < 8; ++i) expect(rig.screen->getEncoderReadout(i).isNotEmpty());
            expect(rig.screen->getEncoderReadout(0).startsWith("PRESET "));
            expectEquals(rig.screen->getEncoderReadout(7), juce::String("VOLUME 75"));
            expect(!rig.screen->getSubPage().startsWith("T1"));
        }
    }

    // ============================================ SCALE ===========================================
    void scaleTests()
    {
        beginTest("SCALE: fine / coarse, readouts, Up / Down = page, keys keep playing with key select on");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            mgr.setActivePage(0);
            const int root = mgr.getRootNote();

            screen.onEncoderChange(1, enc(1));
            expectEquals(mgr.getRootNote(), root + 1);
            screen.setModifiers(false, true);
            screen.onEncoderChange(1, enc(1));
            expectEquals(mgr.getRootNote(), root + 13, "Func: root by octaves");
            screen.setModifiers(false, false);
            expectEquals(screen.getEncoderReadout(1), "ROOT " + NoteUtils::noteToString(root + 13));

            screen.onEncoderChange(2, enc(1));
            expectEquals(screen.getEncoderReadout(2), juce::String("MODE ABS"));
            screen.onEncoderChange(2, enc(-1));
            expectEquals(screen.getEncoderReadout(2), juce::String("MODE REL"));
            for (int i = 0; i < 8; ++i) expect(screen.getEncoderReadout(i).isNotEmpty());
            expect(screen.getEncoderReadout(0).startsWith("PAGE 1 "));

            // Up / Down change the page, in edit mode too; the bank buttons do not, and never
            // touch the octave.
            screen.onPadPageNext();
            expectEquals(mgr.getActivePage(), 1);
            mgr.setEditMode(true);
            const int note0 = mgr.getNoteForPad(0);
            screen.onPadPageNext();
            expectEquals(mgr.getActivePage(), 2);
            screen.onPadPagePrev();
            expectEquals(mgr.getActivePage(), 1);
            screen.onArrowRight();
            screen.onArrowLeft();
            expectEquals(mgr.getActivePage(), 1);
            expectEquals(mgr.getNoteForPad(0), note0, "no octave shift on a paging button");
            mgr.setEditMode(false);

            // Keys select pages and still play.
            std::vector<int> on, off;
            screen.setNoteCallback([&](int n, int) { on.push_back(n); });
            screen.setNoteOffCallback([&](int n) { off.push_back(n); });
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Latched);
            expect(screen.wantsKeyboardEvents());
            screen.onKeyboardNoteOn(64, 100);
            screen.onKeyboardNoteOff(64);
            expectEquals((int) on.size(), 1);
            expectEquals(on[0], 64);
            expectEquals((int) off.size(), 1);

            // Pad colours: root one colour, other notes another, the held pad brighter.
            mgr.setKeySelectMode(CustomScaleManager::KeySelectMode::Off);
            mgr.setActivePage(0);
            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            int rootPad = -1, otherPad = -1;
            for (int i = 0; i < 16; ++i)
            {
                const bool isRoot = mgr.getNoteForPad(i) % 12 == mgr.getRootNote() % 12;
                if (isRoot && rootPad < 0) rootPad = i;
                if (!isRoot && otherPad < 0) otherPad = i;
            }
            expect(rootPad >= 0 && otherPad >= 0);
            expect(!same(c[(size_t) rootPad], c[(size_t) otherPad]));
            const auto restColour = c[(size_t) otherPad];
            screen.onPadPress(otherPad, 100);
            expect(screen.getPadColours(c));
            expect(brightness(c[(size_t) otherPad]) > brightness(restColour), "the selected pad is bright");
            screen.onPadRelease(otherPad);
        }
    }

    // ============================================ DRUMS ===========================================
    void drumTests()
    {
        beginTest("DRUMS: selected pad hook, fine / coarse, readouts, pad colours");
        {
            DrumEngine engine;
            engine.prepareToPlay(44100.0, 256);
            DrumPadScreen screen(&engine);
            std::vector<int> selected;
            screen.onPadSelected = [&](int pad) { selected.push_back(pad); };
            expectEquals(screen.getSelectedPad(), 8);

            screen.onPadPress(3, 100);
            expectEquals(screen.getSelectedPad(), 3);
            expectEquals(engine.getSelectedVoice(), 3, "the engine knows the pad, for the PARAM screen's DR page");
            expectEquals((int) selected.size(), 1);
            expectEquals(selected[0], 3);

            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            expect(c[3].r == 127 && c[3].g == 127 && c[3].b == 127, "a pad being hit is brightest");
            screen.onPadRelease(3);
            screen.onPadPress(3, 100);
            screen.onPadRelease(3);
            expectEquals((int) selected.size(), 1, "only a change of pad is reported");

            // Let the voices die away, then: selected = full colour, the others dim, one colour per type.
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            for (int b = 0; b < 2000; ++b)
            {
                buf.clear();
                engine.processBlock(buf, midi);
            }
            expect(screen.getPadColours(c));
            const auto full = DrumPadScreen::typeColour(engine.getVoice(3).getType());
            expect(same(c[3], full), "the selected pad is bright");
            for (int i = 0; i < 16; ++i)
            {
                if (i == 3) continue;
                const auto type = DrumPadScreen::typeColour(engine.getVoice(i).getType());
                expect(brightness(c[(size_t) i]) < brightness(type), "unselected pads are dim");
                expect(!dark(c[(size_t) i]));
            }
            expect(!same(DrumPadScreen::typeColour(DrumSynthType::FMKick), DrumPadScreen::typeColour(DrumSynthType::ClosedHat)));
            expect(!same(DrumPadScreen::typeColour(DrumSynthType::NoiseSnare), DrumPadScreen::typeColour(DrumSynthType::Clap)));

            // Knobs: plain 1 %, Shift a tenth, Func snaps.
            const auto spec = DrumEngine::voicePage(3).params[5];      // LEVL
            engine.setParameter(spec.id, spec.fromNorm(0.4f));
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(spec.toNorm(engine.getParameter(spec.id)), 0.41f, 0.002f);
            screen.setModifiers(true, false);
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(spec.toNorm(engine.getParameter(spec.id)), 0.411f, 0.0005f, "fine");
            screen.setModifiers(false, true);
            screen.onEncoderChange(5, enc(1));
            const float snapped = spec.toNorm(engine.getParameter(spec.id));
            expect(snapped > 0.45f, "coarse jumps to a snap point");
            screen.setModifiers(false, false);

            expectEquals(screen.getEncoderReadout(5), "LEVEL " + spec.format(engine.getParameter(spec.id)));
            for (int i = 0; i < 8; ++i) expect(screen.getEncoderReadout(i).isNotEmpty());
            expect(screen.getSubPage().startsWith("P4 "));
        }

        beginTest("DRUMS: the bank buttons go round the selected pad's own pages, the ones PARAM shows for it; every knob has "
                  "two letters and a readout whose value is in the name bar");
        {
            // Through ScreenManager, as the controller sends it. The pad's pages are the engine's
            // (DrumEngine::padPages): DR, what the type adds, PLFO, PSLT - eight cells each.
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Drum));
            seq.getPattern().getTrack(0).engineType = EngineType::Drum;
            auto* engine = dynamic_cast<DrumEngine*>(audio.getEngine(0));
            expect(engine != nullptr);
            if (engine == nullptr) return;
            ScreenManager mgr;
            DisplayRenderer display;
            mgr.setDisplay(&display);
            auto* screen = new DrumPadScreen(engine);
            mgr.addScreen(std::unique_ptr<Screen>(screen));
            ParamPageScreen param(seq, &audio);
            const auto bank = [&](bool down)
            {
                mgr.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, down ? ScreenManager::CC_BANK_DOWN : ScreenManager::CC_BANK_UP, 127);
                mgr.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, down ? ScreenManager::CC_BANK_DOWN : ScreenManager::CC_BANK_UP, 0);
            };
            const auto turn = [&](int knob, int clicks) { mgr.controlChangeReceived(16, 0x15 + knob, enc(clicks)); };

            struct Want { int pad; const char* pages; };
            // The starting kit: the lower left pad a kick machine, top pad 7 a resonator drum (the
            // crash), bottom pad 7 an oscillator bank (the hat), top pad 1 a sub-bass.
            for (const auto& want : { Want { 8, "DR 9 KICK KCK2 PLFO PSLT" }, Want { 6, "DR 7 RSN PLFO PSLT" },
                                      Want { 14, "DR 15 BANK PLFO PSLT" }, Want { 0, "DR 1 PLFO PSLT" } })
            {
                screen->selectPad(want.pad);
                const auto pages = engine->padPages(want.pad);
                juce::StringArray names;
                for (auto& page : pages) names.add(page.name);
                expectEquals(names.joinIntoString(" "), juce::String(want.pages));
                expectEquals(screen->getPageCount(), (int) pages.size());
                // PARAM begins with the same pages for the same pad (then the kit's own).
                param.setCurrentPageIndex(0);
                for (int p = 0; p < (int) pages.size(); ++p)
                {
                    param.setCurrentPageIndex(p);
                    expectEquals(param.getPageName(), names[p], "PARAM, pad " + juce::String(want.pad + 1));
                }
                param.setCurrentPageIndex((int) pages.size());
                expectEquals(param.getPageName(), juce::String("WAV1"), "then the kit's pages");

                for (int i = 0; i < (int) pages.size() && screen->getPageIndex() != 0; ++i)
                    bank(false);
                expectEquals(screen->getPageIndex(), 0);
                bank(false);
                expectEquals(screen->getPageIndex(), (int) pages.size() - 1, "the upper button from the first page: the last");
                bank(true);
                for (int p = 0; p < (int) pages.size(); ++p)
                {
                    UiClock::advance(3000);
                    expectEquals(screen->getPageIndex(), p);
                    const auto bar = mgr.composeNameBar();
                    expectEquals(bar.pages, juce::String(p + 1) + "/" + juce::String((int) pages.size()));
                    // The name bar: the pad, and the type on the DR page, the page's name on the others. Whole.
                    expectEquals(bar.text, screen->getSubPage(), "nothing of it is cut");
                    expect(bar.text.startsWith(want.pad < 9 ? "P" + juce::String(want.pad + 1) + " " : juce::String(want.pad + 1) + " "), bar.text);
                    if (p > 0)
                        expect(bar.text.endsWith(" " + names[p]), bar.text);

                    juce::StringArray twos;
                    for (int k = 0; k < 8; ++k)
                    {
                        const auto spec = pages[(size_t) p].params[(size_t) k];
                        const auto two = screen->getEncoderName(k);
                        const juce::String where = names[p] + " " + spec.label;
                        expectEquals(two.length(), 2, where);
                        expect(!twos.contains(two), where + ": " + two + " twice on one page");
                        twos.add(two);
                        if (juce::String(spec.label) == "TYPE")
                            continue;                   // (another type is other pages)
                        const float was = engine->getParameter(spec.id);
                        turn(k, 1);
                        const auto text = mgr.composeNameBar().text;
                        if (const char* why = engine->padModIdleReason(spec.id))
                        {
                            // A depth or an amount with no destination: it says so and stays.
                            expectEquals(text, juce::String(why), where);
                            expectEquals(engine->getParameter(spec.id), was, where);
                        }
                        else
                        {
                            expect(engine->getParameter(spec.id) != was, where + " turns");
                            expect(text.endsWith(" " + spec.format(engine->getParameter(spec.id))), where + ": the value is in the name bar: " + text);
                            expect(text != screen->getSubPage());
                        }
                        UiClock::advance(3000);
                        turn(k, -1);
                        UiClock::advance(3000);
                    }
                    bank(true);
                }
                expectEquals(screen->getPageIndex(), 0, "once round");
            }

            // Another pad keeps the page where it has one of that kind.
            screen->selectPad(8);
            bank(true);
            expectEquals(screen->getSubPage(), juce::String("P9 KICK"));
            screen->selectPad(6);
            expectEquals(screen->getSubPage(), juce::String("P7 RSN"), "a kick's first page: the resonator drum's own");
            expectEquals(screen->getPageIndex(), 1);
            screen->selectPad(0);
            expectEquals(screen->getPageIndex(), 0, "a pad with no page of its type: its DR page");
            screen->selectPad(8);
            expectEquals(screen->getSubPage(), juce::String("P9 KICK"), "and back");
            bank(true); bank(true);
            expectEquals(screen->getSubPage(), juce::String("P9 PLFO"));
            screen->selectPad(15);
            expectEquals(screen->getSubPage(), juce::String("16 PLFO"), "a pad of two digits goes without its P: the page stays readable");
            expectEquals(mgr.composeNameBar().text, juce::String("16 PLFO"));

            // PLFO on DRUMS as on PARAM: DPTH with no destination says so; with one it moves.
            const int depth = PadModParam::id(15, PadModParam::LFO_DEPTH), dest = PadModParam::id(15, PadModParam::LFO_DEST);
            UiClock::advance(3000);
            turn(1, 5);
            expectEquals(engine->getParameter(depth), 0.0f);
            expectEquals(screen->getSubPage(), juce::String("SET DEST"));
            UiClock::advance(3000);
            turn(3, 1);
            expectEquals(engine->getParameter(dest), 1.0f);
            turn(1, 5);
            expectWithinAbsoluteError(engine->getParameter(depth), 0.1f, 1.0e-4f, "five clicks of a hundredth of -100 .. +100");
            UiClock::advance(3000);
        }

        beginTest("A drum pad's WAVE: CYCL, which had no sample to read there, is not offered; the list keeps its places");
        {
            DrumEngine engine;
            const ParamSpec w = DrumEngine::waveSpec(8);
            expectEquals(w.numChoices, phaseosc::waveCount(), "no index moves");
            expectEquals(juce::String(w.choices[phaseosc::WAVE_SINE]), juce::String("SINE"));
            expectEquals(juce::String(w.choices[phaseosc::WAVE_CYCLES]), juce::String(), "the place CYCL had: kept, unnamed");
            expectEquals(juce::String(w.choices[phaseosc::FIRST_TABLE_WAVE]), juce::String(phaseosc::waveLabels()[phaseosc::FIRST_TABLE_WAVE]));
            // A knob passes over it, both ways, at every speed.
            for (auto mode : { TurnStep::Plain, TurnStep::Fine, TurnStep::Coarse })
            {
                expectEquals(w.turned(0.0f, 1, mode), mode == TurnStep::Coarse ? 4.0f : (float) phaseosc::FIRST_TABLE_WAVE);
                expectEquals(w.turned((float) phaseosc::FIRST_TABLE_WAVE, -1, mode), 0.0f);
                for (int from = 0; from < w.numChoices; ++from)
                    for (int clicks : { -3, -1, 1, 3 })
                        expect((int) w.turned((float) from, clicks, mode) != phaseosc::WAVE_CYCLES);
            }
            // A sound stored with it plays the sine it played, and shows it.
            engine.setParameter(DrumEngine::WAVE_BASE + 8, (float) phaseosc::WAVE_CYCLES);
            expectEquals(engine.getParameter(DrumEngine::WAVE_BASE + 8), (float) phaseosc::WAVE_SINE);
            expectEquals(w.format(engine.getParameter(DrumEngine::WAVE_BASE + 8)), juce::String("SINE"));
            engine.setParameter(DrumEngine::WAVE_BASE + 8, 3.0f);
            expectEquals(engine.getParameter(DrumEngine::WAVE_BASE + 8), 3.0f, "every other place is what it was");
            // The other engines' WAVE cells keep CYCL: they have a sample for it.
            expectEquals(juce::String(phaseosc::waveLabels()[phaseosc::WAVE_CYCLES]), juce::String("CYCL"));
            // A list without unnamed places turns as it always did.
            static const char* const three[] = { "A", "B", "C" };
            const ParamSpec plain { "X", 0, 0, 2, 0, ParamKind::Choice, "", three, 3 };
            expectEquals(plain.turned(0.0f, 1, TurnStep::Plain), 1.0f);
            expectEquals(plain.turned(2.0f, 1, TurnStep::Plain), 2.0f);
            // An unnamed place at an end: the knob stays where it is.
            static const char* const ends[] = { "A", "B", "" };
            const ParamSpec last { "X", 0, 0, 2, 0, ParamKind::Choice, "", ends, 3 };
            expectEquals(last.turned(1.0f, 1, TurnStep::Plain), 1.0f);
            expectEquals(last.turned(0.0f, 5, TurnStep::Plain), 1.0f);
        }
    }

    // ======================================= ARP and SETTINGS =====================================
    void arpAndSettingsTests()
    {
        beginTest("ARP: seven page keys, ACFG, clearer labels, >PAT confirmed, pads dark except TAP");
        {
            Arpeggiator arp;
            AudioEngine audio;
            ArpScreen screen(arp);
            screen.setPadRatchet(&audio.getPadRatchet());
            juce::int64 now = 100;
            screen.setClock([&] { return now; });

            expectEquals(screen.getPageKeyCount(), 7);
            static const char* const names[] = { "ARP", "PLAY", "VARY", "MUTA", "ACFG", "ROLL", "ROL2" };
            for (int key = 6; key >= 0; --key)
            {
                screen.onPageKey(key);
                expectEquals(screen.getName(), juce::String(names[key]));
                expectEquals(screen.getActivePageKey(), key);
                expectEquals(screen.getPageIndex(), key);
            }
            screen.onPageKey(7);
            expectEquals(screen.getPageIndex(), 0, "a key without a page does nothing");

            // Vocabulary.
            expectEquals(screen.getCellLabel(4, 0), juce::String("DEST"));
            expectEquals(screen.getCellLabel(4, 1), juce::String("DPTH"));
            expectEquals(screen.getCellLabel(1, 1), juce::String("TIME"));
            expectEquals(screen.getCellLabel(1, 2), juce::String("DIR"));
            expectEquals(screen.getCellLabel(1, 3), juce::String("VEL"));
            expectEquals(screen.getCellLabel(2, 3), juce::String("SCAT"));
            expectEquals(screen.getCellLabel(2, 5), juce::String("GVAR"));
            expectEquals(screen.getCellLabel(2, 6), juce::String("STRP"));
            for (int p = 0; p < 7; ++p)
                for (int cell = 0; cell < 8; ++cell)
                {
                    expect(screen.getCellLabel(p, cell) != "PTGT" && screen.getCellLabel(p, cell) != "PAMT");
                    expect(juce::String(names[p]) != "SETUP");
                }

            // The cells still reach the arpeggiator.
            screen.onPageKey(4);
            screen.onEncoderChange(1, enc(10));
            expectWithinAbsoluteError(arp.getPressureAmount(), 0.1f, 0.001f);

            // >PAT: four clicks spread over 300 ms.
            int exports = 0;
            screen.exportToPattern = [&] { ++exports; return true; };
            screen.onPageKey(3);
            for (int i = 0; i < 4; ++i) screen.onEncoderChange(6, enc(1));
            expectEquals(exports, 0, "too fast");
            screen.onEncoderChange(5, enc(1));          // another knob cancels
            for (int i = 0; i < 3; ++i) { screen.onEncoderChange(6, enc(1)); now += 150; }
            expectEquals(exports, 0);
            expectEquals(screen.getCellText(3, 6), juce::String(">>>"));
            screen.onEncoderChange(6, enc(1));
            expectEquals(exports, 1);
            expectEquals(screen.getCellText(3, 6), juce::String("OK"));

            // Pads: dark, lit while TAP is armed.
            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            for (auto& p : c) expect(dark(p));
            screen.setRhythmCapturing(true);
            expect(screen.getPadColours(c));
            for (auto& p : c) expect(!dark(p));
            screen.setRhythmCapturing(false);
            expect(screen.getPadColours(c));
            expect(dark(c[0]));
        }

        beginTest("SETTINGS: four page keys, MVOL, project actions confirmed, pads dark");
        {
            int loads = 0, saves = 0, news = 0;
            float tempo = 120.0f, level = 0.8f;
            SettingsScreen::Hooks h;
            h.getTempo = [&] { return tempo; };
            h.setTempo = [&](float v) { tempo = v; };
            h.getMasterLevel = [&] { return level; };
            h.setMasterLevel = [&](float v) { level = v; };
            h.loadSlot = [&](int) { ++loads; return true; };
            h.saveToSlot = [&](int) { ++saves; return true; };
            h.newProject = [&] { ++news; };
            SettingsScreen screen(h);
            juce::int64 now = 100;
            screen.setClock([&] { return now; });

            expectEquals(screen.getPageKeyCount(), 4);
            static const char* const names[] = { "SETUP", "MASTR", "PROJ", "CLICK" };
            for (int key = 3; key >= 0; --key)
            {
                screen.onPageKey(key);
                expectEquals(screen.getName(), juce::String(names[key]));
                expectEquals(screen.getActivePageKey(), key);
            }
            expectEquals(screen.getEncoderName(4), juce::String("MVOL"));
            screen.onEncoderChange(4, enc(-10));
            expectWithinAbsoluteError(level, 0.7f, 0.001f);

            // BPM: plain 1, Shift 0.1, Func 10.
            screen.onEncoderChange(0, enc(1));
            expectWithinAbsoluteError(tempo, 121.0f, 0.001f);
            screen.setModifiers(true, false);
            screen.onEncoderChange(0, enc(1));
            expectWithinAbsoluteError(tempo, 121.1f, 0.001f);
            screen.setModifiers(false, true);
            screen.onEncoderChange(0, enc(1));
            expectWithinAbsoluteError(tempo, 131.1f, 0.001f);
            screen.setModifiers(false, false);

            screen.onPageKey(2);
            for (int i = 0; i < 4; ++i) screen.onEncoderChange(1, enc(1));
            expectEquals(loads, 0, "four clicks at once are too fast");
            screen.onEncoderChange(0, enc(1));          // SLOT cancels
            for (int i = 0; i < 3; ++i) { screen.onEncoderChange(1, enc(1)); now += 150; }
            expectEquals(loads, 0);
            screen.onEncoderChange(1, enc(-1));         // anticlockwise cancels
            for (int i = 0; i < 4; ++i) { screen.onEncoderChange(1, enc(1)); now += 150; }
            expectEquals(loads, 1);
            for (int i = 0; i < 4; ++i) { screen.onEncoderChange(4, enc(1)); now += 150; }
            expectEquals(news, 1);
            for (int i = 0; i < 4; ++i) { screen.onEncoderChange(3, enc(1)); now += 150; }
            expectEquals(saves, 1);

            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            for (auto& p : c) expect(dark(p));
        }
    }

    // ============================================ WAVE ============================================
    void waveTests()
    {
        beginTest("WAVE: RC arms on two clicks within a second, never on a stray one; CR is confirmed");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addEngine(1, AudioEngine::createEngine(EngineType::Sampler));
            seq.getPattern().getTrack(1).engineType = EngineType::Sampler;
            audio.prepare(44100.0, 256);
            auto& rec = audio.getResampler();

            WaveformScreen screen(seq, &audio);
            screen.setWriteFiles(false);
            juce::int64 now = 1000;
            screen.setClock([&] { return now; });
            screen.onArrowRight();
            expectEquals(screen.getName(), juce::String("REC"));

            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            for (auto& p : c) expect(dark(p), "REC: pads off");

            screen.onEncoderChange(4, enc(1));
            expect(rec.getState() == Resampler::State::Idle, "one click does not arm");
            now += 1500;
            screen.onEncoderChange(4, enc(1));
            expect(rec.getState() == Resampler::State::Idle, "two clicks more than a second apart do not arm");
            now += 300;
            screen.onEncoderChange(0, enc(1));          // another knob in between: start again
            screen.onEncoderChange(4, enc(1));
            expect(rec.getState() == Resampler::State::Idle);
            now += 300;
            screen.onEncoderChange(4, enc(1));
            expect(rec.getState() != Resampler::State::Idle, "two clicks within a second arm");

            // Recording: the top pad row is red.
            std::vector<float> l(256), r(256);
            audio.getEngine(0)->noteOn(57, 0.9f);
            for (int b = 0; b < 40; ++b) audio.renderBlock(l.data(), r.data(), 256);
            expect(rec.getState() == Resampler::State::Recording);
            expect(screen.getPadColours(c));
            for (int i = 0; i < 8; ++i) expect(c[(size_t) i].r > 100 && c[(size_t) i].g == 0 && c[(size_t) i].b == 0);
            for (int i = 8; i < 16; ++i) expect(dark(c[(size_t) i]));

            // One click anticlockwise stops.
            seq.setSelectedTrack(1);
            const int before = SampleBank::get().size();
            screen.onEncoderChange(4, enc(-1));
            screen.poll();
            expect(rec.getState() == Resampler::State::Idle);
            expectEquals(SampleBank::get().size(), before + 1);

            // Readouts on REC.
            rec.setSource(1);
            expectEquals(screen.getEncoderReadout(0), juce::String("SOURCE TRACK 2"));
            screen.setModifiers(false, false);
            for (int i = 0; i < 8; ++i) expect(screen.getEncoderReadout(i).isNotEmpty());

            // CR: four clicks spread over 300 ms.
            auto* samp = dynamic_cast<SamplerEngine*>(audio.getEngine(1));
            const int length = samp->getSampleLength();
            expect(length > 0);
            audio.setTrackParameter(1, SamplerEngine::StartPoint, 0.25f);
            audio.setTrackParameter(1, SamplerEngine::EndPoint, 0.75f);
            for (int i = 0; i < 4; ++i) screen.onEncoderChange(7, enc(1));
            expectEquals(samp->getSampleLength(), length, "too fast: no crop");
            screen.onEncoderChange(7, enc(-1));
            for (int i = 0; i < 4; ++i) { screen.onEncoderChange(7, enc(1)); now += 150; }
            expectEquals(samp->getSampleLength(), length / 2, "cropped");

            // WAVE page: fine / coarse and readouts.
            screen.onArrowLeft();
            expectEquals(screen.getEncoderReadout(4), juce::String("ZOOM 100%"));
            screen.setModifiers(false, true);
            screen.onEncoderChange(4, enc(2));
            expectEquals(screen.getEncoderReadout(4), juce::String("ZOOM 25%"));
            screen.onEncoderChange(4, enc(-2));
            expectEquals(screen.getEncoderReadout(4), juce::String("ZOOM 100%"));
            screen.onEncoderChange(0, enc(1));
            expectWithinAbsoluteError(samp->getParameter(SamplerEngine::StartPoint), 0.25f, 0.001f, "Func: quarters of the sample");
            screen.setModifiers(false, false);
            screen.onEncoderChange(0, enc(1));
            const float plain = samp->getParameter(SamplerEngine::StartPoint) - 0.25f;
            expectWithinAbsoluteError(plain, 0.004f, 0.0002f);
            screen.setModifiers(true, false);
            screen.onEncoderChange(0, enc(1));
            expectWithinAbsoluteError(samp->getParameter(SamplerEngine::StartPoint) - 0.25f - plain, 0.0004f, 0.0001f, "Shift: a tenth");
            screen.setModifiers(false, false);
            expect(screen.getEncoderReadout(0).startsWith("START 25."));
            expect(screen.getEncoderReadout(6).startsWith("MODE "));
            for (int i = 0; i < 7; ++i) expect(screen.getEncoderReadout(i).isNotEmpty());
            expect(!screen.getSubPage().startsWith("T2"));

            // Slices: one pad each, the selected one bright; a pad picks its slice.
            audio.setTrackParameter(1, SamplerEngine::SliceCount, 8.0f);
            expectEquals(samp->getSliceCount(), 8);
            screen.onPadPress(5, 100);
            expect(screen.getPadColours(c));
            for (int i = 0; i < 8; ++i) expect(!dark(c[(size_t) i]));
            for (int i = 8; i < 16; ++i) expect(dark(c[(size_t) i]));
            expect(brightness(c[5]) > brightness(c[4]));
            expectEquals(screen.getEncoderReadout(7), juce::String("SLICE 6/8"));
        }
    }

    // ===================================== Pressure routing per track =============================
    // Energy of a note on `track` held with `pressure`, in an instrument whose tracks 0 and 1 are
    // the same synth with different pressure routes.
    float pressedEnergy(AudioEngine& audio, int track, float pressure)
    {
        std::vector<float> l(256), r(256);
        auto* engine = audio.getEngine(track);
        engine->noteOn(57, 0.5f);
        for (int b = 0; b < 30; ++b) audio.renderBlock(l.data(), r.data(), 256);
        engine->aftertouch(57, pressure);
        for (int b = 0; b < 10; ++b) audio.renderBlock(l.data(), r.data(), 256);
        double energy = 0.0;
        for (int b = 0; b < 60; ++b)
        {
            audio.renderBlock(l.data(), r.data(), 256);
            for (float v : l) energy += (double) v * v;
        }
        engine->aftertouch(57, 0.0f);
        engine->noteOff(57);
        for (int b = 0; b < 400; ++b) audio.renderBlock(l.data(), r.data(), 256);
        return (float) energy;
    }

    void pressureRoutingTests()
    {
        beginTest("Pressure: two tracks routed differently respond differently in the rendered audio");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            for (int t = 0; t < 2; ++t)
            {
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
                audio.setTrackParameter(t, UnifiedParam::MIX, 0.0f);
                audio.setTrackParameter(t, UnifiedParam::DRIFT, 0.0f);
                audio.setTrackParameter(t, MixParam::VOLUME, 0.05f);     // far below the master limiter
            }
            audio.prepare(44100.0, 256);

            // Each track starts with the engine's default: pressure -> level.
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == AftertouchRouter::Destination::Volume);
            // (The old "one router per track" address test is gone: routers are now on the engine.)

            // Track 1: pressure raises the level. Track 2: pressure takes it away.
            audio.setPressureRoute(0, 0, { AftertouchRouter::Destination::Volume, 1.0f, false });
            audio.setPressureRoute(1, 0, { AftertouchRouter::Destination::Volume, -1.0f, false });

            const float rest0 = pressedEnergy(audio, 0, 0.0f);
            const float rest1 = pressedEnergy(audio, 1, 0.0f);
            const float hard0 = pressedEnergy(audio, 0, 1.0f);
            const float hard1 = pressedEnergy(audio, 1, 1.0f);
            expect(rest0 > 1.0e-5f && rest1 > 1.0e-5f, "both tracks sound");
            expect(rest0 / rest1 > 0.5f && rest0 / rest1 < 2.0f, "without pressure they are alike");
            expect(hard0 > rest0 * 2.0f, "track 1 gets louder with pressure");
            expect(hard1 < rest1 * 0.1f, "track 2 is silenced by the same pressure");

            // Changing one track's route leaves the other alone.
            audio.setPressureRoute(1, 0, { AftertouchRouter::Destination::Off, 0.5f, false });
            const float off1 = pressedEnergy(audio, 1, 1.0f);
            expect(off1 > rest1 * 0.5f && off1 < rest1 * 2.0f, "route off: pressure does nothing on track 2");
            expect(pressedEnergy(audio, 0, 1.0f) > rest0 * 2.0f, "track 1 still responds");
        }

        beginTest("Pressure: defaults per engine, engine change, the legacy single router");
        {
            using D = AftertouchRouter::Destination;
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addEngine(1, AudioEngine::createEngine(EngineType::Drum));
            audio.addEngine(2, AudioEngine::createEngine(EngineType::Sampler));
            audio.prepare(44100.0, 256);

            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::Volume);
            expect(audio.getPressureRouter(1).effectiveRoute(0).dest == D::DrumDecay, "drums: pressure -> decay");
            expect(audio.getPressureRouter(2).effectiveRoute(0).dest == D::Off, "the sampler has no default pressure route");

            // An old route (a project from before the pressure slots had cells) to something AT
            // ROUTE did not offer the engine then is refused, as it was.
            audio.setPressureRoute(2, 0, { D::Volume, 1.0f, false });
            expect(audio.getPressureRouter(2).effectiveRoute(0).dest == D::Off, "an old project's route never reached a sampler");
            audio.setPressureRoute(0, 1, { D::DrumSnap, 1.0f, false });
            expect(audio.getPressureRouter(0).effectiveRoute(1).dest == D::Off);
            // The slots themselves take anything of the engine's list: a sampler's level, too.
            audio.setPressureSlot(2, 0, SamplerEngine::lfoDests().indexOfLabel("AMP"), 1.0f, false);
            expect(audio.getPressureRouter(2).effectiveRoute(0).dest == D::Volume, "the sampler's slot on its level, read as a route");
            audio.resetPressureRouter(2);
            expect(audio.getPressureRouter(2).effectiveRoute(0).dest == D::Off, "and back as a sampler is made");

            // A drum track's route reaches every pad's voice.
            audio.setPressureRoute(1, 0, { D::DrumPitch, 0.5f, false });
            auto* drum = dynamic_cast<DrumEngine*>(audio.getEngine(1));
            for (int slot = 0; slot < DrumEngine::NUM_VOICES; ++slot)
                expect(drum->getCurrentKit().slots[(size_t) slot].aftertouchDest == DrumATDest::Pitch);
            expect(drum->getVoice(5).getAftertouchDest() == DrumVoiceATDest::Pitch);
            expectWithinAbsoluteError(drum->getVoice(5).getAftertouchDepth(), 0.5f, 0.001f);

            // Changing the engine type gives the new engine's default.
            // Rebuilding the same type (force, as a preset load) also gives the defaults: the routes
            // are cells of the engine (part of its sound), and whoever loads a sound sets them.
            audio.setPressureRoute(0, 0, { D::FilterCutoff, 0.3f, false });
            audio.setEngineType(0, EngineType::UnifiedVoice, true);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::Volume, "same-type rebuild gives defaults");
            audio.setEngineType(0, EngineType::Drum);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::DrumDecay);
            audio.setEngineType(0, EngineType::FM);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::Volume);

            // EngineBase::setAftertouchRouter loads old routes into the engine's pressure slots, once.
            AftertouchRouter legacy;
            legacy.setRoute(0, { D::FilterCutoff, 0.8f, false });
            audio.getEngine(0)->setAftertouchRouter(&legacy);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::FilterCutoff);
            expectWithinAbsoluteError(audio.getPressureRouter(0).effectiveRoute(0).depth, 0.8f, 0.001f);
            expect(audio.getPressureRouter(1).effectiveRoute(0).dest == D::DrumPitch, "the drum track keeps its own");
        }

        beginTest("AT ROUTE: edits the selected track's routes and offers only that engine's destinations");
        {
            using D = AftertouchRouter::Destination;
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            const EngineType types[] = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                                         EngineType::WaveSeq, EngineType::Sampler, EngineType::Drum, EngineType::MIDITrack };
            for (int t = 0; t < 8; ++t)
                audio.addEngine(t, AudioEngine::createEngine(types[t]));
            int track = 0;
            AftertouchScreen screen(audio, [&] { return track; });

            // Every engine on the router has 4 pressure slots; drums have 1; MIDI has 0.
            // The DEST choices: OFF, what only pressure reaches (BOW on the macro oscillator, VIB
            // everywhere), then the engine's whole table, then the track's effect cells.
            const int slots[]  = { 4, 4, 4, 4, 4, 4, 1, 0 };
            const char* const second[] = { "VIB", "VIB", "BOW", "VIB", "VIB", "VIB", "VIB", "" };
            for (track = 0; track < 8; ++track)
            {
                const int count = screen.getDestCount();
                expectEquals(screen.getRouteSlots(), slots[track]);
                const char* const* labels = screen.getDestLabels();
                if (track != 7)     // (a MIDI track has none)
                {
                    const auto& table = audio.getEngine(track)->modRouter()->dests();
                    expectEquals(count, table.size() + 1 + mod::TrackDest::COUNT, "engine " + juce::String(track));
                    expectEquals(juce::String(labels[0]), juce::String("OFF"));
                    expectEquals(juce::String(labels[1]), juce::String(second[track]), "first after OFF, engine " + juce::String(track));
                    expectEquals(juce::String(labels[count - 1]), juce::String("RO.D"), "the track's effect cells last");
                    // (every value has its place in the menu and back)
                    for (int v = 0; v < count; ++v)
                        expectEquals(table.pressureValueAt(table.pressurePlaceOf(v)), v);
                }
                DisplayRenderer display;
                screen.render(display);
            }

            // Cells: DEST on top, DPTH below.
            track = 0;
            for (int i = 0; i < 4; ++i)
            {
                expectEquals(screen.getEncoderName(i), juce::String("DEST"));
                expectEquals(screen.getEncoderName(4 + i), juce::String("DPTH"));
            }

            const auto slotDest = [&](int t, int slot) { return (int) audio.getTrackParameter(t, PressureSlotParam::id(slot, PressureSlotParam::DEST_OFFSET)); };
            const auto slotAmount = [&](int t, int slot) { return audio.getTrackParameter(t, PressureSlotParam::id(slot, PressureSlotParam::AMT_OFFSET)); };
            const auto slotBipolar = [&](int t, int slot) { return audio.getTrackParameter(t, PressureSlotParam::id(slot, PressureSlotParam::BIPOLAR_OFFSET)); };

            // Wave-sequence track: one click from OFF is VIB, the vibrato the pad brings in (the
            // old route, one turn away); the next is the table's first entry.
            track = 4;
            const auto& seqTable = audio.getEngine(4)->modRouter()->dests();
            screen.onEncoderChange(1, enc(1));
            expectEquals(slotDest(4, 1), seqTable.pressureVibrato());
            expectEquals(screen.getEncoderReadout(1), juce::String("DEST 2 VIB"));
            expect(audio.getPressureRouter(4).effectiveRoute(1).dest == D::Vibrato, "read as a route: the old VIB");
            expectEquals(slotAmount(4, 1), 0.5f, "a route that is chosen starts at half");
            screen.onEncoderChange(1, enc(1));
            expectEquals(slotDest(4, 1), seqTable.indexOfLabel("PTCH"));
            expectEquals(screen.getEncoderReadout(1), juce::String("DEST 2 PTCH"));
            expectEquals(slotDest(0, 1), 0, "the other tracks are untouched");
            expectEquals(slotDest(3, 1), 0);
            // Func + turn goes from group to group, and ends on the track's effect cells.
            screen.setModifiers(false, true);
            for (int i = 0; i < 40; ++i)
                screen.onEncoderChange(1, enc(1));
            screen.setModifiers(false, false);
            expectEquals(slotDest(4, 1), seqTable.pressureChoices() - 1);
            screen.onEncoderChange(1, enc(-60));
            screen.onEncoderChange(1, enc(-60));
            screen.onEncoderChange(1, enc(-60));
            screen.onEncoderChange(1, enc(-60));
            expectEquals(slotDest(4, 1), 0, "all the way back: OFF");

            // The same knob on another track edits that track.
            track = 0;
            screen.onEncoderChange(1, enc(2));
            expectEquals(slotDest(0, 1), (int) UnifiedVoice::RPitch);

            // Depth: plain 1 % of the -100..+100 range, Shift a tenth, Func to the snap points.
            audio.setTrackParameter(0, PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET), 0.2f);
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(slotAmount(0, 1), 0.22f, 0.0005f);
            screen.setModifiers(true, false);
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(slotAmount(0, 1), 0.222f, 0.0005f);
            screen.setModifiers(false, true);
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(slotAmount(0, 1), 0.5f, 0.0005f);
            screen.onEncoderChange(5, enc(-3));
            expectWithinAbsoluteError(slotAmount(0, 1), -1.0f, 0.0005f);
            screen.setModifiers(false, false);

            // Page 2 (the bank buttons go round): BIP, the pad read both ways, clockwise = on.
            expectEquals(screen.getPageCount(), 2);
            expectEquals(screen.getPageIndex(), 0);
            screen.onArrowRight();
            expectEquals(screen.getPageIndex(), 1);
            expectEquals(screen.getSubPage(), juce::String("BIP"));
            for (int i = 0; i < 4; ++i)
            {
                expectEquals(screen.getEncoderName(i), juce::String("BIP"));
                expectEquals(screen.getEncoderName(4 + i), juce::String("---"));
            }
            screen.onEncoderChange(1, enc(1));
            expectEquals(slotBipolar(0, 1), 1.0f);
            expectEquals(screen.getEncoderReadout(1), juce::String("BIPOLAR 2 ON"));
            screen.onEncoderChange(1, enc(1));
            expectEquals(slotBipolar(0, 1), 1.0f, "clockwise again: still on");
            screen.onEncoderChange(1, enc(-1));
            expectEquals(slotBipolar(0, 1), 0.0f);
            {
                const float depth = slotAmount(0, 1);
                const int said = TurnNotice::count();
                screen.onEncoderChange(5, enc(3));      // (nothing under the bottom knobs of this page)
                expectEquals(slotAmount(0, 1), depth);
                expectEquals(TurnNotice::count(), said, "and they stay silent");
                screen.onEncoderChange(2, enc(1));      // route 3 is off: it has no way to be read
                expectEquals(slotBipolar(0, 2), 0.0f);
                expectEquals(screen.getSubPage(), juce::String("ROUTE OFF"));
                DisplayRenderer display;
                screen.render(display);
            }
            screen.onArrowRight();
            expectEquals(screen.getPageIndex(), 0, "round to the first page");
            screen.onArrowLeft();
            expectEquals(screen.getPageIndex(), 1, "and up from the first is the last");
            screen.onArrowLeft();

            // Sampler: four slots, nothing routed as it is made; its level is one of its destinations.
            track = 5;
            expectEquals(screen.getRouteSlots(), 4);
            for (int s = 0; s < 4; ++s)
                expectEquals(slotDest(5, s), 0);

            // Drum kit: one route; the second column is empty.
            track = 6;
            const auto& kit = audio.getEngine(6)->modRouter()->dests();
            auto* drums = dynamic_cast<DrumEngine*>(audio.getEngine(6));
            screen.onEncoderChange(1, enc(3));
            expectEquals(slotDest(6, 1), 0);
            expectEquals(slotDest(6, 0), kit.indexOfLabel("DEC"), "as a kit is made: squeezing a pad lengthens it");
            screen.onEncoderChange(0, enc(1));
            expectEquals(slotDest(6, 0), kit.indexOfLabel("TONE"), "DEC -> TONE");
            expect(drums->getVoice(3).getAftertouchDest() == DrumVoiceATDest::Filter, "the turn on the screen reaches every drum");
            // A drum's own destinations take the pad one way: 0 .. 1, and BIP says so.
            screen.onEncoderChange(4, enc(-60));
            screen.onEncoderChange(4, enc(-60));
            expectEquals(slotAmount(6, 0), 0.0f, "no negative depth on a drum's own destination");
            screen.onEncoderChange(4, enc(30));
            expectWithinAbsoluteError(drums->getVoice(3).getAftertouchDepth(), 0.3f, 0.001f);
            screen.onArrowRight();
            screen.onEncoderChange(0, enc(1));
            expectEquals(slotBipolar(6, 0), 0.0f);
            expectEquals(screen.getSubPage(), juce::String("KIT ONE WAY"));
            screen.onArrowLeft();
            {
                // In the name bar, through ScreenManager: the page's name and the notice are there
                // whole ("PRESSURE" and "BIPOLAR" were shown as "SURE" and "OLAR": four characters
                // are what the bar has beside "AT ROUTE T1" and the page mark; "KIT: ONE WAY" lost
                // its last letter).
                ScreenManager mgr;
                DisplayRenderer display;
                mgr.setDisplay(&display);
                int shown = 0;
                mgr.addScreen(std::make_unique<AftertouchScreen>(audio, [&] { return shown; }));
                TurnNotice::setSink([&mgr](const juce::String& text) { mgr.showStatus(text); });    // (as the app has it)
                const auto bank = [&] { mgr.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_BANK_DOWN, 127);
                                        mgr.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_BANK_DOWN, 0); };
                UiClock::advance(3000);
                expectEquals(mgr.composeNameBar().left, juce::String("AT ROUTE T1"));
                expectEquals(mgr.composeNameBar().text, juce::String("DEST"));
                expectEquals(mgr.composeNameBar().pages, juce::String("1/2"));
                bank();
                expectEquals(mgr.composeNameBar().text, juce::String("BIP"));
                expectEquals(mgr.composeNameBar().pages, juce::String("2/2"));
                bank();
                expectEquals(mgr.composeNameBar().text, juce::String("DEST"), "the bank buttons go round");
                shown = 6;      // the kit, its one route on DEC: BIP says why it does not turn
                bank();
                mgr.controlChangeReceived(16, 0x15, enc(1));
                expectEquals(mgr.composeNameBar().text, juce::String("KIT ONE WAY"));
                UiClock::advance(3000);
                shown = 7;      // a MIDI track: no routes, one page, and the picture says what happens
                expectEquals(mgr.composeNameBar().text, juce::String());
                expectEquals(mgr.composeNameBar().pages, juce::String());
                TurnNotice::setSink({});
            }
            // Aimed at something that is not a drum's own (VIB), the route is the kit router's:
            // the drums' own path is off.
            audio.setTrackParameter(6, PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET), (float) kit.pressureVibrato());
            expect(drums->getVoice(3).getAftertouchDest() == DrumVoiceATDest::Off);
            expect(! audio.getEngine(6)->modRouter()->pressureIsOwn(0));

            // Pads play the track; all are lit, the Cs marked.
            std::vector<int> on;
            screen.setNoteCallbacks([&](int n, int) { on.push_back(n); }, [](int) {}, [](int, int) {});
            screen.onPadPress(4, 100);
            expectEquals(on.back(), 64);
            std::array<Screen::PadColour, 16> c;
            expect(screen.getPadColours(c));
            for (auto& p : c) expect(!dark(p));
            expect(!same(c[0], c[1]) && same(c[0], c[12]));
            screen.onPadRelease(4);
        }

        beginTest("Pressure routes are saved per track; an old project's single list goes to every track");
        {
            using D = AftertouchRouter::Destination;
            auto build = [](SequencerCore& seq, AudioEngine& audio)
            {
                audio.setSequencer(&seq);
                const EngineType types[] = { EngineType::UnifiedVoice, EngineType::Drum, EngineType::Wavetable };
                for (int t = 0; t < 3; ++t)
                {
                    audio.addEngine(t, AudioEngine::createEngine(types[t]));
                    for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                        seq.getPattern(p).getTrack(t).engineType = types[t];
                }
            };

            SequencerCore seq;
            AudioEngine audio;
            build(seq, audio);
            audio.setPressureRoute(0, 0, { D::FilterCutoff, 0.6f, false });
            audio.setPressureRoute(0, 2, { D::Vibrato, -0.25f, false });
            audio.setPressureRoute(1, 0, { D::DrumSnap, 0.9f, false });
            audio.setPressureRoute(2, 0, { D::WavePosition, 0.4f, false });

            const auto saved = ProjectBridge::capture({ seq, audio }, "ROUTES");
            expect((int) saved.trackAftertouch.size() >= 3);
            const auto reloaded = ProjectData::fromVar(saved.toVar());

            SequencerCore seq2;
            AudioEngine audio2;
            ProjectBridge::apply(reloaded, { seq2, audio2 });
            expect(audio2.getPressureRouter(0).effectiveRoute(0).dest == D::FilterCutoff);
            expectWithinAbsoluteError(audio2.getPressureRouter(0).effectiveRoute(0).depth, 0.6f, 0.001f);
            expect(audio2.getPressureRouter(0).effectiveRoute(1).dest == D::Off);
            expect(audio2.getPressureRouter(0).effectiveRoute(2).dest == D::Vibrato);
            expectWithinAbsoluteError(audio2.getPressureRouter(0).effectiveRoute(2).depth, -0.25f, 0.001f);
            expect(audio2.getPressureRouter(1).effectiveRoute(0).dest == D::DrumSnap);
            expect(audio2.getPressureRouter(2).effectiveRoute(0).dest == D::WavePosition);
            auto* drum = dynamic_cast<DrumEngine*>(audio2.getEngine(1));
            expect(drum != nullptr && drum->getVoice(0).getAftertouchDest() == DrumVoiceATDest::Snap);

            // An old project: no per-track lists, one global list (and, as every project from
            // before the pressure slots had cells, none of their ids among a track's parameters).
            const auto withoutSlots = [](ProjectData data)
            {
                for (auto& pattern : data.patterns)
                    for (auto& track : pattern.tracks)
                        for (int id = PressureSlotParam::BASE; id < PressureSlotParam::END; ++id)
                            track.soundParams.erase(id);
                return data;
            };
            {
                // What was saved holds the slots, and they are what a load reads: the lists beside
                // them are for an older build. (Changed lists, the same slots: nothing changes.)
                const auto& params = reloaded.patterns[(size_t) reloaded.activePattern].tracks[0].soundParams;
                expect(params.count(PressureSlotParam::id(0, PressureSlotParam::DEST_OFFSET)) == 1);
                auto lists = reloaded;
                lists.trackAftertouch[0].routes[0].destination = (int) D::Volume;
                SequencerCore seq4;
                AudioEngine audio4;
                ProjectBridge::apply(lists, { seq4, audio4 });
                expect(audio4.getPressureRouter(0).effectiveRoute(0).dest == D::FilterCutoff, "the slots are the truth");
                // And without the slots the lists are read, as an older project's.
                SequencerCore seq5;
                AudioEngine audio5;
                ProjectBridge::apply(withoutSlots(lists), { seq5, audio5 });
                expect(audio5.getPressureRouter(0).effectiveRoute(0).dest == D::Volume, "an older project: its list");
                expect(audio5.getPressureRouter(0).effectiveRoute(2).dest == D::Vibrato);
                expect(audio5.getPressureRouter(1).effectiveRoute(0).dest == D::DrumSnap);
            }
            auto old = withoutSlots(reloaded);
            old.trackAftertouch.clear();
            old.aftertouchRouting.routes = { { (int) D::FilterCutoff, 0.7f, false }, { (int) D::Volume, 0.2f, false } };
            const auto oldReloaded = ProjectData::fromVar(old.toVar());
            expect(oldReloaded.trackAftertouch.empty());

            SequencerCore seq3;
            AudioEngine audio3;
            AftertouchRouter legacy;
            ProjectBridge::apply(oldReloaded, { seq3, audio3, nullptr, nullptr, &legacy });
            for (int t : { 0, 2 })
            {
                expect(audio3.getPressureRouter(t).effectiveRoute(0).dest == D::FilterCutoff, "the old list on every synth track");
                expectWithinAbsoluteError(audio3.getPressureRouter(t).effectiveRoute(0).depth, 0.7f, 0.001f);
                expect(audio3.getPressureRouter(t).effectiveRoute(1).dest == D::Volume);
            }
            expect(audio3.getPressureRouter(1).effectiveRoute(0).dest == D::DrumDecay, "it never reached the drums: default kept");
            expect(legacy.getRoute(0).dest == D::FilterCutoff, "the legacy router shows it too");

            // A project with neither (a new one): every track back to its engine's default.
            auto blank = withoutSlots(reloaded);
            blank.trackAftertouch.clear();
            blank.aftertouchRouting.routes.clear();
            ProjectBridge::apply(blank, { seq2, audio2 });
            expect(audio2.getPressureRouter(0).effectiveRoute(0).dest == D::Volume);
            expect(audio2.getPressureRouter(0).effectiveRoute(2).dest == D::Off);
            expect(audio2.getPressureRouter(1).effectiveRoute(0).dest == D::DrumDecay);
        }
    }
};
static ScreenGrammarTests ScreenGrammarTestsInstance;
