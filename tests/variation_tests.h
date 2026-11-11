#pragma once

#include "../src/arpeggiator/VariationMacros.h"
#include "../src/arpeggiator/RhythmCapture.h"
#include "../src/arpeggiator/MutateSystem.h"
#include "../src/sequencer/Step.h"
#include <juce_core/juce_core.h>

class VariationMacroTests : public juce::UnitTest
{
public:
    VariationMacroTests() : UnitTest("Variation Macros") {}

    void runTest() override
    {
        beginTest("Default density at 0.5 does not skip or add");
        {
            VariationMacros vm;
            auto mod = vm.apply(0, 16, 12345);
            expect(!mod.skip);
            expectEquals(mod.extraNotes, 0);
        }

        beginTest("Density below 0.5 skips some steps");
        {
            VariationMacros vm;
            vm.setDensity(0.0f);

            int skipCount = 0;
            for (int i = 0; i < 64; ++i)
            {
                auto mod = vm.apply(i, 64, 33333);
                if (mod.skip) ++skipCount;
            }
            expect(skipCount > 0, "Density=0 should skip some steps");
            expect(skipCount < 64, "Density=0 should not skip all steps");
        }

        beginTest("Density above 0.5 adds ghost notes");
        {
            VariationMacros vm;
            vm.setDensity(1.0f);

            int addedCount = 0;
            for (int i = 0; i < 64; ++i)
            {
                auto mod = vm.apply(i, 64, 22222);
                if (mod.extraNotes > 0) ++addedCount;
            }
            expect(addedCount > 0, "Density=1.0 should add ghost notes");
        }

        beginTest("Spice adds extra notes");
        {
            VariationMacros vm;
            vm.setSpice(1.0f);

            int extraCount = 0;
            for (int i = 0; i < 64; ++i)
            {
                auto mod = vm.apply(i, 64, 54321);
                extraCount += mod.extraNotes;
            }
            expect(extraCount > 0, "Spice at 1.0 should add extra notes");
        }

        beginTest("Dice transposes notes");
        {
            VariationMacros vm;
            vm.setDice(1.0f);

            bool sawTranspose = false;
            for (int i = 0; i < 64; ++i)
            {
                auto mod = vm.apply(i, 64, 77777);
                if (mod.noteOffset != 0) sawTranspose = true;
            }
            expect(sawTranspose, "Dice at 1.0 should transpose some notes");
        }

        beginTest("OctaveScatter transposes by octaves");
        {
            VariationMacros vm;
            vm.setOctaveScatter(1.0f);

            bool sawOctave = false;
            for (int i = 0; i < 100; ++i)
            {
                auto mod = vm.apply(i, 100, 44444);
                if (std::abs(mod.noteOffset) >= 12) sawOctave = true;
            }
            expect(sawOctave, "OctaveScatter at 1.0 should transpose by octaves");
        }

        beginTest("RatchetChance produces subdivisions");
        {
            VariationMacros vm;
            vm.setRatchetChance(1.0f);

            bool sawRatchet = false;
            for (int i = 0; i < 64; ++i)
            {
                auto mod = vm.apply(i, 64, 55555);
                expect(mod.ratchetCount >= 1 && mod.ratchetCount <= 4);
                if (mod.ratchetCount > 1) sawRatchet = true;
            }
            expect(sawRatchet, "RatchetChance at 1.0 should produce subdivisions");
        }

        beginTest("RatchetChance at 0 produces no subdivisions");
        {
            VariationMacros vm;
            vm.setRatchetChance(0.0f);

            for (int i = 0; i < 32; ++i)
            {
                auto mod = vm.apply(i, 32, 999);
                expectEquals(mod.ratchetCount, 1);
            }
        }

        beginTest("GateSpread varies gate length");
        {
            VariationMacros vm;
            vm.setGateSpread(1.0f);

            bool changed = false;
            for (int i = 0; i < 64; ++i)
            {
                auto mod = vm.apply(i, 64, 88888);
                expect(mod.gateScale >= 0.25f && mod.gateScale <= 2.0f);
                if (std::abs(mod.gateScale - 1.0f) > 0.001f) changed = true;
            }
            expect(changed, "GateSpread should vary gate lengths");
        }

        beginTest("Humanize varies velocity and timing");
        {
            VariationMacros vm;
            vm.setHumanize(1.0f);

            bool velChanged = false;
            bool timingChanged = false;
            for (int i = 0; i < 64; ++i)
            {
                auto mod = vm.apply(i, 64, 11111);
                if (std::abs(mod.velocityScale - 1.0f) > 0.001f) velChanged = true;
                if (std::abs(mod.timingOffset) > 0.001f) timingChanged = true;
                expect(mod.timingOffset >= -0.5f && mod.timingOffset <= 0.5f);
            }
            expect(velChanged, "Humanize should vary velocity");
            expect(timingChanged, "Humanize should vary timing");
        }

        beginTest("Humanize at 0 preserves values");
        {
            VariationMacros vm;

            for (int i = 0; i < 32; ++i)
            {
                auto mod = vm.apply(i, 32, 12345);
                expectWithinAbsoluteError(mod.velocityScale, 1.0f, 0.0001f);
                expectWithinAbsoluteError(mod.timingOffset, 0.0f, 0.0001f);
            }
        }

        beginTest("Deterministic: same seed yields same results");
        {
            VariationMacros vm;
            vm.setSpice(0.5f);
            vm.setDice(0.5f);
            vm.setHumanize(0.5f);

            uint32_t seed = 12345;

            for (int i = 0; i < 16; ++i)
            {
                auto mod1 = vm.apply(i, 16, seed);
                auto mod2 = vm.apply(i, 16, seed);

                expectEquals((int)mod1.skip, (int)mod2.skip);
                expectEquals(mod1.noteOffset, mod2.noteOffset);
                expectEquals(mod1.ratchetCount, mod2.ratchetCount);
                expectWithinAbsoluteError(mod1.velocityScale, mod2.velocityScale, 0.0001f);
                expectWithinAbsoluteError(mod1.timingOffset, mod2.timingOffset, 0.0001f);
            }
        }
    }
};

class MutateSystemTests : public juce::UnitTest
{
public:
    MutateSystemTests() : UnitTest("Mutate System") {}

    void runTest() override
    {
        beginTest("Mutate Ancestor mode always starts from original");
        {
            MutateSystem ms;
            ms.setMode(MutateSystem::Mode::Ancestor);
            ms.setAmount(0.5f);

            std::vector<Step> original(16);
            original[0].trig = true;
            original[0].notes.push_back({60, 1.0f});
            original[4].trig = true;
            original[4].notes.push_back({64, 0.8f});

            ms.setAncestor(original);

            std::vector<Step> steps1 = original;
            ms.mutate(steps1, 11111);

            std::vector<Step> steps3 = original;
            ms.mutate(steps3, 11111);

            for (int i = 0; i < 16; ++i)
                expectEquals((int)steps1[(size_t)i].trig, (int)steps3[(size_t)i].trig);
        }

        beginTest("Mutate Cumulative mode compounds changes");
        {
            MutateSystem ms;
            ms.setMode(MutateSystem::Mode::Cumulative);
            ms.setAmount(0.8f);

            std::vector<Step> original(16);
            for (int i = 0; i < 16; ++i)
            {
                original[(size_t)i].trig = (i % 4 == 0);
                if (original[(size_t)i].trig)
                    original[(size_t)i].notes.push_back({60, 1.0f});
            }

            ms.setAncestor(original);

            std::vector<Step> steps = original;
            for (int round = 0; round < 5; ++round)
                ms.mutate(steps, (uint32_t)(round * 99999 + 1));

            int differences = 0;
            for (int i = 0; i < 16; ++i)
            {
                if (steps[(size_t)i].trig != original[(size_t)i].trig)
                    ++differences;
            }
            expect(differences > 0, "Cumulative mutations should change the pattern");
        }

        beginTest("ReturnToAncestor restores original");
        {
            MutateSystem ms;
            ms.setAmount(1.0f);

            std::vector<Step> original(8);
            original[0].trig = true;
            original[0].notes.push_back({72, 0.9f});

            ms.setAncestor(original);

            std::vector<Step> steps = original;
            ms.mutate(steps, 54321);

            ms.returnToAncestor(steps);

            expectEquals((int)steps.size(), 8);
            expect(steps[0].trig);
            expectEquals(steps[0].notes[0].pitch, 72);
        }

        beginTest("CommitAncestor updates baseline");
        {
            MutateSystem ms;
            ms.setMode(MutateSystem::Mode::Ancestor);
            ms.setAmount(0.5f);

            std::vector<Step> original(8);
            original[0].trig = true;
            original[0].notes.push_back({60, 1.0f});

            ms.setAncestor(original);

            std::vector<Step> steps = original;
            ms.mutate(steps, 12345);

            ms.commitAncestor(steps);

            std::vector<Step> restored(8);
            ms.returnToAncestor(restored);

            for (int i = 0; i < 8; ++i)
                expectEquals((int)restored[(size_t)i].trig, (int)steps[(size_t)i].trig);
        }

        beginTest("Mutations stay within valid ranges");
        {
            MutateSystem ms;
            ms.setAmount(1.0f);

            std::vector<Step> original(16);
            for (int i = 0; i < 16; ++i)
            {
                original[(size_t)i].trig = true;
                original[(size_t)i].notes.push_back({60, 0.5f});
                original[(size_t)i].velocity = 0.7f;
            }

            ms.setAncestor(original);

            std::vector<Step> steps = original;
            for (int round = 0; round < 10; ++round)
            {
                ms.mutate(steps, (uint32_t)(round * 31337));

                for (int i = 0; i < 16; ++i)
                {
                    expect(steps[(size_t)i].velocity >= 0.0f && steps[(size_t)i].velocity <= 1.0f);
                    for (auto& note : steps[(size_t)i].notes)
                        expect(note.pitch >= 0 && note.pitch <= 127);
                }
            }
        }
    }
};

class RhythmCaptureTests : public juce::UnitTest
{
public:
    RhythmCaptureTests() : UnitTest("Rhythm Capture") {}

    void runTest() override
    {
        beginTest("RhythmCapture stores tapped events");
        {
            RhythmCapture rc;
            rc.startCapture(44100.0);
            expect(rc.isCapturing());

            rc.tapEvent(1.0f);
            rc.advanceSamples(11025.0);
            rc.tapEvent(0.8f);
            rc.advanceSamples(11025.0);
            rc.tapEvent(0.6f);
            rc.advanceSamples(11025.0);
            rc.tapEvent(0.5f);

            rc.stopCapture();
            expect(!rc.isCapturing());
            expectEquals(rc.getEventCount(), 4);

            auto& events = rc.getEvents();
            expectEquals((int)events.size(), 4);
            expectWithinAbsoluteError(events[0].timeSamples, 0.0, 0.001);
            expectWithinAbsoluteError(events[1].timeSamples, 11025.0, 0.001);
            expectWithinAbsoluteError(events[1].velocity, 0.8f, 0.001f);
        }

        beginTest("Taps ignored when not capturing");
        {
            RhythmCapture rc;
            rc.tapEvent(1.0f);
            rc.tapEvent(0.5f);
            expectEquals(rc.getEventCount(), 0);
        }

        beginTest("hasCapture reflects state");
        {
            RhythmCapture rc;
            expect(!rc.hasCapture());
            rc.startCapture(44100.0);
            rc.tapEvent(1.0f);
            rc.stopCapture();
            expect(rc.hasCapture());
        }

        beginTest("normalizeToStepLength scales timing");
        {
            RhythmCapture rc;
            rc.startCapture(44100.0);
            rc.tapEvent(1.0f);
            rc.advanceSamples(1000.0);
            rc.tapEvent(0.8f);
            rc.advanceSamples(1000.0);
            rc.tapEvent(0.6f);
            rc.stopCapture();

            rc.normalizeToStepLength(5000.0);

            auto& events = rc.getEvents();
            expectWithinAbsoluteError(events[0].timeSamples, 0.0, 0.001);
            expectWithinAbsoluteError(events[2].timeSamples, 5000.0, 0.001);
        }

        beginTest("Single tap normalizes to zero");
        {
            RhythmCapture rc;
            rc.startCapture(44100.0);
            rc.tapEvent(1.0f);
            rc.stopCapture();

            rc.normalizeToStepLength(1000.0);
            auto& events = rc.getEvents();
            expectEquals((int)events.size(), 1);
            expectWithinAbsoluteError(events[0].timeSamples, 0.0, 0.001);
        }
    }
};

static VariationMacroTests variationMacroTests;
static MutateSystemTests mutateSystemTests;
static RhythmCaptureTests rhythmCaptureTests;
