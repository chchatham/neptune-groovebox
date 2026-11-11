#pragma once

#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/UnifiedVoiceEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include <juce_core/juce_core.h>

class ModPageDynamicTests : public juce::UnitTest
{
public:
    ModPageDynamicTests() : UnitTest("MOD Page Dynamic") {}

    // A screen on a real engine, already on the MOD page.
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        std::unique_ptr<ParamPageScreen> screen;

        Rig()
        {
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            screen = std::make_unique<ParamPageScreen>(seq, &audio);
            screen->selectPageByName("MOD");
        }
        UnifiedVoiceEngine& engine() { return *dynamic_cast<UnifiedVoiceEngine*>(audio.getEngine(0)); }
    };

    // What the cell of an engine parameter shows (its spec's text for the engine's value).
    static juce::String cellText(Rig& rig, int cell)
    {
        const auto spec = rig.screen->getCurrentPage().params[(size_t) cell];
        return spec.format(rig.engine().getParameter(spec.id));
    }

    void runTest() override
    {
        beginTest("MOD, MOD2 and MENV are the last pages of an engine with modulation; engines without it have none");
        {
            Rig rig;
            expectEquals(rig.screen->getPageName(), juce::String("MOD"));
            expectEquals(rig.screen->getCurrentPageIndex(), rig.screen->getPageCount() - 3);
            rig.screen->setCurrentPageIndex(rig.screen->getPageCount() - 2);
            expectEquals(rig.screen->getPageName(), juce::String("MOD2"));
            rig.screen->setCurrentPageIndex(rig.screen->getPageCount() - 1);
            expectEquals(rig.screen->getPageName(), juce::String("MENV"));

            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Drum));
            ParamPageScreen drumScreen(seq, &audio);
            expect(!drumScreen.selectPageByName("MOD"), "Drum engine has no MOD page");
            expect(!drumScreen.selectPageByName("MOD2") && !drumScreen.selectPageByName("MENV"));
        }

        beginTest("Default: LFO1 selected, cells are LFO LFOS RATE DPTH / SHPE TRIG SYNC DEST");
        {
            Rig rig;
            expectEquals(rig.screen->getModSelectedSource(), 0);
            const char* expected[8] = { "LFO", "LFOS", "RATE", "DPTH", "SHPE", "TRIG", "SYNC", "DEST" };
            for (int i = 0; i < 8; ++i)
                expectEquals(rig.screen->getEncoderName(i), juce::String(expected[i]));
        }

        beginTest("The LFO cell selects LFO 1-4 for both LFO pages; MENV selects its own envelope");
        {
            Rig rig;
            rig.screen->onEncoderChange(0, 1);
            expectEquals(rig.screen->getModSelectedSource(), 1);
            expectEquals(rig.screen->getCurrentPage().params[2].id, ModParam::lfoParamId(1, ModParam::LFO_RATE));
            rig.screen->onEncoderChange(0, 10);          // stops at LFO4
            expectEquals(rig.screen->getModSelectedSource(), 3);

            expect(rig.screen->selectPageByName("MOD2"));
            const char* start[8] = { "LFO", "PHAS", "DLY", "FADE", "WHL", "VEL", "KTRK", "PRES" };
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(rig.screen->getEncoderName(i), juce::String(start[i]));
                expect(rig.screen->getCurrentPage().params[(size_t) i].isValid(), "all eight cells are real");
            }
            expectEquals(rig.screen->getCurrentPage().params[6].id, ModParam::lfoExt2ParamId(3, ModParam::LFO_KTRK));
            expectEquals(rig.screen->getCurrentPage().params[1].id, ModParam::lfoExtParamId(3, ModParam::LFO_PHASE));
            rig.screen->onEncoderChange(0, 127);         // one step back, on this page too
            expectEquals(rig.screen->getModSelectedSource(), 2);

            expect(rig.screen->selectPageByName("MENV"));
            const char* env[8] = { "ENV", "ENVS", "ATK", "DEC", "SUS", "REL", "DPTH", "DEST" };
            for (int i = 0; i < 8; ++i)
                expectEquals(rig.screen->getEncoderName(i), juce::String(env[i]));
            rig.screen->onEncoderChange(0, 2);
            expectEquals(rig.screen->getModSelectedEnv(), 2);
            expectEquals(rig.screen->getCurrentPage().params[2].id, ModParam::envParamId(2, ModParam::ENV_ATK));
            expectEquals(rig.screen->getModSelectedSource(), 2, "the LFO selection is separate");
        }

        beginTest("RATE shows Hz with SYNC off and the division name with SYNC on; both values are kept");
        {
            Rig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            expect(cellText(rig, 2).containsOnly("0123456789."), "Hz: " + cellText(rig, 2));
            const float hz = rig.engine().getParameter(ModParam::lfoParamId(0, ModParam::LFO_RATE));

            rig.screen->onEncoderChange(6, 1);           // SYNC on
            expectEquals(rig.screen->getEncoderName(2), juce::String("RATE"));
            expectEquals(cellText(rig, 2), juce::String("1/4"));
            rig.screen->onEncoderChange(2, 5);           // five divisions faster: 1/8T
            expectEquals(cellText(rig, 2), juce::String("1/8T"));
            expectEquals((int) track.soundParams[ModParam::lfoExtParamId(0, ModParam::LFO_DIV)], 13);
            expectEquals(rig.engine().getVoice(0).getLFOBank().getLFO(0).getSyncDivision(), 13);
            expectWithinAbsoluteError(rig.engine().getParameter(ModParam::lfoParamId(0, ModParam::LFO_RATE)), hz, 0.0001f);

            rig.screen->onEncoderChange(6, 127);         // SYNC off: the Hz value is back
            expect(cellText(rig, 2).containsOnly("0123456789."));
            expectWithinAbsoluteError(rig.engine().getVoice(0).getLFOBank().getLFO(0).getEffectiveRate(), hz, 0.0001f);
        }

        beginTest("MOD2 cells edit the engine and are stored on the track");
        {
            Rig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            expect(rig.screen->selectPageByName("MOD2"));
            rig.screen->onEncoderChange(1, 25);          // PHAS
            rig.screen->onEncoderChange(2, 30);          // DLY
            rig.screen->onEncoderChange(3, 50);          // FADE
            auto& lfo = rig.engine().getVoice(0).getLFOBank().getLFO(0);
            expectWithinAbsoluteError(lfo.getStartPhase(), 0.25f, 0.001f);
            expectWithinAbsoluteError(lfo.getDelayTime(), 0.30f, 0.001f);
            expectWithinAbsoluteError(lfo.getFadeTime(), 0.50f, 0.001f);
            expectWithinAbsoluteError(track.soundParams[ModParam::lfoExtParamId(0, ModParam::LFO_PHASE)], 0.25f, 0.001f);
            expectWithinAbsoluteError(track.soundParams[ModParam::lfoExtParamId(0, ModParam::LFO_DELAY)], 0.30f, 0.001f);
            expectWithinAbsoluteError(track.soundParams[ModParam::lfoExtParamId(0, ModParam::LFO_FADE)], 0.50f, 0.001f);

            // Bottom row: WHL VEL KTRK PRES, -1..1 around 0 (one step = 0.02).
            const int offs[4] = { ModParam::LFO_WHEEL, ModParam::LFO_VEL, ModParam::LFO_KTRK, ModParam::LFO_PRES };
            for (int c = 0; c < 4; ++c)
            {
                const int id = ModParam::lfoExt2ParamId(0, offs[c]);
                expectWithinAbsoluteError(rig.engine().getParameter(id), 0.0f, 1.0e-6f);
                rig.screen->onEncoderChange(4 + c, 10 + c);
                const float expected = 0.02f * (float) (10 + c);
                expectWithinAbsoluteError(rig.engine().getParameter(id), expected, 0.001f);
                expectWithinAbsoluteError(track.soundParams[id], expected, 0.001f);
                rig.screen->onEncoderChange(4 + c, 127 - 40);      // 41 steps down: negative
                expect(rig.engine().getParameter(id) < -0.5f);
            }
            expectWithinAbsoluteError(lfo.getKeyTrack(), rig.engine().getParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_KTRK)), 1.0e-6f);
        }

        beginTest("SHPE, TRIG and DEST offer the whole lists");
        {
            Rig rig;
            auto page = rig.screen->getCurrentPage();
            expectEquals(page.params[4].numChoices, (int) LFO::Shape::COUNT);
            expectEquals(page.params[5].numChoices, (int) LFO::TriggerMode::COUNT);
            expectEquals(page.params[7].numChoices, (int) UnifiedVoice::LfoDest::Count);
            rig.screen->onEncoderChange(4, 30);
            expect(rig.engine().getVoice(0).getLFOBank().getLFO(0).getShape() == LFO::Shape::ExpDown);
            expectEquals(cellText(rig, 4), juce::String("EXPD"));
            rig.screen->onEncoderChange(5, 30);
            expect(rig.engine().getVoice(0).getLFOBank().getLFO(0).getTriggerMode() == LFO::TriggerMode::Hold);
            expectEquals(cellText(rig, 5), juce::String("HOLD"));
            // 23 clicks: the last of the original destinations; the page cells follow them.
            rig.screen->onEncoderChange(7, 23);
            expect(rig.engine().getVoice(0).getLfoDest(0) == UnifiedVoice::LfoDest::Lfo4Depth);
            expectEquals(cellText(rig, 7), juce::String("DPT4"));
            for (int turn = 0; turn < 4; ++turn)
                rig.screen->onEncoderChange(7, 30);
            expectEquals((int) rig.engine().getVoice(0).getLfoDest(0), (int) UnifiedVoice::LfoDest::Count - 1);
            expectEquals(cellText(rig, 7), juce::String(UnifiedVoice::lfoDestLabels()[(int) UnifiedVoice::LfoDest::Count - 1]));
            // Every label fits a cell.
            for (int i = 0; i < (int) UnifiedVoice::LfoDest::Count; ++i)
                expect(juce::String(UnifiedVoice::lfoDestLabels()[i]).length() <= 4);
        }

        beginTest("Count encoder adds and removes LFOs (1..4) in the engine");
        {
            Rig rig;
            rig.screen->onEncoderChange(1, 1);
            expectEquals(rig.engine().getVoice(0).getLFOBank().getActiveCount(), 2);
            rig.screen->onEncoderChange(1, 5);
            expectEquals(rig.engine().getVoice(0).getLFOBank().getActiveCount(), 4);
            rig.screen->onEncoderChange(1, 118);   // -10
            expectEquals(rig.engine().getVoice(0).getLFOBank().getActiveCount(), 1);
        }

        beginTest("Count encoder adds and removes extra envelopes (0..4)");
        {
            Rig rig;
            expect(rig.screen->selectPageByName("MENV"));
            rig.screen->onEncoderChange(1, 2);
            expectEquals(rig.engine().getVoice(0).getEnvelopeChain().getStageCount(), 2);
            rig.screen->onEncoderChange(1, 118);
            expectEquals(rig.engine().getVoice(0).getEnvelopeChain().getStageCount(), 0);
        }

        beginTest("LFO cells edit the engine and are stored on the track");
        {
            Rig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            float rateBefore = rig.engine().getVoice(0).getLFOBank().getLFO(0).getRate();
            rig.screen->onEncoderChange(2, 10);          // RATE up
            float rateAfter = rig.engine().getVoice(0).getLFOBank().getLFO(0).getRate();
            expect(rateAfter > rateBefore, "LFO rate should rise");
            expectWithinAbsoluteError(track.soundParams[ModParam::lfoParamId(0, ModParam::LFO_RATE)], rateAfter, 0.001f);

            rig.screen->onEncoderChange(4, 2);           // SHPE two steps: SIN -> SQR
            expect(rig.engine().getVoice(0).getLFOBank().getLFO(0).getShape() == LFO::Shape::Square);

            rig.screen->onEncoderChange(7, 2);           // DEST two steps: OFF -> CUT
            expect(rig.engine().getVoice(0).getLfoDest(0) == UnifiedVoice::LfoDest::Cutoff);
        }

        beginTest("PW is heard on every waveform, differently below and above 50, and 50 leaves each as it was");
        {
            auto render = [](int wave, float pw)
            {
                UnifiedVoiceEngine e(1);
                e.prepareToPlay(44100.0, 512);
                e.setParameter(UnifiedParam::DRIFT, 0.0f);
                e.getVoice(0).getAnalogCharacter().enabled = false;
                e.setParameter(UnifiedParam::MIX, 0.0f);                 // oscillator 1 alone
                e.setParameter(UnifiedParam::CUTOFF, 20000.0f);
                e.setParameter(UnifiedParam::WAVE1, (float) wave);
                e.setParameter(UnifiedParam::PULSE_WIDTH, pw);
                e.noteOn(48, 1.0f, -1);
                juce::MidiBuffer midi;
                std::vector<float> out;
                for (int b = 0; b < 20; ++b)
                {
                    juce::AudioBuffer<float> buf(2, 512);
                    buf.clear();
                    e.processBlock(buf, midi);
                    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
                }
                return out;
            };
            const char* names[] = { "SIN", "SAW", "SQR", "TRI", "PLS" };
            for (int wave = 0; wave < 5; ++wave)
            {
                const auto centre = render(wave, 0.5f), again = render(wave, 0.5f), narrow = render(wave, 0.15f);
                // The two directions are two different sounds: 30 is not 70 (nor its mirror image).
                const auto low = render(wave, 0.3f), high = render(wave, 0.7f);
                double same = 0.0, mirrored = 0.0, lowLevel = 0.0;
                for (size_t i = 2048; i < low.size(); ++i)
                {
                    same += std::pow((double) low[i] - high[i], 2.0);
                    mirrored += std::pow((double) low[i] + high[i], 2.0);
                    lowLevel += std::pow((double) low[i], 2.0);
                }
                expect(same > 0.05 * lowLevel && mirrored > 0.05 * lowLevel,
                       juce::String(names[wave]) + ": PW 30 and PW 70 are different sounds");
                // ... and each meets the plain wave smoothly: one notch from 50 is nearly the plain wave.
                for (float near : { 0.49f, 0.51f })
                {
                    const auto close = render(wave, near);
                    double d = 0.0, l = 0.0;
                    for (size_t i = 2048; i < close.size(); ++i)
                    {
                        d += std::pow((double) close[i] - centre[i], 2.0);
                        l += std::pow((double) centre[i], 2.0);
                    }
                    // (A pulse whose edge moves by 1 % of the cycle differs by 4 % in energy: that is the step itself.)
                    expect(d < 0.06 * l, juce::String(names[wave]) + ": continuous at 50 (" + juce::String(near, 2) + ": "
                                         + juce::String(d / l, 4) + ")");
                }
                double diff = 0.0, level = 0.0, repeat = 0.0;
                for (size_t i = 2048; i < centre.size(); ++i)
                {
                    diff += std::pow((double) centre[i] - narrow[i], 2.0);
                    repeat += std::pow((double) centre[i] - again[i], 2.0);
                    level += std::pow((double) centre[i], 2.0);
                }
                expect(level > 0.0 && repeat == 0.0, juce::String(names[wave]) + " renders the same twice");
                expect(diff > 0.05 * level, juce::String(names[wave]) + ": PW 15 sounds different from PW 50 ("
                                            + juce::String(diff / level, 3) + ")");
                for (auto v : narrow)
                    if (!std::isfinite(v) || std::abs(v) > 4.0f) { expect(false, juce::String(names[wave]) + " stays bounded"); break; }
            }
            // SQR at 50 is still the square it was: the same as PLS at 50.
            const auto sq = render(2, 0.5f), pls = render(4, 0.5f);
            double d = 0.0;
            for (size_t i = 0; i < sq.size(); ++i) d += std::abs((double) sq[i] - pls[i]);
            expect(d < 1.0e-3 * (double) sq.size());
        }

        beginTest("Oscillator slots: any of the three can be a wavetable, FM, formant or harmonic oscillator");
        {
            using W = UnifiedVoice::OscWaveform;
            struct Setup { int wave[3] = { 1, 1, 0 }; float mix = 0.0f, lvl3 = 0.0f; int note = 48; float a = -1.0f, b = -1.0f; int stack = 0; float lfoToPw = 0.0f; };
            auto render = [](const Setup& s)
            {
                UnifiedVoiceEngine e(1);
                e.prepareToPlay(44100.0, 512);
                e.setParameter(UnifiedParam::DRIFT, 0.0f);
                e.getVoice(0).getAnalogCharacter().enabled = false;
                e.setParameter(UnifiedParam::CUTOFF, 20000.0f);
                e.setParameter(UnifiedParam::MIX, s.mix);
                e.setParameter(UnifiedParam::SUB, s.lvl3);
                e.setParameter(UnifiedParam::WAVE1, (float) s.wave[0]);
                e.setParameter(UnifiedParam::WAVE2, (float) s.wave[1]);
                e.setParameter(UnifiedParam::WAVE3, (float) s.wave[2]);
                if (s.a >= 0.0f) e.setParameter(UnifiedParam::SRC_A1, s.a);
                if (s.b >= 0.0f) e.setParameter(UnifiedParam::SRC_B1, s.b);
                e.setParameter(UnifiedParam::STACK, (float) s.stack);
                if (s.lfoToPw != 0.0f)
                {
                    e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_RATE), 6.0f);
                    e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEPTH), s.lfoToPw);
                    e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEST), (float) (int) UnifiedVoice::LfoDest::PulseWidth);
                }
                e.noteOn(s.note, 1.0f, -1);
                juce::MidiBuffer midi;
                std::vector<float> out;
                for (int b = 0; b < 24; ++b)
                {
                    juce::AudioBuffer<float> buf(2, 512);
                    buf.clear();
                    e.processBlock(buf, midi);
                    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
                }
                return out;
            };
            auto power = [](const std::vector<float>& x)
            {
                double p = 0.0;
                for (size_t i = 2048; i < x.size(); ++i) p += (double) x[i] * x[i];
                return p;
            };
            auto differ = [&](const std::vector<float>& x, const std::vector<float>& y)
            {
                double d = 0.0;
                for (size_t i = 2048; i < x.size(); ++i) d += std::pow((double) x[i] - y[i], 2.0);
                return d / std::max(1.0e-12, power(x));
            };
            auto bounded = [](const std::vector<float>& x)
            {
                for (auto v : x) if (!std::isfinite(v) || std::abs(v) > 4.0f) return false;
                return true;
            };
            const char* names[] = { "WT", "FM", "FORM", "HARM" };
            const int kinds[] = { (int) W::Wavetable, (int) W::FM, (int) W::Formant, (int) W::Harmonic };
            Setup saw;
            const auto sawOut = render(saw);
            std::vector<std::vector<float>> plain;

            for (int k = 0; k < 4; ++k)
            {
                const juce::String who = names[k];
                Setup s; s.wave[0] = kinds[k];
                const auto out = render(s);
                plain.push_back(out);
                expect(power(out) > 0.02 * power(sawOut) && bounded(out), who + " sounds in slot 1");
                expect(differ(out, render(s)) == 0.0, who + " renders the same twice");

                // Its two controls both change the sound.
                Setup a = s; a.a = 0.9f;
                Setup b = s; b.b = (k == 0 ? 6.0f : (k == 1 ? 5.0f : 1.0f));
                expect(differ(out, render(a)) > 0.02, who + ": the first SRC cell is heard");
                expect(differ(out, render(b)) > 0.02, who + ": the second SRC cell is heard");

                // Whatever modulates PW moves the first control: an LFO on PW animates the slot.
                Setup lfo = s; lfo.lfoToPw = 1.0f;
                expect(differ(out, render(lfo)) > 0.01, who + ": an LFO on PW moves it");

                // The same kind in slot 2 and in slot 3 (the mixer turned to each).
                Setup two; two.wave[1] = kinds[k]; two.mix = 1.0f;
                Setup three; three.wave[2] = kinds[k]; three.mix = 0.0f; three.lvl3 = 1.0f; three.wave[0] = 1;
                expect(differ(render(two), [&] { Setup x; x.mix = 1.0f; return render(x); }()) > 0.02, who + " in slot 2");
                expect(differ(render(three), [&] { Setup x; x.lvl3 = 1.0f; return render(x); }()) > 0.02, who + " in slot 3");

                // Stacked, and at the top of the keyboard: still bounded and sounding.
                Setup stacked = s; stacked.stack = 3;
                Setup high = s; high.note = 108;
                expect(bounded(render(stacked)) && power(render(stacked)) > 0.0, who + " stacks");
                expect(bounded(render(high)) && power(render(high)) > 0.0, who + " at the top of the keyboard");
            }
            for (int i = 0; i < 4; ++i)
                for (int j = i + 1; j < 4; ++j)
                    expect(differ(plain[(size_t) i], plain[(size_t) j]) > 0.05, juce::String(names[i]) + " and " + names[j] + " are different oscillators");

            // The SRC page shows two cells for each slot that is not an analog wave, named by slot and kind.
            UnifiedVoiceEngine e(1);
            auto srcPage = [&e] { for (auto& p : e.getParamPages()) if (p.name == "SRC") return p; return ParamPage {}; };
            for (int c = 0; c < 8; ++c) expect(!srcPage().params[(size_t) c].isValid(), "All analog: nothing on SRC");
            e.setParameter(UnifiedParam::WAVE1, (float) (int) W::Wavetable);
            e.setParameter(UnifiedParam::WAVE2, (float) (int) W::FM);
            e.setParameter(UnifiedParam::WAVE3, (float) (int) W::Harmonic);
            const auto page = srcPage();
            const char* expected[6] = { "1POS", "1TBL", "2IDX", "2RAT", "3TLT", "3ODD" };
            for (int c = 0; c < 6; ++c)
            {
                expectEquals(juce::String(page.params[(size_t) c].label), juce::String(expected[c]));
                expectWithinAbsoluteError(e.getParameter(page.params[(size_t) c].id), page.params[(size_t) c].defaultVal, 0.0001f);
            }
            expectWithinAbsoluteError(e.getParameter(UnifiedParam::SRC_B2), 2.0f, 0.0001f, "FM starts at ratio 2");
            e.setParameter(UnifiedParam::SRC_B2, 7.0f);
            e.setParameter(UnifiedParam::WAVE2, (float) (int) W::Formant);
            expectEquals(juce::String(srcPage().params[2].label), juce::String("2FRM"));
            expectWithinAbsoluteError(e.getParameter(UnifiedParam::SRC_B2), 0.5f, 0.0001f, "A new kind starts from its own settings");
        }

        beginTest("An LFO routed to the cutoff changes the sound");
        {
            auto render = [](bool routed)
            {
                UnifiedVoiceEngine e(1);
                e.prepareToPlay(44100.0, 512);
                e.setParameter(UnifiedParam::DRIFT, 0.0f);
                e.getVoice(0).getAnalogCharacter().enabled = false;
                e.setParameter(UnifiedParam::CUTOFF, 400.0f);
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_RATE), 8.0f);
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEPTH), 1.0f);
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEST), routed ? 2.0f : 0.0f);
                e.noteOn(48, 1.0f, -1);
                juce::MidiBuffer midi;
                std::vector<float> out;
                for (int b = 0; b < 40; ++b)
                {
                    juce::AudioBuffer<float> buf(2, 512);
                    buf.clear();
                    e.processBlock(buf, midi);
                    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
                }
                return out;
            };
            auto dry = render(false), wet = render(true);
            double diff = 0.0;
            for (size_t i = 0; i < dry.size(); ++i)
                diff += std::abs(dry[i] - wet[i]);
            expect(diff > 1.0, "Routing the LFO to the cutoff must change the output");
        }

        beginTest("Hold a step and turn: parameter lock on a MOD parameter");
        {
            Rig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            rig.screen->onPadPress(2, 100);
            rig.screen->onEncoderChange(3, 10);          // DPTH
            rig.screen->onPadRelease(2);
            auto& locks = track.getStep(2).paramLocks;
            expect(locks.find(ModParam::lfoParamId(0, ModParam::LFO_DEPTH)) != locks.end(),
                   "Param lock should exist on held step");
            // The track's own value is untouched.
            expectWithinAbsoluteError(rig.engine().getParameter(ModParam::lfoParamId(0, ModParam::LFO_DEPTH)), 0.3f, 0.001f);
        }

        beginTest("Name bar: the screen is PARAM, the sub-page is the page name (the framework adds the track)");
        {
            Rig rig;
            expectEquals(rig.screen->getName(), juce::String("PARAM"));
            expectEquals(rig.screen->getSubPage(), juce::String("MOD"));
            rig.screen->onPadPress(2, 100);
            expectEquals(rig.screen->getSubPage(), juce::String("S3 MOD"), "A held step is named");
            rig.screen->onPadRelease(2);
            expect(rig.screen->getPageCount() > 1 && rig.screen->getPageIndex() == rig.screen->getPageCount() - 3);
        }
    }
};

static ModPageDynamicTests modPageDynamicTests;

// =========================================================================
// UnifiedVoiceEngine MOD Param Handling Tests
// =========================================================================

class EngineModParamTests : public juce::UnitTest
{
public:
    EngineModParamTests() : UnitTest("Engine MOD Params") {}

    void runTest() override
    {
        beginTest("LFO rate param reaches voice LFO");
        {
            UnifiedVoiceEngine engine(2);
            engine.prepareToPlay(44100.0, 512);

            // Set LFO1 rate via param
            engine.setParameter(ModParam::lfoParamId(0, ModParam::LFO_RATE), 5.0f);

            auto& lfo = engine.getVoice(0).getLFOBank().getLFO(0);
            expectWithinAbsoluteError(lfo.getRate(), 5.0f, 0.001f);
        }

        beginTest("LFO shape param sets voice LFO shape");
        {
            UnifiedVoiceEngine engine(2);
            engine.prepareToPlay(44100.0, 512);

            engine.setParameter(ModParam::lfoParamId(0, ModParam::LFO_SHAPE), 2.0f);

            auto& lfo = engine.getVoice(0).getLFOBank().getLFO(0);
            expectEquals(static_cast<int>(lfo.getShape()), static_cast<int>(LFO::Shape::Square));
        }

        beginTest("LFO count param adds/removes LFOs on voices");
        {
            UnifiedVoiceEngine engine(2);
            engine.prepareToPlay(44100.0, 512);

            expectEquals(engine.getVoice(0).getLFOBank().getActiveCount(), 1);

            engine.setParameter(ModParam::LFO_COUNT, 3.0f);
            expectEquals(engine.getVoice(0).getLFOBank().getActiveCount(), 3);
            expectEquals(engine.getVoice(1).getLFOBank().getActiveCount(), 3);

            engine.setParameter(ModParam::LFO_COUNT, 1.0f);
            expectEquals(engine.getVoice(0).getLFOBank().getActiveCount(), 1);
        }

        beginTest("ENV count param adds envelopes to chain");
        {
            UnifiedVoiceEngine engine(2);
            engine.prepareToPlay(44100.0, 512);

            expectEquals(engine.getVoice(0).getEnvelopeChain().getStageCount(), 0);

            engine.setParameter(ModParam::ENV_COUNT, 2.0f);
            expectEquals(engine.getVoice(0).getEnvelopeChain().getStageCount(), 2);
            expectEquals(engine.getVoice(1).getEnvelopeChain().getStageCount(), 2);

            engine.setParameter(ModParam::ENV_COUNT, 0.0f);
            expectEquals(engine.getVoice(0).getEnvelopeChain().getStageCount(), 0);
        }

        beginTest("ENV params reach envelope chain");
        {
            UnifiedVoiceEngine engine(2);
            engine.prepareToPlay(44100.0, 512);

            // Add 1 envelope first
            engine.setParameter(ModParam::ENV_COUNT, 1.0f);

            // Set attack
            engine.setParameter(ModParam::envParamId(0, ModParam::ENV_ATK), 0.5f);
            auto& env = engine.getVoice(0).getEnvelopeChain().getEnvelope(0);
            expectWithinAbsoluteError(env.getParams().attack, 0.5f, 0.001f);

            // Set depth
            engine.setParameter(ModParam::envParamId(0, ModParam::ENV_DEPTH), 0.75f);
            expectWithinAbsoluteError(engine.getVoice(0).getEnvelopeChain().getDepth(), 0.75f, 0.001f);
        }

        beginTest("LFO params ignored for non-existent LFOs");
        {
            UnifiedVoiceEngine engine(2);
            engine.prepareToPlay(44100.0, 512);

            // Only 1 LFO active — setting LFO2 rate should not crash
            engine.setParameter(ModParam::lfoParamId(1, ModParam::LFO_RATE), 10.0f);
            // LFO1 rate should be unaffected
            auto& lfo0 = engine.getVoice(0).getLFOBank().getLFO(0);
            expect(std::abs(lfo0.getRate() - 10.0f) > 0.1f, "LFO1 should not be changed");
        }
    }
};

static EngineModParamTests engineModParamTests;
