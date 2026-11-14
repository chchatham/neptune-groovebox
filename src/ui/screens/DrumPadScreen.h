#pragma once

#include "../ScreenManager.h"
#include "../../engine/DrumEngine.h"
#include <functional>

// -----------------------------------------------------------------------------
// DRUMS: the 16 pads play and select the 16 drum voices; the 8 encoders edit the
// selected voice.
//
//   y 11-31  the pads as a 2 x 8 map (kit position, voice type; selected inverted)
//   y 33-51  eight bars, one per encoder column: TUNE DEC TONE DRIV SNAP LEVL PAN TYPE
//   bottom   encoder labels (the standard encoder bar)
// The name bar shows the selected pad and its voice type. Pads: one colour per kind of voice.
// -----------------------------------------------------------------------------
class DrumPadScreen : public Screen
{
public:
    DrumPadScreen(DrumEngine* engine) : drumEngine(engine) {}

    juce::String getName() const override { return "DRUMS"; }

    juce::String getSubPage() const override
    {
        auto* e = engine();
        if (!e || selectedVoice < 0) return "NO DRUM TRK";
        return "P" + juce::String(selectedVoice + 1) + " "
             + DrumEngine::typeNames()[juce::jlimit(0, DrumEngine::NUM_DRUM_TYPES - 1,
                                                    (int) e->getVoice(selectedVoice).getType())];
    }

    // Full name and value of knob i, for the name bar.
    juce::String getEncoderReadout(int encoderIndex) const override
    {
        static const char* const names[] = { "TUNE", "DECAY", "TONE", "DRIVE", "SNAP", "LEVEL", "PAN", "TYPE" };
        static const char* const kickNames[] = { "SWEEP", "SWEEP TIME", "SWEEP CURVE", "SHAPE", "KNOCK", "KNOCK TONE", "HOLD", "DECAY CURVE" };
        static const char* const cymbalNames[] = { "MODEL", "PING", "COLOUR", "NOISE", "DAMPING", "BLOOM", "VEL SENS", "LO-FI" };
        auto* e = engine();
        if (!e || selectedVoice < 0 || encoderIndex < 0 || encoderIndex >= 8) return {};
        auto spec = currentPage().params[(size_t) encoderIndex];
        return juce::String(onKickPage() ? kickNames[encoderIndex] : onCymbalPage() ? cymbalNames[encoderIndex] : names[encoderIndex])
             + " " + spec.format(e->getParameter(spec.id));
    }

    // A pad whose TYPE is KSYN (the kick machine) has a second knob page: sweep, shape, knock.
    // So has one whose TYPE is CSYN (the cymbal machine): model, ping, colour, bloom.
    bool hasKickPage() const
    {
        auto* e = engine();
        return e != nullptr && selectedVoice >= 0 && e->isKickMachine(selectedVoice);
    }
    bool hasCymbalPage() const
    {
        auto* e = engine();
        return e != nullptr && selectedVoice >= 0 && e->isCymbalMachine(selectedVoice);
    }
    bool hasMachinePage() const { return hasKickPage() || hasCymbalPage(); }
    bool onKickPage() const { return kickPage && hasKickPage(); }
    bool onCymbalPage() const { return kickPage && hasCymbalPage(); }
    ParamPage currentPage() const
    {
        return onKickPage() ? DrumEngine::kickPage(selectedVoice)
             : onCymbalPage() ? DrumEngine::cymbalPage(selectedVoice) : DrumEngine::voicePage(selectedVoice);
    }
    int getPageCount() const override { return hasMachinePage() ? 2 : 1; }
    int getPageIndex() const override { return onKickPage() || onCymbalPage() ? 1 : 0; }
    void onArrowLeft() override  { if (hasMachinePage()) kickPage = !kickPage; }
    void onArrowRight() override { if (hasMachinePage()) kickPage = !kickPage; }

    void render(DisplayRenderer& display) override
    {
        auto* e = engine();
        if (!e)
        {
            display.drawString(8, 28, "NO DRUM TRACK");
            return;
        }

        // Pad map: two rows of eight, 16 x 10 each.
        for (int v = 0; v < DrumEngine::NUM_VOICES; ++v)
        {
            int x = (v % 8) * 16;
            int y = (v / 8 == 0) ? PAD_ROW1_Y : PAD_ROW2_Y;
            display.drawString(x + 2, y + 1, DrumEngine::drumTypeAbbrev(e->getVoice(v).getType()));
            if (e->getVoice(v).isActive())
                display.drawRect(x + 13, y + 1, 2, 2, true);
        }
        if (selectedVoice >= 0 && selectedVoice < DrumEngine::NUM_VOICES)
            display.invertRegion((selectedVoice % 8) * 16, (selectedVoice / 8 == 0) ? PAD_ROW1_Y : PAD_ROW2_Y, 16, 10);

        // The selected voice's eight parameters, one bar above each encoder.
        if (selectedVoice >= 0)
        {
            auto page = currentPage();
            for (int i = 0; i < 8; ++i)
            {
                auto& spec = page.params[(size_t) i];
                int x = i * 16;
                display.drawRect(x + 3, BAR_Y, 10, BAR_H, false);
                int fill = (int) std::lround(spec.toNorm(e->getParameter(spec.id)) * (float) (BAR_H - 2));
                if (fill > 0)
                    display.drawRect(x + 4, BAR_Y + BAR_H - 1 - fill, 8, fill, true);
            }
        }
    }

    juce::String getEncoderName(int encoderIndex) const override
    {
        static const char* names[] = { "TN", "DC", "TO", "DV", "SN", "LV", "PN", "TY" };
        static const char* kickNames[] = { "SW", "ST", "SC", "SH", "KN", "KT", "HD", "AC" };
        static const char* cymbalNames[] = { "MD", "PG", "CL", "NS", "DP", "BL", "VS", "LF" };
        if (encoderIndex < 0 || encoderIndex >= 8 || selectedVoice < 0 || !engine()) return {};
        return onKickPage() ? kickNames[encoderIndex] : onCymbalPage() ? cymbalNames[encoderIndex] : names[encoderIndex];
    }

    void onPadPress(int padIndex, int velocity) override
    {
        auto* e = engine();
        if (!e || padIndex < 0 || padIndex >= DrumEngine::NUM_VOICES) return;

        selectPad(padIndex);
        padDown[(size_t) padIndex] = true;
        lastVelocity[padIndex] = velocity / 127.0f;
        if (padHooks.down) padHooks.down(padIndex, velocity / 127.0f);
        else               e->noteOn(padIndex, velocity / 127.0f);
    }

    void onPadAftertouch(int padIndex, int pressure) override
    {
        auto* e = engine();
        if (e && padIndex >= 0 && padIndex < DrumEngine::NUM_VOICES)
        {
            e->aftertouch(padIndex, (float) pressure / 127.0f);
            if (padHooks.pressure) padHooks.pressure(padIndex, (float) pressure / 127.0f);
        }
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex >= 0 && padIndex < DrumEngine::NUM_VOICES)
            padDown[(size_t) padIndex] = false;
        auto* e = engine();
        if (e && padIndex >= 0 && padIndex < DrumEngine::NUM_VOICES)
        {
            if (padHooks.up) padHooks.up(padIndex);
            else             e->noteOff(padIndex);
        }
    }

    // Plain: one step; Shift: fine; Func: coarse / snap.
    void onEncoderChange(int encoderIndex, int value) override
    {
        auto* e = engine();
        if (!e || selectedVoice < 0 || encoderIndex < 0 || encoderIndex >= 8) return;

        auto spec = currentPage().params[(size_t) encoderIndex];
        const int clicks = (value > 64) ? value - 128 : value;
        const float next = spec.turned(e->getParameter(spec.id), clicks, turnStep());

        if (paramSetter) paramSetter(spec.id, next);
        else             e->setParameter(spec.id, next);
    }

    void onActivate() override
    {
        if (auto* e = engine()) e->setSelectedVoice(selectedVoice);
    }
    void onDeactivate() override { padDown.fill(false); }

    // Pads: one colour per kind of voice, dim; the selected pad at full brightness; a pad that is
    // held or sounding white.
    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        auto* e = engine();
        for (int i = 0; i < 16; ++i)
        {
            PadColour c;
            if (e != nullptr && i < DrumEngine::NUM_VOICES)
            {
                const PadColour full = typeColour(e->getVoice(i).getType());
                if (padDown[(size_t) i] || e->getVoice(i).isActive())
                    c = { 127, 127, 127 };
                else if (i == selectedVoice)
                    c = full;
                else
                    c = { (juce::uint8) (full.r / 5), (juce::uint8) (full.g / 5), (juce::uint8) (full.b / 5) };
            }
            out[(size_t) i] = c;
        }
        return true;
    }

    static PadColour typeColour(DrumSynthType t)
    {
        switch (t)
        {
            case DrumSynthType::FMKick:
            case DrumSynthType::AnalogKick:
            case DrumSynthType::ModCyclesKick:
            case DrumSynthType::KickSynth:     return { 127, 0, 0 };      // kicks: red
            case DrumSynthType::NoiseSnare:
            case DrumSynthType::FMSnare:       return { 127, 60, 0 };     // snares: orange
            case DrumSynthType::Clap:          return { 127, 0, 90 };     // clap: pink
            case DrumSynthType::ClosedHat:
            case DrumSynthType::OpenHat:       return { 0, 110, 127 };    // hats: cyan
            case DrumSynthType::Cymbal:
            case DrumSynthType::CymbalSynth:   return { 0, 30, 127 };     // cymbals: blue
            case DrumSynthType::Tom:           return { 70, 0, 127 };     // tom: violet
            case DrumSynthType::Rim:
            case DrumSynthType::Shaker:
            case DrumSynthType::Clave:         return { 0, 127, 20 };     // small percussion: green
            case DrumSynthType::SubBass:       return { 127, 110, 0 };    // sub: yellow
        }
        return { 60, 60, 60 };
    }

    // When set, pad presses go to the app (which runs them through the pad ratchet) instead of
    // straight to the engine.
    struct PadHooks
    {
        std::function<void(int pad, float velocity)> down;
        std::function<void(int pad, float pressure)> pressure;
        std::function<void(int pad)> up;
    };
    void setPadHooks(PadHooks h) { padHooks = std::move(h); }

    void setDrumEngine(DrumEngine* e) { drumEngine = e; }
    // The screen follows whichever drum track this returns (selected track first); may return null.
    void setEngineProvider(std::function<DrumEngine*()> provider) { engineProvider = std::move(provider); }
    // Route edits through the app (stores the value on the track as well as applying it).
    void setParamSetter(std::function<void(int, float)> setter) { paramSetter = std::move(setter); }
    // The selected pad (0-15): the one the knobs edit and the PARAM screen's DR page shows. It is
    // given to the drum engine (DrumEngine::setSelectedVoice) and to onPadSelected when it changes.
    int getSelectedVoice() const { return selectedVoice; }
    int getSelectedPad() const { return selectedVoice; }
    void selectPad(int pad)
    {
        if (pad < 0 || pad >= DrumEngine::NUM_VOICES) return;
        const bool changed = pad != selectedVoice;
        selectedVoice = pad;
        if (auto* e = engine()) e->setSelectedVoice(pad);
        if (changed && onPadSelected) onPadSelected(pad);
    }
    std::function<void(int)> onPadSelected;

private:
    static constexpr int PAD_ROW1_Y = 11;
    static constexpr int PAD_ROW2_Y = 21;
    static constexpr int BAR_Y = 34;
    static constexpr int BAR_H = 17;

    DrumEngine* engine() const { return engineProvider ? engineProvider() : drumEngine; }
    DrumEngine* drumEngine = nullptr;
    std::function<DrumEngine*()> engineProvider;
    std::function<void(int, float)> paramSetter;
    PadHooks padHooks;
    int selectedVoice = 8;
    bool kickPage = false;          // the second knob page, while the selected pad is a kick or cymbal machine
    std::array<bool, 16> padDown {};
    float lastVelocity[DrumEngine::NUM_VOICES] = {};
};
