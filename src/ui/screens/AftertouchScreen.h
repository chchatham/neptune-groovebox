#pragma once

#include "../TurnNotice.h"
#include "../ScreenManager.h"
#include "../ParamGrid.h"
#include "../../engine/AftertouchRouter.h"
#include "../../engine/AudioEngine.h"
#include "../../dsp/DSPUtils.h"
#include <functional>

// -----------------------------------------------------------------------------
// AT ROUTE: where pad pressure goes on the selected track. One column per route:
//   top row    (encoders 1-4)  DEST  destination of route 1-4
//   bottom row (encoders 5-8)  DPTH  depth of route 1-4 (negative = pressure lowers it)
// Every track has its own routes. Only the destinations the track's engine
// responds to are offered, named as on that engine's pages; a drum kit has one
// route (it goes to every pad), a sampler or MIDI track has none.
// Pads play the selected track (a chromatic octave from middle C) so the routing
// can be heard while it is set; the strip at the bottom shows the pressure.
// -----------------------------------------------------------------------------
class AftertouchScreen : public Screen
{
public:
    // Per track: edits the routes of the track `selectedTrack` returns.
    AftertouchScreen(AudioEngine& audioEngine, std::function<int()> selectedTrack)
        : audio(&audioEngine), trackProvider(std::move(selectedTrack)) {}

    // Legacy: one router for the whole instrument (AudioEngine::setAftertouchRouter copies its
    // changes to every track). Call setTrackSource to make the screen per track.
    explicit AftertouchScreen(AftertouchRouter& router)
        : legacy(&router)
    {
        // All four routes exist; unused ones are simply OFF.
        for (int i = router.getRouteCount(); i < AftertouchRouter::MAX_ROUTES; ++i)
            router.setRoute(i, { AftertouchRouter::Destination::Off, 0.5f, false });
        router.setRouteCount(AftertouchRouter::MAX_ROUTES);
    }

    void setTrackSource(AudioEngine* audioEngine, std::function<int()> selectedTrack)
    {
        audio = audioEngine;
        trackProvider = std::move(selectedTrack);
    }

    bool isPerTrack() const { return audio != nullptr && trackProvider != nullptr; }
    int getTrack() const { return isPerTrack() ? trackProvider() : 0; }

    juce::String getName() const override { return "AT ROUTE"; }
    juce::String getSubPage() const override { return notice.showing() ? notice.shown() : juce::String("PRESSURE"); }
    bool usesEncoderBar() const override { return false; }
    EncoderLayout getEncoderLayout() const override { return EncoderLayout::Grid; }

    // ---- What the selected track offers ----------------------------------------------------------
    // Number of routes (0 = the engine has no pressure destinations).
    int getRouteSlots() const
    {
        return isPerTrack() ? AudioEngine::pressureRouteCount(audio->getEngineTypeOf(getTrack()))
                            : AftertouchRouter::MAX_ROUTES;
    }

    // The choices of a DEST cell: OFF first, then the engine's destinations.
    int getDestCount() const { return destList(nullptr, nullptr); }
    AftertouchRouter::Destination getDestAt(int index) const
    {
        AftertouchRouter::Destination dests[MAX_CHOICES];
        const int n = destList(dests, nullptr);
        return (index >= 0 && index < n) ? dests[index] : AftertouchRouter::Destination::Off;
    }
    juce::String getDestLabel(int index) const
    {
        const char* names[MAX_CHOICES];
        const int n = destList(nullptr, names);
        return (index >= 0 && index < n) ? juce::String(names[index]) : juce::String();
    }

    AftertouchRouter::Route getRoute(int slot) const { return router().effectiveRoute(slot); }

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

        for (int slot = 0; slot < AftertouchRouter::MAX_ROUTES; ++slot)
        {
            if (slot >= slots)
            {
                ParamGrid::drawEmptyCell(display, slot);
                ParamGrid::drawEmptyCell(display, 4 + slot);
                continue;
            }
            auto route = getRoute(slot);
            auto d = destSpec();
            auto a = depthSpec();
            ParamGrid::drawCell(display, slot, d, (float) destIndex(route.dest), d.label);
            if (route.dest == AftertouchRouter::Destination::Off)
                ParamGrid::drawEmptyCell(display, 4 + slot);
            else
                ParamGrid::drawCell(display, 4 + slot, a, route.depth, a.label);
        }

        ParamGrid::drawStripMeter(display, "P", previewPressure);
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        if (encoderIndex < 0 || encoderIndex >= 8)
            return;
        const int clicks = dsp::encoderDelta(value);
        const int slot = encoderIndex % 4;
        if (clicks == 0 || slot >= getRouteSlots())
            return;

        auto route = getRoute(slot);
        selectedSlot = slot;

        if (encoderIndex < 4)
        {
            const int index = (int) destSpec().turned((float) destIndex(route.dest), clicks, turnStep());
            route.dest = getDestAt(index);
            if (route.dest != AftertouchRouter::Destination::Off && route.depth == 0.0f)
                route.depth = 0.5f;
        }
        else
        {
            if (route.dest == AftertouchRouter::Destination::Off)
            {
                notice.say("ROUTE OFF");        // a depth belongs to a destination: choose one first
                return;
            }
            route.depth = depthSpec().turned(route.depth, clicks, turnStep());
        }
        route.bipolar = false;

        if (isPerTrack()) audio->setPressureRoute(getTrack(), slot, route);
        else              legacy->setRoute(slot, route);
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
        return encoderIndex < 4 ? "DEST" : "DPTH";
    }

    juce::String getEncoderReadout(int encoderIndex) const override
    {
        if (encoderIndex < 0 || encoderIndex >= 8 || encoderIndex % 4 >= getRouteSlots()) return {};
        const int slot = encoderIndex % 4;
        const auto route = getRoute(slot);
        if (encoderIndex < 4)
            return "DEST " + juce::String(slot + 1) + " " + getDestLabel(destIndex(route.dest));
        if (route.dest == AftertouchRouter::Destination::Off) return {};
        return "DEPTH " + juce::String(slot + 1) + " " + depthSpec().format(route.depth);
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
    static constexpr int MAX_CHOICES = 12;

    const AftertouchRouter& router() const { return isPerTrack() ? audio->getPressureRouter(getTrack()) : *legacy; }

    // Fills the choices of the DEST cells (either array may be null); returns how many.
    int destList(AftertouchRouter::Destination* dests, const char** names) const
    {
        using D = AftertouchRouter::Destination;
        static const AudioEngine::PressureDestInfo all[] = {
            { D::Volume, "VOL" }, { D::FilterCutoff, "CUT" }, { D::FMDepth, "FM" }, { D::MacroTimbre, "TIMB" },
            { D::WavePosition, "WAVE" }, { D::Vibrato, "VIB" } };
        const AudioEngine::PressureDestInfo* list = all;
        int n = 6;
        if (isPerTrack())
            n = AudioEngine::pressureDestinations(audio->getEngineTypeOf(getTrack()), list);
        if (dests) dests[0] = D::Off;
        if (names) names[0] = "OFF";
        for (int i = 0; i < n && i + 1 < MAX_CHOICES; ++i)
        {
            if (dests) dests[i + 1] = list[i].dest;
            if (names) names[i + 1] = list[i].label;
        }
        return juce::jmin(n + 1, MAX_CHOICES);
    }

    int destIndex(AftertouchRouter::Destination d) const
    {
        AftertouchRouter::Destination dests[MAX_CHOICES];
        const int n = destList(dests, nullptr);
        for (int i = 0; i < n; ++i)
            if (dests[i] == d) return i;
        return 0;
    }

    ParamSpec destSpec() const
    {
        const int n = destList(nullptr, choiceNames.data());
        return { "DEST", 0, 0, (float) (n - 1), 0, ParamKind::Choice, "", choiceNames.data(), n };
    }

    // A drum voice takes a depth of 0..1; everything else can also be lowered by pressure.
    ParamSpec depthSpec() const
    {
        const bool drum = isPerTrack() && audio->getEngineTypeOf(getTrack()) == EngineType::Drum;
        return { "DPTH", 4, drum ? 0.0f : -1.0f, 1, 1, ParamKind::Linear };
    }

    AudioEngine* audio = nullptr;
    std::function<int()> trackProvider;
    AftertouchRouter* legacy = nullptr;
    mutable std::array<const char*, MAX_CHOICES> choiceNames {};
    int selectedSlot = 0;
    float previewPressure = 0.0f;
    std::array<bool, 16> padDown {};
    std::function<void(int, int)> noteOn;
    std::function<void(int)> noteOff;
    std::function<void(int, int)> noteAftertouch;
};
