#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "../GridScreen.h"
#include "../TurnNotice.h"
#include "../UiClock.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/PerfFx.h"
#include "../../dsp/DSPUtils.h"

// PERF: effects played from the pads (src/engine/PerfFx.h). Every pad holds up to four effects
// (layers A - D) that one press plays together, each on what it is set to (SEL = the track
// selected at the press, MSTR = the whole mix, or a track by number). The press itself and the
// pad's pressure each move any of an effect's values, by an amount set per value.
// The pad touched last is the one on the knobs (as on DRUMS); holding it and turning changes it
// while it is heard. Up / Down choose which of its layers the knobs show (the strip at the bottom
// shows the four: a letter where a layer holds an effect, the shown one inverted).
// The pads can be pinned like those of DRUMS, SCALE and CHORD.
//
// Knob pages (the first three keep one layout: knob 4 is WET, knobs 5 - 8 the effect's own four):
//   FX     TRK target, FX the effect, MODE punch / throw, WET, the effect's four values.
//          (An effect in time with the tempo shows its TIME or RATE as the division.)
//   PRESS  what a press does. LTCH: a press switches the pad on, the next off. QNT: a press waits
//          for the grid while the sequencer plays. Knobs 4 - 8: how far the press moves WET and
//          each value for as long as the pad is down (-100 .. +100 of its range).
//   PRES   what pressure does. CURV: how it answers. Knobs 4 - 8: how far full pressure moves WET
//          and each value.
//   MORE   SYNC and DIV lock the layer's effect to the tempo; knobs 3-6 are the effect's cells beyond its
//          four (the dub echo's SAT); CLR empties the pad (four clicks).
// LTCH, QNT and CURV are the pad's; everything else is the shown layer's.
// Copy / clear / paste act on the shown pad, all its layers. FX, CLR, clear and paste are undo steps.
// Pad lights: the colour of the pad's first effect; dim = waiting, full = down, half = ringing.
class PerfFxScreen : public GridScreen
{
public:
    enum PageIndex { PAGE_FX = 0, PAGE_PRESS, PAGE_PRESSURE, PAGE_MORE };

    PerfFxScreen(AudioEngine& engine, std::function<int()> selectedTrackFn, std::function<int()> trackCountFn)
        : rack(engine.getPerfFx()), selectedTrack(std::move(selectedTrackFn)), trackCount(std::move(trackCountFn))
    {
        rebuild();
    }

    juce::String getName() const override { return "PERF"; }

    // What a press of the shown layer does ("DUB>MSTR"), or what the page's knobs are.
    juce::String getSubPage() const override
    {
        if (notice.showing())
            return notice.shown();
        switch (gridPageIndex)
        {
            case PAGE_PRESS:    return "PRESS";
            case PAGE_PRESSURE: return "PRESSURE";
            case PAGE_MORE:     return "MORE";
            default: break;
        }
        const auto& l = rack.get(shown, layer);
        if (l.kind == PerfFx::NONE)
            return juce::String::charToString((juce::juce_wchar) ('A' + layer)) + " EMPTY";
        return juce::String(AudioEngine::perfKindName(l.kind)) + ">" + targetName(l.target);
    }

    int getShownLayer() const { return layer; }

    // ---- pads ------------------------------------------------------------------------------------
    void onPadPress(int padIndex, int velocity) override
    {
        juce::ignoreUnused(velocity);
        if (padIndex < 0 || padIndex >= PerfFx::PADS)
            return;
        confirm.cancel();
        if (padIndex != shown)
        {
            // Another pad on the knobs: its first effect.
            shown = padIndex;
            layer = rack.get(padIndex).firstLayer();
            rebuild();
        }
        if (rack.get(padIndex).empty())
        {
            notice.say("EMPTY: TURN FX");
            return;
        }
        rack.press(padIndex, selectedTrack ? selectedTrack() : 0);
    }

    void onPadAftertouch(int padIndex, int value) override
    {
        if (padIndex >= 0 && padIndex < PerfFx::PADS)
            rack.setPressure(padIndex, (float) value / 127.0f);
    }

    void onPadRelease(int padIndex) override
    {
        if (padIndex >= 0 && padIndex < PerfFx::PADS)
            rack.release(padIndex);
    }

    bool getPadColours(std::array<PadColour, 16>& out) const override
    {
        for (int i = 0; i < PerfFx::PADS; ++i)
        {
            const auto& pad = rack.get(i);
            if (pad.empty())
            {
                out[(size_t) i] = i == shown ? PadColour { 14, 14, 14 } : PadColour { 3, 3, 3 };
                continue;
            }
            const PadColour full = kindColour(pad.layer[(size_t) pad.firstLayer()].kind);
            const int share = rack.isDown(i) ? 100 : (rack.isSounding(i) ? 45 : (i == shown ? 22 : 12));
            out[(size_t) i] = { (juce::uint8) (full.r * share / 100), (juce::uint8) (full.g * share / 100),
                                (juce::uint8) (full.b * share / 100) };
        }
        return true;
    }

    // Up / Down: the layer on the knobs, round and round.
    void onPadPagePrev() override { setLayer((layer + PerfFx::LAYERS - 1) % PerfFx::LAYERS); }
    void onPadPageNext() override { setLayer((layer + 1) % PerfFx::LAYERS); }
    void getHelpFacts(HelpFacts& facts) const override
    {
        GridScreen::getHelpFacts(facts);
        facts.tapUpDown = "PERF LAYER";
    }

    // ---- knobs -----------------------------------------------------------------------------------
    void render(DisplayRenderer& display) override
    {
        refresh();
        GridScreen::render(display);
    }

    void onEncoderChange(int encoderIndex, int value) override
    {
        refresh();
        const int clicks = dsp::encoderDelta(value);
        if (clicks == 0 || encoderIndex < 0 || encoderIndex >= 8)
            return;
        const bool clearKnob = gridPageIndex == PAGE_MORE && encoderIndex == 7;
        if (! clearKnob)
            confirm.cancel();
        if (clearKnob)
        {
            if (rack.get(shown).empty())
            {
                notice.say("PAD IS EMPTY");
                return;
            }
            if (! confirm.step(7, clicks))
                return;
            {
                const UndoMark mark(*this, undoPoint("CLEAR"));
                rack.clear(shown);
            }
            layer = 0;
            rebuild();
            notice.say("PAD CLEARED");
            return;
        }
        GridScreen::onEncoderChange(encoderIndex, value);
    }

    juce::String getEncoderReadout(int encoderIndex) const override
    {
        if (gridPageIndex == PAGE_MORE && encoderIndex == 7 && confirm.knob == 7)
            return "CLEAR PAD " + confirm.progress();
        return {};
    }

    void onArrowLeft() override  { confirm.cancel(); GridScreen::onArrowLeft(); }
    void onArrowRight() override { confirm.cancel(); GridScreen::onArrowRight(); }
    void onDeactivate() override { confirm.cancel(); }

    // ---- copy / clear / paste: the shown pad, all its layers -----------------------------------
    bool getEditScope(EditScope& scope) const override
    {
        const auto point = undoPoint({});
        scope.names = point.names;
        scope.canPaste = clipFilled;
        scope.capture = point.capture;
        scope.apply = point.apply;
        return true;
    }
    bool onCopy() override  { clip = rack.get(shown); clipFilled = true; return true; }
    bool onClear() override { rack.clear(shown); layer = 0; rebuild(); return true; }
    bool onPaste() override
    {
        if (clipFilled) { rack.set(shown, clip); layer = rack.get(shown).firstLayer(); rebuild(); }
        return true;
    }

    static juce::String targetName(int target)
    {
        if (target == PerfFx::SEL)    return "SEL";
        if (target == PerfFx::MASTER) return "MSTR";
        return "T" + juce::String(target - PerfFx::TRACK_BASE + 1);
    }

    // A colour per kind of effect: echoes amber, reverbs blue, the freeze violet, repeats green, the
    // stop red, movement (phaser, flanger, chorus ...) cyan, filters and EQ yellow, dirt magenta.
    static PadColour kindColour(int kind)
    {
        switch (kind)
        {
            case PerfFx::DUB:  return { 127, 60, 0 };
            case PerfFx::FRZ:  return { 70, 20, 127 };
            case PerfFx::RPT:  return { 0, 127, 20 };
            case PerfFx::STOP: return { 127, 0, 0 };
            default: break;
        }
        switch ((EffectType) (kind - PerfFx::INSERT_BASE))
        {
            case EffectType::DelayEffect: case EffectType::ReverseDelayEffect: case EffectType::TapeDelayEffect:
            case EffectType::LCRDelayEffect: case EffectType::MultiTapEffect:
                return { 127, 90, 0 };
            case EffectType::ReverbEffect: case EffectType::HallReverbEffect: case EffectType::SpringReverbEffect:
            case EffectType::ShimmerReverbEffect:
                return { 0, 30, 127 };
            case EffectType::ChorusEffect: case EffectType::FlangerEffect: case EffectType::PhaserEffect:
            case EffectType::TremoloEffect: case EffectType::EnsembleEffect: case EffectType::AutoPanEffect:
            case EffectType::RotaryEffect: case EffectType::VibratoEffect:
                return { 0, 110, 110 };
            case EffectType::FilterEffectType: case EffectType::EQEffect: case EffectType::WahEffect:
            case EffectType::GraphicEQEffect:
                return { 120, 110, 0 };
            case EffectType::DriveEffect: case EffectType::WaveshaperEffect: case EffectType::RateReducerEffect:
            case EffectType::RingModEffect: case EffectType::AmpEffect:
                return { 127, 0, 90 };
            default: break;
        }
        return { 90, 90, 90 };
    }

protected:
    // The strip: the pad's four layers at the left (a letter where a layer holds an effect, a dash
    // where it is empty, the shown one inverted), the page marks in the middle.
    void drawStrip(DisplayRenderer& display) override
    {
        GridScreen::drawStrip(display);
        const auto& pad = rack.get(shown);
        for (int l = 0; l < PerfFx::LAYERS; ++l)
        {
            const int x = 1 + l * 9;
            if (pad.layer[(size_t) l].kind != PerfFx::NONE || l == layer)
                display.drawString(x, ParamGrid::STRIP_Y - 1, juce::String::charToString((juce::juce_wchar) ('A' + l)));
            else
                display.drawHLine(x + 1, ParamGrid::STRIP_Y + 3, 3);
            if (l == layer)
                display.invertRegion(x - 1, ParamGrid::STRIP_Y - 1, 7, 8);
        }
    }

private:
    void setLayer(int l)
    {
        confirm.cancel();
        layer = juce::jlimit(0, PerfFx::LAYERS - 1, l);
        rebuild();
    }

    // The pages follow the shown pad and layer and what is on it (also after a project load or an undo).
    void refresh()
    {
        const auto& l = rack.get(shown, layer);
        const int count = trackCount ? trackCount() : 8;
        if (l.kind != builtKind || l.sync != builtSync || count != builtTracks)
            rebuild();
    }

    UndoPoint undoPoint(const juce::String& verb, int coalesceKey = 0) const
    {
        UndoPoint point;
        point.scope = UndoPoint::Scope::Custom;
        point.verb = verb;
        const juce::String pad = "PAD " + juce::String(shown + 1);
        point.names = { "PERF " + pad, pad, "PAD" };
        point.coalesceKey = coalesceKey;
        point.capture = [this] { return rack.toFlat(); };
        point.apply = [this](const std::vector<float>& v) { rack.fromFlat(v); };
        return point;
    }

    // The shown layer holds an effect; where it does not, a knob that needs one says so.
    bool hasEffect()
    {
        if (rack.get(shown, layer).kind != PerfFx::NONE)
            return true;
        notice.say("NO EFFECT");
        return false;
    }

    // A change to the shown layer's effect / to the shown pad.
    void change(const std::function<void(PerfFx::Layer&)>& edit)
    {
        if (! hasEffect())
            return;
        PerfFx::Layer l = rack.get(shown, layer);
        edit(l);
        rack.setLayer(shown, layer, l);
    }
    void changePad(const std::function<void(PerfFx::Setting&)>& edit)
    {
        PerfFx::Setting p = rack.get(shown);
        edit(p);
        rack.set(shown, p);
    }

    void rebuild()
    {
        const auto& now = rack.get(shown, layer);
        builtKind = now.kind;
        builtSync = now.sync;
        builtTracks = juce::jlimit(1, MAX_TARGET_TRACKS, trackCount ? trackCount() : 8);
        const int page = gridPageIndex;
        gridPages.clear();

        auto lay = [this]() -> const PerfFx::Layer& { return rack.get(shown, layer); };
        auto pad = [this]() -> const PerfFx::Setting& { return rack.get(shown); };
        using K = ParamKind;

        // ---- FX ---------------------------------------------------------------------------------
        Page first;
        first.name = "FX";
        first.cells[0] = cell({ "TRK", 0, 0, (float) (PerfFx::TRACK_BASE + builtTracks - 1), 0, K::Choice, "", targetNames(),
                                PerfFx::TRACK_BASE + builtTracks },
                              [lay] { return (float) lay().target; },
                              [this](float v)
                              {
                                  // (An empty layer can be given its target before its effect.)
                                  PerfFx::Layer l = rack.get(shown, layer);
                                  l.target = (int) std::lround(v);
                                  rack.setLayer(shown, layer, l);
                              });

        const auto& kinds = AudioEngine::perfKinds();
        first.cells[1] = cell({ "FX", 1, 0, (float) (kinds.size() - 1), 0, K::Choice, "", kindNames(), (int) kinds.size() },
                              [lay, &kinds]
                              {
                                  for (size_t i = 0; i < kinds.size(); ++i)
                                      if (kinds[i] == lay().kind) return (float) i;
                                  return 0.0f;
                              },
                              [this, &kinds](float v)
                              {
                                  const int kind = kinds[(size_t) juce::jlimit(0, (int) kinds.size() - 1, (int) std::lround(v))];
                                  if (kind == rack.get(shown, layer).kind) return;
                                  // (Turning on through the list is one undo step.)
                                  const UndoMark mark(*this, undoPoint("FX", 0x50460000 + shown * PerfFx::LAYERS + layer));
                                  rack.setKind(shown, layer, kind);
                              });

        static const char* const modes[] = { "PNCH", "THRW" };
        first.cells[2] = cell({ "MODE", 2, 0, 1, 0, K::Choice, "", modes, 2 },
                              [lay] { return (float) lay().mode; },
                              [this](float v)
                              {
                                  if (! hasEffect()) return;
                                  const auto w = rack.waysOf(rack.get(shown, layer).kind);
                                  if (w.punch && w.thrown) change([v](PerfFx::Layer& l) { l.mode = (int) std::lround(v); });
                                  else                     notice.say(w.thrown ? "THROW ONLY" : "PUNCH ONLY");
                              });
        first.cells[3] = cell({ "WET", 3, 0, 1, 1 },
                              [lay] { return lay().wet; },
                              [this](float v) { change([v](PerfFx::Layer& l) { l.wet = v; }); });

        // ---- PRESS, PRES: an amount under each value's own knob ---------------------------------
        Page press, pressure;
        press.name = "PRESS";
        pressure.name = "PRES";
        auto amounts = [this, lay](Page& p, bool ofPressure, int value, int knob, const char* label)
        {
            // (One click is one hundredth of the value's range, as the cell shows it: -100 .. +100.)
            p.cells[(size_t) knob] = cell({ label, knob, -1, 1, 0, ParamKind::Linear, "", nullptr, 0, 0.01f },
                [lay, ofPressure, value] { return (ofPressure ? lay().pressure : lay().press)[(size_t) value]; },
                [this, ofPressure, value](float v)
                {
                    change([=](PerfFx::Layer& l) { (ofPressure ? l.pressure : l.press)[(size_t) value] = v; });
                });
        };
        if (now.kind != PerfFx::NONE)
        {
            amounts(press, false, PerfFx::WET_VALUE, 3, "WET");
            amounts(pressure, true, PerfFx::WET_VALUE, 3, "WET");
        }

        for (int i = 0; i < 4; ++i)
        {
            ParamSpec spec = rack.cellSpec(shown, layer, i);
            if (! spec.isValid())
                continue;
            amounts(press, false, i, 4 + i, spec.label);
            amounts(pressure, true, i, 4 + i, spec.label);
            if (now.sync && i == rack.syncCell(shown, layer))
            {
                // In time with the tempo: the cell is the division.
                first.cells[(size_t) (4 + i)] = divisionCell(spec.label, 4 + i);
                continue;
            }
            spec.id = 4 + i;
            first.cells[(size_t) (4 + i)] = cell(spec,
                              [lay, i] { return lay().cell[(size_t) i]; },
                              [this, i](float v) { change([v, i](PerfFx::Layer& l) { l.cell[(size_t) i] = v; }); });
        }

        press.cells[0] = cell(onOff("LTCH"),
                              [pad] { return pad().latch ? 1.0f : 0.0f; },
                              [this](float v)
                              {
                                  // (A latched pad that is on is let go when its latch goes.)
                                  if (v < 0.5f && rack.get(shown).latch && rack.isDown(shown)) rack.press(shown, 0);
                                  changePad([v](PerfFx::Setting& p) { p.latch = v > 0.5f; });
                              });
        static const char* const grids[] = { "OFF", "1/16", "1/8", "1/4", "BAR" };
        press.cells[1] = cell({ "QNT", 1, 0, PerfFx::NUM_QUANT - 1, 0, K::Choice, "", grids, PerfFx::NUM_QUANT },
                              [pad] { return (float) pad().quantize; },
                              [this](float v) { changePad([v](PerfFx::Setting& p) { p.quantize = (int) std::lround(v); }); });
        static const char* const curves[] = { "LIN", "SOFT", "HARD" };
        pressure.cells[0] = cell({ "CURV", 0, 0, PerfFx::NUM_CURVES - 1, 0, K::Choice, "", curves, PerfFx::NUM_CURVES },
                                 [pad] { return (float) pad().pressureCurve; },
                                 [this](float v) { changePad([v](PerfFx::Setting& p) { p.pressureCurve = (int) std::lround(v); }); });

        // ---- MORE -------------------------------------------------------------------------------
        Page more;
        more.name = "MORE";
        more.cells[0] = cell(onOff("SYNC"),
                             [lay] { return lay().sync ? 1.0f : 0.0f; },
                             [this](float v) { if (canBeInTime()) change([v](PerfFx::Layer& l) { l.sync = v > 0.5f; }); });
        more.cells[1] = divisionCell("DIV", 1);
        // The effect's cells beyond its four (the dub echo's SAT, the amp's): knobs 3-6.
        for (int i = 0; i < EffectBase::MAX_EXTRAS; ++i)
        {
            ParamSpec spec = rack.extraSpec(shown, layer, i);
            if (! spec.isValid())
                continue;
            spec.id = 2 + i;
            more.cells[(size_t) (2 + i)] = cell(spec,
                              [lay, i] { return lay().extra[(size_t) i]; },
                              [this, i](float v) { change([v, i](PerfFx::Layer& l) { l.extra[(size_t) i] = v; }); });
        }
        static const char* const clearNames[] = { ">>>>" };
        more.cells[7] = cell({ "CLR", 7, 0, 0, 0, K::Choice, "", clearNames, 1 },
                             [] { return 0.0f; }, [](float) {},
                             [this](float) { return confirm.knob == 7 ? confirm.progress() : juce::String(">>>>"); });

        gridPages.push_back(std::move(first));
        gridPages.push_back(std::move(press));
        gridPages.push_back(std::move(pressure));
        gridPages.push_back(std::move(more));
        gridPageIndex = juce::jlimit(0, (int) gridPages.size() - 1, page);
    }

    Cell divisionCell(const char* label, int id)
    {
        return cell({ label, id, 0, FxSync::NUM_DIVISIONS - 1, (float) FxSync::DEFAULT_DIVISION, ParamKind::Choice, "",
                      FxSync::divisionLabels(), FxSync::NUM_DIVISIONS },
                    [this] { return (float) rack.get(shown, layer).division; },
                    [this](float v) { if (canBeInTime()) change([v](PerfFx::Layer& l) { l.division = (int) std::lround(v); }); });
    }

    // The shown layer's effect has a time or a rate to lock to the tempo; otherwise the knob says why not.
    bool canBeInTime()
    {
        if (! hasEffect())
            return false;
        if (rack.canSync(shown, layer))
            return true;
        notice.say("NO TIME CELL");
        return false;
    }

    static constexpr int MAX_TARGET_TRACKS = 64;
    static const char* const* targetNames()
    {
        static const std::vector<std::string> text = []
        {
            std::vector<std::string> t { "SEL", "MSTR" };
            for (int i = 0; i < MAX_TARGET_TRACKS; ++i) t.push_back("T" + std::to_string(i + 1));
            return t;
        }();
        static const std::vector<const char*> names = [] { std::vector<const char*> n; for (auto& t : text) n.push_back(t.c_str()); return n; }();
        return names.data();
    }
    static const char* const* kindNames()
    {
        static const std::vector<const char*> names = []
        {
            std::vector<const char*> n;
            for (int kind : AudioEngine::perfKinds()) n.push_back(AudioEngine::perfKindName(kind));
            return n;
        }();
        return names.data();
    }

    PerfFxRack& rack;
    std::function<int()> selectedTrack, trackCount;
    int shown = 0, layer = 0;
    int builtKind = -1, builtTracks = -1;
    bool builtSync = false;
    PerfFx::Setting clip;
    bool clipFilled = false;
    TurnConfirm confirm;
    TurnNotice notice;
};
