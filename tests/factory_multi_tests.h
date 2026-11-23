#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>
#include "help_card_tests.h"
#include "preset_measure.h"
#include "mod_wheel_tests.h"
#include "../src/project/FactoryMultis.h"
#include "../src/project/PresetManager.h"
#include "../src/project/ProjectBridge.h"
#include "../src/dsp/effects/Vocoder.h"

// Every factory multi, proven by measurement (nobody can listen): what its name promises as a
// number, a sane level, no block that is not finite, and that the member the keys play answers
// the mod strip. The "Multi Presets" suite loads and renders them as well, and covers Duck Pair.
//
// A multi is loaded as the player loads it (PresetScreen::loadMulti: onto the earliest empty
// tracks, members' numbers rewritten to tracks), then notes are started on the engines; the
// output is the instrument's own (inserts, mixer, sends, master). One member is heard alone by
// shutting the others' faders: a track that is listened to goes on feeding.
namespace fmultitest
{
    using namespace helptest;
    namespace pm = presetmeasure;

    static constexpr double SR = 44100.0;
    static constexpr int BLOCK = 256;

    struct FMRig : Rig
    {
        FMRig()
        {
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                seq.getPattern(p).getTrack(1).engineType = EngineType::Drum;
            audio.prepare(SR, BLOCK);
        }

        bool loadMulti(const MultiPreset& multi)
        {
            mgr.setActiveScreen(PRESET);
            auto* screen = dynamic_cast<PresetScreen*>(mgr.getActiveScreen());
            return screen != nullptr && screen->loadMulti(multi);
        }

        // The next `seconds` of the instrument's output (the mean of its two sides).
        std::vector<float> take(double seconds)
        {
            std::vector<float> l(BLOCK), r(BLOCK), out;
            out.reserve((size_t) (seconds * SR) + BLOCK);
            for (int done = 0; done < (int) (seconds * SR); done += BLOCK)
            {
                audio.renderBlock(l.data(), r.data(), BLOCK);
                for (int i = 0; i < BLOCK; ++i)
                {
                    allFinite = allFinite && std::isfinite(l[(size_t) i]) && std::isfinite(r[(size_t) i]);
                    out.push_back(0.5f * (l[(size_t) i] + r[(size_t) i]));
                }
            }
            return out;
        }

        // Only this track is heard (the others still play and still feed whoever listens to them).
        void solo(int track, int members)
        {
            for (int t = 0; t < members; ++t)
                audio.setTrackParameter(t, MixParam::VOLUME, t == track ? 0.8f : 0.0f);
        }

        void on(int track, int note, float velocity = 0.9f) { audio.getEngine(track)->noteOn(note, velocity); }
        void off(int track, int note) { audio.getEngine(track)->noteOff(note); }

        // The notes of the member's first trig (middle C for a member without one).
        std::vector<int> notesOf(const MultiPreset& multi, int member) const
        {
            for (auto& s : multi.members[(size_t) member].steps)
                if (s.trig && ! s.notes.empty())
                {
                    std::vector<int> notes;
                    for (auto& n : s.notes) notes.push_back(n.pitch);
                    return notes;
                }
            return { 60 };
        }
        void play(const MultiPreset& multi, int member)
        {
            for (int note : notesOf(multi, member))
                on(member, note);
        }

        bool allFinite = true;
    };

    inline const MultiPreset& named(const char* name)
    {
        static const MultiPreset none;
        for (auto& m : FactoryMultis::getAll())
            if (m.name == name)
                return m;
        return none;
    }
}

class FactoryMultiTests : public juce::UnitTest
{
public:
    FactoryMultiTests() : UnitTest("Factory Multis") {}

    using Rig = fmultitest::FMRig;

    // The rig with the multi loaded on tracks 0 .. n - 1 (its members in order).
    // (The unified voice's per-note spread is the system random's: off here, so that two takes of
    // one multi are one sound and two rigs differ by the cells alone. Guardrails, TESTING.)
    bool load(Rig& r, const MultiPreset& multi)
    {
        const bool ok = multi.isValid() && r.loadMulti(multi);
        expect(ok, multi.name + ": loads");
        for (int m = 0; ok && m < multi.memberCount(); ++m)
            ModWheelTests::makeRepeatable(*r.audio.getEngine(m));
        return ok;
    }

    void sane(Rig& r, const juce::String& name)
    {
        expect(r.allFinite, name + ": a sample that is no number");
        expectEquals(r.audio.getNonFiniteBlocks(), 0, name + ": blocks dropped as not finite");
    }

    void runTest() override
    {
        using namespace fmultitest;
        const auto& multis = FactoryMultis::getAll();

        beginTest("Every factory multi: valid, a short name, routings that name members, routes that name something");
        {
            expectEquals(PresetRoute::unresolved().joinIntoString("; "), juce::String(), "names that did not resolve");
            expect(multis.size() >= 15, "the fifteen multis");
            for (auto& multi : multis)
            {
                expect(multi.isValid(), multi.name + " not valid");
                // (Duck Pair is from before the eight-character rule and is known by its name.)
                expect(multi.name.length() <= 8 || multi.name == "Duck Pair", multi.name + ": more than eight characters");
                expect(multi.isFactory, multi.name + " not factory");
                expectEquals(multi.category, juce::String("MULT"));
                for (auto& member : multi.members)
                {
                    for (int src : MultiPreset::sourcesOf(member.preset.soundParams))
                        expect(src >= 1 && src <= multi.memberCount(), multi.name + ": source " + juce::String(src) + " is no member");
                    // A DEST, SRC, VIA, shape, trigger or division a member sets is a name of today's lists, never OFF.
                    for (auto& cell : PresetRoute::namedCells(member.preset.engineType, member.preset.soundParams))
                        expect(cell.name.isNotEmpty(), multi.name + " / " + member.preset.name + ": id " + juce::String(cell.id)
                                                       + " = " + juce::String(cell.value) + " names nothing");
                }
                expectEquals(MultiPreset::fromJSON(multi.toJSON()).toJSON(), multi.toJSON(), multi.name + " JSON");
            }
        }

        beginTest("Every factory multi: what it routes is what its tracks hold once loaded; every member sounds; a sane level");
        for (auto& multi : multis)
        {
            Rig r;
            if (! load(r, multi))
                continue;
            for (int m = 0; m < multi.memberCount(); ++m)
            {
                const auto& member = multi.members[(size_t) m];
                expect(r.audio.getEngineTypeOf(m) == member.preset.engineType, multi.name + ": member " + juce::String(m + 1) + " engine");
                for (auto& cell : PresetRoute::namedCells(member.preset.engineType, member.preset.soundParams))
                    expectEquals((int) std::lround(r.audio.getTrackParameter(m, cell.id)), cell.value,
                                 multi.name + " / " + member.preset.name + ": id " + juce::String(cell.id) + " (" + cell.name + ") as loaded");
            }
            for (int m = 0; m < multi.memberCount(); ++m)
                r.play(multi, m);
            const auto first = r.take(1.5);
            const float level = pm::rms(first), top = pm::peak(first);
            logMessage(multi.name.paddedRight(' ', 10) + " level " + juce::String(pm::dB(level), 1) + " dB, peak " + juce::String(top, 2));
            expect(level > 0.003f && level < 0.5f, multi.name + ": level " + juce::String(level, 4));
            expect(top <= 1.0f, multi.name + ": peak " + juce::String(top, 3));
            sane(r, multi.name);

            // Each member alone.
            for (int m = 0; m < multi.memberCount(); ++m)
            {
                Rig one;
                if (! load(one, multi))
                    break;
                one.solo(m, multi.memberCount());
                for (int k = 0; k < multi.memberCount(); ++k)
                    one.play(multi, k);
                const float alone = pm::rms(one.take(1.0));
                // (Howl Loop's second track has no sound of its own: it is the loop's other half.)
                if (! (multi.name == "HowlLoop" && m == 1))
                    expect(alone > 1.0e-4f, multi.name + " member " + juce::String(m + 1) + " is silent (" + juce::String(alone, 6) + ")");
                expect(alone < 0.6f, multi.name + " member " + juce::String(m + 1) + " too loud (" + juce::String(alone, 3) + ")");
                sane(one, multi.name + " member " + juce::String(m + 1));
            }
        }

        beginTest("Every factory multi: the member the keys play answers the mod strip");
        for (auto& multi : multis)
        {
            auto heard = [&](float strip)
            {
                Rig r;
                if (! load(r, multi))
                    return std::vector<float>();
                const int t = multi.playIndex();
                r.audio.getEngine(t)->setModWheel(strip);
                r.take(0.25);                               // (the smoothed strip arrives)
                for (int m = 0; m < multi.memberCount(); ++m)
                    r.play(multi, m);
                r.solo(t, multi.memberCount());
                return r.take(1.5);
            };
            const auto rest = heard(0.0f), again = heard(0.0f), up = heard(1.0f);
            if (rest.empty() || up.empty())
                continue;
            const float same = fastmeasure::difference(rest, again, 2048), moved = fastmeasure::difference(rest, up, 2048);
            logMessage(multi.name.paddedRight(' ', 10) + " strip: " + juce::String(moved, 3) + " (two takes at rest: " + juce::String(same, 3) + ")");
            expect(moved > same + 0.03f, multi.name + ": the strip changes it by " + juce::String(moved, 4)
                                         + ", two takes at rest differ by " + juce::String(same, 4));
        }

        beginTest("DrumHarp: a hit of the kit rings the chord the resonator holds");
        {
            Rig r;
            const auto& multi = named("DrumHarp");
            if (load(r, multi))
            {
                r.solo(1, 2);
                r.play(multi, 1);                           // C3 E3 G3 C4
                r.take(5.0);                                // the chord's own strike rings out
                const float quiet = pm::rms(r.take(0.3));
                r.on(0, 8, 1.0f);                           // the kick
                r.take(0.03);
                const auto rung = r.take(0.5);
                const float excited = pm::rms(rung);
                const double chord = pm::tone(rung, pm::noteHz(48), SR) + pm::tone(rung, pm::noteHz(52), SR)
                                     + pm::tone(rung, pm::noteHz(55), SR) + pm::tone(rung, pm::noteHz(60), SR);
                const double between = pm::tone(rung, pm::noteHz(50), SR) + pm::tone(rung, pm::noteHz(53.5), SR)
                                       + pm::tone(rung, pm::noteHz(57.5), SR) + pm::tone(rung, pm::noteHz(62), SR);
                logMessage("DrumHarp: before the hit " + juce::String(pm::dB(quiet), 1) + " dB, after " + juce::String(pm::dB(excited), 1)
                           + " dB; the chord's four notes over four pitches between them " + juce::String(pm::powerDb(chord / between), 1) + " dB");
                expect(excited > 1.0e-3f && pm::dB(excited / std::max(quiet, 1.0e-9f)) >= 12.0f, "the hit raises the strings by 12 dB or more");
                expect(pm::powerDb(chord / between) >= 10.0f, "what rings is the chord");
                sane(r, multi.name);
            }
        }

        beginTest("VoxPlate: the break rings the plate; without the break the plate is silent");
        {
            Rig r;
            const auto& multi = named("VoxPlate");
            if (load(r, multi))
            {
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(5.0);
                const float quiet = pm::rms(r.take(0.5));
                r.play(multi, 0);                           // the break starts
                r.take(0.2);
                const float fed = pm::rms(r.take(2.0));
                logMessage("VoxPlate: plate alone " + juce::String(pm::dB(quiet), 1) + " dB, with the break " + juce::String(pm::dB(fed), 1) + " dB");
                expect(fed > 3.0e-3f && pm::dB(fed / std::max(quiet, 1.0e-9f)) >= 12.0f, "the break raises the plate by 12 dB or more");
                sane(r, multi.name);
            }
        }

        beginTest("RobtBrk: the chord is heard only where the break is, and with the break's rhythm");
        {
            Rig r;
            const auto& multi = named("RobtBrk");
            if (load(r, multi))
            {
                EffectType type {};
                expect(r.audio.getInsertEffectType(1, 0, type) && type == EffectType::VocoderEffect, "VOC on the carrier");
                expectEquals(r.audio.getTrackParameter(1, TrackInParam::SRC), 1.0f, "the carrier hears track 1");
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(0.5);
                const float mute = pm::rms(r.take(0.5));    // the chord with nothing to speak it
                r.play(multi, 0);
                r.take(0.1);
                const auto spoken = r.take(2.4);            // a bar of the break at 100 bpm
                const auto steps = pm::levels(spoken, 0.03, SR);
                const float loud = *std::max_element(steps.begin(), steps.end());
                float soft = loud;
                for (float s : steps) soft = std::min(soft, s);
                logMessage("RobtBrk: no break " + juce::String(pm::dB(mute), 1) + " dB, with it " + juce::String(pm::dB(pm::rms(spoken)), 1)
                           + " dB; 30 ms windows from " + juce::String(soft, 1) + " to " + juce::String(loud, 1) + " dB");
                expect(pm::dB(pm::rms(spoken) / std::max(mute, 1.0e-9f)) >= 20.0f, "the break opens the chord by 20 dB or more");
                expect(loud - soft >= 12.0f, "the chord has the break's hits and gaps");
                sane(r, multi.name);
            }
        }

        beginTest("RingVox: sum and difference tones of the pad and the table, the pad's own partial taken away");
        {
            // One note each: the pad on C4 (261.6 Hz), the table on G3 (196 Hz).
            struct Heard { double sum, difference, own; };
            auto heard = [&](bool ring)
            {
                Rig r;
                const auto& multi = named("RingVox");
                Heard h { 0.0, 0.0, 0.0 };
                if (! load(r, multi))
                    return h;
                if (! ring)
                    r.audio.setTrackParameter(1, TrackInParam::MODE, 0.0f);
                r.solo(1, 2);
                r.on(0, 55);
                r.on(1, 60);
                r.take(0.6);
                const auto x = r.take(1.0);
                const double pad = pm::noteHz(60), table = pm::noteHz(55);
                h = { pm::tone(x, pad + table, SR), pm::tone(x, pad - table, SR), pm::tone(x, pad, SR) };
                sane(r, multi.name);
                return h;
            };
            const auto with = heard(true), without = heard(false);
            logMessage("RingVox: at the sum " + juce::String(pm::powerDb(with.sum / without.sum), 1) + " dB, at the difference "
                       + juce::String(pm::powerDb(with.difference / without.difference), 1) + " dB, the pad's own first partial "
                       + juce::String(pm::powerDb(with.own / without.own), 1) + " dB (MODE RING against OFF)");
            expect(pm::powerDb(with.sum / without.sum) >= 20.0f, "the sum tone");
            expect(pm::powerDb(with.difference / without.difference) >= 20.0f, "the difference tone");
            expect(pm::powerDb(with.own / without.own) <= -6.0f, "the pad's own partial is carried into the side bands");
        }

        beginTest("SmplFM: the break on the operators' phase tears the bell into noise");
        {
            auto heard = [&](bool fed, float& line)
            {
                Rig r;
                const auto& multi = named("SmplFM");
                std::vector<float> x;
                if (! load(r, multi))
                    return x;
                r.solo(1, 2);
                if (fed)
                    r.play(multi, 0);
                r.take(0.1);
                r.on(1, 72);
                x = r.take(1.2);
                // The bell's partials are at C5 and at 3.5 times C5 either side of it: between them
                // (0.77 and 1.3 times C5) a clean bell has nothing. What is there, against its first partial.
                const double c5 = pm::noteHz(72);
                line = pm::powerDb((pm::tone(x, 0.77 * c5, SR) + pm::tone(x, 1.3 * c5, SR)) / std::max(1.0e-18, pm::tone(x, c5, SR)));
                sane(r, multi.name);
                return x;
            };
            float grain = 0.0f, clean = 0.0f;
            const auto torn = heard(true, grain), bell = heard(false, clean);
            const float changed = fastmeasure::difference(bell, torn, 2048);
            logMessage("SmplFM: between the bell's partials, " + juce::String(clean, 1) + " dB against its first partial; with the break on the operators' phase "
                       + juce::String(grain, 1) + " dB; difference " + juce::String(changed, 3));
            expect(changed >= 0.15f, "the break changes the bell (" + juce::String(changed, 3) + ")");
            expect(grain >= clean + 20.0f, "it fills the gaps between the partials: 20 dB or more of grain");
        }

        beginTest("Sync Saw: the lead's partials are the bass note's, whatever the lead plays, and follow the bass");
        {
            // The lead on F#4 (370.0 Hz). Synced to the sub on C2 (65.41 Hz) its lines are multiples
            // of 65.41: 327.0 Hz is the one under the note; to the sub on Eb2 (77.78 Hz): 311.1 Hz.
            struct Heard { double own, c2, eb2; };
            auto heard = [&](int subNote, bool synced)
            {
                Rig r;
                const auto& multi = named("Sync Saw");
                Heard h { 0.0, 0.0, 0.0 };
                if (! load(r, multi))
                    return h;
                if (! synced)
                    r.audio.setTrackParameter(1, UnifiedVoiceEngine::XIN_IDS + mod::Router::XinAmount1, 0.0f);
                r.solo(1, 2);
                r.on(0, subNote);
                r.on(1, 66);
                r.take(0.5);
                const auto x = r.take(0.8);
                const double c = pm::noteHz(36), e = pm::noteHz(39);
                h = { pm::tone(x, pm::noteHz(66), SR), pm::tone(x, 5.0 * c, SR), pm::tone(x, 4.0 * e, SR) };
                sane(r, multi.name);
                return h;
            };
            const auto onC = heard(36, true), onEb = heard(39, true), free = heard(36, false);
            logMessage("Sync Saw: lines of the bass over the lead's own note, bass on C " + juce::String(pm::powerDb(onC.c2 / onC.own), 1)
                       + " dB, bass on Eb " + juce::String(pm::powerDb(onEb.eb2 / onEb.own), 1) + " dB, not synced "
                       + juce::String(pm::powerDb(free.c2 / free.own), 1) + " dB; C's lines against Eb's with the bass on Eb "
                       + juce::String(pm::powerDb(onEb.c2 / onEb.eb2), 1) + " dB");
            expect(pm::powerDb(onC.c2 / onC.own) >= 10.0f, "synced: the bass note's partials, not the lead's own");
            expect(pm::powerDb(free.c2 / free.own) <= -10.0f, "not synced: the lead's own note");
            expect(pm::powerDb(onEb.eb2 / onEb.c2) >= 10.0f, "the lines follow the bass to another note");
        }

        beginTest("HowlLoop: silent until a note starts it, then it howls on by itself; with LEVL down it dies; bounded");
        {
            struct Heard { float first, early, late, top, centroid; bool ok; };
            auto heard = [&](float level)
            {
                Rig r;
                const auto& multi = named("HowlLoop");
                Heard h { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };
                if (! load(r, multi))
                    return h;
                r.audio.setTrackParameter(0, TrackInParam::LEVEL, level);
                r.audio.setTrackParameter(1, TrackInParam::LEVEL, level);
                h.first = pm::peak(r.take(0.3));            // the pair as loaded, no note yet
                r.on(0, 45);
                auto x = r.take(0.25);
                r.off(0, 45);
                const auto rest = r.take(4.0);
                x.insert(x.end(), rest.begin(), rest.end());
                h.early = pm::rms(pm::part(x, 1.0, 1.5, SR));
                h.late = pm::rms(pm::part(x, 3.5, 4.0, SR));
                h.top = pm::peak(x);
                h.centroid = pm::centroid(pm::part(x, 3.0, 4.0, SR), SR);
                h.ok = r.allFinite && r.audio.getNonFiniteBlocks() == 0;
                return h;
            };
            const auto stored = heard(1.0f), half = heard(0.5f), low = heard(0.2f);
            for (auto* h : { &stored, &half, &low })
                logMessage(juce::String("HowlLoop, LEVL ") + (h == &stored ? "100" : h == &half ? " 50" : " 20") + ": before the note "
                           + juce::String(h->first, 6) + ", 1 s after it " + juce::String(pm::dB(h->early), 1) + " dB, 3.5 s after "
                           + juce::String(pm::dB(h->late), 1) + " dB, peak " + juce::String(h->top, 2) + ", " + juce::String((int) h->centroid) + " Hz");
            expect(stored.ok && half.ok && low.ok, "finite at every LEVL");
            expect(stored.first < 1.0e-5f, "nothing sounds before a note (the first blocks of a feedback pair)");
            expect(stored.late > 0.01f && pm::dB(stored.late / std::max(stored.early, 1.0e-9f)) > -3.0f, "as stored it goes on by itself");
            expect(low.late < 1.0e-4f, "with LEVL down the loop dies");
            expect(stored.centroid >= 2.0f * half.centroid, "and the higher LEVL is, the more the fold bends it: it screams");
            expect(stored.top <= 1.0f, "bounded");
        }

        beginTest("PumpPad: the kick's own loudness takes the pad down and darkens it, and it comes back; no compressor");
        {
            // The pad under one kick: as stored; without the slot on the level; without the slot on the cutoff.
            struct Heard { float open, under, back, bright; };
            auto heard = [&](int without)
            {
                Rig r;
                const auto& multi = named("PumpPad");
                Heard h { 0.0f, 0.0f, 0.0f, 0.0f };
                if (! load(r, multi))
                    return h;
                EffectType type {};
                expect(! r.audio.getInsertEffectType(1, 0, type), "no insert on the pad");
                if (without >= 0)
                    r.audio.setTrackParameter(1, ModSlotParam::id(without, ModSlotParam::AMT), 0.0f);
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(1.0);
                h.open = pm::rms(r.take(0.3));
                r.on(0, 8, 1.0f);
                r.take(0.02);
                const auto ducked = r.take(0.1);
                h.under = pm::rms(ducked);
                h.bright = pm::centroid(ducked, SR);
                r.take(1.2);
                h.back = pm::rms(r.take(0.3));
                sane(r, multi.name);
                return h;
            };
            const auto stored = heard(-1), noLevel = heard(0), noCutoff = heard(1);
            const float fall = pm::dB(stored.under / stored.open), ret = pm::dB(stored.back / stored.open);
            logMessage("PumpPad: under the kick " + juce::String(fall, 1) + " dB (" + juce::String(pm::dB(noLevel.under / noLevel.open), 1)
                       + " dB without the slot on AMP), brightness x " + juce::String(stored.bright / std::max(1.0f, noCutoff.bright), 2)
                       + " of what it is without the slot on CUT, 1.3 s later " + juce::String(ret, 1) + " dB");
            expect(fall <= -6.0f, "the level falls by 6 dB or more");
            expect(pm::dB(noLevel.under / noLevel.open) >= fall + 4.0f, "by the slot IN > AMP");
            expect(stored.bright <= 0.85f * noCutoff.bright, "and it is darker, by the slot IN > CUT");
            expect(std::abs(ret) <= 2.0f, "and comes back");
        }

        beginTest("Key CMP: the pad's compressor is keyed from the kit's track");
        {
            Rig r;
            const auto& multi = named("Key CMP");
            if (load(r, multi))
            {
                EffectType type {};
                expect(r.audio.getInsertEffectType(1, 0, type) && type == EffectType::CompressorEffect, "CMP on the pad");
                expectEquals(r.audio.getTrackParameter(1, AudioEngine::INSERT_EXTRA_BASE), 1.0f, "KEY = track 1");
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(1.0);
                const float open = pm::rms(r.take(0.3));
                r.on(0, 8, 1.0f);
                r.take(0.02);
                const float pressed = pm::rms(r.take(0.1));
                r.take(1.2);
                const float back = pm::rms(r.take(0.3));
                logMessage("Key CMP: under the kick " + juce::String(pm::dB(pressed / open), 1) + " dB, 1.3 s later " + juce::String(pm::dB(back / open), 1) + " dB");
                expect(pm::dB(pressed / open) <= -4.0f, "the kick compresses the pad by 4 dB or more");
                expect(std::abs(pm::dB(back / open)) <= 2.0f, "and it comes back");
                sane(r, multi.name);
            }
        }

        beginTest("VocTune: what the break speaks through is tuned to the note the track plays");
        {
            Rig r;
            const auto& multi = named("VocTune");
            if (load(r, multi))
            {
                expectEquals(r.audio.getTrackParameter(1, AudioEngine::EFFECT_PARAM_BASE), 1.0f, "VOC MODE = TUNE");
                r.solo(1, 2);
                r.play(multi, 0);
                // C3, then F#3: the power at the note's first four harmonics against the other note's.
                const int notes[2] = { 48, 54 };
                float margin[2] = { 0.0f, 0.0f };
                for (int k = 0; k < 2; ++k)
                {
                    r.on(1, notes[k]);
                    r.take(0.6);
                    const auto x = r.take(1.8);
                    margin[k] = pm::powerDb(pm::harmonics(x, pm::noteHz(notes[k]), 4, SR) / pm::harmonics(x, pm::noteHz(notes[1 - k]), 4, SR));
                    r.off(1, notes[k]);
                    r.take(1.5);
                }
                logMessage("VocTune: the played note's harmonics over the other note's, on C3 " + juce::String(margin[0], 1)
                           + " dB, on F#3 " + juce::String(margin[1], 1) + " dB");
                expect(margin[0] >= 6.0f && margin[1] >= 6.0f, "the body follows the note");
                sane(r, multi.name);
            }
        }

        beginTest("Mod Dly: the lead's LFO bends its echo's TIME; pressure throws it into the echo; the pad's LFO sweeps its filter");
        {
            const auto& multi = named("Mod Dly");
            // (a) A C6 pluck and its first echo a quarter of a second later: the echo's pitch
            // against the note's. A delay that grows or shrinks plays back flat or sharp.
            auto echoPitch = [&](bool moved)
            {
                Rig r;
                if (! load(r, multi))
                    return 0.0;
                if (! moved)
                    r.audio.setTrackParameter(0, mod::Router::lfoId(0, mod::Router::Depth), 0.0f);
                r.solo(0, 2);
                r.audio.setTrackParameter(0, AudioEngine::INSERT_WET_BASE, 0.5f);       // (so the echo is as loud as the note)
                r.take(0.37);                               // (somewhere on the LFO's slope)
                r.on(0, 84);
                auto x = r.take(0.12);
                r.off(0, 84);
                const auto rest = r.take(0.5);
                x.insert(x.end(), rest.begin(), rest.end());
                const double hz = pm::noteHz(84);
                const double dry = pm::frequencyNear(x, 0.02, 0.004, hz, SR), echo = pm::frequencyNear(x, 0.29, 0.004, hz, SR);
                sane(r, multi.name);
                return 1200.0 * std::log2(echo / dry);
            };
            const double bent = echoPitch(true), straight = echoPitch(false);
            logMessage("Mod Dly: the first echo against its note, " + juce::String(bent, 1) + " cents with the LFO on TIME, "
                       + juce::String(straight, 1) + " cents without");
            expect(std::abs(bent) >= std::abs(straight) + 8.0 && std::abs(bent) <= 50.0, "the LFO bends the echo, by 8 to 50 cents more than a still delay shows");
            expect(std::abs(straight) <= 5.0, "a still delay does not (what the measure shows of a decaying note: under 5 cents)");

            // (b) Pressure on WET: the note's own level (before any echo is back) falls, and the
            // effect's mix as the chain reads it rises.
            auto pressed = [&](float pressure, float& wet)
            {
                Rig r;
                if (! load(r, multi))
                    return 0.0f;
                r.solo(0, 2);
                r.on(0, 84);
                r.audio.getEngine(0)->aftertouch(84, pressure);
                r.take(0.05);
                const float dry = pm::rms(r.take(0.12));
                if (auto* chain = r.audio.getInsertChain(0))
                    if (auto* fx = chain->getEffect(0))
                        wet = fx->wetWanted();
                sane(r, multi.name);
                return dry;
            };
            float wetSoft = 0.0f, wetHard = 0.0f;
            const float soft = pressed(0.0f, wetSoft), hard = pressed(1.0f, wetHard);
            logMessage("Mod Dly: WET " + juce::String(wetSoft, 2) + " at rest, " + juce::String(wetHard, 2) + " pressed; the dry note "
                       + juce::String(pm::dB(hard / std::max(soft, 1.0e-9f)), 1) + " dB under pressure");
            expectWithinAbsoluteError(wetSoft, 0.15f, 0.02f, "WET as stored");
            expect(wetHard >= 0.8f, "pressure takes WET to 80 or more");
            expect(pm::dB(hard / std::max(soft, 1.0e-9f)) <= -8.0f, "and the dry note falls by 8 dB or more");

            // (c) The pad: its brightness in quarter-second windows over one cycle of LFO 2 (4 s).
            auto sweep = [&](bool moved)
            {
                Rig r;
                if (! load(r, multi))
                    return 1.0f;
                if (! moved)
                    r.audio.setTrackParameter(1, mod::Router::lfoId(1, mod::Router::Depth), 0.0f);
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(1.0);
                const auto x = r.take(4.0);
                float lo = 1.0e9f, hi = 0.0f;
                for (int w = 0; w < 16; ++w)
                {
                    const float c = pm::centroid(pm::part(x, 0.25 * w, 0.25 * (w + 1), SR), SR);
                    lo = std::min(lo, c);
                    hi = std::max(hi, c);
                }
                sane(r, multi.name);
                return hi / std::max(lo, 1.0f);
            };
            const float swept = sweep(true), still = sweep(false);
            logMessage("Mod Dly: the pad's brightness over four seconds, x " + juce::String(swept, 2) + " with LFO 2 on the filter effect's cutoff, x "
                       + juce::String(still, 2) + " without");
            expect(swept >= 2.5f && swept >= 2.0f * still, "the LFO sweeps the insert's cutoff");
        }

        beginTest("Rvb Pump: the hall's tail pumps under the kick (a compressor after it, keyed from the kit)");
        {
            Rig r;
            const auto& multi = named("Rvb Pump");
            if (load(r, multi))
            {
                EffectType t0 {}, t1 {};
                expect(r.audio.getInsertEffectType(1, 0, t0) && t0 == EffectType::HallReverbEffect, "the hall in slot 1");
                expect(r.audio.getInsertEffectType(1, 1, t1) && t1 == EffectType::CompressorEffect, "CMP in slot 2");
                expectEquals(r.audio.getTrackParameter(1, AudioEngine::INSERT_EXTRA_BASE + 4), 1.0f, "its KEY = track 1");
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(1.0);
                const float open = pm::rms(r.take(0.3));
                r.on(0, 8, 1.0f);
                r.take(0.02);
                const float pumped = pm::rms(r.take(0.1));
                logMessage("Rvb Pump: under the kick " + juce::String(pm::dB(pumped / open), 1) + " dB");
                expect(pm::dB(pumped / open) <= -4.0f, "the kick takes pad and tail down by 4 dB or more");
                sane(r, multi.name);
            }
        }

        beginTest("BandDuck: the pad ducks under the kit's low bands (the kick, not the clap); the clap's bands open its filter");
        {
            Rig r;
            const auto& multi = named("BandDuck");
            if (load(r, multi))
            {
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(1.2);
                auto bands = [&]
                {
                    juce::String s;
                    if (const auto* router = r.audio.getEngine(1)->modRouter())
                        for (int b = 0; b < mod::Src::NUM_BANDS; ++b)
                            s << juce::String(router->bandLevel(b), 2) << " ";
                    return s;
                };
                struct Hit { float level, bright; };
                auto hit = [&](int note)
                {
                    const auto open = r.take(0.2);
                    r.on(0, note, 1.0f);
                    r.take(0.015);
                    logMessage("BandDuck, note " + juce::String(note) + ": bands " + bands());
                    const auto under = r.take(0.06);
                    r.take(1.5);
                    return Hit { pm::dB(pm::rms(under) / pm::rms(open)),
                                 pm::centroid(under, SR) / std::max(1.0f, pm::centroid(pm::part(open, 0.0, 0.06, SR), SR)) };
                };
                const Hit kick = hit(8), clap = hit(13);
                logMessage("BandDuck: under the kick " + juce::String(kick.level, 1) + " dB, brightness x " + juce::String(kick.bright, 2)
                           + "; under the clap " + juce::String(clap.level, 1) + " dB, brightness x " + juce::String(clap.bright, 2));
                expect(kick.level <= -4.0f, "the kick ducks it by 4 dB or more");
                expect(clap.level >= kick.level + 3.0f, "the clap ducks it less by 3 dB or more");
                expect(clap.bright >= 1.2f, "the clap opens the filter");
                expect(kick.bright < clap.bright - 0.1f, "the kick does not");
                sane(r, multi.name);
            }
        }

        beginTest("HitSweep: a hit of the kit throws the pad's filter open and it falls shut; nothing moves without a hit");
        {
            auto swept = [&](bool waitsForInput, float& before, float& during, float& after)
            {
                Rig r;
                const auto& multi = named("HitSweep");
                if (! load(r, multi))
                    return;
                if (! waitsForInput)
                {
                    r.audio.setTrackParameter(1, mod::Router::lfoId(0, mod::Router::Trigger), PresetRoute::trigger("ONE"));
                    r.audio.setTrackParameter(1, ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);     // (ONST is the input's too)
                }
                r.solo(1, 2);
                r.play(multi, 1);
                r.take(2.0);
                before = pm::centroid(r.take(0.2), SR);
                r.on(0, 8, 1.0f);
                r.take(0.01);
                during = pm::centroid(r.take(0.1), SR);
                r.take(1.0);
                after = pm::centroid(r.take(0.2), SR);
                sane(r, multi.name);
            };
            float b = 0.0f, d = 0.0f, a = 0.0f, nb = 0.0f, nd = 0.0f, na = 0.0f;
            swept(true, b, d, a);
            swept(false, nb, nd, na);
            logMessage("HitSweep: " + juce::String((int) b) + " Hz, in the hit " + juce::String((int) d) + " Hz, a second later " + juce::String((int) a)
                       + " Hz; with TRIG ONE (the note, not the input) " + juce::String((int) nb) + " / " + juce::String((int) nd) + " / " + juce::String((int) na));
            expect(d >= 1.4f * b, "the hit opens the filter");
            expect(std::abs(a / std::max(b, 1.0f) - 1.0f) <= 0.15f, "and it is shut again a second later");
            expect(nd <= 1.15f * nb, "an LFO that waits for the note does not answer the kit");
        }
    }
};

static FactoryMultiTests factoryMultiTests;
