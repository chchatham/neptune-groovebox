#pragma once

#include "ParamPageScreen.h"
#include "../ScreenLayout.h"
#include <cstring>

// -----------------------------------------------------------------------------
// FLTR: the selected track's filter as a picture, for every engine that has one
// (unified voice, FM, macro, wavetable, wave sequencer, sampler) and every model.
//
//   y 11-52  the response curve, 20 Hz - 20 kHz, measured from the real filter
//            (FilterCurve); the unified voice's two filters are shown combined as routed.
//            Top left: type and mode (and the routing when there are two filters).
//            For a second after ATK, DEC, SUS, REL or CURV is turned (F ENV page) the picture is
//            the filter envelope instead (EnvelopeCurve).
//   bottom   the eight knobs (encoder bar). Page 1 is always
//                CUT  RESO TYPE MODE | the other four cells of the engine's FILTR page
//            and the engine's further filter pages follow as they are (FLT B, F ENV).
//
// The cells are the engine's own, so everything works as on PARAM (ParamCellScreen): fine and
// coarse turns, values stored with the sound, locks by holding a step, pressure ranges. The
// name bar shows the name and value of the knob just turned (getEncoderReadout).
// An engine without a filter (drums, MIDI) gets a line saying so.
// -----------------------------------------------------------------------------
class FilterViewScreen : public ParamCellScreen
{
public:
    FilterViewScreen() : ParamCellScreen(nullptr, nullptr) {}
    FilterViewScreen(SequencerCore& seq, AudioEngine* audio) : ParamCellScreen(&seq, audio) {}

    // Bind the view to the app: it then follows the selected track.
    void setAudio(SequencerCore* seq, AudioEngine* audio) { rebind(seq, audio); }

    juce::String getName() const override { return "FLTR"; }

    bool hasFilter() const { return ready(); }

    juce::String getSubPage() const override
    {
        if (sequencer == nullptr) return {};
        if (!ready()) return "NO FILTER";
        return ParamCellScreen::getSubPage();
    }

    bool usesEncoderBar() const override { return true; }
    EncoderLayout getEncoderLayout() const override { return EncoderLayout::Bar; }

    // Two characters fit a 16-pixel column of the bar.
    juce::String getEncoderName(int encoderIndex) const override
    {
        if (!ready() || encoderIndex < 0 || encoderIndex >= 8)
            return {};
        const ParamPage page = resolvedPage();
        const auto& spec = page.params[(size_t) encoderIndex];
        return spec.isValid() ? shortName(spec.label) : juce::String();
    }

    // "CUT 1.2k" in the name bar after a turn: the knob's own label and value, a held step's lock
    // when there is one.
    juce::String getEncoderReadout(int encoderIndex) const override { return getCellReadout(encoderIndex); }

    void render(DisplayRenderer& display) override
    {
        if (!ready())
        {
            if (sequencer != nullptr)
            {
                display.drawString(8, 22, "THIS ENGINE HAS NO");
                display.drawString(8, 32, "FILTER");
            }
            return;
        }

        FilterCurve::Settings settings;
        const ParamPage page = resolvedPage();
        EnvelopeCurve::Settings envelope;
        if (isEnvelopePictureActive() && getEnvelopeSettings(envelope))
        {
            // Just after an envelope knob was turned (F ENV): the filter envelope instead of the
            // response curve, for the same second.
            EnvelopeCurve::draw(display, 0, ScreenLayout::CONTENT_Y + 9, DisplayRenderer::WIDTH, ScreenLayout::CONTENT_H - 9,
                                EnvelopeCurve::shape(envelope));
            display.drawString(1, ScreenLayout::CONTENT_Y + 1, "FILTER ENVELOPE");
        }
        else if (auto* heights = filterPictureHeights(page, settings))
        {
            FilterCurve::draw(display, 0, ScreenLayout::CONTENT_Y, DisplayRenderer::WIDTH, ScreenLayout::CONTENT_H,
                              *heights, settings.cutoffHz[0]);

            // Type and mode, and the routing when the second filter is in use.
            juce::String text = modelText(settings, 0);
            if (settings.routing != FilterRouting::Single)
                text << (settings.routing == FilterRouting::Series ? " > " : " + ") << modelText(settings, 1);
            const int w = text.length() * 6 + 1;
            for (int y = ScreenLayout::CONTENT_Y; y < ScreenLayout::CONTENT_Y + 9; ++y)
                for (int x = 0; x < w; ++x)
                    display.setPixel(x, y, false);
            display.drawString(1, ScreenLayout::CONTENT_Y + 1, text);
        }

        // Lock marks: a dot above the bar label of every cell locked on the held (or playing) step.
        auto& track = selectedTrack();
        const Step* markStep = nullptr;
        if (isStepHeld())                 markStep = &track.getStep(getHeldStepIndex());
        else if (sequencer->isPlaying())  markStep = &track.getStep(sequencer->getCurrentStep(sequencer->getSelectedTrack()));
        if (markStep != nullptr)
            for (int i = 0; i < 8; ++i)
                if (page.params[(size_t) i].isValid() && markStep->paramLocks.count(page.params[(size_t) i].id) > 0)
                    display.drawRect(i * ScreenLayout::COLUMN_W + 7, ScreenLayout::CONTENT_BOTTOM - 1, 2, 2, true);
    }

    // The filter as the picture shows it (for tests).
    bool getFilterSettings(FilterCurve::Settings& out) const
    {
        return ready() && filterPictureSettings(resolvedPage(), out);
    }

protected:
    // The engine's filter pages, the first one rearranged so the four main cells lead.
    void buildPages(ParamPages& out) const override
    {
        ParamPages enginePages;
        if (auto* e = engine())                  enginePages = e->getParamPages();
        else if (audioEngine == nullptr)         enginePages = UnifiedVoiceEngine::basePages();   // previews, tests
        if (!FilterCurve::bind(enginePages).valid)
            return;

        bool first = true;
        for (auto& page : enginePages)
        {
            if (page.section != "FLTR")
                continue;
            if (first && page.find(filterCellId(page, "CUT")) != nullptr)
            {
                ParamPage lead = page;
                int n = 0;
                for (const char* label : { "CUT", "RESO", "TYPE", "MODE" })
                    for (auto& p : page.params)
                        if (p.isValid() && std::strcmp(p.label, label) == 0 && n < 8)
                            lead.params[(size_t) n++] = p;
                for (auto& p : page.params)
                {
                    bool placed = false;
                    for (int i = 0; i < n; ++i)
                        placed = placed || lead.params[(size_t) i].id == p.id;
                    if (p.isValid() && !placed && n < 8)
                        lead.params[(size_t) n++] = p;
                }
                while (n < 8)
                    lead.params[(size_t) n++] = {};
                out.push_back(lead);
            }
            else
                out.push_back(page);
            first = false;
        }
    }

    // Every FLTR page shows the picture, whatever the page's cells are.
    bool filterPictureSettings(const ParamPage& page, FilterCurve::Settings& out) const override
    {
        juce::ignoreUnused(page);
        if (!filterBinding.valid)
            return false;
        out = FilterCurve::read(filterBinding, [this](int id) { return shownValueOfId(id); });
        return true;
    }

    bool trackSharesParameter(const ParamSpec& spec, int selected, int other) const override
    {
        juce::ignoreUnused(spec);
        return engineTypeOf(other) == engineTypeOf(selected);
    }

private:
    static int filterCellId(const ParamPage& page, const char* label)
    {
        for (auto& p : page.params)
            if (p.isValid() && std::strcmp(p.label, label) == 0)
                return p.id;
        return -1;
    }

    static juce::String modelText(const FilterCurve::Settings& s, int slot)
    {
        const auto model = s.model[slot];
        juce::String text = FilterSlot::modelNames()[FilterSlot::indexFromModel(model)];
        if (model != FilterModel::Bypass)
            text << " " << FilterSlot::modeNames(model)[juce::jlimit(0, FilterSlot::modeCount(model) - 1, s.mode[slot])];
        return text;
    }

    static juce::String shortName(const char* label)
    {
        static const char* const table[][2] = {
            { "CUT", "CT" },  { "RESO", "RS" }, { "TYPE", "TY" }, { "MODE", "MD" }, { "ENV", "EN" },  { "DRIV", "DR" },
            { "MORP", "MO" }, { "FFM", "FM" },  { "KTRK", "KT" }, { "VELF", "VL" }, { "ENVF", "EN" }, { "FDRV", "DR" },
            { "ATK", "AT" },  { "DEC", "DC" },  { "SUS", "SU" },  { "REL", "RL" },  { "CURV", "CV" }, { "EVEL", "EV" },
            { "VEL", "VL" },  { "WHL", "WH" },  { "KEY", "KY" },  { "PMOD", "PM" }, { "TYPB", "TB" }, { "MODB", "MB" },
            { "OFSB", "OF" }, { "RESB", "RB" }, { "ROUT", "RT" }, { "BAL", "BL" },  { "DRVB", "DB" }, { "TRKB", "TK" } };
        for (auto& row : table)
            if (std::strcmp(row[0], label) == 0)
                return row[1];
        return juce::String(label).substring(0, 2);
    }
};
