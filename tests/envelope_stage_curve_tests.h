#pragma once

#include "../src/dsp/Envelope.h"
#include "../src/engine/AudioEngine.h"
#include "../src/ui/EnvelopeCurve.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/FilterViewScreen.h"
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/project/ProjectBridge.h"
#include "../src/controller/DisplayRenderer.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <functional>
#include <vector>

// Per-stage envelope curves (StageCurveParam): attack, decay and release each have their own
// curve, reached by Shift + turning the stage's own knob. Measured on the Envelope class, on
// every engine's rendered sound (the turn goes through the real screen handler), on the envelope
// picture, on step locks and through a project file.
class EnvelopeStageCurveTests : public juce::UnitTest
{
public:
    EnvelopeStageCurveTests() : UnitTest("Envelope Stage Curves") {}

    static constexpr double SR = 44100.0;
    static constexpr int BLOCK = 128;
    static constexpr float STAGE_SECONDS = 0.4f;

    using Stage = StageCurveParam::Stage;

    // One envelope of one engine: where its cells are and how to hear it.
    struct Case
    {
        juce::String name;
        EngineType type;
        const char* page;                               // the PARAM page with its ATK DEC SUS REL
        int slot;                                       // StageCurveParam envelope slot expected
        bool filter;                                    // a filter envelope (heard as brightness)
        std::function<void(AudioEngine&)> setup;        // a steady sound in which the envelope is heard
    };

    static void set(AudioEngine& a, int id, float v) { a.setTrackParameter(0, id, v); }

    // A bright, steady source for each engine, with the level envelope out of the way.
    static void brightFm(AudioEngine& a, int onlyOp = -1)
    {
        static const float ratios[6] = { 1.0f, 3.0f, 6.0f, 9.0f, 12.0f, 16.0f };
        set(a, FMEngine::AlgorithmParam, 5.0f);         // ADD: six carriers
        for (int op = 0; op < 6; ++op)
        {
            set(a, op * 10 + 0, onlyOp >= 0 ? 1.0f : ratios[op]);
            set(a, op * 10 + 1, (onlyOp < 0 || onlyOp == op) ? 1.0f : 0.0f);
            set(a, op * 10 + 4, 0.001f);
            set(a, op * 10 + 6, 1.0f);
            set(a, op * 10 + 7, 2.0f);
        }
    }

    static void steadyWaveSeq(AudioEngine& a)
    {
        for (int id : { (int) WaveSeqEngine::SeqLength, (int) WaveSeqEngine::TimingLaneLen, (int) WaveSeqEngine::PitchLaneLen,
                        (int) WaveSeqEngine::ShapeLaneLen, (int) WaveSeqEngine::LevelLaneLen })
            set(a, id, 1.0f);
        set(a, WaveSeqEngine::StepPitch, 0.0f);
        set(a, WaveSeqEngine::StepShape, 1.0f);
        set(a, WaveSeqEngine::StepLevel, 1.0f);
        set(a, WaveSeqEngine::Crossfade, 0.0f);
    }

    // The shared voice filter, nearly closed, opened by its envelope; the level envelope held open.
    static void voiceFilterSetup(AudioEngine& a, int base, int ampAtk, int ampSus, int ampRel)
    {
        using F = VoiceFilterSection;
        set(a, base + F::Cutoff, 100.0f);
        set(a, base + F::EnvAmount, 0.5f);
        set(a, base + F::Resonance, 0.0f);
        if (ampAtk >= 0) { set(a, ampAtk, 0.001f); set(a, ampSus, 1.0f); set(a, ampRel, 2.0f); }
    }

    static std::vector<Case> cases()
    {
        std::vector<Case> c;
        auto quietUnified = [](AudioEngine& a)
        {
            auto& e = *dynamic_cast<UnifiedVoiceEngine*>(a.getEngine(0));
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                e.getVoice(v).getAnalogCharacter().enabled = false;
            set(a, UnifiedParam::MIX, 0.0f);
            set(a, UnifiedParam::KEYTRACK, 0.0f);
            set(a, UnifiedParam::FILTER_VEL, 0.0f);
        };
        c.push_back({ "Unified amp", EngineType::UnifiedVoice, "AMP", 0, false, quietUnified });
        c.push_back({ "Unified filter envelope", EngineType::UnifiedVoice, "F ENV", 1, true, [quietUnified](AudioEngine& a)
        {
            quietUnified(a);
            set(a, UnifiedParam::CUTOFF, 100.0f);
            set(a, UnifiedParam::FILTER_ENV, 0.5f);
            set(a, UnifiedParam::RESONANCE, 0.0f);
            set(a, UnifiedParam::AMP_ATTACK, 0.001f);
            set(a, UnifiedParam::AMP_SUSTAIN, 1.0f);
            set(a, UnifiedParam::AMP_RELEASE, 2.0f);
        } });
        c.push_back({ "Unified extra envelope", EngineType::UnifiedVoice, "MENV", 2, false, [quietUnified](AudioEngine& a)
        {
            quietUnified(a);
            set(a, ModParam::ENV_COUNT, 1.0f);
            set(a, ModParam::envParamId(0, ModParam::ENV_TARGET), 0.0f);    // AMP
            set(a, ModParam::envParamId(0, ModParam::ENV_DEPTH), 1.0f);
            set(a, UnifiedParam::AMP_ATTACK, 0.001f);
            set(a, UnifiedParam::AMP_SUSTAIN, 1.0f);
            set(a, UnifiedParam::AMP_RELEASE, 2.0f);
        } });

        static const char* const opPages[] = { "OP1", "OP2", "OP3", "OP4", "OP5", "OP6" };
        for (int op = 0; op < 6; ++op)
            c.push_back({ "FM operator " + juce::String(op + 1), EngineType::FM, opPages[op], op, false,
                          [op](AudioEngine& a) { brightFm(a, op); } });
        c.push_back({ "FM filter envelope", EngineType::FM, "F ENV", StageCurveParam::FILTER_ENV, true, [](AudioEngine& a)
        {
            brightFm(a);
            voiceFilterSetup(a, FMEngine::FilterBase, -1, -1, -1);
        } });

        c.push_back({ "Macro amp", EngineType::MacroOsc, "AMP", 0, false,
                      [](AudioEngine& a) { set(a, MacroOscEngine::TimbreParam, 0.0f); } });
        c.push_back({ "Macro filter envelope", EngineType::MacroOsc, "F ENV", StageCurveParam::FILTER_ENV, true, [](AudioEngine& a)
        {
            set(a, MacroOscEngine::TimbreParam, 0.0f);
            voiceFilterSetup(a, MacroOscEngine::FilterBase, MacroOscEngine::AmpAttack, MacroOscEngine::AmpSustain, MacroOscEngine::AmpRelease);
        } });

        c.push_back({ "Wavetable amp", EngineType::Wavetable, "AMP", 0, false,
                      [](AudioEngine& a) { set(a, WavetableEngine::Position, 1.0f); } });
        c.push_back({ "Wavetable filter envelope", EngineType::Wavetable, "F ENV", StageCurveParam::FILTER_ENV, true, [](AudioEngine& a)
        {
            set(a, WavetableEngine::Position, 1.0f);
            voiceFilterSetup(a, WavetableEngine::FilterBase, WavetableEngine::AmpAttack, WavetableEngine::AmpSustain, WavetableEngine::AmpRelease);
        } });

        c.push_back({ "Wave sequencer amp", EngineType::WaveSeq, "AMP", 0, false, steadyWaveSeq });
        c.push_back({ "Wave sequencer filter envelope", EngineType::WaveSeq, "F ENV", StageCurveParam::FILTER_ENV, true, [](AudioEngine& a)
        {
            steadyWaveSeq(a);
            voiceFilterSetup(a, WaveSeqEngine::FilterBase, WaveSeqEngine::AmpAttack, WaveSeqEngine::AmpSustain, WaveSeqEngine::AmpRelease);
        } });

        c.push_back({ "Sampler", EngineType::Sampler, "AMP", 0, false, [](AudioEngine& a)
        {
            // Three seconds of a steady tone.
            std::vector<float> tone((size_t) (SR * 3.0));
            for (size_t i = 0; i < tone.size(); ++i)
                tone[i] = 0.5f * std::sin((float) i * 0.05f);
            auto& e = *dynamic_cast<SamplerEngine*>(a.getEngine(0));
            e.loadSample(tone.data(), (int) tone.size(), 1, SR);
            set(a, SamplerEngine::Mode, (float) SamplerEngine::Forward);
        } });
        return c;
    }

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        ParamPageScreen param;

        explicit Rig(EngineType type) : param(seq, &audio)
        {
            audio.addEngine(0, AudioEngine::createEngine(type));
            seq.getPattern().getTrack(0).engineType = type;
            audio.getEngine(0)->prepareToPlay(SR, BLOCK);
        }
        EngineBase& engine() { return *audio.getEngine(0); }
        Track& track() { return seq.getPattern().getTrack(0); }
        float value(int id) { return audio.getTrackParameter(0, id); }
    };

    static int cellOf(ParamCellScreen& screen, const char* label)
    {
        const auto page = screen.getCurrentPage();
        for (int i = 0; i < 8; ++i)
            if (page.params[(size_t) i].isValid() && std::strcmp(page.params[(size_t) i].label, label) == 0)
                return i;
        return -1;
    }

    // Encoder message for `clicks` (two's complement, as the screens get it).
    static int turn(int clicks) { return clicks >= 0 ? clicks : 128 + clicks; }

    static void shiftTurn(Screen& screen, int cell, int clicks)
    {
        screen.setModifiers(true, false);
        screen.onEncoderChange(cell, turn(clicks));
        screen.setModifiers(false, false);
    }

    static const char* stageLabel(Stage s) { return s == Stage::Attack ? "ATK" : (s == Stage::Decay ? "DEC" : "REL"); }

    // RMS of `seconds` of the engine's output starting now.
    static float renderRms(EngineBase& e, double skipSeconds, double seconds)
    {
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        double sum = 0.0;
        int counted = 0;
        const int skip = (int) (skipSeconds * SR), total = skip + (int) (seconds * SR);
        for (int done = 0; done < total; done += BLOCK)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < BLOCK; ++i)
                if (done + i >= skip && done + i < total)
                {
                    sum += (double) buf.getSample(0, i) * buf.getSample(0, i);
                    ++counted;
                }
        }
        return (float) std::sqrt(sum / juce::jmax(1, counted));
    }

    // The envelope `c` set up so that `stage` lasts STAGE_SECONDS, that stage's curve turned to
    // `clicks` x 0.05 with Shift on its own knob, a note played: the level in the middle of the
    // stage. Checks on the way that the turn changed the right parameter and nothing else.
    float midStageLevel(const Case& c, Stage stage, int clicks)
    {
        Rig rig(c.type);
        c.setup(rig.audio);
        const juce::String who = c.name + " " + stageLabel(stage);
        expect(rig.param.selectPageByName(c.page), who + ": page " + c.page);

        const auto binding = EnvelopeCurve::bind(rig.param.getCurrentPage());
        expect(binding.valid, who + ": the page has an envelope");
        if (!binding.valid)
            return 0.0f;

        const float atk = stage == Stage::Attack ? STAGE_SECONDS : 0.001f;
        const float dec = stage == Stage::Decay ? STAGE_SECONDS : 0.05f;
        const float sus = stage == Stage::Decay ? 0.0f : 1.0f;
        const float rel = stage == Stage::Release ? STAGE_SECONDS : 0.05f;
        set(rig.audio, binding.atk.id, atk);
        set(rig.audio, binding.dec.id, dec);
        set(rig.audio, binding.sus.id, sus);
        set(rig.audio, binding.rel.id, rel);

        const int cell = cellOf(rig.param, stageLabel(stage));
        const int cellId = rig.param.getCurrentPage().params[(size_t) cell].id;
        const int curveId = rig.engine().getStageCurveParam(cellId);
        expectEquals(curveId, StageCurveParam::id(c.slot, (int) stage), who + ": the stage's curve parameter");

        shiftTurn(rig.param, cell, clicks);
        expectWithinAbsoluteError(rig.value(curveId), juce::jlimit(-1.0f, 1.0f, (float) clicks * 0.05f), 0.001f, who + ": curve set");
        expectWithinAbsoluteError(rig.value(cellId), stage == Stage::Attack ? atk : (stage == Stage::Decay ? dec : rel), 1.0e-5f,
                                  who + ": the time is untouched");
        for (int s = 0; s < StageCurveParam::STAGES; ++s)
            if (s != (int) stage)
                expectEquals(rig.value(StageCurveParam::id(c.slot, s)), 0.0f, who + ": the other stages keep their curve");
        expect(rig.track().soundParams.count(curveId) > 0, who + ": stored with the sound");

        const int note = c.filter ? 72 : 60;
        rig.engine().noteOn(note, 1.0f);
        if (stage != Stage::Release)
            return renderRms(rig.engine(), STAGE_SECONDS * 0.5 - 0.02, 0.04);
        renderRms(rig.engine(), 0.0, 0.2);
        rig.engine().noteOff(note);
        return renderRms(rig.engine(), STAGE_SECONDS * 0.5 - 0.02, 0.04);
    }

    static int differingPixels(const DisplayRenderer& a, const DisplayRenderer& b, int y0, int y1)
    {
        int n = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = 0; x < 128; ++x)
                if (a.getPixel(x, y) != b.getPixel(x, y)) ++n;
        return n;
    }

    void runTest() override
    {
        // ================================================================== the Envelope class
        beginTest("Envelope: each stage has its own curve, measured in the middle of the stage");
        {
            auto run = [](float a, float d, float r, float& midAttack, float& midDecay, float& midRelease)
            {
                Envelope env;
                env.setSampleRate(1000.0);
                Envelope::Params p;
                p.attack = 0.1f; p.decay = 0.1f; p.sustain = 0.0f; p.release = 0.1f;
                p.attackCurve = a; p.decayCurve = d; p.releaseCurve = r;
                env.setParams(p);
                env.reset();
                env.noteOn();
                for (int i = 0; i < 50; ++i) midAttack = env.process();
                for (int i = 0; i < 50; ++i) env.process();
                for (int i = 0; i < 50; ++i) midDecay = env.process();

                // Release from full level: a second envelope held at sustain 1.
                Envelope rel;
                rel.setSampleRate(1000.0);
                p.sustain = 1.0f; p.attack = 0.001f; p.decay = 0.001f;
                rel.setParams(p);
                rel.reset();
                rel.noteOn();
                for (int i = 0; i < 20; ++i) rel.process();
                rel.noteOff();
                for (int i = 0; i < 50; ++i) midRelease = rel.process();
            };

            float a0, d0, r0, a1, d1, r1;
            run(0.0f, 0.0f, 0.0f, a0, d0, r0);
            expectWithinAbsoluteError(a0, 0.5f, 0.02f);
            expectWithinAbsoluteError(d0, 0.5f, 0.02f);
            expectWithinAbsoluteError(r0, 0.5f, 0.02f);

            // -1: most of the move comes early (attack bulges up); +1: late (attack hollow).
            run(-1.0f, 0.0f, 0.0f, a1, d1, r1);
            expectWithinAbsoluteError(a1, 0.75f, 0.02f);
            expectWithinAbsoluteError(d1, d0, 1.0e-4f, "attack curve leaves the decay alone");
            expectWithinAbsoluteError(r1, r0, 1.0e-4f, "and the release");
            run(1.0f, 0.0f, 0.0f, a1, d1, r1);
            expectWithinAbsoluteError(a1, 0.25f, 0.02f);

            run(0.0f, -1.0f, 0.0f, a1, d1, r1);
            expectWithinAbsoluteError(d1, 0.25f, 0.02f, "decay -1 has fallen three quarters by half time");
            expectWithinAbsoluteError(a1, a0, 1.0e-4f);
            expectWithinAbsoluteError(r1, r0, 1.0e-4f);
            run(0.0f, 1.0f, 0.0f, a1, d1, r1);
            expectWithinAbsoluteError(d1, 0.75f, 0.02f);

            run(0.0f, 0.0f, -1.0f, a1, d1, r1);
            expectWithinAbsoluteError(r1, 0.25f, 0.02f);
            expectWithinAbsoluteError(a1, a0, 1.0e-4f);
            expectWithinAbsoluteError(d1, d0, 1.0e-4f);
            run(0.0f, 0.0f, 1.0f, a1, d1, r1);
            expectWithinAbsoluteError(r1, 0.75f, 0.02f);

            // All three at once, each its own.
            run(-1.0f, 1.0f, -1.0f, a1, d1, r1);
            expectWithinAbsoluteError(a1, 0.75f, 0.02f);
            expectWithinAbsoluteError(d1, 0.75f, 0.02f);
            expectWithinAbsoluteError(r1, 0.25f, 0.02f);

            for (float curve : { -1.0f, -0.4f, 0.0f, 0.3f, 1.0f })
                for (float x : { 0.0f, 0.2f, 0.5f, 0.9f, 1.0f })
                    expectWithinAbsoluteError(Envelope::unshape(Envelope::shape(x, curve), curve), x, 1.0e-4f);
        }

        beginTest("Envelope: a curve changed while its stage runs does not make the level jump");
        {
            for (int stage = 0; stage < 3; ++stage)
            {
                Envelope env;
                env.setSampleRate(1000.0);
                Envelope::Params p;
                p.attack = stage == 0 ? 0.2f : 0.001f;
                p.decay = stage == 1 ? 0.2f : 0.001f;
                p.sustain = stage == 1 ? 0.0f : 1.0f;
                p.release = 0.2f;
                env.setParams(p);
                env.reset();
                env.noteOn();
                if (stage == 2)
                {
                    for (int i = 0; i < 20; ++i) env.process();
                    env.noteOff();
                }
                float last = 0.0f, biggest = 0.0f;
                for (int i = 0; i < 60; ++i) last = env.process();
                // Swing the curve from end to end, one sample apart.
                for (float curve : { 1.0f, -1.0f, 0.5f, 0.0f, -0.7f })
                {
                    (stage == 0 ? p.attackCurve : (stage == 1 ? p.decayCurve : p.releaseCurve)) = curve;
                    env.setParams(p);
                    const float now = env.process();
                    biggest = juce::jmax(biggest, std::abs(now - last));
                    last = now;
                }
                expect(biggest < 0.03f, "Stage " + juce::String(stage) + ": largest step " + juce::String(biggest));
                // And the stage still ends where it should.
                for (int i = 0; i < 400; ++i) last = env.process();
                expectWithinAbsoluteError(last, stage == 0 ? 1.0f : 0.0f, 0.001f);
            }
        }

        // ================================================================== every engine, by ear
        beginTest("Every envelope of every engine: Shift + turn on ATK / DEC / REL bends that stage of the sound");
        {
            for (auto& c : cases())
            {
                for (Stage stage : { Stage::Attack, Stage::Decay, Stage::Release })
                {
                    const float early = midStageLevel(c, stage, -20);     // -1: the move comes early
                    const float late = midStageLevel(c, stage, 20);       // +1: the move comes late
                    const juce::String who = c.name + " " + stageLabel(stage) + ": " + juce::String(early, 4) + " / " + juce::String(late, 4);
                    logMessage(who);
                    expect(early > 1.0e-4f || late > 1.0e-4f, who + " (silent)");
                    // Half way up an attack the early curve is higher; half way down a decay or
                    // release it is lower.
                    if (stage == Stage::Attack) expect(early > late * 1.5f, who);
                    else                        expect(late > early * 1.5f, who);
                }
            }
        }

        beginTest("The curve parameter is applied and read back; ids do not collide with pages");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler })
            {
                auto engine = AudioEngine::createEngine(type);
                std::set<int> curveIds;
                for (auto& page : engine->getParamPages())
                    for (auto& p : page.params)
                    {
                        if (!p.isValid()) continue;
                        expect(!StageCurveParam::owns(p.id), "No page cell uses a stage-curve id");
                        const int id = engine->getStageCurveParam(p.id);
                        const bool stage = std::strcmp(p.label, "ATK") == 0 || std::strcmp(p.label, "DEC") == 0 || std::strcmp(p.label, "REL") == 0;
                        expect((id >= 0) == (stage && p.unitIs("s")), juce::String(p.label) + " of engine " + juce::String((int) type));
                        if (id >= 0)
                        {
                            expect(StageCurveParam::owns(id) && EngineParamRange::contains(id));
                            expect(curveIds.insert(id).second, "Each stage cell has its own curve id");
                            expectEquals(engine->getParameter(id), 0.0f, "straight by default");
                            engine->setParameter(id, 0.35f);
                            expectWithinAbsoluteError(engine->getParameter(id), 0.35f, 1.0e-6f);
                            engine->setParameter(id, -3.0f);
                            expectEquals(engine->getParameter(id), -1.0f, "limited to -1..1");
                        }
                    }
                expect(!curveIds.empty());
            }
            // The unified voice's extra envelopes (MENV cells are built by the screen).
            UnifiedVoiceEngine u;
            expectEquals(u.getStageCurveParam(ModParam::envParamId(3, ModParam::ENV_REL)), StageCurveParam::id(5, StageCurveParam::Release));
            expectEquals(u.getStageCurveParam(ModParam::envParamId(0, ModParam::ENV_SUS)), -1);
            expectEquals(u.getStageCurveParam(UnifiedParam::AMP_SUSTAIN), -1);
            expectEquals(u.getStageCurveParam(UnifiedParam::AMP_CURVE), -1);

            // No curves on a MIDI track (its ATK / DEC / REL are controller values) or the drum kit.
            for (auto type : { EngineType::MIDITrack, EngineType::Drum })
            {
                auto engine = AudioEngine::createEngine(type);
                for (auto& page : engine->getParamPages())
                    for (auto& p : page.params)
                        if (p.isValid())
                            expectEquals(engine->getStageCurveParam(p.id), -1);
            }
        }

        beginTest("CURV still bends all three stages; a stage's own curve adds to it");
        {
            Rig rig(EngineType::UnifiedVoice);
            auto& voice = dynamic_cast<UnifiedVoiceEngine&>(rig.engine()).getVoice(0);
            set(rig.audio, UnifiedParam::AMP_CURVE, 0.4f);
            set(rig.audio, StageCurveParam::id(0, StageCurveParam::Decay), 0.3f);
            set(rig.audio, StageCurveParam::id(0, StageCurveParam::Release), -1.0f);
            const auto p = voice.getAmpEnvelope().getParams();
            expectWithinAbsoluteError(p.attackCurve, 0.4f, 1.0e-5f);
            expectWithinAbsoluteError(p.decayCurve, 0.7f, 1.0e-5f);
            expectWithinAbsoluteError(p.releaseCurve, -0.6f, 1.0e-5f);
            expectWithinAbsoluteError(rig.value(UnifiedParam::AMP_CURVE), 0.4f, 1.0e-5f, "CURV reads back what was set");
            // Order does not matter (a project loads in id order, a lock restores one value).
            set(rig.audio, UnifiedParam::AMP_CURVE, 0.9f);
            expectWithinAbsoluteError(voice.getAmpEnvelope().getParams().decayCurve, 1.0f, 1.0e-5f, "limited to +1");
            expectWithinAbsoluteError(voice.getAmpEnvelope().getParams().releaseCurve, -0.1f, 1.0e-5f);
            // A time change keeps the curves.
            set(rig.audio, UnifiedParam::AMP_ATTACK, 0.5f);
            expectWithinAbsoluteError(voice.getAmpEnvelope().getParams().releaseCurve, -0.1f, 1.0e-5f);

            // An extra envelope switched on later takes its stored curves.
            set(rig.audio, StageCurveParam::id(3, StageCurveParam::Attack), 0.6f);
            set(rig.audio, ModParam::ENV_COUNT, 2.0f);
            expectWithinAbsoluteError(voice.getEnvelopeChain().getEnvelopeMut(1).getParams().attackCurve, 0.6f, 1.0e-5f);
            expectWithinAbsoluteError(voice.getEnvelopeChain().getEnvelopeMut(0).getParams().attackCurve, 0.0f, 1.0e-5f);
        }

        // ================================================================== the gesture
        beginTest("Shift is still a fine turn on SUS and on every other cell; a plain turn never touches a curve");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler, EngineType::MIDITrack })
            {
                Rig rig(type);
                auto& p = rig.param;
                const juce::String who = "Engine " + juce::String((int) type);
                for (int page = 0; page < p.getPageCount(); ++page)
                {
                    p.setCurrentPageIndex(page);
                    const auto cells = p.getCurrentPage();
                    for (int i = 0; i < 8; ++i)
                    {
                        const auto& spec = cells.params[(size_t) i];
                        if (!spec.isValid() || spec.id >= ParamCellScreen::FX_SLOT)
                            continue;
                        const juce::String what = who + " " + p.getPageName() + " " + spec.label;
                        const int curveId = rig.engine().getStageCurveParam(spec.id);

                        // Plain turn: the cell, never a curve.
                        p.setModifiers(false, false);
                        expectEquals(p.getCurveTurnId(i), -1, what);
                        if (curveId >= 0)
                        {
                            const float before = rig.value(spec.id);
                            p.onEncoderChange(i, turn(3));
                            expect(rig.value(spec.id) != before, what + ": a plain turn changes the time");
                            expectEquals(rig.value(curveId), 0.0f, what + ": and not the curve");
                            expect(p.getCurveReadout().isEmpty());
                        }

                        // Shift + turn.
                        p.setModifiers(true, false);
                        expectEquals(p.getCurveTurnId(i), curveId, what);
                        if (curveId < 0 && !spec.isDiscrete() && std::strcmp(spec.label, "SUS") == 0)
                        {
                            set(rig.audio, spec.id, 0.5f);
                            p.onEncoderChange(i, turn(1));
                            expectWithinAbsoluteError(rig.value(spec.id), 0.501f, 1.0e-4f, what + ": Shift is fine");
                        }
                        // Func (coarse) and Shift + Func are not the curve gesture.
                        p.setModifiers(true, true);
                        expectEquals(p.getCurveTurnId(i), -1, what);
                        p.setModifiers(false, false);
                    }
                }
                for (int id = StageCurveParam::BASE; id < StageCurveParam::END; ++id)
                    expectEquals(rig.value(id), 0.0f, who + ": no curve was changed along the way");
            }

            // A non-envelope continuous cell: Shift moves it by a tenth of a plain click.
            Rig rig(EngineType::UnifiedVoice);
            expect(rig.param.selectPageByName("AMP"));
            const int curv = cellOf(rig.param, "CURV");
            expect(curv >= 0);
            shiftTurn(rig.param, curv, 1);
            expectWithinAbsoluteError(rig.value(UnifiedParam::AMP_CURVE), 0.002f, 1.0e-4f);
            expect(!rig.param.isCurveReadoutActive());
        }

        beginTest("MAIN, FLTR and the MENV page reach the same curves");
        {
            // MAIN's bottom row is the engine's level envelope (FM: operator 1).
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler })
            {
                Rig rig(type);
                expectEquals(rig.param.getPageName(), juce::String("MAIN"));
                expectEquals(rig.param.getEncoderName(4), juce::String("ATK"));
                shiftTurn(rig.param, 4, 4);
                shiftTurn(rig.param, 5, -6);
                shiftTurn(rig.param, 6, 2);         // SUS: fine, no curve
                shiftTurn(rig.param, 7, 10);
                expectWithinAbsoluteError(rig.value(StageCurveParam::id(0, StageCurveParam::Attack)), 0.2f, 1.0e-4f, "Engine " + juce::String((int) type));
                expectWithinAbsoluteError(rig.value(StageCurveParam::id(0, StageCurveParam::Decay)), -0.3f, 1.0e-4f);
                expectWithinAbsoluteError(rig.value(StageCurveParam::id(0, StageCurveParam::Release)), 0.5f, 1.0e-4f);
            }

            // FLTR: the filter envelope page, with the readout in the name bar text.
            for (auto type : { EngineType::UnifiedVoice, EngineType::Wavetable })
            {
                SequencerCore seq;
                AudioEngine audio;
                audio.addEngine(0, AudioEngine::createEngine(type));
                seq.getPattern().getTrack(0).engineType = type;
                FilterViewScreen fltr(seq, &audio);
                expect(fltr.selectPageByName("F ENV"));
                const int dec = cellOf(fltr, "DEC");
                expect(dec >= 0);
                const int slot = type == EngineType::UnifiedVoice ? 1 : StageCurveParam::FILTER_ENV;
                shiftTurn(fltr, dec, -8);
                expectWithinAbsoluteError(audio.getTrackParameter(0, StageCurveParam::id(slot, StageCurveParam::Decay)), -0.4f, 1.0e-4f);
                expectEquals(fltr.getEncoderReadout(dec), juce::String("DEC CRV -40"));
                expect(fltr.isEnvelopePictureActive());
                EnvelopeCurve::Settings shown;
                expect(fltr.getEnvelopeSettings(shown));
                expectWithinAbsoluteError(shown.decayCurve, -0.4f, 1.0e-4f);
                DisplayRenderer d;
                fltr.render(d);         // draws without trouble
                fltr.expireFilterPicture();
                expect(fltr.getEncoderReadout(dec) != juce::String("DEC CRV -40"), "The readout goes back to the time");
            }

            // MENV: the selected extra envelope.
            Rig rig(EngineType::UnifiedVoice);
            expect(rig.param.selectPageByName("MENV"));
            rig.param.onEncoderChange(0, turn(2));                  // ENV cell: the third extra envelope
            expectEquals(rig.param.getModSelectedEnv(), 2);
            shiftTurn(rig.param, cellOf(rig.param, "REL"), -3);
            expectWithinAbsoluteError(rig.value(StageCurveParam::id(4, StageCurveParam::Release)), -0.15f, 1.0e-4f);
            expectEquals(rig.value(StageCurveParam::id(2, StageCurveParam::Release)), 0.0f);
        }

        beginTest("Track + Shift + turn bends the stage on every track with that engine");
        {
            Rig rig(EngineType::Wavetable);
            rig.audio.addEngine(1, AudioEngine::createEngine(EngineType::Wavetable));
            rig.seq.getPattern().getTrack(1).engineType = EngineType::Wavetable;
            rig.audio.addEngine(2, AudioEngine::createEngine(EngineType::FM));
            rig.seq.getPattern().getTrack(2).engineType = EngineType::FM;
            expect(rig.param.selectPageByName("AMP"));
            const int rel = cellOf(rig.param, "REL");
            rig.param.setModifiers(true, false);
            rig.param.onEncoderChangeAllTracks(rel, turn(5));
            rig.param.setModifiers(false, false);
            const int id = StageCurveParam::id(0, StageCurveParam::Release);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, id), 0.25f, 1.0e-4f);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(1, id), 0.25f, 1.0e-4f);
            expectEquals(rig.audio.getTrackParameter(2, id), 0.0f, "another engine is left alone");
            expectEquals(rig.audio.getTrackParameter(0, WavetableEngine::AmpRelease), rig.audio.getTrackParameter(1, WavetableEngine::AmpRelease));
        }

        // ================================================================== feedback
        beginTest("Feedback: the cell and the name bar show the curve, the picture its shape, for a second");
        {
            Rig rig(EngineType::UnifiedVoice);
            auto& p = rig.param;
            expect(p.selectPageByName("AMP"));
            const int atk = cellOf(p, "ATK");
            expect(atk >= 0 && atk < 4);
            set(rig.audio, UnifiedParam::AMP_ATTACK, 1.0f);

            const juce::String plainSubPage = p.getSubPage();
            DisplayRenderer plain;
            p.render(plain);

            shiftTurn(p, atk, 8);
            expect(p.isCurveReadoutActive() && p.isEnvelopePictureActive());
            expectEquals(p.getSubPage(), juce::String("ATK CRV +40"));
            expectEquals(p.getCurveReadout(), juce::String("ATK CRV +40"));
            EnvelopeCurve::Settings shown;
            expect(p.getEnvelopeSettings(shown));
            expectWithinAbsoluteError(shown.attackCurve, 0.4f, 1.0e-4f);
            expectEquals(shown.decayCurve, 0.0f);

            DisplayRenderer hollow, bulging, after;
            p.render(hollow);
            const int cellY = ParamGrid::cellY(atk);
            expect(differingPixels(plain, hollow, cellY, cellY + 18) > 0, "The cell shows the curve instead of the time");
            const int botY0 = ParamCellScreen::PICTURE_BOTTOM_Y, botY1 = botY0 + ParamCellScreen::PICTURE_BOTTOM_H - 1;
            expect(differingPixels(plain, hollow, botY0, botY1) > 0, "The picture covers the other row");

            shiftTurn(p, atk, -16);
            expectEquals(p.getSubPage(), juce::String("ATK CRV -40"));
            p.render(bulging);
            expect(differingPixels(hollow, bulging, botY0, botY1) > 10, "A different curve is a different picture");

            // The same second later: everything back, the curve kept.
            UiClock::advance(1100);
            expect(!p.isCurveReadoutActive() && !p.isEnvelopePictureActive());
            expectEquals(p.getSubPage(), plainSubPage);
            p.render(after);
            expectEquals(differingPixels(plain, after, 0, 63), 0, "The page is as it was (the time did not change)");
            expectWithinAbsoluteError(rig.value(StageCurveParam::id(0, StageCurveParam::Attack)), -0.4f, 1.0e-4f);

            // A plain turn of the same knob ends the curve readout at once.
            shiftTurn(p, atk, 1);
            expect(p.isCurveReadoutActive());
            p.onEncoderChange(atk, turn(1));
            expect(!p.isCurveReadoutActive() && p.getCurveReadout().isEmpty());
            expect(p.getSubPage() != juce::String("ATK CRV -35"));
            expect(p.isEnvelopePictureActive(), "The picture stays: the time was turned");
        }

        beginTest("The envelope picture draws each stage's curve truthfully");
        {
            EnvelopeCurve::Settings s;
            s.attack = 1.0f; s.decay = 1.0f; s.sustain = 0.2f; s.release = 1.0f;
            const auto straight = EnvelopeCurve::shape(s);
            const size_t midA = (size_t) (straight.attackEnd / 2);
            const size_t midD = (size_t) ((straight.attackEnd + straight.decayEnd) / 2);
            const size_t midR = (size_t) ((straight.holdEnd + (int) straight.heights.size() - 1) / 2);

            auto bent = s;
            bent.attackCurve = -1.0f;
            auto shape = EnvelopeCurve::shape(bent);
            expect(shape.heights[midA] > straight.heights[midA] + 0.15f, "attack -1 bulges upward");
            expectWithinAbsoluteError(shape.heights[midD], straight.heights[midD], 1.0e-4f);
            expectWithinAbsoluteError(shape.heights[midR], straight.heights[midR], 1.0e-4f);

            bent = s;
            bent.decayCurve = 1.0f;
            shape = EnvelopeCurve::shape(bent);
            expect(shape.heights[midD] > straight.heights[midD] + 0.1f, "decay +1 holds, then falls");
            expectWithinAbsoluteError(shape.heights[midA], straight.heights[midA], 1.0e-4f);

            bent = s;
            bent.releaseCurve = -1.0f;
            shape = EnvelopeCurve::shape(bent);
            expect(shape.heights[midR] < straight.heights[midR] - 0.02f, "release -1 drops at once");
            expectWithinAbsoluteError(shape.heights[midA], straight.heights[midA], 1.0e-4f);
            expectWithinAbsoluteError(shape.heights[midD], straight.heights[midD], 1.0e-4f);

            // CURV and the stage's own curve add up, as in the engines.
            bent = s;
            bent.curve = 0.5f;
            bent.attackCurve = -0.5f;
            expectWithinAbsoluteError(EnvelopeCurve::shape(bent).heights[midA], straight.heights[midA], 1.0e-4f);
            expect(!(bent == s));
        }

        // ================================================================== locks
        beginTest("A held step + Shift + turn locks the stage's curve on that step");
        {
            Rig rig(EngineType::MacroOsc);
            auto& p = rig.param;
            expect(p.selectPageByName("AMP"));
            const int dec = cellOf(p, "DEC");
            const int id = StageCurveParam::id(0, StageCurveParam::Decay);
            set(rig.audio, id, 0.1f);

            p.onPadPress(3, 100);
            expect(p.isStepHeld());
            shiftTurn(p, dec, 10);
            auto& step = rig.track().getStep(3);
            expect(step.paramLocks.count(id) > 0, "The curve is locked on the step");
            expectWithinAbsoluteError(step.paramLocks[id], 0.6f, 1.0e-4f);
            expect(step.paramLocks.count(MacroOscEngine::AmpDecay) == 0, "The time is not");
            expectWithinAbsoluteError(rig.value(id), 0.1f, 1.0e-5f, "The sound's own curve is unchanged");
            expectWithinAbsoluteError(rig.track().soundParams[id], 0.1f, 1.0e-5f, "and kept as what the lock returns to");
            expect(step.trig);
            EnvelopeCurve::Settings shown;
            expect(p.getEnvelopeSettings(shown));
            expectWithinAbsoluteError(shown.decayCurve, 0.6f, 1.0e-4f, "The picture shows the held step's curve");

            // The sequencer's path: the lock applies on the step and goes back afterwards.
            rig.audio.applyStepLocks(0, step);
            expectWithinAbsoluteError(rig.value(id), 0.6f, 1.0e-4f);
            rig.audio.applyStepLocks(0, rig.track().getStep(4));
            expectWithinAbsoluteError(rig.value(id), 0.1f, 1.0e-5f);

            // > + turn removes the cell's lock; with Shift, the curve's.
            p.onEncoderChange(dec, turn(2));
            expect(step.paramLocks.count(MacroOscEngine::AmpDecay) > 0);
            p.setModifiers(true, false);
            p.onEncoderLockClear(dec);
            p.setModifiers(false, false);
            expect(step.paramLocks.count(id) == 0, "Shift + > + turn removed the curve lock");
            expect(step.paramLocks.count(MacroOscEngine::AmpDecay) > 0, "and left the time lock");
            p.onEncoderLockClear(dec);
            expect(step.paramLocks.count(MacroOscEngine::AmpDecay) == 0);
            p.onPadRelease(3);
        }

        beginTest("STEP: the lock pages take Shift + turn the same way");
        {
            SequencerCore seq;
            AudioEngine audio;
            PadManager pads;
            PadLEDFeedback leds;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            ParamPageScreen params(seq, &audio);
            StepViewScreen steps(seq, pads, leds);
            steps.setLockPages(&params);

            steps.onPadPress(2, 100);
            shiftTurn(steps, 7, -4);            // MAIN (the first lock page): REL
            expectEquals(steps.getName(), juce::String("MAIN"));
            auto& locks = seq.getPattern().getTrack(0).getStep(2).paramLocks;
            const int id = StageCurveParam::id(0, StageCurveParam::Release);
            expect(locks.count(id) > 0);
            expectWithinAbsoluteError(locks[id], -0.2f, 1.0e-4f);
            expect(locks.count(UnifiedParam::AMP_RELEASE) == 0);
            DisplayRenderer d;
            steps.render(d);

            steps.setModifiers(true, false);
            steps.onEncoderLockClear(7);
            steps.setModifiers(false, false);
            expect(locks.count(id) == 0);
            steps.onPadRelease(2);
        }

        // ================================================================== projects
        beginTest("Curves survive a project round trip on every engine");
        {
            const EngineType types[] = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                                         EngineType::WaveSeq, EngineType::Sampler };
            SequencerCore seq;
            AudioEngine audio;
            std::vector<std::pair<int, std::vector<std::pair<int, float>>>> expected;
            for (int t = 0; t < 6; ++t)
            {
                audio.addEngine(t, AudioEngine::createEngine(types[t]));
                seq.getPattern().getTrack(t).engineType = types[t];
            }
            ParamPageScreen screen(seq, &audio);
            for (int t = 0; t < 6; ++t)
            {
                seq.setSelectedTrack(t);
                std::vector<std::pair<int, float>> values;
                // Every stage cell of every page, each to its own amount, by the gesture.
                int n = 0;
                for (int page = 0; page < screen.getPageCount(); ++page)
                {
                    screen.setCurrentPageIndex(page);
                    if (screen.getPageName() == "MAIN")
                        continue;       // the same cells again
                    for (int i = 0; i < 8; ++i)
                    {
                        screen.setModifiers(true, false);
                        const int id = screen.getCurveTurnId(i);
                        if (id < 0) continue;
                        const int clicks = (n % 2 == 0 ? 1 : -1) * (2 + n % 15);
                        ++n;
                        screen.onEncoderChange(i, turn(clicks));
                        values.push_back({ id, (float) clicks * 0.05f });
                    }
                }
                screen.setModifiers(false, false);
                expect(n >= 3, "Track " + juce::String(t) + " has stage cells");
                expected.push_back({ t, values });
            }
            // A curve lock as well.
            seq.getPattern().getTrack(1).getStep(5).paramLocks[StageCurveParam::id(2, StageCurveParam::Decay)] = -0.75f;

            const auto data = ProjectData::fromJSON(ProjectBridge::capture({ seq, audio }, "CURVES").toJSON());
            SequencerCore seq2;
            AudioEngine audio2;
            ProjectBridge::apply(data, { seq2, audio2 });
            for (auto& [t, values] : expected)
                for (auto& [id, value] : values)
                    expectWithinAbsoluteError(audio2.getTrackParameter(t, id), value, 1.0e-4f,
                                              "Track " + juce::String(t) + " id " + juce::String(id));
            auto& locks = seq2.getPattern().getTrack(1).getStep(5).paramLocks;
            expect(locks.count(StageCurveParam::id(2, StageCurveParam::Decay)) > 0);

            // The unified voice with CURV as well: both come back, whatever the order.
            seq.setSelectedTrack(0);
            audio.setTrackParameter(0, UnifiedParam::AMP_CURVE, -0.5f);
            seq.getPattern().getTrack(0).soundParams[UnifiedParam::AMP_CURVE] = -0.5f;
            const auto again = ProjectData::fromJSON(ProjectBridge::capture({ seq, audio }, "CURVES").toJSON());
            SequencerCore seq3;
            AudioEngine audio3;
            ProjectBridge::apply(again, { seq3, audio3 });
            auto* u = dynamic_cast<UnifiedVoiceEngine*>(audio3.getEngine(0));
            auto* was = dynamic_cast<UnifiedVoiceEngine*>(audio.getEngine(0));
            expect(u != nullptr && was != nullptr);
            if (u != nullptr && was != nullptr)
            {
                const auto a = was->getVoice(0).getAmpEnvelope().getParams(), b = u->getVoice(0).getAmpEnvelope().getParams();
                expectWithinAbsoluteError(b.attackCurve, a.attackCurve, 1.0e-4f);
                expectWithinAbsoluteError(b.decayCurve, a.decayCurve, 1.0e-4f);
                expectWithinAbsoluteError(b.releaseCurve, a.releaseCurve, 1.0e-4f);
                expect(std::abs(a.attackCurve - a.decayCurve) > 0.01f, "the stages really differ");
            }

            // A sound preset built from the track carries them too, and a fresh engine starts straight.
            audio.setEngineType(2, EngineType::MacroOsc, true);
            expectEquals(audio.getTrackParameter(2, StageCurveParam::id(0, StageCurveParam::Attack)), 0.0f);
        }
    }
};

static EnvelopeStageCurveTests EnvelopeStageCurveTestsInstance;
