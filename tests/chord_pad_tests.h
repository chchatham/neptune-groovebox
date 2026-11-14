#pragma once
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>
#include "../src/controller/ChordPads.h"
#include "../src/ui/screens/ChordPadScreen.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/project/ProjectBridge.h"

// CHORD: a chord held on the keys is stored on a pad and played from it.
class ChordPadTests : public juce::UnitTest
{
public:
    ChordPadTests() : UnitTest("Chord Pads") {}

    // The screen inside a ScreenManager, driven by controller messages; TRACKS-like screen 0, CHORD 1.
    struct Plain : Screen
    {
        juce::String getName() const override { return "OTHER"; }
        void render(DisplayRenderer&) override {}
    };
    struct Keys : ScreenManager::KeyboardListener
    {
        juce::StringArray log;
        void keyboardNoteOn(int note, int) override { log.add("on" + juce::String(note)); }
        void keyboardNoteOff(int note) override { log.add("off" + juce::String(note)); }
    };

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        ScreenManager mgr;
        ChordPads chords;
        ChordPadScreen* screen = nullptr;
        Keys keys;
        juce::StringArray played;       // "on60:100", "off60", "at60:50"
        juce::int64 now = 1000;

        Rig()
        {
            seq.prepareToPlay(48000.0, 64);
            audio.setSequencer(&seq);
            mgr.setSequencer(&seq);
            mgr.setClock([this] { return now; });
            mgr.setHelpBusyWindow(0);
            mgr.setEngineHooks(GrammarHooks::make(seq, audio));
            mgr.setKeyboardListener(&keys);
            mgr.addScreen(std::make_unique<Plain>());
            auto s = std::make_unique<ChordPadScreen>(chords);
            screen = s.get();
            screen->setHeldKeysProvider([this]
            {
                std::vector<ChordPads::Note> held;
                for (auto& [note, velocity] : mgr.getHeldKeys())
                    held.push_back({ note, (float) velocity / 127.0f });
                return held;
            });
            screen->setNoteCallback([this](int n, int v) { played.add("on" + juce::String(n) + ":" + juce::String(v)); });
            screen->setNoteOffCallback([this](int n) { played.add("off" + juce::String(n)); });
            screen->setAftertouchCallback([this](int n, int p) { played.add("at" + juce::String(n) + ":" + juce::String(p)); });
            mgr.addScreen(std::move(s));
            mgr.setShiftPadScreen(10, 1);           // Shift + bottom pad 3
            mgr.setPinnable(1, true);
            mgr.projectLoaded();
            mgr.setActiveScreen(1);
        }

        static int padNote(int pad) { return pad < 8 ? LaunchkeyController::PAD_TOP_ROW_START + pad
                                                     : LaunchkeyController::PAD_BOTTOM_ROW_START + pad - 8; }
        void keyOn(int note, int velocity = 100) { mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, note, velocity); }
        void keyOff(int note) { mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, note); }
        void padOn(int pad, int velocity = 100) { mgr.noteOnReceived(1, padNote(pad), velocity); }
        void padOff(int pad) { mgr.noteOffReceived(1, padNote(pad)); }
        void padPressure(int pad, int value) { mgr.aftertouchReceived(1, padNote(pad), value); }
        void shift(bool on) { mgr.controlChangeReceived(7, 63, on ? 127 : 0); }
        void func(bool on)  { mgr.controlChangeReceived(1, 105, on ? 127 : 0); }
        void tap(int cc)    { mgr.controlChangeReceived(1, cc, 127); mgr.controlChangeReceived(1, cc, 0); }
        void turn(int knob, int clicks) { mgr.controlChangeReceived(16, 0x15 + knob, clicks < 0 ? 128 + clicks : clicks); }
        juce::String name(int pad) const { return ChordPads::nameOf(chords.get(pad)).full(); }
        std::vector<int> pitches(int pad) const
        {
            std::vector<int> p;
            for (int i = 0; i < chords.get(pad).count; ++i) p.push_back(chords.get(pad).notes[(size_t) i].pitch);
            return p;
        }
    };

    static ChordPads::Name named(std::initializer_list<int> pitches)
    {
        std::vector<ChordPads::Note> notes;
        for (int p : pitches) notes.push_back({ p, 1.0f });
        return ChordPads::nameOf(notes.data(), (int) notes.size());
    }

    void runTest() override
    {
        using V = std::vector<int>;

        beginTest("Names: the chord from its notes, an inversion with its bass, anything else by its lowest note");
        {
            expectEquals(named({ 60, 64, 67 }).full(), juce::String("C"));
            expectEquals(named({ 57, 60, 64 }).full(), juce::String("Am"));
            expectEquals(named({ 64, 67, 72 }).full(), juce::String("C/E"));
            expectEquals(named({ 60, 64, 67, 70 }).full(), juce::String("C7"));
            expectEquals(named({ 62, 65, 69, 72 }).full(), juce::String("Dm7"));
            expectEquals(named({ 60, 64, 67, 71 }).full(), juce::String("CM7"));
            expectEquals(named({ 61, 64, 68 }).full(), juce::String("C#m"));
            expectEquals(named({ 59, 62, 65 }).full(), juce::String("Bo"));
            expectEquals(named({ 60, 65, 67 }).full(), juce::String("Cs4"));
            expectEquals(named({ 48, 60, 64, 67, 76 }).full(), juce::String("C"), "doubled notes change nothing");
            expectEquals(named({ 60 }).full(), juce::String("C"));
            expectEquals(named({ 60, 67 }).full(), juce::String("C5"));
            expectEquals(named({ 60, 61, 62 }).full(), juce::String("C*"), "a cluster: the lowest note, marked");
            expectEquals(named({}).full(), juce::String("--"));
            expectEquals(named({ 60, 64, 67 }).cell(), juce::String("M"), "a pad cell is never blank under its root");
            expectEquals(named({ 62, 65, 69, 72 }).cell(), juce::String("m7"));
        }

        beginTest("Hold a chord on the keys and press a pad: it is on the pad; the pad then plays it");
        {
            Rig r;
            expectEquals(r.mgr.getActiveScreen()->getName(), juce::String("CHORD"));
            r.keyOn(64, 80); r.keyOn(60, 120); r.keyOn(67, 100);
            expect(r.screen->getSubPage().startsWith("C > PAD"), "the name bar names what is held: " + r.screen->getSubPage());
            r.padOn(4); r.padOff(4);
            expect(r.pitches(4) == (V { 60, 64, 67 }), "the notes of the keys, lowest first");
            expectEquals(r.screen->getSubPage(), juce::String("PAD 5 = C"));
            expect(r.played.isEmpty(), "storing plays nothing: the keys are sounding");
            expectEquals(r.keys.log.joinIntoString(","), juce::String("on64,on60,on67"), "and the keys play as always");
            r.keyOff(60); r.keyOff(64); r.keyOff(67);

            // Played from the pad: the pad's velocity, each note at the balance it was played with.
            r.padOn(4, 127);
            expectEquals(r.played.size(), 3);
            expect(r.played[0].startsWith("on60:") && r.played[1].startsWith("on64:") && r.played[2].startsWith("on67:"));
            const int loud = r.played[0].fromFirstOccurrenceOf(":", false, false).getIntValue();
            const int soft = r.played[1].fromFirstOccurrenceOf(":", false, false).getIntValue();
            expect(loud > soft, "the key played harder is louder in the chord: " + r.played.joinIntoString(" "));
            r.padPressure(4, 50);
            expect(r.played.contains("at60:50") && r.played.contains("at64:50") && r.played.contains("at67:50"), "pressure reaches every note");
            r.padOff(4);
            expect(r.played.contains("off60") && r.played.contains("off64") && r.played.contains("off67"));

            // A softer hit is a softer chord; VL off: every note at the pad's velocity.
            r.played.clear();
            r.padOn(4, 40); r.padOff(4);
            expect(r.played[0].fromFirstOccurrenceOf(":", false, false).getIntValue() < loud);
            r.turn(3, -1);
            r.played.clear();
            r.padOn(4, 90); r.padOff(4);
            expect(r.played[0] == "on60:90" && r.played[1] == "on64:90" && r.played[2] == "on67:90", r.played.joinIntoString(" "));

            // An empty pad says what to do and plays nothing.
            r.played.clear();
            r.padOn(9); r.padOff(9);
            expect(r.played.isEmpty());
            expectEquals(r.screen->getSubPage(), juce::String("EMPTY: HOLD KEYS"));
        }

        beginTest("A pad that has a chord takes the new one when keys are held; undo brings the old one back");
        {
            Rig r;
            r.keyOn(60); r.keyOn(64); r.keyOn(67);
            r.padOn(0); r.padOff(0);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);
            r.now += 3000;
            r.keyOn(57); r.keyOn(60); r.keyOn(64);
            r.padOn(0); r.padOff(0);
            r.keyOff(57); r.keyOff(60); r.keyOff(64);
            expectEquals(r.name(0), juce::String("Am"), "overwritten");

            r.func(true); r.shift(true); r.tap(115); r.shift(false); r.func(false);     // Func + Shift + Play
            expectEquals(r.name(0), juce::String("C"), "undo: the chord that was there");
            r.func(true); r.shift(true); r.tap(115); r.shift(false); r.func(false);
            expectEquals(r.name(0), juce::String("--"), "and once more: the pad as it was before any chord");
        }

        beginTest("Latched keys count as held");
        {
            Rig r;
            r.shift(true); r.tap(109); r.shift(false);          // Shift + Arp: key latch (no arp on)
            expect(r.mgr.isKeyLatchOn());
            r.keyOn(62); r.keyOn(65); r.keyOn(69);
            r.keyOff(62); r.keyOff(65); r.keyOff(69);           // hands off the keys: the latch holds them
            r.padOn(3); r.padOff(3);
            expectEquals(r.name(3), juce::String("Dm"));
        }

        beginTest("TR and OC move every pad; BK and Up / Down change the bank; CL empties it after four clicks");
        {
            Rig r;
            r.keyOn(60); r.keyOn(64); r.keyOn(67);
            r.padOn(0); r.padOff(0);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);

            r.turn(1, 2);                                       // TR +2
            r.turn(2, -1);                                      // OC -1
            r.padOn(0, 100);
            expect(r.played[0].startsWith("on50:") && r.played[1].startsWith("on54:") && r.played[2].startsWith("on57:"), r.played.joinIntoString(" "));
            expect(r.pitches(0) == (V { 60, 64, 67 }), "what is stored is untouched");
            // A change of bank lets go of what is sounding.
            r.played.clear();
            r.turn(0, 1);
            expect(r.played.contains("off50"), "no note is left hanging: " + r.played.joinIntoString(" "));
            r.padOff(0);
            expectEquals(r.chords.getBank(), 1);
            expectEquals(r.name(0), juce::String("--"), "the second bank is empty");
            r.tap(107);                                         // Down: next bank
            expectEquals(r.chords.getBank(), 2);
            r.tap(106); r.tap(106);                             // Up, Up
            expectEquals(r.chords.getBank(), 0);
            expectEquals(r.name(0), juce::String("C"));

            for (int i = 0; i < 3; ++i) { r.turn(7, 1); r.now += 150; UiClock::advance(150); }
            expectEquals(r.name(0), juce::String("C"), "three clicks: nothing yet");
            r.turn(7, 1);
            expectEquals(r.name(0), juce::String("--"), "the fourth clears the bank");
            r.func(true); r.shift(true); r.tap(115); r.shift(false); r.func(false);
            expectEquals(r.name(0), juce::String("C"), "undo");
        }

        beginTest("Pads light by root; an empty pad brightens while keys are held; a sounding pad is white");
        {
            Rig r;
            std::array<Screen::PadColour, 16> c;
            expect(r.screen->getPadColours(c));
            const auto dim = c[0];
            r.keyOn(60); r.keyOn(64); r.keyOn(67);
            expect(r.screen->getPadColours(c));
            expect(c[0].r > dim.r, "keys held: the empty pads invite");
            r.padOn(0); r.padOff(0);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);
            r.keyOn(67); r.keyOn(71); r.keyOn(74);
            r.padOn(1); r.padOff(1);
            r.keyOff(67); r.keyOff(71); r.keyOff(74);
            expect(r.screen->getPadColours(c));
            expect(!(c[0].r == c[1].r && c[0].g == c[1].g && c[0].b == c[1].b), "C and G have different colours");
            r.padOn(1);
            expect(r.screen->getPadColours(c));
            expect(c[1].r == 127 && c[1].g == 127 && c[1].b == 127);
            r.padOff(1);
        }

        beginTest("Shift + its pad opens CHORD; the pads can be pinned and then play on another screen");
        {
            Rig r;
            r.keyOn(60); r.keyOn(64); r.keyOn(67);
            r.padOn(2); r.padOff(2);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);

            r.mgr.setActiveScreen(0);
            r.shift(true); r.padOn(10); r.padOff(10); r.shift(false);
            expectEquals(r.mgr.getActiveScreen()->getName(), juce::String("CHORD"));
            r.shift(true); r.padOn(10); r.now += 600; r.mgr.tick(); r.padOff(10); r.shift(false);        // held: pin
            expect(r.mgr.getNameBarTags().contains("PIN"), "pinned: " + r.mgr.getNameBarTags().joinIntoString(" "));
            r.mgr.setActiveScreen(0);
            r.played.clear();
            r.padOn(2); r.padOff(2);
            expect(r.played.size() == 6, "the chord plays from the other screen: " + r.played.joinIntoString(" "));
        }

        beginTest("Chords are part of the project: file round trip, and a project without them empties the pads");
        {
            Rig r;
            r.keyOn(60, 90); r.keyOn(63, 70); r.keyOn(67, 110);
            r.padOn(5); r.padOff(5);
            r.keyOff(60); r.keyOff(63); r.keyOff(67);
            r.chords.setBank(2);
            r.chords.store(15, { { 40, 0.5f }, { 47, 0.6f } });
            r.chords.transpose = -3;

            ProjectBridge::Live live { r.seq, r.audio, nullptr, nullptr, nullptr, nullptr, &r.chords };
            const auto data = ProjectBridge::capture(live, "T");
            const auto back = ProjectData::fromJSON(data.toJSON());
            expect(back.chordPads == data.chordPads && !back.chordPads.empty(), "the file keeps them");

            ChordPads other;
            ProjectBridge::Live into { r.seq, r.audio, nullptr, nullptr, nullptr, nullptr, &other };
            ProjectBridge::apply(back, into);
            expectEquals(other.getBank(), 2);
            expectEquals(other.transpose, -3);
            expectEquals(ChordPads::nameOf(other.get(0, 5)).full(), juce::String("Cm"));
            expectEquals(other.get(0, 5).notes[1].velocity, r.chords.get(0, 5).notes[1].velocity);
            expectEquals(other.get(2, 15).count, 2);

            ProjectData none = back;
            none.chordPads.clear();
            ProjectBridge::apply(none, into);
            expect(!other.anyStored(), "a project saved before chord pads: every pad empty");
        }
    }
};

static ChordPadTests chordPadTests;
