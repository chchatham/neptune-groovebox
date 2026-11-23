#pragma once

#include "../TurnNotice.h"
#include "../ScreenManager.h"
#include "../ParamGrid.h"
#include "../../engine/AudioEngine.h"
#include "../../dsp/DSPUtils.h"
#include <functional>

// -----------------------------------------------------------------------------
// AT ROUTE: where pad pressure goes on the selected track. A route is a pressure
// slot of the track's modulation router (ModRouter.h: THE PRESSURE SLOTS): its
// source is the pad of each voice (polyphonic aftertouch reaches the voices one
// by one), and its three cells are parameters of the track's sound (ids 773-784,
// set through setTrackParameter: a preset, a project and an undo step carry them).
//
//   page 1  top row    (knobs 1-4)  DEST  where route 1-4 goes
//           bottom row (knobs 5-8)  DPTH  how far (negative: pressure lowers it)
//   page 2  top row    (knobs 1-4)  BIP   the pad read both ways: half pressure
//                                         is the middle, less lowers, more raises
//
// DEST offers: OFF; what only pressure reaches on this engine (EXC: the
// resonator's exciter, BOW: the macro oscillator's bow; VIB: a vibrato brought
// in by the pad); then everything an LFO of the engine can move; then the cells
// of the effects on the track. Func + turn goes from group to group.
//
// A drum kit has one route (it goes to every pad), a MIDI track none (its
// pressure is sent on as poly pressure). Pads play the selected track so the
// routing is heard while it is set; the strip at the bottom shows the pressure.
// -----------------------------------------------------------------------------
class AftertouchScreen : public Screen
{
public:
    // Per track: edits the routes of the track `selectedTrack` returns.
    AftertouchScreen(AudioEngine& audioEngine, std::function<int()> selectedTrack)
        : audio(&audioEngine), trackProvider(std::move(selectedTrack)) {}

    // Where a value that was set is also kept (the track's stored parameters: Track::soundParams).
    void setStore(std::function<void(int track, int id, float value)> fn) { store = std::move(fn); }

    int getTrack() const { return trackProvider(); }

    static constexpr int PAGE_ROUTES = 0, PAGE_BIPOLAR = 1, NUM_PAGES = 2;

    juce::String getName() const override { return "AT ROUTE"; }
    juce::String getSubPage() const override
    {
        // (Four characters are what the name bar has beside "AT ROUTE T1" and the page mark: the
        // longer "PRESSURE" and "BIPOLAR" were shown as their ends, "SURE" and "OLAR".)
        if (notice.showing()) return notice.shown();
        if (getRouteSlots() == 0) return {};            // (a MIDI track: the picture says what happens)
        return getPageIndex() == PAGE_BIPOLAR ? "BIP" : "DEST";
    }
    bool usesEncoderBar() const override { return false; }
    EncoderLayout getEncoderLayout() const override { return EncoderLayout::Grid; }

    int getPageCount() const override { return getRouteSlots() > 0 ? (int) NUM_PAGES : 1; }
    int getPageIndex() const override { return getRouteSlots() > 0 ? page : 0; }
    // (The bank buttons go round: up from the first page is the last.)
    void onArrowLeft() override  { page = (getPageIndex() + getPageCount() - 1) % getPageCount(); }
    void onArrowRight() override { page = (getPageIndex() + 1) % getPageCount(); }

    // ---- What the selected track offers -----------------------------------------
    // How many routes (0: a MIDI track, or no engine).
    int getRouteSlots() const
    {
        return router() != nullptr ? AudioEngine::pressureSlotCount(audio->getEngineTypeOf(getTrack())) : 0;
    }

    // The DEST choices as the screen shows them (mod::DestTable::pressureMenu): OFF, what only
    // pressure reaches, the engine's table, the track's effect cells.
    int getDestCount() const { return router() != nullptr ? router()->dests().pressureChoices() : 0; }
    const char* const* getDestLabels() const { return router() != nullptr ? router()->dests().pressureMenu() : nullptr; }

    // A slot's cells as the track holds them (DEST: a value of the slot's list, not a place of the menu).
    float getSlotDest(int slot) const { return cell(slot, PressureSlotParam::DEST_OFFSET); }
    float getSlotAmount(int slot) const { return cell(slot, PressureSlotParam::AMT_OFFSET); }
    float getSlotBipolar(int slot) const { return cell(slot, PressureSlotParam::BIPOLAR_OFFSET); }
    // The label of a slot's destination as the display shows it ("OFF", "CUT", "VIB", "1:MIX").
    juce::String getSlotDestText(int slot) const
    {
        const auto* r = router();
        if (r == nullptr) return "OFF";
        const int value = destValue(slot);
        const int d = value - r->dests().pressureTrack();
        if (d >= 0)
            if (const auto text = audio->trackDestText(getTrack(), d); text.isNotEmpty())
                return text;
        return r->dests().pressureLabels()[juce::jlimit(0, r->dests().pressureChoices() - 1, value)];
    }

    void render(DisplayRenderer& display) override
    {
        const int slots = getRouteSlots();
        if (slots == 0)
        {
            const bool midi = audio->getEngineTypeOf(getTrack()) == EngineType::MIDITrack;
            display.drawString(4, 22, midi ? "MIDI TRACK: PRESSURE" : "THIS ENGINE HAS NO");
            display.drawString(4, 32, midi ? "IS SENT AS POLY AT" : "PRESSURE DESTINATION");
            return;
        }

        for (int slot = 0; slot < mod::Router::NUM_PRESSURE; ++slot)
        {
            const bool off = destValue(slot) == 0;
            if (slot >= slots)
            {
                ParamGrid::drawEmptyCell(display, slot);
                ParamGrid::drawEmptyCell(display, 4 + slot);
            }
            else if (getPageIndex() == PAGE_BIPOLAR)
            {
                const ParamSpec b = bipolarSpec(slot);
                if (off) ParamGrid::drawEmptyCell(display, slot);
                else     ParamGrid::drawCell(display, slot, b, getSlotBipolar(slot), b.label);
                ParamGrid::drawEmptyCell(display, 4 + slot);
            }
            else
            {
                const ParamSpec d = destSpec(slot);
                const ParamSpec a = depthSpec(slot);
                ParamGrid::drawCell(display, slot, d, (float) destPlace(slot), d.label, getSlotDestText(slot));
                if (off) ParamGrid::drawEmptyCell(display, 4 + slot);
                else     ParamGrid::drawCell(display, 4 + slot, a, getSlotAmount(slot), a.label);
            }
        }

        ParamGrid::drawStripMeter(display, "P", previewPressure);
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        if (encoderIndex < 0 || encoderIndex >= 8)
            return;
        const int clicks = dsp::encoderDelta(value);
        const int slot = encoderIndex % 4;
        const auto* r = router();
        if (clicks == 0 || slot >= getRouteSlots() || r == nullptr)
            return;
        const bool top = encoderIndex < 4;

        if (getPageIndex() == PAGE_BIPOLAR)
        {
            if (! top)
                return;                         // (no cell under the bottom knobs of this page)
            selectedSlot = slot;
            if (destValue(slot) == 0)
            {
                notice.say("ROUTE OFF");
                return;
            }
            if (r->pressureIsOwn(slot) && audio->getEngineTypeOf(getTrack()) == EngineType::Drum)
            {
                notice.say("KIT ONE WAY");      // a drum's own destinations take the pad as it is (11 characters: what the name bar holds)
                return;
            }
            const ParamSpec b = bipolarSpec(slot);
            set(b.id, b.turned(getSlotBipolar(slot) > 0.5f ? 1.0f : 0.0f, clicks, turnStep()));
            return;
        }

        selectedSlot = slot;
        if (top)
        {
            // DEST: through the menu (a place of it), stored as the value it stands for.
            const ParamSpec d = destSpec(slot);
            const int place = (int) std::lround(d.turned((float) destPlace(slot), clicks, turnStep()));
            const int next = r->dests().pressureValueAt(place);
            set(d.id, (float) next);
            // A destination chosen for a route whose depth is nothing: half of it, to be heard.
            if (next != 0 && getSlotAmount(slot) == 0.0f)
                set(PressureSlotParam::id(slot, PressureSlotParam::AMT_OFFSET), mod::DestTable::PRESSURE_AMOUNT);
        }
        else
        {
            if (destValue(slot) == 0)
            {
                notice.say("ROUTE OFF");        // a depth belongs to a destination: choose one first
                return;
            }
            const ParamSpec a = depthSpec(slot);
            set(a.id, a.turned(a.clamp(getSlotAmount(slot)), clicks, turnStep()));
        }
    }

    // Pads audition the routing on the selected track.
    void onPadPress(int padIndex, int velocity) override
    {
        previewPressure = 0.0f;
        if (padIndex >= 0 && padIndex < 16)
        {
            padDown[(size_t) padIndex] = true;
            if (noteOn) noteOn(BASE_NOTE + padIndex, velocity);
        }
    }

    void onPadAftertouch(int padIndex, int pressure) override
    {
        previewPressure = (float) pressure / 127.0f;
        if (noteAftertouch && padIndex >= 0 && padIndex < 16)
            noteAftertouch(BASE_NOTE + padIndex, pressure);
    }

    void onPadRelease(int padIndex) override
    {
        previewPressure = 0.0f;
        if (padIndex >= 0 && padIndex < 16)
        {
            padDown[(size_t) padIndex] = false;
            if (noteOff) noteOff(BASE_NOTE + padIndex);
        }
    }

    void onDeactivate() override { padDown.fill(false); previewPressure = 0.0f; notice.clear(); }

    TurnNotice notice;

    // All 16 pads play: the Cs marked, a held pad bright.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        for (int i = 0; i < 16; ++i)
        {
            const bool isC = (BASE_NOTE + i) % 12 == 0;
            PadColour c = isC ? PadColour { 100, 40, 0 } : PadColour { 30, 0, 40 };
            if (padDown[(size_t) i]) c = { 127, 127, 127 };
            out[(size_t) i] = c;
        }
        return true;
    }

    juce::String getEncoderName(int encoderIndex) const override
    {
        if (encoderIndex < 0 || encoderIndex >= 8) return "--";
        if (encoderIndex % 4 >= getRouteSlots()) return "---";
        if (getPageIndex() == PAGE_BIPOLAR)
            return encoderIndex < 4 ? "BIP" : "---";
        return encoderIndex < 4 ? "DEST" : "DPTH";
    }

    juce::String getEncoderReadout(int encoderIndex) const override
    {
        if (encoderIndex < 0 || encoderIndex >= 8 || encoderIndex % 4 >= getRouteSlots()) return {};
        const int slot = encoderIndex % 4;
        if (getPageIndex() == PAGE_BIPOLAR)
        {
            if (encoderIndex >= 4 || destValue(slot) == 0) return {};
            return "BIPOLAR " + juce::String(slot + 1) + (getSlotBipolar(slot) > 0.5f ? " ON" : " OFF");
        }
        if (encoderIndex < 4)
            return "DEST " + juce::String(slot + 1) + " " + getSlotDestText(slot);
        if (destValue(slot) == 0) return {};
        return "DEPTH " + juce::String(slot + 1) + " " + depthSpec(slot).format(getSlotAmount(slot));
    }

    void setNoteCallbacks(std::function<void(int, int)> on, std::function<void(int)> off,
                          std::function<void(int, int)> pressure)
    {
        noteOn = std::move(on);
        noteOff = std::move(off);
        noteAftertouch = std::move(pressure);
    }

    float getPreviewPressure() const { return previewPressure; }
    int getSelectedSlot() const { return selectedSlot; }

    static constexpr int BASE_NOTE = 60;

private:
    const mod::Router* router() const
    {
        const auto* e = audio->getEngine(getTrack());
        return e != nullptr ? e->modRouter() : nullptr;
    }
    float cell(int slot, int offset) const
    {
        return audio->getTrackParameter(getTrack(), PressureSlotParam::id(slot, offset));
    }
    int destValue(int slot) const { return (int) std::lround(getSlotDest(slot)); }
    int destPlace(int slot) const { return router() != nullptr ? router()->dests().pressurePlaceOf(destValue(slot)) : 0; }

    void set(int id, float value)
    {
        const int track = getTrack();
        audio->setTrackParameter(track, id, value);
        if (store)
            store(track, id, audio->getTrackParameter(track, id));
    }

    // DEST as the knob turns it: a place of the menu.
    ParamSpec destSpec(int slot) const
    {
        const int count = getDestCount();
        return { "DEST", PressureSlotParam::id(slot, PressureSlotParam::DEST_OFFSET), 0, (float) juce::jmax(0, count - 1), 0,
                 ParamKind::Choice, "", getDestLabels(), count };
    }
    // A drum's own destinations take a depth of 0..1; everything else can also be lowered by pressure.
    ParamSpec depthSpec(int slot) const
    {
        const auto* r = router();
        const bool oneWay = r != nullptr && r->pressureIsOwn(slot) && audio->getEngineTypeOf(getTrack()) == EngineType::Drum;
        return { "DPTH", PressureSlotParam::id(slot, PressureSlotParam::AMT_OFFSET),
                 oneWay ? 0.0f : -1.0f, 1, 1, ParamKind::Linear };
    }
    ParamSpec bipolarSpec(int slot) const
    {
        return { "BIP", PressureSlotParam::id(slot, PressureSlotParam::BIPOLAR_OFFSET), 0, 1, 0,
                 ParamKind::Choice, "", ParamChoices::offOn, 2 };
    }

    AudioEngine* audio;
    std::function<int()> trackProvider;
    std::function<void(int, int, float)> store;
    int page = PAGE_ROUTES;
    int selectedSlot = 0;
    float previewPressure = 0.0f;
    std::array<bool, 16> padDown {};
    std::function<void(int, int)> noteOn;
    std::function<void(int)> noteOff;
    std::function<void(int, int)> noteAftertouch;
};
