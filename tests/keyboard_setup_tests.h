#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include "../src/controller/KeyboardSplit.h"
#include "../src/project/ProjectBridge.h"

// The keyboard (split and octave) and the arp's switches in what a project, the session and a
// preset store. The preset side is in preset_audition_tests.h.
class KeyboardSetupTests : public juce::UnitTest
{
public:
    KeyboardSetupTests() : juce::UnitTest("Keyboard Setup") {}

    // The firmware as KeyboardSplit sees it: memory read and written through the debug command,
    // and what an octave button does to it.
    struct Firmware
    {
        std::map<juce::uint32, juce::uint32> mem;
        KeyboardSplit split;
        juce::int64 now = 0;
        bool shift = false;
        int sent = 0;

        Firmware()
        {
            split.clock = [this] { return now; };
            split.isShiftHeld = [this] { return shift; };
            split.send = [this](const std::vector<juce::uint8>& m)
            {
                ++sent;
                auto word = [&](int at) { juce::uint32 v = 0; for (int i = 0; i < 5; ++i) v = (v << 7) | m[(size_t) (at + i)]; return v; };
                const juce::uint32 fn = word(5) & ~1u, a0 = word(10), a1 = word(15);
                juce::uint32 r0 = 0;
                if (fn == KeyboardSplit::PEEK32) r0 = mem[a0];
                else if (fn == KeyboardSplit::POKE32) { mem[a0] = a1; r0 = a0; }
                juce::uint8 reply[10] = { 0x00, 0x20, 0x29, 0x00, 0x7D };
                for (int i = 0; i < 5; ++i) reply[5 + i] = (juce::uint8) ((r0 >> (28 - 7 * i)) & 0x7F);
                split.sysExReceived(reply, 10);
            };
            writeKeys();
        }

        int octave() { return (int) (juce::int32) mem[KeyboardSplit::OCTAVE]; }
        void writeKeys()
        {
            for (int k = 0; k < 25; ++k)
                mem[KeyboardSplit::KEY_MAP + 4 * (juce::uint32) k] = KeyboardSplit::KEY_WORD | (juce::uint32) (48 + 12 * octave() + k);
        }
        void octaveButton(int delta) { mem[KeyboardSplit::OCTAVE] = (juce::uint32) (octave() + delta); writeKeys(); }
        int note(int key) { return (int) (mem[KeyboardSplit::KEY_MAP + 4 * (juce::uint32) key] & 0xFF); }
        void polls(int n) { for (int i = 0; i < n; ++i) { now += 200; split.poll(); } }
    };

    void runTest() override
    {
        beginTest("The octave buttons are noticed within two seconds, with one message, while nothing else goes on");
        {
            Firmware fw;
            fw.split.poll();
            expect(fw.split.isOctaveKnown());
            expect(fw.split.getSetup() == KeyboardSetup { 0, 0 });
            fw.now = 5000;
            fw.octaveButton(1);
            int before = fw.sent;
            fw.polls(KeyboardSplit::IDLE_POLLS - 1);
            expectEquals(fw.sent, before, "quiet in between");
            fw.polls(1);
            expectEquals(fw.sent, before + 1, "one question");
            expect(fw.split.getSetup() == KeyboardSetup { 0, 1 });
        }

        beginTest("Setting the octave writes the firmware's octave and the keys; the buttons carry on from there");
        {
            Firmware fw;
            fw.split.poll();
            fw.split.apply({ -1, 2 });
            expectEquals(fw.octave(), 2);
            expectEquals((int) (juce::int32) fw.mem[KeyboardSplit::OCTAVE_SHOWN], 2);
            expectEquals(fw.note(0), 72); expectEquals(fw.note(24), 96);
            expectEquals((int) fw.mem[KeyboardSplit::KEY_RECORDS], 72, "the records the firmware reloads from");
            expectEquals((int) (fw.mem[KeyboardSplit::NOTE_TABLE] & 0xFF), 72, "and its note table");
            expectEquals(fw.split.getSplit(), 0, "a split that is not stored is left alone");

            const int before = fw.sent;
            fw.split.apply({ -1, 2 });
            fw.split.apply({ 0, KeyboardSetup::UNKNOWN });
            expectEquals(fw.sent, before, "nothing is sent for what is already so");

            fw.octaveButton(-1);
            fw.polls(KeyboardSplit::IDLE_POLLS);
            expect(fw.split.getSetup() == KeyboardSetup { 0, 1 });

            // Octave and split together, below zero.
            fw.split.apply({ 1, -2 });
            expectEquals(fw.octave(), -2);
            expectEquals(fw.note(0), 12); expectEquals(fw.note(11), 23);     // lower octave one further down
            expectEquals(fw.note(12), 36); expectEquals(fw.note(24), 48);
            expect(fw.split.getSetup() == KeyboardSetup { 1, -2 });
        }

        beginTest("A keyboard asked for before the controller has answered is set when it does");
        {
            Firmware fw;
            fw.octaveButton(1);                          // the controller was left on octave +1
            expect(fw.split.getSetup() == KeyboardSetup { 0, KeyboardSetup::UNKNOWN }, "the octave is not known yet");
            fw.split.apply({ 2, -1 });                   // the session is put back at the start
            expectEquals(fw.octave(), 1, "nothing is written blind");
            fw.split.poll();
            expectEquals(fw.octave(), -1);
            expectEquals(fw.note(0), 12); expectEquals(fw.note(12), 48);
            expect(fw.split.getSetup() == KeyboardSetup { 2, -1 });
        }

        beginTest("A project holds the keyboard; one without it leaves the keyboard alone");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            KeyboardSetup keys { 3, -2 };
            int sets = 0;
            ProjectBridge::Live live { seq, audio };
            live.getKeyboard = [&] { return keys; };
            live.setKeyboard = [&](const KeyboardSetup& k)
            {
                if (k == KeyboardSetup()) return;
                ++sets;
                keys = k;
            };
            const auto json = ProjectBridge::capture(live, "K").toJSON();
            expect(ProjectData::fromJSON(json).keyboard == KeyboardSetup { 3, -2 });

            keys = { 0, 0 };
            ProjectBridge::apply(ProjectData::fromJSON(json), live);
            expect(keys == KeyboardSetup { 3, -2 });

            // A project from before: nothing about the keyboard in the file.
            sets = 0;
            const auto old = ProjectBridge::capture({ seq, audio }, "OLD").toJSON();
            expect(! old.contains("keyboardSplit") && ! old.contains("keyboardOctave"));
            ProjectBridge::apply(ProjectData::fromJSON(old), live);
            expectEquals(sets, 0);
        }

        beginTest("The arp's switches are stored: on, latch, mutate; an older file's on / off still reads");
        {
            for (int switches = 0; switches < 8; ++switches)
            {
                Arpeggiator a, b;
                a.setEnabled((switches & 1) != 0);
                a.setLatch((switches & 2) != 0);
                a.setMutateEnabled((switches & 4) != 0);
                b.setEnabled((switches & 1) == 0);
                b.setLatch((switches & 2) == 0);
                b.setMutateEnabled((switches & 4) == 0);
                ProjectBridge::applyArp(ProjectBridge::captureArp(a), b);
                expect(b.isEnabled() == a.isEnabled() && b.getLatch() == a.getLatch() && b.isMutateEnabled() == a.isMutateEnabled(),
                       "switches " + juce::String(switches));
            }
            Arpeggiator a, b;
            b.setLatch(true);
            auto old = ProjectBridge::captureArp(a);
            old[29] = 1.0f;                              // "on" as files held it before
            ProjectBridge::applyArp(old, b);
            expect(b.isEnabled() && ! b.getLatch() && ! b.isMutateEnabled());
        }

        beginTest("The roll latch is never brought back by a project or the session; the roll's other settings are");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            auto& roll = audio.getPadRatchet();
            roll.setEnabled(true);
            roll.setLatch(true);
            roll.setBaseDivision(PadRatchet::Div1_16);
            const auto json = ProjectBridge::capture({ seq, audio }, "R").toJSON();

            SequencerCore seq2;
            AudioEngine audio2;
            audio2.setSequencer(&seq2);
            ProjectBridge::apply(ProjectData::fromJSON(json), { seq2, audio2 });
            expect(audio2.getPadRatchet().isEnabled());
            expectEquals(audio2.getPadRatchet().getBaseDivision(), (int) PadRatchet::Div1_16);
            expect(! audio2.getPadRatchet().isLatch());

            // A file from before, with the latch stored as on.
            auto old = ProjectData::fromJSON(json);
            old.roll[13] = 1.0f;
            ProjectBridge::apply(old, { seq2, audio2 });
            expect(! audio2.getPadRatchet().isLatch());

            // A project that stores no roll: the roll as it comes, 1/8 to 1/128.
            old.roll.clear();
            ProjectBridge::apply(old, { seq2, audio2 });
            expectEquals(audio2.getPadRatchet().getBaseDivision(), (int) PadRatchet::Div1_8);
            expectEquals(audio2.getPadRatchet().getMaxDivision(), (int) PadRatchet::Div1_128);
        }

        beginTest("Quit and start again: a track's arp (on, latched), insert effects, effect blocks and sends come back");
        {
            // What the session file is: the whole state as JSON, applied to a newly started app.
            struct App
            {
                SequencerCore seq;
                AudioEngine audio;
                std::vector<std::unique_ptr<Arpeggiator>> arps;
                App()
                {
                    audio.setSequencer(&seq);
                    for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                    {
                        audio.addEngine(t, AudioEngine::createEngine(t == 0 ? EngineType::Wavetable : EngineType::UnifiedVoice));
                        for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                            seq.getPattern(p).getTrack(t).engineType = audio.getEngineTypeOf(t);
                        arps.push_back(std::make_unique<Arpeggiator>());
                    }
                }
                ProjectBridge::Live live()
                {
                    ProjectBridge::Live l { seq, audio };
                    l.arpForTrack = [this](int t) { return t >= 0 && t < (int) arps.size() ? arps[(size_t) t].get() : nullptr; };
                    return l;
                }
            };

            App first;
            auto& arp = *first.arps[0];
            arp.setMode(Arpeggiator::Mode::UpDown);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setOctaveRange(2);
            arp.setGate(0.8f);
            arp.setEnabled(true);
            arp.setLatch(true);
            first.audio.addInsertEffect(0, EffectType::DelayEffect);
            first.audio.addInsertEffect(0, EffectType::ReverbEffect);
            first.audio.setTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE, 0.42f);
            first.audio.setTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE + 1, 0.61f);
            first.audio.setTrackParameter(0, TrackFxParam::DIRT_BASE, first.audio.getTrackParameter(0, TrackFxParam::DIRT_BASE) + 0.3f);
            first.audio.setTrackParameter(0, MixParam::SEND_BASE, 0.37f);
            first.audio.setTrackParameter(0, MixParam::SEND_BASE + 1, 0.22f);
            const auto session = ProjectBridge::capture(first.live(), "S").toJSON();

            App second;
            ProjectBridge::apply(ProjectData::fromJSON(session), second.live());
            auto& arp2 = *second.arps[0];
            expect(arp2.isEnabled(), "the arp is on");
            expect(arp2.getLatch(), "and latched");
            expect(arp2.getMode() == Arpeggiator::Mode::UpDown && arp2.getOctaveRange() == 2);
            expectWithinAbsoluteError(arp2.getGate(), 0.8f, 0.001f);
            EffectType type {};
            expect(second.audio.getInsertEffectType(0, 0, type) && type == EffectType::DelayEffect);
            expect(second.audio.getInsertEffectType(0, 1, type) && type == EffectType::ReverbEffect);
            expect(! second.audio.getInsertEffectType(0, 2, type));
            for (int id : { AudioEngine::EFFECT_PARAM_BASE, AudioEngine::EFFECT_PARAM_BASE + 1, (int) TrackFxParam::DIRT_BASE,
                            (int) MixParam::SEND_BASE, (int) MixParam::SEND_BASE + 1 })
                expectWithinAbsoluteError(second.audio.getTrackParameter(0, id), first.audio.getTrackParameter(0, id), 0.0001f,
                                          "parameter " + juce::String(id));
            const auto again = ProjectBridge::capture(second.live(), "S").toJSON();
            expect(again == session, "and the whole state is the same again");
        }
    }
};

static KeyboardSetupTests keyboardSetupTests;
