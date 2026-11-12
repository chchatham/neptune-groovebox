#pragma once
#include <juce_core/juce_core.h>
#include <set>
#include <vector>
#include "../src/ui/ScreenManager.h"
#include "../src/ui/screens/HomeScreen.h"
#include "../src/ui/screens/DrumPadScreen.h"
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/PadLEDFeedback.h"
#include "../src/controller/PadManager.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/DrumEngine.h"

// A drum track must sound from everything that plays notes: the DRUMS pads, the 25 keys at any
// octave setting, recorded steps and steps tapped in on STEP. Driven the way the hardware drives
// the app: MIDI-level messages into ScreenManager, the ENG page's own calls to switch engine.
namespace drumkeys
{
    // What main.cpp's KeyboardToEngine does with a key (arpeggiator off).
    struct Keys : ScreenManager::KeyboardListener
    {
        AudioEngine* audio = nullptr;
        SequencerCore* seq = nullptr;
        void keyboardNoteOn(int note, int velocity) override
        {
            const int t = seq->getSelectedTrack();
            if (auto* e = audio->getEngine(t)) e->noteOn(note, (float) velocity / 127.0f, -1);
            if (seq->isRecording() && seq->isPlaying()) seq->recordNote(t, note, (float) velocity / 127.0f);
        }
        void keyboardNoteOff(int note) override
        {
            if (auto* e = audio->getEngine(seq->getSelectedTrack())) e->noteOff(note, -1);
        }
    };

    // The app as it starts (track 2 the drum kit, the others the synth), prepared for rendering.
    struct Rig
    {
        static constexpr int BLOCK = 512;
        SequencerCore seq;
        AudioEngine audio;
        std::vector<float> l = std::vector<float>(BLOCK), r = std::vector<float>(BLOCK);

        Rig()
        {
            seq.prepareToPlay(44100.0, BLOCK);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(t == 1 ? EngineType::Drum : EngineType::UnifiedVoice));
            seq.getPattern().getTrack(1).engineType = EngineType::Drum;
            audio.prepare(44100.0, BLOCK);
        }

        // Peak of the next `blocks` blocks of the whole instrument.
        float render(int blocks)
        {
            float peak = 0.0f;
            for (int b = 0; b < blocks; ++b)
            {
                audio.renderBlock(l.data(), r.data(), BLOCK);
                for (int i = 0; i < BLOCK; ++i)
                    peak = std::max(peak, std::max(std::abs(l[(size_t) i]), std::abs(r[(size_t) i])));
            }
            return peak;
        }

        void silence()
        {
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                if (auto* e = audio.getEngine(t)) e->allNotesOff();
            render(8);
        }

        DrumEngine* drums(int track) { return dynamic_cast<DrumEngine*>(audio.getEngine(track)); }

        // The ENG page as the owner uses it: turn the track's knob to DR, tap the track's top pad.
        void switchToDrums(HomeScreen& home, int track)
        {
            home.setPage(HomeScreen::ENG);
            for (int i = 0; i < (int) EngineType::Drum; ++i)
                home.onEncoderChange(track, 1);
            home.onPadPress(track, 100);
        }

        // Which voices are sounding.
        std::set<int> active(int track)
        {
            std::set<int> out;
            if (auto* d = drums(track))
                for (int v = 0; v < DrumEngine::NUM_VOICES; ++v)
                    if (d->getVoice(v).isActive()) out.insert(v);
            return out;
        }
    };
}

class DrumKeysTests : public juce::UnitTest
{
public:
    DrumKeysTests() : UnitTest("Drum Keys") {}

    void runTest() override
    {
        using drumkeys::Rig;
        static constexpr int KEYS = LaunchkeyController::KEYS_PORT_CHANNEL;

        beginTest("Note to pad: pads and pad notes as before; the keys from C = lower left pad");
        {
            for (int n = 0; n < 16; ++n)
                expectEquals(DrumEngine::padForNote(n), n);
            for (int n = 0; n < 8; ++n)
            {
                expectEquals(DrumEngine::padForNote(96 + n), n);
                expectEquals(DrumEngine::padForNote(112 + n), 8 + n);
            }
            // Octave 0: the 25 keys are notes 48-72.
            for (int k = 0; k < 8; ++k)
            {
                expectEquals(DrumEngine::padForNote(48 + k), 8 + k, "C..G = bottom row");
                expectEquals(DrumEngine::padForNote(56 + k), k, "G#..D# = top row");
                expectEquals(DrumEngine::padForNote(64 + k), 8 + k, "E..B = bottom row again");
            }
            expectEquals(DrumEngine::padForNote(72), 8, "the top C is the lower left pad again");
            expectEquals(DrumEngine::padForNote(-1), -1);
            expectEquals(DrumEngine::padForNote(128), -1);
        }

        beginTest("Every note plays a pad, and the 25 keys reach the pads at every octave setting");
        {
            for (int n = 0; n <= 127; ++n)
                expect(DrumEngine::padForNote(n) >= 0 && DrumEngine::padForNote(n) < 16, "note " + juce::String(n));
            for (int octave = -4; octave <= 4; ++octave)
            {
                std::set<int> pads;
                for (int key = 0; key < 25; ++key)
                    if (48 + 12 * octave + key <= 127)
                        pads.insert(DrumEngine::padForNote(48 + 12 * octave + key));
                // Three octaves down the keys start on notes 12-15, which belong to the pads
                // themselves: top pads 6-8 are not on the keys there. The drums' row always is.
                if (octave == -3)
                    expectEquals((int) pads.size(), 13);
                else
                    expectEquals((int) pads.size(), 16, "octave " + juce::String(octave));
                for (int pad = 8; pad < 16; ++pad)
                    expect(pads.count(pad) == 1, "bottom row at octave " + juce::String(octave));
            }
        }

        beginTest("A track switched to DR on the ENG page has the start-up kit");
        {
            Rig rig;
            HomeScreen home(rig.seq, &rig.audio);
            rig.switchToDrums(home, 3);

            auto* fresh = rig.drums(3);
            auto* startup = rig.drums(1);
            expect(fresh != nullptr && startup != nullptr);
            if (fresh == nullptr || startup == nullptr) return;
            expectEquals((int) rig.seq.getPattern().getTrack(3).engineType, (int) EngineType::Drum);
            expectEquals(rig.seq.getSelectedTrack(), 3);

            for (const auto& page : fresh->getParamPages())
                for (const auto& spec : page.params)
                    if (spec.label != nullptr && spec.label[0] != 0)
                        expectWithinAbsoluteError(rig.audio.getTrackParameter(3, spec.id), rig.audio.getTrackParameter(1, spec.id),
                                                  1.0e-6f, page.name + " " + spec.label);
            for (int pad = 0; pad < 16; ++pad)
            {
                expectEquals(fresh->getVoiceForPad(pad), pad);
                expect(fresh->getParameter(pad * DrumEngine::PARAMS_PER_VOICE + DrumEngine::LEVEL) > 0.0f, "pad " + juce::String(pad + 1) + " has a level");
            }
            expect(rig.audio.getTrackParameter(3, MixParam::VOLUME) > 0.0f, "the track is not turned down");
        }

        beginTest("A track switched to DR sounds from padNoteOn on all 16 pads");
        {
            Rig rig;
            HomeScreen home(rig.seq, &rig.audio);
            rig.switchToDrums(home, 0);
            expect(rig.render(8) < 1.0e-4f, "silent before a pad is hit");

            for (int pad = 0; pad < 16; ++pad)
            {
                rig.audio.padNoteOn(0, pad, pad, 0.9f);
                const float peak = rig.render(6);
                expect(peak > 0.01f, "pad " + juce::String(pad + 1) + " peak " + juce::String(peak, 4));
                rig.audio.padNoteOff(pad);
                rig.silence();
            }
        }

        beginTest("DRUMS follows the track just switched to DR: a pad message plays its voice");
        {
            Rig rig;
            ScreenManager mgr;
            mgr.setSequencer(&rig.seq);
            auto* home = new HomeScreen(rig.seq, &rig.audio);
            mgr.addScreen(std::unique_ptr<Screen>(home));

            // Wired as in main.cpp.
            auto drumTrack = [&rig]
            {
                const int sel = rig.seq.getSelectedTrack();
                if (rig.drums(sel) != nullptr) return sel;
                for (int t = 0; t < rig.seq.getPattern().getTrackCount(); ++t)
                    if (rig.drums(t) != nullptr) return t;
                return -1;
            };
            auto* pads = new DrumPadScreen(rig.drums(1));
            pads->setEngineProvider([&rig, drumTrack] { const int t = drumTrack(); return t >= 0 ? rig.drums(t) : nullptr; });
            DrumPadScreen::PadHooks hooks;
            hooks.down = [&rig, drumTrack](int pad, float vel) { const int t = drumTrack(); if (t >= 0) rig.audio.padNoteOn(t, pad, pad, vel); };
            hooks.up = [&rig](int pad) { rig.audio.padNoteOff(pad); };
            pads->setPadHooks(std::move(hooks));
            mgr.addScreen(std::unique_ptr<Screen>(pads));

            mgr.setActiveScreen(0);
            rig.switchToDrums(*home, 4);
            auto* oldEngine = rig.drums(1);
            mgr.setActiveScreen(1);

            mgr.noteOnReceived(1, LaunchkeyController::PAD_BOTTOM_ROW_START + 4, 110);      // bottom pad 5: the snare
            const float peak = rig.render(6);
            expect(peak > 0.01f, "the pad is heard, peak " + juce::String(peak, 4));
            expect(oldEngine->getVoice(12).isActive() == false, "the other drum track stays quiet");
            expectEquals(rig.drums(4)->getSelectedVoice(), 12);
            mgr.noteOffReceived(1, LaunchkeyController::PAD_BOTTOM_ROW_START + 4);
        }

        beginTest("A key message plays the kit on a drum track, with its velocity");
        {
            Rig rig;
            ScreenManager mgr;
            mgr.setSequencer(&rig.seq);
            auto* home = new HomeScreen(rig.seq, &rig.audio);
            mgr.addScreen(std::unique_ptr<Screen>(home));
            drumkeys::Keys keys;
            keys.audio = &rig.audio;
            keys.seq = &rig.seq;
            mgr.setKeyboardListener(&keys);
            mgr.setActiveScreen(0);
            rig.switchToDrums(*home, 2);

            // All 25 keys at octave 0, each alone.
            for (int note = 48; note <= 72; ++note)
            {
                mgr.noteOnReceived(KEYS, note, 110);
                const auto on = rig.active(2);
                expect(on.size() == 1 && *on.begin() == DrumEngine::padForNote(note), "key " + juce::String(note));
                const float peak = rig.render(6);
                expect(peak > 0.01f, "key " + juce::String(note) + " peak " + juce::String(peak, 4));
                mgr.noteOffReceived(KEYS, note);
                rig.silence();
            }

            // A key whose number is a pad's note is still a key (it comes from the keys port).
            mgr.noteOnReceived(KEYS, 100, 110);
            expect(rig.active(2).size() == 1, "a key at a high octave plays a voice");
            mgr.noteOffReceived(KEYS, 100);
            rig.silence();

            // Velocity carries through.
            mgr.noteOnReceived(KEYS, 52, 127);          // E3: the snare
            const float loud = rig.render(6);
            mgr.noteOffReceived(KEYS, 52);
            rig.silence();
            mgr.noteOnReceived(KEYS, 52, 30);
            const float soft = rig.render(6);
            mgr.noteOffReceived(KEYS, 52);
            expect(loud > soft * 1.5f && soft > 0.001f, "loud " + juce::String(loud, 4) + " soft " + juce::String(soft, 4));
        }

        beginTest("A recorded key plays the same voice from the pattern");
        {
            Rig rig;
            ScreenManager mgr;
            mgr.setSequencer(&rig.seq);
            drumkeys::Keys keys;
            keys.audio = &rig.audio;
            keys.seq = &rig.seq;
            mgr.setKeyboardListener(&keys);
            rig.seq.setSelectedTrack(1);

            // Playback as in main.cpp's trig callback.
            std::vector<int> played;
            std::set<int> voices;
            rig.seq.setTrigCallback([&](int t, const Step& step)
            {
                for (auto& n : step.notes)
                {
                    rig.audio.getEngine(t)->noteOn(n.pitch, n.velocity, -1);
                    played.push_back(n.pitch);
                }
                for (int v : rig.active(t)) voices.insert(v);
            });

            rig.seq.setRecording(true);
            rig.seq.start();
            rig.render(4);
            mgr.noteOnReceived(KEYS, 53, 100);          // F3: the clap (bottom pad 6)
            const auto live = rig.active(1);
            mgr.noteOffReceived(KEYS, 53);
            rig.seq.setRecording(false);
            expect(live.size() == 1 && *live.begin() == 13);

            int trigs = 0;
            for (int s = 0; s < rig.seq.getPattern().getTrack(1).length; ++s)
            {
                auto& step = rig.seq.getPattern().getTrack(1).getStep(s);
                if (!step.trig) continue;
                ++trigs;
                expectEquals(step.notes[0].pitch, 53);
                expectWithinAbsoluteError(step.notes[0].velocity, 100.0f / 127.0f, 0.001f);
            }
            expectEquals(trigs, 1);

            rig.silence();
            played.clear();
            voices.clear();
            const float peak = rig.render(400);         // more than one bar at 120 BPM
            expect(!played.empty() && played[0] == 53, "the step played back");
            expect(voices.size() == 1 && *voices.begin() == 13, "the pattern plays the clap");
            expect(peak > 0.01f, "and it is heard, peak " + juce::String(peak, 4));
        }

        beginTest("STEP: a step tapped in on a drum track plays the selected drum pad");
        {
            Rig rig;
            PadManager pm;
            PadLEDFeedback leds;
            leds.setSequencer(&rig.seq);
            StepViewScreen stepView(rig.seq, pm, leds);
            ParamPageScreen pages(rig.seq, &rig.audio);
            stepView.setLockPages(&pages);

            rig.seq.setSelectedTrack(1);
            rig.drums(1)->setSelectedVoice(12);         // what tapping the snare on DRUMS does
            stepView.onPadPress(0, 100);
            stepView.onPadRelease(0);
            auto& step = rig.seq.getPattern().getTrack(1).getStep(0);
            expect(step.trig);
            expectEquals(step.notes[0].pitch, 12);

            rig.drums(1)->noteOn(step.notes[0].pitch, step.notes[0].velocity);
            const auto on = rig.active(1);
            expect(on.size() == 1 && *on.begin() == 12);
            expect(rig.render(6) > 0.01f, "the tapped step is heard");

            // A key while the step is held adds that key's voice beside the selected one.
            rig.silence();
            stepView.onPadPress(1, 100);
            expect(stepView.wantsKeyboardEvents());
            stepView.onKeyboardNoteOn(48, 100);
            stepView.onPadRelease(1);
            const auto& held = rig.seq.getPattern().getTrack(1).getStep(1);
            expectEquals((int) held.notes.size(), 2);
            expectEquals(DrumEngine::padForNote(held.notes[0].pitch), 12);
            expectEquals(DrumEngine::padForNote(held.notes[1].pitch), 8);

            // Any other track still starts on middle C.
            rig.seq.setSelectedTrack(0);
            stepView.onPadPress(2, 100);
            stepView.onPadRelease(2);
            expectEquals(rig.seq.getPattern().getTrack(0).getStep(2).notes[0].pitch, 60);
        }

        beginTest("Without the lock pages a tapped step is still middle C, and middle C plays a drum");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            StepViewScreen stepView(seq, pm, leds);
            stepView.onPadPress(0, 100);
            expectEquals(seq.getPattern().getTrack(0).getStep(0).notes[0].pitch, 60);

            DrumEngine drum;
            drum.prepareToPlay(44100.0, 512);
            drum.noteOn(60, 1.0f);
            expect(drum.getVoice(DrumEngine::padForNote(60)).isActive());
        }
    }
};

static DrumKeysTests drumKeysTests;
