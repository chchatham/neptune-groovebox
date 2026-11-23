#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <limits>
#include <type_traits>
#include <vector>
#include "track_input_tests.h"
#include "resonator_tests.h"
#include "multi_preset_tests.h"
#include "fast_measure.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/ResonatorEngine.h"
#include "../src/engine/EngineModSection.h"
#include "../src/dsp/effects/Vocoder.h"
#include "../src/project/ProjectBridge.h"

// The parts of the track input together, through the AudioEngine with the real engines and
// effects: a drum kit exciting the resonator (EXC IN), a vocoder shaped by another track, a mod
// slot that follows another track's level (SRC IN), a feedback pair, and the input through presets
// and projects. The parts' own suites are "Track Input", "Resonator", "Vocoder", "Mod Slots" and
// "Multi Presets".
namespace inputtest
{
    // The AudioEngine's taps are private, and nothing can write a number that is not one into them
    // from outside (an engine's or an insert's block with one in it is dropped before the tap is
    // written). The test of readTrackInput's guard needs one there: the member is reached through
    // an explicit instantiation, which may name a private member.
    using Taps = std::vector<std::vector<float>>;
    Taps AudioEngine::* tapMember(std::integral_constant<int, 0>);
    Taps AudioEngine::* tapMember(std::integral_constant<int, 1>);
    template <Taps AudioEngine::* Member, int Side>
    struct OpenTap
    {
        friend Taps AudioEngine::* tapMember(std::integral_constant<int, Side>) { return Member; }
    };
    template struct OpenTap<&AudioEngine::trackTapL, 0>;
    template struct OpenTap<&AudioEngine::trackTapR, 1>;
    inline Taps& tapsL(AudioEngine& a) { return a.*tapMember(std::integral_constant<int, 0>()); }
    inline Taps& tapsR(AudioEngine& a) { return a.*tapMember(std::integral_constant<int, 1>()); }

    // The instrument with real engines, rendered block by block; what is kept is the left side of
    // the whole mix.
    struct Bench : TrackInputTests::Rig
    {
        double rate;
        bool finite = true;
        float top = 0.0f;

        explicit Bench(int blockSize = 64, double sampleRate = 48000.0) : Rig(blockSize, sampleRate), rate(sampleRate) {}

        EngineBase* engine(int t, EngineType type)
        {
            seq.getPattern().getTrack(t).engineType = type;
            audio.addEngine(t, AudioEngine::createEngine(type));
            audio.prepare(rate, block);
            // (two renders of the unified voice are compared: no random start phases, no drift)
            if (auto* u = dynamic_cast<UnifiedVoiceEngine*>(audio.getEngine(t)))
                for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                    u->getVoice(v).getAnalogCharacter().enabled = false;
            return audio.getEngine(t);
        }
        void set(int t, int id, float value) { audio.setTrackParameter(t, id, value); }
        // Not heard in the mix (it still feeds whoever listens to it).
        void shut(int t) { set(t, MixParam::VOLUME, 0.0f); }
        void on(int t, int note, float velocity = 0.9f) { audio.getEngine(t)->noteOn(note, velocity, -1); }
        void off(int t, int note) { audio.getEngine(t)->noteOff(note, -1); }

        std::vector<float> render(double seconds)
        {
            std::vector<float> out;
            const int blocksToDo = (int) std::ceil(seconds * rate / block);
            out.reserve((size_t) (blocksToDo * block));
            for (int b = 0; b < blocksToDo; ++b)
            {
                audio.renderBlock(left.data(), right.data(), block);
                for (int i = 0; i < block; ++i)
                {
                    const float l = left[(size_t) i], r = right[(size_t) i];
                    if (! std::isfinite(l) || ! std::isfinite(r))
                        finite = false;
                    else
                        top = std::max(top, std::max(std::abs(l), std::abs(r)));
                    out.push_back(l);
                }
            }
            return out;
        }

        // The index of a slot's DEST (or an LFO's) called `label` on track t.
        int dest(int t, const char* label)
        {
            for (auto& page : audio.getEngine(t)->getParamPages())
                if (const auto* spec = page.find(ModSlotParam::id(0, ModSlotParam::DEST)))
                    for (int i = 0; i < spec->numChoices; ++i)
                        if (juce::String(spec->choices[i]) == label)
                            return i;
            return -1;
        }
    };

    inline double rms(const std::vector<float>& x, double fromSeconds = 0.0, double rate = 48000.0)
    {
        return ResonatorTests::rms(x, (size_t) (fromSeconds * rate));
    }
    inline float dB(double v) { return ResonatorTests::dB(v); }
    inline double hz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }
}

class TrackInputIntegrationTests : public juce::UnitTest
{
public:
    TrackInputIntegrationTests() : UnitTest("Track Input Integration") {}

    using RE = ResonatorEngine;
    using RV = ResonatorVoice;

    // ---------------------------------------------------------------------------------------
    void guard()
    {
        using namespace inputtest;
        beginTest("A sample of the source's tap that is not a number is silence to the listener: the engine, the follower and "
                  "a MIX loop never see it");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            for (float level : { 1.0f, 0.0f })
            {
                TrackInputTests::Rig r;
                auto* echo = r.add<TrackInputTests::EchoEngine>(0);
                auto* tone = r.add<TrackInputTests::ToneEngine>(1);
                echo->copy = false;
                tone->left = 0.5f;
                tone->right = 0.25f;
                r.audio.setTrackParameter(0, TrackInParam::SRC, 2.0f);      // a higher track: the block before's tap
                r.audio.setTrackParameter(0, TrackInParam::MODE, 1.0f);     // MIX: what comes in goes on to the mix
                r.audio.setTrackParameter(0, TrackInParam::LEVEL, level);
                r.audio.setTrackParameter(1, MixParam::VOLUME, 0.0f);
                r.blocks(40);
                if (! echo->connected || echo->inL.size() < 64 || echo->inR.size() < 64)
                {
                    expect(false, "the listener is given no input");
                    continue;
                }
                const float good = fxb::softSat(0.5f * level);
                expectWithinAbsoluteError(echo->inL[3], good, 1.0e-6f);
                r.blocks(1);
                const float followerBefore = echo->envelope, peakBefore = r.peak();
                if (level > 0.0f)
                    expect(followerBefore > 0.3f && peakBefore > 0.05f, "the input is heard and followed");

                auto& l = tapsL(r.audio)[1];
                auto& rr = tapsR(r.audio)[1];
                expect(l.size() >= 64 && rr.size() >= 64);
                l[5] = nan;  l[6] = inf;  l[7] = -inf;
                rr[8] = nan; rr[9] = inf; rr[10] = -inf;
                r.blocks(1);
                bool numbers = true;
                for (int i = 0; i < 64; ++i)
                    numbers = numbers && std::isfinite(echo->inL[(size_t) i]) && std::isfinite(echo->inR[(size_t) i]);
                expect(numbers, "the engine is given numbers only");
                for (int i : { 5, 6, 7 })
                    expectEquals(echo->inL[(size_t) i], 0.0f, "left " + juce::String(i));
                for (int i : { 8, 9, 10 })
                    expectEquals(echo->inR[(size_t) i], 0.0f, "right " + juce::String(i));
                expectWithinAbsoluteError(echo->inL[4], good, 1.0e-6f, "the samples beside them are untouched");
                expectWithinAbsoluteError(echo->inL[8], good, 1.0e-6f);
                expect(std::isfinite(r.peak()), "the mix (MODE MIX) is numbers");
                if (level > 0.0f)
                    expect(r.peak() > 0.5f * peakBefore, "and the block was not dropped: " + juce::String(r.peak()));
                expectEquals(r.audio.getNonFiniteBlocks(), 0, "nothing had to be dropped");

                // The follower: still a number, and where it was (it is given the next block).
                r.blocks(1);
                expect(std::isfinite(echo->envelope), "the follower is a number");
                expectWithinAbsoluteError(echo->envelope, followerBefore, 0.05f);
                expectWithinAbsoluteError(echo->inL[5], good, 1.0e-6f, "the next block is the source again");

                // Whole taps of them, for a while: silence comes in, and the mix falls silent.
                for (int b = 0; b < 400; ++b)
                {
                    std::fill(l.begin(), l.end(), nan);
                    std::fill(rr.begin(), rr.end(), b % 2 ? inf : -inf);
                    r.blocks(1);
                    numbers = numbers && std::isfinite(r.peak());
                    for (int i : { 0, 31, 63 })
                        numbers = numbers && echo->inL[(size_t) i] == 0.0f && echo->inR[(size_t) i] == 0.0f;
                }
                expect(numbers, "a tap of nothing but them is silence");
                expect(r.peak() < 1.0e-4f, "silence in the mix: " + juce::String(r.peak(), 6));
                expect(echo->envelope < 0.05f, "and the follower has fallen: " + juce::String(echo->envelope, 4));
                r.blocks(3);
                expect(std::isfinite(echo->envelope) && std::isfinite(r.peak()));
                expectEquals(r.audio.getNonFiniteBlocks(), 0);
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // The kit on one track, RSN with IN at the top (the input alone drives the body) on another. The kit's own fader is shut: what is heard
    // is the resonator.
    struct Harp
    {
        inputtest::Bench bench;
        int kit, rsn;

        Harp(int kitTrack, int rsnTrack, int model, int block = 64) : bench(block), kit(kitTrack), rsn(rsnTrack)
        {
            bench.engine(kit, EngineType::Drum);
            bench.engine(rsn, EngineType::Resonator);
            bench.set(rsn, RE::MorphParam, (float) model);
            bench.set(rsn, RE::InputParam, 1.0f);
            bench.set(rsn, RE::ExciteBase + res::Grain, 0.2f);      // noise that follows the kit's level: a drum's own tone is not all the body gets
            bench.set(rsn, TrackInParam::SRC, (float) (kit + 1));
            bench.shut(kit);
            bench.render(0.05);         // (the kit's fader arrives at nothing)
        }

        // `seconds` with a pad of the kit struck every eighth of a second (or none).
        std::vector<float> play(double seconds, bool hits)
        {
            std::vector<float> out;
            const int every = std::max(1, (int) std::lround(0.125 * bench.rate / bench.block));
            const int blocks = (int) std::ceil(seconds * bench.rate / bench.block);
            for (int b = 0; b < blocks; ++b)
            {
                if (hits && b % every == 0)
                    bench.on(kit, (b / every) % 4, 1.0f);
                const auto x = bench.render((double) bench.block / bench.rate);
                out.insert(out.end(), x.begin(), x.end());
            }
            return out;
        }
    };

    void exciter()
    {
        using namespace inputtest;
        beginTest("Drum kit into RSN (IN at the top): the resonator sounds only while the kit plays AND a note is held, at the held "
                  "note's pitch, on every model");
        {
            juce::String table = "kit -> RSN, dB / cents off the held note:";
            for (int model = 0; model < RE::NUM_BODIES; ++model)
            {
                const juce::String name = ResonatorTests::modelName(model);
                {
                    Harp h(0, 1, model);
                    const auto x = h.play(0.6, true);
                    expect(rms(x) < 1.0e-6, name + ": the kit plays, no note held: silence (" + juce::String(dB(rms(x)), 1) + " dB)");
                    expect(h.bench.finite);
                }
                {
                    Harp h(0, 1, model);
                    h.bench.on(1, 57);
                    const auto x = h.play(0.6, false);
                    expect(rms(x) < 1.0e-6, name + ": a note held, the kit silent: silence (" + juce::String(dB(rms(x)), 1) + " dB)");
                }
                // Two notes, one at a time, under the same playing of the kit.
                const int notes[2] = { 57, 63 };     // (a tritone apart: neither is a partial of the other in any model)
                std::vector<float> heard[2];
                for (int k = 0; k < 2; ++k)
                {
                    const int note = notes[k];
                    Harp h(0, 1, model);
                    h.bench.on(1, note);
                    heard[k] = h.play(1.5, true);
                    const auto& x = heard[k];
                    const size_t from = 9600, n = x.size() - from;
                    const double level = rms(x, 0.2);
                    const double off = ResonatorTests::centsOff(x, from, n, hz(note), 48000.0);
                    table << "  " << name << " " << note << ": " << juce::String(dB(level), 1) << " / " << juce::String(off, 1);
                    expect(level > 1.0e-3, name + " note " + juce::String(note) + " sounds: " + juce::String(dB(level), 1) + " dB");
                    // (A body whose pitch rises with how hard it moves - DROP: a head, a gong, a tam-tam -
                    // is as sharp as the kit drives it: 20 to 30 cents here. Its pitch at rest is measured
                    // in the "Resonator Drum" suite.)
                    if (res::morePoints()[(size_t) model][res::Drop] <= 0.0f)
                        expect(std::abs(off) < 8.0, name + " note " + juce::String(note) + ": " + juce::String(off, 1) + " cents off");
                    if (model < res::Plate)
                    {
                        // A string, a tube, a comb: nothing beside the note (the plate, the bar and
                        // the bell have partials of their own there, and the kit's drums ring in them).
                        const double at = fastmeasure::goertzel(x, from, n, hz(note), 48000.0);
                        double beside = 0.0;
                        for (double ratio : { 0.84, 0.92, 1.08 })
                            beside = std::max(beside, fastmeasure::goertzel(x, from, n, hz(note) * ratio, 48000.0));
                        expect(at > 10.0 * beside, name + " note " + juce::String(note) + ": the note stands "
                               + juce::String(dB(std::sqrt(at / std::max(beside, 1.0e-30))), 1) + " dB over what is beside it");
                    }
                    expect(h.bench.finite && h.bench.audio.getNonFiniteBlocks() == 0);

                    // Let go: the input is shut out again once the release is over.
                    h.bench.set(1, RE::AmpRelease, 0.02f);
                    h.bench.set(1, RE::DecayParam, 0.05f);
                    h.bench.off(1, note);
                    h.play(0.5, true);
                    expect(rms(h.play(0.3, true)) < 1.0e-6, name + ": released, the kit goes on: silence");
                }
                // The pitch is the held note's: each note's frequency is there while that note is
                // held and not while the other one is.
                for (int k = 0; k < 2; ++k)
                {
                    const size_t from = 9600, n = heard[k].size() - from;
                    const double own = fastmeasure::goertzel(heard[k], from, n, hz(notes[k]), 48000.0);
                    const double others = fastmeasure::goertzel(heard[1 - k], from, n, hz(notes[k]), 48000.0);
                    table << " (" << (int) std::lround(dB(std::sqrt(own / std::max(others, 1.0e-30)))) << " dB over the other note's)";
                    if (res::morePoints()[(size_t) model][res::Drop] <= 0.0f)       // (see above)
                    expect(own > 10.0 * others, name + ": " + juce::String(hz(notes[k]), 0) + " Hz is "
                           + juce::String(dB(std::sqrt(own / std::max(others, 1.0e-30))), 1) + " dB stronger with note "
                           + juce::String(notes[k]) + " held than with note " + juce::String(notes[1 - k]));
                }
            }
            logMessage(table);
        }

        beginTest("The same with the kit on a higher track than the resonator: it works as well, one block late");
        {
            for (int block : { 64, 256 })
            {
                double level[2] = {};
                int onset[2] = { -1, -1 };
                for (int late = 0; late < 2; ++late)
                {
                    const int kit = late ? 3 : 0, rsn = late ? 0 : 3;
                    {
                        Harp h(kit, rsn, res::String, block);
                        h.bench.on(rsn, 57);
                        const auto x = h.play(1.5, true);
                        level[late] = rms(x, 0.2);
                        const double off = ResonatorTests::centsOff(x, 9600, x.size() - 9600, hz(57), 48000.0);
                        expect(std::abs(off) < 8.0, juce::String(late ? "kit above" : "kit below") + ": " + juce::String(off, 1) + " cents off");
                    }
                    // When the first hit arrives (a bank of modes answers at once).
                    Harp h(kit, rsn, res::String, block);
                    h.bench.on(rsn, 57);
                    h.play(0.2, false);
                    h.bench.on(kit, 0, 1.0f);
                    const auto x = h.bench.render(0.05);
                    for (int i = 0; i < (int) x.size() && onset[late] < 0; ++i)
                        if (std::abs(x[(size_t) i]) > 1.0e-7f)
                            onset[late] = i;
                }
                expect(level[0] > 1.0e-3 && level[1] / level[0] > 0.7 && level[1] / level[0] < 1.4,
                       "block " + juce::String(block) + ": as loud either way, " + juce::String(dB(level[0]), 1) + " and "
                       + juce::String(dB(level[1]), 1) + " dB");
                expect(onset[0] >= 0, "the kit below: heard (from sample " + juce::String(onset[0]) + " after the hit)");
                expectEquals(onset[1] - onset[0], block, "the kit above: exactly one block later");
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // A carrier (the unified voice, track 2) with VOC inserts, and a modulator (FM, track 1, its
    // fader shut) as the carrier track's input.
    struct Talk
    {
        inputtest::Bench bench;

        explicit Talk(int vocoders, bool connected = true, EffectChain::Routing routing = EffectChain::Routing::Series)
        {
            bench.engine(0, EngineType::FM);
            bench.engine(1, EngineType::UnifiedVoice);
            bench.shut(0);
            for (int i = 0; i < vocoders; ++i)
                bench.audio.addInsertEffect(1, EffectType::VocoderEffect);
            if (vocoders > 0)
                bench.audio.getInsertChain(1)->setRouting(routing);
            if (connected)
                bench.set(1, TrackInParam::SRC, 1.0f);
            for (int note : { 36, 43, 52 })
                bench.on(1, note);
            bench.render(0.3);
        }
        void speak(bool on)
        {
            for (int note : { 60, 67, 76 })
                on ? bench.on(0, note, 1.0f) : bench.off(0, note);
        }
    };

    void vocoder()
    {
        using namespace inputtest;
        beginTest("VOC on a synth track, another track as its input: the output follows the modulator's level; SRC OFF passes "
                  "the carrier");
        {
            Talk t(1);
            const double before = rms(t.bench.render(0.4));
            t.speak(true);
            t.bench.render(0.1);
            const double during = rms(t.bench.render(0.4));
            t.speak(false);
            t.bench.render(2.5);
            const double after = rms(t.bench.render(0.4));
            expect(during > 1.0e-3, "the modulator plays: the carrier is heard, " + juce::String(dB(during), 1) + " dB");
            expect(before < 0.01 * during, "the modulator silent: silence, " + juce::String(dB(before), 1) + " dB");
            expect(after < 0.02 * during, "the modulator let go: silence again, " + juce::String(dB(after), 1) + " dB");
            expect(t.bench.finite && t.bench.audio.getNonFiniteBlocks() == 0);

            // A quieter modulator (the input's LEVL) is a quieter result.
            auto levelAt = [](float inputLevel)
            {
                Talk q(1);
                q.bench.set(1, TrackInParam::LEVEL, inputLevel);
                q.speak(true);
                q.bench.render(0.1);
                return rms(q.bench.render(0.4));
            };
            const double full = levelAt(1.0f), half = levelAt(0.5f), quarter = levelAt(0.25f);
            logMessage("VOC, modulator at LEVL 100 / 50 / 25: " + juce::String(dB(full), 1) + " / " + juce::String(dB(half), 1)
                       + " / " + juce::String(dB(quarter), 1) + " dB");
            expect(half < 0.8 * full && half > 0.3 * full, "LEVL 50: " + juce::String(half / full, 3) + " of LEVL 100");
            expect(quarter < 0.8 * half && quarter > 0.3 * half, "LEVL 25: " + juce::String(quarter / half, 3) + " of LEVL 50");

            // SRC OFF: the carrier as it is without the insert.
            Talk open(1, false), bare(0, false);
            const auto a = open.bench.render(0.4), b = bare.bench.render(0.4);
            expect(rms(b) > 1.0e-3, "(the carrier sounds: " + juce::String(dB(rms(b)), 1) + " dB)");
            expect(a == b, "SRC OFF: VOC passes the carrier, sample for sample (difference "
                           + juce::String(fastmeasure::difference(a, b, 1024), 5) + ")");
            // ... and the modulator playing changes nothing then.
            open.speak(true);
            bare.speak(true);
            expect(open.bench.render(0.3) == bare.bench.render(0.3), "SRC OFF, the other track playing: still the carrier");
        }

        beginTest("VOC in the mode TUNE on a synth track, set by the track's parameters: it is silent until spoken into, speaks, "
                  "and rings on after the modulator where BAND does not");
        {
            double during[2] = {}, after[2] = {};
            for (int tune = 0; tune < 2; ++tune)
            {
                Talk t(1);
                if (tune)
                {
                    t.bench.set(1, AudioEngine::EFFECT_PARAM_BASE + Vocoder::Mode, (float) Vocoder::Tuned);
                    t.bench.set(1, AudioEngine::INSERT_EXTRA_BASE + Vocoder::Ring, 10.0f);
                    t.bench.set(1, AudioEngine::INSERT_EXTRA_BASE + Vocoder::BodyAt, (float) res::Bell);
                    auto* fx = t.bench.audio.getInsertChain(1)->getEffect(0);
                    expectEquals(fx->getParameter(Vocoder::Mode), (float) Vocoder::Tuned);
                    expectEquals(fx->getExtra(Vocoder::Ring), 10.0f);
                    expectEquals(fx->getExtra(Vocoder::BodyAt), (float) res::Bell);
                }
                const double before = rms(t.bench.render(0.4));
                t.speak(true);
                t.bench.render(0.1);
                during[tune] = rms(t.bench.render(0.4));
                t.speak(false);
                t.bench.render(2.5);
                after[tune] = rms(t.bench.render(0.4));
                expect(during[tune] > 1.0e-3, "spoken into: " + juce::String(dB(during[tune]), 1) + " dB");
                expect(before < 0.01 * during[tune], "the modulator silent: silence, " + juce::String(dB(before), 1) + " dB");
                expect(t.bench.finite && t.bench.audio.getNonFiniteBlocks() == 0);
            }
            logMessage("2.5 s after the modulator: BAND " + juce::String(dB(after[0] / during[0]), 1) + " dB, TUNE at RING 10 s "
                       + juce::String(dB(after[1] / during[1]), 1) + " dB against while it spoke");
            expect(after[0] < 0.02 * during[0], "BAND is silent again");
            expect(after[1] > 0.03 * during[1], "TUNE still rings");
        }

        beginTest("Two VOC inserts in one chain are both given the input, in a serial, a parallel and a mixed chain");
        {
            const EffectChain::Routing routings[] = { EffectChain::Routing::Series, EffectChain::Routing::Parallel, EffectChain::Routing::Mixed };
            const char* const names[] = { "serial", "parallel", "mixed" };
            for (int k = 0; k < 3; ++k)
            {
                // Two real ones. One that had no input would pass the carrier while the modulator is silent.
                Talk t(2, true, routings[k]);
                const double before = rms(t.bench.render(0.4));
                t.speak(true);
                t.bench.render(0.1);
                const double during = rms(t.bench.render(0.4));
                Talk bare(0, false);
                const double carrier = rms(bare.bench.render(0.4));
                expect(during > 1.0e-4, juce::String(names[k]) + ": the modulator plays, " + juce::String(dB(during), 1) + " dB");
                expect(before < 0.01 * carrier && before < 0.05 * during,
                       juce::String(names[k]) + ": the modulator silent: neither passes the carrier (" + juce::String(dB(before), 1)
                       + " dB; the carrier is " + juce::String(dB(carrier), 1) + " dB)");
                expect(t.bench.finite);

                // Two probes: each is given the same input before every block.
                TrackInputTests::Rig r;
                auto* tone = r.add<TrackInputTests::ToneEngine>(0);
                r.add<TrackInputTests::ToneEngine>(1);
                tone->left = 0.5f;
                tone->right = -0.25f;
                auto* first = r.probe(1);
                auto* second = r.probe(1);
                r.audio.getInsertChain(1)->setRouting(routings[k]);
                r.audio.setTrackParameter(1, TrackInParam::SRC, 1.0f);
                r.blocks(5);
                for (auto* p : { first, second })
                {
                    expectEquals(p->gives, 5, names[k]);
                    expect(p->given && p->givenBeforeProcess && p->frames == TrackInputTests::Rig::BLOCK, names[k]);
                    if (p->given)
                    {
                        expectWithinAbsoluteError(p->inL[9], fxb::softSat(0.5f), 1.0e-6f);
                        expectWithinAbsoluteError(p->inR[9], fxb::softSat(-0.25f), 1.0e-6f);
                    }
                }
                r.audio.setTrackParameter(1, TrackInParam::SRC, 0.0f);
                r.blocks(2);
                expect(! first->given && ! second->given, juce::String(names[k]) + ": both are told when there is none");
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    void follower()
    {
        using namespace inputtest;
        beginTest("A mod slot with SRC IN: the level of a synth track follows the level of the track it listens to, on every "
                  "sound engine with slots");
        {
            // Track 1: the source (FM, fader shut). Track 2: the listener, a held note, slot 1 = IN on AMP.
            // What is measured: the listener's level against the same listener without the slot.
            struct Levels { double silent, playing, after; };
            auto run = [](EngineType type, float amount, float inputLevel, bool connected)
            {
                Bench b;
                b.engine(0, EngineType::FM);
                b.engine(1, type);
                b.shut(0);
                if (type == EngineType::Resonator)
                {
                    b.set(1, RE::MorphParam, (float) res::Comb);
                    b.set(1, RE::ExciterMorphParam, (float) res::Bow);
                }
                if (connected)
                    b.set(1, TrackInParam::SRC, 1.0f);
                b.set(1, TrackInParam::LEVEL, inputLevel);
                b.set(1, ModSlotParam::id(0, ModSlotParam::SRC), (float) ModSrc::Input);
                b.set(1, ModSlotParam::id(0, ModSlotParam::DEST), (float) b.dest(1, "AMP"));
                b.set(1, ModSlotParam::id(0, ModSlotParam::AMT), amount);
                b.on(1, 48);
                b.render(0.3);
                Levels l {};
                l.silent = rms(b.render(0.3));
                for (int note : { 60, 67, 76 })
                    b.on(0, note, 1.0f);
                b.render(0.1);
                l.playing = rms(b.render(0.3));
                for (int note : { 60, 67, 76 })
                    b.off(0, note);
                b.render(2.5);
                l.after = rms(b.render(0.3));
                return l;
            };
            juce::String table = "slot IN on AMP, the listener's level while the source plays against no slot (AMT -100 / +100 / -100 at LEVL 30):";
            for (EngineType type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                                     EngineType::WaveSeq, EngineType::Resonator })
            {
                const juce::String name = "engine " + juce::String((int) type);
                const Levels plain = run(type, 0.0f, 1.0f, true);
                const Levels down = run(type, -1.0f, 1.0f, true);
                const Levels up = run(type, 1.0f, 1.0f, true);
                const Levels less = run(type, -1.0f, 0.3f, true);
                const Levels unplugged = run(type, -1.0f, 1.0f, false);
                expect(plain.playing > 1.0e-4, name + ": the listener's note sounds, " + juce::String(dB(plain.playing), 1) + " dB");
                const double d = down.playing / plain.playing, u = up.playing / plain.playing, l = less.playing / plain.playing;
                table << "  " << name << ": " << juce::String(d, 2) << " / " << juce::String(u, 2) << " / " << juce::String(l, 2);
                expect(d < 0.6, name + ": AMT -100, the source plays: the level falls to " + juce::String(d, 3));
                expect(u > 1.3, name + ": AMT +100: it rises to " + juce::String(u, 3));
                expect(l > d + 0.1 && l < 0.97, name + ": a quieter input moves it less: " + juce::String(l, 3));
                expect(std::abs(down.silent / plain.silent - 1.0) < 0.1, name + ": the source silent: the level as it is, x "
                       + juce::String(down.silent / plain.silent, 3));
                expect(std::abs(down.after / plain.after - 1.0) < 0.1, name + ": the source over: the level is back, x "
                       + juce::String(down.after / plain.after, 3));
                expect(std::abs(unplugged.playing / plain.playing - 1.0) < 0.1, name + ": SRC OFF: the slot has nothing to follow, x "
                       + juce::String(unplugged.playing / plain.playing, 3));
            }
            logMessage(table);
        }
    }

    // ---------------------------------------------------------------------------------------
    void presetsAndProjects()
    {
        using namespace inputtest;
        beginTest("LD of a single preset on a track that listens to another: the engine's input is as it was, and still heard");
        {
            multitest::MRig r;
            r.audio.prepare(44100.0, 256);
            // Track 6 hears the kit (track 2) as a bus; the kit's own fader is shut.
            const int values[][2] = { { TrackInParam::SRC, 2 }, { TrackInParam::MODE, 1 } };
            for (auto& v : values)
            {
                r.audio.setTrackParameter(5, v[0], (float) v[1]);
                r.track(5).soundParams[v[0]] = (float) v[1];
            }
            r.audio.setTrackParameter(5, TrackInParam::LEVEL, 0.6f);
            r.track(5).soundParams[TrackInParam::LEVEL] = 0.6f;
            r.audio.setTrackParameter(1, MixParam::VOLUME, 0.0f);
            auto kitThroughTrack6 = [&r]
            {
                r.render(0.3);
                r.audio.getEngine(1)->noteOn(0, 1.0f, -1);
                r.audio.getEngine(1)->noteOn(2, 1.0f, -1);
                return r.render(0.2);
            };
            const float before = kitThroughTrack6();
            expect(before > 1.0e-3f, "the kit is heard through track 6: " + juce::String(before, 4));

            r.select(5);
            r.turn(0, 3);                       // another preset of the track's engine: heard ...
            expect(r.screen->isAuditioning());
            expect(r.names().get(5).name.isEmpty());
            r.confirm(4);                       // ... and loaded
            expect(r.names().get(5).name.isNotEmpty(), "the preset is loaded: " + r.names().get(5).name);
            expectEquals(r.audio.getTrackParameter(5, TrackInParam::SRC), 2.0f, "the engine's input is still track 2");
            expectEquals(r.audio.getTrackParameter(5, TrackInParam::MODE), 1.0f);
            expectWithinAbsoluteError(r.audio.getTrackParameter(5, TrackInParam::LEVEL), 0.6f, 1.0e-5f);
            expectEquals(r.stored(5, TrackInParam::SRC), 2.0f, "and so is the track's stored one");
            const float after = kitThroughTrack6();
            expect(after > 0.3f * before, "the kit is still heard through it: " + juce::String(after, 4));
            r.audio.setTrackParameter(5, TrackInParam::SRC, 0.0f);
            r.render(0.5);
            expect(kitThroughTrack6() < 0.01f * before, "(and not without the input)");
        }

        beginTest("A project keeps a feedback pair: saved and loaded, both tracks hear each other through their inserts again");
        {
            TrackInputTests::Ui u;
            for (int t : { 2, 3 })
            {
                u.audio.setTrackParameter(t, TrackInParam::SRC, (float) (t == 2 ? 4 : 3));
                u.audio.setTrackParameter(t, TrackInParam::MODE, 1.0f);
                u.audio.setTrackParameter(t, TrackInParam::LEVEL, 0.9f);
                u.audio.addInsertEffect(t, EffectType::WaveshaperEffect);
                u.audio.setTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE, 0.8f);
                u.audio.setTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + 1, (float) sat::Fold);
            }
            ProjectBridge::Live live { u.seq, u.audio };
            const auto json = juce::JSON::toString(ProjectBridge::capture(live, "pair").toVar());

            TrackInputTests::Ui other;
            ProjectBridge::Live otherLive { other.seq, other.audio };
            ProjectBridge::apply(ProjectData::fromVar(juce::JSON::parse(json)), otherLive);
            for (int t : { 2, 3 })
            {
                expectEquals(other.audio.getTrackParameter(t, TrackInParam::SRC), (float) (t == 2 ? 4 : 3));
                expectEquals(other.audio.getTrackParameter(t, TrackInParam::MODE), 1.0f);
                expectWithinAbsoluteError(other.audio.getTrackParameter(t, TrackInParam::LEVEL), 0.9f, 1.0e-5f);
                EffectType type {};
                expect(other.audio.getInsertEffectType(t, 0, type) && type == EffectType::WaveshaperEffect, "the insert in the loop");
                expectEquals(other.audio.getTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + 1), (float) sat::Fold);
            }
            for (int t : { 0, 1, 4, 5 })
                expectEquals(other.audio.getTrackParameter(t, TrackInParam::SRC), 0.0f);

            // Heard: only track 4 plays a note, and its own fader is shut. What is in the mix came
            // through track 3's input. With track 3's fader shut instead, track 4 alone.
            auto heard = [](TrackInputTests::Ui& ui, int shut, bool* finite)
            {
                ui.audio.setTrackParameter(2, MixParam::VOLUME, shut == 2 ? 0.0f : 0.8f);
                ui.audio.setTrackParameter(3, MixParam::VOLUME, shut == 3 ? 0.0f : 0.8f);
                ui.audio.getEngine(3)->noteOn(50, 1.0f, -1);
                std::vector<float> l(64), r(64);
                double sum = 0.0;
                int count = 0;
                for (int b = 0; b < 750; ++b)       // one second
                {
                    ui.audio.renderBlock(l.data(), r.data(), 64);
                    for (int i = 0; i < 64; ++i)
                    {
                        if (! std::isfinite(l[(size_t) i]) || ! std::isfinite(r[(size_t) i]))
                            *finite = false;
                        if (b >= 150)
                        {
                            sum += (double) l[(size_t) i] * l[(size_t) i];
                            ++count;
                        }
                    }
                }
                ui.audio.getEngine(3)->noteOff(50, -1);
                return std::sqrt(sum / std::max(1, count));
            };
            bool finite = true;
            const double through = heard(other, 3, &finite);
            expect(through > 1.0e-3, "track 4 is heard through track 3: " + juce::String(dB(through), 1) + " dB");
            const double original = heard(u, 3, &finite);
            expect(through > 0.5 * original && through < 2.0 * original, "as in the instrument it was saved from: "
                   + juce::String(dB(original), 1) + " dB");
            other.audio.setTrackParameter(2, TrackInParam::SRC, 0.0f);
            for (int b = 0; b < 3000; ++b)
            {
                std::vector<float> l(64), r(64);
                other.audio.renderBlock(l.data(), r.data(), 64);
            }
            expect(heard(other, 3, &finite) < 0.01 * through, "(and not once track 3 listens to nothing)");
            expect(finite);
            expectEquals(other.audio.getNonFiniteBlocks(), 0);
        }
    }

    // ---------------------------------------------------------------------------------------
    void feedback()
    {
        using namespace inputtest;
        beginTest("A feedback pair of two real engines, MODE MIX on both, a saturating insert in the loop, LEVL at the top: "
                  "finite and bounded for 5 seconds at blocks of 32 and of 1024");
        {
            struct Pair { EngineType a, b; };
            const Pair pairs[] = { { EngineType::UnifiedVoice, EngineType::FM }, { EngineType::Resonator, EngineType::Wavetable } };
            for (int block : { 32, 1024 })
                for (auto& pair : pairs)
                {
                    const juce::String name = "block " + juce::String(block) + ", engines " + juce::String((int) pair.a) + " and " + juce::String((int) pair.b);
                    Bench b(block);
                    b.engine(0, pair.a);
                    b.engine(1, pair.b);
                    for (int t = 0; t < 2; ++t)
                    {
                        b.audio.addInsertEffect(t, EffectType::WaveshaperEffect);
                        b.set(t, AudioEngine::EFFECT_PARAM_BASE, 1.0f);                         // drive at the top
                        b.set(t, AudioEngine::EFFECT_PARAM_BASE + 1, (float) (t == 0 ? sat::Fold : sat::Hard));
                        b.set(t, TrackInParam::SRC, (float) (2 - t));
                        b.set(t, TrackInParam::MODE, 1.0f);
                        b.set(t, TrackInParam::LEVEL, 1.0f);
                    }
                    if (pair.a == EngineType::Resonator)        // the resonator rings to the other track as well
                        b.set(0, RE::InputParam, 1.0f);
                    for (int note : { 40, 47, 55 })
                    {
                        b.on(0, note, 1.0f);
                        b.on(1, note + 12, 1.0f);
                    }
                    const auto first = b.render(2.5);
                    for (int note : { 40, 47, 55 })
                    {
                        b.off(0, note);
                        b.off(1, note + 12);
                    }
                    const auto rest = b.render(2.5);
                    expect(b.finite, name + ": every sample is a number");
                    expect(b.top <= 1.5f, name + ": bounded, peak " + juce::String(b.top, 3));
                    expect(rms(first, 0.5) > 1.0e-3, name + ": it sounds, " + juce::String(dB(rms(first, 0.5)), 1) + " dB");
                    expectEquals(b.audio.getNonFiniteBlocks(), 0, name + ": no block had to be dropped");
                    juce::ignoreUnused(rest);
                }
        }
    }

    void runTest() override
    {
        guard();
        exciter();
        vocoder();
        follower();
        presetsAndProjects();
        feedback();
    }
};

static TrackInputIntegrationTests trackInputIntegrationTests;
