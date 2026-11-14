#pragma once
#include <array>
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
// Knobs: BK bank, TR transpose (semitones), OC octave, VL each note at its stored level or all at
// the pad's, CL clear this bank (four clicks).
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

    juce::String getName() const override { return "CHORD"; }

    juce::String getSubPage() const override
    {
        if (flash.isNotEmpty() && UiClock::nowMs() - flashMs < FLASH_MS)
            return flash;
        const auto held = held_();
        if (!held.empty())
            return ChordPads::nameOf(held.data(), (int) held.size()).full() + " > PAD";
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

        const auto notes = chords.sounding(padIndex, (float) juce::jlimit(1, 127, velocity) / 127.0f);
        if (notes.empty())
        {
            say("EMPTY: HOLD KEYS");
            return;
        }
        releasePad(padIndex);           // (a pad pressed twice without a release between)
        auto& playing = sounding[(size_t) padIndex];
        for (auto& n : notes)
        {
            playing.push_back(n.pitch);
            if (noteOn) noteOn(n.pitch, juce::jlimit(1, 127, (int) std::lround(n.velocity * 127.0f)));
        }
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
        if (padIndex >= 0 && padIndex < ChordPads::PADS)
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
        return (encoderIndex >= 0 && encoderIndex < 8) ? names[encoderIndex] : "";
    }

    juce::String getEncoderReadout(int encoderIndex) const override
    {
        switch (encoderIndex)
        {
            case 0: return "BANK " + juce::String(chords.getBank() + 1);
            case 1: return "TRANSPOSE " + juce::String(chords.transpose > 0 ? "+" : "") + juce::String(chords.transpose);
            case 2: return "OCTAVE " + juce::String(chords.octave > 0 ? "+" : "") + juce::String(chords.octave);
            case 3: return chords.playedVelocity ? "VELOCITY AS PLAYED" : "VELOCITY PAD";
            case 7: return "CLEAR BANK" + (confirm.knob == 7 ? " " + confirm.progress() : juce::String());
            default: return {};
        }
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        const int clicks = dsp::encoderDelta(value);
        if (clicks == 0)
            return;
        if (encoderIndex != 7)
            confirm.cancel();
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
            case 3: chords.playedVelocity = clicks > 0; break;
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
    int getPageCount() const override { return 1; }

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
    }

    void releaseAll()
    {
        for (int i = 0; i < ChordPads::PADS; ++i)
            releasePad(i);
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
    std::array<std::vector<int>, 16> sounding;
    TurnConfirm confirm;
    juce::String flash;
    juce::int64 flashMs = 0;
};
