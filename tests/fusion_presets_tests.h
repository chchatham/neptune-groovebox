#pragma once

#include "../src/project/presets/FusionPresets.h"
#include "../src/project/presets/ResonatorPresets.h"
#include "../src/project/presets/ResonatorKits.h"
#include "../src/project/PresetManager.h"
#include "../src/engine/AudioEngine.h"
#include "factory_multi_tests.h"
#include "mod_wheel_tests.h"
#include "preset_measure.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

// The fusion presets (src/project/presets/FusionPresets.h), each proven by measurement: what its
// name promises as a number, a sane level, nothing that is not finite from note 0 to 127, that it
// answers the mod strip, and - for every route a preset sets (a free slot, an LFO's DEST, a
// pressure slot, a strip route, a pad's own LFO or slot) - that the route DOES something: the
// same gesture with that one amount at nothing is another sound. A destination or source that
// did not resolve by name fails here (PresetRoute::unresolved, and a routing cell at OFF).
class FusionPresetsTests : public juce::UnitTest
{
public:
    FusionPresetsTests() : juce::UnitTest("Fusion Presets", "Fusion") {}

    using W = ModWheelTests;
    static constexpr double SR = W::SR;
    static constexpr int BLOCK = W::BLOCK;

    static std::shared_ptr<EngineBase> load(const Preset& preset) { return W::makeEngine(preset.engineType, &preset.soundParams); }

    // `seconds` of the engine; at(block) is called before each block.
    static std::vector<float> run(EngineBase& e, double seconds, const std::function<void (int)>& at = {})
    {
        std::vector<float> out;
        const int blocks = (int) (seconds * SR) / BLOCK;
        out.reserve((size_t) (blocks * BLOCK));
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            if (at)
                at(b);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < BLOCK; ++i)
                out.push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
        }
        return out;
    }
    static int blockAt(double seconds) { return (int) (seconds * SR) / BLOCK; }

    // The gesture every preset is judged by: two held notes with the pads pressed as far as
    // `pressure` says (a kit: its sixteen pads one after the other), the strip where `strip` says.
    static std::vector<float> gesture(EngineBase& e, float strip, float pressure, double seconds = 2.0, int onlyPad = -1)
    {
        const bool kit = e.getType() == EngineType::Drum;
        e.setModWheel(strip);
        run(e, 0.25);                                       // the smoothed strip arrives
        const int every = blockAt(0.12);
        return run(e, kit && onlyPad < 0 ? 3.0 : seconds, [&](int block)
        {
            if (kit && onlyPad >= 0)
            {
                if (block == 0)
                    e.noteOn(onlyPad, 0.9f);
            }
            else if (kit)
            {
                if (block % every == 0 && block / every < 16)
                    e.noteOn(block / every, 0.9f);
            }
            else if (block == 0)
            {
                for (int note : { 48, 60 })
                {
                    e.noteOn(note, 0.8f);
                    e.aftertouch(note, pressure);
                }
            }
        });
    }

    // The amounts a preset routes by, one per route it sets.
    struct Route { int amountId; juce::String what; bool strip; int pad = -1; };
    static std::vector<Route> routesOf(const Preset& preset)
    {
        std::vector<Route> out;
        const auto& p = preset.soundParams;
        const bool unified = preset.engineType == EngineType::UnifiedVoice;
        for (int n = 0; n < ModSlotParam::MAX_SLOTS; ++n)
            if (p.count(ModSlotParam::id(n, ModSlotParam::DEST)) != 0)
                out.push_back({ ModSlotParam::id(n, ModSlotParam::AMT), "slot " + juce::String(n + 1), false });
        for (int n = 0; n < 2 && ! unified; ++n)
            if (p.count(mod::Router::lfoId(n, mod::Router::DestCell)) != 0)
                out.push_back({ mod::Router::lfoId(n, mod::Router::Depth), "LFO " + juce::String(n + 1), false });
        for (int n = 0; n < PressureSlotParam::MAX_SLOTS; ++n)
            if (p.count(PressureSlotParam::id(n, PressureSlotParam::DEST_OFFSET)) != 0)
                out.push_back({ PressureSlotParam::id(n, PressureSlotParam::AMT_OFFSET), "pressure slot " + juce::String(n + 1), false });
        const int stripBase = unified ? (int) UnifiedVoiceEngine::STRIP_IDS : (int) mod::Router::STRIP_BASE;
        for (int n = 0; n < mod::Router::NUM_STRIP; ++n)
            if (p.count(stripBase + n * 2) != 0)
                out.push_back({ stripBase + n * 2 + 1, "strip route " + juce::String(n + 1), true });
        for (int pad = 0; pad < PadModParam::MAX_PADS && preset.engineType == EngineType::Drum; ++pad)
        {
            if (p.count(PadModParam::id(pad, PadModParam::LFO_DEST)) != 0)
                out.push_back({ PadModParam::id(pad, PadModParam::LFO_DEPTH), "pad " + juce::String(pad + 1) + " LFO", false, pad });
            if (p.count(PadModParam::id(pad, PadModParam::SLOT1_DEST)) != 0)
                out.push_back({ PadModParam::id(pad, PadModParam::SLOT1_AMT), "pad " + juce::String(pad + 1) + " slot 1", false, pad });
            if (p.count(PadModParam::id(pad, PadModParam::SLOT2_DEST)) != 0)
                out.push_back({ PadModParam::id(pad, PadModParam::SLOT2_AMT), "pad " + juce::String(pad + 1) + " slot 2", false, pad });
        }
        return out;
    }

    static float difference(const std::vector<float>& a, const std::vector<float>& b) { return fastmeasure::difference(a, b, 2048); }

    // A preset on a track of the whole instrument, with its insert effects: loaded as a player
    // loads it (a multi of one member through PresetScreen::loadMulti).
    static bool loadOnTrack(fmultitest::FMRig& r, const Preset& preset)
    {
        MultiPreset one;
        one.name = preset.name;
        one.members.push_back({ preset, false, 16, {} });
        one.members[0].preset.isFactory = false;
        const bool ok = r.loadMulti(one);
        r.solo(0, 2);
        return ok;
    }

    void runTest() override
    {
        namespace pm = presetmeasure;
        using namespace FusionPresets;
        const auto presets = FusionPresets::getAll();

        beginTest("Names: eight characters at most, no two factory presets alike; every routing cell names something");
        {
            expectEquals(PresetRoute::unresolved().joinIntoString("; "), juce::String(), "names that did not resolve");
            PresetManager all;
            std::multiset<juce::String> names;
            for (int i = 0; i < all.getPresetCount(); ++i)
                names.insert(all.getPreset(i).name.toLowerCase());
            for (const auto& preset : presets)
            {
                expect(preset.name.length() <= 8, preset.name + ": more than eight characters");
                expect(preset.isFactory, preset.name);
                expectEquals((int) names.count(preset.name.toLowerCase()), 1, preset.name + ": registered once, and the only one of that name");
                for (auto& cell : PresetRoute::namedCells(preset.engineType, preset.soundParams))
                    expect(cell.name.isNotEmpty(), preset.name + ": id " + juce::String(cell.id) + " = " + juce::String(cell.value) + " names nothing");
            }
            // The resonator's presets and kits route by name as well.
            auto others = resonatorPresets();
            for (auto& kit : resonatorKits()) others.push_back(kit);
            for (const auto& preset : others)
            {
                expect(preset.name.length() <= PresetManager::MAX_NAME_LENGTH, preset.name + ": longer than the list shows");
                for (auto& cell : PresetRoute::namedCells(preset.engineType, preset.soundParams))
                    expect(cell.name.isNotEmpty(), preset.name + ": id " + juce::String(cell.id) + " = " + juce::String(cell.value) + " names nothing");
            }
        }

        beginTest("Every fusion preset: the engine holds what it routes; it sounds at a sane level; finite from note 0 to 127");
        for (const auto& preset : presets)
        {
            auto e = load(preset);
            for (auto& cell : PresetRoute::namedCells(preset.engineType, preset.soundParams))
                expectEquals((int) std::lround(e->getParameter(cell.id)), cell.value, preset.name + ": id " + juce::String(cell.id) + " (" + cell.name + ") as loaded");
            const auto x = gesture(*e, 0.0f, 0.0f);
            const float level = pm::rms(x);
            expect(pm::finite(x), preset.name + ": a sample that is no number");
            expect(level > 0.002f && level < 0.7f, preset.name + ": level " + juce::String(level, 4));
            expect(pm::peak(x) < 2.0f, preset.name + ": peak " + juce::String(pm::peak(x), 2));
            if (preset.engineType != EngineType::Drum)
                for (int note : { 0, 24, 96, 127 })
                {
                    auto edge = load(preset);
                    edge->setModWheel(1.0f);
                    const auto y = run(*edge, 0.3, [&](int b) { if (b == 0) { edge->noteOn(note, 1.0f); edge->aftertouch(note, 1.0f); } });
                    expect(pm::finite(y), preset.name + " at note " + juce::String(note) + ": not finite");
                }
        }

        beginTest("Every fusion preset answers the mod strip");
        for (const auto& preset : presets)
        {
            const auto rest = gesture(*load(preset), 0.0f, 0.0f), again = gesture(*load(preset), 0.0f, 0.0f), up = gesture(*load(preset), 1.0f, 0.0f);
            const float same = difference(rest, again), moved = difference(rest, up);
            logMessage(preset.name.paddedRight(' ', 9) + " strip " + juce::String(moved, 3) + " (two takes at rest " + juce::String(same, 4) + ")");
            expect(moved >= same + 0.03f, preset.name + ": the strip changes it by only " + juce::String(moved, 4));
        }

        beginTest("Every route a preset sets does something: the same gesture without it is another sound");
        for (const auto& preset : presets)
            for (const auto& route : routesOf(preset))
            {
                const float strip = route.strip ? 1.0f : 0.0f;
                auto without = load(preset);
                without->setParameter(route.amountId, 0.0f);
                // (A pad's own LFO or slot: that pad alone is struck.)
                const auto a = gesture(*load(preset), strip, 1.0f, 2.0, route.pad), again = gesture(*load(preset), strip, 1.0f, 2.0, route.pad),
                           b = gesture(*without, strip, 1.0f, 2.0, route.pad);
                const float same = difference(a, again), changed = difference(a, b);
                logMessage(preset.name.paddedRight(' ', 9) + " " + route.what.paddedRight(' ', 16) + " " + juce::String(changed, 3)
                           + (same > 1.0e-4f ? " (two takes alike: " + juce::String(same, 4) + ")" : juce::String()));
                expect(changed >= same + 0.01f, preset.name + ", " + route.what + ": taking it away changes nothing (" + juce::String(changed, 4) + ")");
            }

        beginTest("RSN: the pad's pressure is the bow, the breath, the scrape (pressure slot 1 on EXC), per voice");
        for (const auto& preset : { bowCello(), bowGlass(), breathPipe(), scrapeBar() })
        {
            // (Slot 2 - pressure on the body or the exciter - at nothing: slot 1 alone.)
            auto level = [&](float pressure)
            {
                auto e = load(preset);
                e->setParameter(PressureSlotParam::id(1, PressureSlotParam::AMT_OFFSET), 0.0f);
                return pm::rms(pm::part(run(*e, 1.5, [&](int b) { if (b == 0) { e->noteOn(57, 0.5f); e->aftertouch(57, pressure); } }), 0.7, 1.5, SR));
            };
            // (A pad that is pressed at all gives the energy itself; one that is not, the ENRG cell does.)
            const float soft = level(0.15f), hard = level(1.0f);
            logMessage(preset.name + ": pressed lightly " + juce::String(pm::dB(soft), 1) + " dB, right down " + juce::String(pm::dB(hard), 1)
                       + " dB (a key, no pressure: " + juce::String(pm::dB(level(0.0f)), 1) + " dB)");
            expect(pm::dB(hard / std::max(soft, 1.0e-9f)) >= 6.0f, preset.name + ": pressure raises it by 6 dB or more");
            // Two notes, one pressed: the pressed one is the louder (its own pressure, not the track's).
            auto e = load(preset);
            const auto two = run(*e, 1.5, [&](int b) { if (b == 0) { e->noteOn(57, 0.5f); e->noteOn(64, 0.5f); e->aftertouch(64, 1.0f); e->aftertouch(57, 0.15f); } });
            const auto tail = pm::part(two, 0.7, 1.5, SR);
            auto e2 = load(preset);
            const auto swapped = pm::part(run(*e2, 1.5, [&](int b) { if (b == 0) { e2->noteOn(57, 0.5f); e2->noteOn(64, 0.5f); e2->aftertouch(57, 1.0f); e2->aftertouch(64, 0.15f); } }), 0.7, 1.5, SR);
            expect(difference(tail, swapped) >= 0.05f, preset.name + ": which pad is pressed makes no difference (" + juce::String(difference(tail, swapped), 3) + ")");
        }

        beginTest("BowGlass / BrthPipe: pressure moves the body (MRPH) and the exciter (XMPH)");
        {
            struct Case { Preset preset; int slot; const char* what; };
            for (const auto& c : { Case { bowGlass(), 1, "MRPH" }, Case { breathPipe(), 1, "XMPH" }, Case { bowCello(), 1, "DAMP" }, Case { scrapeBar(), 1, "HARD" } })
            {
                auto bright = [&](bool routed)
                {
                    auto e = load(c.preset);
                    if (! routed)
                        e->setParameter(PressureSlotParam::id(c.slot, PressureSlotParam::AMT_OFFSET), 0.0f);
                    return pm::centroid(pm::part(run(*e, 1.5, [&](int b) { if (b == 0) { e->noteOn(57, 0.6f); e->aftertouch(57, 1.0f); } }), 0.7, 1.5, SR), SR);
                };
                const float with = bright(true), without = bright(false);
                logMessage(c.preset.name + ": pressed, " + juce::String((int) with) + " Hz with pressure on " + c.what + ", " + juce::String((int) without) + " Hz without");
                expect(std::abs(with / std::max(without, 1.0f) - 1.0f) >= 0.04f, c.preset.name + ": pressure on " + c.what + " moves the brightness by 4 % or more");
            }
        }

        beginTest("Deep Mod: pressed, it opens (pressure to CUT, XMOD, RESO) and the LFO comes in through VIA");
        {
            auto take = [&](float pressure)
            {
                auto e = load(deepMod());
                return pm::part(run(*e, 3.5, [&](int b) { if (b == 0) { e->noteOn(48, 0.8f); e->aftertouch(48, pressure); } }), 0.5, 3.5, SR);
            };
            const auto rest = take(0.0f), down = take(1.0f);
            const float open = pm::centroid(down, SR) / std::max(1.0f, pm::centroid(rest, SR));
            // LFO 1 (three seconds a cycle) through VIA PRES: the brightness of half-second windows
            // wanders when pressed and stands still at rest.
            auto wander = [&](const std::vector<float>& x)
            {
                float lo = 1.0e9f, hi = 0.0f;
                for (int w = 0; w < 6; ++w)
                {
                    const float c = pm::centroid(pm::part(x, 0.5 * w, 0.5 * (w + 1), SR), SR);
                    lo = std::min(lo, c);
                    hi = std::max(hi, c);
                }
                return hi / std::max(lo, 1.0f);
            };
            // The same two takes with slot 1 (LFO 1 on PW, VIA PRES) at nothing.
            auto without = [&](float pressure)
            {
                auto e = load(deepMod());
                e->setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
                return pm::part(run(*e, 3.5, [&](int b) { if (b == 0) { e->noteOn(48, 0.8f); e->aftertouch(48, pressure); } }), 0.5, 3.5, SR);
            };
            const auto downPlain = without(1.0f), restPlain = without(0.0f);
            logMessage("Deep Mod: brightness x " + juce::String(open, 2) + " pressed; over three seconds it wanders x " + juce::String(wander(down), 2)
                       + " pressed (x " + juce::String(wander(downPlain), 2) + " without the LFO's slot); at rest that slot changes the sound by "
                       + juce::String(difference(rest, restPlain), 5) + ", pressed by " + juce::String(difference(down, downPlain), 3));
            expect(open >= 1.3f, "pressure opens it");
            expect(difference(rest, restPlain) <= 1.0e-3f, "at rest the LFO is shut out (VIA PRES)");
            expect(difference(down, downPlain) >= 0.05f, "pressed, it is heard");
        }

        beginTest("StepShpe: one held note plays the eight notes of the custom shape, one a step");
        {
            auto e = load(stepShape());
            e->setTempo(120.0f);
            const auto x = run(*e, 2.1, [&](int b) { if (b == 0) e->noteOn(60, 0.8f); });
            static constexpr int expected[8] = { 0, 7, 12, 7, 3, 10, 0, -5 };
            int right = 0;
            juce::String played;
            for (int step = 0; step < 8; ++step)
            {
                const auto window = pm::part(x, 0.25 * step + 0.07, 0.25 * step + 0.22, SR);
                int best = -99;
                double most = 0.0;
                for (int semis = -8; semis <= 15; ++semis)
                    if (const double p = pm::tone(window, pm::noteHz(60 + semis), SR); p > most)
                    {
                        most = p;
                        best = semis;
                    }
                played << best << " ";
                right += best == expected[step] ? 1 : 0;
            }
            logMessage("StepShpe plays (semitones from the held note): " + played);
            expect(right >= 7, "the riff 0 7 12 7 3 10 0 -5, got " + played);
        }

        beginTest("PluckBdy: RDAM damps the string when the key is let go; KDCY lets high notes die sooner");
        {
            using M = MacroOscEngine;
            auto tail = [&](float rdam)
            {
                auto e = load(pluckBody());
                e->setParameter(M::BodyBase + VoiceBody::ReleaseDamp, rdam);
                const auto x = run(*e, 1.0, [&](int b) { if (b == 0) e->noteOn(60, 0.8f); if (b == blockAt(0.3)) e->noteOff(60); });
                return pm::dB(pm::rms(pm::part(x, 0.45, 0.65, SR)) / std::max(1.0e-9f, pm::rms(pm::part(x, 0.1, 0.3, SR))));
            };
            const float damped = tail(0.9f), ringing = tail(0.0f);
            logMessage("PluckBdy: 150 ms after the key is let go " + juce::String(damped, 1) + " dB (RDAM as stored), " + juce::String(ringing, 1) + " dB (RDAM 0)");
            expect(damped <= ringing - 20.0f, "RDAM shortens the tail by 20 dB or more");

            auto fall = [&](float kdcy, int note)
            {
                auto e = load(pluckBody());
                e->setParameter(M::BodyBase + VoiceBody::KeyDecay, kdcy);
                const auto x = run(*e, 1.0, [&](int b) { if (b == 0) e->noteOn(note, 0.8f); });
                return pm::dB(pm::rms(pm::part(x, 0.6, 0.9, SR)) / std::max(1.0e-9f, pm::rms(pm::part(x, 0.05, 0.25, SR))));
            };
            const float highWith = fall(0.6f, 84), highWithout = fall(0.0f, 84), lowWith = fall(0.6f, 48);
            logMessage("PluckBdy: C6 falls " + juce::String(highWith, 1) + " dB in half a second with KDCY, " + juce::String(highWithout, 1)
                       + " dB without; C3 with KDCY " + juce::String(lowWith, 1) + " dB");
            expect(highWith <= highWithout - 6.0f, "KDCY: a high note dies 6 dB or more sooner");
            expect(lowWith >= highWith + 6.0f, "and a low one rings longer than a high one");
        }

        beginTest("DustBody: the body's own grains (GRAN) are what is heard; ReedBody: the reed's curve (REED) is another sound than the bow's");
        {
            using Q = WaveSeqEngine;
            auto dust = [&](bool stored)
            {
                auto e = load(dustBody());
                if (! stored)
                    e->setParameter(Q::BodyBase + VoiceBody::Grain, 0.0f);
                return pm::part(run(*e, 2.0, [&](int b) { if (b == 0) e->noteOn(60, 0.8f); }), 0.8, 2.0, SR);
            };
            const auto grains = dust(true), none = dust(false);
            // Dust is clicks: in the sound's sample-to-sample steps (what a click is made of) the
            // largest stands far above the usual one; an even tone has no such peaks.
            auto crest = [&](const std::vector<float>& x)
            {
                std::vector<float> steps(x.size() > 1 ? x.size() - 1 : 0);
                for (size_t i = 0; i + 1 < x.size(); ++i)
                    steps[i] = x[i + 1] - x[i];
                return pm::dB(pm::peak(steps) / std::max(1.0e-9f, pm::rms(steps)));
            };
            logMessage("DustBody: the largest step over the usual one, " + juce::String(crest(grains), 1) + " dB with GRAN, " + juce::String(crest(none), 1)
                       + " dB without; level " + juce::String(pm::dB(pm::rms(grains) / std::max(1.0e-9f, pm::rms(none))), 1) + " dB; difference "
                       + juce::String(difference(grains, none), 3));
            expect(difference(grains, none) >= 0.1f, "GRAN is heard");
            expect(crest(grains) >= crest(none) + 6.0f, "as clicks: peaks 6 dB or more further over the usual step");

            using M = MacroOscEngine;
            auto reed = [&](bool stored)
            {
                auto e = load(reedBody());
                e->setParameter(mod::Router::lfoId(0, mod::Router::Depth), 0.0f);
                if (! stored)
                    e->setParameter(M::BodyBase + VoiceBody::Reed, 0.0f);
                return pm::part(run(*e, 1.5, [&](int b) { if (b == 0) e->noteOn(57, 0.8f); }), 0.5, 1.5, SR);
            };
            const float changed = difference(reed(true), reed(false));
            logMessage("ReedBody: REED as stored against REED 0, difference " + juce::String(changed, 3));
            expect(changed >= 0.03f, "REED is heard");
        }

        beginTest("MorphBdy: struck, it is the string; a second and a half on, the bell's own partials have come in");
        {
            // The preset; the same with the slot off and the body a string all along; and a bell all along.
            auto take = [&](int which, double from, double to)
            {
                auto e = load(morphBody());
                if (which != 0)
                {
                    e->setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
                    e->setParameter(MacroOscEngine::BodyBase + VoiceBody::Morph, which == 1 ? 0.0f : 5.0f);
                }
                return pm::part(run(*e, 3.0, [&](int b) { if (b == 0) e->noteOn(57, 0.8f); }), from, to, SR);
            };
            const float earlyString = difference(take(0, 0.0, 0.2), take(1, 0.0, 0.2)), earlyBell = difference(take(0, 0.0, 0.2), take(2, 0.0, 0.2));
            // The bell's strongest partial that is no harmonic of the note (between 1.1 and 6 times it).
            const auto bell = take(2, 1.8, 2.6);
            const double f0 = pm::noteHz(57);
            double ratio = 0.0, most = 0.0;
            for (double r = 1.1; r < 6.0; r += 0.01)
                if (std::abs(r - std::round(r)) >= 0.08)
                    if (const double p = pm::tone(bell, r * f0, SR); p > most)
                    {
                        most = p;
                        ratio = r;
                    }
            auto share = [&](const std::vector<float>& x)
            {
                double all = 0.0;
                for (float s : x) all += (double) s * s;
                return pm::powerDb(pm::tone(x, ratio * f0, SR) / std::max(all, 1.0e-18));
            };
            const float early = share(take(0, 0.0, 0.4)), late = share(take(0, 1.8, 2.6)), bellShare = share(bell), stringShare = share(take(1, 1.8, 2.6));
            logMessage("MorphBdy: in the first 200 ms it differs from the string by " + juce::String(earlyString, 3) + " and from the bell by "
                       + juce::String(earlyBell, 3) + "; the bell's partial at " + juce::String(ratio, 2) + " times the note is " + juce::String(late - early, 1)
                       + " dB more of the sound from 1.8 s on than at the strike (within " + juce::String(std::abs(late - bellShare), 1)
                       + " dB of the bell's own; " + juce::String(late - stringShare, 1) + " dB over the string's)");
            expect(earlyString <= 0.6f * earlyBell, "struck, it is nearer the string");
            expect(late >= early + 10.0f, "the bell's partial comes in: 10 dB or more");
            expect(late >= stringShare + 10.0f, "a string all along has none of it");
        }

        beginTest("FMFilter: operator 6 on the cutoff at audio rate adds side bands that die with its envelope");
        {
            auto upper = [&](bool routed, double from, double to)
            {
                auto e = load(fmOnFilter());
                if (! routed)
                    e->setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
                const auto x = pm::part(run(*e, 2.5, [&](int b) { if (b == 0) e->noteOn(48, 0.8f); }), from, to, SR);
                double high = 0.0;
                for (int h = 4; h <= 12; ++h)
                    high += pm::tone(x, pm::noteHz(48) * h, SR);
                return pm::powerDb(high / std::max(1.0e-18, pm::tone(x, pm::noteHz(48), SR)));
            };
            const float early = upper(true, 0.05, 0.35), plain = upper(false, 0.05, 0.35), late = upper(true, 2.0, 2.4), plainLate = upper(false, 2.0, 2.4);
            logMessage("FMFilter: harmonics 4-12 over the first, " + juce::String(early, 1) + " dB with OP6 on CUT (" + juce::String(plain, 1)
                       + " without) at the start, " + juce::String(late, 1) + " dB (" + juce::String(plainLate, 1) + ") two seconds on");
            expect(early >= plain + 6.0f, "the operator on the filter adds 6 dB or more of upper partials");
            expect(early >= late + 6.0f, "and 6 dB or more of them are gone once its envelope has fallen");
        }

        beginTest("SeqMorph: each step of the sequence rings in another body (PLAN on BMRP)");
        {
            auto spread = [&](bool routed)
            {
                auto e = load(seqMorph());
                if (! routed)
                {
                    e->setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
                    e->setParameter(ModSlotParam::id(1, ModSlotParam::AMT), 0.0f);
                }
                return run(*e, 2.5, [&](int b) { if (b == 0) e->noteOn(48, 0.8f); });
            };
            const float changed = difference(spread(true), spread(false));
            logMessage("SeqMorph: the lanes on BMRP and CUT against none, difference " + juce::String(changed, 3));
            expect(changed >= 0.05f, "the lanes move the body and the filter");
        }

        beginTest("Rnd Time: successive notes have their own envelope times (RND2 on ETIM), and the same ones every time (SEED)");
        {
            // Eight short notes of one pitch, 0.6 s apart: how long each takes to fall 12 dB from its top.
            auto times = [&](bool routed, std::vector<float>* render = nullptr)
            {
                auto e = load(rndEnvTime());
                if (! routed)
                    for (int s = 0; s < 4; ++s)
                        e->setParameter(ModSlotParam::id(s, ModSlotParam::AMT), 0.0f);
                const int every = blockAt(0.6);
                const auto x = run(*e, 4.8, [&](int b)
                {
                    if (b % every == 0) e->noteOn(60, 0.8f);
                    if (b % every == blockAt(0.05)) e->noteOff(60);
                });
                if (render != nullptr)
                    *render = x;
                std::vector<float> out;
                for (int n = 0; n < 8; ++n)
                {
                    const auto l = pm::levels(pm::part(x, 0.6 * n, 0.6 * (n + 1), SR), 0.005, SR);
                    const auto top = std::max_element(l.begin(), l.end());
                    float t = 0.6f;
                    for (auto it = top; it != l.end(); ++it)
                        if (*it <= *top - 12.0f) { t = 0.005f * (float) (it - top); break; }
                    out.push_back(t);
                }
                return out;
            };
            std::vector<float> first, second;
            const auto random = times(true, &first), again = times(true, &second), even = times(false);
            auto ratio = [](const std::vector<float>& t) { return *std::max_element(t.begin(), t.end()) / std::max(0.005f, *std::min_element(t.begin(), t.end())); };
            juce::String list;
            for (float t : random) list << juce::String((int) std::lround(t * 1000.0f)) << " ";
            logMessage("Rnd Time: eight notes fall 12 dB in " + list + "ms (longest / shortest " + juce::String(ratio(random), 1)
                       + "; without the slots " + juce::String(ratio(even), 2) + ")");
            expect(ratio(random) >= 2.0f, "the notes' envelope times differ by a factor of 2 or more");
            expect(ratio(even) <= 1.3f, "without the slots they are alike");
            expect(difference(first, second) <= 1.0e-4f, "the same eight notes again: the same sound (SEED)");
        }

        beginTest("FMWaveOp / FM Cycle: an operator that reads a table or a sample's cycles is not a sine");
        for (const auto& preset : { fmWaveOps(), fmCycleOp() })
        {
            auto e = load(preset), sine = load(preset);
            sine->setParameter(0 * 10 + FMEngine::OP_WAVE, 0.0f);
            const float changed = difference(run(*e, 1.0, [&](int b) { if (b == 0) e->noteOn(57, 0.8f); }),
                                             run(*sine, 1.0, [&](int b) { if (b == 0) sine->noteOn(57, 0.8f); }));
            logMessage(preset.name + ": WAVE as stored against SINE, difference " + juce::String(changed, 3));
            expect(changed >= 0.1f, preset.name + ": the wave is heard");
        }

        beginTest("DualFilt / SerlFilt: the second filter beside the first (PAR) lets the top through; after it (SER) it takes more away");
        {
            using Q = WavetableEngine;
            const int routing = Q::FilterDualBase + (int) VoiceFilterSection::Routing - (int) VoiceFilterSection::Count;
            auto bright = [&](const Preset& preset, bool dual)
            {
                auto e = load(preset);
                if (! dual)
                    e->setParameter(routing, 0.0f);
                return pm::centroid(pm::part(run(*e, 2.0, [&](int b) { if (b == 0) e->noteOn(48, 0.8f); }), 1.0, 2.0, SR), SR);
            };
            const float par = bright(dualFilter(), true), parOff = bright(dualFilter(), false);
            const float ser = bright(serialFilter(), true), serOff = bright(serialFilter(), false);
            logMessage("DualFilt: " + juce::String((int) par) + " Hz with filter B in parallel, " + juce::String((int) parOff) + " Hz with A alone; SerlFilt: "
                       + juce::String((int) ser) + " Hz with B in series, " + juce::String((int) serOff) + " Hz with A alone");
            expect(par >= 1.2f * parOff, "PAR: the high-pass beside the low-pass adds the top");
            expect(ser <= 0.9f * serOff, "SER: the second low-pass takes more away");
        }

        beginTest("Seeded / SeedTabl: notes differ from one another, and the same notes come out the same again; another SEED, other notes");
        for (const auto& preset : { seededSpread(), seededWavetable() })
        {
            const int seedId = (preset.engineType == EngineType::MacroOsc ? (int) MacroOscEngine::SpreadBase : (int) WavetableEngine::SpreadBase) + VoiceSpread::Seed;
            auto take = [&](float seed, bool spread)
            {
                auto e = load(preset);
                if (seed >= 0.0f)
                    e->setParameter(seedId, seed);
                if (! spread)
                    for (int o : { (int) VoiceSpread::Slop, (int) VoiceSpread::Cutoff, (int) VoiceSpread::EnvTime, (int) VoiceSpread::Wave })
                        e->setParameter(seedId - VoiceSpread::Seed + o, 0.0f);
                const int every = blockAt(0.5);
                return run(*e, 2.0, [&](int b) { if (b % every == 0) e->noteOn(60, 0.8f); if (b % every == blockAt(0.3)) e->noteOff(60); });
            };
            const auto a = take(-1.0f, true), b = take(-1.0f, true), other = take(99.0f, true), plain = take(-1.0f, false);
            logMessage(preset.name + ": the same seed again " + juce::String(difference(a, b), 5) + ", another seed " + juce::String(difference(a, other), 3)
                       + ", the spread against none " + juce::String(difference(a, plain), 3));
            expect(difference(a, b) <= 1.0e-4f, preset.name + ": the same seed is the same sound");
            expect(difference(a, other) >= 0.01f, preset.name + ": another seed is other notes");
            expect(difference(a, plain) >= 0.01f, preset.name + ": the spread is heard");
        }

        beginTest("VOX: Mono Wt sounds one note of four, Wide Pad all of sixteen");
        {
            // Mono: four notes one after the other, all held: only the last is heard.
            auto mono = load(monoLead());
            const int notes[4] = { 48, 55, 62, 69 };
            const auto x = pm::part(run(*mono, 1.5, [&](int b) { for (int k = 0; k < 4; ++k) if (b == blockAt(0.1 * k)) mono->noteOn(notes[k], 0.8f); }), 0.9, 1.5, SR);
            const double last = pm::tone(x, pm::noteHz(69), SR);
            float others = -200.0f;
            for (int k = 0; k < 3; ++k)
                others = std::max(others, pm::powerDb(pm::tone(x, pm::noteHz(notes[k]), SR) / std::max(last, 1.0e-18)));
            logMessage("Mono Wt: the loudest of the three earlier notes is " + juce::String(others, 1) + " dB under the last");
            expectEquals((int) std::lround(mono->getParameter(WavetableEngine::VoiceCountParam)), 1);
            expect(others <= -20.0f, "one voice: the earlier notes are gone");

            // Wide: sixteen notes a major third apart... every one of them is there.
            auto wide = load(widePad());
            const auto y = pm::part(run(*wide, 3.0, [&](int b) { if (b == 0) for (int k = 0; k < 16; ++k) wide->noteOn(36 + 3 * k, 0.8f); }), 2.0, 3.0, SR);
            std::vector<float> each;
            for (int k = 0; k < 16; ++k)
                each.push_back(pm::powerDb(pm::tone(y, pm::noteHz(36 + 3 * k), SR)));
            const float loudest = *std::max_element(each.begin(), each.end()), weakest = *std::min_element(each.begin(), each.end());
            logMessage("Wide Pad: sixteen held notes, the weakest first partial " + juce::String(weakest - loudest, 1) + " dB under the strongest");
            expectEquals((int) std::lround(wide->getParameter(WavetableEngine::VoiceCountParam)), 16);
            expect(weakest >= loudest - 30.0f, "sixteen voices: every note is there");
            // ... and with VOX at 8 half of them are not.
            auto eight = load(widePad());
            eight->setParameter(WavetableEngine::VoiceCountParam, 8.0f);
            const auto z = pm::part(run(*eight, 3.0, [&](int b) { if (b == 0) for (int k = 0; k < 16; ++k) eight->noteOn(36 + 3 * k, 0.8f); }), 2.0, 3.0, SR);
            int missing = 0;
            for (int k = 0; k < 16; ++k)
                missing += pm::powerDb(pm::tone(z, pm::noteHz(36 + 3 * k), SR)) < loudest - 30.0f ? 1 : 0;
            expect(missing >= 6, "at VOX 8 the same chord loses notes (" + juce::String(missing) + " gone)");
        }

        beginTest("SprngKey: the spring rings on after the note (insert SPRG, stored with the preset)");
        {
            auto tail = [&](bool on, float disp)
            {
                fmultitest::FMRig r;
                std::vector<float> x;
                if (! loadOnTrack(r, springKeys()))
                    return x;
                EffectType type {};
                expect(r.audio.getInsertEffectType(0, 0, type) && type == EffectType::SpringReverbEffect, "SPRG in slot 1");
                expectWithinAbsoluteError(r.audio.getTrackParameter(0, AudioEngine::INSERT_WET_BASE), 0.5f, 1.0e-4f, "its WET as stored");
                if (! on)
                    r.audio.setTrackParameter(0, AudioEngine::INSERT_ON_BASE, 0.0f);
                if (disp >= 0.0f)
                    r.audio.setTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE + 1, disp);
                r.on(0, 60);
                r.take(0.15);
                r.off(0, 60);
                r.take(0.6);
                x = r.take(0.6);
                expect(r.allFinite && r.audio.getNonFiniteBlocks() == 0, "finite");
                return x;
            };
            const auto spring = tail(true, -1.0f), dry = tail(false, -1.0f), plain = tail(true, 0.0f);
            logMessage("SprngKey: 0.6 to 1.2 s after the key, " + juce::String(pm::dB(pm::rms(spring)), 1) + " dB with the spring, "
                       + juce::String(pm::dB(pm::rms(dry)), 1) + " dB without; DISP as stored against 0, difference " + juce::String(difference(spring, plain), 3));
            expect(pm::rms(spring) > 1.0e-4f && pm::dB(pm::rms(spring) / std::max(1.0e-9f, pm::rms(dry))) >= 20.0f, "the spring's tail is 20 dB or more over the dry note's");
            expect(difference(spring, plain) >= 0.03f, "DISP (the spring's drip) is heard");
        }

        beginTest("ShimrPad: the tail climbs an octave (insert SHIM)");
        {
            auto octave = [&](float shim)
            {
                fmultitest::FMRig r;
                if (! loadOnTrack(r, shimmerPad()))
                    return 0.0f;
                EffectType type {};
                expect(r.audio.getInsertEffectType(0, 0, type) && type == EffectType::ShimmerReverbEffect, "SHIM in slot 1");
                if (shim >= 0.0f)
                    r.audio.setTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE + 1, shim);
                r.on(0, 60);
                r.take(0.4);
                r.off(0, 60);
                r.take(1.5);
                const auto x = r.take(1.5);
                expect(r.allFinite && r.audio.getNonFiniteBlocks() == 0, "finite");
                expect(pm::rms(x) > 1.0e-4f, "a tail");
                return pm::powerDb(pm::tone(x, 2.0 * pm::noteHz(60), fmultitest::SR) / std::max(1.0e-18, pm::tone(x, pm::noteHz(60), fmultitest::SR)));
            };
            const float with = octave(-1.0f), without = octave(0.0f);
            logMessage("ShimrPad: in the tail the octave above the note is " + juce::String(with, 1) + " dB against the note itself; with SHIM at 0, "
                       + juce::String(without, 1) + " dB");
            expect(with >= without + 10.0f, "the shimmer puts 10 dB or more into the octave above");
        }

        beginTest("ThruFlng: THRU delays the dry sound by the flanger's centre (the swept copy passes through it)");
        {
            auto take = [&](bool thru, float& onsetMs)
            {
                fmultitest::FMRig r;
                std::vector<float> x;
                if (! loadOnTrack(r, thruFlanger()))
                    return x;
                EffectType type {};
                expect(r.audio.getInsertEffectType(0, 0, type) && type == EffectType::FlangerEffect, "FLG in slot 1");
                expectEquals(r.audio.getTrackParameter(0, AudioEngine::INSERT_EXTRA_BASE + Flanger::Through), 1.0f, "THRU as stored");
                if (! thru)
                    r.audio.setTrackParameter(0, AudioEngine::INSERT_EXTRA_BASE + Flanger::Through, 0.0f);
                r.take(0.2);
                r.on(0, 48);
                x = r.take(4.2);
                const float top = pm::peak(x);
                onsetMs = 0.0f;
                for (size_t i = 0; i < x.size(); ++i)
                    if (std::abs(x[i]) > 0.02f * top) { onsetMs = 1000.0f * (float) i / (float) fmultitest::SR; break; }
                expect(r.allFinite && r.audio.getNonFiniteBlocks() == 0, "finite");
                return x;
            };
            float late = 0.0f, early = 0.0f;
            const auto through = take(true, late), ordinary = take(false, early);
            logMessage("ThruFlng: the note is heard after " + juce::String(late, 2) + " ms with THRU, " + juce::String(early, 2)
                       + " ms without; difference of the two sounds " + juce::String(difference(through, ordinary), 3));
            expect(late >= early + 1.0f, "with THRU the dry sound comes a millisecond or more later");
            expect(difference(through, ordinary) >= 0.02f, "and the sweep is another one");
        }

        beginTest("PointKit: kick-machine points and resonator drums only; the gong's own LFO beats on it and on no other pad");
        {
            const auto kit = pointKit();
            auto e = load(kit);
            for (int pad = 0; pad < 16; ++pad)
            {
                const auto type = (DrumSynthType) (int) std::lround(e->getParameter(pad * 8 + 7));
                const bool kick = DrumVoice::isKickPoint(type) || type == DrumSynthType::KickSynth;
                expect(pad >= 8 && pad <= 10 ? kick : type == DrumSynthType::Resonator, "pad " + juce::String(pad + 1) + ": type " + juce::String((int) type));
            }
            // The gong struck once: its level in 10 ms windows, and how much of that moves five times a second.
            auto beat = [&](bool own, int pad, std::vector<float>* render = nullptr)
            {
                auto k = load(kit);
                if (! own)
                    k->setParameter(PadModParam::id(4, PadModParam::LFO_DEPTH), 0.0f);
                const auto x = run(*k, 2.0, [&](int b) { if (b == 0) k->noteOn(pad, 0.9f); });
                if (render != nullptr)
                    *render = x;
                const auto l = pm::levels(pm::part(x, 0.2, 2.0, SR), 0.01, SR);
                std::vector<float> envelope(l.begin(), l.end());
                // (the slope of the decay taken out: what is left is the beating)
                const float slope = (envelope.back() - envelope.front()) / (float) std::max<size_t>(1, envelope.size() - 1);
                for (size_t i = 0; i < envelope.size(); ++i)
                    envelope[i] -= envelope.front() + slope * (float) i;
                return pm::powerDb(fastmeasure::goertzel(envelope, 0, envelope.size(), 5.0, 100.0));
            };
            const float beating = beat(true, 4), still = beat(false, 4);
            std::vector<float> rideWith, rideWithout;
            beat(true, 5, &rideWith);
            beat(false, 5, &rideWithout);
            logMessage("PointKit: the gong's level moves at 5 Hz by " + juce::String(beating - still, 1) + " dB more with its own LFO; the ride beside it differs by "
                       + juce::String(difference(rideWith, rideWithout), 6));
            expect(beating >= still + 15.0f, "the gong beats five times a second");
            expect(difference(rideWith, rideWithout) <= 1.0e-5f, "the ride does not: the LFO is the gong's own");
        }
    }
};

static FusionPresetsTests fusionPresetsTests;
