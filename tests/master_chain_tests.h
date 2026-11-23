#pragma once

#include <juce_core/juce_core.h>
#include "engine/AudioEngine.h"
#include "sequencer/SequencerCore.h"
#include "master_chain_atlas.h"
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

// -----------------------------------------------------------------------------------------------
// Phase E5 of .ralph/spec-unify.md:
//   1. the master section is an ordinary EffectChain run by the block (AudioEngine::MASTER_TRACK),
//      whose content as a new project has it is the EQ, the compressor at 1:1 and the limiter that
//      were three members run a sample at a time: the same samples, bit for bit;
//   2. one dry / wet mix, in EffectChain, with a 4 ms line (WET works on EQ, GEQ, LMT, AMP too);
//   3. one way of handing another sound to an effect (KeyFeed): one key choice, SELF / T1..T16 /
//      IN, for every keyed effect, on a track, on the master chain and on a PERF pad;
//   4. wiring: the played note to an insert (setPitch), the track's custom LFO shape
//      (setUserShape), a block gone wrong in DIRT / TALK / MODFX kept off the tap and the sum,
//      no step lock on a cell that rebuilds data.
// NEPTUNE_E5_ATLAS / NEPTUNE_E5_COMPARE / NEPTUNE_E5_BENCH: see master_chain_atlas.h and bench().
// -----------------------------------------------------------------------------------------------
class MasterChainE5Tests : public juce::UnitTest
{
public:
    MasterChainE5Tests() : UnitTest("Master Chain") {}

    using Tone = e5atlas::ToneEngine;
    using Stereo = e5atlas::Stereo;
    static constexpr int M = AudioEngine::MASTER_TRACK;
    static constexpr int CELL = AudioEngine::EFFECT_PARAM_BASE;
    static constexpr int EXTRA = AudioEngine::INSERT_EXTRA_BASE;
    static constexpr int ON = AudioEngine::INSERT_ON_BASE;
    static constexpr int WET = AudioEngine::INSERT_ON_BASE + AudioEngine::MAX_INSERTS_PER_TRACK;
    static constexpr float KEY_IN = (float) dyn::KEY_CHOICE_IN;

    // Track 1 a drum that strikes by itself four times a second, track 2 the keys.
    struct Rig
    {
        AudioEngine audio;
        std::shared_ptr<Tone> kick, keys;
        double rate;
        int block;
        std::vector<float> l, r;

        explicit Rig(double sr = 48000.0, int blockSize = 256, Tone::Kind keysKind = Tone::Keys, bool kickHeard = false)
            : kick(std::make_shared<Tone>(Tone::Kick)), keys(std::make_shared<Tone>(keysKind)), rate(sr), block(blockSize),
              l((size_t) blockSize), r((size_t) blockSize)
        {
            audio.addEngine(0, kick);
            audio.addEngine(1, keys);
            audio.setTrackParameter(0, MixParam::VOLUME, kickHeard ? 0.8f : 0.0f);
            audio.setTrackParameter(1, MixParam::VOLUME, 0.8f);
        }
        void start() { audio.prepare(rate, block); }
        void chord() { for (int note : { 48, 55, 64 }) keys->noteOn(note, 0.9f, -1); }

        Stereo render(int blocks, const std::function<void(int)>& atBlock = {})
        {
            Stereo out;
            for (int b = 0; b < blocks; ++b)
            {
                if (atBlock) atBlock(b);
                audio.renderBlock(l.data(), r.data(), block);
                out.left.insert(out.left.end(), l.begin(), l.end());
                out.right.insert(out.right.end(), r.begin(), r.end());
            }
            return out;
        }
    };

    static bool sameSamples(const Stereo& a, const Stereo& b)
    {
        return a.left.size() == b.left.size()
            && std::memcmp(a.left.data(), b.left.data(), a.left.size() * sizeof(float)) == 0
            && std::memcmp(a.right.data(), b.right.data(), a.right.size() * sizeof(float)) == 0;
    }
    static bool finite(const Stereo& s, float bound = std::numeric_limits<float>::max())
    {
        for (size_t i = 0; i < s.left.size(); ++i)
            if (! (std::abs(s.left[i]) <= bound) || ! (std::abs(s.right[i]) <= bound)) return false;
        return true;
    }
    static double level(const Stereo& s, size_t from, size_t to) { return e5atlas::levelOf(s, from, to); }
    static double nullDb(const Stereo& a, const Stereo& b)
    {
        Stereo d;
        const size_t n = std::min(a.left.size(), b.left.size());
        d.left.resize(n); d.right.resize(n);
        for (size_t i = 0; i < n; ++i) { d.left[i] = a.left[i] - b.left[i]; d.right[i] = a.right[i] - b.right[i]; }
        return e5atlas::toDb(level(d, 0, n) / std::max(level(a, 0, n), 1.0e-9));
    }

    // How far the keys are pressed down while the drum strikes: their level in the 40 ms after
    // each strike against their level 150 to 230 ms after it (strikes are 250 ms apart).
    // (`played`: the samples the instrument had played before `s` began.)
    static double duckRatio(const Stereo& s, double rate, int firstStrike = 2, size_t played = 0)
    {
        const size_t period = (size_t) (rate * 0.25);
        double struck = 0.0, between = 0.0;
        int count = 0;
        for (size_t at = (size_t) firstStrike * period + (period - played % period) % period; at + period <= s.left.size(); at += period, ++count)
        {
            struck += level(s, at + (size_t) (rate * 0.005), at + (size_t) (rate * 0.045));
            between += level(s, at + (size_t) (rate * 0.15), at + (size_t) (rate * 0.23));
        }
        return count > 0 && between > 0.0 ? struck / between : 1.0;
    }

    // An effect that only notes what it is told.
    struct Probe : EffectBase
    {
        int source = KEY_SELF;
        float pitch = -1.0f;
        const float* inLeft = nullptr;
        int inFrames = -1;
        float firstKey = 0.0f;
        void reset() override {}
        void process(float&, float&) override {}
        void setPitch(float hz) override { pitch = hz; }
        int keySource() const override { return source; }
        void setInput(const float* left, const float*, int frames) override
        {
            inLeft = left;
            inFrames = frames;
            firstKey = left != nullptr && frames > 0 ? left[0] : 0.0f;
        }
    };

    void runTest() override
    {
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("NEPTUNE_E5_ATLAS", {}); folder.isNotEmpty())
        {
            beginTest("atlas renders");
            e5atlas::renderAll(juce::File(folder), juce::SystemStats::getEnvironmentVariable("NEPTUNE_E5_TAG", "new"), KEY_IN);
            return;
        }
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("NEPTUNE_E5_COMPARE", {}); folder.isNotEmpty())
        {
            beginTest("atlas comparison");
            e5atlas::compareAll(juce::File(folder));
            return;
        }
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_E5_BENCH", {}).isNotEmpty())
        {
            beginTest("bench");
            bench();
            return;
        }
        testMasterArithmetic();
        testMasterInterface();
        testMasterKeyed();
        testKeyChoice();
        testPerfKey();
        testChainMix();
        testChainMixLine();
        testChainBounds();
        testWiring();
        testReview();
    }

    // ---- 1. the master chain ----------------------------------------------------------------------
    // What the master section was: three members, a sample at a time (the arithmetic that is pinned).
    struct OldMaster
    {
        ParametricEQ eq;
        Compressor comp;
        Limiter limiter;
        OldMaster() { comp.setThreshold(0.0f); comp.setRatio(1.0f); }
        void prepare(double sr) { eq.setSampleRate(sr); comp.setSampleRate(sr); limiter.setSampleRate(sr); }
        void set(int index, float value)
        {
            switch (index)
            {
                case 0: case 1: case 2: case 3: eq.setParameter(index, value); break;
                case 4: comp.setThreshold(value); break;
                case 5: comp.setRatio(value); break;
                case 6: comp.setMakeupGain(value); break;
                case 7: limiter.setCeiling(value); break;
                default: break;
            }
        }
        void run(Stereo& s)
        {
            for (size_t i = 0; i < s.left.size(); ++i)
            {
                eq.process(s.left[i], s.right[i]);
                comp.process(s.left[i], s.right[i]);
                limiter.process(s.left[i], s.right[i]);
            }
        }
    };

    void testMasterArithmetic()
    {
        beginTest("the master chain as a new project has it is EQ, CMP at 1:1, LMT");
        {
            AudioEngine audio;
            EffectType type {};
            expect(audio.getInsertChain(M) != nullptr && audio.getInsertChain(M)->getEffectCount() == 3);
            expect(audio.getInsertEffectType(M, 0, type) && type == EffectType::EQEffect);
            expect(audio.getInsertEffectType(M, 1, type) && type == EffectType::CompressorEffect);
            expect(audio.getInsertEffectType(M, 2, type) && type == EffectType::LimiterEffect);
            expect(! audio.getInsertEffectType(M, 3, type));
            const float rest[8] = { 0, 0, 1000, 0, 0, 1, 0, -0.3f };
            for (int i = 0; i < AudioEngine::NUM_MASTER_PARAMS; ++i)
                expectEquals(audio.getMasterParam(i), rest[i], "MASTR value " + juce::String(i));
        }

        beginTest("by the block it gives the samples the three members gave a sample at a time, bit for bit (flat, and with every value moved)");
        {
            const float worked[8] = { 6.0f, -4.0f, 800.0f, 3.0f, -24.0f, 4.0f, 3.0f, -1.0f };
            for (int setting = 0; setting < 3; ++setting)
                for (auto shape : { std::pair<double, int> { 48000.0, 256 }, { 44100.0, 64 }, { 96000.0, 1 }, { 48000.0, 4096 } })
                {
                    const int blocks = std::max(4, (int) (0.6 * shape.first) / shape.second);
                    // setting 2: the values are moved under sound (the equaliser glides, the limiter's ceiling moves)
                    auto play = [&](bool chain, OldMaster* old)
                    {
                        Rig rig(shape.first, shape.second, Tone::Hot, true);
                        rig.audio.setMasterLevel(1.0f);
                        if (! chain)
                            while (rig.audio.getInsertChain(M)->getEffectCount() > 0)
                                rig.audio.getInsertChain(M)->removeEffect(0);       // (emptied without the 2 ms fade removeInsertEffect asks for)
                        auto put = [&](int i, float v) { if (chain) rig.audio.setMasterParam(i, v); else old->set(i, v); };
                        if (setting == 1)
                            for (int i = 0; i < 8; ++i) put(i, worked[i]);
                        rig.start();
                        if (old != nullptr) old->prepare(shape.first);
                        rig.chord();
                        Stereo out;
                        for (int b = 0; b < blocks; ++b)
                        {
                            if (setting == 2 && b == blocks / 3)
                                for (int i = 0; i < 8; ++i) put(i, worked[i]);
                            Stereo piece = rig.render(1);
                            if (old != nullptr) old->run(piece);
                            out.left.insert(out.left.end(), piece.left.begin(), piece.left.end());
                            out.right.insert(out.right.end(), piece.right.begin(), piece.right.end());
                        }
                        return out;
                    };
                    OldMaster old;
                    const Stereo before = play(false, &old), after = play(true, nullptr);
                    const juce::String what = "setting " + juce::String(setting) + ", " + juce::String(shape.first) + " Hz, block " + juce::String(shape.second);
                    expect(level(before, 0, before.left.size()) > 0.05, what + ": it sounds");
                    expect(sameSamples(before, after), what + ": the same samples (difference " + juce::String(nullDb(before, after)) + " dB)");
                    if (setting == 0)
                    {
                        // (the limiter is at work: the comparison is not of a wire with a wire)
                        Rig bare(shape.first, shape.second, Tone::Hot, true);
                        bare.audio.setMasterLevel(1.0f);
                        while (bare.audio.getInsertChain(M)->getEffectCount() > 0)
                            bare.audio.getInsertChain(M)->removeEffect(0);
                        bare.start();
                        bare.chord();
                        const Stereo raw = bare.render(blocks);
                        float peakRaw = 0.0f, peakAfter = 0.0f;
                        for (size_t i = 0; i < raw.left.size(); ++i) { peakRaw = std::max(peakRaw, std::abs(raw.left[i])); peakAfter = std::max(peakAfter, std::abs(after.left[i])); }
                        expect(peakRaw > 1.2f && peakAfter <= 0.967f, what + ": the limiter holds " + juce::String(peakRaw) + " to " + juce::String(peakAfter));
                    }
                }
        }
    }

    void testMasterInterface()
    {
        beginTest("the master chain answers to the insert functions and the insert ids of a track, and to nothing else of a track");
        {
            Rig rig(48000.0, 256, Tone::Keys, true);
            auto& audio = rig.audio;
            // the MASTR page's eight values are cells of the chain
            audio.setMasterParam(0, 5.0f);
            expectEquals(audio.getTrackParameter(M, CELL + 0), 5.0f);
            audio.setTrackParameter(M, CELL + 4 * 1 + 1, 3.0f);         // slot 2 (CMP), RATI
            expectEquals(audio.getMasterParam(5), 3.0f);
            audio.setTrackParameter(M, CELL + 4 * 2 + 0, -2.0f);        // slot 3 (LMT), CEIL
            expectEquals(audio.getMasterParam(7), -2.0f);
            audio.setTrackParameter(M, EXTRA + 4 * 1 + 1, 250.0f);      // CMP's FX2: REL
            expectEquals(audio.getTrackParameter(M, EXTRA + 4 * 1 + 1), 250.0f);
            expectEquals(static_cast<Compressor*> (audio.getMasterEffect(EffectType::CompressorEffect))->getRelease(), 250.0f);
            // ON and WET of a slot
            expectEquals(audio.getTrackParameter(M, ON + 2), 1.0f);
            audio.setTrackParameter(M, ON + 2, 0.0f);
            expect(! audio.getInsertChain(M)->getEffect(2)->isEnabled());
            audio.setTrackParameter(M, ON + 2, 1.0f);
            audio.setTrackParameter(M, WET + 0, 0.25f);
            expectEquals(audio.getTrackParameter(M, WET + 0), 0.25f);
            audio.setTrackParameter(M, WET + 0, 1.0f);
            // nothing else: a mixer id, a block id, an engine id do nothing and read 0
            for (int id : { (int) MixParam::VOLUME, (int) MixParam::PAN, (int) TrackFxParam::DIRT_BASE, (int) TrackInParam::SRC, 0, 5, 764 })
            {
                audio.setTrackParameter(M, id, 0.5f);
                expectEquals(audio.getTrackParameter(M, id), 0.0f, "id " + juce::String(id));
            }
            audio.setTrackParameter(M, CELL + 0, 0.0f);
            audio.setTrackParameter(M, CELL + 5, 1.0f);
            audio.setTrackParameter(M, CELL + 8, -0.3f);

            // any effect can sit on the mix: an echo after the limiter is heard after the notes stop
            rig.start();
            auto tail = [&]
            {
                rig.keys->noteOn(60, 0.9f, -1);
                rig.render(20);
                rig.keys->noteOff(60, -1);
                const Stereo s = rig.render(160);
                return level(s, s.left.size() / 2, s.left.size());
            };
            audio.setTrackParameter(0, MixParam::VOLUME, 0.0f);
            const double dryTail = tail();
            audio.addInsertEffect(M, EffectType::DelayEffect);
            EffectType type {};
            expect(audio.getInsertEffectType(M, 3, type) && type == EffectType::DelayEffect);
            audio.setTrackParameter(M, CELL + 4 * 3 + 0, 0.3f);         // TIME
            audio.setTrackParameter(M, CELL + 4 * 3 + 1, 0.7f);         // FDBK
            const double echoTail = tail();
            expect(dryTail < 0.005 && echoTail > 20.0 * std::max(dryTail, 1.0e-6), "an echo on the mix: " + juce::String(dryTail) + " -> " + juce::String(echoTail));
            // exchanged, taken out, and the chain holds as many as a track's
            audio.setInsertEffect(M, 3, EffectType::ChorusEffect);
            expect(audio.getInsertEffectType(M, 3, type) && type == EffectType::ChorusEffect);
            audio.removeInsertEffect(M, 3);
            expect(audio.getInsertChain(M)->getEffectCount() == 3);
            for (int i = 0; i < 12; ++i) audio.addInsertEffect(M, EffectType::TremoloEffect);
            expectEquals(audio.getInsertChain(M)->getEffectCount(), (int) AudioEngine::MAX_INSERTS_PER_TRACK);
            expect(finite(rig.render(8), 4.0f));
            // an emptied chain is a wire; the MASTR values then read as at rest and set nothing
            while (audio.getInsertChain(M)->getEffectCount() > 0) audio.removeInsertEffect(M, 0);
            audio.setMasterParam(4, -30.0f);
            expectEquals(audio.getMasterParam(4), 0.0f);
            expectEquals(audio.getMasterParam(7), -0.3f);
            expect(finite(rig.render(8), 4.0f));
            audio.resetMasterChain();
            expect(audio.getInsertChain(M)->getEffectCount() == 3 && audio.getMasterSlot(EffectType::LimiterEffect) == 2);
            expectEquals(audio.getNonFiniteBlocks(), 0);
        }

        beginTest("Shift + Play empties the master chain's memory too (an echo on the mix does not ring on)");
        {
            Rig rig;
            rig.audio.addInsertEffect(M, EffectType::DelayEffect);
            rig.audio.setTrackParameter(M, CELL + 4 * 3 + 0, 0.3f);
            rig.audio.setTrackParameter(M, CELL + 4 * 3 + 1, 0.9f);
            rig.start();
            rig.keys->noteOn(60, 0.9f, -1);
            rig.render(40);
            rig.audio.requestSilence();
            rig.render((int) (0.3 * 48000 / 256));
            const Stereo after = rig.render(200);
            expect(level(after, 0, after.left.size()) < 1.0e-5, "silence stays: " + juce::String(level(after, 0, after.left.size())));
        }
    }

    void testMasterKeyed()
    {
        beginTest("the mix ducked by a kick: DUCK on the master chain keyed from track 1, and CMP with KEY at T1");
        {
            auto play = [&](int which, bool kickMuted = false)
            {
                Rig rig;
                SequencerCore seq;
                if (kickMuted)
                {
                    rig.audio.setSequencer(&seq);
                    seq.getPattern().getTrack(0).muted = true;
                }
                if (which == 1)
                {
                    rig.audio.addInsertEffect(M, EffectType::SidechainEffect);
                    rig.audio.setTrackParameter(M, CELL + 4 * 3 + 0, 1.0f);         // SRC T1
                    rig.audio.setTrackParameter(M, CELL + 4 * 3 + 1, -30.0f);
                    rig.audio.setTrackParameter(M, CELL + 4 * 3 + 2, 10.0f);
                    rig.audio.setTrackParameter(M, CELL + 4 * 3 + 3, 40.0f);        // REL
                }
                if (which == 2)
                {
                    // the master's own compressor, keyed
                    rig.audio.setMasterParam(4, -30.0f);
                    rig.audio.setMasterParam(5, 10.0f);
                    rig.audio.setTrackParameter(M, EXTRA + 4 * 1 + 0, 1.0f);        // KEY T1
                    rig.audio.setTrackParameter(M, EXTRA + 4 * 1 + 1, 40.0f);       // REL
                }
                rig.start();
                rig.chord();
                const Stereo s = rig.render((int) (2.0 * 48000 / 256));
                if (kickMuted)
                    expect(seq.trackPlays(0), "a muted track the master chain listens to plays on (its fader is shut instead)");
                return duckRatio(s, 48000.0);
            };
            const double plain = play(0), duck = play(1), comp = play(2);
            expect(plain > 0.9 && plain < 1.1, "no keyed effect: the keys stand, " + juce::String(plain));
            expect(duck < 0.5, "DUCK SRC T1 on the master chain: the mix goes down under each strike, to " + juce::String(duck));
            expect(comp < 0.5, "the master compressor with KEY at T1: to " + juce::String(comp));
            expect(play(1, true) < 0.5, "and a muted kick goes on ducking");
        }

        beginTest("a send's effect is handed the tracks' sounds too: a compressor as the send's effect, keyed from track 1, pumps what is sent to it");
        {
            auto play = [&](float key)
            {
                Rig rig;
                rig.audio.setInsertEffect(AudioEngine::sendTrack(0), 0, EffectType::CompressorEffect);
                auto* fx = rig.audio.getSendBus(0).getChain().getEffect(0);
                fx->setParameter(0, -30.0f);
                fx->setParameter(1, 10.0f);
                fx->setExtra(1, 40.0f);
                fx->setExtra(0, key);
                rig.audio.setTrackParameter(1, MixParam::SEND_BASE, 1.0f);
                rig.start();
                rig.chord();
                return duckRatio(rig.render((int) (2.0 * 48000 / 256)), 48000.0);
            };
            const double self = play(0.0f), keyed = play(1.0f);
            expect(self > 0.9 && self < 1.1 && keyed < 0.85, "the keys and what the send returns of them: SELF " + juce::String(self) + ", T1 " + juce::String(keyed));
        }
    }

    // ---- 3. the key choice ------------------------------------------------------------------------
    void testKeyChoice()
    {
        beginTest("KeyFeed: an effect is handed the sound its keySource() names, for the stretch it is run on");
        {
            std::vector<float> in(64), t0(64), t1(64);
            for (int i = 0; i < 64; ++i) { in[(size_t) i] = 100.0f + (float) i; t0[(size_t) i] = 200.0f + (float) i; t1[(size_t) i] = 300.0f + (float) i; }
            const float* taps[2] = { t0.data(), t1.data() };
            KeyFeed feed;
            feed.inLeft = feed.inRight = in.data();
            feed.inFrames = 64;
            feed.tapLeft = feed.tapRight = taps;
            feed.taps = 2;
            feed.frames = 64;
            feed.self = 1;
            Probe p;
            feed.hand(p);
            expectEquals(p.inFrames, -1, "SELF: nothing is handed over");
            p.source = EffectBase::KEY_IN;
            feed.hand(p);
            expect(p.inFrames == 64 && p.firstKey == 100.0f);
            feed.hand(p, 10, 20);
            expect(p.inFrames == 20 && p.firstKey == 110.0f, "a stretch of the block");
            feed.hand(p, 60, 20);
            expect(p.inFrames == 4 && p.firstKey == 160.0f, "no further than the key goes");
            feed.hand(p, 64, 20);
            expect(p.inLeft == nullptr && p.inFrames == 0);
            p.source = 0;
            feed.hand(p, 3);
            expect(p.inFrames == 61 && p.firstKey == 203.0f, "a track by number");
            p.source = 1;
            feed.hand(p);
            expect(p.inLeft == nullptr && p.inFrames == 0, "its own track: none");
            p.source = 2;
            feed.hand(p);
            expect(p.inLeft == nullptr, "a track that is not there: none");
            KeyFeed none;
            p.source = EffectBase::KEY_IN;
            none.hand(p);
            expect(p.inLeft == nullptr && p.inFrames == 0, "no input: told so");
        }

        beginTest("on a track: CMP, TRNS and WAH take a track by number as DUCK does; IN is the track's input; the track itself is SELF");
        {
            // which: 0 CMP, 1 DUCK. key: the cell's value.
            auto play = [&](int which, float key, bool input, Stereo* sound = nullptr)
            {
                Rig rig;
                if (input) rig.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
                rig.audio.addInsertEffect(1, which == 0 ? EffectType::CompressorEffect : EffectType::SidechainEffect);
                rig.audio.setTrackParameter(1, CELL + (which == 0 ? 0 : 1), -30.0f);
                rig.audio.setTrackParameter(1, CELL + (which == 0 ? 1 : 2), 10.0f);
                rig.audio.setTrackParameter(1, which == 0 ? EXTRA + 1 : CELL + 3, 40.0f);     // REL
                rig.audio.setTrackParameter(1, which == 0 ? EXTRA + 0 : CELL + 0, key);
                rig.start();
                rig.chord();
                const Stereo s = rig.render((int) (2.0 * 48000 / 256));
                if (sound != nullptr) *sound = s;
                expectEquals(rig.audio.getNonFiniteBlocks(), 0);
                return duckRatio(s, 48000.0);
            };
            for (int which = 0; which < 2; ++which)
            {
                const juce::String name = which == 0 ? "CMP" : "DUCK";
                Stereo self, own, absent;
                const double selfRatio = play(which, 0.0f, false, &self);
                expect(selfRatio > 0.9 && selfRatio < 1.1, name + " SELF: no pumping, " + juce::String(selfRatio));
                expect(play(which, 1.0f, false) < 0.5, name + " at T1: down under each strike");
                expect(play(which, KEY_IN, true) < 0.5, name + " at IN, the input being track 1: down under each strike");
                const double noInput = play(which, KEY_IN, false);
                expect(noInput > 0.9 && noInput < 1.1, name + " at IN with no input: as SELF");
                play(which, 2.0f, false, &own);
                expect(sameSamples(own, self), name + " at T2 on track 2: SELF, sample for sample");
                const double silentKey = play(which, 5.0f, false, &absent);
                expect(silentKey > 0.9 && silentKey < 1.1 && level(absent, 48000, 96000) > 1.5 * level(self, 48000, 96000),
                       name + " at T5, a track with nothing on it: nothing presses (" + juce::String(level(absent, 48000, 96000)) + " against "
                       + juce::String(level(self, 48000, 96000)) + " compressing itself)");
            }
            // WAH and TRNS: the key cell is found by its list, wherever the effect has it
            Rig rig;
            rig.audio.addInsertEffect(1, EffectType::WahEffect);
            rig.audio.addInsertEffect(1, EffectType::TransientEffect);
            rig.audio.setTrackParameter(1, EXTRA + Wah::KEY_EXTRA, 1.0f);
            rig.audio.setTrackParameter(1, EXTRA + 4 + TransientShaper::KEY_EXTRA, 16.0f);
            expectEquals(rig.audio.getInsertChain(1)->getEffect(0)->keySource(), 0);
            expectEquals(rig.audio.getInsertChain(1)->getEffect(1)->keySource(), 15);
            rig.audio.setTrackParameter(1, EXTRA + Wah::KEY_EXTRA, KEY_IN);
            expectEquals(rig.audio.getInsertChain(1)->getEffect(0)->keySource(), (int) EffectBase::KEY_IN);
            expect(rig.audio.getInsertChain(1)->getEffect(0)->wantsInput());
        }

        beginTest("an effect whose slot is switched off is not left holding a key (the pointer of an old block is never read)");
        {
            Rig rig;
            rig.audio.addInsertEffect(1, EffectType::SidechainEffect);
            rig.audio.setTrackParameter(1, CELL + 0, 1.0f);
            rig.audio.setTrackParameter(1, CELL + 1, -30.0f);
            rig.audio.setTrackParameter(1, CELL + 2, 10.0f);
            rig.start();
            rig.chord();
            rig.render(40);
            rig.audio.setTrackParameter(1, ON + 0, 0.0f);
            rig.render(40);
            rig.audio.setTrackParameter(1, ON + 0, 1.0f);
            const Stereo s = rig.render((int) (1.5 * 48000 / 256));
            expect(finite(s, 2.0f) && duckRatio(s, 48000.0, 1, 80 * 256) < 0.6, "it ducks again: " + juce::String(duckRatio(s, 48000.0, 1, 80 * 256)));
            expectEquals(rig.audio.getNonFiniteBlocks(), 0);
        }
    }

    void testPerfKey()
    {
        beginTest("PERF: a keyed effect on a pad listens to what its KEY cell says (a track by number; IN = the input of the track it acts on)");
        {
            // where: 0 = the pad acts on the whole mix, 1 = on track 2. key: the cell's value.
            auto play = [&](int where, float key, bool pressed, bool input)
            {
                Rig rig;
                if (input) rig.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
                auto& perf = rig.audio.getPerfFx();
                perf.clear(0);
                perf.setKind(0, 0, PerfFx::INSERT_BASE + (int) EffectType::CompressorEffect);
                PerfFx::Setting s = perf.get(0);
                s.layer[0].target = where == 0 ? (int) PerfFx::MASTER : PerfFx::TRACK_BASE + 1;
                s.layer[0].cell = { -30.0f, 10.0f, 1.0f, 0.0f };
                s.layer[0].extra = { key, 40.0f, 3.0f, 20.0f };
                s.layer[0].press = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                s.layer[0].pressure = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                perf.set(0, s);
                rig.start();
                rig.chord();
                rig.render(20);
                if (pressed) perf.press(0, 1);
                const Stereo out = rig.render((int) (2.0 * 48000 / 256));
                expectEquals(rig.audio.getNonFiniteBlocks(), 0);
                return duckRatio(out, 48000.0, 2, 20 * 256);
            };
            const double up = play(0, 1.0f, false, false);
            expect(up > 0.9 && up < 1.1, "the pad up: the keys stand, " + juce::String(up));
            expect(play(0, 1.0f, true, false) < 0.5, "on the mix, KEY T1: pressed, the mix goes down under each strike");
            expect(play(1, 1.0f, true, false) < 0.5, "on track 2, KEY T1");
            expect(play(1, KEY_IN, true, true) < 0.5, "on track 2, KEY IN, the track's input being track 1");
            const double self = play(1, 0.0f, true, false);
            expect(self > 0.9 && self < 1.1, "KEY SELF: pressed down evenly, no pumping, " + juce::String(self));
            const double mixIn = play(0, KEY_IN, true, true);
            expect(mixIn > 0.9 && mixIn < 1.1, "on the mix KEY IN is SELF (the mix has no input), " + juce::String(mixIn));
        }
    }

    // ---- 2. the chain's mix -----------------------------------------------------------------------
    static constexpr int N = 24000;
    static Stereo testSound(int n = N, uint32_t seed = 3)
    {
        Stereo s;
        s.left.resize((size_t) n); s.right.resize((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const float noise = 0.1f * (float) (int32_t) seed * (1.0f / 2147483648.0f);
            const float gate = (i % 6000) < 3000 ? 1.0f : 0.15f;
            s.left[(size_t) i] = gate * (0.6f * (float) std::sin(6.283185307 * 110.0 * i / 48000.0) + 0.3f * (float) std::sin(6.283185307 * 1730.0 * i / 48000.0)) + noise;
            s.right[(size_t) i] = gate * (0.6f * (float) std::sin(6.283185307 * 165.0 * i / 48000.0)) - noise;
        }
        return s;
    }

    // An effect of a type set to work (flat equalisers and an idle limiter would say nothing).
    static std::unique_ptr<EffectBase> working(EffectType t)
    {
        auto fx = AudioEngine::createEffect(t);
        if (t == EffectType::EQEffect)        { fx->setParameter(0, 9.0f); fx->setParameter(3, -9.0f); }
        if (t == EffectType::GraphicEQEffect) { fx->setParameter(0, 9.0f); fx->setParameter(2, -9.0f); }
        if (t == EffectType::LimiterEffect)   fx->setParameter(0, -3.0f);
        if (t == EffectType::CompressorEffect) { fx->setParameter(0, -30.0f); fx->setParameter(1, 8.0f); }
        if (t == EffectType::SidechainEffect)  { fx->setParameter(1, -30.0f); fx->setParameter(2, 8.0f); }
        if (t == EffectType::TransientEffect)  { fx->setParameter(0, 1.0f); fx->setParameter(1, -1.0f); }
        if (t == EffectType::DirtInsertEffect) fx->setParameter(DirtInsert::Drive, 0.8f);
        if (t == EffectType::TalkInsertEffect) { fx->setParameter(TalkInsert::Reso, 0.8f); fx->setParameter(TalkInsert::LfoRate, 2.0f); }
        return fx;
    }

    static Stereo through(EffectChain& chain, const Stereo& in, int block, const std::function<void(int)>& atBlock = {})
    {
        Stereo out = in;
        for (int done = 0, b = 0; done < (int) in.left.size(); done += block, ++b)
        {
            if (atBlock) atBlock(b);
            chain.processStereo(out.left.data() + done, out.right.data() + done, std::min(block, (int) in.left.size() - done));
        }
        return out;
    }
    static Stereo through(EffectBase& fx, const Stereo& in, int block)
    {
        Stereo out = in;
        for (int done = 0; done < (int) in.left.size(); done += block)
            fx.processStereo(out.left.data() + done, out.right.data() + done, std::min(block, (int) in.left.size() - done));
        return out;
    }

    static bool mixesByItself(EffectType t)
    {
        return t == EffectType::DelayEffect || t == EffectType::ReverbEffect || t == EffectType::HallReverbEffect
            // FLG no longer mixes by itself: the chain handles its through-zero dry delay.
            || t == EffectType::DriveEffect || t == EffectType::WaveshaperEffect
            || t == EffectType::RateReducerEffect || t == EffectType::VocoderEffect;
    }

    void testChainMix()
    {
        const Stereo in = testSound();

        beginTest("one mix, in the chain: every effect at WET 0.5 is the dry sound and its wet sound in halves; at WET 1 the chain does nothing; at 0 the dry sound comes out");
        for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
        {
            const EffectType type = (EffectType) t;
            const juce::String name = AudioEngine::effectTypeName(type);
            auto chained = [&](float wet)
            {
                EffectChain chain;
                chain.prepare(48000.0, 256);
                auto fx = working(type);
                fx->setMix(wet);
                chain.addEffect(std::move(fx));
                return through(chain, in, 256);
            };
            auto alone = [&](float wet)
            {
                auto fx = working(type);
                fx->prepare(48000.0, 256);
                fx->setMix(wet);
                return through(*fx, in, 256);
            };
            const Stereo half = chained(0.5f), full = chained(1.0f);
            expect(finite(half, 8.0f) && finite(full, 8.0f), name);
            if (mixesByItself(type))
            {
                // these keep their own arithmetic (three of them are pinned): the chain adds nothing
                expect(sameSamples(half, alone(0.5f)) && sameSamples(full, alone(1.0f)), name + ": as it mixes by itself, bit for bit");
                continue;
            }
            expect(AudioEngine::createEffect(type)->givesWetOnly() == false, name + ": outside a chain an effect mixes by itself");
            // WET 1: what the effect gives alone at mix 1, bit for bit (AMP, EQ, GEQ, LMT have no mix: at any)
            expect(sameSamples(full, alone(1.0f)), name + " at WET 1: the effect itself, bit for bit");
            // WET 0.5: dry + 0.5 (wet - dry), with the wet sound of WET 1; the dry sound as late as
            // the effect's own (LMT's lookahead), so that the two are in step
            int late = 0;
            {
                auto fx = working(type);
                fx->prepare(48000.0, 256);
                late = fx->latencySamples();
            }
            expect(type == EffectType::LimiterEffect ? late == 144 : late == 0, name + ": late by " + juce::String(late));
            Stereo dry = in;
            for (size_t i = 0; i < dry.left.size(); ++i)
            {
                dry.left[i] = i >= (size_t) late ? in.left[i - (size_t) late] : 0.0f;
                dry.right[i] = i >= (size_t) late ? in.right[i - (size_t) late] : 0.0f;
            }
            Stereo want = dry;
            for (size_t i = 0; i < want.left.size(); ++i)
            {
                want.left[i] = dry.left[i] + 0.5f * (full.left[i] - dry.left[i]);
                want.right[i] = dry.right[i] + 0.5f * (full.right[i] - dry.right[i]);
            }
            expect(nullDb(want, half) < -100.0, name + " at WET 0.5: the halves, " + juce::String(nullDb(want, half)) + " dB off");
            expect(nullDb(dry, full) > -60.0 || type == EffectType::SidechainEffect, name + ": it does something (" + juce::String(nullDb(dry, full)) + " dB)");
            const Stereo none = chained(0.0f);
            expect(sameSamples(none, dry), name + " at WET 0: the dry sound, bit for bit");
        }

        beginTest("the chain's mix is what the effect's own mix was: an effect of cells in a chain against the same effect mixing by itself, at WET 0.5");
        {
            // (the effects whose mix was already a plain dry / wet: every one of cells but the four
            // that had none. CMP, DUCK, TRNS as gains, the rest as sounds.)
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
            {
                const EffectType type = (EffectType) t;
                if (mixesByItself(type) || type == EffectType::EQEffect || type == EffectType::GraphicEQEffect
                    || type == EffectType::LimiterEffect || type == EffectType::AmpEffect)
                    continue;
                EffectChain chain;
                chain.prepare(48000.0, 256);
                auto a = working(type);
                a->setMix(0.5f);
                chain.addEffect(std::move(a));
                auto b = working(type);
                b->prepare(48000.0, 256);
                b->setMix(0.5f);
                const Stereo inChain = through(chain, in, 256), byItself = through(*b, in, 256);
                const double off = nullDb(byItself, inChain);
                expect(off < -90.0, juce::String(AudioEngine::effectTypeName(type)) + ": " + juce::String(off) + " dB off what it was");
            }
        }

        beginTest("LMT's WET: the dry sound is mixed as late as the limited one (LOOK), so an idle limiter at any WET is a delay and not a comb; a WET moved under sound waits for the line");
        {
            Stereo quiet = in;
            for (auto* ch : { &quiet.left, &quiet.right }) for (auto& x : *ch) x *= 0.3f;       // (below the ceiling: the limiter does nothing)
            for (float lookMs : { 1.0f, 3.0f, 5.0f })
            {
                EffectChain chain;
                chain.prepare(48000.0, 256);
                auto limiter = std::make_unique<Limiter>();
                limiter->setLookahead(lookMs);
                limiter->setMix(0.5f);
                chain.addEffect(std::move(limiter));
                const int late = (int) (lookMs * 48.0f);
                Stereo delayed = quiet;
                for (size_t i = 0; i < delayed.left.size(); ++i)
                {
                    delayed.left[i] = i >= (size_t) late ? quiet.left[i - (size_t) late] : 0.0f;
                    delayed.right[i] = i >= (size_t) late ? quiet.right[i - (size_t) late] : 0.0f;
                }
                expect(nullDb(delayed, through(chain, quiet, 256)) < -120.0, "LOOK " + juce::String(lookMs) + " ms, WET 0.5 from the start");
                // WET at the top for a while (the line is not written), then thrown down: still the delay, never a comb
                EffectChain moved;
                moved.prepare(48000.0, 64);
                auto second = std::make_unique<Limiter>();
                second->setLookahead(lookMs);
                moved.addEffect(std::move(second));
                const Stereo out = through(moved, quiet, 64, [&](int b)
                {
                    if (b == 100) moved.getEffect(0)->setMix(0.2f);
                    if (b == 200) moved.getEffect(0)->setMix(1.0f);
                    if (b == 300) moved.getEffect(0)->setMix(0.0f);
                });
                expect(nullDb(delayed, out) < -120.0, "LOOK " + juce::String(lookMs) + " ms, WET moved under sound: " + juce::String(nullDb(delayed, out)) + " dB off the delayed sound");
                expectEquals(moved.getMixNow(0), 0.0f);
            }
        }

        beginTest("the mix in the three routings, and in a chain that was not prepared for the block (in pieces)");
        {
            for (auto routing : { EffectChain::Routing::Series, EffectChain::Routing::Parallel, EffectChain::Routing::Mixed })
            {
                auto make = [&](float wet, bool prepared)
                {
                    auto chain = std::make_unique<EffectChain>();
                    if (prepared) chain->prepare(48000.0, 4096);
                    chain->setRouting(routing);
                    auto fx = working(EffectType::EQEffect);
                    if (! prepared) fx->prepare(48000.0, 0);
                    fx->setMix(wet);
                    chain->addEffect(std::move(fx));
                    return chain;
                };
                const Stereo full = through(*make(1.0f, true), in, 512), half = through(*make(0.5f, true), in, 512);
                Stereo want = in;
                for (size_t i = 0; i < want.left.size(); ++i)
                {
                    want.left[i] = in.left[i] + 0.5f * (full.left[i] - in.left[i]);
                    want.right[i] = in.right[i] + 0.5f * (full.right[i] - in.right[i]);
                }
                expect(nullDb(want, half) < -100.0, "routing " + juce::String((int) routing) + ": " + juce::String(nullDb(want, half)) + " dB");
                if (routing == EffectChain::Routing::Series)
                    expect(nullDb(half, through(*make(0.5f, false), in, 1000)) < -100.0, "not prepared, blocks of 1000: the same sound");
            }
        }

        beginTest("modulateCell(WET) moves the chain's mix, and the cell reads as it was set");
        {
            EffectChain chain;
            chain.prepare(48000.0, 256);
            chain.addEffect(working(EffectType::EQEffect));
            auto* fx = chain.getEffect(0);
            fx->modulateCell(EffectBase::WET_CELL, -0.5f);
            const Stereo out = through(chain, in, 256);
            expectEquals(fx->getMix(), 1.0f);
            expectWithinAbsoluteError(chain.getMixNow(0), 0.5f, 1.0e-6f);
            fx->modulateCell(EffectBase::WET_CELL, 0.0f);
            through(chain, in, 256);
            expectEquals(chain.getMixNow(0), 1.0f);
            juce::ignoreUnused(out);
        }
    }

    void testChainMixLine()
    {
        beginTest("a WET that is thrown is gone to in a straight line of 4 ms: no step in the sound");
        for (double rate : { 48000.0, 96000.0 })
        {
            const int n = (int) rate;
            Stereo in;
            in.left.resize((size_t) n); in.right.resize((size_t) n);
            for (int i = 0; i < n; ++i)
                in.left[(size_t) i] = in.right[(size_t) i] = 0.5f * (float) std::sin(6.283185307 * 100.0 * i / rate);
            EffectChain chain;
            chain.prepare(rate, 64);
            auto eq = std::make_unique<ParametricEQ>();
            eq->setLowShelf(400.0f, 15.0f);
            chain.addEffect(std::move(eq));
            const int swing = (int) (0.004 * rate);
            const int throwAt = n / 2 / 64;
            float onTheWay = -1.0f;
            const Stereo out = through(chain, in, 64, [&](int b)
            {
                if (b == throwAt) chain.getEffect(0)->setMix(0.0f);
                if (b == throwAt + 2) onTheWay = chain.getMixNow(0);        // (128 samples after the throw)
            });
            expectWithinAbsoluteError(onTheWay, 1.0f - 128.0f / (float) swing, 0.01f, "128 samples of a 4 ms line");
            expectEquals(chain.getMixNow(0), 0.0f);
            // the largest step from one sample to the next, while it moves, against the boosted
            // tone's own largest step (a cut from +15 dB to the dry sound at the tone's peak would
            // be a step of about 2)
            float steady = 0.0f, moving = 0.0f;
            for (int i = n / 4; i < n / 2 - 64; ++i) steady = std::max(steady, std::abs(out.left[(size_t) i] - out.left[(size_t) i - 1]));
            for (int i = throwAt * 64; i < throwAt * 64 + 2 * swing; ++i) moving = std::max(moving, std::abs(out.left[(size_t) i] - out.left[(size_t) i - 1]));
            expect(moving < 1.5f * steady, juce::String(rate) + " Hz: largest step " + juce::String(moving) + " against the tone's own " + juce::String(steady));
            // and back up
            chain.getEffect(0)->setMix(1.0f);
            through(chain, in, 64);
            expectEquals(chain.getMixNow(0), 1.0f);
            // emptied, nothing sounding: no line to draw
            chain.getEffect(0)->setMix(0.3f);
            chain.reset();
            float atOnce = -1.0f;
            through(chain, in, 64, [&](int b) { if (b == 1) atOnce = chain.getMixNow(0); });
            expectEquals(atOnce, 0.3f, "after the first block it is where it was set (no line from where it stood)");
            expectEquals(chain.getMixNow(0), 0.3f);
        }
    }

    void testChainBounds()
    {
        beginTest("whatever comes in (not a number, infinite, 1e30), an effect that the chain mixes gives a bounded sound, at WET 0.5 too: 22.05 to 192 kHz, blocks of 1 to 4096");
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
        {
            const EffectType type = (EffectType) t;
            if (mixesByItself(type)) continue;      // (as they were: the AudioEngine's guard is in front of them)
            for (auto shape : { std::pair<double, int> { 22050.0, 1 }, { 48000.0, 64 }, { 192000.0, 4096 } })
                for (float wet : { 0.5f, 1.0f })
                {
                    // (AMP is not an effect of cells: its own sound has no guard, as before; what
                    // the chain mixes of it has)
                    if (type == EffectType::AmpEffect) continue;
                    EffectChain chain;
                    chain.prepare(shape.first, shape.second);
                    auto fx = working(type);
                    fx->setMix(wet);
                    chain.addEffect(std::move(fx));
                    const int n = shape.second == 1 ? 3000 : 3 * 4096;
                    Stereo in = testSound(n);
                    for (int i = 0; i < n; ++i)
                    {
                        if (i % 997 == 5) in.left[(size_t) i] = nan;
                        if (i % 1013 == 7) in.right[(size_t) i] = inf;
                        if (i % 1019 == 9) in.left[(size_t) i] = -inf;
                        if (i % 1021 == 11) in.right[(size_t) i] = 1.0e30f;
                    }
                    for (int i = n / 2; i < n; ++i) in.left[(size_t) i] = in.right[(size_t) i] = 0.0f;
                    const Stereo out = through(chain, in, shape.second);
                    expect(finite(out, 2000.0f), juce::String(AudioEngine::effectTypeName(type)) + " at " + juce::String(shape.first) + " Hz, block "
                                                 + juce::String(shape.second) + ", WET " + juce::String(wet));
                }
        }
    }

    // ---- 4. wiring ----------------------------------------------------------------------------------
    void testWiring()
    {
        beginTest("the note a track plays reaches its inserts as a pitch (EngineBase::getLastNoteHz -> EffectBase::setPitch); the master chain is told none");
        {
            Rig rig;
            rig.audio.addInsertEffect(1, EffectType::TremoloEffect);
            rig.audio.getInsertChain(1)->replaceEffect(0, std::make_unique<Probe>());
            rig.audio.addInsertEffect(M, EffectType::TremoloEffect);
            rig.audio.getInsertChain(M)->replaceEffect(3, std::make_unique<Probe>());
            auto* onTrack = static_cast<Probe*> (rig.audio.getInsertChain(1)->getEffect(0));
            auto* onMaster = static_cast<Probe*> (rig.audio.getInsertChain(M)->getEffect(3));
            // and the vocoder in its mode TUNE, whose body is tuned to that pitch
            rig.audio.addInsertEffect(1, EffectType::VocoderEffect);
            rig.audio.setTrackParameter(1, CELL + 4 * 1 + 0, 1.0f);
            auto* vocoder = static_cast<Vocoder*> (rig.audio.getInsertChain(1)->getEffect(1));
            rig.start();
            rig.render(2);
            expectEquals(onTrack->pitch, 0.0f, "no note yet");
            expectWithinAbsoluteError(vocoder->tunedHz(), 130.81f, 0.5f, "VOC TUNE with no note: C3");
            expectEquals(rig.keys->getLastNoteHz(), 0.0f);
            rig.audio.padNoteOn(1, 0, 69, 0.8f);
            rig.audio.processGenerators(256);
            rig.render(2);
            expectWithinAbsoluteError(onTrack->pitch, 440.0f, 0.01f, "a pad's note");
            rig.render(40);
            expectWithinAbsoluteError(vocoder->tunedHz(), 440.0f, 2.0f, "VOC TUNE follows the played note");
            rig.keys->notePlayed(57.0f);        // (what an engine does in its noteOn)
            rig.render(1);
            expectWithinAbsoluteError(onTrack->pitch, 220.0f, 0.01f);
            rig.keys->notePlayed(57.5f);
            expectWithinAbsoluteError(rig.keys->getLastNoteHz(), 220.0f * std::pow(2.0f, 0.5f / 12.0f), 0.01f, "between two keys (a glide, a bend)");
            rig.keys->notePlayed(std::numeric_limits<float>::quiet_NaN());
            rig.keys->notePlayed(1.0e9f);
            expect(std::isfinite(rig.keys->getLastNoteHz()) && rig.keys->getLastNoteHz() < 100000.0f);
            expectEquals(onMaster->pitch, 0.0f, "the mix plays no note");
            rig.audio.padNoteOff(0);
        }

        beginTest("the track's custom LFO shape reaches the effects on it: an insert's USR shape plays the track's eight levels, and another engine's when the engine changes");
        {
            int usr = -1;
            {
                Tremolo probe;
                const ParamSpec shape = probe.getParamSpec(Tremolo::Shape);
                for (int i = 0; i < shape.numChoices; ++i)
                    if (juce::String(shape.choices[i]) == "USR") usr = i;
            }
            expect(usr >= 0);
            // The tremolo at full depth with the custom shape: how far the level moves (loudest
            // against quietest 20 ms window of a held chord).
            auto swing = [&](Rig& rig)
            {
                const Stereo s = rig.render((int) (1.0 * 48000 / 256));
                double lo = 1.0e9, hi = 0.0;
                for (size_t at = 4800; at + 960 <= s.left.size(); at += 960)
                {
                    const double v = level(s, at, at + 960);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                return hi / std::max(lo, 1.0e-9);
            };
            Rig rig;
            rig.keys->withShape = true;
            rig.audio.addEngine(1, rig.keys);               // (the engine is taken again, now that it has the shape)
            rig.audio.addInsertEffect(1, EffectType::TremoloEffect);
            rig.audio.setTrackParameter(1, CELL + Tremolo::Rate, 5.0f);
            rig.audio.setTrackParameter(1, CELL + Tremolo::Depth, 1.0f);
            rig.audio.setTrackParameter(1, CELL + Tremolo::Shape, (float) usr);
            for (int i = 0; i < 8; ++i)
                rig.audio.setTrackParameter(1, LfoShapeParam::BASE + i, 1.0f);      // every level at the top: the tremolo stands open
            rig.start();
            rig.chord();
            const double flat = swing(rig);
            expect(flat < 1.5, "eight equal levels: the level stands, x " + juce::String(flat));
            for (int i = 0; i < 8; ++i)
                rig.audio.setTrackParameter(1, LfoShapeParam::BASE + i, i % 2 == 0 ? 1.0f : -1.0f);
            const double stepped = swing(rig);
            expect(stepped > 5.0, "levels up and down: the level follows them, x " + juce::String(stepped));
            // another engine on the track (one with the shape at rest, all levels equal): the same insert plays that one
            auto other = std::make_shared<Tone>(Tone::Keys);
            other->withShape = true;
            other->shape.fill(0.5f);
            rig.audio.addEngine(1, other);
            for (int note : { 48, 55, 64 }) other->noteOn(note, 0.9f, -1);
            const double changed = swing(rig);
            expect(changed < 1.5, "the new engine's shape: x " + juce::String(changed));
            // an engine without the shape: the rising staircase (the level moves)
            auto bare = std::make_shared<Tone>(Tone::Keys);
            rig.audio.addEngine(1, bare);
            for (int note : { 48, 55, 64 }) bare->noteOn(note, 0.9f, -1);
            const double staircase = swing(rig);
            expect(staircase > 3.0, "no shape of its own: the staircase, x " + juce::String(staircase));
        }

        beginTest("the shape is taken anew from the engine when the track's engine type is changed, set aside for an audition, and brought back");
        {
            int usr = -1;
            {
                Tremolo probe;
                const ParamSpec shape = probe.getParamSpec(Tremolo::Shape);
                for (int i = 0; i < shape.numChoices; ++i)
                    if (juce::String(shape.choices[i]) == "USR") usr = i;
            }
            Rig rig;
            rig.audio.setEngineType(1, EngineType::UnifiedVoice, true);     // (an engine with the LSHP page)
            rig.audio.addInsertEffect(1, EffectType::TremoloEffect);
            rig.audio.setTrackParameter(1, CELL + Tremolo::Rate, 5.0f);
            rig.audio.setTrackParameter(1, CELL + Tremolo::Depth, 1.0f);
            rig.audio.setTrackParameter(1, CELL + Tremolo::Shape, (float) usr);
            for (int i = 0; i < 8; ++i)
                rig.audio.setTrackParameter(1, LfoShapeParam::BASE + i, 1.0f);
            rig.start();
            auto swing = [&]()
            {
                for (int note : { 48, 55, 64 }) rig.audio.getEngine(1)->noteOn(note, 0.9f, -1);
                rig.render(40);
                const Stereo s = rig.render((int) (1.0 * 48000 / 256));
                double lo = 1.0e9, hi = 0.0;
                for (size_t at = 0; at + 960 <= s.left.size(); at += 960)
                {
                    const double v = level(s, at, at + 960);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                return hi / std::max(lo, 1.0e-9);
            };
            // (the engine's own movement of level is under x 2.1; the staircase at full depth is x 25 and more)
            const double flat = swing();
            expect(flat < 4.0, "eight equal levels: the level stands, x " + juce::String(flat));
            rig.audio.setEngineType(1, EngineType::Drum, true);             // (no LSHP page: the staircase)
            rig.audio.setEngineType(1, EngineType::UnifiedVoice, true);     // (a new one: its levels as they come)
            const double fresh = swing();
            expect(fresh > 10.0, "another engine: its own levels, not the ones before, x " + juce::String(fresh));
            for (int i = 0; i < 8; ++i)
                rig.audio.setTrackParameter(1, LfoShapeParam::BASE + i, 1.0f);
            auto parked = rig.audio.parkEngine(1, EngineType::UnifiedVoice);
            const double audition = swing();
            expect(audition > 10.0, "set aside for an audition: the new engine's levels, x " + juce::String(audition));
            rig.audio.unparkEngine(1, parked);
            const double back = swing();
            expect(back < 4.0, "brought back: its levels again, x " + juce::String(back));
        }

        // (TALK non-finite test removed: TALK is now a CellEffect insert, not a fixed block.
        //  CellEffect's own saneBlock handles non-finite input.)

        beginTest("a step lock on a cell that rebuilds data is not applied; the lock beside it is");
        {
            Rig rig;
            rig.start();
            Step step;
            step.trig = true;
            step.paramLocks[5] = 1.0f;      // (Tone::isRebuildParam(5))
            step.paramLocks[6] = 0.5f;
            rig.audio.applyStepLocks(1, step);
            expectEquals(rig.keys->cell[0], 0.0f, "the rebuild cell stays");
            expectEquals(rig.keys->cell[1], 0.5f);
            Step next;
            next.trig = true;
            rig.audio.applyStepLocks(1, next);
            expectEquals(rig.keys->cell[0], 0.0f);
            expectEquals(rig.keys->cell[1], 0.0f, "and goes back after the step");
            rig.audio.setTrackParameter(1, 5, 1.0f);
            expectEquals(rig.keys->cell[0], 1.0f, "(the knob still sets it)");
        }
    }

    // ---- 5. what the review of the phase found ----------------------------------------------------
    // The largest step from one sample to the next in [from, to).
    static float largestStep(const Stereo& s, size_t from, size_t to)
    {
        float worst = 0.0f;
        for (size_t i = std::max<size_t>(1, from); i < std::min(to, s.left.size()); ++i)
            worst = std::max({ worst, std::abs(s.left[i] - s.left[i - 1]), std::abs(s.right[i] - s.right[i - 1]) });
        return worst;
    }
    static float peakOf(const Stereo& s)
    {
        float peak = 0.0f;
        for (float x : s.left) peak = std::max(peak, std::abs(x));
        return peak;
    }

    // An effect that gives what is not a number in one block, and counts how often it is emptied.
    struct Spoiler : EffectBase
    {
        int blocks = 0, spoilAt = -1, resets = 0;
        void reset() override { ++resets; }
        void process(float&, float&) override {}
        void processBlock(float* left, float*, int) override
        {
            if (blocks++ == spoilAt) left[0] = std::numeric_limits<float>::quiet_NaN();
        }
    };

    void testReview()
    {
        beginTest("FLG with THRU on hands its mix to the chain: the chain delays the dry by TIME (the unified through-zero), and the crossing at MIX 0.5 is audible");
        {
            const Stereo in = testSound();
            auto flanger = [](float mix, bool thru)
            {
                auto fx = AudioEngine::createEffect(EffectType::FlangerEffect);
                fx->setExtra(2, thru ? 1.0f : 0.0f);   // THRU
                fx->setParameter(2, 0.0f);              // no feedback
                fx->setParameter(3, mix);
                return fx;
            };
            // In a chain, FLG with THRU hands its mix over: the chain's DryLine holds the
            // delayed dry, and at MIX 0.5 the crossing sounds different from no THRU.
            EffectChain chainThru, chainPlain;
            chainThru.prepare(48000.0, 256);
            chainThru.addEffect(flanger(0.5f, true));
            chainPlain.prepare(48000.0, 256);
            chainPlain.addEffect(flanger(0.5f, false));
            const Stereo withThru = through(chainThru, in, 256), withoutThru = through(chainPlain, in, 256);
            expect(nullDb(withThru, withoutThru) > -40.0, "THRU ON differs from THRU OFF by " + juce::String(nullDb(withThru, withoutThru)) + " dB");
            expect(chainThru.getEffect(0)->givesWetOnly(), "FLG hands its mix to the chain");
        }

        beginTest("an effect whose WET was below the top from its first block is faded when it is switched off and on under sound, not cut");
        for (EffectType type : { EffectType::EQEffect, EffectType::TremoloEffect, EffectType::CompressorEffect })
        {
            const int n = 48000;
            Stereo in;
            in.left.resize((size_t) n); in.right.resize((size_t) n);
            for (int i = 0; i < n; ++i)
                in.left[(size_t) i] = in.right[(size_t) i] = 0.5f * (float) std::sin(6.283185307 * 100.0 * i / 48000.0);
            EffectChain chain;
            chain.prepare(48000.0, 64);
            auto fx = AudioEngine::createEffect(type);
            if (type == EffectType::EQEffect) { fx->setParameter(0, 12.0f); fx->setParameter(1, 12.0f); }
            if (type == EffectType::TremoloEffect) { fx->setParameter(0, 0.5f); fx->setParameter(1, 1.0f); }
            if (type == EffectType::CompressorEffect) { fx->setParameter(0, -40.0f); fx->setParameter(1, 20.0f); }
            fx->setMix(0.6f);
            chain.addEffect(std::move(fx));
            // (switched in blocks that begin near the tone's peak: 100 Hz, 480 samples a cycle)
            const int offAt = (120 + 480 * 20) / 64 + 1, onAt = (120 + 480 * 50) / 64 + 1;
            float afterOff = -2.0f, afterOn = -2.0f;
            const Stereo out = through(chain, in, 64, [&](int b)
            {
                if (b == offAt) chain.getEffect(0)->setEnabled(false);
                if (b == offAt + 1) afterOff = chain.getEffect(0)->presence;
                if (b == onAt) chain.getEffect(0)->setEnabled(true);
                if (b == onAt + 1) afterOn = chain.getEffect(0)->presence;
            });
            const juce::String name = AudioEngine::effectTypeName(type);
            expectWithinAbsoluteError(afterOff, 1.0f - 64.0f / 144.0f, 0.01f, name + ": 64 samples into a fade of 3 ms");
            expectWithinAbsoluteError(afterOn, 64.0f / 144.0f, 0.01f, name + ": and on the way back in");
            const float tone = largestStep(in, 1000, 2000);
            for (int at : { offAt, onAt })
            {
                const float step = largestStep(out, (size_t) at * 64, (size_t) at * 64 + 256);
                const float before = largestStep(out, (size_t) at * 64 - 960, (size_t) at * 64);
                const float after = largestStep(out, (size_t) at * 64 + 256, (size_t) at * 64 + 1216);
                expect(step < 1.3f * std::max({ tone, before, after }), name + ": a step of " + juce::String(step) + " where the sound's own are "
                                                                         + juce::String(before) + " and " + juce::String(after));
            }
            // off, it is the sound that came in
            expect(out.left[(size_t) (onAt * 64 - 1)] == in.left[(size_t) (onAt * 64 - 1)], name + ": switched off, a wire");
        }

        beginTest("PERF: a muted track that a pad's effect listens to by number plays on while the pad is down, as for an insert");
        {
            Rig rig;
            SequencerCore seq;
            rig.audio.setSequencer(&seq);
            seq.getPattern().getTrack(0).muted = true;
            auto& perf = rig.audio.getPerfFx();
            perf.clear(0);
            perf.setKind(0, 0, PerfFx::INSERT_BASE + (int) EffectType::CompressorEffect);
            PerfFx::Setting s = perf.get(0);
            s.layer[0].target = (int) PerfFx::MASTER;
            s.layer[0].cell = { -30.0f, 10.0f, 1.0f, 0.0f };
            s.layer[0].extra = { 1.0f, 40.0f, 3.0f, 20.0f };        // KEY T1
            perf.set(0, s);
            rig.start();
            rig.chord();
            rig.render(20);
            expect(! seq.trackPlays(0), "the pad up: the muted track is not kept playing");
            perf.press(0, 1);
            rig.render(20);
            expect(seq.trackPlays(0), "the pad down: the track it listens to plays on (its fader shut)");
            s.layer[0].extra[0] = 0.0f;                             // KEY SELF
            perf.set(0, s);
            rig.render(20);
            expect(! seq.trackPlays(0), "KEY at SELF: nothing is listened to");
        }

        beginTest("the master chain gone wrong (what is not a number): the block is silence, the chain is emptied, the next block sounds again");
        {
            Rig rig(48000.0, 256, Tone::Keys, true);
            auto spoiler = std::make_unique<Spoiler>();
            auto* seen = spoiler.get();
            rig.audio.getInsertChain(M)->addEffect(std::move(spoiler));
            rig.start();
            rig.chord();
            rig.render(30);
            const int resetsBefore = seen->resets;
            seen->spoilAt = seen->blocks;
            const Stereo bad = rig.render(1), next = rig.render(8);
            expectEquals(rig.audio.getNonFiniteBlocks(), 1);
            expectEquals(seen->resets, resetsBefore + 1, "the master chain's memory is emptied");
            expect(finite(next, 4.0f) && level(next, 0, next.left.size()) > 1.0e-3, "and it plays on");
            expect(finite(bad, 4.0f), "the bad block is numbers");
        }

        beginTest("an edit of the master chain under sound is carried off (the master declick), not heard as a step");
        {
            // Twelve decibels on three bands of the master EQ, then the EQ taken out where the
            // sound stands far from zero: a cut from several times the level to the level itself.
            Rig rig(48000.0, 64, Tone::Keys, false);
            rig.audio.removeInsertEffect(M, 2);         // (no limiter: nothing holds the level)
            for (int i : { 0, 1, 3 }) rig.audio.setMasterParam(i, 12.0f);
            rig.start();
            rig.keys->noteOn(33, 0.9f, -1);
            const Stereo before = rig.render(400);
            const float steady = largestStep(before, before.left.size() / 2, before.left.size());
            Stereo out;
            int waited = 0;
            do { out = rig.render(1); } while (std::abs(out.left.back()) < 0.5f * peakOf(before) && ++waited < 2000);
            rig.audio.removeInsertEffect(M, 0);
            const Stereo after = rig.render(8);
            out.left.insert(out.left.end(), after.left.begin(), after.left.end());
            out.right.insert(out.right.end(), after.right.begin(), after.right.end());
            const float step = largestStep(out, 60, out.left.size());
            expect(waited < 2000 && step < 1.5f * steady, "the step at the edit is " + juce::String(step) + ", the sound's own largest " + juce::String(steady));
        }
    }

    // ---- cost ---------------------------------------------------------------------------------------
    // NEPTUNE_E5_BENCH=1: microseconds per block of 512 samples at 48 kHz, old against new in this
    // one binary, each the best of several runs taken in turn.
    template <typename Fn>
    static double microseconds(Fn&& fn, int blocks)
    {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
    }

    void bench()
    {
        const int B = 512, blocks = 4000;
        const Stereo sound = testSound(B * 8);
        auto report = [&](const juce::String& name, const std::function<double()>& before, const std::function<double()>& after)
        {
            double a = 1.0e9, b = 1.0e9;
            for (int run = 0; run < 7; ++run) { a = std::min(a, before()); b = std::min(b, after()); }
            std::cout << "E5 bench | " << name << " | " << juce::String(a, 2) << " | " << juce::String(b, 2) << " | "
                      << juce::String(100.0 * (b - a) / a, 0) << " %" << std::endl;
        };
        std::cout << "E5 bench | what | old us / 512 | new us / 512 | change" << std::endl;

        for (int hot = 0; hot < 2; ++hot)
        {
            Stereo in = sound;
            if (hot) for (auto* ch : { &in.left, &in.right }) for (auto& x : *ch) x *= 4.0f;
            report(hot ? "master, limiter working: three members per sample / the chain by the block"
                       : "master at rest: three members per sample / the chain by the block",
                [&]
                {
                    OldMaster old;
                    old.prepare(48000.0);
                    Stereo s = in;
                    return microseconds([&] { for (int b = 0; b < blocks; ++b) { const size_t at = (size_t) (b % 8) * B;
                        for (size_t i = at; i < at + B; ++i) { old.eq.process(s.left[i], s.right[i]); old.comp.process(s.left[i], s.right[i]); old.limiter.process(s.left[i], s.right[i]); } } }, blocks);
                },
                [&]
                {
                    AudioEngine audio;
                    auto& chain = *audio.getInsertChain(M);
                    chain.prepare(48000.0, B);
                    Stereo s = in;
                    KeyFeed keys;
                    return microseconds([&] { for (int b = 0; b < blocks; ++b) { const size_t at = (size_t) (b % 8) * B;
                        chain.processStereo(s.left.data() + at, s.right.data() + at, B, &keys); } }, blocks);
                });
        }

        for (EffectType type : { EffectType::ChorusEffect, EffectType::TapeDelayEffect, EffectType::PhaserEffect, EffectType::CompressorEffect, EffectType::EQEffect })
            for (float wet : { 1.0f, 0.5f })
                report(juce::String(AudioEngine::effectTypeName(type)) + " at WET " + juce::String(wet) + ": mixing by itself / in a chain (the chain mixes)",
                    [&]
                    {
                        auto fx = working(type);
                        fx->prepare(48000.0, B);
                        fx->setMix(wet);
                        Stereo s = sound;
                        return microseconds([&] { for (int b = 0; b < blocks; ++b) { const size_t at = (size_t) (b % 8) * B;
                            fx->processStereo(s.left.data() + at, s.right.data() + at, B); } }, blocks);
                    },
                    [&]
                    {
                        EffectChain chain;
                        chain.prepare(48000.0, B);
                        auto fx = working(type);
                        fx->setMix(wet);
                        chain.addEffect(std::move(fx));
                        Stereo s = sound;
                        KeyFeed keys;
                        return microseconds([&] { for (int b = 0; b < blocks; ++b) { const size_t at = (size_t) (b % 8) * B;
                            chain.processStereo(s.left.data() + at, s.right.data() + at, B, &keys); } }, blocks);
                    });

        // The whole instrument: four tracks of the test tone, two inserts each, the master chain.
        {
            double best = 1.0e9;
            for (int run = 0; run < 5; ++run)
                best = std::min(best, e5atlas::engineBlockMicroseconds());
            std::cout << "E5 bench | whole engine, 4 tracks x (CHR + CMP), sends, master | " << juce::String(best, 2) << " us / 512 (run the binary of the commit before for the old number)" << std::endl;
        }
    }
};

static MasterChainE5Tests masterChainE5Tests;
