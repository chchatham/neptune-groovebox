#pragma once

#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/TrackFxScreen.h"
#include "../src/controller/DisplayRenderer.h"
#include <juce_core/juce_core.h>

// The control grammar on PARAM and TRK FX (docs/launchkey-mk4-control-map.md, "CONTROL GRAMMAR"): the page
// split, the single drum page, paging, step pages, page keys, fine / coarse turns, pressure
// ranges, locks, control-all, the held-step clipboard and the filter picture over the grid.
// The screens' methods are called directly, as ScreenManager calls them.
class ParamGrammarTests : public juce::UnitTest
{
public:
    ParamGrammarTests() : UnitTest("Param Grammar") {}

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        ParamPageScreen param;
        TrackFxScreen fx;

        // The tests below were written from the engine's first own page; PARAM now opens on the
        // MAIN landing page, which has its own test.
        explicit Rig(EngineType type = EngineType::UnifiedVoice) : param(seq, &audio), fx(seq, &audio)
        {
            add(0, type);
            if (param.getPageName() == "MAIN") param.onArrowRight();
        }

        void add(int trackIdx, EngineType type)
        {
            audio.addEngine(trackIdx, AudioEngine::createEngine(type));
            seq.getPattern().getTrack(trackIdx).engineType = type;
        }
        Track& track(int t = 0) { return seq.getPattern().getTrack(t); }
        float value(int id, int t = 0) { return audio.getTrackParameter(t, id); }
    };

    static juce::StringArray pageNames(ParamCellScreen& screen)
    {
        juce::StringArray names;
        const int was = screen.getCurrentPageIndex();
        for (int i = 0; i < screen.getPageCount(); ++i)
        {
            screen.setCurrentPageIndex(i);
            names.add(screen.getPageName());
        }
        screen.setCurrentPageIndex(was);
        return names;
    }

    static bool same(Screen::PadColour a, Screen::PadColour b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

    static bool rowsEqual(const DisplayRenderer& a, const DisplayRenderer& b, int y0, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = 0; x < 128; ++x)
                if (a.getPixel(x, y) != b.getPixel(x, y))
                    return false;
        return true;
    }

    // Columns that have at least one lit pixel in the rows y0..y1.
    static int litColumns(const DisplayRenderer& d, int y0, int y1)
    {
        int n = 0;
        for (int x = 0; x < 128; ++x)
        {
            bool lit = false;
            for (int y = y0; y <= y1; ++y)
                lit = lit || d.getPixel(x, y);
            n += lit ? 1 : 0;
        }
        return n;
    }

    void runTest() override
    {
        const juce::StringArray trackPages { "TRACK", "ARPLK", "DIRT", "TALK", "MODFX", "FX" };

        // ================================================================== the split
        beginTest("PARAM shows only the engine's own pages; the six track pages are gone from it");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler, EngineType::MIDITrack })
            {
                Rig rig(type);
                auto* engine = rig.audio.getEngine(0);
                juce::StringArray expected;
                // Every engine with a filter opens on the MAIN landing page.
                if (FilterCurve::bind(engine->getParamPages()).valid) expected.add("MAIN");
                for (auto& p : engine->getParamPages()) expected.add(p.name);
                if (engine->supportsModulation()) expected.addArray({ "MOD", "MOD2", "MENV" });

                const auto names = pageNames(rig.param);
                expect(names == expected, "Engine " + juce::String((int) type) + ": " + names.joinIntoString(" "));
                for (auto& t : trackPages)
                    expect(!names.contains(t), "Engine " + juce::String((int) type) + " PARAM has no " + t);
                expectEquals(rig.param.getName(), juce::String("PARAM"));
            }

            // Without an audio engine (previews): the unified voice's pages and the MOD pages.
            SequencerCore seq;
            ParamPageScreen bare(seq);
            expectEquals(bare.getPageCount(), (int) UnifiedVoiceEngine::basePages().size() + 4);
            expectEquals(bare.getPageName(), juce::String("MAIN"));
        }

        beginTest("MAIN: every engine with a filter opens on cutoff, resonance and its level envelope");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler })
            {
                SequencerCore seq;
                AudioEngine audio;
                audio.addEngine(0, AudioEngine::createEngine(type));
                seq.getPattern().getTrack(0).engineType = type;
                ParamPageScreen p(seq, &audio);
                const juce::String who = "Engine " + juce::String((int) type);
                expectEquals(p.getPageIndex(), 0);
                expectEquals(p.getPageName(), juce::String("MAIN"), who);
                expectEquals(p.getEncoderName(0), juce::String("CUT"), who);
                expectEquals(p.getEncoderName(1), juce::String("RESO"), who);
                expectEquals(p.getEncoderName(4), juce::String("ATK"), who);
                expectEquals(p.getEncoderName(5), juce::String("DEC"), who);
                expectEquals(p.getEncoderName(6), juce::String("SUS"), who);
                expectEquals(p.getEncoderName(7), juce::String("REL"), who);

                // The cells are the engine's own: a turn here is the turn on its FILTR page.
                const auto main = p.getCurrentPage();
                const float before = audio.getTrackParameter(0, main.params[1].id);
                p.onEncoderChange(1, 10);
                const float after = audio.getTrackParameter(0, main.params[1].id);
                expect(after > before, who + ": RESO moves the engine's resonance");
                expect(p.isFilterPictureActive(), who + ": and shows the filter picture");
                expect(p.selectPageByName("FILTR"));
                expectEquals(p.getCurrentPage().params[1].id, main.params[1].id, who);
                expectWithinAbsoluteError(audio.getTrackParameter(0, p.getCurrentPage().params[1].id), after, 0.0001f);

                p.setCurrentPageIndex(0);
                p.onEncoderChange(4, 1);
                expect(p.isEnvelopePictureActive(), who + ": ATK shows the envelope picture");
            }
            // Drums and MIDI tracks have no filter and no MAIN page.
            for (auto type : { EngineType::Drum, EngineType::MIDITrack })
            {
                SequencerCore seq;
                AudioEngine audio;
                audio.addEngine(0, AudioEngine::createEngine(type));
                seq.getPattern().getTrack(0).engineType = type;
                ParamPageScreen p(seq, &audio);
                expect(p.getPageName() != "MAIN");
            }
        }

        beginTest("TRK FX has exactly the six pages on every engine");
        {
            for (int e = 0; e <= (int) EngineType::MIDITrack; ++e)
            {
                Rig rig((EngineType) e);
                expectEquals(rig.fx.getName(), juce::String("TRK FX"));
                expectEquals(rig.fx.getPageCount(), 6);
                expect(pageNames(rig.fx) == trackPages, "Engine " + juce::String(e) + ": " + pageNames(rig.fx).joinIntoString(" "));
                expect(rig.fx.getEncoderLayout() == Screen::EncoderLayout::Grid && !rig.fx.usesEncoderBar());
                expectEquals(rig.fx.getSubPage(), juce::String("TRACK"));
            }
        }

        beginTest("TRK FX: every page's cells still edit what they did");
        {
            Rig rig(EngineType::FM);
            auto& track = rig.track();
            auto& fx = rig.fx;

            // TRACK: VOL PAN SND1 SND2 / BUS1 BUS2 LEN SPD
            const char* trackCells[8] = { "VOL", "PAN", nullptr, nullptr, "BUS1", "BUS2", "LEN", "SPD" };
            for (int i = 0; i < 8; ++i)
                if (trackCells[i] != nullptr)
                    expectEquals(fx.getEncoderName(i), juce::String(trackCells[i]));
            fx.onEncoderChange(0, 10);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME), 0.9f, 0.001f);
            expectWithinAbsoluteError(track.soundParams[MixParam::VOLUME], 0.9f, 0.001f);
            fx.onEncoderChange(1, 128 - 25);
            expectWithinAbsoluteError(rig.value(MixParam::PAN), -0.5f, 0.001f);
            fx.onEncoderChange(2, 30);
            expectWithinAbsoluteError(rig.value(MixParam::SEND_BASE), 0.3f, 0.001f);
            const auto busBefore = rig.audio.getSendBusType(0);
            fx.onEncoderChange(4, 127);
            expect(rig.audio.getSendBusType(0) != busBefore, "BUS1 chooses the send bus effect");
            fx.onEncoderChange(6, 128 - 4);
            expectEquals(track.length, 12);
            fx.onEncoderChange(7, 1);
            expectWithinAbsoluteError(track.speedMultiplier, 2.0f, 0.001f);

            // ARPLK: a held step locks the arpeggiator's ratchet.
            expect(fx.selectPageByName("ARPLK"));
            expectEquals(fx.getEncoderName(0), juce::String("GATE"));
            expectEquals(fx.getEncoderName(2), juce::String("RTCH"));
            fx.onPadPress(5, 100);
            fx.onEncoderChange(2, 2);
            fx.onPadRelease(5);
            expectWithinAbsoluteError(track.getStep(5).paramLocks[AudioEngine::ARP_LOCK_BASE + 2], 2.0f, 0.001f);

            // DIRT, TALK, MODFX: the three blocks.
            expect(fx.selectPageByName("DIRT"));
            fx.onEncoderChange(0, 20);
            expectWithinAbsoluteError(rig.value(TrackFxParam::DIRT_BASE + DirtBlock::Drive), 0.2f, 0.001f);
            expect(fx.selectPageByName("TALK"));
            fx.onEncoderChange(3, 50);
            expectWithinAbsoluteError(rig.value(TrackFxParam::TALK_BASE + TalkBlock::Mix), 0.5f, 0.001f);
            expectWithinAbsoluteError(track.soundParams[TrackFxParam::TALK_BASE + TalkBlock::Mix], 0.5f, 0.001f);
            expect(fx.selectPageByName("MODFX"));
            fx.onEncoderChange(1, 40);
            expectWithinAbsoluteError(rig.value(TrackFxParam::MODFX_BASE + ModFxBlock::ChorusDepth), 0.4f, 0.001f);

            // FX: SLOT TYPE ON WET / the effect's four cells; SYNC replaces ON, TIME becomes the division.
            expect(fx.selectPageByName("FX"));
            expectEquals(fx.getEncoderName(0), juce::String("SLOT"));
            expectEquals(fx.getEncoderName(1), juce::String("TYPE"));
            expectEquals(fx.getEncoderName(4), juce::String("---"));
            fx.onEncoderChange(1, 6);                                   // OFF -> DLY
            expectEquals(rig.audio.getInsertChain(0)->getEffectCount(), 1);
            auto* delay = rig.audio.getInsertChain(0)->getEffect(0);
            expectEquals(fx.getEncoderName(2), juce::String("SYNC"));
            expectEquals(fx.getEncoderName(3), juce::String("WET"));
            expectEquals(fx.getEncoderName(4), juce::String("TIME"));
            const float seconds = delay->getParameter(0);
            fx.onEncoderChange(2, 1);
            expect(delay->isSynced());
            expect(fx.getCurrentPage().params[4].kind == ParamKind::Choice, "TIME is a division list while synced");
            const int division = delay->getSyncDivision();
            fx.onEncoderChange(4, 1);
            expectEquals(delay->getSyncDivision(), division + 1);
            fx.onEncoderChange(2, 127);
            expect(!delay->isSynced());
            expectWithinAbsoluteError(delay->getParameter(0), seconds, 0.0005f);
            fx.onEncoderChange(3, 128 - 30);
            expectWithinAbsoluteError(delay->getMix(), 0.7f, 0.001f);
            fx.onEncoderChange(0, 1);                                   // the next (empty) slot
            expectEquals(fx.getFxSelectedSlot(), 1);
            expectEquals(fx.getEncoderName(4), juce::String("---"));
        }

        // ================================================================== drums
        beginTest("A drum track has one DR page, for the pad selected on DRUMS, then the engine's other pages");
        {
            Rig rig(EngineType::Drum);
            auto* drums = dynamic_cast<DrumEngine*>(rig.audio.getEngine(0));
            expect(drums != nullptr);
            if (drums == nullptr) return;
            const auto all = drums->getParamPages();
            // The engine lists a DR page and a KICK page for each of the 16 pads, then STRIP and its LFO pages.
            const int extra = (int) all.size() - 2 * DrumEngine::NUM_VOICES;
            expectEquals(extra, 3);
            // The default kit's lower left pad (the one DRUMS starts on) is a kick machine, which
            // has a page more; this part is about an ordinary pad, the kick's page comes below.
            expect(drums->isKickMachine(8));
            drums->setParameter(8 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TYPE, (float) (int) DrumSynthType::FMKick);

            expectEquals(drums->getSelectedVoice(), 8, "The pad DRUMS starts on");
            expectEquals(rig.param.getPageCount(), 1 + extra);
            expectEquals(rig.param.getPageName(), juce::String("DR 9"));
            auto names = pageNames(rig.param);
            for (int i = 0; i < extra; ++i)
                expectEquals(names[1 + i], all[(size_t) (2 * DrumEngine::NUM_VOICES + i)].name);
            int drPages = 0;
            for (auto& n : names) drPages += n.startsWith("DR ") ? 1 : 0;
            expectEquals(drPages, 1);

            // Selecting another pad changes the page in place.
            drums->setSelectedVoice(2);
            expectEquals(rig.param.getPageName(), juce::String("DR 3"));
            expectEquals(rig.param.getPageCount(), 1 + extra);
            expectEquals(rig.param.getCurrentPage().params[2].id, 2 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TONE);
            rig.param.onEncoderChange(2, 20);                           // TONE 0 -> 0.2 on voice 3 only (a sub pad: 0 = a sine)
            expectWithinAbsoluteError(drums->getParameter(2 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TONE), 0.2f, 0.001f);
            expectWithinAbsoluteError(drums->getParameter(8 * DrumEngine::PARAMS_PER_VOICE + DrumEngine::TONE), 0.7f, 0.001f);

            // On another page the selection does not pull the screen away.
            if (extra > 0)
            {
                rig.param.onArrowRight();
                const auto other = rig.param.getPageName();
                drums->setSelectedVoice(11);
                expectEquals(rig.param.getPageName(), other);
                rig.param.onArrowLeft();
                expectEquals(rig.param.getPageName(), juce::String("DR 12"));
            }
            drums->setSelectedVoice(99);
            expectEquals(drums->getSelectedVoice(), DrumEngine::NUM_VOICES - 1);

            // A pad set to the kick machine (TYPE KSYN) gets a KICK page after its DR page.
            drums->setSelectedVoice(0);
            rig.param.setCurrentPageIndex(0);
            expectEquals(rig.param.getPageCount(), 1 + extra);
            rig.audio.setTrackParameter(0, DrumEngine::TYPE, (float) (int) DrumSynthType::KickSynth);
            expectEquals(rig.param.getPageCount(), 2 + extra);
            rig.param.onArrowRight();
            expectEquals(rig.param.getPageName(), juce::String("KICK"));
            const char* kickLabels[8] = { "SWEP", "SWPT", "SCRV", "SHAP", "KNCK", "KTON", "HOLD", "ACRV" };
            for (int i = 0; i < 8; ++i)
                expectEquals(rig.param.getEncoderName(i), juce::String(kickLabels[i]));
            const int sweepId = rig.param.getCurrentPage().params[0].id;
            expect(EngineParamRange::contains(sweepId) && DrumEngine::isKickParam(sweepId));
            rig.param.onEncoderChange(0, 5);
            expect(drums->getParameter(sweepId) > 36.0f, "SWEP reaches the engine");
            expect(rig.track().soundParams.count(sweepId) == 1, "and is stored with the sound");
            drums->setSelectedVoice(1);                                 // an ordinary pad: no KICK page
            expectEquals(rig.param.getPageCount(), 1 + extra);
        }

        beginTest("Kick machine: sweep, shape, knock, hold and decay each change the sound as named");
        {
            auto render = [](std::function<void(DrumVoice&)> setup, int samples = 22050)
            {
                DrumVoice v;
                v.setSampleRate(44100.0);
                v.setType(DrumSynthType::KickSynth);
                v.setTune(50.0f);
                v.setDecay(0.3f);
                v.setTone(1.0f);
                v.setLevel(1.0f);
                setup(v);
                v.noteOn(1.0f);
                std::vector<float> out((size_t) samples);
                for (auto& s : out) s = v.process();
                return out;
            };
            auto energy = [](const std::vector<float>& x, int from, int to)
            {
                double e = 0.0;
                for (int i = from; i < to && i < (int) x.size(); ++i) e += (double) x[(size_t) i] * x[(size_t) i];
                return e;
            };
            auto crossings = [](const std::vector<float>& x, int from, int to)
            {
                int n = 0;
                for (int i = from + 1; i < to && i < (int) x.size(); ++i)
                    if ((x[(size_t) (i - 1)] < 0.0f) != (x[(size_t) i] < 0.0f)) ++n;
                return n;
            };
            auto plain = [](DrumVoice& v) { v.setKickParam(DrumVoice::KickKnock, 0.0f); };

            const auto base = render(plain);
            expect(energy(base, 0, 22050) > 1.0, "It sounds");
            for (auto s : base) if (!std::isfinite(s) || std::abs(s) > 1.5f) { expect(false, "bounded output"); break; }
            expect(energy(base, 20000, 22050) < 1.0e-4 * energy(base, 0, 2205), "and dies away after DEC");

            // SWEP: more sweep = a higher pitch at the start (more zero crossings in the first 20 ms),
            // the same pitch at the end.
            const auto noSweep = render([&](DrumVoice& v) { plain(v); v.setKickParam(DrumVoice::KickSweep, 0.0f); });
            const auto bigSweep = render([&](DrumVoice& v) { plain(v); v.setKickParam(DrumVoice::KickSweep, 60.0f); });
            expect(crossings(bigSweep, 0, 882) > crossings(noSweep, 0, 882) + 3);
            expect(std::abs(crossings(noSweep, 0, 4410) - 10) <= 1, "TUNE 50 Hz without sweep: 10 crossings in 100 ms");
            // SWPT: a longer sweep keeps the pitch high for longer.
            const auto slow = render([&](DrumVoice& v) { plain(v); v.setKickParam(DrumVoice::KickSweepTime, 0.3f); });
            expect(crossings(slow, 2205, 6615) > crossings(base, 2205, 6615) + 2);
            // SHAP: towards square = more energy for the same peak.
            const auto square = render([&](DrumVoice& v) { plain(v); v.setKickParam(DrumVoice::KickShape, 1.0f); });
            expect(energy(square, 0, 8820) > 1.3 * energy(base, 0, 8820));
            // KNCK: adds a tick confined to the first few milliseconds.
            const auto knock = render([](DrumVoice& v) { v.setKickParam(DrumVoice::KickKnock, 1.0f); v.setKickParam(DrumVoice::KickKnockTone, 0.8f); });
            double early = 0.0, late = 0.0;
            for (int i = 0; i < 22050; ++i)
            {
                const double d = (double) knock[(size_t) i] - base[(size_t) i];
                (i < 441 ? early : late) += d * d;
            }
            expect(early > 0.5 && late < 0.01 * early, "the knock is at the very start only");
            // HOLD: full level for longer.
            const auto held = render([&](DrumVoice& v) { plain(v); v.setKickParam(DrumVoice::KickHold, 0.2f); });
            expect(energy(held, 4410, 8820) > 2.0 * energy(base, 4410, 8820));
            // DEC (the pad's own page): a longer tail.
            const auto longer = render([&](DrumVoice& v) { plain(v); v.setDecay(1.0f); });
            expect(energy(longer, 13230, 22050) > 10.0 * energy(base, 13230, 22050));

            // The fourteen older types are untouched by the kick's cells.
            DrumVoice a, b;
            for (auto* v : { &a, &b }) { v->setSampleRate(44100.0); v->setType(DrumSynthType::AnalogKick); }
            b.setKickParam(DrumVoice::KickSweep, 70.0f);
            b.setKickParam(DrumVoice::KickKnock, 1.0f);
            a.noteOn(1.0f); b.noteOn(1.0f);
            bool identical = true;
            for (int i = 0; i < 4410; ++i) identical = identical && a.process() == b.process();
            expect(identical);
        }

        // ================================================================== paging
        beginTest("Bank buttons step the encoder page, also while a step is held; they never touch ranges");
        {
            Rig rig;
            auto& p = rig.param;
            p.setCurrentPageIndex(0);
            expectEquals(p.getPageName(), juce::String("MAIN"));
            p.onArrowLeft();
            expectEquals(p.getPageIndex(), p.getPageCount() - 1, "Back from the first page is the last");
            p.onArrowRight();
            expectEquals(p.getPageIndex(), 0, "and on from the last is the first");
            p.onArrowRight();
            p.onArrowRight();
            expectEquals(p.getPageName(), juce::String("OSC3"));
            expectEquals(p.getSubPage(), juce::String("OSC3"));

            p.onPadPress(3, 100);
            expect(p.isStepHeld());
            p.onArrowRight();
            expectEquals(p.getPageName(), juce::String("SRC"));
            expect(p.isStepHeld() && p.getHeldStepIndex() == 3, "The hold survives paging");
            p.onPadRelease(3);

            p.setCurrentPageIndex(p.getPageCount() - 1);

            p.onEncoderChange(2, 1);
            p.onArrowLeft();
            p.onArrowRight();
            p.onPadPagePrev();
            p.onPadPageNext();
            for (auto& [id, v] : rig.track().soundParams)
            {
                juce::ignoreUnused(v);
                expect(!RangeParam::isRangeParam(id), "Plain arrows never make a range end");
            }
        }

        beginTest("Envelope picture: an envelope knob shows the envelope's shape over the other half of the grid");
        {
            // The shape comes from the real Envelope class, on a logarithmic time axis.
            EnvelopeCurve::Settings s;
            s.attack = 0.01f; s.decay = 0.1f; s.sustain = 0.5f; s.release = 0.3f;
            auto shape = EnvelopeCurve::shape(s);
            expectWithinAbsoluteError(shape.heights[(size_t) shape.attackEnd], 1.0f, 0.001f);
            expectWithinAbsoluteError(shape.heights[(size_t) shape.decayEnd], 0.5f, 0.001f);
            expectWithinAbsoluteError(shape.heights[(size_t) shape.holdEnd], 0.5f, 0.001f);
            expectWithinAbsoluteError(shape.heights.back(), 0.0f, 0.001f);
            expect((int) shape.heights.size() <= 128, "Always fits the display");
            EnvelopeCurve::Settings longest;
            longest.attack = longest.decay = longest.release = 10.0f;
            expect((int) EnvelopeCurve::shape(longest).heights.size() <= 128);
            int previous = 0;
            for (float t : { 0.001f, 0.003f, 0.01f, 0.03f, 0.1f, 0.3f, 1.0f, 3.0f, 10.0f })
            {
                expect(EnvelopeCurve::stageColumns(t) > previous, "A longer time is a wider stage");
                previous = EnvelopeCurve::stageColumns(t);
            }
            // CURV bends the stages: half way through a long attack the level is not one half.
            s.attack = 1.0f;
            const auto straight = EnvelopeCurve::shape(s);
            s.curve = 1.0f;
            const auto bent = EnvelopeCurve::shape(s);
            const size_t mid = (size_t) (straight.attackEnd / 2);
            expectWithinAbsoluteError(straight.heights[mid], 0.5f, 0.05f);
            expect(std::abs(bent.heights[mid] - straight.heights[mid]) > 0.1f);

            // Which pages have one: by their cells.
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler })
            {
                int found = 0;
                for (auto& page : AudioEngine::createEngine(type)->getParamPages())
                    if (EnvelopeCurve::bind(page).valid) ++found;
                expect(found >= 1, "Engine " + juce::String((int) type) + " has an envelope page");
            }
            for (auto& page : AudioEngine::createEngine(EngineType::MIDITrack)->getParamPages())
                expect(!EnvelopeCurve::bind(page).valid, "A MIDI track's ATK / DEC / REL are controller values, not an envelope");

            Rig rig;
            auto& p = rig.param;
            expect(p.selectPageByName("AMP"));
            EnvelopeCurve::Settings shown;
            expect(p.getEnvelopeSettings(shown));
            expectWithinAbsoluteError(shown.sustain, rig.value(UnifiedParam::AMP_SUSTAIN), 0.0001f);

            const int topY0 = ParamCellScreen::PICTURE_TOP_Y, topY1 = topY0 + ParamCellScreen::PICTURE_TOP_H - 1;
            const int botY0 = ParamCellScreen::PICTURE_BOTTOM_Y, botY1 = botY0 + ParamCellScreen::PICTURE_BOTTOM_H - 1;
            DisplayRenderer with, without;
            p.render(without);
            expect(!p.isEnvelopePictureActive(), "Not before a knob is turned");

            int atkCell = -1, susCell = -1, otherCell = -1;
            const auto page = p.getCurrentPage();
            const auto binding = EnvelopeCurve::bind(page);
            for (int i = 0; i < 8; ++i)
            {
                if (page.params[(size_t) i].id == binding.atk.id) atkCell = i;
                else if (page.params[(size_t) i].id == binding.sus.id) susCell = i;
                else if (page.params[(size_t) i].isValid() && !binding.isCell(page.params[(size_t) i].id)) otherCell = i;
            }
            expect(atkCell >= 0 && atkCell < 4 && susCell >= 0);

            p.onEncoderChange(atkCell, 1);
            expect(p.isEnvelopePictureActive() && !p.isFilterPictureActive());
            p.render(with);
            p.expireFilterPicture();
            without.clear();
            p.render(without);
            expect(rowsEqual(with, without, 0, botY0 - 1), "The row with the turned knob stays");
            expect(rowsEqual(with, without, botY1 + 1, 63), "and so does the page strip");
            expect(!rowsEqual(with, without, botY0, botY1), "The other row is the picture");
            {
                expect(p.getEnvelopeSettings(shown));
                DisplayRenderer expected;
                EnvelopeCurve::draw(expected, 0, botY0, 128, ParamCellScreen::PICTURE_BOTTOM_H, EnvelopeCurve::shape(shown));
                expect(rowsEqual(with, expected, botY0, botY1), "The picture is EnvelopeCurve's, with the page's own values");
            }

            // The picture follows the value: a lower sustain is a different picture.
            p.onEncoderChange(susCell, 127);
            DisplayRenderer a, b;
            p.render(a);
            for (int i = 0; i < 20; ++i) p.onEncoderChange(susCell, 127);
            p.render(b);
            if (susCell < 4) expect(!rowsEqual(a, b, botY0, botY1));
            else             expect(!rowsEqual(a, b, topY0, topY1));

            // A knob that is not part of the envelope shows no envelope; leaving the page ends it.
            if (otherCell >= 0)
            {
                p.onEncoderChange(otherCell, 1);
                expect(!p.isEnvelopePictureActive());
            }
            p.onEncoderChange(atkCell, 1);
            p.onArrowRight();
            expect(!p.isEnvelopePictureActive());

            // The extra envelopes (MENV) and an FM operator page have it too.
            expect(p.selectPageByName("MENV"));
            p.onEncoderChange(2, 1);
            expect(p.isEnvelopePictureActive());
            Rig fm(EngineType::FM);
            expect(fm.param.selectPageByName("OP1"));
            fm.param.onEncoderChange(4, 1);
            expect(fm.param.isEnvelopePictureActive());
        }

        beginTest("Up / Down change which 16 steps the pads address: locks reach steps 17 and beyond");
        {
            Rig rig;
            auto& p = rig.param;
            auto& track = rig.track();
            expectEquals(p.getStepPageCount(), 1);
            p.onPadPageNext();
            expectEquals(p.getStepPage(), 0, "A 16-step track has one step page");
            expectEquals(p.getSubPage(), juce::String("OSC"));

            track.setLength(40);
            expectEquals(p.getStepPageCount(), 3);
            expectEquals(p.getSubPage(), juce::String("OSC 1+"));
            p.onPadPageNext();
            expectEquals(p.getStepPage(), 1);
            expectEquals(p.getSubPage(), juce::String("OSC 17+"));

            p.onPadPress(0, 100);                                       // step 17
            expectEquals(p.getHeldStepIndex(), 16);
            expectEquals(p.getSubPage(), juce::String("S17 OSC"));
            p.onEncoderChange(2, 10);                                   // MIX
            p.onPadRelease(0);
            expectWithinAbsoluteError(track.getStep(16).paramLocks[UnifiedParam::MIX], 0.6f, 0.001f);
            expect(track.getStep(0).paramLocks.empty(), "Step 1 is untouched");
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.5f, 0.001f);

            p.onPadPageNext();
            p.onPadPageNext();
            expectEquals(p.getStepPage(), 2, "Stops at the last step page");
            p.onPadPress(7, 100);                                       // step 40, the last one
            expectEquals(p.getHeldStepIndex(), 39);
            p.onPadRelease(7);
            p.onPadPress(8, 100);                                       // step 41 does not exist
            expect(!p.isStepHeld());
            p.onPadRelease(8);

            p.onPadPagePrev();
            expectEquals(p.getStepPage(), 1);
            track.setLength(16);
            expectEquals(p.getStepPage(), 0, "A shorter track pulls the step page back");

            // The same on TRK FX.
            track.setLength(32);
            rig.fx.onPadPageNext();
            rig.fx.onPadPress(15, 100);
            expectEquals(rig.fx.getHeldStepIndex(), 31);
            rig.fx.onEncoderChange(0, 128 - 10);                        // VOL locked on step 32
            rig.fx.onPadRelease(15);
            expectWithinAbsoluteError(track.getStep(31).paramLocks[MixParam::VOLUME], 0.7f, 0.001f);
        }

        beginTest("Func + bank jumps sections; page keys: one per section, the same key again steps through it");
        {
            Rig rig;
            auto& p = rig.param;
            p.onSectionNext();
            expectEquals(p.getPageName(), juce::String("FILTR"));
            p.onSectionNext();
            expectEquals(p.getPageName(), juce::String("AMP"));
            p.onSectionPrev();
            expectEquals(p.getPageName(), juce::String("FILTR"));
            p.onArrowRight();
            p.onArrowRight();
            expectEquals(p.getPageName(), juce::String("F ENV"));
            p.onSectionPrev();
            expectEquals(p.getPageName(), juce::String("FILTR"));
            p.onSectionPrev();
            expectEquals(p.getPageName(), juce::String("OSC"));

            // Sections of the unified voice in order of appearance.
            juce::StringArray sections { "MAIN" };
            for (auto& page : rig.audio.getEngine(0)->getParamPages()) sections.addIfNotAlreadyThere(page.section);
            sections.addIfNotAlreadyThere("MOD");
            expectEquals(p.getPageKeyCount(), juce::jmin(8, sections.size()));
            expect(p.getPageKeyCount() >= 4 && p.getPageKeyCount() <= 8);
            expectEquals(p.getActivePageKey(), 1);

            const int fltr = sections.indexOf("FLTR");
            p.onPageKey(fltr);
            expectEquals(p.getPageName(), juce::String("FILTR"));
            expectEquals(p.getActivePageKey(), fltr);
            p.onPageKey(fltr);
            expectEquals(p.getPageName(), juce::String("FLT B"));
            p.onPageKey(fltr);
            expectEquals(p.getPageName(), juce::String("F ENV"));
            p.onPageKey(fltr);
            expectEquals(p.getPageName(), juce::String("FILTR"), "Wraps to the section's first page");
            p.onPageKey(sections.indexOf("MOD"));
            expectEquals(p.getPageName(), juce::String("MOD"));
            p.onPageKey(sections.indexOf("MOD"));
            expectEquals(p.getPageName(), juce::String("MOD2"));
            p.onPageKey(0);
            expectEquals(p.getPageName(), juce::String("MAIN"));
            p.onPageKey(1);
            expectEquals(p.getPageName(), juce::String("OSC"));
            p.onPageKey(-1);
            p.onPageKey(p.getPageKeyCount());
            expectEquals(p.getPageName(), juce::String("OSC"), "Keys without a section do nothing");

            // TRK FX: one key per page (TRACK, ARPLK, DIRT, TALK, MODFX, FX); sections still jump with Func + bank.
            auto& fx = rig.fx;
            expectEquals(fx.getPageKeyCount(), 6);
            fx.onPageKey(2);
            expectEquals(fx.getPageName(), juce::String("DIRT"));
            fx.onPageKey(5);
            expectEquals(fx.getPageName(), juce::String("FX"));
            expectEquals(fx.getActivePageKey(), 5);
            fx.onPageKey(1);
            expectEquals(fx.getPageName(), juce::String("ARPLK"));
            fx.onSectionNext();
            expectEquals(fx.getPageName(), juce::String("DIRT"));
            fx.onSectionPrev();
            expectEquals(fx.getPageName(), juce::String("TRACK"));
        }

        // ================================================================== turning
        beginTest("Plain, fine (Shift) and coarse (Func) turns on linear, log, int and choice cells");
        {
            Rig rig;
            auto& p = rig.param;

            // Linear: MIX 0.5.
            p.onEncoderChange(2, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.51f, 0.0001f);
            p.setModifiers(true, false);
            p.onEncoderChange(2, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.511f, 0.0001f);
            p.onEncoderChange(2, 128 - 11);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.5f, 0.0001f);
            p.setModifiers(false, true);
            p.onEncoderChange(2, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.75f, 0.0001f);
            p.onEncoderChange(2, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 1.0f, 0.0001f);
            p.onEncoderChange(2, 128 - 3);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.25f, 0.0001f);
            expectWithinAbsoluteError(rig.track().soundParams[UnifiedParam::MIX], 0.25f, 0.0001f);

            // Int in semitones: TUN2.
            p.setModifiers(false, false);
            p.onEncoderChange(4, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::TUNE2), 1.0f, 0.0001f);
            p.setModifiers(true, false);
            p.onEncoderChange(4, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::TUNE2), 2.0f, 0.0001f, "A whole number cannot move by less than 1");
            p.setModifiers(false, true);
            p.onEncoderChange(4, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::TUNE2), 14.0f, 0.0001f, "Coarse = an octave");
            p.onEncoderChange(4, 127);
            expectWithinAbsoluteError(rig.value(UnifiedParam::TUNE2), 2.0f, 0.0001f);

            // Log in Hz: CUT 8 kHz.
            expect(p.selectPageByName("FILTR"));
            p.setModifiers(false, false);
            p.onEncoderChange(0, 127);
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 8000.0f / std::pow(1000.0f, 0.01f), 1.0f);
            rig.audio.setTrackParameter(0, UnifiedParam::CUTOFF, 1000.0f);
            p.setModifiers(true, false);
            p.onEncoderChange(0, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 1000.0f * std::pow(1000.0f, 0.001f), 0.2f);
            rig.audio.setTrackParameter(0, UnifiedParam::CUTOFF, 1000.0f);
            p.setModifiers(false, true);
            p.onEncoderChange(0, 2);
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 4000.0f, 0.5f, "Coarse = octaves");
            p.onEncoderChange(0, 127);
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 2000.0f, 0.5f);

            // Choice: the filter TYPE, six entries.
            p.setModifiers(false, false);
            p.onEncoderChange(4, 1);
            expectEquals((int) rig.value(UnifiedParam::MODEL_A), 1);
            p.setModifiers(true, false);
            p.onEncoderChange(4, 1);
            expectEquals((int) rig.value(UnifiedParam::MODEL_A), 2);
            p.setModifiers(false, true);
            p.onEncoderChange(4, 127);
            expectEquals((int) rig.value(UnifiedParam::MODEL_A), 0, "Coarse = four entries, clamped");
            p.onEncoderChange(4, 1);
            expectEquals((int) rig.value(UnifiedParam::MODEL_A), 4);

            // An Int count: LFOS 1..4 on MOD.
            expect(p.selectPageByName("MOD"));
            p.onEncoderChange(1, 1);
            expectEquals((int) rig.value(ModParam::LFO_COUNT), 4);
            p.setModifiers(false, false);
            p.onEncoderChange(1, 127);
            expectEquals((int) rig.value(ModParam::LFO_COUNT), 3);

            // The same three step sizes while a step is held (locks), and on TRK FX.
            expect(p.selectPageByName("OSC"));
            p.onPadPress(1, 100);
            p.setModifiers(true, false);
            p.onEncoderChange(2, 1);
            expectWithinAbsoluteError(rig.track().getStep(1).paramLocks[UnifiedParam::MIX], 0.251f, 0.0001f);
            p.setModifiers(false, true);
            p.onEncoderChange(2, 1);
            expectWithinAbsoluteError(rig.track().getStep(1).paramLocks[UnifiedParam::MIX], 0.5f, 0.0001f);
            p.setModifiers(false, false);
            p.onEncoderChange(2, 1);
            expectWithinAbsoluteError(rig.track().getStep(1).paramLocks[UnifiedParam::MIX], 0.51f, 0.0001f);
            p.onPadRelease(1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.25f, 0.0001f);

            auto& fx = rig.fx;
            fx.setModifiers(true, false);
            fx.onEncoderChange(0, 128 - 5);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME), 0.795f, 0.0001f);
            fx.setModifiers(false, true);
            fx.onEncoderChange(0, 1);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME), 0.8f, 0.0001f, "Coarse stops at the default");
            fx.onEncoderChange(0, 1);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME), 1.0f, 0.0001f);
            fx.onEncoderChange(1, 127);
            expectWithinAbsoluteError(rig.value(MixParam::PAN), -0.5f, 0.0001f, "Bipolar: half-way, then the end");
            fx.onEncoderChange(6, 1);
            expectEquals(rig.track().length, 20, "LEN by four");
        }

        // ================================================================== pressure ranges
        beginTest("Func + Down / Up set the low / high end of the knob turned last; the same again clears it");
        {
            Rig rig;
            auto& p = rig.param;
            auto& params = rig.track().soundParams;
            expect(p.selectPageByName("FILTR"));
            const int lo = RangeParam::LO_BASE + UnifiedParam::RESONANCE;
            const int hi = RangeParam::HI_BASE + UnifiedParam::RESONANCE;

            p.onFuncDown();
            p.onFuncUp();
            expect(!p.pageHasRanges(), "No knob turned on this page yet: nothing happens");
            expectEquals(p.getLastTurnedCell(), -1);

            p.onEncoderChange(1, 10);                                   // RESO 0.20 -> 0.30
            p.onFuncDown();
            expect(params.count(lo) == 1 && params.count(hi) == 0);
            expectWithinAbsoluteError(params[lo], 0.30f, 0.001f);
            expectEquals(p.getPageName(), juce::String("FILTR"));
            expect(p.getSubPage().contains("RESO LO"), p.getSubPage());

            p.onEncoderChange(1, 50);                                   // RESO -> 0.80
            p.onFuncUp();
            expectWithinAbsoluteError(params[hi], 0.80f, 0.001f);
            expect(p.cellHasRange(1) && p.pageHasRanges() && !p.cellHasRange(0));

            // There is no time limit: the knob turned last keeps that role.
            p.onFuncUp();                                               // value unchanged: the high end goes
            expect(params.count(hi) == 0 && params.count(lo) == 1);
            p.onFuncDown();                                             // the value has changed since: the low end moves
            expectWithinAbsoluteError(params[lo], 0.80f, 0.001f);
            p.onFuncDown();                                             // unchanged now: cleared
            expect(params.count(lo) == 0);
            expect(!p.pageHasRanges());

            // Range ends are never sent to the engine.
            p.onEncoderChange(1, 1);
            p.onFuncDown();
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.81f, 0.001f);

            // Another page: no knob turned there yet.
            p.onArrowRight();
            p.onFuncUp();
            int ends = 0;
            for (auto& [id, v] : params) { juce::ignoreUnused(v); ends += RangeParam::isRangeParam(id) ? 1 : 0; }
            expectEquals(ends, 1);

            // A cell that cannot be locked (which LFO the page shows) takes no range.
            expect(p.selectPageByName("MOD"));
            p.onEncoderChange(0, 1);
            p.onFuncDown();
            expect(!p.pageHasRanges());

            // TRK FX works the same way.
            auto& fx = rig.fx;
            fx.onEncoderChange(1, 20);                                  // PAN +0.4
            fx.onFuncUp();
            expectWithinAbsoluteError(params[RangeParam::HI_BASE + MixParam::PAN], 0.4f, 0.001f);
            expect(fx.cellHasRange(1));
        }

        beginTest("Pressure: one end is enough - from the knob's value toward the end, back on release");
        {
            Rig rig;
            auto& p = rig.param;
            auto& params = rig.track().soundParams;
            expect(p.selectPageByName("FILTR"));

            // RESO (cell 2) has only a high end; the knob is at 0.2.
            params[RangeParam::HI_BASE + UnifiedParam::RESONANCE] = 0.9f;
            p.onPadPress(9, 100);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.2f, 0.001f, "No pressure: the knob's value");
            expectEquals(p.getHeldStepIndex(), -1, "A range pad does not hold a step");
            p.onPadAftertouch(9, 127);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.9f, 0.001f);
            p.onPadAftertouch(9, 64);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.2f + 0.7f * 64.0f / 127.0f, 0.005f);
            p.onPadRelease(9);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.2f, 0.001f, "Release returns to the knob's value");

            // Only a low end: pressure pulls down toward it.
            params.erase(RangeParam::HI_BASE + UnifiedParam::RESONANCE);
            params[RangeParam::LO_BASE + UnifiedParam::RESONANCE] = 0.05f;
            p.onPadPress(9, 100);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.2f, 0.001f);
            p.onPadAftertouch(9, 127);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.05f, 0.001f);
            p.onPadRelease(9);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.2f, 0.001f);

            // Both ends: low end with no pressure, high end at full; in the parameter's own scale.
            params[RangeParam::LO_BASE + UnifiedParam::CUTOFF] = 500.0f;
            params[RangeParam::HI_BASE + UnifiedParam::CUTOFF] = 4000.0f;
            params[UnifiedParam::CUTOFF] = 1000.0f;
            rig.audio.setTrackParameter(0, UnifiedParam::CUTOFF, 1000.0f);
            p.onPadPress(8, 100);
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 500.0f, 1.0f);
            p.onPadAftertouch(8, 127);
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 4000.0f, 5.0f);
            p.onPadAftertouch(8, 64);
            const float mid = rig.value(UnifiedParam::CUTOFF);
            expect(mid > 1300.0f && mid < 1550.0f, "Half way by pitch: " + juce::String(mid));

            // Two pads at once, and a page change lets both go.
            p.onPadPress(9, 100);
            p.onPadAftertouch(9, 127);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.05f, 0.001f);
            expect(rig.value(UnifiedParam::CUTOFF) > 1300.0f);
            p.onArrowRight();
            expectWithinAbsoluteError(rig.value(UnifiedParam::CUTOFF), 1000.0f, 1.0f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.2f, 0.001f);
            expectWithinAbsoluteError(params[UnifiedParam::CUTOFF], 1000.0f, 0.01f, "The stored value never moved");
            p.onPadRelease(8);
            p.onPadRelease(9);

            // Setting an end while the pad is down uses the knob's value, not where pressure holds it.
            p.onArrowLeft();
            p.onEncoderChange(1, 10);                                   // RESO 0.2 -> 0.3
            p.onPadPress(9, 100);
            p.onPadAftertouch(9, 127);
            p.onFuncUp();
            expectWithinAbsoluteError(params[RangeParam::HI_BASE + UnifiedParam::RESONANCE], 0.3f, 0.001f);
            p.onPadRelease(9);
            expectWithinAbsoluteError(rig.value(UnifiedParam::RESONANCE), 0.3f, 0.001f);
        }

        beginTest("Pad colours say what each pad does: range pads, steps by trig / lock, the held step, nothing past the end");
        {
            Rig rig;
            auto& p = rig.param;
            auto& track = rig.track();
            track.setLength(12);
            track.getStep(1).trig = true;
            track.getStep(2).trig = true;
            track.getStep(2).paramLocks[UnifiedParam::MIX] = 0.9f;
            expect(p.selectPageByName("FILTR"));

            std::array<Screen::PadColour, 16> c;
            expect(p.getPadColours(c), "The screen owns its pad colours");
            expect(same(c[0], ParamCellScreen::PAD_EMPTY));
            expect(same(c[1], ParamCellScreen::PAD_TRIG));
            expect(same(c[2], ParamCellScreen::PAD_LOCKED));
            expect(same(c[11], ParamCellScreen::PAD_EMPTY));
            for (int pad = 12; pad < 16; ++pad)
                expect(same(c[(size_t) pad], ParamCellScreen::PAD_OFF), "Steps 13-16 do not exist on a 12-step track");

            // A range on RESO (cell 2) turns bottom-row pad 2 into its pressure pad; only that one.
            track.soundParams[RangeParam::HI_BASE + UnifiedParam::RESONANCE] = 0.9f;
            p.getPadColours(c);
            expect(same(c[9], ParamCellScreen::PAD_RANGE));
            expect(same(c[8], ParamCellScreen::PAD_EMPTY) && same(c[10], ParamCellScreen::PAD_EMPTY));
            expect(p.isRangePad(9) && !p.isRangePad(8) && !p.isRangePad(1));
            p.onPadPress(9, 100);
            p.getPadColours(c);
            expect(same(c[9], ParamCellScreen::PAD_RANGE_DOWN));
            expect(!p.isStepHeld());
            p.onPadRelease(9);

            // The other bottom-row pads still hold their steps, and the held step is bright.
            p.onPadPress(10, 100);
            expectEquals(p.getHeldStepIndex(), 10);
            p.getPadColours(c);
            expect(same(c[10], ParamCellScreen::PAD_HELD));
            p.onPadRelease(10);
            p.onPadPress(2, 100);
            p.getPadColours(c);
            expect(same(c[2], ParamCellScreen::PAD_HELD), "Held wins over locked");
            p.onPadRelease(2);

            // On a page where that cell has no range the pad is a step again.
            p.onArrowRight();
            p.getPadColours(c);
            expect(same(c[9], ParamCellScreen::PAD_EMPTY));

            // The pads follow the step page.
            track.setLength(32);
            track.getStep(17).trig = true;
            p.onPadPageNext();
            p.getPadColours(c);
            expect(same(c[1], ParamCellScreen::PAD_TRIG) && same(c[2], ParamCellScreen::PAD_EMPTY));

            // TRK FX uses the same colours.
            std::array<Screen::PadColour, 16> f;
            expect(rig.fx.getPadColours(f));
            expect(same(f[1], ParamCellScreen::PAD_TRIG) && same(f[2], ParamCellScreen::PAD_LOCKED));
        }

        // ================================================================== locks
        beginTest("Hold + turn locks; the hold never toggles the step; > + turn removes one lock and restores the sound");
        {
            Rig rig;
            auto& p = rig.param;
            auto& track = rig.track();
            track.getStep(3).trig = true;
            track.getStep(3).notes.push_back({ 60, 1.0f });

            // Holding and releasing changes nothing, on a trig or on an empty step.
            p.onPadPress(3, 100);
            expect(p.isStepHeld());
            p.onPadRelease(3);
            expect(!p.isStepHeld());
            p.onPadPress(6, 100);
            p.onPadRelease(6);
            expect(track.getStep(3).trig && !track.getStep(6).trig);
            expect(track.getStep(3).paramLocks.empty() && track.getStep(6).paramLocks.empty());

            // Two locks on step 4.
            p.onPadPress(3, 100);
            p.onEncoderChange(2, 20);                                   // MIX 0.5 -> 0.7 on the step
            p.onEncoderChange(3, 10);                                   // PW
            auto& locks = track.getStep(3).paramLocks;
            expectEquals((int) locks.size(), 2);
            expectWithinAbsoluteError(locks[UnifiedParam::MIX], 0.7f, 0.001f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.5f, 0.001f, "The track's own value is untouched");
            expect(track.getStep(3).trig);

            // The step plays: the lock is in the sound.
            rig.audio.applyStepLocks(0, track.getStep(3));
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.7f, 0.001f);

            p.onEncoderLockClear(2);
            expect(locks.count(UnifiedParam::MIX) == 0 && locks.count(UnifiedParam::PULSE_WIDTH) == 1, "Only that cell's lock goes");
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.5f, 0.001f, "The sound is back at the base value");
            p.onEncoderLockClear(2);                                    // nothing left to clear
            p.onEncoderLockClear(7);                                    // a cell without a lock
            expectEquals((int) locks.size(), 1);
            p.onPadRelease(3);

            p.onEncoderLockClear(3);                                    // no step held: nothing
            expectEquals((int) locks.size(), 1);

            // A lock on a step without a trig makes it one (otherwise it would never be heard).
            p.onPadPress(6, 100);
            p.onEncoderChange(2, 1);
            p.onPadRelease(6);
            expect(track.getStep(6).trig && !track.getStep(6).notes.empty());
        }

        // ================================================================== control-all
        beginTest("Track + turn: the same click on every track with the same kind of engine, each from its own value");
        {
            Rig rig;                                                    // track 1: unified voice
            rig.add(1, EngineType::UnifiedVoice);
            rig.add(2, EngineType::FM);
            rig.add(3, EngineType::UnifiedVoice);
            auto& p = rig.param;
            rig.audio.setTrackParameter(1, UnifiedParam::MIX, 0.2f);
            const float fmBefore = rig.value(UnifiedParam::MIX, 2);

            p.onEncoderChangeAllTracks(2, 10);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX, 0), 0.6f, 0.001f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX, 1), 0.3f, 0.001f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX, 3), 0.6f, 0.001f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX, 2), fmBefore, 0.0001f, "The FM track has no such parameter");
            expectWithinAbsoluteError(rig.track(1).soundParams[UnifiedParam::MIX], 0.3f, 0.001f, "Stored on each track");
            expect(rig.track(2).soundParams.count(UnifiedParam::MIX) == 0);

            // Coarse works through it too.
            p.setModifiers(false, true);
            p.onEncoderChangeAllTracks(2, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX, 0), 0.75f, 0.001f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX, 1), 0.5f, 0.001f);
            p.setModifiers(false, false);

            // Which LFO the MOD page shows is not a track parameter.
            expect(p.selectPageByName("MOD"));
            p.onEncoderChangeAllTracks(0, 1);
            expectEquals(p.getModSelectedSource(), 1);

            // With a step held it is an ordinary lock on this track.
            expect(p.selectPageByName("OSC"));
            p.onPadPress(0, 100);
            p.onEncoderChangeAllTracks(2, 5);
            p.onPadRelease(0);
            expect(rig.track(0).getStep(0).paramLocks.count(UnifiedParam::MIX) == 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX, 1), 0.5f, 0.001f);

            // TRK FX: every track, whatever its engine.
            auto& fx = rig.fx;
            rig.audio.setTrackParameter(2, MixParam::VOLUME, 0.5f);
            fx.onEncoderChangeAllTracks(0, 128 - 10);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME, 0), 0.7f, 0.001f);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME, 2), 0.4f, 0.001f);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME, 3), 0.7f, 0.001f);
            expectWithinAbsoluteError(rig.value(MixParam::VOLUME, 5), 0.7f, 0.001f, "Tracks without an engine have a mixer too");

            // An insert effect's cells only where the same effect sits in that slot.
            rig.audio.addInsertEffect(0, EffectType::DelayEffect);
            rig.audio.addInsertEffect(1, EffectType::DelayEffect);
            rig.audio.addInsertEffect(2, EffectType::ReverbEffect);
            expect(fx.selectPageByName("FX"));
            const int cell = AudioEngine::EFFECT_PARAM_BASE + 1;        // slot 1, second parameter
            const float d0 = rig.value(cell, 0), d1 = rig.value(cell, 1), r2 = rig.value(cell, 2);
            fx.onEncoderChangeAllTracks(5, 5);
            expect(rig.value(cell, 0) > d0 && rig.value(cell, 1) > d1, "Both delays moved");
            expectWithinAbsoluteError(rig.value(cell, 2), r2, 0.0001f, "The reverb did not");
        }

        // ================================================================== copy / clear / paste
        beginTest("Copy / clear / paste act on a held step's locks, and decline without one");
        {
            Rig rig;
            auto& p = rig.param;
            auto& track = rig.track();
            expect(!p.onCopy() && !p.onClear() && !p.onPaste(), "No step held: the app's default applies");
            expect(!rig.fx.onCopy() && !rig.fx.onClear() && !rig.fx.onPaste());

            p.onPadPress(2, 100);
            p.onEncoderChange(2, 20);                                   // MIX 0.7
            p.onEncoderChange(3, 128 - 10);                             // PW
            expect(p.onCopy());
            p.onPadRelease(2);
            const ParamMap copied = track.getStep(2).paramLocks;
            expectEquals((int) copied.size(), 2);

            p.onPadPress(9, 100);
            expect(p.onPaste());
            expect(track.getStep(9).paramLocks == copied, "Step 10 has step 3's locks");
            expect(track.getStep(9).trig, "A step with locks is a trig");
            // (Taking a paste back is ScreenManager's undo history: the screen just pastes again.)
            expect(p.onPaste());
            expect(track.getStep(9).paramLocks == copied, "The same again is the same paste");
            p.onPadRelease(9);

            // Clear: the step's locks go and the sound returns to its own values.
            rig.audio.applyStepLocks(0, track.getStep(2));
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.7f, 0.001f);
            p.onPadPress(2, 100);
            expect(p.onClear());
            expect(track.getStep(2).paramLocks.empty());
            expectWithinAbsoluteError(rig.value(UnifiedParam::MIX), 0.5f, 0.001f);
            expect(track.getStep(2).trig, "Clearing locks does not remove the trig");
            expect(p.onClear());
            expect(track.getStep(2).paramLocks.empty(), "Clear again is the same clear (the undo is ScreenManager's)");
            p.onEncoderChange(2, 1);                                    // a lock again
            expect(p.onClear());
            expect(track.getStep(2).paramLocks.empty(), "After an edit, clear clears again");
            p.onPadRelease(2);

            // One clipboard for PARAM and TRK FX.
            rig.fx.onPadPress(12, 100);
            expect(rig.fx.onPaste());
            expect(track.getStep(12).paramLocks == copied);
            rig.fx.onPadRelease(12);
            expect(track.getStep(9).paramLocks == copied, "Other steps are untouched");
        }

        // ================================================================== the filter picture
        beginTest("Filter picture: appears only after a filter cell is turned, over the other half of the grid");
        {
            Rig rig;
            auto& p = rig.param;
            DisplayRenderer with, without;
            const int topY0 = ParamCellScreen::PICTURE_TOP_Y, topY1 = topY0 + ParamCellScreen::PICTURE_TOP_H - 1;
            const int botY0 = ParamCellScreen::PICTURE_BOTTOM_Y, botY1 = botY0 + ParamCellScreen::PICTURE_BOTTOM_H - 1;
            expectEquals(topY1 + 1, botY0 - 1, "One free row between the halves");

            // Not on a page that is not about the filter.
            p.onEncoderChange(2, 1);
            expect(!p.isFilterPictureActive(), "OSC is not a filter page");

            // On FILTR, before any turn: the plain grid (gaps between the cells).
            expect(p.selectPageByName("FILTR"));
            expect(!p.isFilterPictureActive());
            without.clear();
            p.render(without);
            expect(litColumns(without, botY0, botY1) < 126 && litColumns(without, topY0, topY1) < 126);

            // Turn RESO (top row): the curve covers the bottom row of cells, the top row stays.
            p.onEncoderChange(1, 30);
            expect(p.isFilterPictureActive());
            with.clear();
            p.render(with);
            p.expireFilterPicture();
            expect(!p.isFilterPictureActive(), "Gone after its second");
            without.clear();
            p.render(without);
            expect(rowsEqual(with, without, 0, botY0 - 1), "The turned knob's row is untouched");
            expect(rowsEqual(with, without, botY1 + 1, 63), "The page strip is untouched");
            expect(!rowsEqual(with, without, botY0, botY1), "The bottom half shows something else");
            expectEquals(litColumns(with, botY0, botY1), 128, "A curve crosses every column");

            // What is drawn is the measured response of the track's filter.
            {
                FilterCurve::Settings s = FilterCurve::single(FilterModel::SVF, 0, rig.value(UnifiedParam::CUTOFF),
                                                              rig.value(UnifiedParam::RESONANCE));
                DisplayRenderer expected;
                expected.clear();
                FilterCurve::draw(expected, 0, botY0, 128, ParamCellScreen::PICTURE_BOTTOM_H, FilterCurve::response(s, 128), s.cutoffHz[0]);
                expect(rowsEqual(with, expected, botY0, botY1), "The picture is FilterCurve's");
            }

            // Turn MORP (bottom row): the curve covers the top row of cells, the bottom row stays.
            p.onEncoderChange(6, 5);
            with.clear();
            p.render(with);
            p.expireFilterPicture();
            without.clear();
            p.render(without);
            expect(rowsEqual(with, without, topY1 + 1, 63), "The bottom row and the strip are untouched");
            expect(rowsEqual(with, without, 0, topY0 - 1));
            expectEquals(litColumns(with, topY0, topY1), 128);
            expect(litColumns(without, topY0, topY1) < 126);

            // Leaving the page ends it; the other filter pages have it too.
            p.onEncoderChange(0, 1);
            p.onArrowRight();
            expect(!p.isFilterPictureActive());
            p.onEncoderChange(3, 1);                                    // RESB on FLT B
            expect(p.isFilterPictureActive());
            p.onArrowRight();
            p.onEncoderChange(0, 1);                                    // ATK on F ENV: the envelope instead
            expect(!p.isFilterPictureActive() && p.isEnvelopePictureActive());
            p.onEncoderChange(6, 1);                                    // VEL on F ENV: the filter again
            expect(p.isFilterPictureActive() && !p.isEnvelopePictureActive());
            p.onSectionNext();
            p.onEncoderChange(0, 1);                                    // AMP
            expect(!p.isFilterPictureActive());

            // The picture follows the type: a different model draws a different curve.
            expect(p.selectPageByName("FILTR"));
            p.onEncoderChange(1, 1);
            with.clear();
            p.render(with);
            rig.audio.setTrackParameter(0, UnifiedParam::MODEL_A, 4.0f);            // comb
            without.clear();
            p.render(without);
            expect(!rowsEqual(with, without, botY0, botY1));
        }

        beginTest("Filter picture on the other engines' filter pages, and on the filter insert effect");
        {
            for (auto type : { EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable, EngineType::WaveSeq, EngineType::Sampler })
            {
                Rig rig(type);
                const juce::String name = "Engine " + juce::String((int) type);
                expect(rig.param.selectPageByName("FILTR"), name);
                expectEquals(rig.param.getEncoderName(0), juce::String("CUT"), name);
                expect(!rig.param.isFilterPictureActive(), name);
                rig.param.onEncoderChange(0, 128 - 30);
                expect(rig.param.isFilterPictureActive(), name);
                DisplayRenderer display;
                display.clear();
                rig.param.render(display);
                expectEquals(litColumns(display, ParamCellScreen::PICTURE_BOTTOM_Y,
                                        ParamCellScreen::PICTURE_BOTTOM_Y + ParamCellScreen::PICTURE_BOTTOM_H - 1), 128, name);
                rig.param.onSectionNext();
                rig.param.onEncoderChange(0, 1);
                expect(!rig.param.isFilterPictureActive(), name + ": not outside the filter section");
            }

            // No filter, no picture.
            Rig drums(EngineType::Drum);
            drums.param.onEncoderChange(0, 1);
            expect(!drums.param.isFilterPictureActive());

            // TRK FX: only the FX page, and only with the filter effect in the selected slot.
            Rig rig(EngineType::Drum);
            auto& fx = rig.fx;
            fx.onEncoderChange(0, 1);
            expect(!fx.isFilterPictureActive());
            rig.audio.addInsertEffect(0, EffectType::DelayEffect);
            rig.audio.addInsertEffect(0, EffectType::FilterEffectType);
            expect(fx.selectPageByName("FX"));
            fx.onEncoderChange(4, 1);                                   // the delay's TIME
            expect(!fx.isFilterPictureActive());
            fx.onEncoderChange(0, 1);                                   // slot 2: the filter
            fx.expireFilterPicture();
            expectEquals(fx.getEncoderName(4), juce::String("CUT"));
            fx.onEncoderChange(5, 30);                                  // its RESO
            expect(fx.isFilterPictureActive());
            DisplayRenderer display;
            display.clear();
            fx.render(display);
            expectEquals(litColumns(display, ParamCellScreen::PICTURE_TOP_Y,
                                    ParamCellScreen::PICTURE_TOP_Y + ParamCellScreen::PICTURE_TOP_H - 1), 128);
        }

        beginTest("Vocabulary: TYPE / MODE / CUT / RESO / ROUT on the filter pages, DEST on every destination cell");
        {
            Rig rig;
            auto& p = rig.param;
            expect(p.selectPageByName("FILTR"));
            const char* filtr[8] = { "CUT", "RESO", "ENV", "DRIV", "TYPE", "MODE", "MORP", "FFM" };
            for (int i = 0; i < 8; ++i)
                expectEquals(p.getEncoderName(i), juce::String(filtr[i]));
            expect(p.selectPageByName("FLT B"));
            expectEquals(p.getEncoderName(4), juce::String("ROUT"));
            expect(p.selectPageByName("MOD"));
            expectEquals(p.getEncoderName(7), juce::String("DEST"));
            expect(p.selectPageByName("MENV"));
            expectEquals(p.getEncoderName(7), juce::String("DEST"));

            for (auto type : { EngineType::FM, EngineType::Sampler })
            {
                Rig other(type);
                expect(other.param.selectPageByName("FILTR"));
                auto page = other.param.getCurrentPage();
                juce::StringArray labels;
                for (auto& cell : page.params) labels.add(cell.label);
                for (auto word : { "CUT", "RESO", "TYPE", "MODE" })
                    expect(labels.contains(word), juce::String(word) + " on engine " + juce::String((int) type));
            }
        }
    }
};
static ParamGrammarTests ParamGrammarTestsInstance;
