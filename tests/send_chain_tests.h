#pragma once

#include <juce_core/juce_core.h>
#include "engine/AudioEngine.h"
#include "sequencer/SequencerCore.h"
#include "project/ProjectBridge.h"
#include "ui/screens/TrackFxScreen.h"
#include "master_chain_atlas.h"
#include "old/OldSendsAudioEngine.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------------------------------
// The send buses as ordinary insert chains (.ralph/spec-unify.md, section 4: "send buses as chains
// with a page and stored cells, on the insert interface as the master is"):
//   1. the sends as a new project has them (a reverb, a delay) give the samples they gave before,
//      bit for bit, whatever the send levels do: against the AudioEngine as it was
//      (tests/old/OldSendsAudioEngine.h), and a file from before with another type on a bus loads
//      to what that engine played;
//   2. a send is addressed as the master chain is: AudioEngine::sendTrack(bus) with the insert
//      functions and the insert ids; any insert type, several in a row, keyed from a track, the
//      cells beyond four; a send gives the wet sound alone and an empty one gives nothing;
//   3. two pages a send on TRK FX (S1FX, S1FX2, ...) laid out as FX and FX2 are;
//   4. stored with the project (the ids a track's inserts are stored with), and the old shape read.
// NEPTUNE_SENDS_BENCH=1: the cost table (Release binary).
// -----------------------------------------------------------------------------------------------
class SendChainTests : public juce::UnitTest
{
public:
    SendChainTests() : UnitTest("Send Chains") {}

    using Tone = e5atlas::ToneEngine;
    using Stereo = e5atlas::Stereo;
    static constexpr int M = AudioEngine::MASTER_TRACK;
    static constexpr int S1 = AudioEngine::SEND_TRACK_BASE, S2 = AudioEngine::SEND_TRACK_BASE + 1;
    static constexpr int CELL = AudioEngine::EFFECT_PARAM_BASE;
    static constexpr int EXTRA = AudioEngine::INSERT_EXTRA_BASE;
    static constexpr int ON = AudioEngine::INSERT_ON_BASE;
    static constexpr int WET = AudioEngine::INSERT_WET_BASE;
    static constexpr int SYNC = AudioEngine::INSERT_SYNC_BASE;
    static constexpr int DIV = AudioEngine::INSERT_DIV_BASE;
    static constexpr int SND = MixParam::SEND_BASE;

    // Track 1 a drum that strikes by itself four times a second, track 2 the keys; on either engine.
    template <typename Engine>
    struct RigOf
    {
        Engine audio;
        std::shared_ptr<Tone> kick, keys;
        double rate;
        int block;
        std::vector<float> l, r;

        explicit RigOf(double sr = 48000.0, int blockSize = 256, bool kickHeard = false)
            : kick(std::make_shared<Tone>(Tone::Kick)), keys(std::make_shared<Tone>(Tone::Keys)), rate(sr), block(blockSize),
              l((size_t) blockSize), r((size_t) blockSize)
        {
            audio.addEngine(0, kick);
            audio.addEngine(1, keys);
            audio.setTrackParameter(0, MixParam::VOLUME, kickHeard ? 0.8f : 0.0f);
            audio.setTrackParameter(1, MixParam::VOLUME, 0.8f);
        }
        void start() { audio.prepare(rate, block); }
        void chord() { for (int note : { 48, 55, 64 }) keys->noteOn(note, 0.9f, -1); }
        // Nothing after the sum: what comes out is the tracks and what the sends return.
        void bareMaster()
        {
            while (audio.getInsertChain(M)->getEffectCount() > 0)
                audio.removeInsertEffect(M, 0);
            audio.setMasterLevel(1.0f);
        }

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
    using Rig = RigOf<AudioEngine>;
    using OldRig = RigOf<OldSendsAudioEngine>;

    static bool sameSamples(const Stereo& a, const Stereo& b)
    {
        return a.left.size() == b.left.size() && ! a.left.empty()
            && std::memcmp(a.left.data(), b.left.data(), a.left.size() * sizeof(float)) == 0
            && std::memcmp(a.right.data(), b.right.data(), a.right.size() * sizeof(float)) == 0;
    }
    static size_t differing(const Stereo& a, const Stereo& b)
    {
        size_t n = 0;
        for (size_t i = 0; i < std::min(a.left.size(), b.left.size()); ++i)
            n += (a.left[i] != b.left[i] || a.right[i] != b.right[i]) ? 1 : 0;
        return n;
    }
    static double level(const Stereo& s, size_t from, size_t to)
    {
        double sum = 0.0;
        to = std::min(to, s.left.size());
        for (size_t i = from; i < to; ++i)
            sum += (double) s.left[i] * s.left[i] + (double) s.right[i] * s.right[i];
        return to > from ? std::sqrt(sum / (2.0 * (double) (to - from))) : 0.0;
    }
    static double level(const Stereo& s) { return level(s, 0, s.left.size()); }
    static Stereo minus(const Stereo& a, const Stereo& b)
    {
        Stereo d = a;
        for (size_t i = 0; i < std::min(a.left.size(), b.left.size()); ++i)
        {
            d.left[i] -= b.left[i];
            d.right[i] -= b.right[i];
        }
        return d;
    }
    static bool finite(const Stereo& s, float limit)
    {
        for (size_t i = 0; i < s.left.size(); ++i)
            if (! (std::abs(s.left[i]) <= limit) || ! (std::abs(s.right[i]) <= limit))
                return false;
        return true;
    }
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

    // ---- the send levels of a take: still, jumping, gliding, shut, on both tracks and both buses ----
    static float sendLevelAt(int block, int track, int bus)
    {
        const int which = track * 2 + bus;
        if (block < 40)
            return 0.0f;                                    // nothing sent yet
        if (block < 300)
        {
            // a new level every 23 to 44 blocks, 0 and 1 among them
            const int period = 23 + 7 * which;
            juce::uint32 x = (juce::uint32) (block / period) * 2654435761u + (juce::uint32) which * 40503u + 17u;
            x ^= x >> 13; x *= 1274126177u; x ^= x >> 16;
            const int step = (int) (x % 6u);
            return step == 0 ? 0.0f : (step == 5 ? 1.0f : (float) step * 0.21f);
        }
        if (block < 520)                                    // moving in every block
            return 0.5f + 0.5f * std::sin(0.07f * (float) block + 1.3f * (float) which);
        if (block < 600)
            return which == 1 ? 0.0f : 0.35f;               // one of them shut while the others stand
        return ((block / 3) % 2 == 0) ? 0.9f : 0.1f * (float) (which + 1);     // hard to and fro
    }

    template <typename Engine>
    static Stereo take(RigOf<Engine>& rig, double seconds, bool withLevels = true)
    {
        rig.start();
        const int blocks = (int) (seconds * rig.rate / rig.block);
        return rig.render(blocks, [&](int b)
        {
            if (b == 0) rig.chord();
            if (b % 97 == 50) rig.keys->noteOn(36 + (b / 97) % 24, 0.8f, -1);
            if (b % 97 == 90) rig.keys->noteOff(36 + (b / 97) % 24, -1);
            if (withLevels)
                for (int t = 0; t < 2; ++t)
                    for (int bus = 0; bus < 2; ++bus)
                        rig.audio.setTrackParameter(t, SND + bus, sendLevelAt(b, t, bus));
        });
    }

    // A project loaded into a rig, then the tone and the drum in place of the project's own (silent)
    // tracks. (A load is an edit of the sends and the output is carried from what it was over 2 ms:
    // those blocks are played, on silence, before the take.)
    static void load(Rig& rig, SequencerCore& seq, const ProjectData& data)
    {
        ProjectBridge::Live live { seq, rig.audio };
        ProjectBridge::apply(data, live);
        rig.start();
        const Stereo quiet = rig.render(4);
        jassert(level(quiet) == 0.0);
        rig.audio.addEngine(0, rig.kick);
        rig.audio.addEngine(1, rig.keys);
        rig.audio.setTrackParameter(0, MixParam::VOLUME, 0.8f);
        rig.audio.setTrackParameter(1, MixParam::VOLUME, 0.8f);
    }

    // A project file as it was written before the sends stored their chains: one type a bus.
    static ProjectData oldProject(std::initializer_list<int> busTypes)
    {
        ProjectData data;
        for (int type : busTypes)
            data.sendBuses.push_back({ type, 1.0f });
        const auto read = ProjectData::fromJSON(data.toJSON());
        return read;
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

    // The values of every insert id of a chain's slots in use, by id.
    static std::vector<float> chainValues(const AudioEngine& audio, int chainTrack)
    {
        std::vector<float> v;
        const auto* chain = audio.getInsertChain(chainTrack);
        const int count = chain != nullptr ? chain->getEffectCount() : 0;
        v.push_back((float) count);
        for (int slot = 0; slot < count; ++slot)
        {
            EffectType type {};
            v.push_back(audio.getInsertEffectType(chainTrack, slot, type) ? (float) (int) type : -1.0f);
            for (int id : { ON + slot, WET + slot, SYNC + slot, DIV + slot })
                v.push_back(audio.getTrackParameter(chainTrack, id));
            for (int i = 0; i < 4; ++i)
            {
                v.push_back(audio.getTrackParameter(chainTrack, CELL + slot * 4 + i));
                v.push_back(audio.getTrackParameter(chainTrack, EXTRA + slot * 4 + i));
            }
        }
        return v;
    }
    static bool near(const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 1.0e-4f * std::max(1.0f, std::abs(a[i])))
                return false;
        return true;
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_SENDS_BENCH", {}).isNotEmpty())
        {
            beginTest("bench");
            bench();
            return;
        }
        testSameSamples();
        testAddressing();
        testChains();
        testPages();
        testStored();
        testReview();
    }

    // ---- 1. nothing moved ---------------------------------------------------------------------------
    void testSameSamples()
    {
        beginTest("the sends as a new project has them: the samples the engine gave before, bit for bit, with the send levels still, jumping, gliding and shut");
        {
            struct Case { double rate; int block; double seconds; };
            for (const Case c : { Case { 48000.0, 256, 6.0 }, Case { 44100.0, 64, 3.0 }, Case { 96000.0, 512, 3.0 }, Case { 48000.0, 1, 0.25 } })
            {
                OldRig before(c.rate, c.block, true);
                Rig now(c.rate, c.block, true);
                const Stereo a = take(before, c.seconds), b = take(now, c.seconds);
                const juce::String at = juce::String(c.rate) + " Hz, blocks of " + juce::String(c.block) + ", " + juce::String(c.seconds) + " s";
                expect(sameSamples(a, b), at + ": " + juce::String((int) differing(a, b)) + " of " + juce::String((int) a.left.size()) + " samples differ");
                expectEquals(now.audio.getNonFiniteBlocks(), 0);
                if (c.block == 256)
                {
                    // (and the take is one that would show a difference: the sends are most of what moves in it)
                    Rig dry(c.rate, c.block, true);
                    const Stereo d = take(dry, c.seconds, false);
                    expect(level(minus(b, d)) > 0.05 * level(d), "the sends are heard in the take: " + juce::String(level(minus(b, d))) + " beside " + juce::String(level(d)));
                }
            }
        }

        beginTest("the same after the sends were worked on and put back (resetSendBuses), and after a project without sends was loaded");
        {
            OldRig before(48000.0, 256, true);
            const Stereo a = take(before, 2.0);

            Rig worked(48000.0, 256, true);
            worked.audio.setInsertEffect(S1, 0, EffectType::HallReverbEffect);
            worked.audio.addInsertEffect(S1, EffectType::PhaserEffect);
            worked.audio.setTrackParameter(S2, CELL + 0, 0.111f);
            worked.audio.removeInsertEffect(S2, 0);
            worked.audio.resetSendBuses();
            expect(sameSamples(a, take(worked, 2.0)), "resetSendBuses: the sends of a new project");

            for (int shape = 0; shape < 2; ++shape)
            {
                // (the project's own tracks are silent synths; the tone and the drum take their place)
                SequencerCore seq;
                Rig loaded(48000.0, 256, true);
                loaded.audio.setInsertEffect(S1, 0, EffectType::ChorusEffect);
                loaded.audio.setTrackParameter(S2, CELL + 1, 0.9f);
                load(loaded, seq, shape == 0 ? ProjectData() : oldProject({ 6, 5 }));
                const Stereo b = take(loaded, 2.0);
                expect(sameSamples(a, b), juce::String(shape == 0 ? "a project without sends" : "a project of before, RVB and DLY by type")
                                              + ": " + juce::String((int) differing(a, b)) + " samples differ");
            }
        }

        beginTest("the sends of a new project saved as chains and loaded again: still the samples of before, bit for bit (a project saved from now on plays as it did)");
        {
            OldRig before(48000.0, 256, true);
            const Stereo a = take(before, 3.0);
            SequencerCore seq0;
            Rig fresh(48000.0, 256, true);      // (the faders as the take has them: a fader that moves in the first block is another take)
            ProjectBridge::Live live0 { seq0, fresh.audio };
            const ProjectData saved = ProjectData::fromJSON(ProjectBridge::capture(live0, "new").toJSON());
            expect(saved.sendBuses.size() == 2 && saved.sendBuses[0].hasChain && saved.sendBuses[1].hasChain);
            for (int only = 0; only < 3; ++only)
            {
                ProjectData data = saved;
                if (only == 1) data.sendBuses[1] = { 5, 1.0f };       // (bus 2 by its type alone: bus 1 is what is looked at)
                if (only == 2) data.sendBuses[0] = { 6, 1.0f };
                SequencerCore s2;
                Rig again(48000.0, 256, true);
                load(again, s2, data);
                const Stereo b = take(again, 3.0);
                expect(sameSamples(a, b), juce::String(only == 0 ? "both sends" : (only == 1 ? "the reverb" : "the delay")) + " from the stored chain: "
                                              + juce::String((int) differing(a, b)) + " samples differ");
            }
        }

        beginTest("a project of before with another type on a bus: that effect as the engine of before made it (HALL on bus 1, CHR on bus 2), bit for bit");
        {
            OldRig before(48000.0, 256, true);
            before.audio.setSendBusEffect(0, EffectType::HallReverbEffect);
            before.audio.setSendBusEffect(1, EffectType::ChorusEffect);
            const Stereo a = take(before, 2.0);

            SequencerCore seq;
            Rig loaded(48000.0, 256, true);
            load(loaded, seq, oldProject({ (int) EffectType::HallReverbEffect, (int) EffectType::ChorusEffect }));
            EffectType t0 {}, t1 {};
            expect(loaded.audio.getInsertEffectType(S1, 0, t0) && t0 == EffectType::HallReverbEffect
                   && loaded.audio.getInsertEffectType(S2, 0, t1) && t1 == EffectType::ChorusEffect, "the types of the file");
            expectEquals(loaded.audio.getInsertChain(S1)->getEffectCount(), 1);
            const Stereo b = take(loaded, 2.0);
            expect(sameSamples(a, b), juce::String((int) differing(a, b)) + " samples differ");

            // A type a file cannot name, and more buses than there are: held, nothing built for them.
            SequencerCore seq2;
            Rig odd;
            ProjectBridge::Live live2 { seq2, odd.audio };
            ProjectBridge::apply(oldProject({ 9999, -4, 3, 3, 3, 3, 3, 3, 3, 3, 3 }), live2);
            expectEquals(odd.audio.getSendCount(), 2);
            expect(odd.audio.getInsertEffectType(S1, 0, t0) && (int) t0 == (int) EffectType::NumEffectTypes - 1);
            expect(odd.audio.getInsertEffectType(S2, 0, t1) && t1 == EffectType::ChorusEffect);
        }
    }

    // ---- 2. addressed like the master ---------------------------------------------------------------
    void testAddressing()
    {
        beginTest("a send answers to the insert functions and the insert ids as the master chain does, and to nothing else of a track");
        {
            AudioEngine audio;
            expectEquals((int) AudioEngine::sendTrack(0), M + 1);
            expect(AudioEngine::isSendTrack(S1) && AudioEngine::isSendTrack(S1 + AudioEngine::MAX_SENDS - 1)
                   && ! AudioEngine::isSendTrack(M) && ! AudioEngine::isSendTrack(S1 + AudioEngine::MAX_SENDS) && ! AudioEngine::isSendTrack(0));
            expect(audio.getInsertChain(S1) == &audio.getSendBus(0).getChain() && audio.getInsertChain(S2) == &audio.getSendBus(1).getChain());
            expect(audio.getInsertChain(S1 + 2) == nullptr, "a send that is not there has no chain");

            EffectType type {};
            expect(audio.getInsertEffectType(S1, 0, type) && type == EffectType::ReverbEffect);
            expect(audio.getInsertEffectType(S2, 0, type) && type == EffectType::DelayEffect);
            expect(! audio.getInsertEffectType(S1, 1, type) && ! audio.getInsertEffectType(S1 + 2, 0, type));
            expect(audio.getSendBusType(0) == EffectType::ReverbEffect && audio.getSendBusType(1) == EffectType::DelayEffect);

            // the cells of the reverb and the delay a new project has, read through the ids
            auto* reverb = audio.getInsertChain(S1)->getEffect(0);
            auto* delay = audio.getInsertChain(S2)->getEffect(0);
            for (int i = 0; i < 4; ++i)
            {
                expectEquals(audio.getTrackParameter(S1, CELL + i), reverb->getParameter(i));
                expectEquals(audio.getTrackParameter(S2, CELL + i), delay->getParameter(i));
                expectEquals(audio.getTrackParameter(S2, EXTRA + i), delay->getExtra(i));
            }
            expectEquals(audio.getTrackParameter(S1, WET + 0), 1.0f);
            expectEquals(audio.getTrackParameter(S1, ON + 0), 1.0f);

            // set through the ids: the four cells, the cells beyond, ON, WET, SYNC and the division
            for (int i = 0; i < 4; ++i)
            {
                const ParamSpec spec = delay->getParamSpec(i), more = delay->getExtraSpec(i);
                if (spec.isValid() && ! spec.isDiscrete())
                {
                    const float v = spec.minVal + 0.3f * (spec.maxVal - spec.minVal);
                    audio.setTrackParameter(S2, CELL + i, v);
                    expectWithinAbsoluteError(delay->getParameter(i), v, 1.0e-4f * std::max(1.0f, std::abs(v)));
                    expectEquals(audio.getTrackParameter(S2, CELL + i), delay->getParameter(i));
                }
                if (more.isValid() && ! more.isDiscrete())
                {
                    const float v = more.minVal + 0.6f * (more.maxVal - more.minVal);
                    audio.setTrackParameter(S2, EXTRA + i, v);
                    expectWithinAbsoluteError(delay->getExtra(i), v, 1.0e-4f * std::max(1.0f, std::abs(v)));
                }
            }
            audio.setTrackParameter(S2, WET + 0, 0.4f);
            expectEquals(delay->getMix(), 0.4f);
            audio.setTrackParameter(S2, ON + 0, 0.0f);
            expect(! delay->isEnabled() && audio.getTrackParameter(S2, ON + 0) == 0.0f);
            expect(delay->canSync(), "the delay has a time to lock to the tempo");
            audio.setTrackParameter(S2, SYNC + 0, 1.0f);
            audio.setTrackParameter(S2, DIV + 0, 3.0f);
            expect(delay->isSynced() && delay->getSyncDivision() == 3);
            expectEquals(audio.getTrackParameter(S2, SYNC + 0), 1.0f);
            expectEquals(audio.getTrackParameter(S2, DIV + 0), 3.0f);
            expectEquals(reverb->getMix(), 1.0f, "the other send is another chain");

            // nothing else of a track: the ids are ignored and read 0, nothing is built for them
            for (int id : { (int) MixParam::VOLUME, (int) MixParam::PAN, SND, (int) TrackFxParam::DIRT_BASE, (int) TrackInParam::SRC,
                            (int) AudioEngine::ARP_LOCK_BASE, 5, 400, 700, 232 })
            {
                audio.setTrackParameter(S1, id, 0.77f);
                expectEquals(audio.getTrackParameter(S1, id), 0.0f, "id " + juce::String(id));
            }
            expect(audio.getEngine(S1) == nullptr);  // sends have no engine
            // a send that is not there: nothing happens
            audio.setInsertEffect(S1 + 5, 0, EffectType::ChorusEffect);
            audio.addInsertEffect(S1 + 5, EffectType::ChorusEffect);
            audio.removeInsertEffect(S1 + 5, 0);
            audio.setTrackParameter(S1 + 5, CELL, 0.5f);
            expectEquals(audio.getTrackParameter(S1 + 5, CELL), 0.0f);
            expectEquals(audio.getSendCount(), 2);
            // what is not a number is not taken
            const float was = audio.getTrackParameter(S1, CELL + 0);
            audio.setTrackParameter(S1, CELL + 0, std::numeric_limits<float>::quiet_NaN());
            expectEquals(audio.getTrackParameter(S1, CELL + 0), was);
        }

        beginTest("the chain says what it holds: the type of every slot follows add, set and remove on a track, the master chain and a send (no list beside the chain)");
        {
            AudioEngine audio;
            audio.addEngine(0, std::make_shared<Tone>(Tone::Keys));
            for (int chainTrack : { 0, M, S1 })
            {
                while (audio.getInsertChain(chainTrack) != nullptr && audio.getInsertChain(chainTrack)->getEffectCount() > 0)
                    audio.removeInsertEffect(chainTrack, 0);
                audio.addInsertEffect(chainTrack, EffectType::ChorusEffect);
                audio.addInsertEffect(chainTrack, EffectType::PhaserEffect);
                audio.addInsertEffect(chainTrack, EffectType::WahEffect);
                audio.setInsertEffect(chainTrack, 1, EffectType::TapeDelayEffect);
                audio.setInsertEffect(chainTrack, 3, EffectType::LimiterEffect);      // the first empty slot: added
                audio.removeInsertEffect(chainTrack, 0);
                const EffectType expected[] = { EffectType::TapeDelayEffect, EffectType::WahEffect, EffectType::LimiterEffect };
                expectEquals(audio.getInsertChain(chainTrack)->getEffectCount(), 3);
                for (int slot = 0; slot < 3; ++slot)
                {
                    EffectType type {};
                    expect(audio.getInsertEffectType(chainTrack, slot, type) && type == expected[slot],
                           "chain " + juce::String(chainTrack) + " slot " + juce::String(slot));
                    // (and it is that effect: its first cell is that type's)
                    expectEquals(juce::String(audio.getInsertChain(chainTrack)->getEffect(slot)->getParamSpec(0).label),
                                 juce::String(AudioEngine::createEffect(expected[slot])->getParamSpec(0).label));
                }
                EffectType none {};
                expect(! audio.getInsertEffectType(chainTrack, 3, none));
            }
            expectEquals(audio.getMasterSlot(EffectType::LimiterEffect), 2);
            expectEquals(audio.getMasterSlot(EffectType::EQEffect), -1);
        }

        beginTest("one more send, and one fewer: addSendBus / removeSendBus");
        {
            AudioEngine audio;
            audio.prepare(48000.0, 128);
            const int bus = audio.addSendBus(EffectType::ChorusEffect);
            expectEquals(bus, 2);
            EffectType type {};
            expect(audio.getInsertEffectType(AudioEngine::sendTrack(2), 0, type) && type == EffectType::ChorusEffect);
            expectEquals(audio.getTrackParameter(AudioEngine::sendTrack(2), WET), 1.0f);
            audio.removeSendBus(0);
            expectEquals(audio.getSendCount(), 2);
            expect(audio.getInsertEffectType(S1, 0, type) && type == EffectType::DelayEffect, "the types go with their chains");
            expect(audio.getInsertEffectType(S2, 0, type) && type == EffectType::ChorusEffect);
            for (int i = 0; i < 12; ++i) audio.addSendBus(EffectType::ChorusEffect);
            expectEquals(audio.getSendCount(), (int) AudioEngine::MAX_SENDS);
            audio.resetSendBuses();
            expectEquals(audio.getSendCount(), 2);
            expect(audio.getInsertEffectType(S1, 0, type) && type == EffectType::ReverbEffect);
        }
    }

    // ---- 3. what a send can hold ----------------------------------------------------------------------
    void testChains()
    {
        beginTest("a send gives the wet sound alone: an empty send adds nothing, and a send whose effect is switched off adds nothing (silence, not a dry copy)");
        {
            auto play = [&](int kind)
            {
                Rig rig;
                rig.bareMaster();
                if (kind == 1)
                    rig.audio.removeInsertEffect(S1, 0);
                if (kind == 2)
                    rig.audio.setTrackParameter(S1, ON, 0.0f);
                rig.audio.setTrackParameter(1, SND, kind == 0 ? 0.0f : 0.7f);
                rig.start();
                rig.chord();
                return rig.render(60);
            };
            const Stereo dry = play(0), empty = play(1), off = play(2);
            expect(level(dry) > 0.01);
            expect(sameSamples(dry, empty), "an empty send: the tracks alone, " + juce::String((int) differing(dry, empty)) + " samples differ");
            expect(sameSamples(dry, off), "effect OFF: the tracks alone (no dry copy), " + juce::String((int) differing(dry, off)) + " samples differ");
        }

        beginTest("any insert type can sit on a send: it is that type, its mix starts at 1, and it sounds without a number that is not one");
        {
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
            {
                const auto type = static_cast<EffectType>(t);
                const juce::String name = AudioEngine::effectTypeName(type);
                Rig rig(48000.0, 256, true);
                rig.bareMaster();
                rig.audio.setInsertEffect(S1, 0, type);
                EffectType read {};
                expect(rig.audio.getInsertEffectType(S1, 0, read) && read == type, name);
                expectEquals(rig.audio.getInsertChain(S1)->getEffectCount(), 1, name);
                expectEquals(rig.audio.getTrackParameter(S1, WET), 1.0f, name + ": WET of an effect put on a send");
                rig.audio.setTrackParameter(0, SND, 0.8f);
                rig.audio.setTrackParameter(1, SND, 0.8f);
                rig.start();
                rig.chord();
                const Stereo s = rig.render(120);
                expect(finite(s, 30.0f) && level(s) > 0.01, name + ": level " + juce::String(level(s)));
                expectEquals(rig.audio.getNonFiniteBlocks(), 0, name);
            }
        }

        // The send's chain made of `types`, every WET at 1 (a phaser's starts at half), then what
        // `set` sets; the chord on track 2 sent to it. What comes out is the tracks and the return.
        using Types = std::vector<EffectType>;
        auto played = [&](const Types& types, const std::function<void(Rig&)>& set = {}, float sent = 0.8f)
        {
            Rig rig;
            rig.bareMaster();
            rig.audio.removeInsertEffect(S1, 0);
            for (auto type : types)
                rig.audio.addInsertEffect(S1, type);
            expectEquals(rig.audio.getInsertChain(S1)->getEffectCount(), (int) types.size());
            for (int slot = 0; slot < (int) types.size(); ++slot)
                rig.audio.setTrackParameter(S1, WET + slot, 1.0f);
            if (set) set(rig);
            rig.audio.setTrackParameter(1, SND, sent);
            rig.start();
            rig.chord();
            const Stereo s = rig.render(300);
            expect(finite(s, 30.0f));
            expectEquals(rig.audio.getNonFiniteBlocks(), 0);
            return s;
        };
        const Types three { EffectType::TapeDelayEffect, EffectType::PhaserEffect, EffectType::ReverbEffect };
        const char* const threeNames[] = { "TAPE", "PHS", "RVB" };

        beginTest("several in a row: a tape delay into a phaser into a reverb on one send; each is heard, and a chain with one of them switched off is the chain without that effect (the send is not muted)");
        {
            const Stereo tracks = played(three, {}, 0.0f);          // nothing sent: the tracks alone
            const Stereo all = played(three);
            const double returned = level(minus(all, tracks));
            expect(returned > 0.01, "the three in a row return something: " + juce::String(returned));
            for (int off = 0; off < 3; ++off)
            {
                const juce::String name = juce::String(threeNames[off]) + " (slot " + juce::String(off + 1) + ") off";
                const Stereo withOff = played(three, [&](Rig& rig) { rig.audio.setTrackParameter(S1, ON + off, 0.0f); });
                Types rest = three;
                rest.erase(rest.begin() + off);
                const Stereo without = played(rest);
                expect(sameSamples(withOff, without), name + ": the chain without it, " + juce::String((int) differing(withOff, without))
                                                      + " samples differ, by " + juce::String(level(minus(withOff, without)) / returned) + " of the return");
                const double back = level(minus(withOff, tracks)) / returned;
                expect(back > 0.05, name + ": the send still returns " + juce::String(back) + " of what the three return");
                const double moved = level(minus(all, withOff)) / returned;
                expect(moved > 0.02, name + " changes the return by " + juce::String(moved));
            }
            expect(level(minus(all, played({ three[0], three[1] }))) / returned > 0.02, "and the third is not the second's output alone");
        }

        beginTest("a send's law is its entrance's: with every effect off the chain returns silence; with the first off the second is the entrance (no dry copy of what was sent)");
        {
            const Stereo tracks = played(three, {}, 0.0f);
            const Stereo allOff = played(three, [&](Rig& rig) { for (int slot = 0; slot < 3; ++slot) rig.audio.setTrackParameter(S1, ON + slot, 0.0f); });
            expect(sameSamples(tracks, allOff), "every slot off: the tracks alone, " + juce::String((int) differing(tracks, allOff)) + " samples differ");
            // A phaser at WET 0.5 behind a tape delay that is off: it is the entrance, and half of
            // what it makes is returned; nothing of what the bus was sent comes back with it.
            const Types two { EffectType::TapeDelayEffect, EffectType::PhaserEffect };
            auto halfPhaser = [&](Rig& rig) { rig.audio.setTrackParameter(S1, ON, 0.0f); rig.audio.setTrackParameter(S1, WET + 1, 0.5f); };
            const Stereo entrance = minus(played(two, halfPhaser), tracks);
            const Stereo alone = minus(played({ EffectType::PhaserEffect }, [&](Rig& rig) { rig.audio.setTrackParameter(S1, WET, 0.5f); }), tracks);
            const double apart = level(minus(entrance, alone)) / level(alone);
            expect(level(alone) > 0.005 && apart < 1.0e-5, "PHS at WET 0.5 behind a TAPE that is off is PHS alone at WET 0.5: apart by " + juce::String(apart));
        }

        beginTest("WET on a later slot is the blend with the stage before it, as on an insert: TAPE > PHS at WET 0.5 returns half the tape and half the phased tape");
        {
            const Types two { EffectType::TapeDelayEffect, EffectType::PhaserEffect };
            const Stereo tracks = played(two, {}, 0.0f);
            const Stereo tape = minus(played({ EffectType::TapeDelayEffect }), tracks);
            const Stereo phased = minus(played(two), tracks);
            const Stereo half = minus(played(two, [&](Rig& rig) { rig.audio.setTrackParameter(S1, WET + 1, 0.5f); }), tracks);
            Stereo blend = tape;
            for (size_t i = 0; i < blend.left.size(); ++i)
            {
                blend.left[i] = 0.5f * (tape.left[i] + phased.left[i]);
                blend.right[i] = 0.5f * (tape.right[i] + phased.right[i]);
            }
            const double apart = level(minus(half, blend)) / level(blend);
            const double asScaled = level(minus(half, phased)) / level(phased);       // (the old law gave half the phased tape: 0.5 here)
            expect(level(blend) > 0.005 && apart < 1.0e-4, "WET 0.5 on slot 2 is the blend: apart by " + juce::String(apart) + " of it");
            expect(asScaled > 0.02, "and it is not the phased tape alone at some level: apart by " + juce::String(asScaled));
            // WET 0 on slot 2 is the stage before it, to the sample; on slot 1 (the entrance) it is silence.
            const Stereo wetNone = minus(played(two, [&](Rig& rig) { rig.audio.setTrackParameter(S1, WET + 1, 0.0f); }), tracks);
            expect(level(minus(wetNone, tape)) / level(tape) < 1.0e-6, "WET 0 on slot 2: the tape alone, apart by " + juce::String(level(minus(wetNone, tape)) / level(tape)));
            const Stereo shut = played(two, [&](Rig& rig) { rig.audio.setTrackParameter(S1, WET, 0.0f); });
            expect(sameSamples(shut, tracks), "WET 0 on slot 1: nothing is returned (" + juce::String((int) differing(shut, tracks)) + " samples differ)");
        }

        beginTest("a slot of a send's chain switched off and on under sound, the entrance or one behind it: no click, and the send goes on returning");
        {
            for (int which = 0; which < 3; ++which)
            {
                Rig rig(48000.0, 64);
                rig.bareMaster();
                rig.audio.removeInsertEffect(S1, 0);
                for (auto type : three)
                    rig.audio.addInsertEffect(S1, type);
                for (int slot = 0; slot < 3; ++slot)
                    rig.audio.setTrackParameter(S1, WET + slot, 1.0f);
                rig.audio.setTrackParameter(1, SND, 0.5f);
                rig.start();
                rig.keys->noteOn(33, 0.9f, -1);
                const Stereo steady = rig.render(400);
                const float steadyStep = largestStep(steady, steady.left.size() / 2, steady.left.size());
                rig.audio.setTrackParameter(S1, ON + which, 0.0f);
                const Stereo offBlocks = rig.render(8);
                const Stereo offLater = rig.render(200);
                rig.audio.setTrackParameter(S1, ON + which, 1.0f);
                const Stereo onBlocks = rig.render(8);
                const juce::String name = juce::String(threeNames[which]) + " (slot " + juce::String(which + 1) + ")";
                const float offStep = largestStep(offBlocks, 2, offBlocks.left.size()), onStep = largestStep(onBlocks, 2, onBlocks.left.size());
                expect(offStep < 3.0f * steadyStep, name + " OFF: step " + juce::String(offStep) + " vs steady " + juce::String(steadyStep));
                expect(onStep < 3.0f * steadyStep, name + " ON: step " + juce::String(onStep) + " vs steady " + juce::String(steadyStep));
                // With it off the send is not mute: what comes out is not the keys alone.
                Rig bare(48000.0, 64);
                bare.bareMaster();
                bare.start();
                bare.keys->noteOn(33, 0.9f, -1);
                bare.render(408);
                const Stereo keysAlone = bare.render(200);
                const double back = level(minus(offLater, keysAlone)) / level(keysAlone);
                expect(back > 0.01, name + " off: the send still returns " + juce::String(back) + " of the keys' level");
                expectEquals(rig.audio.getNonFiniteBlocks(), 0);
            }
        }

        beginTest("an effect on a send keyed from a track: a compressor after the reverb with KEY at T1 pulls the reverb down under each strike (the kick itself is not heard)");
        {
            auto returned = [&](float key)
            {
                Stereo out[2];
                for (int withSend = 0; withSend < 2; ++withSend)
                {
                    Rig rig;
                    rig.bareMaster();
                    rig.audio.addInsertEffect(S1, EffectType::CompressorEffect);
                    rig.audio.setTrackParameter(S1, CELL + 4 + 0, -40.0f);      // THR
                    rig.audio.setTrackParameter(S1, CELL + 4 + 1, 20.0f);       // RATI
                    rig.audio.setTrackParameter(S1, EXTRA + 4 + 1, 40.0f);
                    rig.audio.setTrackParameter(S1, EXTRA + 4 + 0, key);        // KEY
                    rig.audio.setTrackParameter(1, SND, withSend ? 1.0f : 0.0f);
                    rig.start();
                    rig.chord();
                    out[withSend] = rig.render((int) (2.0 * 48000 / 256));
                }
                // What the send returns in the 40 ms after each strike against 150 to 230 ms after it
                // (the drum strikes every 250 ms).
                const Stereo back = minus(out[1], out[0]);
                const size_t period = 12000;
                double struck = 0.0, between = 0.0;
                for (size_t at = 2 * period; at + period <= back.left.size(); at += period)
                {
                    struck += level(back, at + 240, at + 2160);
                    between += level(back, at + 7200, at + 11040);
                }
                expect(between > 1.0e-4, "the send returns something: " + juce::String(between));
                return between > 0.0 ? struck / between : 1.0;
            };
            const double self = returned(0.0f), keyed = returned(1.0f);
            expect(self > 0.8 && self < 1.25 && keyed < 0.6, "what the send returns under a strike, against between two: KEY SELF "
                                                              + juce::String(self) + ", KEY T1 " + juce::String(keyed));
        }

        beginTest("the cells beyond four are the send's too: a cell of FX2 moved through its id changes what the send returns");
        {
            auto play = [&](bool moved)
            {
                Rig rig;
                rig.bareMaster();
                auto* delay = rig.audio.getInsertChain(S2)->getEffect(0);
                int cell = -1;
                for (int i = 0; i < EffectBase::MAX_EXTRAS && cell < 0; ++i)
                    if (delay->getExtraSpec(i).isValid() && juce::String(delay->getExtraSpec(i).label) == "DRIV")
                        cell = i;
                expect(cell >= 0, "the delay has its tape stage's DRIV on FX2");
                if (moved && cell >= 0)
                    rig.audio.setTrackParameter(S2, EXTRA + cell, delay->getExtraSpec(cell).maxVal);
                rig.audio.setTrackParameter(1, SND + 1, 0.8f);
                rig.start();
                rig.chord();
                return rig.render(200);
            };
            const Stereo a = play(false), b = play(true);
            expect(level(minus(a, b)) > 0.01 * level(a), "moved by " + juce::String(level(minus(a, b)) / level(a)));
        }

        beginTest("a send's chain gone wrong (what is not a number): it is dropped on its own bus and emptied; the tracks play on through that very block");
        {
            // The same take with the second send's chain spoiled in block 120 and without: the
            // spoiled take is the other one's tracks in that block (what the send would have
            // returned is missing, nothing else).
            auto play = [&](bool spoil, Stereo& bad, int& dropped, int& emptied, int& where)
            {
                Rig rig(48000.0, 256, true);
                rig.bareMaster();
                auto spoiler = std::make_unique<Spoiler>();
                auto* seen = spoiler.get();
                rig.audio.getInsertChain(S2)->addEffect(std::move(spoiler));
                rig.audio.setTrackParameter(1, SND + 1, 0.5f);
                rig.start();
                rig.chord();
                rig.render(120);            // (the delay's first echo is back after 0.375 s)
                const int resetsBefore = seen->resets;
                if (spoil) seen->spoilAt = seen->blocks;
                bad = rig.render(1);
                const Stereo next = rig.render(8);
                dropped = rig.audio.getNonFiniteBlocks();
                emptied = seen->resets - resetsBefore;
                where = rig.audio.getNonFiniteTrack();
                expect(finite(next, 4.0f) && level(next) > 1.0e-3, "it plays on");
            };
            Stereo good, bad;
            int dropped = 0, emptied = 0, where = 0, none = 0;
            play(false, good, none, none, none);
            play(true, bad, dropped, emptied, where);
            expectEquals(dropped, 1);
            expectEquals(emptied, 1, "the send's memory is emptied");
            expectEquals(where, 2001, "counted against the second send");
            expect(finite(bad, 4.0f) && level(bad) > 1.0e-3, "the tracks are heard in the bad block: " + juce::String(level(bad)));
            expect(level(minus(good, bad)) > 1.0e-5, "what the send would have returned is not in it");
        }

        beginTest("the sum gone wrong (what is not a number, from the master chain): the sends' memory is emptied with the master chain's");
        {
            Rig rig(48000.0, 256, true);
            auto spoiler = std::make_unique<Spoiler>();
            auto counter = std::make_unique<Spoiler>();
            auto* bad = spoiler.get();
            auto* seen = counter.get();
            rig.bareMaster();
            rig.audio.getInsertChain(M)->addEffect(std::move(spoiler));
            rig.audio.getInsertChain(S1)->addEffect(std::move(counter));
            rig.audio.setTrackParameter(1, SND, 0.5f);
            rig.start();
            rig.chord();
            rig.render(30);
            const int resetsBefore = seen->resets;
            bad->spoilAt = bad->blocks;
            const Stereo silence = rig.render(1), next = rig.render(8);
            expectEquals(rig.audio.getNonFiniteBlocks(), 1);
            expectEquals(seen->resets, resetsBefore + 1, "the send's memory is emptied");
            expect(level(silence) == 0.0 && finite(next, 4.0f) && level(next) > 1.0e-3, "silence, then it plays on");
        }

        beginTest("an edit of a send under sound is carried off (the output's declick), not heard as a step");
        {
            // What the send returns is several times the track (an EQ with twelve decibels on three
            // bands); it is taken off the send where the sound stands far from zero.
            Rig rig(48000.0, 64);
            rig.bareMaster();
            rig.audio.setInsertEffect(S1, 0, EffectType::EQEffect);
            for (int i : { 0, 1, 3 }) rig.audio.setTrackParameter(S1, CELL + i, 12.0f);
            rig.audio.setTrackParameter(1, SND, 1.0f);
            rig.start();
            rig.keys->noteOn(33, 0.9f, -1);
            const Stereo before = rig.render(400);
            const float steady = largestStep(before, before.left.size() / 2, before.left.size());
            Stereo out;
            int waited = 0;
            do { out = rig.render(1); } while (std::abs(out.left.back()) < 0.5f * peakOf(before) && ++waited < 2000);
            rig.audio.removeInsertEffect(S1, 0);
            const Stereo after = rig.render(8);
            out.left.insert(out.left.end(), after.left.begin(), after.left.end());
            out.right.insert(out.right.end(), after.right.begin(), after.right.end());
            const float step = largestStep(out, 60, out.left.size());
            expect(waited < 2000 && step < 1.5f * steady, "the step at the edit is " + juce::String(step) + ", the sound's own largest " + juce::String(steady));
        }

        beginTest("a synced delay on a send follows the tempo (the sends are told it each block, as the inserts are)");
        {
            SequencerCore seq;
            Rig rig;
            rig.audio.setSequencer(&seq);
            seq.setTempo(120.0f);
            rig.audio.setTrackParameter(S2, DIV, (float) FxSync::DEFAULT_DIVISION);
            rig.audio.setTrackParameter(S2, SYNC, 1.0f);
            auto* delay = rig.audio.getInsertChain(S2)->getEffect(0);
            const int cell = delay->getSyncParamIndex();
            expect(cell >= 0 && cell < 4);
            rig.start();
            rig.render(2);
            const float at120 = delay->getParameter(cell);
            seq.setTempo(60.0f);
            rig.render(2);
            const float at60 = delay->getParameter(cell);
            expectWithinAbsoluteError(at120, 0.5f, 1.0e-3f);
            expectWithinAbsoluteError(at60, 1.0f, 1.0e-3f);
        }
    }

    // ---- 4. the pages ---------------------------------------------------------------------------------
    struct ScreenRig
    {
        SequencerCore seq;
        AudioEngine audio;
        TrackFxScreen fx;
        ScreenRig() : fx(seq, &audio)
        {
            for (int t = 0; t < 2; ++t)
            {
                audio.addEngine(t, AudioEngine::createEngine(EngineType::FM));
                seq.getPattern().getTrack(t).engineType = EngineType::FM;
            }
        }
        juce::StringArray pageNames()
        {
            juce::StringArray names;
            const int was = fx.getCurrentPageIndex();
            for (int i = 0; i < fx.getPageCount(); ++i)
            {
                fx.setCurrentPageIndex(i);
                names.add(fx.getPageName());
            }
            fx.setCurrentPageIndex(was);
            return names;
        }
        juce::StringArray labels()
        {
            juce::StringArray names;
            for (int i = 0; i < 8; ++i) names.add(fx.getEncoderName(i));
            return names;
        }
        void turn(int encoder, int clicks)
        {
            for (int i = 0; i < std::abs(clicks); ++i)
                fx.onEncoderChange(encoder, clicks > 0 ? 1 : 127);
        }
        size_t storedCount()
        {
            size_t n = 0;
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
            {
                n += seq.getPattern().getTrack(t).soundParams.size();
                for (auto& step : seq.getPattern().getTrack(t).steps)
                    n += step.paramLocks.size();
            }
            return n;
        }
    };

    struct UndoCatcher : Screen::UndoHost
    {
        Screen::UndoPoint point;
        std::vector<float> blob;
        int begun = 0, ended = 0;
        void undoBegin(const Screen::UndoPoint& p) override
        {
            if (begun++ == 0)
            {
                point = p;
                if (p.capture) blob = p.capture();
            }
        }
        void undoEnd() override { ++ended; }
        int undoSelectedTrack() const override { return 0; }
    };

    void testPages()
    {
        beginTest("TRK FX: two pages a send after the track's eight (S1FX S1FX2 S2FX S2FX2), reached by paging; the eight page keys are the track's");
        {
            ScreenRig rig;
            const juce::StringArray expected { "TRACK", "ARPLK", "FX", "FX2", "IN", "S1FX", "S1FX2", "S2FX", "S2FX2" };
            expect(rig.pageNames() == expected, rig.pageNames().joinIntoString(" "));
            expectEquals(rig.fx.getPageKeyCount(), 8);    // min(8, 9 pages)
            rig.fx.onPageKey(4);                             // IN is page key 4
            expectEquals(rig.fx.getPageName(), juce::String("IN"));
            rig.fx.onArrowRight();
            expectEquals(rig.fx.getPageName(), juce::String("S1FX"));
            rig.fx.onArrowRight();
            expectEquals(rig.fx.getPageName(), juce::String("S1FX2"));
            rig.fx.onArrowRight();
            rig.fx.onArrowRight();
            expectEquals(rig.fx.getPageName(), juce::String("S2FX2"));
            rig.fx.onArrowRight();
            expectEquals(rig.fx.getPageName(), juce::String("TRACK"), "round at the end");
            rig.fx.onArrowLeft();
            expectEquals(rig.fx.getPageName(), juce::String("S2FX2"));
            // a third send: its two pages
            rig.audio.addSendBus(EffectType::ChorusEffect);
            expect(rig.pageNames().contains("S3FX") && rig.pageNames().contains("S3FX2") && rig.fx.getPageCount() == 11);
        }

        beginTest("a send's page is the insert page: the cells FX shows for the same effect on a track, in the same places (reverb, the delay with SYNC, FX2)");
        {
            ScreenRig rig;
            rig.audio.addInsertEffect(0, EffectType::ReverbEffect);
            rig.audio.addInsertEffect(1, EffectType::DelayEffect);

            expect(rig.fx.selectPageByName("FX"));
            const auto trackReverb = rig.labels();
            expect(rig.fx.selectPageByName("FX2"));
            const auto trackReverb2 = rig.labels();
            expect(rig.fx.selectPageByName("S1FX"));
            expect(rig.labels() == trackReverb, rig.labels().joinIntoString(" ") + " / " + trackReverb.joinIntoString(" "));
            expectEquals(rig.labels()[0], juce::String("SLOT"));
            expectEquals(rig.labels()[1], juce::String("TYPE"));
            expectEquals(rig.labels()[3], juce::String("WET"));
            expect(rig.fx.selectPageByName("S1FX2"));
            expect(rig.labels() == trackReverb2, rig.labels().joinIntoString(" "));

            rig.seq.setSelectedTrack(1);
            expect(rig.fx.selectPageByName("FX"));
            const auto trackDelay = rig.labels();
            expect(rig.fx.selectPageByName("S2FX"));
            expect(rig.labels() == trackDelay, rig.labels().joinIntoString(" "));
            expectEquals(rig.labels()[2], juce::String("SYNC"));
            // the send's page does not follow the selected track
            rig.seq.setSelectedTrack(0);
            expect(rig.labels() == trackDelay, "the same send whatever track is selected");
        }

        beginTest("a cell with a text of its own gets it from its effect (TALK's VOWL: the vowel, then the place), on a track's FX page and on a send's alike; no other cell has one");
        {
            struct Open : TrackFxScreen
            {
                using TrackFxScreen::TrackFxScreen;
                juce::String own(int cell) const
                {
                    const auto spec = getCurrentPage().params[(size_t) cell];
                    return spec.isValid() ? cellText(spec, spec.clamp(displayValue(spec))) : juce::String();
                }
                bool goTo(const juce::String& name)
                {
                    for (int i = 0; i < getPageCount(); ++i)
                    {
                        setCurrentPageIndex(i);
                        if (getPageName() == name)
                            return true;
                    }
                    return false;
                }
            };
            for (bool talk : { true, false })
            {
                SequencerCore seq;
                AudioEngine audio;
                audio.addEngine(0, AudioEngine::createEngine(EngineType::FM));
                seq.getPattern().getTrack(0).engineType = EngineType::FM;
                const auto type = talk ? EffectType::TalkInsertEffect : EffectType::WahEffect;
                audio.setInsertEffect(0, 0, type);
                audio.setInsertEffect(S1, 0, type);
                audio.setTrackParameter(0, CELL + TalkInsert::Vowel, talk ? 0.5f : 800.0f);
                audio.setTrackParameter(S1, CELL + TalkInsert::Vowel, talk ? 1.0f : 800.0f);
                Open fx(seq, &audio);
                expect(fx.goTo("FX"));
                expect(fx.own(4) == (talk ? "I 50" : ""), "the track's FX page: \"" + fx.own(4) + "\"");
                for (int cell : { 5, 6, 7 })
                    expect(fx.own(cell).isEmpty(), "FX cell " + juce::String(cell) + ": \"" + fx.own(cell) + "\"");
                expect(fx.goTo("FX2"));
                for (int cell = 4; cell < 8; ++cell)
                    expect(fx.own(cell).isEmpty(), "FX2 cell " + juce::String(cell) + ": \"" + fx.own(cell) + "\"");
                expect(fx.goTo("S1FX"));
                expect(fx.own(4) == (talk ? "U 100" : ""), "the send's page: \"" + fx.own(4) + "\"");
                expect(fx.goTo("S1FX2"));
                for (int cell = 4; cell < 8; ++cell)
                    expect(fx.own(cell).isEmpty());
            }
            // The hook itself: empty on every cell of every effect but TALK's first.
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
                if (auto made = AudioEngine::createEffect((EffectType) t))
                    for (int cell = 0; cell < 8; ++cell)
                        expect(made->cellText(cell, 0.5f).isEmpty() == ! ((EffectType) t == EffectType::TalkInsertEffect && cell == 0),
                               juce::String(AudioEngine::effectTypeName((EffectType) t)) + " cell " + juce::String(cell));
        }

        beginTest("turning: a cell of the send's page moves the send's effect and nothing of any track (no stored value, no lock with a step held)");
        {
            ScreenRig rig;
            expect(rig.fx.selectPageByName("S1FX"));
            const size_t storedBefore = rig.storedCount();
            for (int cell = 0; cell < 4; ++cell)
            {
                const float was = rig.audio.getTrackParameter(S1, CELL + cell);
                rig.turn(4 + cell, -6);
                const float down = rig.audio.getTrackParameter(S1, CELL + cell);
                rig.turn(4 + cell, 12);
                const float up = rig.audio.getTrackParameter(S1, CELL + cell);
                expect(down != up, "cell " + juce::String(cell + 1) + " (" + rig.fx.getEncoderName(4 + cell) + "): " + juce::String(was)
                                       + " -> " + juce::String(down) + " -> " + juce::String(up));
            }
            // WET and ON
            rig.turn(3, -20);
            expect(rig.audio.getTrackParameter(S1, WET) < 0.95f, "WET " + juce::String(rig.audio.getTrackParameter(S1, WET)));
            if (rig.fx.getEncoderName(2) == "ON")
            {
                rig.turn(2, -1);
                expectEquals(rig.audio.getTrackParameter(S1, ON), 0.0f);
                rig.turn(2, 1);
                expectEquals(rig.audio.getTrackParameter(S1, ON), 1.0f);
            }
            // a held step: still the send's own value (a send has no steps), and no lock is written
            rig.fx.onPadPress(5, 100);
            expect(rig.fx.isStepHeld());
            const float before = rig.audio.getTrackParameter(S1, CELL + 0);
            rig.turn(4, 5);
            expect(rig.audio.getTrackParameter(S1, CELL + 0) != before, "with a step held the send's cell still turns");
            rig.fx.onPadRelease(5);
            expectEquals((int) rig.storedCount(), (int) storedBefore, "nothing of the send is kept in a track's sound or steps");

            // Track + turn (every track): once, not once a track
            const float one = rig.audio.getTrackParameter(S1, CELL + 0);
            rig.fx.onEncoderChangeAllTracks(4, 127);
            const float after = rig.audio.getTrackParameter(S1, CELL + 0);
            rig.fx.onEncoderChange(4, 1);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(S1, CELL + 0), one, 1.0e-5f, "one click down with every track, one click up alone: where it was");
            expect(after != one);
        }

        beginTest("SLOT and TYPE: a second and a third effect are put on the send from its page, one is exchanged, one is taken off; each send keeps its own SLOT");
        {
            ScreenRig rig;
            expect(rig.fx.selectPageByName("S1FX"));
            rig.turn(0, 1);                                                     // SLOT 2: the first empty one
            expectEquals(rig.fx.getSendSelectedSlot(0), 1);
            expect(rig.fx.getEncoderName(3) != "WET" && rig.fx.getEncoderName(4) == rig.fx.getEncoderName(3), "an empty slot has no cells: " + rig.labels().joinIntoString(" "));
            rig.turn(1, 1 + (int) EffectType::PhaserEffect);                    // TYPE: OFF -> ... -> PHS
            EffectType type {};
            expect(rig.audio.getInsertEffectType(S1, 1, type) && type == EffectType::PhaserEffect, "slot 2 holds the phaser");
            expect(rig.audio.getInsertEffectType(S1, 0, type) && type == EffectType::ReverbEffect, "slot 1 is untouched");
            expectEquals(rig.audio.getTrackParameter(S1, WET + 1), 1.0f);
            expectEquals(rig.fx.getEncoderName(3), juce::String("WET"));
            rig.turn(0, 5);                                                     // SLOT: no further than the first empty one
            expectEquals(rig.fx.getSendSelectedSlot(0), 2);
            rig.turn(1, 1 + (int) EffectType::CompressorEffect);
            expectEquals(rig.audio.getInsertChain(S1)->getEffectCount(), 3);
            // the cells beyond four of the third (the compressor's KEY is one): S1FX2 shows the same slot
            expect(rig.fx.selectPageByName("S1FX2"));
            expectEquals((int) std::lround(rig.audio.getTrackParameter(S1, EXTRA + 2 * 4 + 0)), 0);
            int keyCell = -1;
            for (int i = 4; i < 8; ++i)
                if (rig.fx.getEncoderName(i) == "KEY") keyCell = i;
            expect(keyCell >= 4, "KEY is on the send's second page: " + rig.labels().joinIntoString(" "));
            rig.turn(keyCell, 1);
            expectEquals(rig.audio.getInsertChain(S1)->getEffect(2)->keySource(), 0, "KEY T1: the send's compressor listens to track 1");
            // exchanged, and taken off
            rig.turn(0, -1);                                                    // SLOT 2
            rig.turn(1, 1);                                                     // the next type in the list
            expect(rig.audio.getInsertEffectType(S1, 1, type) && (int) type == (int) EffectType::PhaserEffect + 1);
            expectEquals(rig.audio.getInsertChain(S1)->getEffectCount(), 3);
            rig.turn(1, -(2 + (int) EffectType::PhaserEffect));                 // down to OFF
            expectEquals(rig.audio.getInsertChain(S1)->getEffectCount(), 2);
            expect(rig.audio.getInsertEffectType(S1, 1, type) && type == EffectType::CompressorEffect, "the third moved up");
            expect(rig.audio.getInsertEffectType(S1, 0, type) && type == EffectType::ReverbEffect, "the first stays (review)");
            // the other send's SLOT did not move, nor the track's
            expectEquals(rig.fx.getSendSelectedSlot(1), 0);
            expectEquals(rig.fx.getFxSelectedSlot(), 0);
            expectEquals(rig.audio.getInsertChain(S2)->getEffectCount(), 1);
        }

        beginTest("SYNC on the send delay's page: the TIME cell then chooses the division");
        {
            ScreenRig rig;
            expect(rig.fx.selectPageByName("S2FX"));
            auto* delay = rig.audio.getInsertChain(S2)->getEffect(0);
            const int cell = delay->getSyncParamIndex();
            expect(cell >= 0 && cell < 4);
            expectEquals(rig.fx.getEncoderName(2), juce::String("SYNC"));
            rig.turn(2, 1);
            expect(delay->isSynced(), "SYNC on");
            const int was = delay->getSyncDivision();
            rig.turn(4 + cell, 2);
            expectEquals(delay->getSyncDivision(), was + 2, "the cell turns the division");
            rig.turn(2, -1);
            expect(! delay->isSynced());
        }

        beginTest("BUS1 / BUS2 on the TRACK page are the type of the send's first effect: a view of the chain");
        {
            ScreenRig rig;
            rig.audio.addInsertEffect(S1, EffectType::PhaserEffect);
            expect(rig.fx.selectPageByName("TRACK"));
            expectEquals(rig.fx.getEncoderName(4), juce::String("BUS1"));
            rig.turn(4, 1);                                                     // RVB -> the next type
            EffectType type {};
            expect(rig.audio.getInsertEffectType(S1, 0, type) && (int) type == (int) EffectType::ReverbEffect + 1);
            expect(rig.audio.getSendBusType(0) == type);
            expect(rig.audio.getInsertEffectType(S1, 1, type) && type == EffectType::PhaserEffect, "what else is on the send stays");
            expectEquals(rig.audio.getTrackParameter(S1, WET), 1.0f);
            expect(rig.seq.getPattern().getTrack(0).soundParams.count(AudioEngine::SEND_TYPE_PARAM_BASE) == 0, "nothing of it in the track's sound");
        }

        beginTest("TYPE on a send is one undo step that puts the chain back as it was (cells, switches, the effects behind it)");
        {
            ScreenRig rig;
            UndoCatcher undo;
            rig.fx.setUndoHost(&undo);
            rig.audio.addInsertEffect(S1, EffectType::PhaserEffect);
            rig.audio.setTrackParameter(S1, CELL + 1, 0.83f);
            rig.audio.setTrackParameter(S1, WET + 1, 0.4f);
            const auto before = chainValues(rig.audio, S1);
            expect(rig.fx.selectPageByName("S1FX"));
            rig.fx.onEncoderChange(1, 1);
            expectEquals(undo.begun, 1);
            expectEquals(undo.ended, 1);
            expect(undo.point.scope == Screen::UndoPoint::Scope::Custom && undo.point.apply != nullptr);
            expect(undo.point.names.size() > 0 && undo.point.names[0].startsWith("SND1"), undo.point.names.joinIntoString("|"));
            expect(! near(before, chainValues(rig.audio, S1)), "the type was changed");
            if (undo.point.apply)
                undo.point.apply(undo.blob);
            expect(near(before, chainValues(rig.audio, S1)), "and put back");
        }
    }

    // ---- 5. stored ------------------------------------------------------------------------------------
    void testStored()
    {
        beginTest("a project keeps the sends' chains: types, cells, cells beyond four, ON, WET, SYNC and division come back, and the sound with them");
        {
            auto work = [&](AudioEngine& audio)
            {
                audio.setInsertEffect(S1, 0, EffectType::LCRDelayEffect);
                audio.addInsertEffect(S1, EffectType::PhaserEffect);
                audio.addInsertEffect(S1, EffectType::ReverbEffect);
                for (int slot = 0; slot < 3; ++slot)
                    for (int i = 0; i < 4; ++i)
                    {
                        auto* fx = audio.getInsertChain(S1)->getEffect(slot);
                        const ParamSpec a = fx->getParamSpec(i), b = fx->getExtraSpec(i);
                        if (a.isValid()) audio.setTrackParameter(S1, CELL + slot * 4 + i, a.clamp(a.minVal + (0.31f + 0.1f * (float) slot) * (a.maxVal - a.minVal)));
                        if (b.isValid()) audio.setTrackParameter(S1, EXTRA + slot * 4 + i, b.clamp(b.minVal + (0.62f - 0.1f * (float) slot) * (b.maxVal - b.minVal)));
                    }
                // (the phaser's LFO shape at 0.52 of its list is a random one, which no two phasers
                // play alike: a shape that is the same every time, so that the takes can be compared)
                audio.setTrackParameter(S1, EXTRA + 4 + 0, 1.0f);
                audio.setTrackParameter(S1, WET + 1, 0.45f);
                audio.setTrackParameter(S1, ON + 2, 0.0f);
                audio.setTrackParameter(S2, DIV, 5.0f);
                audio.setTrackParameter(S2, SYNC, 1.0f);
                audio.setTrackParameter(S2, EXTRA + 1, audio.getInsertChain(S2)->getEffect(0)->getExtraSpec(1).minVal);
            };
            SequencerCore seq1, seq2;
            Rig first(48000.0, 256, true), second(48000.0, 256, true);
            work(first.audio);
            ProjectBridge::Live live1 { seq1, first.audio }, live2 { seq2, second.audio };
            const juce::String json = ProjectBridge::capture(live1, "sends").toJSON();
            const ProjectData read = ProjectData::fromJSON(json);
            expectEquals((int) read.sendBuses.size(), 2);
            expect(read.sendBuses[0].hasChain && read.sendBuses[1].hasChain);
            expectEquals(read.sendBuses[0].effectType, (int) EffectType::LCRDelayEffect, "the first slot's type is still written for a build of before");
            second.audio.setInsertEffect(S2, 0, EffectType::WahEffect);         // (what the second had is gone after the load)
            ProjectBridge::apply(read, live2);
            second.start();
            second.render(4);           // (the load's own 2 ms, on the project's silent tracks)
            for (int chainTrack : { S1, S2 })
                expect(near(chainValues(first.audio, chainTrack), chainValues(second.audio, chainTrack)), "send " + juce::String(chainTrack - S1 + 1));
            expectEquals(second.audio.getInsertChain(S1)->getEffectCount(), 3);
            EffectType type {};
            expect(second.audio.getInsertEffectType(S1, 2, type) && type == EffectType::ReverbEffect);
            expect(second.audio.getInsertChain(S2)->getEffect(0)->isSynced() && second.audio.getInsertChain(S2)->getEffect(0)->getSyncDivision() == 5);
            expectEquals(second.audio.getTrackParameter(S1, ON + 2), 0.0f);
            expectWithinAbsoluteError(second.audio.getTrackParameter(S1, WET + 1), 0.45f, 1.0e-5f);

            // and the sound: the two instruments play the same take, sample for sample
            for (auto* rig : { &first, &second })
                for (int t = 0; t < 2; ++t)
                {
                    rig->audio.addEngine(t, t == 0 ? rig->kick : rig->keys);
                    rig->audio.setTrackParameter(t, MixParam::VOLUME, 0.8f);
                }
            const Stereo a = take(first, 2.0), b = take(second, 2.0);
            expect(sameSamples(a, b), "the saved sends and the loaded ones: " + juce::String((int) differing(a, b)) + " samples differ");
            expect(level(a) > 0.01);

            // saved again from what was loaded: the same file
            const juce::String again = ProjectBridge::capture(live2, "sends").toJSON();
            const ProjectData twice = ProjectData::fromJSON(again);
            expect(twice.sendBuses.size() == 2 && twice.sendBuses[0].chain.size() == read.sendBuses[0].chain.size()
                   && twice.sendBuses[1].chain.size() == read.sendBuses[1].chain.size());
            for (auto& [id, value] : read.sendBuses[0].chain)
                expectWithinAbsoluteError(twice.sendBuses[0].chain.count(id) ? twice.sendBuses[0].chain.at(id) : -999.0f, value,
                                          1.0e-4f * std::max(1.0f, std::abs(value)), "id " + juce::String(id));
        }

        beginTest("a send with nothing on it is stored as that, and a third send comes back; the next project without them has two again");
        {
            SequencerCore seq1, seq2;
            Rig first, second;
            first.audio.removeInsertEffect(S1, 0);
            first.audio.addSendBus(EffectType::SpringReverbEffect);
            ProjectBridge::Live live1 { seq1, first.audio }, live2 { seq2, second.audio };
            const ProjectData read = ProjectData::fromJSON(ProjectBridge::capture(live1, "sends").toJSON());
            ProjectBridge::apply(read, live2);
            expectEquals(second.audio.getSendCount(), 3);
            expectEquals(second.audio.getInsertChain(S1)->getEffectCount(), 0);
            EffectType type {};
            expect(second.audio.getInsertEffectType(S2, 0, type) && type == EffectType::DelayEffect);
            expect(second.audio.getInsertEffectType(S2 + 1, 0, type) && type == EffectType::SpringReverbEffect);
            ProjectBridge::apply(ProjectData(), live2);
            expectEquals(second.audio.getSendCount(), 2);
            expect(second.audio.getInsertEffectType(S1, 0, type) && type == EffectType::ReverbEffect);
        }

        beginTest("a damaged file: values that are not numbers, types that do not exist, ids that are not a slot's: nothing but the chain is touched, and it sounds");
        {
            ProjectData data;
            SendBusData bus;
            bus.hasChain = true;
            bus.chain[ProjectBridge::INSERT_TYPE_BASE + 0] = 1.0e9f;
            bus.chain[ProjectBridge::INSERT_TYPE_BASE + 1] = std::numeric_limits<float>::infinity();
            bus.chain[ProjectBridge::INSERT_TYPE_BASE + 2] = 4.0f;
            bus.chain[CELL + 0] = std::numeric_limits<float>::quiet_NaN();
            bus.chain[CELL + 4] = 1.0e30f;
            bus.chain[CELL + 5] = -1.0e30f;
            bus.chain[EXTRA + 8] = std::numeric_limits<float>::infinity();
            bus.chain[WET + 1] = 77.0f;
            bus.chain[DIV + 1] = -5.0f;
            bus.chain[(int) MixParam::VOLUME] = 0.0f;
            bus.chain[5] = 0.5f;
            data.sendBuses.push_back(bus);
            SequencerCore seq;
            Rig rig(48000.0, 256, true);
            ProjectBridge::Live live { seq, rig.audio };
            ProjectBridge::apply(data, live);
            expectEquals(rig.audio.getInsertChain(S1)->getEffectCount(), 3);
            EffectType type {};
            expect(rig.audio.getInsertEffectType(S1, 0, type) && (int) type == (int) EffectType::NumEffectTypes - 1);
            expect(rig.audio.getInsertEffectType(S1, 2, type) && (int) type == 3);
            expectEquals(rig.audio.getTrackParameter(S1, WET + 1), 1.0f);
            rig.audio.addEngine(0, rig.kick);
            rig.audio.addEngine(1, rig.keys);
            rig.audio.setTrackParameter(1, MixParam::VOLUME, 0.8f);
            rig.audio.setTrackParameter(1, SND, 0.8f);
            rig.start();
            rig.chord();
            const Stereo s = rig.render(80);
            expect(finite(s, 30.0f) && level(s) > 0.01);
            expectEquals(rig.audio.getNonFiniteBlocks(), 0);
        }
    }

    // ---- cost -------------------------------------------------------------------------------------------
    // NEPTUNE_SENDS_BENCH=1: microseconds per block of 512 samples at 48 kHz, the engine of before
    // against this one in this one binary, each the best of several runs taken in turn.
    // A source that costs next to nothing (the sends and the master are what is measured): a
    // decaying buzz struck again every 6000 samples.
    struct Buzz : Tone
    {
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            float* l = buffer.getWritePointer(0);
            float* r = buffer.getWritePointer(1);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                if (++count % 6000 == 0) amp = 0.5f;
                phase += 0.011f;
                if (phase >= 1.0f) phase -= 2.0f;
                amp *= 0.9996f;
                l[i] = phase * amp;
                r[i] = -phase * amp;
            }
        }
        int count = 0;
        float phase = 0.0f, amp = 0.5f;
    };

    // ---- 6. the review's additions --------------------------------------------------------------------
    // An effect that counts the samples it is run on.
    struct Counter : EffectBase
    {
        long samples = 0;
        void reset() override {}
        void process(float&, float&) override { ++samples; }
        void processBlock(float*, float*, int n) override { samples += n; }
    };

    // Every insert id (and its neighbours) set on tracks with and without a chain, on the master
    // chain and on indices that are nobody's; what every id then reads, the type of every slot, and
    // a take. The same on the engine of before and on this one.
    template <typename Engine>
    static std::pair<std::vector<float>, Stereo> foldTake()
    {
        RigOf<Engine> rig(48000.0, 256, true);
        auto& a = rig.audio;
        a.addInsertEffect(0, EffectType::DelayEffect);
        a.addInsertEffect(0, EffectType::AmpEffect);
        a.addInsertEffect(1, EffectType::EQEffect);
        a.addInsertEffect(1, EffectType::LCRDelayEffect);
        a.addInsertEffect(1, EffectType::CompressorEffect);
        a.setInsertEffect(1, 0, EffectType::DriveEffect);
        a.removeInsertEffect(1, 1);
        a.addInsertEffect(M, EffectType::TapeDelayEffect);
        a.setInsertEffect(M, 9, EffectType::DelayEffect);       // (past the end: appended)
        const int chains[] = { 0, 1, 2, 31, M, -1, AudioEngine::SEND_TRACK_BASE + AudioEngine::MAX_SENDS };
        for (int t : chains)
            for (int id = 150; id < 400; ++id)
            {
                if (TrackInParam::contains(id) || TrackFxParam::isTrackFxParam(id))
                    continue;       // (the track's input is another matter; TrackFx params are dead since DIRT/TALK/MODFX became inserts)
                const float x = (float) id * 0.6180339f + (float) t * 0.113f;
                a.setTrackParameter(t, id, x - std::floor(x));
            }
        std::vector<float> values;
        for (int t : chains)
        {
            for (int id = 150; id < 400; ++id)
            {
                if (TrackInParam::contains(id) || TrackFxParam::isTrackFxParam(id))
                    continue;
                values.push_back(a.getTrackParameter(t, id));
            }
            for (int slot = -1; slot <= AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
            {
                EffectType type {};
                values.push_back(a.getInsertEffectType(t, slot, type) ? (float) (int) type : -1.0f);
            }
        }
        return { values, take(rig, 1.5) };
    }

    void testReview()
    {
        beginTest("review: the insert ids of a TRACK and of the MASTER chain answer as they did before the three branches became one (every id 150..399 set and read, the types of the slots, the take with inserts on both tracks and five effects on the mix: bit for bit)");
        {
            const auto before = foldTake<OldSendsAudioEngine>();
            const auto now = foldTake<AudioEngine>();
            expectEquals((int) now.first.size(), (int) before.first.size());
            int differ = 0, firstAt = -1;
            for (size_t i = 0; i < std::min(now.first.size(), before.first.size()); ++i)
                if (std::memcmp(&now.first[i], &before.first[i], sizeof(float)) != 0)
                {
                    if (firstAt < 0) firstAt = (int) i;
                    ++differ;
                }
            expectEquals(differ, 0, "values that read differently; the first at " + juce::String(firstAt));
            expect(sameSamples(before.second, now.second), juce::String((int) differing(before.second, now.second)) + " samples differ");
            expect(level(now.second) > 0.01 && finite(now.second, 8.0f));
        }

        beginTest("review: a block shorter than the prepared one: a send's chain is run on the samples played, not on the prepared length (its time does not run fast)");
        {
            Rig rig(48000.0, 512);
            auto counter = std::make_unique<Counter>();
            Counter* count = counter.get();
            rig.audio.getInsertChain(S1)->addEffect(std::move(counter));
            rig.audio.setTrackParameter(1, SND, 0.5f);
            rig.start();
            rig.chord();
            for (int b = 0; b < 10; ++b)
                rig.audio.renderBlock(rig.l.data(), rig.r.data(), 64);
            expectEquals((int) count->samples, 640);
            rig.audio.renderBlock(rig.l.data(), rig.r.data(), 512);
            expectEquals((int) count->samples, 640 + 512);

            // and the sound: the send delay's echo of a chord comes when it should (0.375 s after
            // it), in blocks of 64 as in blocks of 512
            auto echoAt = [&](int played)
            {
                Stereo out[2];
                for (int withSend = 0; withSend < 2; ++withSend)
                {
                    Rig r2(48000.0, 512, false);
                    r2.bareMaster();
                    r2.audio.setTrackParameter(1, SND + 1, withSend ? 1.0f : 0.0f);
                    r2.audio.setTrackParameter(S2, CELL + 1, 0.0f);     // no feedback: one echo
                    r2.start();
                    r2.chord();
                    for (int done = 0; done < 24000; done += played)
                    {
                        r2.audio.renderBlock(r2.l.data(), r2.r.data(), played);
                        out[withSend].left.insert(out[withSend].left.end(), r2.l.begin(), r2.l.begin() + played);
                        out[withSend].right.insert(out[withSend].right.end(), r2.r.begin(), r2.r.begin() + played);
                    }
                }
                const Stereo echo = minus(out[1], out[0]);
                size_t first = 0;
                while (first < echo.left.size() && std::abs(echo.left[first]) < 1.0e-3f) ++first;
                return (int) first;
            };
            const int whole = echoAt(512), pieces = echoAt(64);
            expect(std::abs(whole - 18000) < 600, "blocks of 512: the first echo at sample " + juce::String(whole));
            expect(std::abs(pieces - 18000) < 600, "blocks of 64: the first echo at sample " + juce::String(pieces) + ", in blocks of 512 at " + juce::String(whole));
        }

        beginTest("review: copy / clear / paste on a send's page act on that send's chain, through the real ScreenManager (the track's effects are not touched); each clear and paste is undone from the history; with a step held they are the step's locks");
        {
            helptest::Rig r;
            r.fill();
            r.audio.addInsertEffect(0, EffectType::ChorusEffect);
            r.track(0).soundParams[ProjectBridge::INSERT_TYPE_BASE] = (float) ((int) EffectType::ChorusEffect + 1);
            r.audio.addInsertEffect(S1, EffectType::CompressorEffect);
            r.audio.setTrackParameter(S1, CELL + 1, 0.83f);
            r.audio.setTrackParameter(S1, WET + 1, 0.4f);
            r.mgr.setActiveScreen(helptest::TRKFX);
            auto* fx = dynamic_cast<TrackFxScreen*>(r.mgr.getActiveScreen());
            expect(fx != nullptr);
            if (fx == nullptr) return;
            const auto trackFxVals = [&] { return chainValues(r.audio, 0); };
            const auto track0 = trackFxVals(), send1 = chainValues(r.audio, S1), send2 = chainValues(r.audio, S2);
            const auto chord = [&](int cc, bool withShift = false)
            {
                r.func(true);
                if (withShift) r.shift(true);
                r.tap(cc);
                if (withShift) r.shift(false);
                r.func(false);
            };

            // the track's own pages: the track's effects, as before
            expect(fx->selectPageByName("FX"));
            r.func(true);
            r.wait();
            expect(r.card(helptest::Key::Func, 0).lines[1].text.contains("T1 FX"), r.card(helptest::Key::Func, 0).lines[1].text);
            r.func(false);

            // a send's page: the card names the send, and clear empties the send alone
            expect(fx->selectPageByName("S1FX"));
            r.func(true);
            r.wait();
            expect(r.card(helptest::Key::Func, 0).lines[1].text.contains("SND1"), r.card(helptest::Key::Func, 0).lines[1].text);
            r.func(false);
            chord(helptest::RECORD);                                // copy send 1
            expect(r.text().contains("COPY SND1"), r.text());
            r.turn(0, 1);                                           // (another control in between: the same chord again is not "take it back")
            r.turn(0, -1);
            chord(helptest::PLAY);                                  // clear send 1
            expect(r.text().contains("CLEAR SND1"), r.text());
            expectEquals(r.audio.getInsertChain(S1)->getEffectCount(), 0);
            expect(near(track0, trackFxVals()), "the track's effects stay");
            expect(near(send2, chainValues(r.audio, S2)), "the other send stays");
            chord(helptest::PLAY, true);                            // undo
            expect(r.text().contains("UNDO CLEAR SND1"), r.text());
            expect(near(send1, chainValues(r.audio, S1)), "undo: the chain as it was");
            chord(helptest::RECORD, true);                          // redo
            expectEquals(r.audio.getInsertChain(S1)->getEffectCount(), 0);
            chord(helptest::PLAY, true);
            expect(near(send1, chainValues(r.audio, S1)));

            // paste on the other send: send 1's chain there; undone, the delay again
            expect(fx->selectPageByName("S2FX2"));
            chord(helptest::PATTERN_KEY);
            expect(r.text().contains("PASTE SND2"), r.text());
            expect(near(send1, chainValues(r.audio, S2)), "send 2 holds what was copied on send 1");
            expect(near(send1, chainValues(r.audio, S1)) && near(track0, trackFxVals()));
            chord(helptest::PLAY, true);
            expect(near(send2, chainValues(r.audio, S2)), "undo: send 2 as it was");

            // TYPE on a send through the real history
            expect(fx->selectPageByName("S1FX"));
            r.turn(1, 1);
            expect(! near(send1, chainValues(r.audio, S1)), "the type was changed");
            chord(helptest::PLAY, true);
            expect(r.text().contains("UNDO SET SND1"), r.text());
            expect(near(send1, chainValues(r.audio, S1)), "undo of TYPE: the chain as it was");

            // Shift fine, Func coarse on a send's cell (WET: 1 % plain, 0.1 % fine, the quarters coarse)
            const auto wet = [&] { return r.audio.getTrackParameter(S1, WET); };
            r.turn(3, -1);
            expectWithinAbsoluteError(wet(), 0.99f, 1.0e-4f);
            r.shift(true); r.turn(3, -1); r.shift(false);
            expectWithinAbsoluteError(wet(), 0.989f, 1.0e-4f);
            r.func(true); r.turn(3, -1); r.func(false);
            expectWithinAbsoluteError(wet(), 0.75f, 1.0e-4f);
            r.audio.setTrackParameter(S1, WET, 1.0f);

            // a step held on the send's page: the three are that step's locks, as on every page
            r.padDown(0);
            r.wait(1000);
            chord(helptest::PLAY);
            expect(r.text().contains("CLEAR S1 L"), r.text());
            r.padUp(0);
            expect(near(send1, chainValues(r.audio, S1)), "the send stays when a step is held");
            expect(r.track(0).getStep(0).paramLocks.empty(), "the held step's locks were cleared");
            expect(near(track0, trackFxVals()));
        }

        beginTest("review: the same through the real history: a scroll of three clicks through a send's TYPE list is one undo step; BUS1 exchanged is one too; the help cards offer no pressure range and no every-track turn on a send's page");
        {
            helptest::Rig r;
            r.mgr.setActiveScreen(helptest::TRKFX);
            auto* fx = dynamic_cast<TrackFxScreen*>(r.mgr.getActiveScreen());
            expect(fx != nullptr);
            if (fx == nullptr) return;
            const auto undo = [&] { r.func(true); r.shift(true); r.tap(helptest::PLAY); r.shift(false); r.func(false); };
            r.audio.setTrackParameter(S1, CELL + 0, 0.77f);
            const auto send1 = chainValues(r.audio, S1);
            expect(fx->selectPageByName("S1FX"));
            r.turn(1, 1);
            r.turn(1, 1);
            r.turn(1, 1);
            EffectType type {};
            expect(r.audio.getInsertEffectType(S1, 0, type) && type != EffectType::ReverbEffect);
            undo();
            expect(near(send1, chainValues(r.audio, S1)), "one undo takes the whole scroll back: " + r.text());

            expect(fx->selectPageByName("TRACK"));
            r.turn(4, 1);                                           // BUS1
            expect(! near(send1, chainValues(r.audio, S1)), "BUS1 exchanged the send's first effect");
            undo();
            expect(near(send1, chainValues(r.audio, S1)), "BUS1's exchange is undone: " + r.text());

            Screen::HelpFacts facts;
            expect(fx->selectPageByName("FX"));
            fx->getHelpFacts(facts);
            expect(facts.pressureRange && facts.allTracksTurn);
            expect(fx->selectPageByName("S2FX"));
            fx->getHelpFacts(facts);
            expect(! facts.pressureRange && ! facts.allTracksTurn, "a send's page: no pressure range, no every-track turn on the cards");
        }

        beginTest("review: a third send added while prepared is heard; a send taken from the middle leaves the others theirs; WET is held within 0..1; a send's SLOT stays within what is on it; BUS1 and SND1 of an emptied send");
        {
            // a third send, added after prepare: what track 2 sends to it comes back
            auto third = [&](float sent)
            {
                Rig rig;
                rig.bareMaster();
                rig.start();
                expectEquals(rig.audio.addSendBus(EffectType::DriveEffect), 2);
                rig.audio.setTrackParameter(1, SND + 2, sent);
                rig.chord();
                return rig.render(60);
            };
            const Stereo without = third(0.0f), with = third(1.0f);
            expect(level(minus(with, without)) > 0.1 * level(without), "the third send returns: " + juce::String(level(minus(with, without))) + " beside " + juce::String(level(without)));

            // one taken from the middle
            AudioEngine audio;
            audio.prepare(48000.0, 128);
            audio.addSendBus(EffectType::ChorusEffect);
            audio.removeSendBus(1);
            EffectType type {};
            expectEquals(audio.getSendCount(), 2);
            expect(audio.getInsertEffectType(S1, 0, type) && type == EffectType::ReverbEffect, "the first send is still the reverb");
            expect(audio.getInsertEffectType(S2, 0, type) && type == EffectType::ChorusEffect, "the third moved down");
            audio.removeSendBus(7);
            audio.removeSendBus(-1);
            expectEquals(audio.getSendCount(), 2);

            // WET
            for (int chain : { S1, M })
            {
                audio.setTrackParameter(chain, WET, 1.7f);
                expectEquals(audio.getTrackParameter(chain, WET), 1.0f);
                audio.setTrackParameter(chain, WET, -0.5f);
                expectEquals(audio.getTrackParameter(chain, WET), 0.0f);
            }

            // SLOT after effects went another way than by the page
            ScreenRig rig;
            rig.audio.addInsertEffect(S1, EffectType::ChorusEffect);
            rig.audio.addInsertEffect(S1, EffectType::DriveEffect);
            expect(rig.fx.selectPageByName("S1FX"));
            rig.turn(0, 3);
            expectEquals(rig.fx.getSendSelectedSlot(0), 3, "the first empty slot");
            rig.audio.removeInsertEffect(S1, 0);
            rig.audio.removeInsertEffect(S1, 0);
            expectEquals(rig.fx.getSendSelectedSlot(0), 1, "held within the chain: the first empty slot");
            expectEquals(rig.fx.getEncoderName(1), juce::String("TYPE"));

            // an emptied send: BUS1 says OFF, the send level's cell keeps its plain name
            rig.audio.removeInsertEffect(S1, 0);
            expect(rig.fx.selectPageByName("TRACK"));
            expect(rig.fx.getCellReadout(4).contains("OFF"), rig.fx.getCellReadout(4));
            expectEquals(rig.fx.getEncoderName(2), juce::String("SND1"));
            expect(! rig.fx.getCellReadout(5).contains("OFF"), rig.fx.getCellReadout(5));
            expect(rig.fx.getEncoderName(3) != juce::String("SND2"), "send 2 is named by its effect: " + rig.fx.getEncoderName(3));
        }

        beginTest("review: a project with three sends, a keyed effect on one and one emptied, loaded twice and over other sends: the same chains each time, and it plays");
        {
            SequencerCore seq1, seq2;
            Rig first, second;
            auto& a = first.audio;
            a.addInsertEffect(S1, EffectType::CompressorEffect);
            a.setTrackParameter(S1, CELL + 4 + 0, -40.0f);
            a.setTrackParameter(S1, EXTRA + 4 + 0, 1.0f);       // KEY at T1
            a.removeInsertEffect(S2, 0);
            expectEquals(a.addSendBus(EffectType::TapeDelayEffect), 2);
            a.setTrackParameter(S2 + 1, WET, 0.6f);
            a.setTrackParameter(S2 + 1, SYNC, 1.0f);
            ProjectBridge::Live live1 { seq1, a }, live2 { seq2, second.audio };
            const ProjectData read = ProjectData::fromJSON(ProjectBridge::capture(live1, "three").toJSON());
            const auto want1 = chainValues(a, S1), want2 = chainValues(a, S2), want3 = chainValues(a, S2 + 1);
            expectEquals(a.getInsertChain(S1)->getEffect(1)->keySource(), 0);

            second.start();
            second.chord();
            second.audio.setTrackParameter(1, SND, 0.8f);
            second.render(40);
            for (int pass = 0; pass < 3; ++pass)
            {
                if (pass == 2)      // (other sends in between: four, all of another kind)
                {
                    second.audio.setInsertEffect(S1, 0, EffectType::ChorusEffect);
                    second.audio.addSendBus(EffectType::DriveEffect);
                }
                ProjectBridge::apply(read, live2);
                expectEquals(second.audio.getSendCount(), 3);
                expect(near(want1, chainValues(second.audio, S1)) && near(want2, chainValues(second.audio, S2))
                       && near(want3, chainValues(second.audio, S2 + 1)), "load " + juce::String(pass + 1));
                expectEquals(second.audio.getInsertChain(S1)->getEffect(1)->keySource(), 0, "the key comes back");
                second.audio.addEngine(1, second.keys);
                second.audio.setTrackParameter(1, MixParam::VOLUME, 0.8f);
                second.audio.setTrackParameter(1, SND, 0.8f);
                second.audio.setTrackParameter(1, SND + 2, 0.8f);
                expect(finite(second.render(40), 8.0f));
            }
            expectEquals(second.audio.getNonFiniteBlocks(), 0);
        }

        beginTest("review: the sends worked on from another thread while blocks are played (add / remove a bus, set / add / remove an effect, reset): the blocks are numbers, a block that could not have the structure is silence, and the sends end as they began");
        {
            Rig rig(48000.0, 128, true);
            rig.audio.setTrackParameter(1, SND, 0.5f);
            rig.audio.setTrackParameter(1, SND + 1, 0.5f);
            rig.audio.setTrackParameter(0, SND + 2, 0.5f);
            rig.start();
            rig.chord();
            std::atomic<bool> done { false };
            std::thread worker([&]
            {
                auto& a = rig.audio;
                for (int i = 0; i < 30; ++i)
                {
                    a.setInsertEffect(S1, 0, i % 2 == 0 ? EffectType::ChorusEffect : EffectType::DelayEffect);
                    const int bus = a.addSendBus(EffectType::TremoloEffect);
                    a.addInsertEffect(S2, EffectType::DriveEffect);
                    a.setTrackParameter(S2, WET + 1, 0.5f);
                    if (bus >= 0)
                        a.removeSendBus(bus);
                    a.removeInsertEffect(S2, 1);
                    if (i % 10 == 9)
                        a.resetSendBuses();
                }
                a.resetSendBuses();
                done.store(true);
            });
            bool numbers = true;
            int blocks = 0;
            while (! done.load() || blocks < 20)
            {
                rig.audio.renderBlock(rig.l.data(), rig.r.data(), 128);
                for (int i = 0; i < 128; ++i)
                    numbers = numbers && std::abs(rig.l[(size_t) i]) <= 8.0f && std::abs(rig.r[(size_t) i]) <= 8.0f;
                ++blocks;
                // A real audio thread rests between its blocks; without a rest this loop takes the structure
                // lock again at once and the worker waits behind it for seconds (the test then took 25-50 s
                // alone and minutes on a busy machine, whatever the number of rounds).
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
            worker.join();
            expect(numbers, "every block played is numbers within reason");
            expectEquals(rig.audio.getNonFiniteBlocks(), 0);
            expectEquals(rig.audio.getSendCount(), 2);
            EffectType t0 {}, t1 {};
            expect(rig.audio.getInsertEffectType(S1, 0, t0) && t0 == EffectType::ReverbEffect
                   && rig.audio.getInsertEffectType(S2, 0, t1) && t1 == EffectType::DelayEffect);
        }

        beginTest("review (2): a send whose one effect is OFF, or at WET 0, returns silence: no level added to the tracks alone");
        {
            auto play = [&](int kind, float sent)
            {
                Rig rig;
                rig.bareMaster();
                if (kind == 1) rig.audio.setTrackParameter(S1, ON, 0.0f);
                if (kind == 2) rig.audio.setTrackParameter(S1, WET, 0.0f);
                rig.audio.setTrackParameter(1, SND, sent);
                rig.start();
                rig.chord();
                return rig.render(60);
            };
            for (float sent : { 0.5f, 1.0f })
            {
                const Stereo dryOut = play(0, 0.0f), offOut = play(1, sent), wet0Out = play(2, sent);
                expect(sameSamples(dryOut, offOut), "effect OFF at SND " + juce::String(sent, 2) + ": the tracks alone, " + juce::String((int) differing(dryOut, offOut)) + " samples differ");
                expect(sameSamples(dryOut, wet0Out), "WET 0 at SND " + juce::String(sent, 2) + ": the tracks alone, " + juce::String((int) differing(dryOut, wet0Out)) + " samples differ");
            }
        }

        beginTest("review (2): a send's return rises smoothly as WET leaves zero: monotonic from silence, no jump at any WET");
        {
            // Measure the returned level at WET 0, 0.01, 0.02, ..., 1.0 on the reverb send.
            // The level must start at zero and rise monotonically (no step, no dip).
            std::vector<double> levels;
            const int steps = 101;
            for (int w = 0; w < steps; ++w)
            {
                const float wet = (float) w / (float) (steps - 1);
                Rig rig;
                rig.bareMaster();
                rig.audio.setTrackParameter(S1, WET, wet);
                rig.audio.setTrackParameter(1, SND, 0.5f);
                rig.start();
                rig.chord();
                // A short render: enough for the reverb to have output.
                const Stereo out = rig.render(40);
                // The returned level = level of (output with send) - (output without send).
                // Since the master is bare and the drum is silent, the output is keys + send return.
                levels.push_back(level(out, 1024, out.left.size()));
            }
            // At WET 0: the level should be just the keys; at WET 1: keys + reverb tail.
            const double atZero = levels[0];
            expect(levels.back() > atZero * 1.01, "the send adds something at WET 1: " + juce::String(levels.back()) + " vs " + juce::String(atZero));
            // Monotonic: each step's level is at least the previous one (within tolerance for
            // measurement noise).
            bool monotonic = true;
            int failAt = -1;
            double failDrop = 0.0;
            for (int i = 1; i < steps; ++i)
            {
                // Allow tiny decreases from measurement noise (0.1% of the signal)
                if (levels[(size_t) i] < levels[(size_t) (i - 1)] - 0.001 * levels.back())
                {
                    monotonic = false;
                    failAt = i;
                    failDrop = levels[(size_t) (i - 1)] - levels[(size_t) i];
                    break;
                }
            }
            expect(monotonic, "not monotonic at step " + juce::String(failAt) + " (WET " + juce::String((float) failAt / (float) (steps - 1), 2)
                              + "): dropped by " + juce::String(failDrop));
            // No jump: the largest step in level between adjacent WET values is small compared
            // to the full range.
            double maxStep = 0.0;
            for (int i = 1; i < steps; ++i)
                maxStep = std::max(maxStep, std::abs(levels[(size_t) i] - levels[(size_t) (i - 1)]));
            const double range = levels.back() - levels[0];
            expect(range > 0 && maxStep < 0.1 * range, "largest step " + juce::String(maxStep) + " is small against the range " + juce::String(range));
        }

        beginTest("review (2): switching a send's effect ON / OFF under sound: no click (the step is bounded by the presence fade)");
        {
            Rig rig(48000.0, 64);
            rig.bareMaster();
            rig.audio.setTrackParameter(1, SND, 0.5f);
            rig.start();
            rig.keys->noteOn(33, 0.9f, -1);
            const Stereo steady = rig.render(400);
            const float steadyStep = largestStep(steady, steady.left.size() / 2, steady.left.size());
            // Switch OFF under sound.
            rig.audio.setTrackParameter(S1, ON, 0.0f);
            const Stereo offBlock = rig.render(8);
            const float offStep = largestStep(offBlock, 2, offBlock.left.size());
            // Switch back ON.
            rig.audio.setTrackParameter(S1, ON, 1.0f);
            const Stereo onBlock = rig.render(8);
            const float onStep = largestStep(onBlock, 2, onBlock.left.size());
            // The step at the switch should be bounded, not a hard cut.
            expect(offStep < 3.0f * steadyStep, "OFF step " + juce::String(offStep) + " vs steady " + juce::String(steadyStep));
            expect(onStep < 3.0f * steadyStep, "ON step " + juce::String(onStep) + " vs steady " + juce::String(steadyStep));
        }

        beginTest("review (2): the sends put back while the engine is prepared and playing (resetSendBuses) sound as a fresh engine's, sample for sample");
        {
            auto play = [&](bool worked)
            {
                Rig rig(48000.0, 256, true);
                rig.bareMaster();
                rig.start();
                if (worked)
                {
                    rig.audio.setTrackParameter(1, SND, 0.6f);
                    rig.chord();
                    rig.render(20);
                    rig.audio.setInsertEffect(S1, 0, EffectType::HallReverbEffect);
                    rig.audio.addInsertEffect(S2, EffectType::ChorusEffect);
                    rig.audio.setTrackParameter(S2, CELL + 1, 0.9f);
                    rig.render(20);
                    // (fresh tones in place of the ones that played: only the sends are compared)
                    rig.audio.addEngine(0, rig.kick = std::make_shared<Tone>(Tone::Kick));
                    rig.audio.addEngine(1, rig.keys = std::make_shared<Tone>(Tone::Keys));
                    rig.audio.resetSendBuses();
                }
                rig.audio.setTrackParameter(1, SND, 0.6f);
                rig.audio.setTrackParameter(1, SND + 1, 0.6f);
                rig.render(8);      // (the output's declick of the reset, on silence)
                rig.chord();
                return rig.render(120);
            };
            const Stereo fresh = play(false), again = play(true);
            expect(level(fresh) > 0.01);
            expect(sameSamples(fresh, again), juce::String((int) differing(fresh, again)) + " samples differ");
        }

        beginTest("review (2): a send taken away under sound is carried off by the output's declick, not heard as a step");
        {
            Rig rig(48000.0, 64);
            rig.bareMaster();
            expectEquals(rig.audio.addSendBus(EffectType::EQEffect), 2);
            for (int i : { 0, 1, 3 }) rig.audio.setTrackParameter(S2 + 1, CELL + i, 12.0f);
            rig.audio.setTrackParameter(1, SND + 2, 1.0f);
            rig.start();
            rig.keys->noteOn(33, 0.9f, -1);
            const Stereo before = rig.render(400);
            const float steady = largestStep(before, before.left.size() / 2, before.left.size());
            Stereo out;
            int waited = 0;
            do { out = rig.render(1); } while (std::abs(out.left.back()) < 0.5f * peakOf(before) && ++waited < 2000);
            rig.audio.removeSendBus(2);
            const Stereo after = rig.render(8);
            out.left.insert(out.left.end(), after.left.begin(), after.left.end());
            out.right.insert(out.right.end(), after.right.begin(), after.right.end());
            const float step = largestStep(out, 60, out.left.size());
            expect(waited < 2000 && step < 1.5f * steady, "the step at the removal is " + juce::String(step) + ", the sound's own largest " + juce::String(steady));
        }

        beginTest("review (2): a send taken away and the sends put back while prepared: the bus that comes back is prepared and heard; effects on sends make no track chains");
        {
            auto play = [&](bool removed)
            {
                Rig rig;
                rig.bareMaster();
                rig.start();
                if (removed)
                {
                    rig.audio.removeSendBus(1);
                    expectEquals(rig.audio.getSendCount(), 1);
                    rig.audio.resetSendBuses();
                }
                rig.audio.setTrackParameter(1, SND + 1, 1.0f);
                rig.render(8);
                rig.chord();
                return rig.render(150);
            };
            const Stereo fresh = play(false), back = play(true);
            expect(level(fresh) > 0.01);
            expect(sameSamples(fresh, back), "the delay send after it came back: " + juce::String((int) differing(fresh, back)) + " samples differ");

            AudioEngine audio;
            audio.prepare(48000.0, 128);
            audio.addInsertEffect(S1, EffectType::ChorusEffect);
            audio.setInsertEffect(S2, 1, EffectType::DriveEffect);
            expect(audio.getInsertChain(0) == nullptr && audio.getInsertChain(AudioEngine::MAX_TRACKS - 1) == nullptr,
                   "a send's effect gives no track an insert chain");
        }

        beginTest("review (2): paste on a send's page with nothing copied there says so, changes nothing and is no undo step");
        {
            helptest::Rig r;
            r.mgr.setActiveScreen(helptest::TRKFX);
            auto* fx = dynamic_cast<TrackFxScreen*>(r.mgr.getActiveScreen());
            expect(fx != nullptr);
            if (fx == nullptr) return;
            const auto send1 = chainValues(r.audio, S1);
            expect(fx->selectPageByName("S1FX"));
            r.func(true); r.tap(helptest::PATTERN_KEY); r.func(false);
            expect(r.text().contains("NOTHING COPIED") || r.text().contains("NONE COPIED"), r.text());
            expect(near(send1, chainValues(r.audio, S1)));
            r.somethingElse();
            r.func(true); r.shift(true); r.tap(helptest::PLAY); r.shift(false); r.func(false);
            expect(r.text().contains("NOTHING TO UNDO"), r.text());
        }

        beginTest("review (2): a send emptied is written as an empty chain (not as none): saved, read and loaded, it is still empty");
        {
            SequencerCore seq1, seq2;
            Rig first, second;
            first.audio.removeInsertEffect(S1, 0);
            ProjectBridge::Live live1 { seq1, first.audio }, live2 { seq2, second.audio };
            const ProjectData read = ProjectData::fromJSON(ProjectBridge::capture(live1, "empty").toJSON());
            expect(read.sendBuses.size() == 2 && read.sendBuses[0].hasChain && read.sendBuses[0].chain.empty());
            ProjectBridge::apply(read, live2);
            expectEquals(second.audio.getInsertChain(S1)->getEffectCount(), 0);
            expectEquals(second.audio.getInsertChain(S2)->getEffectCount(), 1);
        }

        beginTest("review (2): stored chains put on the sends from another thread while blocks are played (as a project load does): numbers, and the chains end as stored");
        {
            ParamMap stored;
            {
                AudioEngine shaper;
                const int t = AudioEngine::sendTrack(0);
                shaper.setInsertEffect(t, 0, EffectType::TapeDelayEffect);
                shaper.addInsertEffect(t, EffectType::PhaserEffect);
                shaper.addInsertEffect(t, EffectType::HallReverbEffect);
                shaper.setTrackParameter(t, WET + 2, 0.4f);
                ProjectBridge::captureChain(shaper, t, stored);
            }
            Rig rig(48000.0, 128, true);
            rig.audio.setTrackParameter(1, SND, 0.7f);
            rig.audio.setTrackParameter(0, SND + 1, 0.7f);
            rig.start();
            rig.chord();
            std::atomic<bool> done { false };
            std::thread worker([&]
            {
                for (int i = 0; i < 12; ++i)
                {
                    rig.audio.resetSendBuses();
                    ProjectBridge::applySendChain(rig.audio, 0, stored);
                    ProjectBridge::applySendChain(rig.audio, 1, i % 2 == 0 ? ParamMap {} : stored);
                }
                done.store(true);
            });
            bool numbers = true;
            int blocks = 0;
            while (! done.load() || blocks < 20)
            {
                rig.audio.renderBlock(rig.l.data(), rig.r.data(), 128);
                for (int i = 0; i < 128; ++i)
                    numbers = numbers && std::abs(rig.l[(size_t) i]) <= 8.0f && std::abs(rig.r[(size_t) i]) <= 8.0f;
                ++blocks;
                // A real audio thread rests between its blocks; without a rest this loop takes the structure
                // lock again at once and the worker waits behind it for seconds (the test then took 25-50 s
                // alone and minutes on a busy machine, whatever the number of rounds).
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
            worker.join();
            expect(numbers);
            expectEquals(rig.audio.getNonFiniteBlocks(), 0);
            ParamMap now0, now1;
            ProjectBridge::captureChain(rig.audio, S1, now0);
            ProjectBridge::captureChain(rig.audio, S2, now1);
            expect(now0 == stored && now1 == stored);
        }
    }

    template <typename Engine>
    static double blockMicroseconds(int sends, const std::function<void(Engine&)>& setup = {})
    {
        Engine audio;
        for (int t = 0; t < 4; ++t)
        {
            audio.addEngine(t, std::make_shared<Buzz>());
            audio.setTrackParameter(t, MixParam::VOLUME, 0.6f);
            audio.setTrackParameter(t, SND, 0.3f);
            audio.setTrackParameter(t, SND + 1, 0.3f);
        }
        if (sends == 0)
            while (audio.getSendCount() > 0)
                audio.removeSendBus(0);
        if (setup) setup(audio);
        audio.prepare(48000.0, 512);
        for (int t = 0; t < 4; ++t)
            for (int note : { 48 + t, 55 + t, 64 + t })
                audio.getEngine(t)->noteOn(note, 0.8f, -1);
        std::vector<float> l(512), r(512);
        for (int b = 0; b < 200; ++b) audio.renderBlock(l.data(), r.data(), 512);
        const int blocks = 4000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) audio.renderBlock(l.data(), r.data(), 512);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
    }

    void bench()
    {
        double best[6];
        for (auto& b : best) b = 1.0e9;
        for (int round = 0; round < 15; ++round)
        {
            best[0] = std::min(best[0], blockMicroseconds<OldSendsAudioEngine>(0));
            best[1] = std::min(best[1], blockMicroseconds<AudioEngine>(0));
            best[2] = std::min(best[2], blockMicroseconds<OldSendsAudioEngine>(2));
            best[3] = std::min(best[3], blockMicroseconds<AudioEngine>(2));
            best[4] = std::min(best[4], blockMicroseconds<AudioEngine>(2, [](AudioEngine& a)
            {
                a.setInsertEffect(S1, 0, EffectType::TapeDelayEffect);
                a.addInsertEffect(S1, EffectType::PhaserEffect);
                a.addInsertEffect(S1, EffectType::ReverbEffect);
            }));
            best[5] = std::min(best[5], blockMicroseconds<AudioEngine>(2, [](AudioEngine& a)
            {
                a.removeInsertEffect(S1, 0);
                a.removeInsertEffect(S2, 0);
            }));
        }
        std::cout << "SENDS bench, microseconds per block of 512 at 48 kHz, four tracks of a cheap buzz, both sends at 0.3, the master chain of a new project, best of 15:\n"
                  << "  no send buses            before " << best[0] << "   now " << best[1] << "\n"
                  << "  the two sends (RVB, DLY)  before " << best[2] << "   now " << best[3] << "\n"
                  << "  the sends alone           before " << best[2] - best[0] << "   now " << best[3] - best[1] << "\n"
                  << "  send 1 = TAPE > PHS > RVB, send 2 = DLY   now " << best[4] << "   (the sends alone " << best[4] - best[1] << ")\n"
                  << "  both sends empty          now " << best[5] << std::endl;
        expect(true);
    }
};

static SendChainTests sendChainTests;
