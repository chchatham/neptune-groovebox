#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <limits>
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/ui/screens/TrackFxScreen.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/project/ProjectBridge.h"

// Track input (spec-track-input.md, 1.1). The contracts of step 0: the id blocks, the engine's
// view of its input, the fourth engine block reaching an engine. Then the input itself in the
// AudioEngine (source, LEVL, the limiter, MODE, the follower, effects, mute, a feedback pair),
// TRK FX page IN from the controller, and the project file.
class TrackInputTests : public juce::UnitTest
{
public:
    TrackInputTests() : UnitTest("Track Input") {}

    // An engine that remembers what it was given.
    struct ProbeEngine : public EngineBase
    {
        EngineType getType() const override { return EngineType::Resonator; }
        juce::String getName() const override { return "Probe"; }
        void prepareToPlay(double, int) override {}
        void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override
        {
            seenConnected = audioInput.connected();
            seenFrames = audioInput.frames;
            seenFirst = audioInput.connected() ? audioInput.left[0] : 0.0f;
            seenEnvelope = audioInput.envelope;
        }
        void noteOn(int, float, int) override {}
        void noteOff(int, int) override {}
        void allNotesOff() override {}
        void setParameter(ParamId id, float value) override { values[id] = value; ++sets; }
        float getParameter(ParamId id) const override { auto it = values.find(id); return it != values.end() ? it->second : 0.0f; }
        juce::String getParameterName(ParamId) const override { return {}; }
        int getParameterCount() const override { return 0; }

        ParamMap values;
        int sets = 0;
        bool seenConnected = false;
        int seenFrames = 0;
        float seenFirst = 0.0f, seenEnvelope = 0.0f;
    };

    // ---- Test engines and a test effect for the track input ----------------------------------------
    struct SilentBase : public EngineBase
    {
        EngineType getType() const override { return EngineType::Resonator; }
        juce::String getName() const override { return "Test"; }
        void prepareToPlay(double, int) override {}
        void noteOn(int, float, int) override {}
        void noteOff(int, int) override {}
        void allNotesOff() override {}
        void setParameter(ParamId, float) override {}
        float getParameter(ParamId) const override { return 0.0f; }
        juce::String getParameterName(ParamId) const override { return {}; }
        int getParameterCount() const override { return 0; }
    };

    // A steady level on each side; or, counting, 0.01 x the number of the block.
    struct ToneEngine : public SilentBase
    {
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            ++blocksDone;
            const float l = countBlocks ? 0.01f * (float) blocksDone : left, r = countBlocks ? l : right;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                buffer.setSample(0, i, l);
                buffer.setSample(1, i, r);
            }
        }
        float left = 0.0f, right = 0.0f;
        bool countBlocks = false;
        int blocksDone = 0;
    };

    // Remembers the input it was given and (unless told not to) copies it to its output.
    struct EchoEngine : public SilentBase
    {
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            ++calls;
            connected = audioInput.connected();
            frames = audioInput.frames;
            envelope = audioInput.envelope;
            inL.clear();
            inR.clear();
            if (! connected)
                return;
            inL.assign(audioInput.left, audioInput.left + audioInput.frames);
            inR.assign(audioInput.right, audioInput.right + audioInput.frames);
            if (copy)
                for (int i = 0; i < buffer.getNumSamples() && i < frames; ++i)
                {
                    buffer.setSample(0, i, inL[(size_t) i]);
                    buffer.setSample(1, i, inR[(size_t) i]);
                }
        }
        bool connected = false, copy = true;
        int frames = 0, calls = 0;
        float envelope = 0.0f;
        std::vector<float> inL, inR;
    };

    // Noise for a while after each note (or after trigger()).
    struct BurstEngine : public SilentBase
    {
        void trigger(int samples, float level) { left = samples; amp = level; }
        void noteOn(int, float, int) override { ++noteOns; trigger(480, 0.5f); }
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            for (int i = 0; i < buffer.getNumSamples() && left > 0; ++i, --left)
            {
                seed = seed * 1664525u + 1013904223u;
                const float v = amp * ((float) (seed >> 8) / 8388608.0f - 1.0f);
                buffer.setSample(0, i, v);
                buffer.setSample(1, i, -v);
            }
        }
        int left = 0, noteOns = 0;
        float amp = 0.5f;
        juce::uint32 seed = 12345u;
    };

    // An insert effect that wants the track's input: remembers what it was given and the sound it
    // was handed (which it passes unchanged).
    struct ProbeFx : public EffectBase
    {
        void reset() override {}
        void process(float&, float&) override {}
        int keySource() const override { return KEY_IN; }
        void setInput(const float* l, const float* r, int n) override
        {
            ++gives;
            fresh = true;
            given = l != nullptr && r != nullptr;
            frames = n;
            inL.clear();
            inR.clear();
            if (given)
            {
                inL.assign(l, l + n);
                inR.assign(r, r + n);
            }
        }
        void processBlock(float* l, float* r, int n) override
        {
            givenBeforeProcess = givenBeforeProcess && fresh;
            fresh = false;
            audioL.assign(l, l + n);
            audioR.assign(r, r + n);
        }
        int gives = 0, frames = 0;
        bool given = false, fresh = false, givenBeforeProcess = true;
        std::vector<float> inL, inR, audioL, audioR;
    };

    // The instrument, rendered block by block.
    struct Rig
    {
        static constexpr int BLOCK = 64;
        SequencerCore seq;
        AudioEngine audio;
        std::vector<float> left, right;
        int block;

        explicit Rig(int blockSize = BLOCK, double sampleRate = 48000.0) : left((size_t) blockSize), right((size_t) blockSize), block(blockSize)
        {
            seq.prepareToPlay(sampleRate, blockSize);
            audio.setSequencer(&seq);
            audio.prepare(sampleRate, blockSize);
        }

        template <typename Engine>
        Engine* add(int track)
        {
            auto e = std::make_shared<Engine>();
            audio.addEngine(track, e);
            return e.get();
        }

        // A probe as the last insert of a track.
        ProbeFx* probe(int track)
        {
            audio.addInsertEffect(track, EffectType::ChorusEffect);     // (makes the slot)
            auto fx = std::make_unique<ProbeFx>();
            auto* raw = fx.get();
            auto* chain = audio.getInsertChain(track);
            chain->replaceEffect(chain->getEffectCount() - 1, std::move(fx));
            return raw;
        }

        void blocks(int count, int samples = -1)
        {
            last = samples > 0 ? samples : block;
            for (int i = 0; i < count; ++i)
                audio.renderBlock(left.data(), right.data(), last);
        }

        // Of the last block of the whole mix.
        float peak() const
        {
            float p = 0.0f;
            for (int i = 0; i < last; ++i)
            {
                if (! std::isfinite(left[(size_t) i]) || ! std::isfinite(right[(size_t) i]))
                    return std::numeric_limits<float>::infinity();
                p = std::max(p, std::max(std::abs(left[(size_t) i]), std::abs(right[(size_t) i])));
            }
            return p;
        }
        int last = BLOCK;
    };

    // TRK FX behind ScreenManager, as the controller reaches it.
    struct OpenFx : public TrackFxScreen
    {
        using TrackFxScreen::TrackFxScreen;
        juce::String text(int cell) const
        {
            const auto spec = getCurrentPage().params[(size_t) cell];
            const float shown = spec.clamp(displayValue(spec));
            const juce::String own = cellText(spec, shown);
            if (own.isNotEmpty()) return own;
            return spec.kind == ParamKind::Choice ? juce::String(spec.choices[(int) std::lround(shown)]) : juce::String(shown, 2);
        }
    };
    struct Ui
    {
        SequencerCore seq;
        AudioEngine audio;
        ScreenManager mgr;
        OpenFx* fx = nullptr;
        juce::int64 now = 1000;

        Ui()
        {
            seq.prepareToPlay(48000.0, 64);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(48000.0, 64);
            mgr.setSequencer(&seq);
            mgr.setClock([this] { return now; });
            mgr.setHelpBusyWindow(0);
            mgr.setEngineHooks(GrammarHooks::make(seq, audio));
            auto f = std::make_unique<OpenFx>(seq, &audio);
            fx = f.get();
            mgr.addScreen(std::move(f));
            mgr.projectLoaded();
        }
        void turn(int knob, int clicks) { mgr.controlChangeReceived(16, 0x15 + knob, clicks < 0 ? 128 + clicks : clicks); }
    };

    void runTest() override
    {
        beginTest("The id blocks: track input 344-359, mod slots 700-763, LFO shape 764-771, the fourth engine block 700-784");
        {
            expectEquals((int) TrackInParam::BASE, 344);
            expectEquals((int) TrackInParam::SRC, 344);
            expectEquals((int) TrackInParam::LEVEL, 345);
            expectEquals((int) TrackInParam::MODE, 346);
            expectEquals((int) TrackInParam::FOLLOW_ATK, 347);
            expectEquals((int) TrackInParam::FOLLOW_REL, 348);
            expectEquals((int) TrackInParam::END, 360);
            expect(TrackInParam::contains(344) && TrackInParam::contains(359) && ! TrackInParam::contains(343) && ! TrackInParam::contains(360));
            // The track's input is the track's, not the engine's.
            for (int id = TrackInParam::BASE; id < TrackInParam::END; ++id)
                expect(! EngineParamRange::contains(id) && ! AudioEngine::isInsertExtraParam(id) && ! MixParam::isMixParam(id));

            expectEquals(ModSlotParam::id(0, ModSlotParam::SRC), 700);
            expectEquals(ModSlotParam::id(1, ModSlotParam::AMT), 706);
            expectEquals(ModSlotParam::id(ModSlotParam::MAX_SLOTS - 1, ModSlotParam::VIA), (int) ModSlotParam::END - 1);
            expectEquals((int) LfoShapeParam::BASE, (int) ModSlotParam::END);
            expectEquals((int) LfoShapeParam::BASE + (int) LfoShapeParam::POINTS, (int) LfoShapeParam::END);
            expectEquals((int) ModSlotParam::COUNT, (int) LfoShapeParam::END);

            expect(EngineParamRange::contains(700) && EngineParamRange::contains(784));
            expect(! EngineParamRange::contains(699) && ! EngineParamRange::contains(785) && ! EngineParamRange::contains(656));
            expect(EngineParamRange::contains(ModSlotParam::COUNT));
            // A pressure range on an id of the fourth block is still a range id.
            expect(RangeParam::isRangeParam(RangeParam::LO_BASE + 784) && RangeParam::targetOf(RangeParam::HI_BASE + 784) == 784);
            expect(RangeParam::LO_BASE + EngineParamRange::FOURTH_END <= RangeParam::HI_BASE);
        }

        beginTest("A value set at id 700 on a track reaches the engine, and is the track's value");
        {
            AudioEngine audio;
            auto probe = std::make_shared<ProbeEngine>();
            audio.addEngine(0, probe);
            audio.setTrackParameter(0, ModSlotParam::id(0, ModSlotParam::SRC), 3.0f);
            audio.setTrackParameter(0, EngineParamRange::FOURTH_END - 1, 0.25f);
            expectEquals(probe->values[700], 3.0f);
            expectEquals(probe->values[EngineParamRange::FOURTH_END - 1], 0.25f);
            expectEquals(audio.getTrackParameter(0, 700), 3.0f);
            expectEquals(audio.getTrackParameter(0, EngineParamRange::FOURTH_END - 1), 0.25f);

            // Outside the engine blocks nothing arrives: 656-699, FOURTH_END up, the track input's ids.
            const int before = probe->sets;
            for (int id : { 656, 699, (int) EngineParamRange::FOURTH_END, 899, (int) TrackInParam::SRC, (int) TrackInParam::END - 1 })
                audio.setTrackParameter(0, id, 1.0f);
            expectEquals(probe->sets, before);

            // Every real engine takes the whole block without minding (nothing is on those ids yet).
            for (int t = 0; t < ENGINE_TYPE_COUNT; ++t)
            {
                audio.addEngine(1, AudioEngine::createEngine((EngineType) t));
                for (int id = EngineParamRange::FOURTH_BASE; id < EngineParamRange::FOURTH_END; ++id)
                {
                    audio.setTrackParameter(1, id, 1.0f);
                    expect(std::isfinite(audio.getTrackParameter(1, id)));
                }
            }
        }

        beginTest("Changing the engine clears the old engine's values in the fourth block, and their pressure ranges");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            auto& params = seq.getPattern().getTrack(0).soundParams;
            params[700] = 1.0f;
            params[RangeParam::LO_BASE + 701] = 0.1f;
            params[RangeParam::HI_BASE + 779] = 0.9f;
            params[TrackInParam::SRC] = 2.0f;
            params[MixParam::VOLUME] = 0.5f;
            audio.setEngineType(0, EngineType::FM);
            expect(params.count(700) == 0 && params.count(RangeParam::LO_BASE + 701) == 0 && params.count(RangeParam::HI_BASE + 779) == 0);
            expect(params.count(TrackInParam::SRC) == 1, "the track's input stays with the track");
            expect(params.count(MixParam::VOLUME) == 1);
        }

        beginTest("An engine sees the input it was given, and none by default");
        {
            ProbeEngine e;
            juce::AudioBuffer<float> buffer(2, 64);
            juce::MidiBuffer midi;
            e.processBlock(buffer, midi);
            expect(! e.seenConnected);

            std::vector<float> in(64, 0.5f);
            AudioInput input;
            input.left = in.data();
            input.right = in.data();        // the right side may be the left
            input.frames = 64;
            input.envelope = 0.3f;
            expect(input.connected());
            e.setAudioInput(input);
            e.processBlock(buffer, midi);
            expect(e.seenConnected);
            expectEquals(e.seenFrames, 64);
            expectEquals(e.seenFirst, 0.5f);
            expectEquals(e.seenEnvelope, 0.3f);

            e.setAudioInput({});
            e.processBlock(buffer, midi);
            expect(! e.seenConnected);
            AudioInput empty;
            empty.left = in.data();         // a pointer without frames is no input
            expect(! empty.connected());
        }

        // =============================== the track input in the AudioEngine ===============================

        beginTest("IN values: defaults, set and read back, held to their ranges; the ids not in use read 0");
        {
            Rig r;
            r.add<ToneEngine>(0);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::SRC), 0.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::LEVEL), 1.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::MODE), 0.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::FOLLOW_ATK), 5.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::FOLLOW_REL), 150.0f);
            for (int id : AudioEngine::TRACK_IN_IDS)
                expectEquals(r.audio.getTrackParameter(0, id), AudioEngine::trackInDefault(id));

            r.audio.setTrackParameter(0, TrackInParam::SRC, 3.0f);
            r.audio.setTrackParameter(0, TrackInParam::LEVEL, 0.4f);
            r.audio.setTrackParameter(0, TrackInParam::MODE, 2.0f);
            r.audio.setTrackParameter(0, TrackInParam::FOLLOW_ATK, 20.0f);
            r.audio.setTrackParameter(0, TrackInParam::FOLLOW_REL, 800.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::SRC), 3.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::LEVEL), 0.4f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::MODE), 2.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::FOLLOW_ATK), 20.0f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::FOLLOW_REL), 800.0f);
            expectEquals(r.audio.getTrackParameter(1, TrackInParam::SRC), 0.0f, "one track's input is not another's");

            // Anything a file or a lock may hold.
            const float wild[] = { -5.0f, 1.0e9f, -1.0e9f, 99.0f, std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::infinity() };
            for (float v : wild)
                for (int id = TrackInParam::BASE; id < TrackInParam::END; ++id)
                {
                    r.audio.setTrackParameter(0, id, v);
                    expect(std::isfinite(r.audio.getTrackParameter(0, id)));
                }
            expect(r.audio.getTrackParameter(0, TrackInParam::SRC) >= 0.0f && r.audio.getTrackParameter(0, TrackInParam::SRC) <= (float) AudioEngine::MAX_TRACKS);
            expect(r.audio.getTrackParameter(0, TrackInParam::LEVEL) >= 0.0f && r.audio.getTrackParameter(0, TrackInParam::LEVEL) <= 1.0f);
            expect(r.audio.getTrackParameter(0, TrackInParam::MODE) >= 0.0f && r.audio.getTrackParameter(0, TrackInParam::MODE) <= 2.0f);
            expect(r.audio.getTrackParameter(0, TrackInParam::FOLLOW_ATK) >= 1.0f && r.audio.getTrackParameter(0, TrackInParam::FOLLOW_ATK) <= 200.0f);
            expect(r.audio.getTrackParameter(0, TrackInParam::FOLLOW_REL) >= 10.0f && r.audio.getTrackParameter(0, TrackInParam::FOLLOW_REL) <= 2000.0f);
            for (int id = TrackInParam::FOLLOW_REL + 1; id < TrackInParam::END; ++id)
                expectEquals(r.audio.getTrackParameter(0, id), 0.0f);

            // Pressure moves a value without changing what it is read as afterwards being wrong.
            r.audio.setTrackParameter(0, TrackInParam::LEVEL, 0.5f);
            r.audio.modulateTrackParameter(0, TrackInParam::LEVEL, 0.9f);
            expectEquals(r.audio.getTrackParameter(0, TrackInParam::LEVEL), 0.9f);
        }

        beginTest("The input reaches the engine: the source's sound times LEVL, both sides, and none while SRC is OFF");
        {
            Rig r;
            auto* tone = r.add<ToneEngine>(0);
            auto* echo = r.add<EchoEngine>(1);
            tone->left = 0.5f;
            tone->right = -0.25f;
            r.blocks(3);
            expect(! echo->connected && echo->calls == 3, "no input until SRC is set");

            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            r.blocks(3);
            expect(echo->connected);
            expectEquals(echo->frames, Rig::BLOCK);
            expect(echo->inL.size() == (size_t) Rig::BLOCK && echo->inR.size() == (size_t) Rig::BLOCK);
            for (int i : { 0, 17, Rig::BLOCK - 1 })
            {
                expectWithinAbsoluteError(echo->inL[(size_t) i], fxb::softSat(0.5f), 1.0e-6f);
                expectWithinAbsoluteError(echo->inR[(size_t) i], fxb::softSat(-0.25f), 1.0e-6f);
            }
            // The test engine copies its input to its output: it is in the mix (the tone's own fader shut).
            r.audio.setTrackParameter(0, MixParam::VOLUME, 0.0f);
            r.blocks(4);
            expect(r.peak() > 0.05f, "the engine's copy of its input is heard: " + juce::String(r.peak()));
            echo->copy = false;
            r.blocks(4);
            expectEquals(r.peak(), 0.0f, "MODE OFF adds nothing by itself");

            r.audio.setTrackParameter(1, TrackInParam::SRC, 0.0f);
            r.blocks(2);
            expect(! echo->connected && echo->frames == 0 && echo->envelope == 0.0f);
        }

        beginTest("A lower-numbered source is this block's, a higher-numbered one exactly one block late");
        {
            Rig r;
            auto* before = r.add<EchoEngine>(0);
            auto* counter = r.add<ToneEngine>(1);
            auto* after = r.add<EchoEngine>(2);
            counter->countBlocks = true;        // block n holds 0.01 n
            r.audio.setTrackParameter(0, TrackInParam::SRC, 2.0f);
            r.audio.setTrackParameter(2, TrackInParam::SRC, 2.0f);
            r.blocks(2);
            for (int n = 3; n <= 12; ++n)
            {
                r.blocks(1);
                expectEquals(counter->blocksDone, n);
                for (int i : { 0, Rig::BLOCK - 1 })
                {
                    expectWithinAbsoluteError(after->inL[(size_t) i], fxb::softSat(0.01f * (float) n), 1.0e-6f);
                    expectWithinAbsoluteError(before->inL[(size_t) i], fxb::softSat(0.01f * (float) (n - 1)), 1.0e-6f);
                }
            }
        }

        beginTest("LEVL scales the input, moves to a new value across one block, and the limiter holds it inside 1");
        {
            Rig r;
            auto* tone = r.add<ToneEngine>(0);
            auto* echo = r.add<EchoEngine>(1);
            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            tone->left = tone->right = 0.4f;
            for (float level : { 1.0f, 0.5f, 0.1f, 0.0f })
            {
                r.audio.setTrackParameter(1, TrackInParam::LEVEL, level);
                r.blocks(2);
                expectWithinAbsoluteError(echo->inL[5], fxb::softSat(0.4f * level), 1.0e-6f);
            }
            // From 0 to 1: the block of the change rises through it, it does not jump.
            r.audio.setTrackParameter(1, TrackInParam::LEVEL, 1.0f);
            r.blocks(1);
            expect(echo->inL[0] < 0.05f && echo->inL[(size_t) Rig::BLOCK / 2] > 0.1f && echo->inL[(size_t) Rig::BLOCK / 2] < 0.3f);
            expectWithinAbsoluteError(echo->inL[(size_t) Rig::BLOCK - 1], fxb::softSat(0.4f), 1.0e-5f);
            for (int i = 1; i < Rig::BLOCK; ++i)
                expect(echo->inL[(size_t) i] - echo->inL[(size_t) i - 1] < 0.02f);

            // Another source: it comes up from nothing over the block of the change (no jump from
            // one track's sound to the other's level).
            auto* second = r.add<ToneEngine>(2);
            second->left = second->right = 0.4f;
            r.blocks(2);
            r.audio.setTrackParameter(1, TrackInParam::SRC, 3.0f);
            r.blocks(1);
            expect(echo->inL[0] < 0.05f, "a new source starts from nothing: " + juce::String(echo->inL[0]));
            expect(echo->inL[(size_t) Rig::BLOCK / 2] > 0.1f && echo->inL[(size_t) Rig::BLOCK / 2] < 0.3f);
            expectWithinAbsoluteError(echo->inL[(size_t) Rig::BLOCK - 1], fxb::softSat(0.4f), 1.0e-5f);
            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            r.blocks(2);

            // A quiet source passes almost as it is; a far too loud one is held.
            tone->left = tone->right = 0.05f;
            r.blocks(2);
            expectWithinAbsoluteError(echo->inL[5], 0.05f, 0.001f);
            for (float loud : { 1.0f, 3.0f, 50.0f, -50.0f, 1.0e6f })
            {
                tone->left = loud;
                tone->right = -loud;
                r.blocks(2);
                expect(std::abs(echo->inL[5]) <= 1.0f && std::abs(echo->inR[5]) <= 1.0f, juce::String(loud) + ": " + juce::String(echo->inL[5]));
                expect(std::abs(echo->inL[5]) > 0.7f && echo->inL[5] * loud > 0.0f && echo->inR[5] * loud < 0.0f);
                expect(echo->envelope <= 1.0f);
            }
            expectEquals(r.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("MODE: OFF leaves the track's sound, MIX adds the input before the effects, RING multiplies the sound by it");
        {
            Rig r;
            auto* source = r.add<ToneEngine>(0);
            auto* own = r.add<ToneEngine>(1);
            auto* probe = r.probe(1);
            source->left = 0.5f;  source->right = 0.2f;
            own->left = 0.3f;     own->right = -0.6f;
            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            const float inL = fxb::softSat(0.5f), inR = fxb::softSat(0.2f);

            r.blocks(3);
            expectWithinAbsoluteError(probe->audioL[9], 0.3f, 1.0e-6f);
            expectWithinAbsoluteError(probe->audioR[9], -0.6f, 1.0e-6f);

            r.audio.setTrackParameter(1, TrackInParam::MODE, 1.0f);
            r.blocks(3);
            expectWithinAbsoluteError(probe->audioL[9], 0.3f + inL, 1.0e-6f);
            expectWithinAbsoluteError(probe->audioR[9], -0.6f + inR, 1.0e-6f);
            // MIX with nothing of its own: the track is a bus for its source.
            own->left = own->right = 0.0f;
            r.blocks(2);
            expectWithinAbsoluteError(probe->audioL[9], inL, 1.0e-6f);
            // (The old TrackFx blocks ran before the insert chain; with DIRT as an insert, it runs
            //  after the probe. The input going through the track's effects is covered by the insert
            //  chain tests in send_chain_tests.h.)
            own->left = 0.3f;
            own->right = -0.6f;

            r.audio.setTrackParameter(1, TrackInParam::MODE, 2.0f);
            r.blocks(3);
            expectWithinAbsoluteError(probe->audioL[9], 0.3f * inL, 1.0e-6f);
            expectWithinAbsoluteError(probe->audioR[9], -0.6f * inR, 1.0e-6f);
            // RING of a silent source is silence; RING with LEVL at 0 too.
            source->left = source->right = 0.0f;
            r.blocks(2);
            expectEquals(probe->audioL[9], 0.0f);
            source->left = 0.5f;
            r.audio.setTrackParameter(1, TrackInParam::LEVEL, 0.0f);
            r.blocks(2);
            expectEquals(probe->audioL[9], 0.0f);
            r.audio.setTrackParameter(1, TrackInParam::LEVEL, 1.0f);
            r.blocks(2);

            // A change of MODE crosses over one block: no step larger than the block's share of it.
            r.audio.setTrackParameter(1, TrackInParam::MODE, 0.0f);
            r.blocks(1);
            const float from = 0.3f * inL, to = 0.3f;
            expect(probe->audioL[0] < from + 0.02f && std::abs(probe->audioL[(size_t) Rig::BLOCK - 1] - to) < 1.0e-5f);
            for (int i = 1; i < Rig::BLOCK; ++i)
                expect(std::abs(probe->audioL[(size_t) i] - probe->audioL[(size_t) i - 1]) < 2.0f * (to - from) / (float) Rig::BLOCK);

            // RING with the source taken away: the track's own sound is back.
            r.audio.setTrackParameter(1, TrackInParam::MODE, 2.0f);
            r.blocks(2);
            r.audio.setTrackParameter(1, TrackInParam::SRC, 0.0f);
            r.blocks(2);
            expectWithinAbsoluteError(probe->audioL[9], 0.3f, 1.0e-6f);
            expectEquals(r.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("The follower rises in FATK and falls in FREL, and the engine is given it");
        {
            // One block is one millisecond here.
            auto measure = [this](float attackMs, float releaseMs, int riseBlocks, int fallBlocks, float& rise, float& fall)
            {
                Rig r(48, 48000.0);
                auto* tone = r.add<ToneEngine>(0);
                auto* echo = r.add<EchoEngine>(1);
                r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
                r.audio.setTrackParameter(1, TrackInParam::FOLLOW_ATK, attackMs);
                r.audio.setTrackParameter(1, TrackInParam::FOLLOW_REL, releaseMs);
                r.blocks(5);
                expectEquals(echo->envelope, 0.0f, "silence in: nothing");
                tone->left = tone->right = 0.8f;
                r.blocks(riseBlocks + 1);           // (the value handed over is the one the block starts with)
                rise = echo->envelope / fxb::softSat(0.8f);
                r.blocks(3000);
                expectWithinAbsoluteError(echo->envelope, fxb::softSat(0.8f), 0.002f);
                tone->left = tone->right = 0.0f;
                r.blocks(fallBlocks + 1);
                fall = echo->envelope / fxb::softSat(0.8f);
            };
            float rise = 0.0f, fall = 0.0f;
            measure(10.0f, 100.0f, 10, 100, rise, fall);       // after one time constant each
            expectWithinAbsoluteError(rise, 1.0f - std::exp(-1.0f), 0.02f);
            expectWithinAbsoluteError(fall, std::exp(-1.0f), 0.02f);
            measure(40.0f, 400.0f, 10, 100, rise, fall);       // four times slower: a quarter of a time constant
            expectWithinAbsoluteError(rise, 1.0f - std::exp(-0.25f), 0.02f);
            expectWithinAbsoluteError(fall, std::exp(-0.25f), 0.02f);
            measure(5.0f, 150.0f, 5, 150, rise, fall);         // the defaults
            expectWithinAbsoluteError(rise, 1.0f - std::exp(-1.0f), 0.03f);
            expectWithinAbsoluteError(fall, std::exp(-1.0f), 0.02f);

            // The same times at another sample rate and block size.
            Rig r(96, 96000.0);
            auto* tone = r.add<ToneEngine>(0);
            auto* echo = r.add<EchoEngine>(1);
            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            r.audio.setTrackParameter(1, TrackInParam::FOLLOW_ATK, 10.0f);
            tone->left = 0.8f;
            r.blocks(11);
            expectWithinAbsoluteError(echo->envelope / fxb::softSat(0.8f), 1.0f - std::exp(-1.0f), 0.02f);
        }

        beginTest("An effect that wants the input is given it before it runs, and told when there is none");
        {
            Rig r;
            auto* tone = r.add<ToneEngine>(0);
            r.add<ToneEngine>(1);
            auto* probe = r.probe(1);
            r.audio.addInsertEffect(1, EffectType::VocoderEffect);      // the real one, beside it
            r.audio.addInsertEffect(1, EffectType::WaveshaperEffect);   // and one that does not want it
            tone->left = 0.5f;
            tone->right = 0.1f;
            r.blocks(2);
            expect(probe->gives == 2 && ! probe->given && probe->frames == 0, "no input: told so each block");

            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            r.blocks(3);
            expect(probe->gives == 5 && probe->given && probe->frames == Rig::BLOCK);
            expectWithinAbsoluteError(probe->inL[9], fxb::softSat(0.5f), 1.0e-6f);
            expectWithinAbsoluteError(probe->inR[9], fxb::softSat(0.1f), 1.0e-6f);
            expect(probe->givenBeforeProcess, "set before the chain runs, in every block");

            r.audio.setTrackParameter(1, TrackInParam::SRC, 0.0f);
            r.blocks(1);
            expect(! probe->given && probe->frames == 0);
            expectEquals(r.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("SRC = the track itself, a track without an engine or a number past the last track: no input");
        {
            Rig r;
            auto* tone = r.add<ToneEngine>(0);
            auto* echo = r.add<EchoEngine>(1);
            auto* probe = r.probe(1);
            tone->left = 0.5f;
            r.audio.setTrackParameter(1, TrackInParam::MODE, 1.0f);
            for (float src : { 2.0f, 3.0f, 8.0f, 20.0f, 32.0f, 33.0f, 1000.0f, -1.0f })
            {
                r.audio.setTrackParameter(1, TrackInParam::SRC, src);
                r.blocks(3);
                expect(! echo->connected && echo->envelope == 0.0f, "SRC " + juce::String(src));
                expect(! probe->given);
                expectEquals(probe->audioL[9], 0.0f, "nothing mixed in");
            }
            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            r.blocks(2);
            expect(echo->connected);
            expectEquals(r.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("A muted or soloed-out source still feeds its listener; it is silent in the mix, and plays nothing when nobody listens");
        {
            struct Play
            {
                Rig r;
                BurstEngine* source = nullptr;
                EchoEngine* listener = nullptr;
                float fed = 0.0f, heard = 0.0f;

                Play()
                {
                    source = r.add<BurstEngine>(0);
                    listener = r.add<EchoEngine>(1);
                    r.add<EchoEngine>(2);
                    listener->copy = false;         // only the source could be heard
                    auto& track = r.seq.getPattern().getTrack(0);
                    track.setLength(16);
                    for (int s = 0; s < 16; s += 2)
                    {
                        track.getStep(s).trig = true;
                        track.getStep(s).notes.push_back({ 60, 1.0f });
                    }
                    r.seq.setTempo(120.0f);
                    r.seq.setTrigCallback([this](int t, const Step& step)
                    {
                        if (auto* e = r.audio.getEngine(t))
                            for (auto& note : step.notes) e->noteOn(note.pitch, note.velocity, -1);
                    });
                }
                void run()
                {
                    r.blocks(4);                    // (a fader that was just shut has closed)
                    source->noteOns = 0;
                    r.seq.start();
                    fed = heard = 0.0f;
                    for (int b = 0; b < 750; ++b)   // one second: eight sixteenth-note pairs
                    {
                        r.blocks(1);
                        heard = std::max(heard, r.peak());
                        if (listener->connected)
                            for (float v : listener->inL) fed = std::max(fed, std::abs(v));
                    }
                    r.seq.stop();
                    r.blocks(40);
                }
            };

            Play p;
            p.r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            p.run();
            expect(p.source->noteOns >= 4 && p.fed > 0.3f && p.heard > 0.05f, "playing: fed " + juce::String(p.fed) + ", heard " + juce::String(p.heard));

            p.r.seq.getPattern().getTrack(0).muted = true;
            p.run();
            expect(p.source->noteOns >= 4, "the muted source goes on playing: " + juce::String(p.source->noteOns));
            expect(p.fed > 0.3f, "and feeds: " + juce::String(p.fed));
            expectEquals(p.heard, 0.0f, "but is not heard");

            // Soloed out by another track: the same.
            p.r.seq.getPattern().getTrack(0).muted = false;
            p.r.seq.getPattern().getTrack(2).soloed = true;
            p.run();
            expect(p.source->noteOns >= 4 && p.fed > 0.3f, "soloed out: fed " + juce::String(p.fed));
            expectEquals(p.heard, 0.0f);
            p.r.seq.getPattern().getTrack(2).soloed = false;
            p.run();
            expect(p.heard > 0.05f, "heard again");

            // Nobody listens: a muted track plays no note, as before there were inputs.
            p.r.seq.getPattern().getTrack(0).muted = true;
            p.r.audio.setTrackParameter(1, TrackInParam::SRC, 0.0f);
            p.run();
            expectEquals(p.source->noteOns, 0);
            expectEquals(p.heard, 0.0f);
            // A listener whose SRC is itself or a missing track is nobody.
            p.r.audio.setTrackParameter(0, TrackInParam::SRC, 1.0f);
            p.r.audio.setTrackParameter(1, TrackInParam::SRC, 20.0f);
            p.run();
            expectEquals(p.source->noteOns, 0);

            // A MIDI track that is somebody's source is not kept playing while muted: its notes
            // go to an instrument outside, which no fader here could shut.
            struct MidiBurst : public BurstEngine
            {
                EngineType getType() const override { return EngineType::MIDITrack; }
            };
            auto* midi = p.r.add<MidiBurst>(0);
            p.source = midi;
            p.r.audio.setTrackParameter(0, TrackInParam::SRC, 0.0f);
            p.r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            p.r.seq.getPattern().getTrack(0).muted = false;
            p.run();
            expect(midi->noteOns >= 4, "the MIDI track plays while not muted: " + juce::String(midi->noteOns));
            p.r.seq.getPattern().getTrack(0).muted = true;
            p.run();
            expectEquals(midi->noteOns, 0, "a muted MIDI track sends nothing, listened to or not");
            expectEquals(p.r.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("A feedback pair holds: MIX on both, each other's SRC, LEVL at the top, any inserts, 10 s");
        {
            // What each track gave (after its inserts) and was given, over a run.
            struct Loop
            {
                float peak = 0.0f, inPeak = 0.0f, late = 0.0f;
                bool finite = true;
                int dropped = 0;
            };
            auto run = [](double sampleRate, int block, double seconds, int modeA, int modeB,
                          const std::vector<EffectType>& inserts, bool fold)
            {
                Rig r(block, sampleRate);
                r.add<BurstEngine>(0)->trigger((int) (0.2 * sampleRate), 0.9f);
                r.add<BurstEngine>(1);
                for (int t = 0; t < 2; ++t)
                {
                    for (int i = 0; i < (int) inserts.size(); ++i)
                    {
                        r.audio.addInsertEffect(t, inserts[(size_t) i]);
                        for (int p = 0; p < 4; ++p)        // every cell at its top: full drive, feedback, depth
                        {
                            const auto spec = r.audio.getInsertChain(t)->getEffect(i)->getParamSpec(p);
                            if (spec.isValid())
                                r.audio.setTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + i * 4 + p, spec.maxVal);
                        }
                        if (fold && inserts[(size_t) i] == EffectType::WaveshaperEffect)
                            r.audio.setTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + i * 4 + 1, (float) sat::Fold);
                    }
                    r.audio.setTrackParameter(t, TrackInParam::SRC, (float) (2 - t));
                    r.audio.setTrackParameter(t, TrackInParam::LEVEL, 1.0f);
                    r.audio.setTrackParameter(t, TrackInParam::MODE, (float) (t == 0 ? modeA : modeB));
                }
                auto* probeA = r.probe(0);
                auto* probeB = r.probe(1);
                Loop result;
                const int total = (int) (seconds * sampleRate / (double) block);
                for (int b = 0; b < total; ++b)
                {
                    r.blocks(1);
                    for (auto* probe : { probeA, probeB })
                    {
                        for (float v : probe->audioL) { result.finite = result.finite && std::isfinite(v); result.peak = std::max(result.peak, std::abs(v)); }
                        for (float v : probe->audioR) { result.finite = result.finite && std::isfinite(v); result.peak = std::max(result.peak, std::abs(v)); }
                        for (float v : probe->inL) result.inPeak = std::max(result.inPeak, std::abs(v));
                        if (b >= total - 8)
                            for (float v : probe->audioL) result.late = std::max(result.late, std::abs(v));
                    }
                    result.finite = result.finite && std::isfinite(r.peak());
                }
                result.dropped = r.audio.getNonFiniteBlocks();
                return result;
            };
            // `open`: the same chain with no loop (both MODE OFF). The inserts at the top of every
            // cell have a gain of their own (a resonant filter into a compressor's make-up: 88);
            // what the loop may add to it is bounded by the limiter on each input.
            auto check = [this](const Loop& l, const juce::String& what, float open = 1.0f)
            {
                expect(l.finite && l.dropped == 0, what + ": finite, no block dropped (" + juce::String(l.dropped) + ")");
                expect(l.inPeak <= 1.000001f, what + ": the input stays inside 1: " + juce::String(l.inPeak, 8));
                expect(l.peak < 4.0f * std::max(1.0f, open), what + ": bounded: " + juce::String(l.peak) + " (open " + juce::String(open) + ")");
            };

            // The howl: a wavefolder at full drive in each half of the loop, ten seconds.
            const Loop howl = run(48000.0, 64, 10.0, 1, 1, { EffectType::WaveshaperEffect }, true);
            check(howl, "FOLD 10 s");
            expect(howl.inPeak > 0.1f, "the loop is closed: " + juce::String(howl.inPeak));
            expect(howl.late > 0.05f, "and howls on by itself, ten seconds after the burst: " + juce::String(howl.late));
            logMessage("feedback pair, WSH FOLD full drive: peak " + juce::String(howl.peak, 3) + ", still sounding after 10 s at "
                       + juce::String(howl.late, 3));

            // Every pair of modes, with and without inserts, shorter.
            const std::vector<std::vector<EffectType>> chains = {
                {}, { EffectType::WaveshaperEffect }, { EffectType::AmpEffect, EffectType::DelayEffect },
                { EffectType::DriveEffect, EffectType::FilterEffectType, EffectType::CompressorEffect },
                { EffectType::VocoderEffect, EffectType::RingModEffect, EffectType::TapeDelayEffect } };
            for (size_t c = 0; c < chains.size(); ++c)
            {
                float open = 1.0f;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                    {
                        const Loop l = run(48000.0, 64, 0.5, a, b, chains[c], c == 1);
                        if (a == 0 && b == 0) open = l.peak;
                        check(l, "modes " + juce::String(a) + " " + juce::String(b) + ", chain " + juce::String((int) c), open);
                    }
            }
            // The ends of the sample rates and block sizes.
            check(run(44100.0, 1, 0.1, 1, 1, { EffectType::WaveshaperEffect }, true), "44.1 kHz, blocks of 1");
            check(run(192000.0, 2048, 1.0, 1, 1, { EffectType::WaveshaperEffect }, true), "192 kHz, blocks of 2048");
            check(run(96000.0, 333, 0.5, 1, 2, { EffectType::AmpEffect }, false), "96 kHz, blocks of 333", 4.0f);     // (the amp at full gain)
        }

        beginTest("A feedback pair of real engines with held notes stays finite; a block shorter than the prepared one is taken");
        {
            Rig r(256, 48000.0);
            r.seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            r.audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            r.audio.addEngine(1, AudioEngine::createEngine(EngineType::FM));
            r.audio.prepare(48000.0, 256);
            for (int t = 0; t < 2; ++t)
            {
                r.audio.addInsertEffect(t, EffectType::WaveshaperEffect);
                r.audio.setTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE, 1.0f);
                r.audio.setTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + 1, (float) sat::Fold);
                r.audio.setTrackParameter(t, TrackInParam::SRC, (float) (2 - t));
                r.audio.setTrackParameter(t, TrackInParam::MODE, t == 0 ? 1.0f : 2.0f);
                r.audio.getEngine(t)->noteOn(48 + 7 * t, 1.0f, -1);
            }
            bool finite = true;
            float peak = 0.0f;
            for (int b = 0; b < 190; ++b)           // one second, in blocks of every length
            {
                r.blocks(1, 1 + (b * 37) % 256);
                finite = finite && std::isfinite(r.peak());
                peak = std::max(peak, r.peak());
            }
            expect(finite && peak > 0.001f && peak <= 1.5f, "peak " + juce::String(peak));
            expectEquals(r.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("A step lock on LEVL: held while its step plays, back at the track's value on the next trig");
        {
            Rig r;
            auto* tone = r.add<ToneEngine>(0);
            auto* echo = r.add<EchoEngine>(1);
            tone->left = 0.6f;
            r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            r.audio.setTrackParameter(1, TrackInParam::LEVEL, 0.8f);
            r.blocks(2);
            expectWithinAbsoluteError(echo->inL[5], fxb::softSat(0.6f * 0.8f), 1.0e-6f);

            Step locked, plain;
            locked.trig = plain.trig = true;
            locked.paramLocks[TrackInParam::LEVEL] = 0.25f;
            locked.paramLocks[TrackInParam::MODE] = 1.0f;
            r.audio.applyStepLocks(1, locked);
            expectEquals(r.audio.getTrackParameter(1, TrackInParam::LEVEL), 0.25f);
            expectEquals(r.audio.getTrackParameter(1, TrackInParam::MODE), 1.0f);
            r.blocks(2);
            expectWithinAbsoluteError(echo->inL[5], fxb::softSat(0.6f * 0.25f), 1.0e-6f);
            // The knob turned while the lock holds: that is what it returns to.
            r.audio.setTrackParameter(1, TrackInParam::LEVEL, 0.5f);
            r.audio.applyStepLocks(1, locked);
            expectEquals(r.audio.getTrackParameter(1, TrackInParam::LEVEL), 0.25f);
            r.audio.applyStepLocks(1, plain);
            expectEquals(r.audio.getTrackParameter(1, TrackInParam::LEVEL), 0.5f);
            expectEquals(r.audio.getTrackParameter(1, TrackInParam::MODE), 0.0f);
            r.blocks(2);
            expectWithinAbsoluteError(echo->inL[5], fxb::softSat(0.6f * 0.5f), 1.0e-6f);

            // From the sequencer, as the app wires it.
            r.seq.setTrigCallback([&](int t, const Step& step) { r.audio.applyStepLocks(t, step); });
            auto& track = r.seq.getPattern().getTrack(1);
            track.setLength(16);
            track.soundParams[TrackInParam::LEVEL] = 0.5f;
            track.getStep(0).trig = true;
            track.getStep(0).paramLocks[TrackInParam::LEVEL] = 0.1f;
            track.getStep(8).trig = true;
            r.seq.setTempo(120.0f);
            r.seq.start();
            r.blocks(20);                           // inside step 1 (a step is 6000 samples)
            expectWithinAbsoluteError(echo->inL[5], fxb::softSat(0.6f * 0.1f), 1.0e-6f);
            r.blocks(770);                          // past step 9
            expectWithinAbsoluteError(echo->inL[5], fxb::softSat(0.6f * 0.5f), 1.0e-6f);
            r.seq.stop();
        }

        // =============================== TRK FX page IN ===============================

        beginTest("TRK FX page IN from the controller: Func + top pad 5, SRC skips the track itself, LEVL MODE FATK FREL reach the engine");
        {
            Ui u;
            expectEquals(u.fx->getPageCount(), 9);          // (the track's five, and two a send since the sends have pages)
            expectEquals(u.fx->getPageKeyCount(), 8);    // min(8, 9 pages)
            expectEquals(u.fx->getPageKeyName(4), juce::String("IN"));
            u.mgr.controlChangeReceived(1, 105, 127);       // Func
            u.mgr.noteOnReceived(1, 100, 100);              // + top pad 5 (IN is page key 4)
            u.mgr.noteOffReceived(1, 100);
            u.mgr.controlChangeReceived(1, 105, 0);
            expectEquals(u.fx->getPageName(), juce::String("IN"));
            expectEquals(u.fx->getSubPage(), juce::String("IN"));

            const auto page = u.fx->getCurrentPage();
            const char* labels[8] = { "SRC", "LEVL", "MODE", nullptr, "FATK", "FREL", nullptr, nullptr };
            for (int i = 0; i < 8; ++i)
            {
                expect(page.params[(size_t) i].isValid() == (labels[i] != nullptr), "cell " + juce::String(i + 1));
                if (labels[i] != nullptr)
                    expectEquals(juce::String(page.params[(size_t) i].label), juce::String(labels[i]));
            }
            const int tracks = u.seq.getPattern().getTrackCount();
            expect(tracks >= 3);
            expectEquals((int) page.params[0].maxVal, tracks, "SRC ends at the last track");
            expectEquals(juce::String(page.params[0].choices[tracks]), "T" + juce::String(tracks));

            // A track more: SRC's list grows with it (the page is made again).
            u.seq.getPattern().addTrack();
            expectEquals((int) u.fx->getCurrentPage().params[0].maxVal, tracks + 1, "SRC follows the track count");
            u.seq.getPattern().removeTrack(tracks);
            expectEquals((int) u.fx->getCurrentPage().params[0].maxVal, tracks);

            // Track 1: one click up from OFF is T2 (never T1), down again is OFF.
            auto src = [&](int t = -1) { return (int) u.audio.getTrackParameter(t < 0 ? u.seq.getSelectedTrack() : t, TrackInParam::SRC); };
            u.turn(0, 1);
            expectEquals(src(), 2);
            expectEquals((int) u.seq.getPattern().getTrack(0).soundParams[TrackInParam::SRC], 2, "kept with the sound");
            u.turn(0, 1);
            expectEquals(src(), 3);
            u.turn(0, -1);
            u.turn(0, -1);
            expectEquals(src(), 0);
            u.turn(0, -1);
            expectEquals(src(), 0);
            for (int i = 0; i < 40; ++i) u.turn(0, 1);
            expectEquals(src(), tracks, "the top of the list is the last track");
            for (int i = 0; i < 40; ++i) u.turn(0, -1);
            expectEquals(src(), 0);

            // Track 2: up from OFF is T1, then T3; down from T3 is T1.
            u.seq.setSelectedTrack(1);
            u.turn(0, 1);
            expectEquals(src(), 1);
            u.turn(0, 1);
            expectEquals(src(), 3);
            u.turn(0, -1);
            expectEquals(src(), 1);
            expectEquals(src(0), 0, "the other track's input is its own");
            // The last track: its own number is the top of the list and is never reached.
            u.seq.setSelectedTrack(tracks - 1);
            for (int i = 0; i < 40; ++i) u.turn(0, 1);
            expectEquals(src(), tracks - 1);
            // Whatever the step size (Shift fine, Func coarse), never the track itself.
            u.seq.setSelectedTrack(2);
            for (int mod : { 63, 105 })
            {
                u.mgr.controlChangeReceived(mod == 63 ? 7 : 1, mod, 127);
                for (int i = 0; i < 12; ++i) { u.turn(0, 1); expect(src() != 3); }
                for (int i = 0; i < 12; ++i) { u.turn(0, -1); expect(src() != 3); }
                u.mgr.controlChangeReceived(mod == 63 ? 7 : 1, mod, 0);
            }

            // The other cells.
            u.seq.setSelectedTrack(1);
            auto value = [&](int id) { return u.audio.getTrackParameter(1, id); };
            u.turn(1, -10);
            expectWithinAbsoluteError(value(TrackInParam::LEVEL), 0.9f, 0.001f);
            expectWithinAbsoluteError(u.seq.getPattern().getTrack(1).soundParams[TrackInParam::LEVEL], 0.9f, 0.001f);
            u.turn(2, 1);
            expectEquals(value(TrackInParam::MODE), 1.0f);
            expectEquals(u.fx->text(2), juce::String("MIX"));
            u.turn(2, 1);
            expectEquals(u.fx->text(2), juce::String("RING"));
            u.turn(2, 5);
            expectEquals(value(TrackInParam::MODE), 2.0f);
            u.turn(2, -5);
            expectEquals(u.fx->text(2), juce::String("OFF"));
            const float attack = value(TrackInParam::FOLLOW_ATK), release = value(TrackInParam::FOLLOW_REL);
            u.turn(4, 10);
            u.turn(5, -10);
            expect(value(TrackInParam::FOLLOW_ATK) > attack * 1.2f && value(TrackInParam::FOLLOW_REL) < release * 0.8f);
            for (int i = 0; i < 6; ++i) { u.turn(4, 60); u.turn(5, 60); }
            expectWithinAbsoluteError(value(TrackInParam::FOLLOW_ATK), 200.0f, 0.01f);
            expectWithinAbsoluteError(value(TrackInParam::FOLLOW_REL), 2000.0f, 0.1f);
            const float before = value(TrackInParam::LEVEL);
            u.turn(3, 5); u.turn(6, 5); u.turn(7, 5);       // no cells there
            expectEquals(value(TrackInParam::LEVEL), before);

            // What the source cell shows.
            expectEquals(u.fx->text(0), juce::String("T1"));
            u.audio.setTrackParameter(1, TrackInParam::SRC, 2.0f);  // the track itself, from a file
            expectEquals(u.fx->text(0), juce::String("--"));

            // It is heard: the knob's source arrives in the listening track's engine.
            u.audio.setTrackParameter(1, TrackInParam::SRC, 0.0f);
            auto tone = std::make_shared<ToneEngine>();
            auto echo = std::make_shared<EchoEngine>();
            tone->left = 0.5f;
            u.audio.addEngine(0, tone);
            u.audio.addEngine(1, echo);
            u.turn(0, 1);
            std::vector<float> l(64), rr(64);
            for (int b = 0; b < 3; ++b) u.audio.renderBlock(l.data(), rr.data(), 64);
            expect(echo->connected);
            expectWithinAbsoluteError(echo->inL[5], fxb::softSat(0.5f * value(TrackInParam::LEVEL)), 1.0e-5f);
        }

        beginTest("Page IN with a step held: LEVL and MODE are locked on the step, SRC is not; SRC takes no pressure range");
        {
            Ui u;
            u.fx->onPageKey(4);       // IN is page key 4
            auto& track = u.seq.getPattern().getTrack(0);
            u.fx->onPadPress(2, 100);                       // hold step 3
            expect(u.fx->isStepHeld());
            u.turn(1, -20);
            u.turn(2, 1);
            u.turn(4, 5);
            auto& locks = track.getStep(2).paramLocks;
            expect(locks.count(TrackInParam::LEVEL) == 1 && locks.count(TrackInParam::MODE) == 1 && locks.count(TrackInParam::FOLLOW_ATK) == 1);
            expectWithinAbsoluteError(locks[TrackInParam::LEVEL], 0.8f, 0.001f);
            expectEquals(locks[TrackInParam::MODE], 1.0f);
            expectEquals(u.audio.getTrackParameter(0, TrackInParam::LEVEL), 1.0f, "the track's own value is untouched");
            u.turn(0, 1);
            expect(locks.count(TrackInParam::SRC) == 0 && locks.count(TrackFxScreen::IN_SRC) == 0, "no lock on SRC");
            expectEquals(locks.size(), (size_t) 3);
            u.fx->onPadRelease(2);

            track.soundParams[RangeParam::LO_BASE + TrackInParam::LEVEL] = 0.2f;
            track.soundParams[RangeParam::LO_BASE + TrackInParam::SRC] = 1.0f;
            track.soundParams[RangeParam::LO_BASE + TrackFxScreen::IN_SRC] = 1.0f;
            expect(u.fx->cellHasRange(1), "LEVL can have a pressure range");
            expect(! u.fx->cellHasRange(0), "SRC cannot");
        }

        beginTest("The effects of a track as one thing (copy / clear / paste on TRK FX) include its input");
        {
            Ui u;
            const auto hooks = GrammarHooks::make(u.seq, u.audio);
            for (int id : AudioEngine::TRACK_IN_IDS)
                expect(hooks.isFxParam(id) && hooks.isFxParam(RangeParam::LO_BASE + id) && hooks.isFxParam(RangeParam::HI_BASE + id));
            expect(! hooks.isFxParam(MixParam::VOLUME) && ! hooks.isFxParam(TrackInParam::END) && ! hooks.isFxParam(0));
            ParamMap cleared;
            hooks.fxDefaults(cleared);
            for (int id : AudioEngine::TRACK_IN_IDS)
                expect(cleared.count(id) == 1 && cleared[id] == AudioEngine::trackInDefault(id));

            // A captured sound carries the input; applied to another track it arrives; a sound
            // without one (captured before there were inputs) leaves none.
            u.audio.setTrackParameter(0, TrackInParam::SRC, 3.0f);
            u.audio.setTrackParameter(0, TrackInParam::LEVEL, 0.3f);
            u.audio.setTrackParameter(0, TrackInParam::MODE, 2.0f);
            u.audio.setTrackParameter(0, TrackInParam::FOLLOW_REL, 600.0f);
            ParamMap sound = hooks.captureSound(0);
            expectEquals(sound[TrackInParam::SRC], 3.0f);
            expectEquals(sound[TrackInParam::LEVEL], 0.3f);
            hooks.applySound(1, EngineType::UnifiedVoice, sound);
            expectEquals(u.audio.getTrackParameter(1, TrackInParam::SRC), 3.0f);
            expectEquals(u.audio.getTrackParameter(1, TrackInParam::MODE), 2.0f);
            expectEquals(u.audio.getTrackParameter(1, TrackInParam::FOLLOW_REL), 600.0f);
            for (int id : AudioEngine::TRACK_IN_IDS) sound.erase(id);
            hooks.applySound(1, EngineType::UnifiedVoice, sound);
            for (int id : AudioEngine::TRACK_IN_IDS)
                expectEquals(u.audio.getTrackParameter(1, id), AudioEngine::trackInDefault(id));

            // From the controller: Func + Record... is ScreenManager's; here its clear, by the hooks it uses.
            u.mgr.setActiveScreen(0);
            u.audio.setTrackParameter(0, MixParam::VOLUME, 0.33f);
            ParamMap own = hooks.captureSound(0);
            for (auto it = own.begin(); it != own.end();)
                it = hooks.isFxParam(it->first) ? own.erase(it) : std::next(it);
            hooks.fxDefaults(own);
            hooks.applySound(0, EngineType::UnifiedVoice, own);
            expectEquals(u.audio.getTrackParameter(0, TrackInParam::SRC), 0.0f, "cleared: no input");
            expectEquals(u.audio.getTrackParameter(0, TrackInParam::LEVEL), 1.0f);
            expectWithinAbsoluteError(u.audio.getTrackParameter(0, MixParam::VOLUME), 0.33f, 1.0e-6f, "the mixer stays");
        }

        beginTest("Projects: the input is saved and comes back; a project saved before there were inputs loads with none");
        {
            Ui u;
            u.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
            u.audio.setTrackParameter(1, TrackInParam::LEVEL, 0.35f);
            u.audio.setTrackParameter(1, TrackInParam::MODE, 1.0f);
            u.audio.setTrackParameter(1, TrackInParam::FOLLOW_ATK, 33.0f);
            u.audio.setTrackParameter(1, TrackInParam::FOLLOW_REL, 444.0f);
            u.audio.setTrackParameter(2, TrackInParam::SRC, 2.0f);
            u.audio.setTrackParameter(2, TrackInParam::MODE, 2.0f);
            u.seq.getPattern().getTrack(1).getStep(4).trig = true;
            u.seq.getPattern().getTrack(1).getStep(4).paramLocks[TrackInParam::LEVEL] = 0.75f;

            ProjectBridge::Live live { u.seq, u.audio };
            const auto data = ProjectBridge::capture(live, "in");
            const auto json = juce::JSON::toString(data.toVar());

            Ui other;
            // (the other instrument had inputs of its own: the project's replace them)
            other.audio.setTrackParameter(0, TrackInParam::SRC, 4.0f);
            other.audio.setTrackParameter(0, TrackInParam::MODE, 2.0f);
            other.audio.setTrackParameter(0, TrackInParam::LEVEL, 0.1f);
            ProjectBridge::Live otherLive { other.seq, other.audio };
            ProjectBridge::apply(ProjectData::fromVar(juce::JSON::parse(json)), otherLive);
            expectEquals(other.audio.getTrackParameter(1, TrackInParam::SRC), 1.0f);
            expectEquals(other.audio.getTrackParameter(1, TrackInParam::LEVEL), 0.35f);
            expectEquals(other.audio.getTrackParameter(1, TrackInParam::MODE), 1.0f);
            expectEquals(other.audio.getTrackParameter(1, TrackInParam::FOLLOW_ATK), 33.0f);
            expectEquals(other.audio.getTrackParameter(1, TrackInParam::FOLLOW_REL), 444.0f);
            expectEquals(other.audio.getTrackParameter(2, TrackInParam::SRC), 2.0f);
            expectEquals(other.audio.getTrackParameter(2, TrackInParam::MODE), 2.0f);
            for (int id : AudioEngine::TRACK_IN_IDS)
                expectEquals(other.audio.getTrackParameter(0, id), AudioEngine::trackInDefault(id), "track 1 had none in the project");
            expectEquals(other.seq.getPattern().getTrack(1).getStep(4).paramLocks[TrackInParam::LEVEL], 0.75f, "the lock too");

            // The same project as it was written before tracks had an input: no such values in it.
            ProjectData old = ProjectData::fromVar(juce::JSON::parse(json));
            int removed = 0;
            for (auto& pattern : old.patterns)
                for (auto& track : pattern.tracks)
                    for (int id = TrackInParam::BASE; id < TrackInParam::END; ++id)
                        removed += (int) track.soundParams.erase(id);
            expect(removed >= 5 * (int) old.patterns[0].tracks.size(), "the project held them: " + juce::String(removed));
            ProjectBridge::apply(ProjectData::fromVar(juce::JSON::parse(juce::JSON::toString(old.toVar()))), otherLive);
            for (int t = 0; t < other.seq.getPattern().getTrackCount(); ++t)
                for (int id : AudioEngine::TRACK_IN_IDS)
                    expectEquals(other.audio.getTrackParameter(t, id), AudioEngine::trackInDefault(id));
        }
    }
};

static TrackInputTests trackInputTests;
