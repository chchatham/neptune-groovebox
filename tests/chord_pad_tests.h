#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>
#include "../src/controller/ChordPads.h"
#include "../src/controller/NotePadSlots.h"
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
            screen->setPhysicalKeysProvider([this]
            {
                std::vector<ChordPads::Note> held;
                for (auto& [note, velocity] : mgr.getPhysicallyHeldKeys())
                    held.push_back({ note, (float) velocity / 127.0f });
                return held;
            });
            screen->setNoteCallback([this](int n, int v) { played.add("on" + juce::String(n) + ":" + juce::String(v)); });
            screen->setNoteOffCallback([this](int n) { played.add("off" + juce::String(n)); });
            screen->setAftertouchCallback([this](int n, int p) { played.add("at" + juce::String(n) + ":" + juce::String(p)); });
            screen->setLatchState([this] { return mgr.isKeyLatchOn(); }, [this] { return mgr.getLatchReleaseCount(); });
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
            r.turn(3, 1);

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

        beginTest("Latched notes alone do not store; physically held keys do store");
        {
            Rig r;
            r.shift(true); r.tap(109); r.shift(false);          // Shift + Arp: key latch (no arp on)
            expect(r.mgr.isKeyLatchOn());
            // Store a known chord on pad 3 first (with real keys down).
            r.keyOn(60); r.keyOn(64); r.keyOn(67);
            r.padOn(3); r.padOff(3);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);
            expectEquals(r.name(3), juce::String("C"));
            // Now: latch is holding C. Press and release Dm keys: the latch replaces with Dm.
            r.keyOn(62); r.keyOn(65); r.keyOn(69);
            r.keyOff(62); r.keyOff(65); r.keyOff(69);           // hands off: the latch holds Dm
            // A pad press with no physical keys: plays, does not store.
            r.played.clear();
            r.padOn(3); r.padOff(3);
            expectEquals(r.name(3), juce::String("C"), "latched notes alone do not overwrite");
            expect(r.played.size() > 0, "the pad played its chord instead");
            // With a finger on a key, the pad stores.
            r.keyOn(62); r.keyOn(65); r.keyOn(69);              // physically holding Dm
            r.padOn(4); r.padOff(4);
            r.keyOff(62); r.keyOff(65); r.keyOff(69);
            expectEquals(r.name(4), juce::String("Dm"), "physically held keys store the chord");
        }

        beginTest("No physical keys held: a pad plays its chord, not stores; arp latch does not count as held keys");
        {
            Rig r;
            // Store a Cmaj on pad 0 with real keys.
            r.keyOn(60); r.keyOn(64); r.keyOn(67);
            r.padOn(0); r.padOff(0);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);
            expectEquals(r.name(0), juce::String("C"));
            // Store a Dm on pad 1.
            r.keyOn(62); r.keyOn(65); r.keyOn(69);
            r.padOn(1); r.padOff(1);
            r.keyOff(62); r.keyOff(65); r.keyOff(69);
            expectEquals(r.name(1), juce::String("Dm"));
            // Press pad 0 (plays C), then press pad 1 with no keys held: pad 1 must still be Dm.
            r.played.clear();
            r.padOn(0);
            expect(r.played.size() > 0, "pad 0 should sound");
            r.padOn(1);
            expect(!r.screen->getSounding(1).empty(), "pad 1 should sound");
            expectEquals(r.name(1), juce::String("Dm"), "pad 1 must not be overwritten");
            r.padOff(0); r.padOff(1);
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

        beginTest("VL: every key's own velocity is on the pad - exactly (KEYS), or as a balance under the pad's velocity");
        {
            Rig r;
            r.keyOn(60, 3); r.keyOn(64, 40); r.keyOn(67, 80);
            r.padOn(0); r.padOff(0);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);
            expectWithinAbsoluteError(r.chords.get(0).notes[0].velocity, 3.0f / 127.0f, 1.0e-6f, "the softest key is kept as it was");

            expectEquals(r.chords.velocityMode, (int) ChordPads::VEL_BALANCE);
            r.padOn(0, 127); r.padOff(0);
            expect(r.played[0] == "on60:5" && r.played[1] == "on64:64" && r.played[2] == "on67:127",
                   "the loudest key at the pad's velocity, the others in proportion: " + r.played.joinIntoString(" "));

            r.turn(3, 1);
            expectEquals(r.chords.velocityMode, (int) ChordPads::VEL_KEYS);
            for (int padVelocity : { 20, 127 })
            {
                r.played.clear();
                r.padOn(0, padVelocity); r.padOff(0);
                expect(r.played[0] == "on60:3" && r.played[1] == "on64:40" && r.played[2] == "on67:80",
                       "exactly as the keys were played: " + r.played.joinIntoString(" "));
            }
            r.turn(3, 1);
            expectEquals(r.chords.velocityMode, (int) ChordPads::VEL_KEYS, "the end of the list");

            ChordPads other;
            other.fromFlat(r.chords.toFlat());
            expectEquals(other.velocityMode, (int) ChordPads::VEL_KEYS);
        }

        beginTest("Two pads that share a note: no slot is lost, the note ends with the last pad, a later pad still plays");
        {
            NotePadSlots slots;
            juce::StringArray log;
            slots.slotOn = [&log](int slot, int note, float) { log.add("on" + juce::String(note) + "@" + juce::String(slot)); };
            slots.slotOff = [&log](int slot) { log.add("off@" + juce::String(slot)); };
            slots.slotPressure = [&log](int slot, float) { log.add("at@" + juce::String(slot)); };
            slots.keyOn = [&log](int note, int) { log.add("key" + juce::String(note)); };
            slots.keyOff = [&log](int note) { log.add("keyoff" + juce::String(note)); };

            const std::vector<int> a { 55, 62, 67, 70 }, b { 55, 62, 69 };
            // The owner's playing: one chord pressed before the last is let go, over and over.
            for (int round = 0; round < 20; ++round)
            {
                for (int n : a) expect(slots.down(n, 100, false));
                for (int n : b) expect(slots.down(n, 100, false));
                expectEquals(slots.slotsInUse(), 5, "seven presses, five different notes");
                for (int n : a) slots.up(n);
                expect(slots.isHeld(55) && slots.isHeld(62), "the second pad still holds the notes they share");
                expect(!slots.isHeld(67) && !slots.isHeld(70));
                for (int n : b) slots.up(n);
                expectEquals(slots.slotsInUse(), 0, "everything given back, round " + juce::String(round));
            }
            log.clear();
            for (int n : a) slots.down(n, 100, false);
            expect(slots.down(31, 100, false), "a pad pressed second is played");
            expect(log.contains("on31@4"), log.joinIntoString(" "));
            slots.pressure(31, 60);
            expect(log.contains("at@4"));

            // The same note again strikes it again where it is; it ends once.
            log.clear();
            slots.down(31, 90, false);
            expectEquals(log.joinIntoString(" "), juce::String("on31@4"));
            slots.up(31);
            expect(!log.contains("off@4"), "still held once");
            slots.up(31);
            expect(log.contains("off@4"));
            slots.up(31);                                       // one release too many changes nothing
            for (int n : a) slots.up(n);
            expectEquals(slots.slotsInUse(), 0);

            // Sixteen notes at most; the seventeenth says so, and is free to play once one is let go.
            for (int n = 40; n < 56; ++n) expect(slots.down(n, 100, false));
            expect(!slots.down(60, 100, false), "no slot left");
            slots.up(60);
            slots.up(40);
            expect(slots.down(60, 100, false));
            for (int n = 40; n < 61; ++n) slots.up(n);
            expectEquals(slots.slotsInUse(), 0);

            // With the arp on the notes go the keys' way, counted the same.
            log.clear();
            slots.down(55, 100, true); slots.down(55, 100, true);
            slots.up(55);
            expect(!log.contains("keyoff55"));
            slots.up(55);
            expectEquals(log.joinIntoString(" "), juce::String("key55 key55 keyoff55"));
            expectEquals(slots.slotsInUse(), 0);
        }

        beginTest("Key latch: a pad let go sounds on until the next pad, the latch going off, or stop; a stroke is played out");
        {
            Rig r;
            r.chords.store(0, { { 60, 1.0f }, { 64, 1.0f }, { 67, 1.0f } });
            r.chords.store(1, { { 62, 1.0f }, { 65, 1.0f } });
            r.chords.store(2, { { 48, 1.0f } });
            auto offs = [&r] { int n = 0; for (auto& e : r.played) n += e.startsWith("off") ? 1 : 0; return n; };

            r.shift(true); r.tap(109); r.shift(false);          // key latch on
            expect(r.mgr.isKeyLatchOn());
            r.padOn(0); r.padOff(0);
            expectEquals(offs(), 0, "let go, and it sounds on");
            expect(r.screen->getSounding(0).size() == 3 && r.screen->strumIsWaiting());
            r.screen->serviceStrum();
            expectEquals(offs(), 0, "nothing has let go of the latch");

            r.padOn(1);                                         // a new chord: the old one ends
            expect(r.played.contains("off60") && r.played.contains("off64") && r.played.contains("off67"));
            expect(r.played.contains("on62:100"));
            r.padOn(2);                                         // pressed while pad 2 is down: added
            r.padOff(1); r.padOff(2);
            expect(!r.played.contains("off62") && !r.played.contains("off48"), "both are kept");

            r.shift(true); r.tap(109); r.shift(false);          // latch off
            r.screen->serviceStrum();
            expect(r.played.contains("off62") && r.played.contains("off65") && r.played.contains("off48"), r.played.joinIntoString(" "));
            expect(!r.screen->strumIsWaiting());

            // Without the latch a pad ends with the finger, as before.
            r.played.clear();
            r.padOn(0); r.padOff(0);
            expectEquals(offs(), 3);

            // Stop lets go of what the latch holds; the latch stays on.
            r.shift(true); r.tap(109); r.shift(false);
            r.played.clear();
            r.padOn(0); r.padOff(0);
            r.tap(115);                                         // Play: start
            r.tap(115);                                         // Play: stop
            r.screen->serviceStrum();
            expectEquals(offs(), 3, "stop is quiet: " + r.played.joinIntoString(" "));
            expect(r.mgr.isKeyLatchOn());

            // A stroke is played out after the pad is let go.
            r.chords.strumMs = 200.0f;
            r.played.clear();
            r.padOn(0); r.padOff(0);
            expectEquals(r.played.size(), 1);
            UiClock::advance(250); r.screen->serviceStrum();
            expect(r.played.size() == 3 && offs() == 0, r.played.joinIntoString(" "));
            r.padOn(1);
            expectEquals(offs(), 3);
            r.padOff(1);
        }

        beginTest("STRUM page: a pad's notes start one after the other over TM; a pad let go early leaves the rest unplayed");
        {
            Rig r;
            r.keyOn(60); r.keyOn(64); r.keyOn(67);
            r.padOn(0); r.padOff(0);
            r.keyOff(60); r.keyOff(64); r.keyOff(67);
            auto pass = [&r](int ms) { UiClock::advance(ms); r.screen->serviceStrum(); };
            r.chords.velocityMode = ChordPads::VEL_PAD;                   // every note at the pad's level
            UiClock::advance(2000);                             // (the "PAD 1 = C" message has gone)

            expectEquals(r.screen->getPageCount(), 2);
            r.tap(52);                                          // bank button: the STRUM page
            expectEquals(r.screen->getPageIndex(), 1);
            expectEquals(r.screen->getEncoderName(0), juce::String("TM"));
            expectEquals(r.screen->getSubPage(), juce::String("STRUM OFF"));
            r.turn(0, 1);
            expectEquals(r.chords.strumMs, 10.0f, "one click is 10 ms");
            r.turn(0, -1);
            expectEquals(r.chords.strumMs, 0.0f);
            r.turn(7, 1); r.turn(7, 1); r.turn(7, 1); r.turn(7, 1);
            expectEquals(r.name(0), juce::String("C"), "knob 8 clears nothing on this page");
            r.chords.strumMs = 300.0f;
            expectEquals(r.screen->getSubPage(), juce::String("STRUM 300"));

            r.played.clear();
            r.padOn(0, 100);
            expectEquals(r.played.joinIntoString(" "), juce::String("on60:100"), "the first note at once, alone");
            expect(r.screen->strumIsWaiting());
            pass(149);
            expectEquals(r.played.size(), 1, "not before its time");
            pass(2);
            expect(r.played.size() == 2 && r.played[1] == "on64:100", r.played.joinIntoString(" "));
            pass(150);
            expect(r.played.size() == 3 && r.played[2] == "on67:100", r.played.joinIntoString(" "));
            expect(!r.screen->strumIsWaiting());
            r.padPressure(0, 50);
            expect(r.played.contains("at67:50"), "pressure reaches the notes of the stroke");
            r.padOff(0);
            expect(r.played.contains("off60") && r.played.contains("off64") && r.played.contains("off67"));

            r.played.clear();
            r.padOn(0, 100);
            pass(160);
            r.padOff(0);
            pass(500);
            expectEquals(r.played.joinIntoString(" "), juce::String("on60:100 on64:100 off60 off64"), "the last note never starts");
            expect(!r.screen->strumIsWaiting());

            // Down, and one then the other.
            r.tap(51);                                          // back to the pads' page and to STRUM again
            expectEquals(r.screen->getEncoderName(0), juce::String("BK"));
            r.tap(51);
            r.turn(1, 1);
            expectEquals(r.chords.strumDir, (int) ChordPads::STRUM_DOWN);
            r.played.clear();
            r.padOn(0, 100); pass(400); r.padOff(0);
            expect(r.played[0] == "on67:100" && r.played[2] == "on60:100", r.played.joinIntoString(" "));
            r.turn(1, 1);
            expectEquals(r.chords.strumDir, (int) ChordPads::STRUM_ALT);
            juce::String firsts;
            for (int i = 0; i < 4; ++i)
            {
                r.played.clear();
                r.padOn(0, 100); pass(400); r.padOff(0);
                firsts += r.played[0].substring(2, 4) + " ";
            }
            expect(firsts == "60 67 60 67 " || firsts == "67 60 67 60 ", firsts);

            // FL: the later, the softer.
            r.chords.strumDir = ChordPads::STRUM_UP;
            r.chords.strumFall = 1.0f;
            r.played.clear();
            r.padOn(0, 100); pass(400); r.padOff(0);
            expect(r.played[0] == "on60:100" && r.played[1] == "on64:70" && r.played[2] == "on67:40", r.played.joinIntoString(" "));

            // The arp on (the app says so through setStrumAllowed): the notes go together.
            r.screen->setStrumAllowed([] { return false; });
            r.played.clear();
            r.padOn(0, 100);
            expectEquals(r.played.size(), 3);
            expect(!r.screen->strumIsWaiting());
            r.padOff(0);
        }

        beginTest("HU makes the timing uneven and VR the pattern; every note is still played once, inside the stroke");
        {
            ChordPads c;
            c.store(0, { { 48, 1.0f }, { 55, 1.0f }, { 60, 1.0f }, { 64, 1.0f }, { 67, 1.0f } });
            c.strumMs = 200.0f;
            juce::Random random(7);
            bool alt = false;

            auto plain = c.strum(0, 1.0f, random, alt);
            expectEquals((int) plain.size(), 5);
            for (int i = 0; i < 5; ++i)
                expectWithinAbsoluteError(plain[(size_t) i].delayMs, 50.0f * (float) i, 0.01f);
            expectEquals(plain[4].pitch, 67);

            c.strumHuman = 1.0f;
            float shortest = 1.0e9f, longest = 0.0f;
            bool uneven = false;
            for (int press = 0; press < 200; ++press)
            {
                const auto s = c.strum(0, 1.0f, random, alt);
                expectEquals(s[0].delayMs, 0.0f);
                for (size_t i = 1; i < s.size(); ++i)
                {
                    expect(s[i].delayMs > s[i - 1].delayMs, "in order of time");
                    expect(s[i].pitch > s[i - 1].pitch, "HU leaves the pattern alone");
                }
                shortest = juce::jmin(shortest, s[4].delayMs);
                longest = juce::jmax(longest, s[4].delayMs);
                uneven = uneven || std::abs((s[1].delayMs - s[0].delayMs) - (s[2].delayMs - s[1].delayMs)) > 10.0f;
            }
            expect(shortest >= 139.9f && shortest < 160.0f, "the quickest stroke: " + juce::String(shortest));
            expect(longest <= 260.1f && longest > 240.0f, "the slowest stroke: " + juce::String(longest));
            expect(uneven, "the gaps differ");

            c.strumHuman = 0.0f;
            c.strumVary = 1.0f;
            int downward = 0, shuffled = 0;
            for (int press = 0; press < 200; ++press)
            {
                const auto s = c.strum(0, 1.0f, random, alt);
                std::vector<int> pitches;
                for (auto& n : s) pitches.push_back(n.pitch);
                expectWithinAbsoluteError(s[4].delayMs, 200.0f, 0.01f, "VR leaves the timing alone");
                if (pitches[0] > pitches[4]) ++downward;
                auto sorted = pitches;
                std::sort(sorted.begin(), sorted.end());
                if (pitches != sorted && pitches != std::vector<int>(sorted.rbegin(), sorted.rend())) ++shuffled;
                expect(sorted == (std::vector<int> { 48, 55, 60, 64, 67 }), "every note, once");
            }
            expect(downward > 60 && downward < 140, "about half the strokes go the other way: " + juce::String(downward));
            expect(shuffled > 100, "neighbours change places: " + juce::String(shuffled));

            c.strumVary = 0.0f;
            c.strumMs = 0.0f;
            for (auto& n : c.strum(0, 1.0f, random, alt))
                expectEquals(n.delayMs, 0.0f, "no strum time: together");
        }

        beginTest("The strum is kept with the chords; a list from before it reads as no strum");
        {
            Rig r;
            r.chords.strumMs = 420.0f;
            r.chords.strumDir = ChordPads::STRUM_ALT;
            r.chords.strumHuman = 0.25f;
            r.chords.strumVary = 0.5f;
            r.chords.strumFall = 0.75f;

            ProjectBridge::Live live { r.seq, r.audio, nullptr, nullptr, nullptr, nullptr, &r.chords };
            const auto back = ProjectData::fromJSON(ProjectBridge::capture(live, "T").toJSON());
            expect(!back.chordPads.empty(), "a strum with no chord stored is still saved");
            ChordPads other;
            ProjectBridge::Live into { r.seq, r.audio, nullptr, nullptr, nullptr, nullptr, &other };
            ProjectBridge::apply(back, into);
            expectEquals(other.strumMs, 420.0f);
            expectEquals(other.strumDir, (int) ChordPads::STRUM_ALT);
            expectEquals(other.strumHuman, 0.25f);
            expectEquals(other.strumVary, 0.5f);
            expectEquals(other.strumFall, 0.75f);

            r.chords.store(3, { { 60, 1.0f }, { 64, 1.0f } });
            auto old = r.chords.toFlat();
            old.resize(old.size() - 5);                         // as written before the strum existed
            other.fromFlat(old);
            expectEquals(other.get(0, 3).count, 2);
            expectEquals(other.strumMs, 0.0f);
            old.insert(old.end(), { 1.0e9f, 9.0f, -4.0f, std::numeric_limits<float>::quiet_NaN(), 7.0f });
            other.fromFlat(old);
            expectEquals(other.strumMs, ChordPads::MAX_STRUM_MS, "values from a file are brought into range");
            expectEquals(other.strumDir, (int) ChordPads::STRUM_ALT);
            expect(other.strumHuman == 0.0f && other.strumVary == 0.0f && other.strumFall == 1.0f);
        }
    }
};

static ChordPadTests chordPadTests;
