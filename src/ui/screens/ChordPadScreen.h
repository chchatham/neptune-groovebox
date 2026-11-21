#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <vector>

#include "../ScreenManager.h"
#include "../ScreenLayout.h"
#include "../UiClock.h"
#include "../../controller/ChordPads.h"
#include "../../engine/ParamSpec.h"
#include "../../dsp/DSPUtils.h"

// CHORD: chord pads. Hold a chord on the keys and press a pad: the chord is on that pad (whatever
// was there is replaced; an undo step). Press a pad with no key held: it plays its chord on the
// selected track, with the pad's velocity and its pressure on every note. Latched keys count as
// held. Four banks of sixteen (Up / Down or BK). The pads can be pinned like those of DRUMS and SCALE.
//
// Knobs: BK bank, TR transpose (semitones), OC octave, VL how hard the notes are played (all at the
// pad's velocity / the balance of the keys under the pad's velocity / exactly as the keys were played),
// CL clear this bank (four clicks).
// Second knob page, STRUM (the bank buttons), one setting for every pad: TM the time from a chord's
// first note to its last (0 = together), DR up / down / alternating, HU uneven timing, VR a changing
// pattern (a stroke the other way, neighbours changing places), FL later notes softer. A pad let go
// before its stroke is over leaves the rest unplayed. With the track's arpeggiator on the notes go
// to it together (setStrumAllowed): the arp has its own strum.
// The later notes of a stroke are started by serviceStrum(), which the app calls every millisecond
// (under the screen manager's lock) while strumIsWaiting().
// Key latch on (setLatchState): a pad let go keeps sounding, and finishes its stroke. The next pad
// pressed with no other pad down is a new chord and ends the latched ones; a pad pressed while
// another is held down adds to them. Latch off, or whatever lets go of latched keys (transport
// stop), ends them: serviceStrum() sees to that too. (With the arp on the pads feed the arp, whose
// own latch keeps them.)
// Display: the sixteen pads as two rows of eight cells, the chord's root over its kind ("C#" over
// "m7"; "M" = major, a line under an inversion, "--" = empty). While keys are held the name bar
// names the chord and says what a pad will do; after a store it says where the chord went.
class ChordPadScreen : public Screen
{
public:
    using HeldKeys = std::function<std::vector<ChordPads::Note>()>;

    explicit ChordPadScreen(ChordPads& pads) : chords(pads) {}

    // The keys that count as held now (physically down or latched), with their velocities.
    void setHeldKeysProvider(HeldKeys p) { heldKeys = std::move(p); }
    // The pads play through these (the same path as the SCALE pads): note, velocity 1..127.
    void setNoteCallback(std::function<void(int, int)> cb) { noteOn = std::move(cb); }
    void setNoteOffCallback(std::function<void(int)> cb) { noteOff = std::move(cb); }
    void setAftertouchCallback(std::function<void(int, int)> cb) { pressure = std::move(cb); }
    // False while the pads' notes go somewhere that wants them together (the arpeggiator).
    void setStrumAllowed(std::function<bool()> f) { strumAllowed = std::move(f); }

    // The key latch: is it on, and a count that moves whenever what it holds is let go.
    void setLatchState(std::function<bool()> on, std::function<int()> releaseCount)
    {
        latchOn = std::move(on);
        latchReleases = std::move(releaseCount);
    }

    // Notes of a stroke whose time has come are started; latched pads are let go when the latch
    // is. Call under ScreenManager::getLock(), while strumIsWaiting().
    bool strumIsWaiting() const { return strumWaiting.load() || anyLatched.load(); }
    void serviceStrum()
    {
        if (anyLatched.load() && (!latchIsOn() || (latchReleases && latchReleases() != latchSeen)))
            releaseLatched();
        const auto now = UiClock::nowMs();
        for (size_t i = 0; i < pending.size();)
        {
            if (pending[i].dueMs > now) { ++i; continue; }
            const auto p = pending[i];
            pending.erase(pending.begin() + (long) i);
            sounding[(size_t) p.pad].push_back(p.pitch);
            if (noteOn) noteOn(p.pitch, p.velocity);
        }
        strumWaiting = !pending.empty();
    }
    juce::Random& getRandom() { return random; }

    juce::String getName() const override { return "CHORD"; }

    juce::String getSubPage() const override
    {
        if (flash.isNotEmpty() && UiClock::nowMs() - flashMs < FLASH_MS)
            return flash;
        const auto held = held_();
        if (!held.empty())
            return ChordPads::nameOf(held.data(), (int) held.size()).full() + " > PAD";
        if (page == 1)
            return chords.strumMs > 0.0f ? "STRUM " + juce::String((int) std::lround(chords.strumMs)) : juce::String("STRUM OFF");
        juce::String sub = "B" + juce::String(chords.getBank() + 1);
        if (chords.transpose != 0) sub += " T" + juce::String(chords.transpose > 0 ? "+" : "") + juce::String(chords.transpose);
        if (chords.octave != 0)    sub += " O" + juce::String(chords.octave > 0 ? "+" : "") + juce::String(chords.octave);
        return sub;
    }

    void onDeactivate() override
    {
        // (A pinned screen keeps getting its pads; one that is left lets go of what it plays.)
        confirm.cancel();
    }

    // ---- pads ------------------------------------------------------------------------------------
    void onPadPress(int padIndex, int velocity) override
    {
        if (padIndex < 0 || padIndex >= ChordPads::PADS)
            return;
        confirm.cancel();

        // Keys held: the pad takes their chord. Nothing is played (the keys are sounding).
        auto held = held_();
        if (!held.empty())
        {
            UndoPoint point;
            point.scope = UndoPoint::Scope::Custom;
            point.verb = "STORE";
            const juce::String pad = "PAD " + juce::String(padIndex + 1);
            point.names = { "CHORD " + pad, pad, "CHORD" };
            point.capture = [this] { return chords.toFlat(); };
            point.apply = [this](const std::vector<float>& v) { releaseAll(); chords.fromFlat(v); };
            {
                const UndoMark mark(*this, point);
                chords.store(padIndex, std::move(held));
            }
            say(pad + " = " + ChordPads::nameOf(chords.get(padIndex)).full());
            return;
        }

        const float padVelocity = (float) juce::jlimit(1, 127, velocity) / 127.0f;
        const bool strummed = chords.strumMs > 0.0f && (!strumAllowed || strumAllowed());
        std::vector<ChordPads::Stroke> notes;
        if (strummed)
            notes = chords.strum(padIndex, padVelocity, random, altDown);
        else
            for (auto& n : chords.sounding(padIndex, padVelocity))
                notes.push_back({ n.pitch, n.velocity, 0.0f });
        if (notes.empty())
        {
            say("EMPTY: HOLD KEYS");
            return;
        }
        // Latched pads: a pad pressed with no other pad down is a new chord.
        bool anotherDown = false;
        for (int i = 0; i < ChordPads::PADS; ++i)
            anotherDown = anotherDown || (padDown[(size_t) i] && i != padIndex);
        if (!anotherDown)
            releaseLatched();
        releasePad(padIndex);           // (a pad pressed twice without a release between)
        padDown[(size_t) padIndex] = true;
        auto& playing = sounding[(size_t) padIndex];
        const auto now = UiClock::nowMs();
        for (auto& n : notes)
        {
            const int level = juce::jlimit(1, 127, (int) std::lround(n.velocity * 127.0f));
            const auto wait = (juce::int64) std::lround(n.delayMs);
            if (wait > 0)
            {
                pending.push_back({ padIndex, n.pitch, level, now + wait });
                continue;
            }
            playing.push_back(n.pitch);
            if (noteOn) noteOn(n.pitch, level);
        }
        strumWaiting = !pending.empty();
    }

    void onPadAftertouch(int padIndex, int value) override
    {
        if (padIndex < 0 || padIndex >= ChordPads::PADS || !pressure)
            return;
        for (int pitch : sounding[(size_t) padIndex])
            pressure(pitch, value);
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex < 0 || padIndex >= ChordPads::PADS)
            return;
        const bool wasPlaying = padDown[(size_t) padIndex];
        padDown[(size_t) padIndex] = false;
        const bool toArp = strumAllowed && !strumAllowed();     // (the arp has the notes: its own latch decides)
        if (wasPlaying && !toArp && latchIsOn() && (!sounding[(size_t) padIndex].empty() || strumWaiting.load()))
        {
            latched[(size_t) padIndex] = true;      // it sounds on, and its stroke is played out
            latchSeen = latchReleases ? latchReleases() : 0;
            anyLatched = true;
            return;
        }
        releasePad(padIndex);
    }

    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        const bool keysHeld = !held_().empty();
        for (int i = 0; i < ChordPads::PADS; ++i)
        {
            const auto& c = chords.get(i);
            PadColour colour;
            if (!sounding[(size_t) i].empty())  colour = { 127, 127, 127 };
            else if (c.empty())                 colour = keysHeld ? PadColour { 45, 45, 45 } : PadColour { 5, 5, 5 };
            else                                colour = rootColour(c.notes[0].pitch, ChordPads::nameOf(c));
            out[(size_t) i] = colour;
        }
        return true;
    }

    // ---- knobs -----------------------------------------------------------------------------------
    bool usesEncoderBar() const override { return true; }

    juce::String getEncoderName(int encoderIndex) const override
    {
        static const char* const names[] = { "BK", "TR", "OC", "VL", "", "", "", "CL" };
        static const char* const strum[] = { "TM", "DR", "HU", "VR", "FL", "", "", "" };
        return (encoderIndex >= 0 && encoderIndex < 8) ? (page == 1 ? strum : names)[encoderIndex] : "";
    }

    juce::String getEncoderReadout(int encoderIndex) const override
    {
        if (page == 1)
        {
            auto percent = [](float v) { return juce::String((int) std::lround(v * 100.0f)) + "%"; };
            static const char* const dirs[] = { "UP", "DOWN", "ALTERNATE" };
            switch (encoderIndex)
            {
                case 0: return chords.strumMs > 0.0f ? "STRUM " + juce::String((int) std::lround(chords.strumMs)) + " MS"
                                                     : juce::String("STRUM OFF");
                case 1: return "DIRECTION " + juce::String(dirs[juce::jlimit(0, 2, chords.strumDir)]);
                case 2: return "HUMAN TIME " + percent(chords.strumHuman);
                case 3: return "VARY PATTERN " + percent(chords.strumVary);
                case 4: return "FALL " + percent(chords.strumFall);
                default: return {};
            }
        }
        switch (encoderIndex)
        {
            case 0: return "BANK " + juce::String(chords.getBank() + 1);
            case 1: return "TRANSPOSE " + juce::String(chords.transpose > 0 ? "+" : "") + juce::String(chords.transpose);
            case 2: return "OCTAVE " + juce::String(chords.octave > 0 ? "+" : "") + juce::String(chords.octave);
            case 3: return chords.velocityMode == ChordPads::VEL_KEYS ? "VELOCITY AS KEYS"
                         : chords.velocityMode == ChordPads::VEL_BALANCE ? "VELOCITY BALANCE" : "VELOCITY PAD";
            case 7: return "CLEAR BANK" + (confirm.knob == 7 ? " " + confirm.progress() : juce::String());
            default: return {};
        }
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        const int clicks = dsp::encoderDelta(value);
        if (clicks == 0)
            return;
        if (encoderIndex != 7 || page == 1)
            confirm.cancel();
        if (page == 1)
        {
            const ParamSpec time { "TM", 0, 0, ChordPads::MAX_STRUM_MS, 0, ParamKind::Linear, "ms" };
            const ParamSpec unit { "", 0, 0, 1, 0, ParamKind::Linear };
            switch (encoderIndex)
            {
                case 0: chords.strumMs = std::round(time.turned(chords.strumMs, clicks, turnStep())); break;
                case 1: chords.strumDir = juce::jlimit(0, ChordPads::NUM_STRUM_DIRS - 1, chords.strumDir + clicks); break;
                case 2: chords.strumHuman = unit.turned(chords.strumHuman, clicks, turnStep()); break;
                case 3: chords.strumVary = unit.turned(chords.strumVary, clicks, turnStep()); break;
                case 4: chords.strumFall = unit.turned(chords.strumFall, clicks, turnStep()); break;
                default: break;
            }
            return;
        }
        switch (encoderIndex)
        {
            case 0: setBank(chords.getBank() + clicks); break;
            case 1:
            {
                const ParamSpec spec { "TR", 0, -24, 24, 0, ParamKind::Int, "st" };
                chords.transpose = (int) spec.turned((float) chords.transpose, clicks, turnStep());
                break;
            }
            case 2: chords.octave = juce::jlimit(-3, 3, chords.octave + clicks); break;
            case 3: chords.velocityMode = juce::jlimit(0, ChordPads::NUM_VELOCITY_MODES - 1, chords.velocityMode + clicks); break;
            case 7:
                // Four clicks clockwise empty this bank (an undo step).
                if (!confirm.step(7, clicks))
                    break;
                {
                    UndoPoint point;
                    point.scope = UndoPoint::Scope::Custom;
                    point.verb = "CLEAR";
                    const juce::String bank = "BANK " + juce::String(chords.getBank() + 1);
                    point.names = { "CHORD " + bank, bank, "CHORD" };
                    point.capture = [this] { return chords.toFlat(); };
                    point.apply = [this](const std::vector<float>& v) { releaseAll(); chords.fromFlat(v); };
                    const UndoMark mark(*this, point);
                    releaseAll();
                    chords.clearBank(chords.getBank());
                }
                say("BANK CLEARED");
                break;
            default: break;
        }
    }

    // Up / Down: the bank, round and round.
    void onPadPagePrev() override { setBank((chords.getBank() + ChordPads::NUM_BANKS - 1) % ChordPads::NUM_BANKS); }
    void onPadPageNext() override { setBank((chords.getBank() + 1) % ChordPads::NUM_BANKS); }
    void getHelpFacts(HelpFacts& facts) const override { facts.tapUpDown = "CHORD BANK"; }
    // The bank buttons: the pads' page and the STRUM page.
    int getPageCount() const override { return 2; }
    int getPageIndex() const override { return page; }
    void onArrowLeft() override  { confirm.cancel(); page = 1 - page; }
    void onArrowRight() override { confirm.cancel(); page = 1 - page; }

    // ---- picture ---------------------------------------------------------------------------------
    void render(DisplayRenderer& display) override
    {
        for (int pad = 0; pad < ChordPads::PADS; ++pad)
        {
            const int x = (pad % 8) * ScreenLayout::COLUMN_W;
            const int y = ScreenLayout::CONTENT_Y + (pad / 8) * 21;
            auto centred = [&](const juce::String& text, int ty)
            {
                display.drawString(x + (ScreenLayout::COLUMN_W - text.length() * 6) / 2 + 1, ty, text);
            };
            const auto& c = chords.get(pad);
            if (c.empty())
            {
                centred("--", y + 5);
            }
            else
            {
                const auto name = ChordPads::nameOf(c);
                centred(name.root.substring(0, 2), y + 1);
                centred(name.cell(), y + 9);
                if (name.bass.isNotEmpty())                 // an inversion
                    display.drawHLine(x + 2, y + 18, 12);
            }
            if (!sounding[(size_t) pad].empty())
                display.invertRegion(x, y, ScreenLayout::COLUMN_W, 21);
        }
    }

    // What a pad is sounding (tests).
    const std::vector<int>& getSounding(int pad) const { return sounding[(size_t) juce::jlimit(0, 15, pad)]; }

private:
    static constexpr juce::int64 FLASH_MS = 1500;

    std::vector<ChordPads::Note> held_() const { return heldKeys ? heldKeys() : std::vector<ChordPads::Note>(); }

    void say(const juce::String& text) { flash = text; flashMs = UiClock::nowMs(); }

    void setBank(int b)
    {
        releaseAll();
        chords.setBank(b);
    }

    void releasePad(int pad)
    {
        auto& playing = sounding[(size_t) pad];
        for (int pitch : playing)
            if (noteOff) noteOff(pitch);
        playing.clear();
        latched[(size_t) pad] = false;
        padDown[(size_t) pad] = false;
        // What its stroke had not reached yet is not played.
        pending.erase(std::remove_if(pending.begin(), pending.end(), [pad](const Pending& p) { return p.pad == pad; }),
                      pending.end());
        strumWaiting = !pending.empty();
    }

    void releaseAll()
    {
        for (int i = 0; i < ChordPads::PADS; ++i)
            releasePad(i);
        anyLatched = false;
    }

    bool latchIsOn() const { return latchOn && latchOn(); }
    void releaseLatched()
    {
        for (int i = 0; i < ChordPads::PADS; ++i)
            if (latched[(size_t) i])
                releasePad(i);
        anyLatched = false;
    }

    // A pad's light by its chord's root, round the circle of fifths (neighbouring keys, neighbouring
    // colours), so a progression reads as colours.
    static PadColour rootColour(int lowestPitch, const ChordPads::Name& name)
    {
        int pc = lowestPitch % 12;
        for (int i = 0; i < 12; ++i)
            if (ChordPads::pitchClassName(i) == name.root) pc = i;
        static const PadColour wheel[12] = {
            { 110, 0, 0 },  { 110, 40, 0 },  { 100, 80, 0 },  { 50, 100, 0 },  { 0, 100, 0 },  { 0, 100, 50 },
            { 0, 90, 100 }, { 0, 40, 110 },  { 0, 0, 110 },   { 50, 0, 110 },  { 100, 0, 100 }, { 110, 0, 45 } };
        return wheel[(pc * 7) % 12];
    }

    ChordPads& chords;
    HeldKeys heldKeys;
    std::function<void(int, int)> noteOn, pressure;
    std::function<void(int)> noteOff;
    std::function<bool()> strumAllowed, latchOn;
    std::function<int()> latchReleases;
    std::array<bool, 16> padDown {}, latched {};    // a finger on the pad now; let go and kept by the latch
    std::atomic<bool> anyLatched { false };
    int latchSeen = 0;
    std::array<std::vector<int>, 16> sounding;
    struct Pending { int pad; int pitch; int velocity; juce::int64 dueMs; };
    std::vector<Pending> pending;           // the notes of strokes under way, not yet started
    std::atomic<bool> strumWaiting { false };
    juce::Random random;
    bool altDown = false;
    int page = 0;
    TurnConfirm confirm;
    juce::String flash;
    juce::int64 flashMs = 0;
};
