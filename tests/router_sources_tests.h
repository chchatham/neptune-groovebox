#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <functional>
#include <set>
#include <type_traits>
#include <vector>
#include "mod_router_tests.h"
#include "old/OldEnvelope.h"
#include "old/OldOnset.h"
#include "old/OldVocoder.h"
#include "../src/dsp/Envelope.h"
#include "../src/dsp/effects/Detector.h"
#include "../src/dsp/effects/FxLfo.h"
#include "../src/dsp/effects/Vocoder.h"
#include "../src/engine/ModRouter.h"
#include "../src/engine/VoiceSpread.h"
#include "../src/engine/MacroOscEngine.h"
#include "../src/engine/WaveSeqEngine.h"
#include "../src/engine/FMEngine.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/SamplerEngine.h"
#include "../src/engine/DrumEngine.h"
#include "../src/engine/ResonatorEngine.h"

// -----------------------------------------------------------------------------
// "Router Sources": what phase U4-0 of the unification added to the shared pieces before the
// engines are ported onto the modulation router (src/engine/ModRouter.h).
//
//   the source list        what SRC and VIA show, in order and in groups; places are stored
//   an engine's own        sources a voice publishes, at control rate and as samples; wants()
//   the randoms            RAND, RND2 .. RND4 from the one seeded per-note generator; Sequence
//   the envelope           its time scale: the old class bit for bit at 1; ETIM / FTIM
//   the track input        ONST and an LFO restarted by a hit (XKEY ..); the sixteen band levels
//   moved code             dyn::Onset against the old fxm::Onset, VOC against the old vocoder
//   ids                    158-189 are the router's on every engine
//   the macro oscillator   OSC, ETIM, FTIM, the randoms repeating, no allocation, no click
// -----------------------------------------------------------------------------
class RouterSourcesTests : public juce::UnitTest
{
public:
    RouterSourcesTests() : UnitTest("Router Sources") {}

    using R = mod::Router;
    using M = MacroOscEngine;
    using V = MacroOscVoice;
    using Old = ModRouterTests;
    enum { PTCH = 1, AMP, CUT, RESO, ETIM, FTIM };
    enum { LANE = 0, WOB, OPA };            // the rig's own sources, in the order published
    static constexpr double SR = 48000.0;

    static const mod::DestTable& table()
    {
        static const mod::DestTable t = []
        {
            mod::DestTable d;
            d.group();
            d.input("PTCH", 1.0f, 1.0f, 1.0f / 1024.0f, 1024.0f, mod::Pitch);
            d.input("AMP", 1.0f, 1.0f, 0.0f, 4.0f, mod::PerSample).audioFloor(-4.0f);
            d.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);
            d.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
            d.group();
            d.envTimes();
            d.finish(2);
            return d;
        }();
        return t;
    }

    // An engine's list: a value that goes one way, one that swings, and one handed over as samples.
    static const mod::SourceTable& sources()
    {
        static const mod::SourceTable t = []
        {
            mod::SourceTable s(2);
            s.publish("LANE", mod::Unipolar);
            s.publish("WOB");
            s.publish("OPA", mod::AudioRate);
            return s;
        }();
        static const bool registered = [] { t.registerGroups(); return true; }();
        juce::ignoreUnused(registered);
        return t;
    }

    struct Rig
    {
        R r { table(), 2, 0, &sources() };
        std::array<mod::Voice, 2> v;
        std::function<float(long)> input;       // the track input by sample (nullptr: not connected)
        long position = 0;
        std::array<float, 16> in {};
        int n = 16;
        float strip = 0.0f;

        Rig()
        {
            r.prepare(SR);
            for (auto& voice : v)
                voice.prepare(SR);
        }
        // src and via are sources of mod::Src (the rig's own: mod::Src::Published1 + k).
        void slot(int k, int src, int dest, float amount, int via = mod::Src::Off)
        {
            r.set(ModSlotParam::id(k, ModSlotParam::SRC), (float) r.sourceIndex(src));
            r.set(ModSlotParam::id(k, ModSlotParam::DEST), (float) dest);
            r.set(ModSlotParam::id(k, ModSlotParam::AMT), amount);
            r.set(ModSlotParam::id(k, ModSlotParam::VIA), (float) r.sourceIndex(via));
        }
        void lfo(int i, int dest, float depth, int shape, int trigger, float hz)
        {
            r.set(R::lfoId(i, R::Rate), hz);
            r.set(R::lfoId(i, R::Shape), (float) shape);
            r.set(R::lfoId(i, R::Trigger), (float) trigger);
            r.set(R::lfoId(i, R::Depth), depth);
            r.set(R::lfoId(i, R::DestCell), (float) dest);
        }
        void note(int which = 0, int note = 60) { v[(size_t) which].noteOn(r, note, 1.0f); }
        void chunk()
        {
            if (input)
                for (int i = 0; i < 16; ++i)
                    in[(size_t) i] = input(position + i);
            r.beginBlock(input ? in.data() : nullptr, input ? in.data() : nullptr, input ? 16 : 0, 0.0f, strip, SR);
            n = r.beginChunk(0, 16);
            position += 16;
        }
        bool run(int which = 0) { return v[(size_t) which].tick(r, n, 1.0f, 0.0f, 0.0f); }
        bool tick(int which = 0) { chunk(); return run(which); }
        void settle(int which = 0, int ticks = 80) { for (int i = 0; i < ticks; ++i) tick(which); }
        // Chunks until the input is at `seconds`; voice `which` sounds, the other is idle.
        void until(double seconds, int which = 0)
        {
            while (position < (long) (seconds * SR))
            {
                chunk();
                for (int k = 0; k < 2; ++k)
                    if (k == which) run(k); else v[(size_t) k].idle(r, n);
            }
        }
        float at(int dest, int which = 0) const
        {
            const float* b = v[(size_t) which].buffer(dest);
            return b != nullptr ? b[n - 1] : r.baseOf(dest);
        }
        float told(int dest, int which = 0) const
        {
            float out = -1000.0f;
            v[(size_t) which].forEachControl([&](int d, float value) { if (d == dest) out = value; });
            return out;
        }
    };

    static int own(int k) { return mod::Src::Published1 + k; }

    static juce::String names(const mod::SourceTable& t, bool groupsOnly = false)
    {
        juce::StringArray out;
        if (groupsOnly)
            for (int g = 0; g < t.numGroups(); ++g)
                out.add(t.labels()[t.groupStart(g)]);
        else
            for (int i = 0; i < t.size(); ++i)
                out.add(t.labels()[i]);
        return out.joinIntoString(" ");
    }

    // A burst of a 200 Hz tone, `level` high, from `from` for `length` seconds.
    static float burst(long i, double from, double length, float level)
    {
        const double t = (double) i / SR;
        return t >= from && t < from + length ? level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 200.0 * (t - from)) : 0.0f;
    }

    void runTest() override
    {
        listTests();
        publishedTests();
        randomTests();
        envelopeTests();
        inputTests();
        movedCodeTests();
        idTests();
        macroTests();
    }

    // =============================================================================================
    void listTests()
    {
        beginTest("The source list: the places that were there keep their sources, then RND2 .. RND4, ONST, the bands, the engine's own; groups for Func + turn");
        {
            const auto& plain = mod::SourceTable::standard(2, 0);
            expectEquals(names(plain), juce::String("OFF LFO1 LFO2 ENV FENV VEL PRES STRP KEY RAND IN XIN XLP XHP XBP VIB ONE "
                                                    "RND2 RND3 RND4 ONST BND1 BND2 BND3 BND4 BND5 BND6 BND7 BND8 "
                                                    "BND9 BN10 BN11 BN12 BN13 BN14 BN15 BN16"));
            expectEquals(names(plain, true), juce::String("OFF VEL IN VIB RND2 ONST"));
            const auto& full = mod::SourceTable::standard(4, 4);
            expectEquals(names(full), juce::String("OFF LFO1 LFO2 ENV FENV VEL PRES STRP KEY RAND IN XIN XLP XHP XBP VIB ONE "
                                                   "LFO3 LFO4 ENV2 ENV3 ENV4 ENV5 "
                                                   "RND2 RND3 RND4 ONST BND1 BND2 BND3 BND4 BND5 BND6 BND7 BND8 "
                                                   "BND9 BN10 BN11 BN12 BN13 BN14 BN15 BN16"));
            expectEquals(names(full, true), juce::String("OFF VEL IN VIB LFO3 RND2 ONST"));
            expectEquals(names(mod::SourceTable::standard(3, 0)).upToFirstOccurrenceOf(" RND2", false, false).fromLastOccurrenceOf(" ", false, false),
                         juce::String("LFO3"));
            // A stored SRC of before this phase means what it meant: the places up to the old end
            // of the list are the sources of the same number.
            for (int i = 0; i < mod::Src::offered(2); ++i)
                expectEquals(plain.source(i), i);
            for (int i = 0; i < mod::Src::offered(4, 4); ++i)
                expectEquals(full.source(i), i);
            expectEquals(plain.indexOf(mod::Src::Random2), 17);
            expectEquals(plain.indexOf(mod::Src::Onset), 20);
            expectEquals(plain.indexOf(mod::Src::Band1 + 15), 36);
            expectEquals(plain.indexOf(mod::Src::Lfo3), -1);
            expectEquals(plain.source(-1), (int) mod::Src::Off);
            expectEquals(plain.source(1000), (int) mod::Src::Off);

            // An engine's own come last, in the order published, as a group.
            const auto& mine = sources();
            expectEquals(mine.size(), plain.size() + 3);
            expectEquals(names(mine).fromFirstOccurrenceOf("BN16 ", false, false), juce::String("LANE WOB OPA"));
            expectEquals(names(mine, true), juce::String("OFF VEL IN VIB RND2 ONST LANE"));
            expectEquals(mine.published(), 3);
            expect(! mine.audio(LANE) && ! mine.audio(WOB) && mine.audio(OPA));
            expect(! mine.bipolar(LANE) && mine.bipolar(WOB) && mine.bipolar(OPA));
            expectEquals(mine.placeOfPublished(OPA), mine.size() - 1);
            expectEquals(mine.source(mine.size() - 1), own(OPA));
            expectEquals(mine.indexOfLabel("WOB"), mine.size() - 2);
            for (int i = 0; i < plain.size(); ++i)
                expectEquals(mine.source(i), plain.source(i), "what the engine adds moves nothing");

            // The cells: SRC and VIA show the engine's list, and Func + turn hops its groups.
            Rig g;
            expectEquals(g.r.numSources(), mine.size());
            ParamPages pages;
            g.r.appendPages(pages);
            const ParamSpec& src = pages[1].params[0];
            const ParamSpec& via = pages[1].params[3];
            expectEquals(juce::String(src.label), juce::String("SRC"));
            expectEquals(src.numChoices, mine.size());
            expectEquals(via.numChoices, mine.size());
            expectEquals(juce::String(src.choices[mine.size() - 1]), juce::String("OPA"));
            expectEquals(juce::String(via.choices[20]), juce::String("ONST"));
            expect(ChoiceGroups::find(src.choices) != nullptr, "the list is grouped");
            expect(ChoiceGroups::find(M::sources().labels()) != nullptr && ChoiceGroups::find(mod::SourceTable::standard(2, 0).labels()) != nullptr);
            logMessage("grouped choice lists registered in this process so far: " + juce::String(ChoiceGroups::used().load()) + " of " + juce::String((int) ChoiceGroups::MAX_LISTS));
            static const int stops[] = { 5, 10, 15, 17, 20, 37, 39 };
            float at = 0.0f;
            for (int stop : stops)
            {
                at = src.turned(at, 1, TurnStep::Coarse);
                expectEquals((int) at, stop, "Func + turn stops at " + juce::String(src.choices[stop]));
            }
            // A place past the end is the last one; a place is what the cell reads back.
            g.r.set(ModSlotParam::id(0, ModSlotParam::SRC), 1000.0f);
            expectEquals(g.r.get(ModSlotParam::id(0, ModSlotParam::SRC)), (float) (mine.size() - 1));
            g.r.set(ModSlotParam::id(0, ModSlotParam::VIA), (float) g.r.sourceIndex(mod::Src::Onset));
            expectEquals(g.r.get(ModSlotParam::id(0, ModSlotParam::VIA)), 20.0f);
        }

        beginTest("An LFO's TRIG: dsp/LFO.h's five, then the four that wait for the track input");
        {
            Rig g;
            ParamPages pages;
            g.r.appendPages(pages);
            const ParamSpec* trig = nullptr;
            for (auto& page : pages)
                if (page.name == "LFO1")
                    trig = &page.params[R::Trigger];
            expect(trig != nullptr);
            if (trig != nullptr)
            {
                juce::StringArray list;
                for (int i = 0; i < trig->numChoices; ++i)
                    list.add(trig->choices[i]);
                expectEquals(list.joinIntoString(" "), juce::String("FREE KEY ONE HALF HOLD XKEY XONE XHLF XHLD"));
                expectEquals(trig->maxVal, 8.0f);
                for (int i = 0; i < (int) LFO::TriggerMode::COUNT; ++i)
                    expectEquals(juce::String(trig->choices[i]), juce::String(LFO::triggerLabels()[i]), "the first five are the LFO's own, by index");
            }
            for (int t = 0; t < R::NUM_TRIGGERS; ++t)
            {
                g.r.set(R::lfoId(0, R::Trigger), (float) t);
                expectEquals(g.r.get(R::lfoId(0, R::Trigger)), (float) t);
                expect(R::triggeredByInput(t) == (t >= 5));
                expectEquals(R::lfoMode(t), t < 5 ? t : t - 4);
            }
            g.r.set(R::lfoId(0, R::Trigger), 99.0f);
            expectEquals(g.r.get(R::lfoId(0, R::Trigger)), 8.0f);
        }
    }

    // =============================================================================================
    void publishedTests()
    {
        const float near = 1.0e-5f;

        beginTest("An engine's own source at control rate: per voice, taken as it stands at the note-on, led under a note, as a VIA by its polarity");
        {
            Rig g;
            expect(! g.r.wants(LANE) && ! g.r.wants(WOB) && ! g.r.wants(OPA), "nobody reads them");
            g.v[0].publish(LANE, 0.8f);
            g.note(0);
            expect(! g.tick(0), "a published value by itself moves nothing: the voice's plain path");

            g.slot(0, own(LANE), AMP, 0.5f);
            g.v[0].publish(LANE, 0.8f);
            g.v[1].publish(LANE, 0.2f);
            g.note(0);
            g.note(1);
            g.chunk();
            expect(g.r.wants(LANE) && ! g.r.wants(WOB) && ! g.r.wants(OPA), "wants(k) is true for the one a slot reads");
            expect(g.run(0));
            expectWithinAbsoluteError(g.at(AMP, 0), 1.4f, near, "the note's first sub-block has it as it stands");
            expect(g.run(1));
            expectWithinAbsoluteError(g.at(AMP, 1), 1.1f, near, "each voice its own");

            // Under the note it is led (its whole swing in 4 ms), not jumped to.
            g.v[0].publish(LANE, 0.0f);
            g.tick(0);
            expect(g.at(AMP, 0) < 1.39f && g.at(AMP, 0) > 1.25f, "one sub-block later: on its way (" + juce::String(g.at(AMP, 0), 4) + ")");
            g.settle(0);
            expectWithinAbsoluteError(g.at(AMP, 0), 1.0f, near);
            // Held to -1 .. 1; not a number is 0.
            g.v[0].publish(LANE, 7.0f);
            g.settle(0);
            expectWithinAbsoluteError(g.at(AMP, 0), 1.5f, near);
            g.v[0].publish(LANE, std::numeric_limits<float>::quiet_NaN());
            g.settle(0);
            expectWithinAbsoluteError(g.at(AMP, 0), 1.0f, near);
            g.v[0].publish(99, 1.0f);       // (no such source)
            g.v[0].publish(-1, 1.0f);

            // As a VIA: a source that swings both ways is moved up, one that goes one way is as it is.
            Rig via;
            via.slot(0, mod::Src::One, CUT, 1.0f, own(WOB));
            via.v[0].publish(WOB, 0.0f);
            via.note(0);
            via.tick(0);
            expect(via.r.wants(WOB), "a VIA reads it too");
            expectWithinAbsoluteError(via.at(CUT), 2.0f, near, "WOB at 0 opens the slot half: 2 of 4 octaves");
            via.v[0].publish(WOB, -1.0f);
            via.settle(0);
            expectWithinAbsoluteError(via.at(CUT), 0.0f, near);
            via.slot(0, mod::Src::One, CUT, 1.0f, own(LANE));
            via.v[0].publish(LANE, 0.25f);
            via.settle(0);
            expectWithinAbsoluteError(via.at(CUT), 1.0f, near, "LANE at 0.25 opens it a quarter");
        }

        beginTest("wants(k): true while a slot reads the source and for a few milliseconds after the last one went");
        {
            Rig g;
            g.slot(0, own(OPA), AMP, 0.5f);
            g.note(0);
            g.tick(0);
            expect(g.r.wants(OPA));
            g.slot(0, mod::Src::Off, AMP, 0.5f);
            g.tick(0);
            expect(g.r.wants(OPA), "the voices go on handing it over while what the slot gave is faded");
            g.settle(0, 40);
            expect(! g.r.wants(OPA), "then nobody need work it out");
        }

        beginTest("An engine's own source as samples: per sample on a PerSample destination one sub-block later, its last sample elsewhere, per voice, silence at a note-on");
        {
            Rig g;
            g.slot(0, own(OPA), AMP, 0.5f);
            g.slot(1, own(OPA), RESO, 1.0f);
            std::array<float, 64> ramp {};
            for (int i = 0; i < 64; ++i)
                ramp[(size_t) i] = (float) (i - 8) / 16.0f;
            g.note(0);
            g.note(1);
            g.tick(0);
            for (int i = 0; i < g.n; ++i)
                expectWithinAbsoluteError(g.v[0].buffer(AMP)[i], 1.0f, near, "nothing handed over yet: silence");
            g.v[0].publishAudio(OPA, ramp.data(), 16);
            g.chunk();
            g.run(0);
            for (int i = 0; i < g.n; ++i)
                expectWithinAbsoluteError(g.v[0].buffer(AMP)[i], 1.0f + 0.5f * ramp[(size_t) i], near, "sample " + juce::String(i));
            expectWithinAbsoluteError(g.v[0].value(RESO), 0.5f * ramp[15], near, "a control-rate destination takes the last sample");
            // The other voice, worked on after it in the same sub-block, handed nothing over.
            g.run(1);
            for (int i = 0; i < g.n; ++i)
                expectWithinAbsoluteError(g.v[1].buffer(AMP)[i], 1.0f, near, "the other voice hears its own, which is silence");

            // A run shorter than the sub-block (the end of a host block that is not a whole number
            // of them): every sample is still heard exactly one whole sub-block after it was made
            // (U4-1; it was held at its last sample, a delay that changed with the host's block).
            g.v[0].publishAudio(OPA, ramp.data(), 4);
            g.tick(0);
            for (int i = 0; i < g.n; ++i)
                expectWithinAbsoluteError(g.v[0].buffer(AMP)[i], 1.0f + 0.5f * ramp[(size_t) (i < 12 ? i + 4 : i - 12)], near);
            // Held to -1 .. 1, and no number is silence.
            std::array<float, 16> wild {};
            wild.fill(3.0f);
            wild[5] = std::numeric_limits<float>::quiet_NaN();
            wild[6] = -std::numeric_limits<float>::infinity();
            g.v[0].publishAudio(OPA, wild.data(), 16);
            g.tick(0);
            for (int i = 0; i < g.n; ++i)
                expectWithinAbsoluteError(g.v[0].buffer(AMP)[i], i == 5 || i == 6 ? 1.0f : 1.5f, near);
            // Nothing: silence.
            g.v[0].publishAudio(OPA, nullptr, 16);
            g.tick(0);
            expectWithinAbsoluteError(g.v[0].buffer(AMP)[7], 1.0f, near);
            g.v[0].publishAudio(OPA, ramp.data(), 0);
            g.tick(0);
            expectWithinAbsoluteError(g.v[0].buffer(AMP)[7], 1.0f, near);
            // A new note does not hear what the last one made.
            g.v[0].publishAudio(OPA, wild.data(), 16);
            g.note(0);
            g.tick(0);
            expectWithinAbsoluteError(g.v[0].buffer(AMP)[0], 1.0f, near, "a note-on begins from silence");
            expectWithinAbsoluteError(g.v[0].buffer(AMP)[15], 1.0f, near);
        }
    }

    // =============================================================================================
    void randomTests()
    {
        beginTest("RAND, RND2, RND3, RND4: draws of the one per-note generator, independent, held for the note, the same again from the same seed");
        {
            Rig a, b, c;
            for (int i = 0; i < 2; ++i)
            {
                a.v[(size_t) i].seed(7, i);
                b.v[(size_t) i].seed(7, i);
                c.v[(size_t) i].seed(8, i);
            }
            spread::Generator reference;
            reference.seed(7, 0);
            std::set<int> seen;
            int sameAsOtherSeed = 0, sameAsOtherVoice = 0;
            for (int note = 0; note < 20; ++note)
            {
                reference.advance();
                for (auto* g : { &a, &b, &c })
                {
                    g->note(0, 40 + note);
                    g->note(1, 40 + note);
                }
                for (int k = 0; k < mod::Src::NUM_RANDOM; ++k)
                {
                    const int source = mod::Src::ofRandom(k);
                    const float value = a.v[0].source(source);
                    expect(value >= -1.0f && value <= 1.0f);
                    expect(value == b.v[0].source(source), "the same seed, the same notes: the same value");
                    expect(a.v[1].source(source) == b.v[1].source(source));
                    expect(value == reference.bipolar(spread::Free, k), "RAND and RND2 .. are spread::Free 0 .. 3");
                    sameAsOtherSeed += value == c.v[0].source(source) ? 1 : 0;
                    sameAsOtherVoice += value == a.v[1].source(source) ? 1 : 0;
                    seen.insert((int) std::lround(value * 1.0e6f));
                }
            }
            expectEquals((int) seen.size(), 80, "four values a note that have nothing to do with one another, new every note");
            expectEquals(sameAsOtherSeed, 0, "another seed: other values");
            expectEquals(sameAsOtherVoice, 0, "another voice: other values");
            expectEquals(mod::Src::ofRandom(0), (int) mod::Src::Random);
            expectEquals(mod::Src::ofRandom(3), (int) mod::Src::Random4);

            // A slot reads one of them: held for the note.
            Rig g;
            g.v[0].seed(3, 0);
            g.slot(0, mod::Src::Random3, AMP, 0.5f);
            g.note(0);
            g.tick(0);
            const float r3 = g.v[0].source(mod::Src::Random3);
            expect(r3 != 0.0f);
            expectWithinAbsoluteError(g.at(AMP), 1.0f + 0.5f * r3, 1.0e-5f);
            g.settle(0, 20);
            expectWithinAbsoluteError(g.at(AMP), 1.0f + 0.5f * r3, 1.0e-5f, "held for the note");
            // As a VIA it is moved up.
            g.slot(0, mod::Src::One, CUT, 1.0f, mod::Src::Random2);
            g.note(0);
            g.tick(0);
            expectWithinAbsoluteError(g.at(CUT), 4.0f * 0.5f * (g.v[0].source(mod::Src::Random2) + 1.0f), 1.0e-4f, "RND2 as a VIA swings both ways");
        }

        beginTest("The randoms from the voice's spread generator: SEED repeats them with the spread");
        {
            VoiceSpread section(125);
            Rig g;
            auto play = [&](int seed)
            {
                section.set(section.id(VoiceSpread::Seed), (float) seed);
                spread::Generator generator;
                generator.seed(section.seed(), 0);
                std::vector<float> out;
                for (int note = 0; note < 6; ++note)
                {
                    section.next(generator);
                    g.v[0].noteOn(g.r, 60 + note, 1.0f, generator);
                    expect(g.v[0].source(mod::Src::Random) == generator.bipolar(spread::Free, 0));
                    expect(g.v[0].source(mod::Src::Random4) == generator.bipolar(spread::Free, 3));
                    out.push_back(g.v[0].source(mod::Src::Random));
                    out.push_back(g.v[0].source(mod::Src::Random2));
                }
                return out;
            };
            const auto first = play(0), other = play(9), again = play(0);
            expect(first == again, "SEED back where it was: the same sequence");
            expect(first != other, "another SEED: another sequence");
            // The randoms are not the spread's own draws.
            spread::Generator generator;
            generator.seed(0, 0);
            generator.advance();
            expect(generator.bipolar(spread::Free, 0) != generator.bipolar(spread::Pitch) && generator.bipolar(spread::Free, 1) != generator.bipolar(spread::Cutoff));
        }

        beginTest("spread::Sequence: the draws made while a note sounds (the wavetable's RAND scan) repeat with the seed");
        {
            spread::Generator generator;
            generator.seed(3, 1);
            generator.advance();
            spread::Sequence q;
            q.start(generator);
            std::vector<float> first;
            for (int i = 0; i < 16; ++i)
                first.push_back(q.unipolar());
            expectEquals(q.count(), 16);
            std::set<int> seen;
            for (int i = 0; i < 16; ++i)
            {
                expect(first[(size_t) i] >= 0.0f && first[(size_t) i] < 1.0f);
                expect(first[(size_t) i] == generator.unipolar(spread::Scan, i), "draw n is a function of the note and n");
                seen.insert((int) (first[(size_t) i] * 1.0e6f));
            }
            expectEquals((int) seen.size(), 16);
            q.start(generator);
            expect(q.unipolar() == first[0], "started again on the same note: the same draws");
            expect(q.bipolar() == first[1] * 2.0f - 1.0f);
            generator.advance();
            q.start(generator);
            expect(q.unipolar() != first[0], "the next note: others");
            expect(generator.unipolar(spread::Scan, 0) != generator.unipolar(spread::Free, 0), "a stream of its own");
        }
    }

    // =============================================================================================
    // Old and new envelope under the same gestures; false at the first sample that differs.
    template <typename Gesture>
    bool sameAsOld(Gesture&& gesture, int samples)
    {
        OldEnvelope before;
        Envelope after;
        before.setSampleRate(SR);
        after.setSampleRate(SR);
        for (int i = 0; i < samples; ++i)
        {
            gesture(before, i);
            gesture(after, i);
            const float a = before.process(), b = after.process();
            if (std::memcmp(&a, &b, sizeof(float)) != 0 || (int) before.getStage() != (int) after.getStage())
                return false;
        }
        return true;
    }

    // Samples until the envelope's stage is no longer `stage`.
    static int stageLength(Envelope& e, Envelope::Stage stage)
    {
        int n = 0;
        while (e.getStage() == stage && n < 10000000)
        {
            e.process();
            ++n;
        }
        return n;
    }

    void envelopeTests()
    {
        beginTest("Envelope: at a time scale of 1 it is the old class, bit for bit, through notes, releases, re-strikes and every kind of change under sound");
        {
            // A fixed run of gestures on a counter-based random.
            int trials = 0;
            for (uint32_t seed = 1; seed <= 12; ++seed)
            {
                auto random = [seed](int i, uint32_t salt) { return (float) (spread::mix(seed * 7919u + (uint32_t) i * 31u + salt) >> 8) / 16777216.0f; };
                const bool same = sameAsOld([&](auto& e, int i)
                {
                    if (i == 0)
                    {
                        typename std::remove_reference<decltype(e)>::type::Params p;
                        p.attack = 0.0005f + 0.05f * random(0, 1);
                        p.decay = 0.002f + 0.1f * random(0, 2);
                        p.sustain = random(0, 3);
                        p.release = 0.001f + 0.08f * random(0, 4);
                        p.attackCurve = seed % 3 == 0 ? 0.0f : 2.0f * random(0, 5) - 1.0f;
                        p.decayCurve = seed % 3 == 1 ? 0.0f : 2.0f * random(0, 6) - 1.0f;
                        p.releaseCurve = seed % 3 == 2 ? 0.0f : 2.0f * random(0, 7) - 1.0f;
                        e.setParams(p);
                        e.noteOn();
                    }
                    if (i % 613 != 0 || i == 0)
                        return;
                    switch ((int) (random(i, 8) * 9.0f))
                    {
                        case 0: e.noteOn(); break;
                        case 1: e.noteOff(); break;
                        case 2: e.setAttack(0.0005f + 0.05f * random(i, 9)); break;
                        case 3: e.setDecay(0.002f + 0.1f * random(i, 10)); break;
                        case 4: e.setSustain(random(i, 11)); break;
                        case 5: e.setRelease(0.001f + 0.08f * random(i, 12)); break;
                        case 6:
                        {
                            auto p = e.getParams();
                            p.decayCurve = 2.0f * random(i, 13) - 1.0f;
                            p.attackCurve = 2.0f * random(i, 14) - 1.0f;
                            p.sustain = random(i, 15);
                            e.setParams(p);
                            break;
                        }
                        case 7: e.reset(); e.noteOn(); break;
                        default: break;
                    }
                }, 60000);
                expect(same, "gesture run " + juce::String((int) seed));
                ++trials;
            }
            expectEquals(trials, 12);
            // ... and with the scale set to 1 by name, and after it has been elsewhere.
            OldEnvelope before;
            Envelope after;
            before.setSampleRate(SR);
            after.setSampleRate(SR);
            after.setTimeScale(3.0f);
            after.setTimeScale(1.0f);
            before.noteOn();
            after.noteOn();
            bool same = true;
            for (int i = 0; i < 30000; ++i)
            {
                if (i == 12000) { before.noteOff(); after.noteOff(); }
                same = same && before.process() == after.process();
            }
            expect(same);
            expectEquals(Envelope().getTimeScale(), 1.0f);
        }

        beginTest("Envelope: the time scale multiplies every stage time and leaves the params as set");
        {
            Envelope::Params p;
            p.attack = 0.02f;
            p.decay = 0.05f;
            p.sustain = 0.5f;
            p.release = 0.04f;
            for (float scale : { 0.25f, 1.0f, 2.0f, 8.0f })
            {
                Envelope e;
                e.setSampleRate(SR);
                e.setParams(p);
                e.setTimeScale(scale);
                e.noteOn();
                const int attack = stageLength(e, Envelope::Stage::Attack);
                const int decay = stageLength(e, Envelope::Stage::Decay);
                e.noteOff();
                const int release = stageLength(e, Envelope::Stage::Release);
                const juce::String at = " at x" + juce::String(scale);
                expectWithinAbsoluteError((float) attack, scale * 0.02f * (float) SR, 3.0f, "attack" + at);
                expectWithinAbsoluteError((float) decay, scale * 0.05f * (float) SR, 3.0f, "decay" + at);
                expectWithinAbsoluteError((float) release, scale * 0.04f * (float) SR, 3.0f, "release" + at);
                expectEquals(e.getParams().attack, 0.02f, "the params are as set");
                expectEquals(e.getParams().release, 0.04f);
            }
            // Held to 1/64 .. 64; no number is no scale.
            Envelope e;
            e.setTimeScale(0.0f);
            expectEquals(e.getTimeScale(), Envelope::MIN_TIME_SCALE);
            e.setTimeScale(-3.0f);
            expectEquals(e.getTimeScale(), Envelope::MIN_TIME_SCALE);
            e.setTimeScale(1.0e9f);
            expectEquals(e.getTimeScale(), Envelope::MAX_TIME_SCALE);
            e.setTimeScale(std::numeric_limits<float>::quiet_NaN());
            expectEquals(e.getTimeScale(), 1.0f);
            e.setTimeScale(std::numeric_limits<float>::infinity());
            expectEquals(e.getTimeScale(), Envelope::MAX_TIME_SCALE);
            // The shortest stage is still a millisecond.
            e.setSampleRate(SR);
            p.attack = 0.002f;
            e.setParams(p);
            e.setTimeScale(Envelope::MIN_TIME_SCALE);
            e.noteOn();
            expectWithinAbsoluteError((float) stageLength(e, Envelope::Stage::Attack), 0.001f * (float) SR, 2.0f);
        }

        beginTest("Envelope: the time scale set under a sounding note changes the speed from where the stage is: the level never jumps");
        {
            Envelope::Params p;
            p.attack = 0.05f;
            p.decay = 0.08f;
            p.sustain = 0.4f;
            p.release = 0.1f;
            for (float curve : { 0.0f, -0.8f, 0.8f })
            {
                p.attackCurve = p.decayCurve = p.releaseCurve = curve;
                Envelope e;
                e.setSampleRate(SR);
                e.setParams(p);
                e.noteOn();
                float last = 0.0f, largest = 0.0f;
                bool upInAttack = true;
                for (int i = 0; i < 40000; ++i)
                {
                    if (i % 37 == 0)
                        e.setTimeScale(std::exp2(4.0f * ((float) (spread::mix((uint32_t) i) >> 8) / 16777216.0f) - 2.0f));    // 1/4 .. 4
                    if (i == 20000)
                        e.noteOff();
                    const bool attacking = e.getStage() == Envelope::Stage::Attack;
                    const float level = e.process();
                    largest = std::max(largest, std::abs(level - last));
                    if (attacking && e.getStage() == Envelope::Stage::Attack)
                        upInAttack = upInAttack && level >= last;
                    last = level;
                }
                // The fastest it can go: the whole attack in a quarter of 50 ms, steepened by the
                // curve at most (1 + |curve|) times.
                const float fastest = (1.0f + std::abs(curve)) / (0.25f * 0.05f * (float) SR);
                expect(largest <= fastest * 1.05f, "curve " + juce::String(curve) + ": the largest step is " + juce::String(largest, 6)
                                                   + ", a stage's own is " + juce::String(fastest, 6));
                expect(upInAttack, "an attack only rises");
                expect(! e.isActive(), "and it ends");
            }
        }

        beginTest("ETIM and FTIM: envelope time as a destination, a factor of up to eight either way");
        {
            const auto& t = table();
            expectEquals(t.ampTime(), (int) ETIM);
            expectEquals(t.filterTime(), (int) FTIM);
            expectEquals(juce::String(t[ETIM].label), juce::String("ETIM"));
            expectEquals(juce::String(t[FTIM].label), juce::String("FTIM"));
            expect(! t[ETIM].perSample && t[ETIM].exp && t[ETIM].base == 1.0f);
            mod::DestTable none;
            expectEquals(none.ampTime(), -1);
            expectEquals(none.filterTime(), -1);
            mod::DestTable ampOnly;
            ampOnly.envTimes(false);
            expect(ampOnly.ampTime() == 1 && ampOnly.filterTime() == -1);

            Rig g;
            g.slot(0, mod::Src::One, ETIM, 1.0f);
            g.note(0);
            g.tick(0);
            expectWithinAbsoluteError(g.told(ETIM), 8.0f, 1.0e-4f, "full depth: eight times as long");
            expectEquals(g.told(FTIM), -1000.0f, "the filter's is another destination");
            g.slot(0, mod::Src::One, ETIM, -1.0f);
            g.settle(0);
            expectWithinAbsoluteError(g.told(ETIM), 0.125f, 1.0e-5f);
            g.slot(0, mod::Src::One, ETIM, 1.0f);
            g.slot(1, mod::Src::One, ETIM, 1.0f);
            g.settle(0);
            expectWithinAbsoluteError(g.told(ETIM), 32.0f, 1.0e-3f, "held to 32");
            g.slot(1, mod::Src::Velocity, FTIM, 0.5f);
            g.settle(0);
            expectWithinAbsoluteError(g.told(FTIM), std::exp2(1.5f), 1.0e-4f, "VEL at 1 by half: 1.5 octaves");
            // Let go: the factor comes back to 1 without a jump, and 1 is handed over at the end.
            g.slot(0, mod::Src::Off, ETIM, 1.0f);
            g.tick(0);
            float last = g.told(ETIM);
            expectWithinAbsoluteError(last, 8.0f, 1.0e-3f, "in the sub-block the slot goes, the factor is where it was");
            g.tick(0);
            expect(g.told(ETIM) > 3.0f && g.told(ETIM) < 7.9f, "then it is carried off, not dropped (" + juce::String(g.told(ETIM), 3) + ")");
            bool gaveOne = false;
            for (int i = 0; i < 80; ++i)
            {
                g.tick(0);
                const float now = g.told(ETIM);
                if (now > -999.0f)
                {
                    expect(now <= last + 1.0e-4f);
                    last = now;
                    gaveOne = gaveOne || now == 1.0f;
                }
            }
            expect(gaveOne, "the set value, 1, is given back");
        }
    }

    // =============================================================================================
    void inputTests()
    {
        const auto twoHits = [](long i) { return burst(i, 0.25, 0.05, 0.5f) + burst(i, 0.60, 0.05, 0.25f); };

        beginTest("ONST: 1 when a sound begins in the track input, falling to 1 / e in a tenth of a second; heard only while somebody listens");
        {
            Rig g;
            g.input = twoHits;
            g.slot(0, mod::Src::Onset, AMP, 0.5f);
            g.note(0);
            std::vector<long> heardAt;
            float beforeFirst = 0.0f, atPeak = 0.0f, tenthLater = 0.0f, ampTenthLater = 0.0f;
            while (g.position < (long) (0.9 * SR))
            {
                g.tick(0);
                if (g.r.onsetHeard())
                    heardAt.push_back(g.position - 16);
                if (g.position == (long) (0.24 * SR)) beforeFirst = g.r.onsetLevel();
                if (! heardAt.empty() && g.position - 16 == heardAt[0]) atPeak = g.r.onsetLevel();
                if (! heardAt.empty() && g.position - 16 == heardAt[0] + 4800)
                {
                    tenthLater = g.r.onsetLevel();
                    ampTenthLater = g.at(AMP);
                }
            }
            expectEquals((int) heardAt.size(), 2, "two hits, two onsets");
            if (heardAt.size() == 2)
            {
                expect(heardAt[0] >= (long) (0.25 * SR) - 16 && heardAt[0] <= (long) (0.25 * SR) + 96,
                       "the first within 2 ms of where the sound begins (" + juce::String((double) heardAt[0] / SR, 4) + " s)");
                expect(heardAt[1] >= (long) (0.60 * SR) - 16 && heardAt[1] <= (long) (0.60 * SR) + 96,
                       "the second, half as loud, as well (" + juce::String((double) heardAt[1] / SR, 4) + " s)");
            }
            if (heardAt.size() == 2)
                logMessage("hits at 0.2500 s and 0.6000 s heard in the sub-blocks from " + juce::String((double) heardAt[0] / SR, 4) + " s and "
                           + juce::String((double) heardAt[1] / SR, 4) + " s; ONST 0.1 s on: " + juce::String(tenthLater, 4));
            expectEquals(beforeFirst, 0.0f, "nothing before the first hit");
            expectEquals(atPeak, 1.0f);
            expectWithinAbsoluteError(tenthLater, std::exp(-1.0f), 0.01f, "1 / e a tenth of a second on");
            expectWithinAbsoluteError(ampTenthLater, 1.0f + 0.5f * tenthLater, 0.01f, "the slot gives it to its destination");

            // The value is led to, not jumped to: one sub-block after the hit the level is on its way.
            Rig led;
            led.input = twoHits;
            led.slot(0, mod::Src::Onset, AMP, 1.0f);
            led.note(0);
            float largest = 0.0f, last = 1.0f;
            while (led.position < (long) (0.5 * SR))
            {
                led.tick(0);
                for (int i = 0; i < led.n; ++i)
                {
                    largest = std::max(largest, std::abs(led.v[0].buffer(AMP)[i] - last));
                    last = led.v[0].buffer(AMP)[i];
                }
            }
            expect(largest > 0.001f && largest < 0.012f, "the level goes up by " + juce::String(largest, 5) + " a sample at most (its whole swing in 4 ms)");

            // Nobody listens: nothing is heard, and nothing is worked out.
            Rig deaf;
            deaf.input = twoHits;
            deaf.note(0);
            int heard = 0;
            float level = 0.0f;
            while (deaf.position < (long) (0.9 * SR))
            {
                deaf.tick(0);
                heard += deaf.r.onsetHeard() ? 1 : 0;
                level = std::max(level, deaf.r.onsetLevel());
            }
            expectEquals(heard, 0);
            expectEquals(level, 0.0f);
            // The engine itself asks (an envelope it restarts on a hit).
            Rig asked;
            asked.input = twoHits;
            asked.r.needOnsets(true);
            heard = 0;
            while (asked.position < (long) (0.9 * SR))
            {
                asked.tick(0);
                heard += asked.r.onsetHeard() ? 1 : 0;
            }
            expectEquals(heard, 2, "needOnsets(true): onsetHeard() without any slot");
            asked.r.needOnsets(false);
            asked.tick(0);
            expectEquals(asked.r.onsetLevel(), 0.0f, "and put away when nobody asks any more");
            // Not connected: silence.
            Rig alone;
            alone.slot(0, mod::Src::Onset, AMP, 0.5f);
            alone.note(0);
            alone.settle(0, 200);
            expect(! alone.r.onsetHeard() && alone.r.onsetLevel() == 0.0f);
            expectWithinAbsoluteError(alone.at(AMP), 1.0f, 1.0e-6f);
        }

        beginTest("An LFO at XKEY begins again on a hit in the track input, in every voice at once, and not on a note; at KEY it does not hear the input");
        {
            const auto oneHit = [](long i) { return burst(i, 0.30, 0.05, 0.5f); };
            const int saw = (int) LFO::Shape::Saw;
            // A 2 Hz ramp from -1 to 1 on the level: AMP = 2 x the place in the cycle.
            Rig x;
            x.input = oneHit;
            x.lfo(0, AMP, 1.0f, saw, 5, 2.0f);         // XKEY
            x.note(0);
            x.until(0.29);
            expectWithinAbsoluteError(x.at(AMP, 0), 2.0f * 0.58f, 0.03f, "before the hit: running since it was set");
            x.until(0.40);
            expectWithinAbsoluteError(x.at(AMP, 0), 2.0f * 0.2f, 0.03f, "a tenth of a second after the hit: a fifth of a cycle");
            // The voice that did not sound was restarted with it: its note is in step, and the
            // note-on itself restarts nothing.
            x.note(1);
            x.chunk();
            x.run(0);
            const float sounding = x.at(AMP, 0);
            x.run(1);
            expectWithinAbsoluteError(x.at(AMP, 1), sounding, 0.01f, "the other voice's note joins in step");
            expect(x.at(AMP, 1) > 0.3f, "a note-on does not restart an LFO that waits for the input");

            Rig key;
            key.input = oneHit;
            key.lfo(0, AMP, 1.0f, saw, 1, 2.0f);       // KEY
            key.note(0);
            key.until(0.40);
            expectWithinAbsoluteError(key.at(AMP, 0), 2.0f * 0.8f, 0.03f, "KEY: the hit is not heard");
            key.note(0);
            key.tick(0);
            expect(key.at(AMP, 0) < 0.05f, "... and a note restarts it");
        }

        beginTest("An LFO at XONE rests at the end of its cycle until the first hit, runs once from it, and rests again");
        {
            const auto oneHit = [](long i) { return burst(i, 0.30, 0.05, 0.5f); };
            Rig g;
            g.input = oneHit;
            g.lfo(0, AMP, 1.0f, (int) LFO::Shape::Saw, 6, 2.0f);      // XONE, half a second long
            g.note(0);
            g.tick(0);
            expectWithinAbsoluteError(g.at(AMP), 2.0f, 1.0e-4f, "at rest from the note's first sub-block");
            g.until(0.29);
            expectWithinAbsoluteError(g.at(AMP), 2.0f, 1.0e-4f, "... to the hit");
            g.until(0.55);
            expectWithinAbsoluteError(g.at(AMP), 2.0f * 0.5f, 0.03f, "a quarter of a second after it: half-way");
            g.until(0.95);
            expectWithinAbsoluteError(g.at(AMP), 2.0f, 1.0e-4f, "and at rest again when the cycle is over");
        }

        beginTest("BND1 .. BN16: the level of the track input in the vocoder's sixteen bands, worked out only while a slot reads one");
        {
            const float centre = voc::bandHz(5);
            float level = 1.0f;
            bool sounding = true;
            const auto tone = [&](long i) { return sounding ? level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * (double) centre * (double) i / SR) : 0.0f; };
            Rig g;
            g.input = tone;
            g.note(0);
            g.until(0.2);
            for (int b = 0; b < 16; ++b)
                expectEquals(g.r.bandLevel(b), 0.0f, "nobody reads a band: none is worked out");
            g.slot(0, mod::Src::Band1 + 5, AMP, 0.5f);
            g.until(0.5);
            const float full = g.r.bandLevel(5);
            expect(full > 0.85f && full <= 1.0f, "a full-scale sine at the band's centre reads about 1 (" + juce::String(full, 3) + ")");
            expectWithinAbsoluteError(g.at(AMP), 1.0f + 0.5f * full, 0.01f, "the slot gives it to its destination");
            expect(g.r.bandLevel(4) < 0.5f * full && g.r.bandLevel(6) < 0.5f * full,
                   "the neighbours hear less (" + juce::String(g.r.bandLevel(4), 3) + ", " + juce::String(g.r.bandLevel(6), 3) + ")");
            expect(g.r.bandLevel(0) < 0.02f && g.r.bandLevel(15) < 0.02f, "the far bands nothing");
            logMessage("full-scale sine at band 6's centre (" + juce::String(centre, 1) + " Hz): BND6 " + juce::String(full, 3) + ", BND5 "
                       + juce::String(g.r.bandLevel(4), 3) + ", BND7 " + juce::String(g.r.bandLevel(6), 3) + ", BND1 " + juce::String(g.r.bandLevel(0), 4));
            level = 0.5f;
            g.until(1.0);
            expectWithinAbsoluteError(g.r.bandLevel(5), 0.5f * full, 0.03f, "half the level, half the reading");
            sounding = false;
            g.until(1.4);
            expect(g.r.bandLevel(5) < 0.02f, "the input stops: the level falls (" + juce::String(g.r.bandLevel(5), 4) + ")");
            sounding = true;
            level = 1.0f;
            g.until(1.7);
            expect(g.r.bandLevel(5) > 0.85f);
            // The slot goes: the analysis is put away.
            g.slot(0, mod::Src::Off, AMP, 0.5f);
            g.until(1.75);
            for (int b = 0; b < 16; ++b)
                expectEquals(g.r.bandLevel(b), 0.0f);

            // The first band and the last, each read by name; as a VIA a band is one-way.
            for (int band : { 0, 15 })
            {
                const float hz = voc::bandHz(band);
                Rig ends;
                ends.input = [hz](long i) { return (float) std::sin(2.0 * juce::MathConstants<double>::pi * (double) hz * (double) i / SR); };
                ends.slot(0, mod::Src::One, CUT, 1.0f, mod::Src::Band1 + band);
                ends.note(0);
                ends.until(0.4);
                expect(ends.r.bandLevel(band) > 0.8f, juce::String(mod::Src::labels()[mod::Src::Band1 + band]) + " hears " + juce::String(hz, 0) + " Hz");
                expectWithinAbsoluteError(ends.at(CUT), 4.0f * ends.r.bandLevel(band), 0.05f);
                expect(ends.r.bandLevel(band == 0 ? 15 : 0) < 0.02f);
            }
            // Whatever comes in, a level from 0 to 1 comes out.
            Rig wild;
            wild.input = [](long i) { return i % 7 == 0 ? std::numeric_limits<float>::quiet_NaN() : (i % 5 == 0 ? 1.0e30f : -3.0f); };
            wild.slot(0, mod::Src::Band1 + 2, AMP, 1.0f);
            wild.slot(1, mod::Src::Onset, CUT, 1.0f);
            wild.note(0);
            bool held = true;
            while (wild.position < (long) (0.3 * SR))
            {
                wild.tick(0);
                for (int b = 0; b < 16; ++b)
                    held = held && wild.r.bandLevel(b) >= 0.0f && wild.r.bandLevel(b) <= 1.0f;
                for (int i = 0; i < wild.n; ++i)
                    held = held && std::isfinite(wild.v[0].buffer(AMP)[i]) && std::isfinite(wild.v[0].buffer(CUT)[i]);
            }
            expect(held);
        }
    }

    // =============================================================================================
    void movedCodeTests()
    {
        beginTest("The one onset detector (dyn::Onset) hears what the effects' own (fxm::Onset) heard, sample for sample");
        {
            expect((std::is_same<fxm::Onset, dyn::Onset>::value), "the effects' is this one");
            expect((std::is_same<fxm::Trigger, dyn::Trigger>::value));
            struct Setting { bool jumps; float release; double rate; };
            for (const auto& s : { Setting { true, 30.0f, 48000.0 }, Setting { false, 100.0f, 48000.0 }, Setting { true, 30.0f, 96000.0 }, Setting { true, 5.0f, 44100.0 } })
            {
                OldOnset before;
                dyn::Onset after;
                before.prepare(s.rate, s.jumps, s.release);
                after.prepare(s.rate, s.jumps, s.release);
                int onsets = 0, differences = 0;
                const int total = (int) (4.0 * s.rate);
                for (int i = 0; i < total; ++i)
                {
                    // Hits of several levels, a held note with hits over it, noise that swells.
                    const double t = (double) i / s.rate;
                    const double beat = std::fmod(t, 0.125);
                    float level = 0.0f;
                    if (t < 1.5)       level = beat < 0.03 ? (float) ((0.1 + 0.4 * std::fmod(t * 3.0, 1.0)) * std::abs(std::sin(900.0 * t))) : 0.0f;
                    else if (t < 3.0)  level = (float) (0.2 * std::abs(std::sin(380.0 * t)) + (beat < 0.02 ? 0.6 : 0.0));
                    else               level = (float) ((t - 3.0) * (double) (spread::mix((uint32_t) i) >> 8) / 16777216.0);
                    const bool a = before.hears(level), b = after.hears(level);
                    onsets += a ? 1 : 0;
                    differences += a != b ? 1 : 0;
                    differences += before.fast.y != after.level() || before.floor.y != after.floorLevel() ? 1 : 0;
                }
                expectEquals(differences, 0);
                expect(onsets >= (s.jumps ? 10 : 1), juce::String(onsets) + " onsets were there to hear");
            }
        }

        beginTest("VOC, its analysis moved out as voc::Analysis, is the old vocoder sample for sample (BAND, TUNE, frozen, modulated, the source going and coming)");
        {
            OldVocoder before;
            Vocoder after;
            before.prepare(SR, 64);
            after.prepare(SR, 64);
            expectEquals(Vocoder::NUM_BANDS, OldVocoder::NUM_BANDS);
            for (int b = 0; b < 16; ++b)
                expect(Vocoder::bandHz(b) == OldVocoder::bandHz(b) && voc::bandHz(b) == OldVocoder::bandHz(b));
            expect(Vocoder::bandQ() == OldVocoder::bandQ());
            const int block = 64, blocks = (int) (3.2 * SR) / block;
            std::vector<float> modulator((size_t) block);
            std::array<std::vector<float>, 4> io;
            for (auto& ch : io)
                ch.resize((size_t) block);
            uint32_t noise = 12345u;
            long differences = 0;
            double energy = 0.0;
            bool levelsSame = true;
            for (int b = 0; b < blocks; ++b)
            {
                const double t0 = (double) (b * block) / SR;
                const auto both = [&](const std::function<void(EffectBase&)>& fn) { fn(before); fn(after); };
                if (b == (int) (0.3 * SR) / block)  both([](EffectBase& e) { e.setExtra(Vocoder::Freeze, 1.0f); });
                if (b == (int) (0.5 * SR) / block)  both([](EffectBase& e) { e.setExtra(Vocoder::Freeze, 0.0f); e.setParameter(Vocoder::Noise, 0.4f); });
                if (b == (int) (0.7 * SR) / block)  both([](EffectBase& e) { e.setParameter(Vocoder::Band, 7.0f); e.modulateCell(Vocoder::ReleaseCell, 0.5f); });
                if (b == (int) (0.9 * SR) / block)  both([](EffectBase& e) { e.setParameter(Vocoder::Mode, 1.0f); });
                if (b == (int) (1.3 * SR) / block)  both([](EffectBase& e) { e.setExtra(Vocoder::BodyAt, 4.5f); e.setExtra(Vocoder::Freeze, 1.0f); e.setPitch(220.0f); });
                if (b == (int) (1.6 * SR) / block)  both([](EffectBase& e) { e.setExtra(Vocoder::Freeze, 0.0f); e.setParameter(Vocoder::Mix, 0.7f); });
                if (b == (int) (2.0 * SR) / block)  both([](EffectBase& e) { e.setParameter(Vocoder::Mode, 0.0f); e.setEnabled(false); });
                if (b == (int) (2.1 * SR) / block)  both([](EffectBase& e) { e.setEnabled(true); });
                const bool connected = ! (t0 > 1.05 && t0 < 1.2) && ! (t0 > 1.8 && t0 < 1.85);
                for (int i = 0; i < block; ++i)
                {
                    const double t = t0 + (double) i / SR;
                    // A voice-like modulator: a buzz through a moving formant, in syllables, with rests.
                    // From 2.3 s on nobody speaks: the analysis rings out and is put to rest (what
                    // it holds when it is, and when, is part of what must be the same).
                    const double syllable = t < 2.3 && std::fmod(t, 0.31) < 0.2 ? 1.0 : 0.0;
                    modulator[(size_t) i] = (float) (syllable * 0.6 * std::sin(2.0 * juce::MathConstants<double>::pi * (300.0 + 2500.0 * std::fmod(t, 0.31)) * t)
                                                     * (0.5 + 0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * 110.0 * t)));
                    const float carrierL = (float) (2.0 * std::fmod(110.0 * t, 1.0) - 1.0) * 0.5f, carrierR = dsp::xorshiftNoise(noise) * 0.3f;
                    io[0][(size_t) i] = io[2][(size_t) i] = carrierL;
                    io[1][(size_t) i] = io[3][(size_t) i] = carrierR;
                }
                before.setInput(connected ? modulator.data() : nullptr, connected ? modulator.data() : nullptr, connected ? block : 0);
                after.setInput(connected ? modulator.data() : nullptr, connected ? modulator.data() : nullptr, connected ? block : 0);
                before.processBlock(io[0].data(), io[1].data(), block);
                after.processBlock(io[2].data(), io[3].data(), block);
                for (int i = 0; i < block; ++i)
                {
                    differences += std::memcmp(&io[0][(size_t) i], &io[2][(size_t) i], sizeof(float)) != 0 ? 1 : 0;
                    differences += std::memcmp(&io[1][(size_t) i], &io[3][(size_t) i], sizeof(float)) != 0 ? 1 : 0;
                    energy += (double) io[2][(size_t) i] * io[2][(size_t) i];
                }
                for (int band = 0; band < 16; ++band)
                    levelsSame = levelsSame && before.bandLevel(band) == after.bandLevel(band);
                levelsSame = levelsSame && before.modesAwake() == after.modesAwake();
            }
            expectEquals((int) differences, 0, "samples that differ");
            expect(levelsSame, "the band levels and what is awake, block by block");
            expect(energy > 100.0, "and there was something to compare");
        }
    }

    // =============================================================================================
    void idTests()
    {
        beginTest("Ids: 158-189 are the modulation router's on every engine (LFO1, LFO2, STRIP, XIN), 773-779 hold no cell, and BODY collides with nothing");
        {
            // (once: no id on two cells. FM and the drum kit show a cell on more than one page.)
            struct Named { const char* name; std::unique_ptr<EngineBase> engine; bool once; };
            std::vector<Named> engines;
            engines.push_back({ "macro", std::make_unique<MacroOscEngine>(), true });
            engines.push_back({ "wave sequencer", std::make_unique<WaveSeqEngine>(), true });
            engines.push_back({ "FM", std::make_unique<FMEngine>(), false });
            engines.push_back({ "wavetable", std::make_unique<WavetableEngine>(), false });
            engines.push_back({ "sampler", std::make_unique<SamplerEngine>(), false });
            engines.push_back({ "drums", std::make_unique<DrumEngine>(), false });
            engines.push_back({ "resonator", std::make_unique<ResonatorEngine>(), false });
            for (auto& e : engines)
            {
                std::set<int> ids;
                int inRouterBlock = 0;
                for (auto& page : e.engine->getParamPages())
                    for (auto& cell : page.params)
                    {
                        if (! cell.isValid())
                            continue;
                        const juce::String where = juce::String(e.name) + " " + page.name + " " + cell.label + " (" + juce::String(cell.id) + ")";
                        expect(EngineParamRange::contains(cell.id), where + " is an engine id");
                        expect(ids.insert(cell.id).second || ! e.once, where + " is used once");
                        if (cell.id >= R::LFO_BASE && cell.id < R::XIN_END)
                        {
                            ++inRouterBlock;
                            const bool lfo = cell.id < R::STRIP_BASE, strip = cell.id >= R::STRIP_BASE && cell.id < R::XIN_BASE;
                            expect(lfo ? page.name.startsWith("LFO") : (strip ? page.name == "STRIP" : page.name == "XIN"), where + " is on the router's page");
                        }
                        expect(cell.id < 773 || cell.id > 779, where + ": 773-779 are kept free for the router");
                    }
                expect(inRouterBlock >= 24, juce::String(e.name) + " has its LFO and STRIP pages there");
            }
            // The wave sequencer's BODY page, which was at 182-189: in the engine's own block.
            expectEquals((int) WaveSeqEngine::BodyBase, 133);
            WaveSeqEngine w;
            int bodyCells = 0;
            for (auto& page : w.getParamPages())
                if (page.name == "BODY")
                    for (int c = 0; c < 8; ++c)
                    {
                        expectEquals(page.params[(size_t) c].id, 133 + c);
                        ++bodyCells;
                    }
            expectEquals(bodyCells, 8);
            w.setParameter(WaveSeqEngine::BodyBase + VoiceBody::Mix, 0.7f);
            expectEquals(w.getParameter(WaveSeqEngine::BodyBase + VoiceBody::Mix), 0.7f);
            expect(w.getParameterName(WaveSeqEngine::BodyBase + VoiceBody::Mix).startsWith("Body"));
            expect(! mod::Router::owns(WaveSeqEngine::BodyBase) && mod::Router::owns(182) && mod::Router::owns(189));
        }
    }

    // =============================================================================================
    static int placeOf(const char* label) { return M::sources().indexOfLabel(label); }

    // The sounding voice's amp envelope level after `seconds` of a note on a new engine.
    static float levelAfter(M& e, double seconds)
    {
        e.noteOn(69, 1.0f, -1);
        Old::render(e, seconds);
        for (int v = 0; v < 8; ++v)
            if (e.getVoice(v).isActive())
                return e.getVoice(v).getAmpEnvelope().getCurrentLevel();
        return -1.0f;
    }

    void macroTests()
    {
        beginTest("Macro oscillator: OSC, the model's own output, as an audio-rate source (on the level it is the wave times itself: the octave appears, as strong as the law says)");
        {
            const auto& list = M::sources();
            expectEquals(list.published(), 1);
            expectEquals(placeOf("OSC"), list.size() - 1);
            expect(list.audio(V::PubOsc) && list.bipolar(V::PubOsc));
            M pagesOf(8);
            const auto pages = pagesOf.getParamPages();
            for (auto& page : pages)
                if (page.name == "SLT1")
                {
                    expectEquals(juce::String(page.params[0].choices[page.params[0].numChoices - 1]), juce::String("OSC"));
                    expectEquals(juce::String(page.params[3].choices[page.params[3].numChoices - 1]), juce::String("OSC"));
                }

            // A sine at A5 on its own level by 60 %: s (1 + 0.6 s') = s + 0.3 (cos d - cos(2wt - d)),
            // s' the sine one sub-block earlier: the octave at 0.3 of the fundamental.
            for (int block : { 128, 100, 16 })
            {
                auto plain = Old::sine(SR, block);
                plain->noteOn(81, 1.0f, -1);
                const auto dry = Old::render(*plain, 0.6, nullptr, SR, block);
                auto e = Old::sine(SR, block);
                Old::freeSlot(*e, 0, placeOf("OSC"), V::LfoAmp, 0.6f);
                e->noteOn(81, 1.0f, -1);
                const auto y = Old::render(*e, 0.6, nullptr, SR, block);
                const int a = 9600, b = (int) y.size() - 64;
                const double f1 = Old::partial(y, a, b, 880.0), f2 = Old::partial(y, a, b, 1760.0);
                const double d1 = Old::partial(dry, a, b, 880.0), d2 = Old::partial(dry, a, b, 1760.0);
                expect(d2 / d1 < 0.002, "without the slot there is no octave (" + juce::String(d2 / d1, 5) + ")");
                // (A block that is not a whole number of sub-blocks used to hold a short run at its
                // last sample, 0.283 at blocks of 100; since U4-1 every sample is a sub-block late.)
                const double tolerance = 0.015;
                expectWithinAbsoluteError(f2 / f1, 0.3, tolerance, "blocks of " + juce::String(block) + ": octave / fundamental " + juce::String(f2 / f1, 4));
                expectWithinAbsoluteError(f1 / d1, 1.0, 0.03, "the fundamental stays");
                logMessage("OSC > AMP at 60 %, blocks of " + juce::String(block) + ": octave / fundamental " + juce::String(f2 / f1, 4) + " (the law: 0.3)");
            }

            // On the cutoff it is filter FM by the voice's own oscillator: the sound changes.
            auto filtered = [](float amount)
            {
                auto e = Old::sine();
                e->setParameter(M::TimbreParam, 0.0f);
                e->setParameter(M::HarmonicsParam, 0.5f);       // a saw
                e->setParameter(M::FilterBase + 2, 900.0f);
                if (amount != 0.0f)
                    Old::freeSlot(*e, 0, placeOf("OSC"), V::LfoCutoff, amount);
                e->noteOn(57, 1.0f, -1);
                return Old::render(*e, 0.5);
            };
            const auto closed = filtered(0.0f), swung = filtered(0.8f);
            double difference = 0.0, level = 0.0;
            for (size_t i = 9600; i < closed.size(); ++i)
            {
                difference += (double) (closed[i] - swung[i]) * (closed[i] - swung[i]);
                level += (double) closed[i] * closed[i];
            }
            expect(difference > 0.01 * level, "OSC > CUT is heard (" + juce::String(10.0 * std::log10(difference / level), 1) + " dB of the sound)");
        }

        beginTest("Macro oscillator: a slot on OSC that came and went leaves the voice exactly as an engine that never had it");
        {
            auto never = Old::sine();
            auto once = Old::sine();
            never->noteOn(64, 0.9f, -1);
            once->noteOn(64, 0.9f, -1);
            const auto plain = Old::render(*never, 0.5);
            const auto touched = Old::render(*once, 0.5, nullptr, SR, 128, [&](int b)
            {
                if (b == 20) Old::freeSlot(*once, 0, placeOf("OSC"), V::LfoAmp, 0.5f);
                if (b == 60) Old::freeSlot(*once, 0, 0, V::LfoAmp, 0.5f);
            });
            // (It handed its samples over while the slot read them, and stopped when nobody did.)
            auto handing = [](M& e) { return e.getVoice(0).getMod().publishing(V::PubOsc) || e.getVoice(1).getMod().publishing(V::PubOsc); };
            expect(! handing(*once) && ! handing(*never), "nobody reads OSC: no voice hands it over");
            Old::freeSlot(*once, 0, placeOf("OSC"), V::LfoAmp, 0.5f);
            Old::render(*once, 0.01);
            expect(handing(*once), "a slot reads it: the sounding voice does");
            Old::freeSlot(*once, 0, 0, V::LfoAmp, 0.5f);
            Old::render(*once, 0.05);
            expect(! handing(*once), "the slot has gone: it stops");
            int during = 0, after = 0;
            for (size_t i = 0; i < plain.size(); ++i)
            {
                const bool differs = plain[i] != touched[i];
                if (i >= 20 * 128 && i < 60 * 128) during += differs ? 1 : 0;
                if (i >= 70 * 128) after += differs ? 1 : 0;
                if (i < 20 * 128) expect(! differs);
            }
            expect(during > 4000, "while the slot was there the level moved");
            expectEquals(after, 0, "samples that differ once it has gone");
        }

        beginTest("Macro oscillator: a slot on OSC set, turned and taken away under a held note does not click");
        {
            auto e = Old::sine();
            e->noteOn(81, 1.0f, -1);
            const auto y = Old::render(*e, 1.0, nullptr, SR, 128, [&](int b)
            {
                if (b == 100) Old::freeSlot(*e, 0, placeOf("OSC"), V::LfoAmp, 0.8f);
                if (b == 180) e->setParameter(ModSlotParam::id(0, ModSlotParam::AMT), -0.8f);
                if (b == 260) e->setParameter(ModSlotParam::id(0, ModSlotParam::DEST), (float) V::LfoPan);
                if (b == 300) Old::freeSlot(*e, 0, 0, 0, 0.0f);
            });
            // The largest step from one sample to the next: a click is a step far over what the
            // sound has when it is at its loudest and nothing changes.
            auto largest = [&](int from, int to)
            {
                float step = 0.0f;
                for (int i = from + 1; i < to; ++i)
                    step = std::max(step, std::abs(y[(size_t) i] - y[(size_t) (i - 1)]));
                return step;
            };
            const float steady = largest(140 * 128, 170 * 128);
            expect(steady > 0.01f);
            for (int b : { 100, 180, 260, 300 })
                expect(largest(b * 128 - 8, (b + 8) * 128) <= steady * 1.15f,
                       "block " + juce::String(b) + ": " + juce::String(largest(b * 128 - 8, (b + 8) * 128), 4) + " against " + juce::String(steady, 4));
        }

        beginTest("Macro oscillator: ETIM and FTIM stretch the envelopes' times, and let them go again");
        {
            const auto& t = M::lfoDests(0);
            expectEquals(t.ampTime(), (int) V::LfoEnvTime);
            expectEquals(t.filterTime(), (int) V::LfoFilterTime);
            expectEquals(juce::String(t.labels()[V::LfoEnvTime]), juce::String("ETIM"));
            // A 50 ms attack is at its top after a tenth of a second; eight times as long it is a
            // quarter of the way; an eighth as long it was there long ago.
            auto attackOf = [](float amount)
            {
                auto e = Old::sine();
                e->setParameter(M::AmpAttack, 0.05f);
                if (amount != 0.0f)
                    Old::freeSlot(*e, 0, mod::Src::One, V::LfoEnvTime, amount);
                return levelAfter(*e, 0.1);
            };
            expectWithinAbsoluteError(attackOf(0.0f), 1.0f, 1.0e-4f);
            expectWithinAbsoluteError(attackOf(1.0f), 0.25f, 0.02f, "ONE > ETIM at 100 %: the attack takes 0.4 s");
            auto quick = Old::sine();
            quick->setParameter(M::AmpAttack, 0.4f);
            Old::freeSlot(*quick, 0, mod::Src::One, V::LfoEnvTime, -1.0f);
            expectWithinAbsoluteError(levelAfter(*quick, 0.025), 0.5f, 0.04f, "at -100 %: a 0.4 s attack takes 50 ms");

            // The envelope chain's stages follow, the filter's envelope has FTIM, and both come back.
            auto e = Old::sine();
            e->setParameter(M::EnvChainMode, 1.0f);
            Old::freeSlot(*e, 0, mod::Src::One, V::LfoEnvTime, 1.0f);
            Old::freeSlot(*e, 1, mod::Src::One, V::LfoFilterTime, 2.0f / 3.0f);
            e->noteOn(60, 1.0f, -1);
            Old::render(*e, 0.05);
            auto& voice = e->getVoice(0);
            expect(voice.isActive());
            const float filterAttack = M(8).getVoice(0).getFilter().getEnvelope().getParams().attack;
            expectWithinAbsoluteError(voice.getAmpEnvelope().getTimeScale(), 8.0f, 1.0e-3f);
            expect(voice.getEnvelopeChain().getStageCount() >= 2);
            for (int s = 0; s < voice.getEnvelopeChain().getStageCount(); ++s)
                expectWithinAbsoluteError(voice.getEnvelopeChain().getEnvelopeMut(s).getTimeScale(), 8.0f, 1.0e-3f, "the chain's stages too");
            expectWithinAbsoluteError(voice.getFilter().getEnvelope().getParams().attack, 4.0f * filterAttack, 1.0e-4f * filterAttack + 1.0e-6f, "FTIM at two thirds: four times");
            Old::freeSlot(*e, 0, 0, 0, 0.0f);
            Old::freeSlot(*e, 1, 0, 0, 0.0f);
            Old::render(*e, 0.05);
            expectEquals(voice.getAmpEnvelope().getTimeScale(), 1.0f, "let go: the envelope as set");
            expectEquals(voice.getFilter().getEnvelope().getParams().attack, filterAttack);
        }

        beginTest("Macro oscillator: the randoms repeat from one engine to the next (the same notes, the same sound, bit for bit)");
        {
            auto play = []
            {
                auto e = Old::sine();
                Old::freeSlot(*e, 0, mod::Src::Random, V::LfoPan, 1.0f);
                Old::freeSlot(*e, 1, M::sources().indexOf(mod::Src::Random2), V::LfoAmp, 0.8f);
                Old::freeSlot(*e, 2, M::sources().indexOf(mod::Src::Random4), V::LfoPitch, 0.5f);
                std::vector<float> out;
                juce::AudioBuffer<float> buf(2, 128);
                juce::MidiBuffer midi;
                for (int note = 0; note < 10; ++note)
                {
                    e->noteOn(50 + 3 * note, 0.9f, -1);
                    for (int b = 0; b < 12; ++b)
                    {
                        buf.clear();
                        e->processBlock(buf, midi);
                        for (int i = 0; i < 128; ++i)
                        {
                            out.push_back(buf.getSample(0, i));
                            out.push_back(buf.getSample(1, i));
                        }
                    }
                    e->noteOff(50 + 3 * note, -1);
                }
                return out;
            };
            const auto first = play();
            std::array<mod::Voice, 3> others;       // (voices made in between: nothing process-wide decides the draws)
            juce::ignoreUnused(others);
            const auto second = play();
            expect(first == second, "two engines, the same render");
            double left = 0.0, right = 0.0;
            for (size_t i = 0; i + 1 < first.size(); i += 2)
            {
                left += std::abs(first[i]);
                right += std::abs(first[i + 1]);
            }
            expect(std::abs(left - right) > 0.01 * (left + right), "and RAND did place the notes");
        }

        beginTest("Macro oscillator: every new source in use at once, with a hit in the input: no allocation on the audio thread, the output finite");
        {
            for (double rate : { 48000.0, 96000.0 })
            {
                const int block = 256;
                M e(8);
                e.prepareToPlay(rate, block);
                Old::freeSlot(e, 0, placeOf("OSC"), V::LfoCutoff, 0.5f);
                Old::freeSlot(e, 1, placeOf("ONST"), V::LfoAmp, 0.5f, placeOf("BND3"));
                Old::freeSlot(e, 2, placeOf("BN16"), V::LfoTimbre, 0.7f, placeOf("OSC"));
                Old::freeSlot(e, 3, placeOf("RND3"), V::LfoEnvTime, 0.5f);
                Old::freeSlot(e, 4, placeOf("BND1"), V::LfoFilterTime, -0.5f, placeOf("RND2"));
                e.setParameter(R::lfoId(0, R::Trigger), 5.0f);
                e.setParameter(R::lfoId(0, R::Depth), 0.5f);
                e.setParameter(R::lfoId(0, R::DestCell), (float) V::LfoPitch);
                e.setParameter(R::lfoId(1, R::Trigger), 6.0f);
                e.setParameter(R::lfoId(1, R::Depth), 0.5f);
                e.setParameter(R::lfoId(1, R::DestCell), (float) V::LfoPan);
                std::vector<float> in((size_t) block);
                juce::AudioBuffer<float> buf(2, block);
                juce::MidiBuffer midi;
                long position = 0;
                bool finite = true;
                double energy = 0.0;
                auto run = [&](int blocks)
                {
                    for (int b = 0; b < blocks; ++b)
                    {
                        for (int i = 0; i < block; ++i, ++position)
                            in[(size_t) i] = (position / 6000) % 2 == 1 ? 0.6f * (float) std::sin(0.05 * (double) position) : 0.0f;
                        e.setAudioInput({ in.data(), in.data(), block, 0.3f });
                        buf.clear();
                        e.processBlock(buf, midi);
                        for (int i = 0; i < block; ++i)
                        {
                            finite = finite && std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i));
                            energy += (double) buf.getSample(0, i) * buf.getSample(0, i);
                        }
                    }
                };
                e.noteOn(48, 1.0f, -1);
                e.noteOn(60, 0.7f, -1);
                run(4);
               #if defined(__has_feature) && __has_feature(address_sanitizer)
                run(300);
                logMessage("(a sanitizer's allocator is not seen by malloc_logger: not counted in this build)");
               #else
                auto* const before = malloc_logger;
                malloc_logger = ModRouterAllocations::count;
                ModRouterAllocations::events = 0;
                ModRouterAllocations::counting = true;
                // (Through a volatile pointer: an optimising build may not take the pair away.)
                static float* volatile probe = nullptr;
                probe = new float[4096];
                probe[0] = buf.getSample(0, 0);
                delete[] probe;
                ModRouterAllocations::counting = false;
                expect(ModRouterAllocations::events >= 2, "the counter counts");
                ModRouterAllocations::events = 0;
                ModRouterAllocations::counting = true;
                run(200);
                e.noteOn(72, 1.0f, -1);
                run(100);
                ModRouterAllocations::counting = false;
                malloc_logger = before;
                expectEquals(ModRouterAllocations::events, 0, "allocations at " + juce::String(rate, 0) + " Hz");
               #endif
                expect(finite);
                expect(energy > 1.0);
            }
        }
    }
};

static RouterSourcesTests routerSourcesTests;
