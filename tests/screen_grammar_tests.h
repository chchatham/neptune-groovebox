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
            expect(&audio.getPressureRouter(0) != &audio.getPressureRouter(1), "one router per track");

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
            expect(audio.getPressureRouter(2).effectiveRoute(0).dest == D::Off, "the sampler has no pressure response");

            // A destination the engine does not respond to is refused.
            audio.setPressureRoute(2, 0, { D::Volume, 1.0f, false });
            expect(audio.getPressureRouter(2).effectiveRoute(0).dest == D::Off);
            audio.setPressureRoute(0, 1, { D::DrumSnap, 1.0f, false });
            expect(audio.getPressureRouter(0).effectiveRoute(1).dest == D::Off);

            // A drum track's route reaches every pad's voice.
            audio.setPressureRoute(1, 0, { D::DrumPitch, 0.5f, false });
            auto* drum = dynamic_cast<DrumEngine*>(audio.getEngine(1));
            for (int slot = 0; slot < DrumEngine::NUM_VOICES; ++slot)
                expect(drum->getCurrentKit().slots[(size_t) slot].aftertouchDest == DrumATDest::Pitch);
            expect(drum->getVoice(5).getAftertouchDest() == DrumVoiceATDest::Pitch);
            expectWithinAbsoluteError(drum->getVoice(5).getAftertouchDepth(), 0.5f, 0.001f);

            // Changing the engine gives the new engine's default; rebuilding the same engine keeps the routes.
            audio.setPressureRoute(0, 0, { D::FilterCutoff, 0.3f, false });
            audio.setEngineType(0, EngineType::UnifiedVoice, true);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::FilterCutoff);
            audio.setEngineType(0, EngineType::Drum);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::DrumDecay);
            audio.setEngineType(0, EngineType::FM);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::Volume);

            // Legacy: an edit to the one router given to setAftertouchRouter reaches every track
            // that uses a router (not the drum kit).
            AftertouchRouter legacy;
            audio.setAftertouchRouter(&legacy);
            std::vector<float> l(256), r(256);
            audio.renderBlock(l.data(), r.data(), 256);
            expect(audio.getPressureRouter(0).effectiveRoute(0).dest == D::Volume, "setting it changes nothing by itself");
            legacy.setRoute(0, { D::FilterCutoff, 0.8f, false });
            audio.renderBlock(l.data(), r.data(), 256);
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

            // How many choices each engine's DEST cell has (OFF + its destinations), and one label.
            const int counts[] = { 7, 4, 6, 5, 3, 1, 8, 1 };
            const int slots[]  = { 4, 4, 4, 4, 4, 0, 1, 0 };
            for (track = 0; track < 8; ++track)
            {
                expectEquals(screen.getDestCount(), counts[track], "destinations of engine " + juce::String(track));
                expectEquals(screen.getRouteSlots(), slots[track]);
                expectEquals(screen.getDestLabel(0), juce::String("OFF"));
                for (int i = 1; i < screen.getDestCount(); ++i)
                    expect(AudioEngine::pressureDestinationValid(types[track], screen.getDestAt(i)));
                DisplayRenderer display;
                screen.render(display);         // every engine draws without touching a missing route
            }
            track = 0; expectEquals(screen.getDestLabel(6), juce::String("XMOD"));
            track = 1; expectEquals(screen.getDestLabel(3), juce::String("FM"));
            track = 3; expectEquals(screen.getDestLabel(4), juce::String("POS"));
            track = 6; expectEquals(screen.getDestLabel(1), juce::String("DEC"));

            // Cells: DEST on top, DPTH below.
            track = 0;
            for (int i = 0; i < 4; ++i)
            {
                expectEquals(screen.getEncoderName(i), juce::String("DEST"));
                expectEquals(screen.getEncoderName(4 + i), juce::String("DPTH"));
            }

            // Wave-sequence track: turning DEST all the way never leaves its two destinations.
            track = 4;
            screen.onEncoderChange(1, enc(20));
            expect(audio.getPressureRouter(4).effectiveRoute(1).dest == D::FilterCutoff);
            expectEquals(screen.getEncoderReadout(1), juce::String("DEST 2 CUT"));
            expect(audio.getPressureRouter(0).effectiveRoute(1).dest == D::Off, "the other tracks are untouched");
            expect(audio.getPressureRouter(3).effectiveRoute(1).dest == D::Off);

            // The same knob on another track edits that track.
            track = 0;
            screen.onEncoderChange(1, enc(3));
            expect(audio.getPressureRouter(0).effectiveRoute(1).dest == D::Vibrato);
            expect(audio.getPressureRouter(4).effectiveRoute(1).dest == D::FilterCutoff);

            // Depth: plain 1 % of the -100..+100 range, Shift a tenth, Func to the snap points.
            audio.setPressureRoute(0, 1, { D::Vibrato, 0.2f, false });
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(audio.getPressureRouter(0).effectiveRoute(1).depth, 0.22f, 0.0005f);
            screen.setModifiers(true, false);
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(audio.getPressureRouter(0).effectiveRoute(1).depth, 0.222f, 0.0005f);
            screen.setModifiers(false, true);
            screen.onEncoderChange(5, enc(1));
            expectWithinAbsoluteError(audio.getPressureRouter(0).effectiveRoute(1).depth, 0.5f, 0.0005f);
            screen.onEncoderChange(5, enc(-3));
            expectWithinAbsoluteError(audio.getPressureRouter(0).effectiveRoute(1).depth, -1.0f, 0.0005f);
            screen.setModifiers(false, false);

            // Sampler: nothing to route; the knobs do nothing.
            track = 5;
            for (int k = 0; k < 8; ++k) screen.onEncoderChange(k, enc(3));
            for (int i = 0; i < 4; ++i)
                expect(audio.getPressureRouter(5).effectiveRoute(i).dest == D::Off);

            // Drum kit: one route; the second column is empty.
            track = 6;
            screen.onEncoderChange(1, enc(3));
            expect(audio.getPressureRouter(6).effectiveRoute(1).dest == D::Off);
            screen.onEncoderChange(0, enc(1));
            expect(audio.getPressureRouter(6).effectiveRoute(0).dest == D::DrumPitch, "DEC -> TUNE");

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

            // An old project: no per-track lists, one global list.
            auto old = reloaded;
            old.trackAftertouch.clear();
            old.aftertouchRouting.routes = { { (int) D::FilterCutoff, 0.7f, false }, { (int) D::Volume, 0.2f, false } };
            const auto oldReloaded = ProjectData::fromVar(old.toVar());
            expect(oldReloaded.trackAftertouch.empty());

            SequencerCore seq3;
            AudioEngine audio3;
            AftertouchRouter legacy;
            audio3.setAftertouchRouter(&legacy);
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
            auto blank = reloaded;
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
