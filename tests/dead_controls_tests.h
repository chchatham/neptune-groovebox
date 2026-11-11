#pragma once

#include "lfo_destinations_tests.h"
#include "help_card_tests.h"
#include "../src/ui/ScreenAudit.h"
#include "../src/ui/TurnNotice.h"
#include "../src/engine/AudioEngine.h"
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// Dead controls: a cell that is on a page must be heard, and a knob that is on a
// screen must be seen.
//
// Heard: a cell is rendered at the bottom, the middle and the top of its range
// and the three renders are compared with the spectral difference of the "Mod
// Wheel" tests (0 = the same sound, 1 = nothing in common). The largest of the
// three differences is the cell's "range"; below MIN_RANGE the cell is dead.
//   - every macro of every macro-oscillator model
//   - every cell of every drum type
//   - every continuous cell of every engine's pages, with what a musician would
//     have switched on for it (prerequisites(): a filter type that has a drive, a
//     loop for the loop points ...)
//
// Seen: the knobs the screen audit used to list as DEAD either act or say why
// not in the name bar (TurnNotice), through the real screen handlers, and the
// audit itself, run headless, reports no DEAD knob.
//
// NEPTUNE_DEAD_DUMP=<file> writes the measured table.
// -----------------------------------------------------------------------------
class DeadControlsTests : public juce::UnitTest
{
public:
    DeadControlsTests() : UnitTest("Dead Controls") {}

    using L = LfoDestinationsTests;
    using W = ModWheelTests;
    using Plan = L::Plan;

    static constexpr float MIN_RANGE = 0.03f;

    // ---- measuring one cell ----------------------------------------------------------------------

    using Setup = std::function<void (Plan&)>;      // builds the engine and what is played

    struct Row
    {
        juce::String where, label;
        float range = 0.0f;         // the largest difference between the cell at 0, 1/2 and 1 of its range
        bool finite = true;
        float peak = 0.0f;          // the largest sample of the three renders
        float rms[3] = { 0, 0, 0 };
    };

    static Row measureCell(const juce::String& where, const Setup& setup, const ParamSpec& spec)
    {
        Row row { where, spec.label };
        std::vector<std::vector<float>> renders;
        int i = 0;
        for (float n : { 0.0f, 0.5f, 1.0f })
        {
            Plan plan;
            setup(plan);
            plan.engine->setParameter(spec.id, spec.fromNorm(n));
            renders.push_back(L::render(plan));
            double sq = 0.0;
            for (float s : renders.back())
            {
                row.finite = row.finite && std::isfinite(s);
                row.peak = std::max(row.peak, std::abs(s));
                sq += (double) s * s;
            }
            row.rms[i++] = (float) std::sqrt(sq / (double) renders.back().size());
        }
        row.range = std::max({ W::difference(renders[0], renders[1]), W::difference(renders[1], renders[2]),
                               W::difference(renders[0], renders[2]) });
        return row;
    }

    static juce::String table(const std::vector<Row>& rows)
    {
        juce::String out;
        for (auto& r : rows)
            out << r.where.paddedRight(' ', 26) << r.label.paddedRight(' ', 6) << juce::String(r.range, 3)
                << "   level " << juce::roundToInt(W::dB(r.rms[0])) << " " << juce::roundToInt(W::dB(r.rms[1])) << " "
                << juce::roundToInt(W::dB(r.rms[2])) << " dB  peak " << juce::String(r.peak, 2)
                << (r.range < MIN_RANGE ? "   DEAD" : (r.range < 2.0f * MIN_RANGE ? "   weak" : ""))
                << (r.finite ? "" : "   NOT FINITE") << "\n";
        return out;
    }

    static juce::String deadOnes(const std::vector<Row>& rows)
    {
        juce::StringArray dead;
        for (auto& r : rows)
            if (r.range < MIN_RANGE || !r.finite)
                dead.add(r.where + " " + r.label + " " + juce::String(r.range, 3));
        return dead.joinIntoString("; ");
    }

    // ---- the macro oscillator ----------------------------------------------------------------------

    static std::vector<Row> macroRows()
    {
        static const char* const models[] = { "VA", "FM", "FOLD", "FORM", "HARM", "PLCK", "DUST", "BOW" };
        std::vector<Row> rows;
        for (int m = 0; m < 8; ++m)
        {
            const auto pages = MacroOscEngine::pagesFor(m);
            for (int cell = 1; cell <= 4; ++cell)
            {
                const ParamSpec spec = pages[0].params[(size_t) cell];
                Setup setup = [m](Plan& plan)
                {
                    plan.engine = L::makeEngine(EngineType::MacroOsc);
                    plan.engine->setParameter(MacroOscEngine::ModelParam, (float) m);
                };
                rows.push_back(measureCell(juce::String("macro ") + models[m], setup, spec));
            }
        }
        return rows;
    }

    // ---- the drums ---------------------------------------------------------------------------------

    // A pad of each type as a kit would have it (TUNE, DEC; the other cells at the page's defaults).
    struct DrumSound { float tune, decay; };
    static DrumSound drumSound(int type)
    {
        static const DrumSound sounds[DrumEngine::NUM_DRUM_TYPES] = {
            { 50, 0.5f }, { 52, 0.6f }, { 55, 0.7f }, { 200, 0.2f }, { 200, 0.2f }, { 1200, 0.3f }, { 8000, 0.04f },
            { 8000, 0.5f }, { 7000, 1.0f }, { 100, 0.3f }, { 800, 0.3f }, { 5000, 0.2f }, { 2500, 0.3f }, { 40, 0.1f },
            { 48, 0.5f } };
        return sounds[type];
    }

    static void drumSetup(Plan& plan, int type)
    {
        using D = DrumEngine;
        plan.engine = L::makeEngine(EngineType::Drum);
        auto& e = *plan.engine;
        const auto page = D::voicePage(0);
        for (auto& p : page.params)
            e.setParameter(p.id, p.defaultVal);
        e.setParameter(D::TYPE, (float) type);
        e.setParameter(D::TUNE, drumSound(type).tune);
        e.setParameter(D::DECAY, drumSound(type).decay);
        plan.events = { { 0.0, 0, 0.8f }, { 0.12, 0, 0.0f }, { 0.3, 0, 0.8f }, { 0.42, 0, 0.0f } };
    }

    // DEC on a pad that is only tapped (let go after 30 ms): a second and a half of it.
    static std::vector<Row> drumTapRows()
    {
        using D = DrumEngine;
        std::vector<Row> rows;
        for (int type = 0; type < D::NUM_DRUM_TYPES; ++type)
            rows.push_back(measureCell(juce::String("tapped ") + D::typeNames()[type], [type](Plan& plan)
            {
                drumSetup(plan, type);
                plan.seconds = 1.5;
                plan.events = { { 0.0, 0, 0.8f }, { 0.03, 0, 0.0f } };
            }, D::voicePage(0).params[D::DECAY]));
        return rows;
    }

    // Pad pressure on each of the kit's destinations: squeezed 20 ms after the hit and held,
    // against the same hit without pressure.
    static std::vector<Row> drumPressureRows()
    {
        using D = DrumEngine;
        static const DrumATDest dests[] = { DrumATDest::Decay, DrumATDest::Pitch, DrumATDest::Filter, DrumATDest::Level,
                                            DrumATDest::Drive, DrumATDest::Snap, DrumATDest::Pan };
        static const char* const names[] = { "DEC", "TUNE", "TONE", "LEVL", "DRIV", "SNAP", "PAN" };
        std::vector<Row> rows;
        for (int type = 0; type < D::NUM_DRUM_TYPES; ++type)
            for (int d = 0; d < 7; ++d)
            {
                Row row { juce::String("pressure ") + D::typeNames()[type], names[d] };
                std::vector<float> renders[2];
                for (int pressed = 0; pressed < 2; ++pressed)
                {
                    Plan plan;
                    drumSetup(plan, type);
                    auto& kit = static_cast<D&> (*plan.engine);
                    kit.setSlotAftertouchDest(0, dests[d]);
                    kit.setSlotAftertouchDepth(0, 1.0f);
                    kit.setParameter(D::LEVEL, 0.5f);       // room for pressure on the level
                    juce::AudioBuffer<float> buf(2, L::BLOCK);
                    juce::MidiBuffer midi;
                    kit.noteOn(0, 0.8f);
                    for (int block = 0; block < (int) (0.5 * L::SR) / L::BLOCK; ++block)
                    {
                        if (pressed && block == (int) (0.02 * L::SR) / L::BLOCK)
                            kit.aftertouch(0, 1.0f);
                        buf.clear();
                        kit.processBlock(buf, midi);
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < L::BLOCK; ++i)
                                renders[pressed].push_back(buf.getSample(ch, i));
                    }
                }
                row.range = W::difference(renders[0], renders[1]);
                rows.push_back(row);
            }
        return rows;
    }

    static std::vector<Row> drumRows()
    {
        using D = DrumEngine;
        std::vector<Row> rows;
        for (int type = 0; type < D::NUM_DRUM_TYPES; ++type)
        {
            const auto page = D::voicePage(0);
            for (int cell = D::TUNE; cell <= D::PAN; ++cell)
            {
                rows.push_back(measureCell(juce::String("drum ") + D::typeNames()[type],
                                           [type](Plan& plan) { drumSetup(plan, type); }, page.params[(size_t) cell]));
            }
        }
        return rows;
    }

    // ---- every continuous cell of every engine ------------------------------------------------------

    // What a musician would have switched on before reaching for the cell. Nothing here touches
    // the cell itself.
    static void prerequisites(Plan& plan, EngineType type, const juce::String& page, const ParamSpec& spec, bool on)
    {
        if (!on)
        {
            plan.engine = L::makeEngine(type);
            return;
        }
        using S = EngineLfoSection;
        juce::String label = spec.label;
        // The LFO destination tests know what most cells need (their destinations are the cells):
        // a filter that is doing something, operators that sound, a loop, grains, slices ...
        juce::String asDest = label;
        if (type == EngineType::Sampler && page == "STRCH" && label == "SIZE") asDest = "SSIZ";
        if (type == EngineType::Sampler && page == "STRCH" && label == "SPRD") asDest = "SSPR";
        if (type == EngineType::Sampler && page == "GRAIN") asDest = "SIZE";
        if (type == EngineType::Sampler && page == "SLICE") asDest = "SLIC";
        if (type == EngineType::Wavetable && page == "SCAN") asDest = "TIME";
        if (type == EngineType::WaveSeq && page.contains(" ")) asDest = "P" + label.substring(1);    // one step: the only one that sounds
        if (page == "STRIP") asDest = "AMP";
        if (type == EngineType::FM && page.startsWith("OP"))
            asDest = "LVL" + page.substring(2);                             // six carriers: every operator is heard
        if (type == EngineType::UnifiedVoice)
        {
            if (page == "FLT B") asDest = "RESB";
            if (page == "F ENV") asDest = "FDEC";
            if (page == "ANALG") asDest = label == "EVAR" ? "FDEC" : "DRFT";
            if (page == "FILTR" && label == "DRIV") asDest = "OFF";    // on the default type, the SVF
        }
        const auto labels = L::destLabels(*L::makeEngine(type));
        plan = L::prepare(type, asDest, labels);
        auto& e = *plan.engine;
        auto set = [&e](int id, float v) { e.setParameter(id, v); };

        // DRIV is heard on the type a new sound has (the SVF), not only on the ladder.
        const int filterBase = voiceFilterBase(type);
        if (label == "DRIV" && page == "FILTR" && filterBase >= 0)
            set(filterBase + VoiceFilterSection::Type, 0.0f);
        if (label == "WHL")
            e.setModWheel(1.0f);
        // The STRIP page: the strip is up; an amount is heard on the level; the vibrato's rate needs a vibrato.
        if (page == "STRIP")
        {
            e.setModWheel(1.0f);
            for (int r = 0; r < 3; ++r)
                if (spec.id == S::stripAmountId(r))
                    set(S::stripDestId(r), (float) L::indexOf(labels, "AMP"));
            if (spec.id == S::STRIP_VIBRATO_RATE)
                set(S::STRIP_VIBRATO, 1.0f);
        }
        // The filter envelope's cells: a filter that is nearly closed, opened by its envelope, and
        // soft notes for the velocity cells (where a velocity amount takes most away).
        if (page == "F ENV" && filterBase >= 0)
        {
            set(filterBase + VoiceFilterSection::Cutoff, 200.0f);
            set(filterBase + VoiceFilterSection::EnvAmount, 0.8f);
            set(filterBase + VoiceFilterSection::Sustain, label == "REL" ? 0.9f : 0.3f);   // a release needs something to fall from
            if (type == EngineType::FM)
                for (int op = 0; op < 6; ++op)
                    set(op * 10 + 1, FMEngine().getParameter(op * 10 + 1));    // the default sound: less bright
            if (label == "REL")
                plan.events = { { 0.0, 48, 0.7f }, { 0.15, 48, 0.0f } };        // the rest of the render is the release
            else if (plan.events.size() == 4)
                plan.events[0].velocity = plan.events[2].velocity = 0.25f;
        }

        // An LFO's own cells: the LFO on the level, fast enough to be heard in half a second.
        if (page.startsWith("LFO") && S::owns(spec.id))
        {
            const int lfo = spec.id >= S::id(1, S::Rate) ? 1 : 0;
            L::setLfo(e, lfo, L::indexOf(labels, "AMP"), 0.8f, L::SIN, label == "FADE" ? (int) L::KEY : (int) L::FREE, 9.0f);
        }

        switch (type)
        {
            case EngineType::UnifiedVoice:
            {
                using namespace UnifiedParam;
                if (label == "NCOL") set(NOISE, 0.8f);
                if (label == "LVL3") set(WAVE3, (float) (int) UnifiedVoice::OscWaveform::Saw);
                if (label == "SPRD") set(STACK, 1.0f);
                if (label == "GLID")
                {
                    set(VOICE_COUNT, 1.0f);
                    plan.events = { { 0.0, 48, 0.7f }, { 0.2, 72, 0.7f }, { 0.5, 72, 0.0f } };
                }
                if (page == "ANALG")
                {
                    // Analog character on; what is random per note stays at zero but for the cell.
                    auto& u = static_cast<UnifiedVoiceEngine&> (e);
                    for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                        u.getVoice(v).getAnalogCharacter().enabled = true;
                    set(DRIFT, label == "DRAT" ? 0.6f : 0.0f);
                    if (label == "WSHP")
                        for (int id : { WAVE1, WAVE2 })
                            set(id, (float) (int) UnifiedVoice::OscWaveform::Square);
                }
                break;
            }
            case EngineType::Wavetable:
                if (label == "MDPT") set(WavetableEngine::MorphSpeed, 4.0f);
                break;
            case EngineType::Sampler:
                if (page == "SLICE" && (label == "SENS" || label == "GAP"))
                {
                    set(SamplerEngine::SliceMode, (float) SamplerEngine::SliceTransient);
                    set(SamplerEngine::SliceCount, 1.0f);                   // no limit on the slices found
                    set(SamplerEngine::SlicePlay, (float) SamplerEngine::SliceKeys);
                    plan.events = { { 0.0, 62, 0.7f }, { 0.25, 62, 0.0f }, { 0.3, 65, 0.7f }, { 0.55, 65, 0.0f } };
                }
                break;
            case EngineType::Drum:
                if (! page.startsWith("LFO"))
                {
                    // The page is pad 1's: that pad is struck, as a kick machine for its KICK page.
                    if (page == "KICK")
                        set(DrumEngine::TYPE, (float) (int) DrumSynthType::KickSynth);
                    for (auto& ev : plan.events)
                        ev.note = 0;
                }
                break;
            default:
                break;
        }
    }

    // The shared voice filter's first id on the four engines that have it; -1 on the others.
    static int voiceFilterBase(EngineType type)
    {
        switch (type)
        {
            case EngineType::FM:        return FMEngine::FilterBase;
            case EngineType::MacroOsc:  return MacroOscEngine::FilterBase;
            case EngineType::Wavetable: return WavetableEngine::FilterBase;
            case EngineType::WaveSeq:   return WaveSeqEngine::FilterBase;
            default:                    return -1;
        }
    }

    static const ParamSpec* findCell(const ParamPages& pages, const juce::String& label, const juce::String& page = {})
    {
        for (auto& p : pages)
            if (page.isEmpty() || p.name == page)
                for (auto& spec : p.params)
                    if (spec.isValid() && label == spec.label)
                        return &spec;
        return nullptr;
    }

    // ---- the screens --------------------------------------------------------------------------------

    using Rig = helptest::Rig;

    // The rig with the app's sink: what a screen says goes to the name bar's message.
    struct NoticeRig : Rig
    {
        NoticeRig() { TurnNotice::setSink([this](const juce::String& text) { mgr.showStatus(text); }); }
        ~NoticeRig() { TurnNotice::setSink({}); }

        void setEngine(int track, EngineType type)
        {
            audio.setEngineType(track, type, true);
            seq.getPattern().getTrack(track).engineType = type;
        }
        // The page of the active screen that has a knob called `label` - and one called `other`,
        // if given (bank-down until one does).
        bool goToKnob(const juce::String& label, const juce::String& other = {})
        {
            for (int i = 0; i < 40; ++i)
            {
                if (knobNamed(label) >= 0 && (other.isEmpty() || knobNamed(other) >= 0))
                    return true;
                tap(helptest::BANK_DOWN);
                UiClock::advance(1500);
            }
            return false;
        }
        int knobNamed(const juce::String& label)
        {
            for (int e = 0; e < 8; ++e)
                if (mgr.getActiveScreen()->getEncoderName(e) == label)
                    return e;
            return -1;
        }
    };

    // One turn of the knob called `label`: what the name bar says just after it, and that it has
    // gone again a second and a half later.
    void expectNotice(NoticeRig& r, const juce::String& label, const juce::String& message, const juce::String& where)
    {
        UiClock::advance(3000);
        const int knob = r.knobNamed(label);
        expect(knob >= 0, where + ": no knob called " + label);
        if (knob < 0)
            return;
        const auto before = r.picture();
        const int said = TurnNotice::count();
        for (int clicks : { 1, -1 })
        {
            r.turn(knob, clicks);
            expectEquals(r.text(), message, where + ", " + label + " turned " + juce::String(clicks));
            expect(r.picture() != before, where + ": the display shows it");
            UiClock::advance(1500);
            expect(r.text() != message, where + ": the message goes away");
        }
        expectEquals(TurnNotice::count(), said + 2, where + ": counted for the audit");
        UiClock::advance(3000);
        expect(r.picture() == before, where + ": and nothing else changed");
    }

    static std::vector<Row> sweepRows(EngineType type, bool withPrerequisites)
    {
        std::vector<Row> rows;
        const auto pages = L::makeEngine(type)->getParamPages();
        for (auto& page : pages)
            for (auto& spec : page.params)
            {
                if (!spec.isValid() || spec.isDiscrete())
                    continue;
                if (type == EngineType::Drum && spec.id >= DrumEngine::PARAMS_PER_VOICE
                    && spec.id < DrumEngine::NUM_VOICES * DrumEngine::PARAMS_PER_VOICE)
                    continue;       // the other fifteen pads' pages: the drum tests go through every type
                if (type == EngineType::Drum && spec.id >= DrumEngine::KICK_BASE + DrumVoice::NumKickParams
                    && DrumEngine::isKickParam(spec.id))
                    continue;
                const juce::String pageName = page.name;
                const ParamSpec cell = spec;
                rows.push_back(measureCell(L::engineName(type) + " " + pageName, [=](Plan& plan)
                {
                    prerequisites(plan, type, pageName, cell, withPrerequisites);
                }, cell));
            }
        return rows;
    }

    void runTest() override
    {
        juce::String dump;

        beginTest("macro oscillator: every macro of every model is heard");
        {
            const auto rows = macroRows();
            dump << table(rows) << "\n";
            expectEquals(deadOnes(rows), juce::String());
        }

        beginTest("drums: every cell of every drum type is heard");
        {
            const auto rows = drumRows();
            dump << table(rows) << "\n";
            expectEquals(deadOnes(rows), juce::String());
        }

        beginTest("drums: DEC is heard on a pad that is only tapped");
        {
            const auto rows = drumTapRows();
            dump << table(rows) << "\n";
            expectEquals(deadOnes(rows), juce::String());
        }

        beginTest("drums: pad pressure is heard on the destinations that outlast the strike");
        {
            // Squeezing comes after the hit, so SNAP (read at the strike) cannot answer it, nor can
            // anything on a drum that is over by then (closed hat, rim shot); TONE where it shapes
            // the attack only (TOM, MKIK, KSYN) and DEC on a held SUB or a cymbal that outlasts the
            // render are not asked for either. Everything else must be heard.
            const auto rows = drumPressureRows();
            dump << table(rows) << "\n";
            std::vector<Row> must;
            for (auto& r : rows)
            {
                const auto type = r.where.fromFirstOccurrenceOf(" ", false, false);
                if (r.label == "SNAP" || type == "HAT" || type == "RIM")
                    continue;
                if (r.label == "TONE" && (type == "TOM" || type == "MKIK" || type == "KSYN"))
                    continue;
                if (r.label == "DEC" && (type == "SUB" || type == "CYMB"))
                    continue;
                must.push_back(r);
            }
            expect(must.size() >= 70);
            expectEquals(deadOnes(must), juce::String());
        }

        beginTest("drums: a closed hat still cuts the open one, and SUB still stops when it is let go");
        {
            DrumEngine kit;
            kit.prepareToPlay(L::SR, L::BLOCK);
            juce::AudioBuffer<float> buf(2, L::BLOCK);
            juce::MidiBuffer midi;
            auto run = [&](double seconds)
            {
                float peak = 0.0f;
                for (int b = 0; b < (int) (seconds * L::SR) / L::BLOCK; ++b)
                {
                    buf.clear();
                    kit.processBlock(buf, midi);
                    peak = std::max(peak, buf.getMagnitude(0, L::BLOCK));
                }
                return peak;
            };
            kit.noteOn(15, 0.9f);               // the open hat (DEC 0.5 s)
            kit.noteOff(15);                    // a tap
            run(0.1);
            expect(kit.getVoice(15).isActive(), "the open hat rings on after the tap");
            kit.noteOn(14, 0.9f);               // the closed hat chokes it
            run(0.1);
            expect(! kit.getVoice(15).isActive(), "choked");

            kit.noteOn(0, 0.9f);                // a sub pad: held
            run(0.3);
            expect(kit.getVoice(0).isActive(), "SUB sounds while it is held");
            kit.noteOff(0);
            run(0.3);
            expect(! kit.getVoice(0).isActive(), "and stops after its DEC once let go");
        }

        for (auto type : L::engineTypes())
        {
            beginTest(L::engineName(type) + ": every continuous cell of every page is heard");
            if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_DEAD_RAW", {}).isNotEmpty())
                dump << "RAW (nothing switched on)\n" << table(sweepRows(type, false)) << "\n";
            const auto rows = sweepRows(type, true);
            dump << table(rows) << "\n";
            expect(rows.size() >= 16);
            expectEquals(deadOnes(rows), juce::String());
        }

        beginTest("filter DRIV is heard on the default type (SVF) and is exactly off at 0");
        {
            FilterSlot clean, driven;
            for (auto* f : { &clean, &driven })
            {
                f->setSampleRate(L::SR);
                f->setModel(0, FilterModel::SVF);
                f->setCutoff(2000.0f);
            }
            driven.setDrive(0.0f);
            double difference = 0.0, peak = 0.0;
            for (int i = 0; i < 4800; ++i)
            {
                const float x = 0.8f * std::sin((float) i * 0.03f);
                difference += std::abs(clean.process(x) - driven.process(x));
            }
            expect(difference == 0.0, "DRIV 0 leaves the SVF untouched");
            driven.setDrive(1.0f);
            for (int i = 0; i < 4800; ++i)
            {
                const float x = 0.8f * std::sin((float) i * 0.03f);
                const float y = driven.process(x);
                difference += std::abs(clean.process(x) - y);
                peak = std::max(peak, (double) std::abs(y));
            }
            expect(difference > 100.0, "DRIV 1 changes the sound");
            expect(peak < 1.5, "and keeps the level in bounds");
            // Far beyond full level the input stage is still bounded.
            float wild = 0.0f;
            for (int i = 0; i < 4800; ++i)
                wild = std::max(wild, std::abs(driven.process(40.0f * std::sin((float) i * 0.03f))));
            expect(wild < 4.0f, "bounded with a signal forty times too loud");
        }

        beginTest("the filter's MORP: heard in the SEM's MRPH mode, and the knob says so everywhere else");
        {
            using namespace helptest;
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable, EngineType::WaveSeq })
            {
                const juce::String name = L::engineName(type);
                NoticeRig r;
                r.setEngine(0, type);
                r.mgr.setActiveScreen(PARAM);
                expect(r.goToKnob("MORP", "TYPE"), name + ": FILTR has TYPE and MORP");
                expectNotice(r, "MORP", "SEM MRPH ONLY", name + " FILTR on the SVF");

                // TYPE one click on is the SEM, whose first mode is MRPH: now MORP moves.
                r.turn(r.knobNamed("TYPE"), 1);
                UiClock::advance(3000);
                const auto before = r.picture();
                const int said = TurnNotice::count();
                r.turn(r.knobNamed("MORP"), 5);
                UiClock::advance(3000);
                expect(r.picture() != before && TurnNotice::count() == said, name + ": MORP moves on the SEM in MRPH");
                // ... and not in the SEM's other mode.
                r.turn(r.knobNamed("MODE"), 1);
                expectNotice(r, "MORP", "SEM MRPH ONLY", name + " FILTR on the SEM in BP");
            }
        }

        beginTest("a knob with nothing to act on says why in the name bar, for about a second");
        {
            using namespace helptest;
            {
                // WAVE on a synth track: the sampler's markers, mode and slices.
                NoticeRig r;
                r.mgr.setActiveScreen(WAVE);
                for (auto* knob : { "ST", "EN", "LS", "LE", "MD", "SL" })
                    expectNotice(r, knob, "SAMPLER ONLY", "WAVE on a synth track");
                expectNotice(r, "SC", "NOT ZOOMED", "WAVE on a synth track");
                // On a sampler with one slice and the whole sample in view.
                r.setEngine(0, EngineType::Sampler);
                expectNotice(r, "SC", "NOT ZOOMED", "WAVE on a sampler");
                expectNotice(r, "SL", "ONE SLICE", "WAVE on a sampler");
                // Zoomed in, SC scrolls and says nothing.
                r.turn(r.knobNamed("ZM"), 5);
                const int said = TurnNotice::count();
                const auto before = r.picture();
                r.turn(r.knobNamed("SC"), 3);
                expect(r.picture() != before && TurnNotice::count() == said, "SC scrolls once zoomed in");
            }
            {
                // AT ROUTE: the depth of a route that has no destination.
                NoticeRig r;
                r.mgr.setActiveScreen(ATROUTE);
                UiClock::advance(3000);
                const auto before = r.picture();
                r.turn(5, 1);                           // route 2's DPTH (its DEST is OFF)
                expectEquals(r.text(), juce::String("ROUTE OFF"));
                UiClock::advance(1500);
                expect(r.picture() == before, "AT ROUTE: the message goes away");
                const int said = TurnNotice::count();
                r.turn(4, 1);                           // route 1 has a destination: its depth moves
                expect(r.picture() != before && TurnNotice::count() == said, "AT ROUTE: a routed depth moves");
            }
            {
                // PARAM: MODB while filter B is OFF.
                NoticeRig r;
                r.mgr.setActiveScreen(PARAM);
                expect(r.goToKnob("MODB"));
                expectNotice(r, "MODB", "FLT B IS OFF", "PARAM FLT B");
                // With a type chosen for filter B the same knob changes its mode.
                r.turn(r.knobNamed("TYPB"), -3);
                UiClock::advance(3000);
                const auto before = r.picture();
                const int said = TurnNotice::count();
                r.turn(r.knobNamed("MODB"), 1);
                expect(r.picture() != before && TurnNotice::count() == said, "MODB chooses a mode once filter B is on");
            }
            {
                // TRK FX: SLOT with no effect in the first slot.
                NoticeRig r;
                r.mgr.setActiveScreen(TRKFX);
                expect(r.goToKnob("SLOT"));
                expectNotice(r, "SLOT", "SLOT IS EMPTY", "TRK FX");
                // With an effect in slot 1, SLOT goes on to slot 2.
                r.turn(r.knobNamed("TYPE"), 2);
                UiClock::advance(3000);
                const auto before = r.picture();
                const int said = TurnNotice::count();
                r.turn(r.knobNamed("SLOT"), 1);
                expect(r.picture() != before && TurnNotice::count() == said, "SLOT moves to the next slot once this one is used");
            }
            {
                // ARP: nothing kept, nothing tapped.
                NoticeRig r;
                r.mgr.setActiveScreen(ARP);
                expect(r.goToKnob("ANC"));
                expectNotice(r, "ANC", "NOTHING KEPT", "ARP MUTA");
                expect(r.goToKnob("RHYT"));
                UiClock::advance(3000);
                r.turn(r.knobNamed("RHYT"), 1);
                expectEquals(r.text(), juce::String("NOTHING TAPPED"));
            }
            {
                // SONG: the row selector of an empty song.
                NoticeRig r;
                r.mgr.setActiveScreen(SONG);
                expectNotice(r, "RW", "SONG IS EMPTY", "SONG");
                r.turn(r.knobNamed("IN"), 1);           // one row: still nothing to choose between
                expectNotice(r, "RW", "ONE ROW", "SONG with one row");
                r.turn(r.knobNamed("IN"), 1);
                UiClock::advance(3000);
                const int said = TurnNotice::count();
                r.turn(r.knobNamed("RW"), -1);
                expect(TurnNotice::count() == said, "RW selects once there are two rows");
            }
            {
                // STEP: LCKS on a held step that has no locks.
                NoticeRig r;
                r.mgr.setActiveScreen(STEP);
                r.padDown(1);
                UiClock::advance(1000);
                r.tick();
                expect(r.goToKnob("LCKS"));             // the step's TRIG page
                UiClock::advance(3000);
                r.turn(7, 1);
                expectEquals(r.text(), juce::String("NO LOCKS"));
                UiClock::advance(1500);
                expect(r.text() != "NO LOCKS");
                r.padUp(1);
            }
        }

        beginTest("the screen audit, headless, on every screen and engine: no DEAD knob");
        {
            using namespace helptest;
            NoticeRig r;
            const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile("neptune-dead-audit-" + juce::String(juce::Random::getSystemRandom().nextInt(1000000)));
            ScreenAudit audit(r.mgr, r.display);
            audit.engineNames = { "synth", "FM", "macro", "wavetable", "wave seq", "sampler", "drums", "MIDI" };
            audit.setEngine = [&r](int e) { r.setEngine(r.seq.getSelectedTrack(), (EngineType) e); };
            // The app's own list (main.cpp, startScreenAudit) without SETTINGS and SCALE: this rig
            // gives those two screens none of the app's hooks (tempo, master effects, scale lock),
            // so their knobs have nothing behind them here. Neither is among the knobs this is about.
            audit.targets = { { TRACKS, "TRACKS", false, false }, { STEP, "STEP", false, true },
                              { PARAM, "PARAM", true, true },     { TRKFX, "TRK FX", false, true },
                              { FLTR, "FLTR", true, true },       { DRUMS, "DRUMS", false, false },
                              { PRESET, "PRESET", false, false }, { ARP, "ARP", false, false },
                              { SONG, "SONG", false, false },     { ATROUTE, "AT ROUTE", true, false },
                              { WAVE, "WAVE", true, false } };
            const auto totals = audit.run(folder);
            const auto report = folder.getChildFile("report.txt").loadFileAsString();
            folder.deleteRecursively();

            juce::StringArray dead, said;
            for (auto& line : juce::StringArray::fromLines(report))
            {
                if (line.startsWith("DEAD ")) dead.add(line.upToFirstOccurrenceOf("   [findings", false, false));
                if (line.startsWith("SAID ")) said.add(line);
            }
            dump << "\nScreen audit, headless: " << totals.pages << " pages, " << totals.turns << " turns, DEAD " << totals.dead
                 << ", SAID " << totals.said << ", STUCK " << totals.stuck << ", UNLABELLED " << totals.unlabelled << "\n"
                 << dead.joinIntoString("\n") << "\n" << said.joinIntoString("\n") << "\n";
            expectEquals(dead.joinIntoString("\n"), juce::String(), "no knob is dead");
            expectEquals(totals.dead, 0);
            expect(totals.said >= 60, "the knobs that have nothing to act on said why: " + juce::String(totals.said));
            expect(totals.pages >= 150);
            expectEquals(totals.overlap, 0, report);
            expectEquals(totals.clipped, 0, report);
            expectEquals(totals.paging, 0, report);
        }

        // NEPTUNE_DEAD_BENCH=1: what the macros cost. Eight notes of each model for four seconds,
        // with the third and fourth macro where they leave the model alone (50 and 0: the code as
        // it was before they acted) and with both turned up. Percent of one core at 48 kHz.
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_DEAD_BENCH", {}).isNotEmpty())
        {
            static const char* const models[] = { "VA", "FM", "FOLD", "FORM", "HARM", "PLCK", "DUST", "BOW" };
            for (int m = 0; m < 8; ++m)
            {
                double percent[2] = { 0, 0 };
                for (int engaged = 0; engaged < 2; ++engaged)
                {
                    MacroOscEngine e;
                    e.prepareToPlay(L::SR, L::BLOCK);
                    e.setParameter(MacroOscEngine::ModelParam, (float) m);
                    e.setParameter(MacroOscEngine::HarmonicsParam, engaged ? 0.8f : 0.5f);
                    e.setParameter(MacroOscEngine::MorphParam, engaged ? 0.6f : 0.0f);
                    for (int n = 0; n < 8; ++n)
                        e.noteOn(48 + n * 3, 0.8f, -1);
                    juce::AudioBuffer<float> buf(2, L::BLOCK);
                    juce::MidiBuffer midi;
                    const int blocks = (int) (4.0 * L::SR) / L::BLOCK;
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    for (int b = 0; b < blocks; ++b)
                    {
                        buf.clear();
                        e.processBlock(buf, midi);
                    }
                    percent[engaged] = (juce::Time::getMillisecondCounterHiRes() - t0) / 4000.0 * 100.0;
                }
                logMessage(juce::String("macro ") + models[m] + ": neutral " + juce::String(percent[0], 2) + " %, third and fourth up "
                           + juce::String(percent[1], 2) + " %");
            }
        }

        if (const auto file = juce::SystemStats::getEnvironmentVariable("NEPTUNE_DEAD_DUMP", {}); file.isNotEmpty())
            juce::File(file).replaceWithText(dump);
    }
};

static DeadControlsTests deadControlsTests;
