#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include "controller/LaunchkeyController.h"
#include "controller/DisplayRenderer.h"
#include "controller/KeyboardSplit.h"
#include "controller/PadManager.h"
#include "ui/ScreenManager.h"
#include "ui/screens/HomeScreen.h"
#include "ui/screens/SettingsScreen.h"
#include "project/ProjectBridge.h"
#include "ui/screens/StepViewScreen.h"
#include "ui/screens/ParamPageScreen.h"
#include "ui/screens/DrumPadScreen.h"
#include "ui/PadLEDFeedback.h"
#include "ui/screens/PresetScreen.h"
#include "ui/screens/WaveformScreen.h"
#include "ui/screens/ArpScreen.h"
#include "ui/screens/CustomScaleScreen.h"
#include "ui/screens/ChordPadScreen.h"
#include "ui/screens/SongScreen.h"
#include "ui/screens/AftertouchScreen.h"
#include "sequencer/SequencerCore.h"
#include "sequencer/SongMode.h"
#include "sequencer/SongPlayer.h"
#include "engine/AftertouchRouter.h"
#include "engine/AudioEngine.h"
#include "engine/ModStripFollower.h"
#include "engine/UnifiedVoiceEngine.h"
#include "engine/FMEngine.h"
#include "engine/MIDITrack.h"
#include "project/PresetManager.h"
#include "arpeggiator/Arpeggiator.h"
#include "controller/CustomScaleManager.h"
#include "ui/MainContentComponent.h"
#include "ui/StartupAnimation.h"
#include "ui/screens/FilterViewScreen.h"
#include "ui/GrammarHooks.h"
#include "ui/ScreenAudit.h"
#include "ui/TurnNotice.h"
#if __has_include("ui/screens/TrackFxScreen.h")
 #include "ui/screens/TrackFxScreen.h"
 #define NEPTUNE_HAS_TRACK_FX_SCREEN 1
#endif
#include "project/ProjectManager.h"
#include "sync/MIDIClock.h"
#include "sync/AbletonLink.h"

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow()
        : DocumentWindow("Neptune Groovebox",
                         juce::Colour(0xFF1A1A1A),
                         DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
    }

    void setContent(MainContentComponent* content)
    {
        setContentOwned(content, true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class ControllerLogger : public LaunchkeyController::Listener
{
public:
    void launchkeyConnected() override
    {
        juce::Logger::writeToLog("[LK] Connected");
    }

    void launchkeyDisconnected() override
    {
        juce::Logger::writeToLog("[LK] Disconnected");
    }

    void dawModeEntered() override
    {
        juce::Logger::writeToLog("[LK] DAW mode entered");
    }

    void dawModeExited() override
    {
        juce::Logger::writeToLog("[LK] DAW mode exited");
    }

    void noteOnReceived(int channel, int note, int velocity) override
    {
        juce::String label;
        if (note >= 96 && note < 104)
            label = " [TopPad " + juce::String(note - 96) + "]";
        else if (note >= 112 && note < 120)
            label = " [BotPad " + juce::String(note - 112) + "]";
        else
            label = " [Key " + juce::MidiMessage::getMidiNoteName(note, true, true, 3) + "]";

        juce::Logger::writeToLog("[LK] Note ON  ch=" + juce::String(channel)
                                 + " note=" + juce::String(note)
                                 + " vel=" + juce::String(velocity) + label);
    }

    void noteOffReceived(int channel, int note) override
    {
        juce::String label;
        if (note >= 96 && note < 104)
            label = " [TopPad " + juce::String(note - 96) + "]";
        else if (note >= 112 && note < 120)
            label = " [BotPad " + juce::String(note - 112) + "]";
        else
            label = " [Key " + juce::MidiMessage::getMidiNoteName(note, true, true, 3) + "]";

        juce::Logger::writeToLog("[LK] Note OFF ch=" + juce::String(channel)
                                 + " note=" + juce::String(note) + label);
    }

    void controlChangeReceived(int channel, int cc, int value) override
    {
        juce::String label;
        if (channel == 7 && cc == 63)
            label = value > 0 ? " [Shift DOWN]" : " [Shift UP]";
        else if (channel == 1 && cc == 104)
            label = " [> pattern key]";
        else if (channel == 1 && cc == 105)
            label = " [Func]";
        else if (channel == 1 && (cc == 106 || cc == 107))
            label = cc == 106 ? " [Up]" : " [Down]";
        else if (channel == 1 && (cc == 51 || cc == 52))
            label = cc == 51 ? " [Bank up]" : " [Bank down]";
        else if ((channel == 16 || channel == 1) && cc >= 0x15 && cc <= 0x1C)
            label = " [Encoder " + juce::String(cc - 0x15) + "]";
        else if (cc == 0x73)
            label = " [Play]";
        else if (cc == 0x75)
            label = " [Record]";
        else if (channel == 7 && cc == 0x49)
            label = " [Arp]";
        else if (channel == 7 && cc == 0x4A)
            label = " [Scale]";

        juce::Logger::writeToLog("[LK] CC ch=" + juce::String(channel)
                                 + " cc=" + juce::String(cc)
                                 + " val=" + juce::String(value) + label);
    }

    void aftertouchReceived(int channel, int note, int pressure) override
    {
        juce::Logger::writeToLog("[LK] AT ch=" + juce::String(channel)
                                 + " note=" + juce::String(note)
                                 + " pressure=" + juce::String(pressure));
    }

    void bitmapAckReceived() override {}     // about 13 a second while the screen changes: not logged

    void padModeChanged(int mode) override
    {
        juce::Logger::writeToLog("[LK] Pad mode changed to " + juce::String(mode));
    }

    void encoderModeChanged(int mode) override
    {
        juce::Logger::writeToLog("[LK] Encoder mode changed to " + juce::String(mode));
    }
};

class TransportController : public ScreenManager::TransportListener
{
public:
    SequencerCore* sequencer = nullptr;
    MIDIClock* midiClock = nullptr;
    AudioEngine* audioEngine = nullptr;

    void playPressed() override
    {
        if (!sequencer) return;
        if (sequencer->isRunning())     // playing, or counting in
        {
            sequencer->stop();
            if (midiClock) midiClock->stopTransport();
        }
        else
        {
            // With recording armed and a count-in set (SETTINGS -> CLICK -> CNT) the bars are
            // counted in first; the audio thread then starts the MIDI clock with the transport.
            sequencer->requestStart();
            if (midiClock && sequencer->isPlaying()) midiClock->startTransport();
        }
    }

    void stopPressed() override
    {
        if (sequencer) sequencer->stop();
        if (midiClock) midiClock->stopTransport();
        if (audioEngine) audioEngine->requestSilence();     // Shift + Play also cuts notes and effect tails
    }

    void recordPressed() override
    {
        if (sequencer)
            sequencer->setRecording(!sequencer->isRecording());
    }
};

// Pitch strip and mod strip go to the selected track's engine.
class PerformanceControls : public LaunchkeyController::Listener
{
public:
    AudioEngine* audioEngine = nullptr;
    SequencerCore* sequencer = nullptr;

    void pitchBendReceived(int value14) override
    {
        auto* e = engine();
        if (e != nullptr)
            e->pitchBend(juce::jlimit(-1.0f, 1.0f, (float) (value14 - 8192) / 8191.0f));
        // In the log: where a bend went, at its ends and whenever it has moved a quarter of the way.
        if (std::abs(value14 - loggedBend) >= 2048 || ((value14 == 8192 || value14 == 0 || value14 >= 16383) && value14 != loggedBend))
        {
            loggedBend = value14;
            juce::Logger::writeToLog("[Bend] " + juce::String(value14 - 8192) + " -> track " + juce::String(sequencer ? sequencer->getSelectedTrack() + 1 : 0)
                                     + " " + (e != nullptr ? e->getName() : juce::String("(no engine)")));
        }
    }
    int loggedBend = 8192;

    void controlChangeReceived(int channel, int cc, int value) override
    {
        // The mod strip is CC 1 on the keys channel (1); nothing on the DAW port uses that number.
        if (channel == 1 && cc == 1)
        {
            // A running arpeggiator may claim the strip for one of its variation macros (ARP > VARY > STRP).
            if (audioEngine && sequencer)
                if (auto* arp = audioEngine->getTrackArpeggiator(sequencer->getSelectedTrack()))
                    if (arp->isEnabled() && arp->applyStrip((float) value / 127.0f))
                        return;
            if (sequencer)
                strip.moved(engine(), sequencer->getSelectedTrack(), (float) value / 127.0f);
        }
    }

    // From the 30 Hz refresh: every track's engine has the value the strip last gave that track
    // (also after a preset / project load or an engine change rebuilt it).
    void followStrip()
    {
        if (audioEngine)
            strip.follow([this](int t) { return audioEngine->getEngine(t); });
    }
    ModStripFollower strip;

private:
    EngineBase* engine() const
    {
        return (audioEngine && sequencer) ? audioEngine->getEngine(sequencer->getSelectedTrack()) : nullptr;
    }
};

class KeyboardToEngine : public ScreenManager::KeyboardListener
{
public:
    AudioEngine* audioEngine = nullptr;
    SequencerCore* sequencer = nullptr;

    // Each track has its own arpeggiator. A key goes to the selected track (through its
    // arpeggiator when that is on) and its release goes wherever its press went, so changing
    // track with keys held never leaves a note hanging.
    void keyboardNoteOn(int note, int velocity) override
    {
        if (!audioEngine || !sequencer || note < 0 || note > 127)
            return;

        float vel = velocity / 127.0f;
        const int trackIdx = sequencer->getSelectedTrack();
        auto* arp = audioEngine->getTrackArpeggiator(trackIdx);
        const bool viaArp = arp != nullptr && arp->isEnabled();

        noteTrack[note] = (int8_t) trackIdx;
        noteViaArp[note] = viaArp;

        if (viaArp)
            arp->noteOn(note, vel);
        else if (auto* engine = audioEngine->getEngine(trackIdx))
            engine->noteOn(note, vel, -1);

        // With the arpeggiator on, what it plays is recorded (see the arpeggiators' record hook),
        // not the raw key presses.
        if (!viaArp && sequencer->isRecording() && sequencer->isPlaying())
            sequencer->recordNote(trackIdx, note, vel);
    }

    void keyboardNoteOff(int note) override
    {
        if (!audioEngine || !sequencer || note < 0 || note > 127)
            return;

        const int trackIdx = noteTrack[note] >= 0 ? (int) noteTrack[note] : sequencer->getSelectedTrack();
        noteTrack[note] = -1;

        if (noteViaArp[note])
        {
            if (auto* arp = audioEngine->getTrackArpeggiator(trackIdx))
                arp->noteOff(note);
        }
        else if (auto* engine = audioEngine->getEngine(trackIdx))
        {
            engine->noteOff(note, -1);
        }
    }

    KeyboardToEngine()
    {
        for (auto& t : noteTrack) t = -1;
        for (auto& a : noteViaArp) a = false;
    }

private:
    int8_t noteTrack[128];
    bool noteViaArp[128];
};

class BitmapAckForwarder : public LaunchkeyController::Listener
{
public:
    KeyboardSplit* keyboardSplit = nullptr;
    void sysExReceived(const juce::uint8* data, int size) override
    {
        // Debug replies arrive on the MIDI thread; the split acts on them on the message thread.
        if (keyboardSplit == nullptr || size != 10) return;
        std::vector<juce::uint8> copy(data, data + size);
        juce::MessageManager::callAsync([split = keyboardSplit, copy] { split->sysExReceived(copy.data(), (int) copy.size()); });
    }

public:
    DisplayRenderer* display = nullptr;
    ScreenManager* screenManager = nullptr;

    void bitmapAckReceived() override
    {
        if (display)
            display->onBitmapAck();
    }

    void noteOnReceived(int channel, int note, int velocity) override
    {
        if (screenManager)
            screenManager->noteOnReceived(channel, note, velocity);
    }

    void noteOffReceived(int channel, int note) override
    {
        if (screenManager)
            screenManager->noteOffReceived(channel, note);
    }

    void aftertouchReceived(int channel, int note, int pressure) override
    {
        if (screenManager)
            screenManager->aftertouchReceived(channel, note, pressure);
    }

    void controlChangeReceived(int channel, int cc, int value) override
    {
        if (screenManager)
            screenManager->controlChangeReceived(channel, cc, value);
    }
};

class NeptuneApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Neptune Groovebox"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override           { return false; }

    void initialise(const juce::String& /*commandLine*/) override
    {
        controller = std::make_unique<LaunchkeyController>();
        display = std::make_unique<DisplayRenderer>();

        mainWindow = std::make_unique<MainWindow>();
        mainContent = new MainContentComponent(*display);
        mainWindow->setContent(mainContent);

        auto* oledMirror = &mainContent->getOLEDMirror();
        oledMirror->onPadPress = [this](int padIndex, float velocity)
        {
            if (!screenManager) return;
            int note = (padIndex < 8)
                ? LaunchkeyController::PAD_TOP_ROW_START + padIndex
                : LaunchkeyController::PAD_BOTTOM_ROW_START + (padIndex - 8);
            screenManager->noteOnReceived(1, note, (int)(velocity * 127.0f));
        };
        oledMirror->onPadRelease = [this](int padIndex)
        {
            if (!screenManager) return;
            int note = (padIndex < 8)
                ? LaunchkeyController::PAD_TOP_ROW_START + padIndex
                : LaunchkeyController::PAD_BOTTOM_ROW_START + (padIndex - 8);
            screenManager->noteOffReceived(1, note);
        };
        oledMirror->onKeyAction = [this](MirrorKeyAction action, bool pressed)
        {
            if (!screenManager) return;
            int value = pressed ? 127 : 0;
            switch (action)
            {
                case MirrorKeyAction::Play:
                    screenManager->controlChangeReceived(1, 0x73, value);
                    break;
                case MirrorKeyAction::Record:
                    screenManager->controlChangeReceived(1, 0x75, value);
                    break;
                case MirrorKeyAction::ArrowLeft:
                    screenManager->controlChangeReceived(1, 102, value);  // Shift+Up = prev track
                    break;
                case MirrorKeyAction::ArrowRight:
                    screenManager->controlChangeReceived(1, 103, value);  // Shift+Down = next track
                    break;
                case MirrorKeyAction::Scale:
                    screenManager->controlChangeReceived(7, 0x4A, value);
                    break;
                case MirrorKeyAction::Arp:
                    screenManager->controlChangeReceived(7, 0x49, value);
                    break;
                case MirrorKeyAction::Shift:
                    screenManager->controlChangeReceived(7, 63, value);
                    break;
                case MirrorKeyAction::EncoderBankUp:
                    if (pressed) screenManager->controlChangeReceived(1, 0x33, 127);
                    break;
                case MirrorKeyAction::EncoderBankDown:
                    if (pressed) screenManager->controlChangeReceived(1, 0x34, 127);
                    break;
            }
        };

        auto& vc = mainContent->getVirtualController();
        vc.onNoteOn = [this](int channel, int note, int velocity)
        {
            if (screenManager)
                screenManager->noteOnReceived(channel, note, velocity);
        };
        vc.onNoteOff = [this](int channel, int note)
        {
            if (screenManager)
                screenManager->noteOffReceived(channel, note);
        };
        vc.onCC = [this](int channel, int cc, int value)
        {
            if (screenManager)
                screenManager->controlChangeReceived(channel, cc, value);
        };
        // The strips go where the hardware's go: to the selected track's engine.
        vc.onPitchBend = [this](int value14) { performanceControls.pitchBendReceived(value14); };
        vc.onModStrip = [this](int value) { performanceControls.controlChangeReceived(1, 1, value); };

        logMidiDevices();
        padManager = std::make_unique<PadManager>();
        screenManager = std::make_unique<ScreenManager>();
        TurnNotice::setSink([this](const juce::String& text) { screenManager->showStatus(text); });
        sequencer = std::make_unique<SequencerCore>();
        audioEngine = std::make_unique<AudioEngine>();
        audioEngine->setSequencer(sequencer.get());
        presetManager = std::make_unique<PresetManager>();
        presetManager->loadFromDirectory(userPresetDir());
        for (auto& slot : notePadSlot) slot = -1;

        // One arpeggiator per track, each playing (and recording into) its own track.
        for (int t = 0; t < AudioEngine::MAX_TRACKS; ++t)
        {
            arps.push_back(std::make_unique<Arpeggiator>());
            auto* arp = arps.back().get();
            arp->setRandomSeed(0x1234567u + (uint32_t) t * 7919u);
            arp->onNoteOn = [this, t](int pitch, float velocity)
            {
                if (auto* engine = audioEngine->getEngine(t))
                    engine->noteOn(pitch, velocity, -1);
            };
            arp->onNoteOff = [this, t](int pitch)
            {
                if (auto* engine = audioEngine->getEngine(t))
                    engine->noteOff(pitch, -1);
            };
            // What the arpeggiator plays is what gets recorded (each note, or the held chord once
            // per step: REC on the ARP screen's SETUP page).
            arp->onRecordNote = [this, t](int pitch, float velocity)
            {
                if (sequencer->isRecording() && sequencer->isPlaying())
                    sequencer->recordNote(t, pitch, velocity);
            };
            audioEngine->setTrackArpeggiator(t, arp);
        }
        scaleManager = std::make_unique<CustomScaleManager>();
        chordPads = std::make_unique<ChordPads>();
        songMode = std::make_unique<SongMode>();
        projectManager = std::make_unique<ProjectManager>();

        // Saving captures the whole live state; loading (or "new") rebuilds it.
        projectManager->onCapture = [this]() -> ProjectData
        {
            auto data = ProjectBridge::capture(projectLive(), projectManager->getProjectName());
            data.clockMode = clockModeSetting;
            // A preset being auditioned on PRESET is not the track's sound: save the sound it returns to.
            if (screenManager && presetScreenRef)
            {
                const juce::ScopedLock ui(screenManager->getLock());
                presetScreenRef->keepOriginalInSave(data);
            }
            return data;
        };

        projectManager->onRebuild = [this](const ProjectData& data)
        {
            ProjectBridge::apply(data, projectLive());
            applyClockMode(data.clockMode);
            if (midiClock) midiClock->setTempo(sequencer->getTempo());
            if (screenManager)
            {
                screenManager->setTrackCount(sequencer->getPattern().getTrackCount());
                screenManager->setSelectedTrack(sequencer->getSelectedTrack());
                screenManager->projectLoaded();     // > + Play reloads a pattern from this state
                screenManager->requestRedraw();
            }
            juce::Logger::writeToLog("[Project] Loaded: " + data.name);
        };

        projectManager->startAutosave();

        midiClock = std::make_unique<MIDIClock>();
        midiClock->setTempo(sequencer->getTempo());
        audioEngine->setMIDIClock(midiClock.get());

        abletonLink = std::make_unique<AbletonLink>();
        abletonLink->setTempoCallback([this](double newTempo)
        {
            if (sequencer)
                sequencer->setTempo(static_cast<float>(newTempo));
            if (midiClock)
                midiClock->setTempo(newTempo);
        });

        sequencer->getPattern().getTrack(1).engineType = EngineType::Drum;

        int trackCount = sequencer->getPattern().getTrackCount();
        for (int i = 0; i < trackCount; ++i)
        {
            auto& track = sequencer->getPattern().getTrack(i);
            audioEngine->addEngine(i, AudioEngine::createEngine(track.engineType));
        }

        auto* drumEng = dynamic_cast<DrumEngine*>(audioEngine->getEngine(1));
        drumPadScreen = std::make_unique<DrumPadScreen>(drumEng);
        // Engines can be swapped at any time, so the drum screen and pad colours look the drum
        // track up each time: the selected track if it is a drum track, else the first one.
        drumPadScreen->setEngineProvider([this]() { return currentDrumEngine(); });
        // Drum pads go through the pad ratchet (ROLL): a held pad can roll, and what the pads play
        // is recorded.
        {
            DrumPadScreen::PadHooks hooks;
            hooks.down = [this](int pad, float velocity)
            {
                int t = currentDrumTrack();
                if (t >= 0) audioEngine->padNoteOn(t, pad, pad, velocity);
            };
            hooks.pressure = [this](int pad, float pressure) { audioEngine->padPressure(pad, pressure); };
            hooks.up = [this](int pad) { audioEngine->padNoteOff(pad); };
            drumPadScreen->setPadHooks(std::move(hooks));
        }
        audioEngine->onPadHit = [this](int trackIdx, int note, float velocity)
        {
            if (sequencer->isRecording() && sequencer->isPlaying())
                sequencer->recordNote(trackIdx, note, velocity);
        };

        drumPadScreen->setParamSetter([this](int id, float value)
        {
            int t = currentDrumTrack();
            if (t < 0) return;
            sequencer->getPattern().getTrack(t).soundParams[id] = value;
            audioEngine->setTrackParameter(t, id, value);
        });

        sequencer->setTrigCallback([this](int trackIdx, const Step& step)
        {
            auto* engine = audioEngine->getEngine(trackIdx);
            if (!engine) return;

            // Locks from the previous trig return to the track's values; this step's are applied.
            audioEngine->applyStepLocks(trackIdx, step);

            // Song rows can transpose the pitched tracks (drum pads and samples stay put).
            int shift = isPitchedTrack(engine) ? songPlayer.transpose.load() : 0;
            if (trackIdx >= 0 && trackIdx < (int) trigTranspose.size())
                trigTranspose[(size_t) trackIdx] = shift;
            for (auto& note : step.notes)
                engine->noteOn(juce::jlimit(0, 127, note.pitch + shift), note.velocity, -1);
        });

        sequencer->setNoteOffCallback([this](int trackIdx, int note)
        {
            auto* engine = audioEngine->getEngine(trackIdx);
            int shift = (trackIdx >= 0 && trackIdx < (int) trigTranspose.size()) ? trigTranspose[(size_t) trackIdx] : 0;
            if (engine)
                engine->noteOff(juce::jlimit(0, 127, note + shift), -1);
        });

        // Recorded pad pressure follows its note, transposed like it.
        sequencer->setPressureCallback([this](int trackIdx, int note, float pressure)
        {
            int shift = (trackIdx >= 0 && trackIdx < (int) trigTranspose.size()) ? trigTranspose[(size_t) trackIdx] : 0;
            audioEngine->lanePressure(trackIdx, juce::jlimit(0, 127, note + shift), pressure);
        });

        songPlayer.song = songMode.get();
        songPlayer.sequencer = sequencer.get();
        sequencer->addListener(&songPlayer);

        ledFeedback = std::make_unique<PadLEDFeedback>();
        ledFeedback->setSequencer(sequencer.get());
        if (drumEng)
            ledFeedback->setDrumEngine(drumEng);

        keyboardToEngine.audioEngine = audioEngine.get();
        keyboardToEngine.sequencer = sequencer.get();
        screenManager->setKeyboardListener(&keyboardToEngine);

        transportController.sequencer = sequencer.get();
        transportController.midiClock = midiClock.get();
        transportController.audioEngine = audioEngine.get();
        screenManager->setTransportListener(&transportController);

        // Patterns, tracks, mutes, fill and tempo for the hold layers and buttons; arpeggiators and
        // track sounds through the engine hooks (the Arp button switches the selected track's arp).
        screenManager->setSequencer(sequencer.get());
        screenManager->setEngineHooks(GrammarHooks::make(*sequencer, *audioEngine, [this](float bpm)
        {
            if (midiClock) midiClock->setTempo(bpm);
        }));

        screenManager->setScaleToggleCallback([this](bool enabled)
        {
            // Scale lock: the arpeggiators' variations and mutations stay on the notes of the active
            // scale page (kept up to date by the refresh timer).
            scaleLocked = enabled;
            updateArpScales();
            juce::Logger::writeToLog("[Scale] Lock " + juce::String(enabled ? "enabled" : "disabled"));
        });
        // ... and the keys play the nearest note of that scale on pitched tracks (drum pads and
        // sample slices are chosen by key, so those tracks are left alone).
        screenManager->setScaleNameProvider([this] { return scaleManager ? GrammarHooks::scaleLabel(*scaleManager) : juce::String(); });
        screenManager->setKeySnap([this](int note)
        {
            if (!scaleManager || !isPitchedTrack(audioEngine->getEngine(sequencer->getSelectedTrack())))
                return note;
            return GrammarHooks::snapToScale(*scaleManager, note);
        });

        // Shift + pad = go to a screen (docs/launchkey-mk4-control-map.md, "CONTROL GRAMMAR"). Screen indices,
        // in the order they are added below:
        //   0 TRACKS  1 STEP  2 PARAM  3 DRUMS  4 PRESET  5 SETTINGS  6 FLTR  7 ARP  8 SCALE
        //   9 SONG  10 AT ROUTE  11 WAVE  12 TRK FX
        // Top row:    PARAM, TRACKS, TRK FX, STEP, PRESET, SONG, SETTINGS, AT ROUTE
        // Bottom row: STEP, DRUMS, ARP, SCALE, WAVE, FLTR, -, -   (unassigned pads do nothing)
        {
            // (Bottom pad 3 is printed "User Chord": the chord pads. ARP has its own button.)
            static constexpr int map[16] = { SCR_PARAM, SCR_TRACKS, SCR_TRACK_FX, SCR_STEP, SCR_PRESET, SCR_SONG, SCR_SETTINGS, SCR_AT_ROUTE,
                                             SCR_STEP, SCR_DRUMS, SCR_CHORD, SCR_SCALE, SCR_WAVE, SCR_FILTER, -1, -1 };
            for (int pad = 0; pad < 16; ++pad)
                screenManager->setShiftPadScreen(pad, map[pad]);
        }

        screenManager->setTrackCount(sequencer->getPattern().getTrackCount());
        screenManager->setSelectedTrack(sequencer->getSelectedTrack());
        screenManager->setTrackSelectionCallback([this](int trackIdx)
        {
            if (sequencer && trackIdx >= 0 && trackIdx < sequencer->getPattern().getTrackCount())
            {
                sequencer->setSelectedTrack(trackIdx);
                screenManager->requestRedraw();
                screenManager->redrawIfNeeded();
            }
        });

        deviceManager = std::make_unique<juce::AudioDeviceManager>();
        deviceManager->initialiseWithDefaultDevices(0, 2);
        {
            // Low latency: 64-sample blocks (1.3 ms at 48 kHz). Measured with `NeptuneTests --bench`
            // in the optimised build: the heaviest eight-track session's slowest block takes 0.37 ms.
            // The device may round to what it supports, so the result is logged; drop-outs are logged
            // every ten seconds if any occur.
            auto setup = deviceManager->getAudioDeviceSetup();
            setup.bufferSize = 64;
            deviceManager->setAudioDeviceSetup(setup, true);
            if (auto* device = deviceManager->getCurrentAudioDevice())
                juce::Logger::writeToLog("[Audio] " + device->getName() + ", " + juce::String(device->getCurrentSampleRate(), 0)
                                         + " Hz, block " + juce::String(device->getCurrentBufferSizeSamples())
                                         + ", output latency " + juce::String(device->getOutputLatencyInSamples()) + " samples");
        }
        deviceManager->addAudioCallback(audioEngine.get());

        display->setController(controller.get());
        padManager->setController(controller.get());
        screenManager->setDisplay(display.get());
        screenManager->setController(controller.get());

        ackForwarder.display = display.get();
        // Keyboard split (Shift + Oct -): reads the firmware's octave and rewrites the lower keys' notes.
        ackForwarder.keyboardSplit = &keyboardSplit;
        keyboardSplit.send = [this](const std::vector<juce::uint8>& body) { controller->sendMidiPortSysEx(body); };
        keyboardSplit.isShiftHeld = [this] { return screenManager->isShiftHeld(); };
        keyboardSplit.showMessage = [this](const juce::String& text) { screenManager->showStatus(text); };
        ackForwarder.screenManager = screenManager.get();

        controller->addListener(&logger);
        controller->addListener(&ackForwarder);

        performanceControls.audioEngine = audioEngine.get();
        performanceControls.sequencer = sequencer.get();
        controller->addListener(&performanceControls);


        {
            auto home = std::make_unique<HomeScreen>(*sequencer, audioEngine.get());
            home->setPadManager(padManager.get());
            home->setTrackSelectionCallback([this](int trackIdx)
            {
                sequencer->setSelectedTrack(trackIdx);
                screenManager->requestRedraw();
                screenManager->redrawIfNeeded();
            });
            screenManager->addScreen(std::move(home));
        }
        {
            auto stepView = std::make_unique<StepViewScreen>(*sequencer, *padManager, *ledFeedback);
            auto paramPages = std::make_unique<ParamPageScreen>(*sequencer, audioEngine.get());
            stepView->setLockPages(paramPages.get());
            stepView->setAudioEngine(audioEngine.get());
            stepView->showStatus = [this](const juce::String& text) { screenManager->showStatus(text); };
            screenManager->addScreen(std::move(stepView));
            screenManager->addScreen(std::move(paramPages));
        }
        screenManager->addScreen(std::move(drumPadScreen));
        {
            auto presetScreen = std::make_unique<PresetScreen>(*sequencer, audioEngine.get(), *presetManager);
            // The pads play the selected track so a preset can be heard before it is loaded.
            presetScreen->setNoteCallback([this](int note, int velocity) { notePadDown(note, velocity); });
            // ... on the notes of the scale chosen on SCALE (page and root), like SCALE's own pads.
            presetScreen->setArpProvider([this](int t) { return (t >= 0 && t < (int) arps.size()) ? arps[(size_t) t].get() : nullptr; });
            presetScreen->setPadScale([this](int pad) { return scaleManager ? scaleManager->getNoteForPad(pad) : -1; },
                                      [this] { return scaleManager ? scaleManager->getRootNote() : 0; });
            presetScreen->setNoteOffCallback([this](int note) { notePadUp(note); });
            presetScreen->setAftertouchCallback([this](int note, int pressure)
                {
                    if (auto* engine = audioEngine->getEngine(sequencer->getSelectedTrack()))
                        engine->aftertouch(note, (float) pressure / 127.0f);
                    notePadPressure(note, pressure);
                    if (auto* arp = currentArp(); arp != nullptr && arp->isEnabled())
                        arp->applyPressure((float) pressure / 127.0f);
                });
            presetScreen->onUserPresetsChanged = [this]
            {
                // The folder mirrors the user presets in memory: rewrite it whole so deletions stick.
                for (auto& f : userPresetDir().findChildFiles(juce::File::findFiles, false, "*.json"))
                    f.deleteFile();
                presetManager->saveUserPresetsToDirectory(userPresetDir());
            };
            // SV arrives on the MIDI thread; the name is asked for on the Mac, whose keyboard types.
            presetScreen->onPresetSaved = [this](const juce::String& savedName)
            {
                if (auditFolder().isEmpty())        // the screen audit turns SV too: no box for it
                    juce::MessageManager::callAsync([this, savedName] { askPresetName(savedName); });
            };
            presetScreen->setPresetLoadCallback([this](int trackIdx, EngineType newType)
            {
                juce::ignoreUnused(newType);
                screenManager->requestRedraw();
                screenManager->redrawIfNeeded();
                juce::Logger::writeToLog("[Preset] Loaded on track " + juce::String(trackIdx));
            });
            presetScreenRef = presetScreen.get();
            screenManager->addScreen(std::move(presetScreen));
        }
        {
            SettingsScreen::Hooks h;
            h.getTempo = [this] { return sequencer->getTempo(); };
            h.setTempo = [this](float bpm)
            {
                sequencer->setTempo(bpm);
                if (midiClock) midiClock->setTempo(bpm);
            };
            h.getSwing = [this] { return sequencer->getPattern().swing; };
            h.setSwing = [this](float v) { sequencer->setSwing(v); };
            h.getQuantize = [this] { return sequencer->getQuantizeAmount(); };
            h.setQuantize = [this](float v) { sequencer->setQuantizeAmount(v); };
            h.getClockMode = [this] { return clockModeSetting; };
            h.setClockMode = [this](int mode) { applyClockMode(mode); };
            h.getMasterLevel = [this] { return audioEngine->getMasterLevel(); };
            h.setMasterLevel = [this](float v) { audioEngine->setMasterLevel(v); };
            h.getPattern = [this] { return sequencer->getPendingPattern() >= 0 ? sequencer->getPendingPattern()
                                                                               : sequencer->getActivePatternIndex(); };
            h.setPattern = [this](int p)
            {
                sequencer->selectPattern(p, sequencer->isPlaying() ? SequencerCore::ChangeMode::NextLoop
                                                                   : SequencerCore::ChangeMode::Immediate);
            };
            // HELP: the Shift / Func help cards (off, after 0.35 s, at once); kept between sessions.
            h.getHelpMode = [this] { return (int) screenManager->getHelpMode(); };
            h.setHelpMode = [this](int mode)
            {
                screenManager->setHelpMode((ScreenManager::HelpMode) juce::jlimit(0, 2, mode));
                if (screenAudit == nullptr)         // (the audit turns this knob too: not the player's choice)
                    helpModeFile().replaceWithText(juce::String(mode));
            };
            // CLICK: the metronome. App-wide, kept between sessions like HELP; never in a project.
            h.getClickMode = [this] { return (int) audioEngine->getMetronome().getMode(); };
            h.setClickMode = [this](int mode)
            {
                audioEngine->getMetronome().setMode((Metronome::Mode) juce::jlimit(0, Metronome::NUM_MODES - 1, mode));
                saveMetronomeSettings();
            };
            h.getClickLevel = [this] { return audioEngine->getMetronome().getLevel(); };
            h.setClickLevel = [this](float v)
            {
                audioEngine->getMetronome().setLevel(v);
                if (screenAudit == nullptr)
                    audioEngine->getMetronome().audition();     // one click at the new level
                saveMetronomeSettings();
            };
            h.getCountIn = [this] { return sequencer->getCountInBars(); };
            h.setCountIn = [this](int bars)
            {
                sequencer->setCountInBars(bars);
                saveMetronomeSettings();
            };
            h.getMaster = [this](int i) { return audioEngine->getMasterParam(i); };
            h.setMaster = [this](int i, float v) { audioEngine->setMasterParam(i, v); };

            h.slotUsed = [this](int slot) { return projectSlotDir(slot).getChildFile("project.json").existsAsFile(); };
            h.currentSlot = [this] { return currentProjectSlot(); };
            h.loadSlot = [this](int slot)
            {
                const bool ok = projectManager->loadProject(projectSlotDir(slot));
                if (ok) rememberProjectSlot(slot);
                return ok;
            };
            h.saveToSlot = [this](int slot)
            {
                projectManager->getData().name = "P" + juce::String(slot + 1).paddedLeft('0', 2);
                const bool ok = projectManager->saveProjectAs(projectSlotDir(slot));
                if (ok) rememberProjectSlot(slot);
                return ok;
            };
            h.saveCurrent = [this] { return projectManager->saveProject(); };
            h.newProject = [this]
            {
                projectManager->newProject();
                // Same starting point as a freshly launched app: track 2 is the drum kit.
                audioEngine->setEngineType(1, EngineType::Drum);
                for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                    if (sequencer->getPattern(p).getTrackCount() > 1)
                        sequencer->getPattern(p).getTrack(1).engineType = EngineType::Drum;
            };
            h.getAutosave = [this] { return autosaveOn; };
            h.setAutosave = [this](bool on)
            {
                autosaveOn = on;
                if (on) projectManager->startAutosave();
                else    projectManager->stopAutosave();
            };
            auto settings = std::make_unique<SettingsScreen>(std::move(h));
            screenManager->addScreen(std::move(settings));
        }
        {
            auto filterView = std::make_unique<FilterViewScreen>();
            filterView->setAudio(sequencer.get(), audioEngine.get());
            screenManager->addScreen(std::move(filterView));
        }
        {
            auto arpScreen = std::make_unique<ArpScreen>(*arps[0]);
            arpScreen->setArpProvider([this] { return currentArp(); });
            arpScreen->setPadRatchet(&audioEngine->getPadRatchet());
            arpScreen->exportToPattern = [this] { return writeArpToPattern(); };
            screenManager->addScreen(std::move(arpScreen));
        }
        {
            // Custom scale pads play the selected track like the keys do, with per-note pressure.
            auto scaleScreen = std::make_unique<CustomScaleScreen>(*scaleManager);
            scaleScreen->setNoteCallback([this](int note, int velocity) { notePadDown(note, velocity); });
            scaleScreen->setNoteOffCallback([this](int note) { notePadUp(note); });
            scaleScreen->setAftertouchCallback([this](int note, int pressure)
            {
                if (auto* engine = audioEngine->getEngine(sequencer->getSelectedTrack()))
                    engine->aftertouch(note, (float) pressure / 127.0f);
                    notePadPressure(note, pressure);
                if (auto* arp = currentArp(); arp != nullptr && arp->isEnabled())
                    arp->applyPressure((float) pressure / 127.0f);
            });
            scaleScreen->setLockHooks([this] { return screenManager->isScaleLocked(); },
                                      [this](bool on) { screenManager->setScaleLocked(on); });
            screenManager->addScreen(std::move(scaleScreen));
        }
        screenManager->addScreen(std::make_unique<SongScreen>(*songMode, *sequencer));
        {
            // Pressure routes are per track: the screen edits the selected track's.
            auto atScreen = std::make_unique<AftertouchScreen>(*audioEngine, [this] { return sequencer->getSelectedTrack(); });
            atScreen->setNoteCallbacks(
                [this](int note, int velocity) { notePadDown(note, velocity); },
                [this](int note) { notePadUp(note); },
                [this](int note, int pressure)
                {
                    if (auto* engine = audioEngine->getEngine(sequencer->getSelectedTrack()))
                        engine->aftertouch(note, (float) pressure / 127.0f);
                    notePadPressure(note, pressure);
                    if (auto* arp = currentArp(); arp != nullptr && arp->isEnabled())
                        arp->applyPressure((float) pressure / 127.0f);
                });
            screenManager->addScreen(std::move(atScreen));
        }
        {
            // WAVE: the sample with its markers and playheads, and the resampling page.
            auto wave = std::make_unique<WaveformScreen>(*sequencer, audioEngine.get());
            waveformScreen = wave.get();
            screenManager->addScreen(std::move(wave));
        }

#ifdef NEPTUNE_HAS_TRACK_FX_SCREEN
        screenManager->addScreen(std::make_unique<TrackFxScreen>(*sequencer, audioEngine.get()));
#endif
        jassert(screenManager->getScreenCount() >= SCR_TRACK_FX);
        {
            // CHORD: a chord held on the keys goes onto a pad; the pads play the selected track
            // through the same path as the SCALE pads, one pad slot per note.
            auto chordScreen = std::make_unique<ChordPadScreen>(*chordPads);
            chordScreen->setHeldKeysProvider([this]
            {
                std::vector<ChordPads::Note> held;
                for (auto& [note, velocity] : screenManager->getHeldKeys())
                    held.push_back({ note, (float) velocity / 127.0f });
                // With the arp on, the keys it has latched count as held too.
                if (auto* arp = currentArp(); held.empty() && arp != nullptr && arp->isEnabled() && arp->getLatch())
                    for (int i = 0; i < arp->getInputNoteCount(); ++i)
                        held.push_back({ arp->getInputPitch(i), arp->getInputVelocity(i) });
                return held;
            });
            chordScreen->setNoteCallback([this](int note, int velocity) { notePadDown(note, velocity); });
            chordScreen->setNoteOffCallback([this](int note) { notePadUp(note); });
            chordScreen->setAftertouchCallback([this](int note, int pressure)
            {
                if (auto* engine = audioEngine->getEngine(sequencer->getSelectedTrack()))
                    engine->aftertouch(note, (float) pressure / 127.0f);
                notePadPressure(note, pressure);
            });
            screenManager->addScreen(std::move(chordScreen));
        }
        jassert(screenManager->getScreenCount() == SCR_CHORD + 1);

        // Screen names change with their page, so the buttons and the copy / clear / paste defaults
        // go by index. DRUMS and SCALE are the screens whose playing pads can be pinned.
        screenManager->setButtonScreens(SCR_SETTINGS, SCR_ARP, SCR_SCALE);
        screenManager->setStepScreen(SCR_STEP);     // where a recording is laid out when it ends
        screenManager->setPinnable(SCR_DRUMS, true);
        screenManager->setPinnable(SCR_SCALE, true);
        screenManager->setPinnable(SCR_CHORD, true);
        screenManager->setScreenContext(SCR_STEP, ScreenManager::CopyContext::Steps);
        screenManager->setScreenContext(SCR_TRACKS, ScreenManager::CopyContext::Pattern);
        screenManager->setScreenContext(SCR_PARAM, ScreenManager::CopyContext::Sound);
        screenManager->setScreenContext(SCR_FILTER, ScreenManager::CopyContext::Sound);
        screenManager->setScreenContext(SCR_TRACK_FX, ScreenManager::CopyContext::Fx);
        // The screens' names on the Shift help card's pad map (four letters, one pad column each).
        {
            static const char* const mapNames[] = { "TRKS", "STEP", "PARM", "DRUM", "PRST", "SETT", "FLTR", "ARP",
                                                    "SCAL", "SONG", "ATRT", "WAVE", "TKFX", "CHRD" };
            for (int i = 0; i <= SCR_CHORD; ++i)
                screenManager->setScreenMapName(i, mapNames[i]);
        }
        if (helpModeFile().existsAsFile())
            screenManager->setHelpMode((ScreenManager::HelpMode) juce::jlimit(0, 2, helpModeFile().loadFileAsString().trim().getIntValue()));
        if (metronomeFile().existsAsFile())
        {
            const auto m = Metronome::fromText(metronomeFile().loadFileAsString(), SequencerCore::MAX_COUNT_IN_BARS);
            audioEngine->getMetronome().setMode((Metronome::Mode) m.mode);
            audioEngine->getMetronome().setLevel(m.level);
            sequencer->setCountInBars(m.countInBars);
        }
        screenManager->projectLoaded();

        {
            auto midiOutputs = juce::MidiOutput::getAvailableDevices();
            for (auto& d : midiOutputs)
            {
                if (!d.name.containsIgnoreCase("Launchkey"))
                {
                    midiOutputDevice = juce::MidiOutput::openDevice(d.identifier);
                    if (midiOutputDevice)
                    {
                        audioEngine->setMIDIOutput(midiOutputDevice.get());
                        juce::Logger::writeToLog("[MIDI] Output opened: " + d.name);
                        break;
                    }
                }
            }
        }

        if (controller->scanAndConnect())
        {
            juce::Logger::writeToLog("Launchkey found, entering DAW mode...");
            controller->enterDawMode();
            controller->enableFeatureControls();
            controller->setPadMode(LaunchkeyController::PAD_MODE_DAW);
            controller->disableFirmwareArp();
            controller->disableFirmwareScale();
            controller->setEncoderMode(LaunchkeyController::ENCODER_MODE_PLUGIN);
            controller->configureNeptuneFirmware();

            screenManager->configureEncoderDisplays();

            padManager->setColorCallback([this](int padIndex, PadManager::Mode mode) -> int
            {
                // While recording, the step position also runs across pads that are not the step
                // pads (those have their playhead).
                if (mode != PadManager::Mode::Step && padIndex == screenManager->getRecordLightPad())
                    return PadColors::STEP_PLAYHEAD;
                return ledFeedback->getColor(padIndex, mode);
            });
            startupAnimation = std::make_unique<StartupAnimation>(*controller);
            startupAnimation->onComplete = [this]()
            {
                padManager->updatePadLEDs();
                juce::Logger::writeToLog("[LK] Startup animation complete, step LEDs active");
                // Keep pad LEDs in step with sequencer state (trigs, locks, playhead): diff-based, ~30 Hz.
                ledRefreshTimer = std::make_unique<FunctionTimer>([this]()
                {
                    // A refresh that comes late (the system napping a hidden app, a blocked message
                    // thread) is worth knowing about: the display and the pads wait for it.
                    {
                        const double nowTick = juce::Time::getMillisecondCounterHiRes();
                        if (lastRefreshTick > 0.0 && nowTick - lastRefreshTick > 300.0)
                            juce::Logger::writeToLog("[Refresh] the 30 Hz refresh was " + juce::String(nowTick - lastRefreshTick, 0) + " ms late");
                        lastRefreshTick = nowTick;
                    }
                    sequencer->serviceLanePool();      // buffers for recorded pad pressure, kept topped up
                    // Screen state is also changed by controller messages on the MIDI thread.
                    const juce::ScopedLock ui(screenManager->getLock());
                    ledFeedback->setDrumEngine(currentDrumEngine());
                    syncArpState();
                    performanceControls.followStrip();
                    if (waveformScreen) waveformScreen->poll();      // a finished resample becomes a sample
                    screenManager->tick();      // a held clear acts when its time is up; recording passes
                    audioEngine->motionPoll();  // knob movements being recorded follow the playhead
                    display->tickAckTimeout();
                    if (++splitTick >= 6) { splitTick = 0; keyboardSplit.poll(); }      // five times a second
                    if (++displayLogTick >= 300)                      // every 10 s, when frames were sent
                    {
                        displayLogTick = 0;
                        if (auto* device = deviceManager ? deviceManager->getCurrentAudioDevice() : nullptr)
                        {
                            const int xruns = device->getXRunCount();
                            const int skipped = audioEngine->getSkippedBlocks();
                            if ((xruns >= 0 && xruns != loggedXruns) || skipped != loggedSkipped)     // -1: not reported
                                juce::Logger::writeToLog("[Audio] drop-outs so far " + juce::String(xruns)
                                                         + ", blocks silenced by a rebuild " + juce::String(skipped));
                            loggedXruns = xruns; loggedSkipped = skipped;
                        }
                        if (display->getFramesSent() != displayLogFrames)
                        {
                            juce::Logger::writeToLog("[Display] " + juce::String(display->getFramesSent() - displayLogFrames)
                                                     + " frames in 10 s, mean time to acknowledge "
                                                     + juce::String(display->getMeanAckMs(), 1) + " ms, time-outs "
                                                     + juce::String(display->getTimeoutCount()));
                            displayLogFrames = display->getFramesSent();
                        }
                    }
                    refreshPads();
                    // Re-render so the playhead and other time-driven state reach the OLED; the
                    // renderer only sends a frame when the pixels actually changed.
                    screenManager->requestRedraw();
                    screenManager->redrawIfNeeded();
                });
                ledRefreshTimer->startTimerHz(30);
            };
            startupAnimation->start(3000, 20);

            // NEPTUNE_AUDIT_SCREENS=<folder>: turn every knob on every page and check the display
            // (ui/ScreenAudit.h), with the pictures really going to the device; then quit.
            if (auditFolder().isNotEmpty())
            {
                juce::Timer::callAfterDelay(4500, [this] { startScreenAudit(true); });
                return;
            }

            // NEPTUNE_DUMP_SCREENS=<folder>: write every screen and page as a PBM image, then quit
            // (documentation and review; nothing is loaded so the pictures show the defaults).
            if (auto dumpDir = juce::SystemStats::getEnvironmentVariable("NEPTUNE_DUMP_SCREENS", {}); dumpDir.isNotEmpty())
            {
                juce::Timer::callAfterDelay(4500, [this, dumpDir] { dumpScreens(juce::File(dumpDir)); quit(); });
                return;
            }

            // Pick up where the last session left off: reopen the project last saved or loaded.
            {
                const int last = lastProjectFile().loadFileAsString().trim().getIntValue() - 1;
                if (last >= 0 && last < SettingsScreen::NUM_SLOTS
                    && projectSlotDir(last).getChildFile("project.json").existsAsFile())
                    projectManager->loadProject(projectSlotDir(last));
            }
            restoreSession();

            screenManager->requestRedraw();
            screenManager->redrawIfNeeded();
        }
        else
        {
            juce::Logger::writeToLog("Launchkey not found. Connect it and restart.");
            // No hardware pads to light: the panel in the window still shows what they would show.
            panelPadTimer = std::make_unique<FunctionTimer>([this] { mirrorPadsToPanel(); });
            panelPadTimer->startTimerHz(30);
            if (auditFolder().isNotEmpty())
                juce::Timer::callAfterDelay(500, [this] { startScreenAudit(false); });     // drawing checks only
            else if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_DUMP_SCREENS", {}).isEmpty()
                     && juce::SystemStats::getEnvironmentVariable("NEPTUNE_SNAPSHOT_CONTROLLER", {}).isEmpty())
                restoreSession();
        }
    }

    void shutdown() override
    {
        saveSession();                       // the next start begins where this one ends
        sessionTimer.reset();
        keyboardSplit.restore();             // the keys go back to the firmware's own notes
        ledRefreshTimer.reset();
        panelPadTimer.reset();
        startupAnimation.reset();
        if (controller)
        {
            controller->removeListener(&ackForwarder);
            controller->removeListener(&logger);
        }

        if (deviceManager)
            deviceManager->removeAudioCallback(audioEngine.get());
        deviceManager.reset();
        midiOutputDevice.reset();
        abletonLink.reset();
        midiClock.reset();
        TurnNotice::setSink({});
        screenManager.reset();
        ledFeedback.reset();
        presetManager.reset();
        if (audioEngine)
            for (int t = 0; t < AudioEngine::MAX_TRACKS; ++t)
                audioEngine->setTrackArpeggiator(t, nullptr);
        arps.clear();
        scaleManager.reset();
        audioEngine.reset();
        sequencer.reset();
        padManager.reset();
        display.reset();
        controller.reset();
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<LaunchkeyController> controller;
    std::unique_ptr<DisplayRenderer> display;
    std::unique_ptr<PadManager> padManager;
    std::unique_ptr<ScreenManager> screenManager;
    std::unique_ptr<SequencerCore> sequencer;
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<PadLEDFeedback> ledFeedback;
    std::unique_ptr<PresetManager> presetManager;
    WaveformScreen* waveformScreen = nullptr;
    int displayLogTick = 0, displayLogFrames = 0, loggedXruns = 0, loggedSkipped = 0;
    KeyboardSplit keyboardSplit;
    int splitTick = 0;
    std::unique_ptr<DrumPadScreen> drumPadScreen;
    PresetScreen* presetScreenRef = nullptr;     // owned by screenManager

    // The preset is already saved as `savedName`; this offers to rename it. Return renames,
    // Escape keeps the name. The controller goes on working while the box is open.
    void askPresetName(const juce::String& savedName)
    {
        if (mainWindow == nullptr)
            return;
        auto* box = new juce::AlertWindow("Name the preset",
                                          "Saved as \"" + savedName + "\". Type a name (up to "
                                              + juce::String(PresetManager::MAX_NAME_LENGTH) + " characters) and press Return.",
                                          juce::MessageBoxIconType::NoIcon, mainWindow.get());
        box->addTextEditor("name", savedName);
        if (auto* editor = box->getTextEditor("name"))
        {
            editor->setInputRestrictions(PresetManager::MAX_NAME_LENGTH);
            editor->selectAll();
        }
        box->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        box->addButton("Keep " + savedName, 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<juce::AlertWindow> safeBox(box);
        box->enterModalState(true, juce::ModalCallbackFunction::create([this, safeBox, savedName](int result)
        {
            if (result != 1 || safeBox == nullptr || screenManager == nullptr || presetScreenRef == nullptr)
                return;
            const auto typed = safeBox->getTextEditorContents("name");
            const auto name = PresetManager::cleanName(typed);
            if (name.isEmpty() || name == savedName)
                return;
            const juce::ScopedLock ui(screenManager->getLock());
            const bool ok = presetScreenRef->renameUserPreset(savedName, typed);
            screenManager->showStatus(ok ? "NAMED" : "NAME IN USE");
            juce::Logger::writeToLog("[Preset] " + savedName + (ok ? " renamed to " : " not renamed to ") + typed);
        }), true);
        juce::Process::makeForegroundProcess();
        box->toFront(true);
    }
    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    std::vector<std::unique_ptr<Arpeggiator>> arps;

    Arpeggiator* currentArp() const
    {
        int t = sequencer ? sequencer->getSelectedTrack() : 0;
        return (t >= 0 && t < (int) arps.size()) ? arps[(size_t) t].get() : nullptr;
    }
    std::unique_ptr<CustomScaleManager> scaleManager;
    std::unique_ptr<ChordPads> chordPads;
    std::unique_ptr<SongMode> songMode;
    std::unique_ptr<ProjectManager> projectManager;
    std::unique_ptr<MIDIClock> midiClock;
    std::unique_ptr<AbletonLink> abletonLink;
    std::unique_ptr<juce::MidiOutput> midiOutputDevice;
    MainContentComponent* mainContent = nullptr;
    std::unique_ptr<StartupAnimation> startupAnimation;

    struct FunctionTimer : public juce::Timer
    {
        explicit FunctionTimer(std::function<void()> f) : fn(std::move(f)) {}
        ~FunctionTimer() override { stopTimer(); }
        void timerCallback() override { if (fn) fn(); }
        std::function<void()> fn;
    };
    std::unique_ptr<FunctionTimer> ledRefreshTimer;
    std::unique_ptr<FunctionTimer> sessionTimer;
    std::unique_ptr<FunctionTimer> panelPadTimer;      // only without a controller (see mirrorPadsToPanel)

    // Screen indices (the order of addScreen in initialise).
    enum ScreenIndex
    {
        SCR_TRACKS = 0, SCR_STEP, SCR_PARAM, SCR_DRUMS, SCR_PRESET, SCR_SETTINGS, SCR_FILTER, SCR_ARP,
        SCR_SCALE, SCR_SONG, SCR_AT_ROUTE, SCR_WAVE, SCR_TRACK_FX, SCR_CHORD
    };

    // The pads of the panel in the Mac window take the colours the hardware's pads get (only a change repaints).
    // Called with or without a controller; takes the screens' lock itself.
    void mirrorPadsToPanel()
    {
        if (mainContent == nullptr || ! screenManager || ! padManager || ! ledFeedback)
            return;
        auto& panel = mainContent->getVirtualController();
        const juce::ScopedLock ui(screenManager->getLock());
        std::array<Screen::PadColour, 16> frame;
        if (screenManager->getPadFrame(frame) == ScreenManager::PadSource::PadMode)
        {
            ledFeedback->setDrumEngine(currentDrumEngine());
            const auto mode = padManager->getMode();
            for (int i = 0; i < 16; ++i)
            {
                const int palette = (mode != PadManager::Mode::Step && i == screenManager->getRecordLightPad())
                                        ? PadColors::STEP_PLAYHEAD : ledFeedback->getColor(i, mode);
                const auto rgb = PadColors::approximateRGB(palette);
                panel.setPadColor(i, rgb.r, rgb.g, rgb.b);
            }
            return;
        }
        for (int i = 0; i < 16; ++i)       // full-colour frames are 0..127 per channel
            panel.setPadColor(i, frame[(size_t) i].r * 2, frame[(size_t) i].g * 2, frame[(size_t) i].b * 2);
    }

    // Pad colours, 30 times a second, only what changed: a hold layer (Shift, >, Up / Down, Func),
    // else the pinned screen, else the active screen's own colours, else the pad manager's mode.
    void refreshPads()
    {
        mirrorPadsToPanel();
        std::array<Screen::PadColour, 16> frame;
        if (screenManager->getPadFrame(frame) == ScreenManager::PadSource::PadMode)
        {
            padManager->refreshPadLEDs();
            return;
        }
        std::array<PadManager::RGB, PadManager::TOTAL_PADS> rgb;
        for (size_t i = 0; i < rgb.size(); ++i)
            rgb[i] = { frame[i].r, frame[i].g, frame[i].b };
        padManager->showRGB(rgb);
    }
    ControllerLogger logger;
    BitmapAckForwarder ackForwarder;
    KeyboardToEngine keyboardToEngine;
    TransportController transportController;
    PerformanceControls performanceControls;
    SongPlayer songPlayer;
    int clockModeSetting = 0;      // 0 internal, 1 external, 2 auto, 3 link
    bool autosaveOn = true;

    ProjectBridge::Live projectLive()
    {
        return { *sequencer, *audioEngine, songMode.get(), scaleManager.get(), nullptr,
                 [this](int t) { return (t >= 0 && t < (int) arps.size()) ? arps[(size_t) t].get() : nullptr; },
                 chordPads.get() };
    }

    // Projects are numbered slots: ~/Music/Neptune/Projects/P01 ... P32.
    static juce::File projectSlotDir(int slot)
    {
        return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile("Neptune").getChildFile("Projects")
            .getChildFile("P" + juce::String(slot + 1).paddedLeft('0', 2));
    }

    static juce::String auditFolder() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_AUDIT_SCREENS", {}); }

    // The screen audit runs on a thread of its own and sends the same messages the controller
    // does. Nothing is loaded before it and autosave is off, so no project is touched.
    // NEPTUNE_AUDIT_PHOTOS=1 makes it wait on every page for tools/screen_audit.py to photograph
    // the device.
    void startScreenAudit(bool connected)
    {
        projectManager->stopAutosave();
        autosaveOn = false;
        sequencer->setSelectedTrack(0);
        screenManager->setHelpMode(ScreenManager::HelpMode::Delayed);      // the audit checks the cards as shipped

        screenAudit = std::make_unique<ScreenAudit>(*screenManager, *display);
        auto& a = *screenAudit;
        a.deviceConnected = connected;
        a.photoHandshake = connected && juce::SystemStats::getEnvironmentVariable("NEPTUNE_AUDIT_PHOTOS", {}).isNotEmpty();
        a.log = [](const juce::String& text) { juce::Logger::writeToLog("[Audit] " + text); };
        a.engineNames = { "synth", "FM", "macro", "wavetable", "wave seq", "sampler", "drums", "MIDI" };
        a.setEngine = [this](int e)
        {
            const int track = sequencer->getSelectedTrack();
            audioEngine->setEngineType(track, (EngineType) e, true);
            sequencer->getPattern().getTrack(track).engineType = (EngineType) e;
        };
        //            screen        name        every engine   also with a step held
        a.targets = { { SCR_TRACKS,   "TRACKS",   false, false }, { SCR_STEP,     "STEP",     false, true },
                      { SCR_PARAM,    "PARAM",    true,  true },  { SCR_TRACK_FX, "TRK FX",   false, true },
                      { SCR_FILTER,   "FLTR",     true,  true },  { SCR_DRUMS,    "DRUMS",    false, false },
                      { SCR_PRESET,   "PRESET",   false, false }, { SCR_SETTINGS, "SETTINGS", false, false },
                      { SCR_ARP,      "ARP",      false, false }, { SCR_SCALE,    "SCALE",    false, false },
                      { SCR_SONG,     "SONG",     false, false }, { SCR_AT_ROUTE, "AT ROUTE", true,  false },
                      { SCR_WAVE,     "WAVE",     true,  false }, { SCR_CHORD,    "CHORD",    false, false } };

        juce::Thread::launch([this]
        {
            const auto totals = screenAudit->run(juce::File(auditFolder()));
            juce::Logger::writeToLog("[Audit] finished: " + juce::String(totals.faults()) + " findings to look at");
            juce::MessageManager::callAsync([] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
        });
    }
    std::unique_ptr<ScreenAudit> screenAudit;
    double lastRefreshTick = 0.0;

    void dumpScreens(const juce::File& dir)
    {
        dir.createDirectory();
        int serial = 0;
        auto shot = [&](const juce::String& label)
        {
            screenManager->requestRedraw();
            screenManager->redrawIfNeeded();
            juce::String pbm = "P1\n128 64\n";
            const auto& px = display->getPixels();
            for (int y = 0; y < 64; ++y)
            {
                for (int x = 0; x < 128; ++x)
                    pbm << (px[(size_t) y][(size_t) x] ? "1 " : "0 ");
                pbm << "\n";
            }
            dir.getChildFile(juce::String(serial++).paddedLeft('0', 3) + " " + label + ".pbm").replaceWithText(pbm);
        };
        auto pages = [&](const juce::String& prefix)
        {
            auto* scr = screenManager->getActiveScreen();
            if (scr == nullptr) return;
            const int n = juce::jmax(1, scr->getPageCount());
            for (int i = 0; i < n && scr->getPageIndex() != 0; ++i)      // back to the first page
                scr->onArrowLeft();
            for (int i = 0; i < n; ++i)
            {
                shot(prefix + " " + juce::String(i + 1) + " of " + juce::String(n));
                scr->onArrowRight();
            }
        };
        static const char* const screenNames[] = { "TRACKS", "STEP", "PARAM", "DRUMS", "PRESET", "SETTINGS", "FLTR", "ARP", "SCALE", "SONG", "AT ROUTE", "WAVE" };
        static const char* const engineNames[] = { "synth", "FM", "macro", "wavetable", "wave seq", "sampler", "drums", "MIDI" };
        for (int i = 0; i < 12; ++i)
        {
            if (i == 2) continue;
            screenManager->setActiveScreen(i);
            pages(screenNames[i]);
        }
        if (screenManager->getScreenCount() > SCR_TRACK_FX)
        {
            screenManager->setActiveScreen(SCR_TRACK_FX);
            pages("TRK FX");
        }
        if (screenManager->getScreenCount() > SCR_CHORD)
        {
            // (With a progression on the pads, so the picture shows what the screen is for.)
            const int progression[8][4] = { { 48, 55, 64, 72 }, { 45, 52, 60, 64 }, { 41, 53, 57, 60 }, { 43, 50, 59, 65 },
                                            { 50, 53, 57, 60 }, { 52, 55, 59, 62 }, { 52, 55, 60, 67 }, { 47, 50, 53, 59 } };
            for (int pad = 0; pad < 8; ++pad)
                chordPads->store(0, pad, { { progression[pad][0], 1.0f }, { progression[pad][1], 0.8f },
                                           { progression[pad][2], 0.8f }, { progression[pad][3], 0.8f } });
            screenManager->setActiveScreen(SCR_CHORD);
            pages("CHORD");
            chordPads->clearBank(0);
        }
        const int track = sequencer->getSelectedTrack();
        for (int e = 0; e < 8; ++e)
        {
            audioEngine->setEngineType(track, (EngineType) e, true);
            sequencer->getPattern().getTrack(track).engineType = (EngineType) e;
            screenManager->setActiveScreen(0);
            screenManager->setActiveScreen(2);
            pages(juce::String("PARAM ") + engineNames[e]);
        }
    }

    static juce::File userPresetDir()
    {
        return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile("Neptune").getChildFile("Presets");
    }

    static juce::File lastProjectFile()
    {
        return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile("Neptune").getChildFile("Projects").getChildFile("last.txt");
    }
    // ---- The session: everything as it was when the app was last running -------------------------
    // Every sound, pattern and setting a project holds, written when the app quits and every 15 s
    // while it runs (an app that is killed or crashes has no quit), and put back at the next start
    // on top of the project last saved or loaded - which stays the project that SAVE writes to.
    // Never read or written by the screen audit, the screen dump or the panel snapshot.
    static juce::File sessionFile() { return lastProjectFile().getSiblingFile("session.json"); }

    void restoreSession()
    {
        const auto json = sessionFile().loadFileAsString();
        if (json.isNotEmpty() && projectManager && projectManager->onRebuild)
        {
            const auto data = ProjectData::fromJSON(json);
            if (!data.patterns.empty() && !data.patterns[0].tracks.empty())
            {
                projectManager->getData() = data;
                projectManager->onRebuild(data);
                lastSessionJson = json;
                juce::Logger::writeToLog("[Session] restored the state the app was last quit in");
            }
        }
        sessionOn = true;
        sessionTimer = std::make_unique<FunctionTimer>([this] { saveSession(); });
        sessionTimer->startTimer(15000);
    }

    void saveSession()
    {
        if (!sessionOn || !projectManager || !projectManager->onCapture || !sequencer || !audioEngine)
            return;
        const auto json = projectManager->onCapture().toJSON();
        if (json == lastSessionJson)
            return;
        const auto file = sessionFile();
        file.getParentDirectory().createDirectory();
        if (file.replaceWithText(json))
            lastSessionJson = json;
    }

    bool sessionOn = false;
    juce::String lastSessionJson;

    static void rememberProjectSlot(int slot) { lastProjectFile().replaceWithText(juce::String(slot + 1)); }
    // SETTINGS -> SETUP -> HELP (0 off, 1 after 0.35 s, 2 at once): a preference of the player, not of a project.
    static juce::File helpModeFile() { return lastProjectFile().getSiblingFile("help.txt"); }
    // The metronome's settings (CLICK page): one line, see Metronome::toText.
    static juce::File metronomeFile() { return lastProjectFile().getSiblingFile("metronome.txt"); }
    void saveMetronomeSettings()
    {
        if (screenAudit != nullptr)         // (the audit turns these knobs too: not the player's choice)
            return;
        Metronome::Settings m;
        m.mode = (int) audioEngine->getMetronome().getMode();
        m.level = audioEngine->getMetronome().getLevel();
        m.countInBars = sequencer->getCountInBars();
        metronomeFile().replaceWithText(Metronome::toText(m));
    }

    int currentProjectSlot() const
    {
        if (!projectManager || !projectManager->hasFile())
            return -1;
        auto name = projectManager->getCurrentFile().getFileName();
        if (name.length() == 3 && name.startsWithChar('P'))
            return juce::jlimit(1, SettingsScreen::NUM_SLOTS, name.substring(1).getIntValue()) - 1;
        return -1;
    }

    void applyClockMode(int mode)
    {
        clockModeSetting = juce::jlimit(0, 3, mode);
        if (!midiClock) return;
        switch (clockModeSetting)
        {
            case 1: midiClock->setMode(MIDIClock::Mode::External); break;
            case 2: midiClock->setMode(MIDIClock::Mode::Auto); break;
            case 3:
                if (abletonLink) abletonLink->enable();
                midiClock->setMode(MIDIClock::Mode::Internal);
                break;
            default: midiClock->setMode(MIDIClock::Mode::Internal); break;
        }
    }
    std::array<int, AudioEngine::MAX_TRACKS> trigTranspose {};

    static bool isPitchedTrack(EngineBase* e)
    {
        return e != nullptr && e->getType() != EngineType::Drum && e->getType() != EngineType::Sampler;
    }

    // ---- Pads that play notes (scale pads, the pressure-routing screen's pads) -------------------
    // If the track's arpeggiator is on they feed it, like the keys. Otherwise they go through the
    // pad ratchet, each held note taking one of its sixteen slots.
    int8_t notePadSlot[128];
    bool padSlotBusy[PadRatchet::kNumPads] = {};
    bool notePadViaKeys[128] = {};

    void notePadDown(int note, int velocity)
    {
        if (note < 0 || note > 127) return;
        auto* arp = currentArp();
        if (arp != nullptr && arp->isEnabled())
        {
            notePadViaKeys[note] = true;
            keyboardToEngine.keyboardNoteOn(note, velocity);
            return;
        }
        notePadViaKeys[note] = false;
        for (int slot = 0; slot < PadRatchet::kNumPads; ++slot)
        {
            if (!padSlotBusy[slot])
            {
                padSlotBusy[slot] = true;
                notePadSlot[note] = (int8_t) slot;
                audioEngine->padNoteOn(sequencer->getSelectedTrack(), slot, note, (float) velocity / 127.0f);
                return;
            }
        }
    }

    void notePadPressure(int note, int pressure)
    {
        if (note >= 0 && note <= 127 && !notePadViaKeys[note] && notePadSlot[note] >= 0)
            audioEngine->padPressure(notePadSlot[note], (float) pressure / 127.0f);
    }

    void notePadUp(int note)
    {
        if (note < 0 || note > 127) return;
        if (notePadViaKeys[note])
        {
            notePadViaKeys[note] = false;
            keyboardToEngine.keyboardNoteOff(note);
            return;
        }
        if (notePadSlot[note] >= 0)
        {
            audioEngine->padNoteOff(notePadSlot[note]);
            padSlotBusy[notePadSlot[note]] = false;
            notePadSlot[note] = -1;
        }
    }

    bool scaleLocked = false;
    bool arpsRecording = false;

    // The pitch classes of the active scale page, relative to its root, or everything when the
    // scale lock is off.
    void updateArpScales()
    {
        int root = 0;
        uint16_t mask = Arpeggiator::SCALE_CHROMATIC;
        if (scaleLocked && scaleManager)
        {
            root = ((scaleManager->getRootNote() % 12) + 12) % 12;
            mask = 0;
            for (int pad = 0; pad < 16; ++pad)
                mask = (uint16_t) (mask | (1u << ((((scaleManager->getNoteForPad(pad) - root) % 12) + 12) % 12)));
        }
        for (auto& arp : arps)
            if (arp->getScaleRoot() != root || arp->getScaleMask() != mask)
                arp->setScale(root, mask);
    }

    // From the 30 Hz timer: arpeggiators record while the sequencer is recording and running, and
    // follow the scale page.
    void syncArpState()
    {
        const bool rec = sequencer->isRecording() && sequencer->isPlaying();
        if (rec != arpsRecording)
        {
            arpsRecording = rec;
            for (auto& arp : arps)
                arp->setRecording(rec);
        }
        if (scaleLocked)
            updateArpScales();
    }

    // >PAT on the ARP screen: the selected track's arpeggio (its frozen loop if frozen) becomes
    // steps of that track's pattern, at the arpeggiator's rate. Existing steps are replaced.
    bool writeArpToPattern()
    {
        auto* arp = currentArp();
        if (arp == nullptr) return false;
        Arpeggiator::ExportedStep exported[64];
        const int count = arp->exportSteps(exported, 64);
        if (count <= 0) return false;

        // Pattern steps are sixteenths; an arp step is getStepLengthBeats() quarter notes.
        const double stepsPerArpStep = arp->getStepLengthBeats() * 4.0;
        auto& track = sequencer->getPattern().getTrack(sequencer->getSelectedTrack());
        const auto& last = exported[count - 1];
        const int length = juce::jlimit(1, 64, (int) std::ceil((last.startSteps + last.lengthSteps) * stepsPerArpStep - 0.001));
        track.setLength(length);
        track.clearAllSteps();

        bool wroteAny = false;
        for (int i = 0; i < count; ++i)
        {
            const auto& e = exported[i];
            if (e.rest || e.noteCount <= 0) continue;
            const double position = e.startSteps * stepsPerArpStep;
            const int index = juce::jlimit(0, length - 1, (int) std::floor(position + 0.001));
            auto& step = track.getStep(index);
            step.trig = true;
            step.notes.clear();
            for (int n = 0; n < e.noteCount; ++n)
                step.notes.push_back({ e.pitches[n], e.velocities[n] });
            step.microTiming = juce::jlimit(-0.5f, 0.5f, (float) (position - (double) index));
            step.length = juce::jmax(0.1f, (float) (e.gate * e.lengthSteps * stepsPerArpStep));
            wroteAny = true;
        }
        return wroteAny;
    }

    int currentDrumTrack() const
    {
        if (!audioEngine || !sequencer) return -1;
        int selected = sequencer->getSelectedTrack();
        if (dynamic_cast<DrumEngine*>(audioEngine->getEngine(selected)) != nullptr)
            return selected;
        for (int t = 0; t < sequencer->getPattern().getTrackCount(); ++t)
            if (dynamic_cast<DrumEngine*>(audioEngine->getEngine(t)) != nullptr)
                return t;
        return -1;
    }

    DrumEngine* currentDrumEngine() const
    {
        int t = currentDrumTrack();
        return t >= 0 ? dynamic_cast<DrumEngine*>(audioEngine->getEngine(t)) : nullptr;
    }

    void logMidiDevices()
    {
        auto midiInputs = juce::MidiInput::getAvailableDevices();
        auto midiOutputs = juce::MidiOutput::getAvailableDevices();

        juce::Logger::writeToLog("=== MIDI Input Devices ===");
        for (auto& d : midiInputs)
            juce::Logger::writeToLog("  [IN]  " + d.name + " (" + d.identifier + ")");

        juce::Logger::writeToLog("=== MIDI Output Devices ===");
        for (auto& d : midiOutputs)
            juce::Logger::writeToLog("  [OUT] " + d.name + " (" + d.identifier + ")");

        if (midiInputs.isEmpty() && midiOutputs.isEmpty())
            juce::Logger::writeToLog("No MIDI devices found.");
    }
};

START_JUCE_APPLICATION(NeptuneApplication)
