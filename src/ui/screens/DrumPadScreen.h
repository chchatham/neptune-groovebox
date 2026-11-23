#pragma once

#include "../ScreenManager.h"
#include "../TurnNotice.h"
#include "../../engine/DrumEngine.h"
#include <cstring>
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// DRUMS: the 16 pads play and select the 16 drum voices; the 8 encoders edit the
// selected voice.
//
//   y 11-31  the pads as a 2 x 8 map (kit position, voice type; selected inverted)
//   y 33-51  eight bars, one per encoder column
//   bottom   encoder labels (the standard encoder bar)
// The name bar shows the selected pad and its voice type (on a further page: the page).
// Pads: one colour per kind of voice.
//
// Knob pages (the bank buttons, going round): the selected pad's own pages, the same ones PARAM
// shows for it (DrumEngine::padPages), eight knobs each:
//   DR     TN DC TO DV SN LV PN TY     tune, decay, tone, drive, snap, level, pan, type
//   KICK   SW ST SC SH KN KT HD AC     a kick: the kick machine's first page
//   KCK2   KD LN RS LS LK SA ED PA     a kick: its second
//   RSN    SN MR XM EN DP WR MT DR     a resonator drum
//   BANK   MD PG CL EX DP BL CP LF     an oscillator bank
//   PLFO   RT DP SH DS TG SY DV FD     the pad's own LFO
//   PSLT   S1 D1 A1 V1 S2 D2 A2 V2     the pad's two own slots
// Another pad keeps the page where it has one of that kind (a kick's KICK page becomes the
// sound page of a resonator drum or a bank), else its DR page.
// -----------------------------------------------------------------------------
class DrumPadScreen : public Screen
{
public:
    DrumPadScreen(DrumEngine* engine) : drumEngine(engine) {}

    juce::String getName() const override { return "DRUMS"; }

    // "P9 KSYN": the pad and its type; on a further page the page's name ("P9 KCK2"). Seven
    // characters are what the name bar has beside "DRUMS T1" and the page mark, so a pad with two
    // digits goes without its P ("16 KCK2").
    juce::String getSubPage() const override
    {
        auto* e = engine();
        if (!e || selectedVoice < 0) return "NO DRUM TRK";
        if (notice.showing())
            return notice.shown();
        const juce::String word = getPageIndex() > 0
                                      ? currentPage().name
                                      : juce::String(DrumEngine::typeNames()[juce::jlimit(0, DrumEngine::NUM_DRUM_TYPES - 1,
                                                                                          (int) e->getVoice(selectedVoice).getType())]);
        const juce::String pad(selectedVoice + 1);
        const juce::String text = "P" + pad + " " + word;
        return text.length() <= STATUS_ROOM ? text : pad + " " + word;
    }
    static constexpr int STATUS_ROOM = 7;

    // A cell's two letters on the bar and its name in the readout, by its label on the page.
    struct CellName { const char* label; const char* two; const char* name; };
    static const CellName* cellName(const char* label)
    {
        static const CellName names[] = {
            { "TUNE", "TN", "TUNE" },       { "DEC",  "DC", "DECAY" },      { "TONE", "TO", "TONE" },       { "DRIV", "DV", "DRIVE" },
            { "SNAP", "SN", "SNAP" },       { "LEVL", "LV", "LEVEL" },      { "PAN",  "PN", "PAN" },        { "TYPE", "TY", "TYPE" },
            { "SWEP", "SW", "SWEEP" },      { "SWPT", "ST", "SWP TIME" },   { "SCRV", "SC", "SWP CURVE" },  { "SHAP", "SH", "SHAPE" },
            { "KNCK", "KN", "KNOCK" },      { "KTON", "KT", "KNOCK TONE" }, { "HOLD", "HD", "HOLD" },       { "ACRV", "AC", "DEC CURVE" },
            { "KIND", "KD", "KIND" },       { "LIN",  "LN", "LIN SWEEP" },  { "RES",  "RS", "TONE RESO" },  { "LPSW", "LS", "TONE SWP" },
            { "LPKT", "LK", "TONE TRACK" }, { "SAT",  "SA", "SATURATE" },   { "EDGE", "ED", "EDGE" },       { "PATH", "PA", "STAGES" },
            { "MODL", "MD", "MODEL" },      { "PING", "PG", "PING" },       { "COLR", "CL", "COLOUR" },     { "EXC",  "EX", "EXCITER" },
            { "DAMP", "DP", "DAMPING" },    { "BLOM", "BL", "BLOOM" },      { "COUP", "CP", "COUPLING" },   { "LOFI", "LF", "LO-FI" },
            { "SND",  "SN", "SOUND" },      { "MRPH", "MR", "MORPH" },      { "XMPH", "XM", "EXC MRPH" },   { "ENRG", "EN", "ENERGY" },
            { "WIRE", "WR", "WIRES" },      { "MEET", "MT", "BOW MEETS" },  { "DIR",  "DR", "DIRECT" },
            { "RATE", "RT", "LFO RATE" },   { "DPTH", "DP", "LFO DEPTH" },  { "SHPE", "SH", "LFO SHAPE" },  { "DEST", "DS", "LFO DEST" },
            { "TRIG", "TG", "LFO TRIG" },   { "SYNC", "SY", "LFO SYNC" },   { "DIV",  "DV", "LFO DIV" },    { "FADE", "FD", "LFO FADE" },
            { "SRC1", "S1", "SRC1" },       { "DST1", "D1", "DST1" },       { "AMT1", "A1", "AMT1" },       { "VIA1", "V1", "VIA1" },
            { "SRC2", "S2", "SRC2" },       { "DST2", "D2", "DST2" },       { "AMT2", "A2", "AMT2" },       { "VIA2", "V2", "VIA2" } };
        for (auto& n : names)
            if (std::strcmp(n.label, label) == 0)
                return &n;
        return nullptr;
    }

    // Full name and value of knob i, for the name bar ("KNOCK TONE 40"). The name bar has
    // READOUT_ROOM characters for it beside "DRUMS"; where the full name would push the value out,
    // the cell's own label stands for it ("KTON 40"): the value is what the turn is about.
    static constexpr int READOUT_ROOM = 14;
    juce::String getEncoderReadout(int encoderIndex) const override
    {
        auto* e = engine();
        if (!e || selectedVoice < 0 || encoderIndex < 0 || encoderIndex >= 8) return {};
        if (notice.showing())
            return notice.shown();
        const auto spec = currentPage().params[(size_t) encoderIndex];
        if (!spec.isValid()) return {};
        const juce::String value = spec.format(e->getParameter(spec.id));
        const auto* n = cellName(spec.label);
        const juce::String full = juce::String(n != nullptr ? n->name : spec.label) + " " + value;
        return full.length() <= READOUT_ROOM ? full : juce::String(spec.label) + " " + value;
    }

    // ---- The selected pad's pages ----------------------------------------------------------------
    enum class PadPage { Voice, Kick, Sound, Lfo, Slot };
    std::vector<PadPage> pageKinds() const
    {
        std::vector<PadPage> kinds { PadPage::Voice };
        auto* e = engine();
        if (e != nullptr && selectedVoice >= 0)
        {
            if (e->isKickMachine(selectedVoice)) kinds.push_back(PadPage::Kick);
            if (e->hasSoundPage(selectedVoice))  kinds.push_back(PadPage::Sound);
            kinds.push_back(PadPage::Lfo);
            kinds.push_back(PadPage::Slot);
        }
        return kinds;
    }
    ParamPage currentPage() const
    {
        auto* e = engine();
        if (e == nullptr || selectedVoice < 0) return DrumEngine::voicePage(juce::jmax(0, selectedVoice));
        const auto pages = e->padPages(selectedVoice);
        return pages[(size_t) juce::jlimit(0, (int) pages.size() - 1, getPageIndex())];
    }
    int getPageCount() const override { return (int) pageKinds().size(); }
    int getPageIndex() const override
    {
        const auto kinds = pageKinds();
        for (int i = 0; i < (int) kinds.size(); ++i)
            if (kinds[(size_t) i] == wantedPage)
                return i;
        // (A kick's first page on a pad that is none: that pad's sound page, where it has one.)
        if (wantedPage == PadPage::Kick)
            for (int i = 0; i < (int) kinds.size(); ++i)
                if (kinds[(size_t) i] == PadPage::Sound)
                    return i;
        return 0;
    }
    void onArrowLeft() override  { stepPage(-1); }
    void onArrowRight() override { stepPage(1); }

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
                if (!spec.isValid()) continue;
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
        if (encoderIndex < 0 || encoderIndex >= 8 || selectedVoice < 0 || !engine()) return {};
        const auto spec = currentPage().params[(size_t) encoderIndex];
        if (!spec.isValid()) return {};
        const auto* n = cellName(spec.label);
        return n != nullptr ? juce::String(n->two) : juce::String(spec.label).substring(0, 2);
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
        if (!spec.isValid() || clicks == 0) return;
        // A depth or an amount whose destination is OFF moves nothing: say so (as PARAM does).
        if (const char* why = e->padModIdleReason(spec.id))
        {
            notice.say(why);
            return;
        }
        notice.clear();
        const float next = spec.turned(e->getParameter(spec.id), clicks, turnStep());

        if (paramSetter) paramSetter(spec.id, next);
        else             e->setParameter(spec.id, next);
    }

    void onActivate() override
    {
        if (auto* e = engine()) e->setSelectedVoice(selectedVoice);
    }
    void onDeactivate() override { padDown.fill(false); notice.clear(); }

    TurnNotice notice;

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
            case DrumSynthType::OscBank:       return { 0, 110, 127 };    // the oscillator bank (hats, metal): cyan
            case DrumSynthType::Resonator:     return { 70, 0, 127 };     // the resonator drum: violet
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
    void stepPage(int by)
    {
        const auto kinds = pageKinds();
        const int n = (int) kinds.size();
        wantedPage = kinds[(size_t) ((getPageIndex() + by + n) % n)];
        notice.clear();
    }

    int selectedVoice = 8;
    PadPage wantedPage = PadPage::Voice;    // the knob page, as a kind: a pad without it shows another (getPageIndex)
    std::array<bool, 16> padDown {};
    float lastVelocity[DrumEngine::NUM_VOICES] = {};
};
