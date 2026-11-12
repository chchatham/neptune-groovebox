#pragma once
#include "preset_audition_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/SampleBank.h"
#include "../src/engine/SamplerEngine.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/PresetManager.h"
#include "../src/project/ProjectBridge.h"
#include "../src/sequencer/SequencerCore.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <vector>

// The drum breaks of the sample bank (two rendered from the drum voices, six recordings from
// assets/breaks built into the program) and the two factory presets each of them has: where they
// sit in the bank, that they are whole bars at the tempo their name gives, that the presets play
// them in time and in slices, and that stored sounds find their sample by name when the places in
// the bank have moved.
class BreakbeatTests : public juce::UnitTest
{
public:
    BreakbeatTests() : UnitTest("Breakbeats") {}

    using S = SamplerEngine;
    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;

    // The preset's values on a fresh sampler, as AudioEngine::applyAllTrackParameters gives them:
    // the sample first.
    static std::unique_ptr<SamplerEngine> engineWith(const ParamMap& sound, float tempo = 120.0f)
    {
        auto e = std::make_unique<SamplerEngine>();
        e->prepareToPlay(SR, BLOCK);
        e->setTempo(tempo);
        if (auto it = sound.find(S::SampleSelect); it != sound.end())
            e->setParameter(S::SampleSelect, it->second);
        for (auto& [id, value] : sound)
            if (id != S::SampleSelect)
                e->setParameter(id, value);
        return e;
    }

    static std::vector<float> render(SamplerEngine& e, double seconds)
    {
        std::vector<float> out;
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        const int total = (int) (seconds * SR);
        while ((int) out.size() < total)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < BLOCK; ++i)
                out.push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
        }
        return out;
    }

    static double rms(const float* x, int n)
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += (double) x[i] * (double) x[i];
        return std::sqrt(sum / (double) std::max(1, n));
    }

    // Loudness per 10 ms.
    static std::vector<float> envelope(const std::vector<float>& x, int from, int to)
    {
        std::vector<float> env;
        const int hop = (int) (0.01 * SR);
        for (int i = from; i + hop <= to; i += hop)
            env.push_back((float) rms(x.data() + i, hop));
        return env;
    }

    static double correlation(const std::vector<float>& a, const std::vector<float>& b)
    {
        const size_t n = std::min(a.size(), b.size());
        double ma = 0.0, mb = 0.0;
        for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= (double) n; mb /= (double) n;
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            ab += (a[i] - ma) * (b[i] - mb);
            aa += (a[i] - ma) * (a[i] - ma);
            bb += (b[i] - mb) * (b[i] - mb);
        }
        return ab / std::sqrt(std::max(1.0e-12, aa * bb));
    }

    static const Preset* find(const std::vector<Preset>& list, const juce::String& name)
    {
        for (auto& p : list)
            if (p.name == name)
                return &p;
        return nullptr;
    }

    static juce::String titleOf(const SampleBank::Entry& e)
    {
        return "Break " + juce::String(e.name).substring(1).trimCharactersAtStart("0");
    }

    void runTest() override
    {
        auto& bank = SampleBank::get();

        beginTest("The bank: the old places are untouched and the breaks follow the user's files");
        {
            // The first ten are what they have been since before the breaks (projects store the place).
            static const char* const first[] = { "BEAT", "KICK", "SNAR", "HAT", "OHAT", "CLAP", "TOM", "CYMB", "RIM", "TONE" };
            for (int i = 0; i < 10; ++i)
                expectEquals(juce::String(bank.entry(i)->name), juce::String(first[i]));
            expectEquals(bank.defaultIndex(), 9);

            // Rendered breaks first, then the files, in this order for good (new ones at the end).
            static const char* const breaks[] = { "B100", "B170", "B080", "B090", "B105", "B120", "B157", "B160" };
            static const float bars[] = { 2, 2, 2, 2, 4, 2, 1, 4 };
            expectEquals(NEPTUNE_HAS_BREAK_DATA, 1, "the recordings of assets/breaks are built in");
            expectEquals(bank.breakCount(), 8);
            const int start = bank.breakIndex(0);
            expect(start >= 10, "after the factory sounds");
            for (int i = 10; i < start; ++i)
                expect(bank.entry(i)->bars == 0.0f, "what lies between is the user's: files keep the places they had");
            logMessage("user files before the breaks: " + juce::String(start - 10) + "; breaks at " + juce::String(start)
                       + " .. " + juce::String(start + bank.breakCount() - 1));
            for (int n = 0; n < bank.breakCount() && n < 8; ++n)
            {
                expectEquals(bank.breakIndex(n), start + n);
                const auto* e = bank.entry(bank.breakIndex(n));
                expectEquals(juce::String(e->name), juce::String(breaks[n]));
                expectEquals(e->bars, bars[n]);
                expectEquals(bank.indexOfBreak(breaks[n]), start + n);
                expectEquals(juce::String(bank.names()[start + n]), juce::String(breaks[n]), "the SMPL cell lists it");
            }
            expectEquals(bank.breakIndex(bank.breakCount()), -1);
            expectEquals(bank.indexOfBreak("KICK"), -1, "only breaks are breaks");
            expect(bank.size() <= SampleBank::MAX_ENTRIES);
            expect(SampleBank::FILES_END + 8 <= SampleBank::MAX_ENTRIES, "the user's files cannot crowd the breaks out");
        }

        beginTest("Every break is whole bars at the tempo of its name, starts on a hit and is not clipped");
        {
            double seconds = 0.0;
            for (int n = 0; n < bank.breakCount(); ++n)
            {
                const auto* e = bank.entry(bank.breakIndex(n));
                const juce::String name(e->name);
                const int frames = (int) e->left.size();
                const double nominal = name.substring(1).getDoubleValue();
                seconds += (double) frames / e->sampleRate;
                float peak = 0.0f;
                for (float v : e->left) peak = std::max(peak, std::abs(v));
                for (float v : e->right) peak = std::max(peak, std::abs(v));
                const double head = rms(e->left.data(), (int) (0.05 * e->sampleRate));
                const double whole = rms(e->left.data(), frames);
                logMessage(name + ": " + juce::String(e->bars, 0) + " bars, " + juce::String(e->tempo(), 2) + " BPM, "
                           + juce::String((double) frames / e->sampleRate, 3) + " s, " + (e->right.empty() ? "mono" : "stereo")
                           + ", " + juce::String(e->sampleRate, 0) + " Hz, peak " + juce::String(peak, 3)
                           + ", first 50 ms " + juce::String(head / whole, 2) + " x the average level");
                expectWithinAbsoluteError(e->tempo(), nominal, 1.0, name + ": the tempo in the name");
                expect(e->right.empty() || e->right.size() == e->left.size());
                expect(peak > 0.5f && peak <= 1.0f, name + ": level");
                expect(head > 0.5 * whole, name + ": the first beat is at the start");
                expectEquals(e->defaultSlices, 0);
                expect(! e->sustains);
            }
            logMessage("all breaks together: " + juce::String(seconds, 1) + " s");
        }

        beginTest("Choosing a break sets LEN to its bars, so SYNC alone plays it in time");
        {
            for (int n = 0; n < bank.breakCount(); ++n)
            {
                const auto* e = bank.entry(bank.breakIndex(n));
                SamplerEngine s;
                s.prepareToPlay(SR, BLOCK);
                s.setTempo(120.0f);
                const float before = s.getParameter(S::StretchLength);
                s.setParameter(S::SampleSelect, (float) bank.breakIndex(n));
                expectEquals(S::stretchLengthBars()[(int) s.getParameter(S::StretchLength)], e->bars, juce::String(e->name));
                expectEquals((int) s.getParameter(S::Mode), (int) S::Forward, "a break is not a held tone");
                s.setParameter(S::StretchMode, (float) S::StretchTape);
                s.setParameter(S::StretchSync, 1.0f);
                // The speed that makes the break last its bars at 120.
                expectWithinAbsoluteError((double) s.getStretchSpeed(), 120.0 / e->tempo(), 0.001, juce::String(e->name));
                // Another sample leaves LEN alone.
                s.setParameter(S::SampleSelect, (float) bank.indexOf("KICK"));
                expectEquals(S::stretchLengthBars()[(int) s.getParameter(S::StretchLength)], e->bars);
                juce::ignoreUnused(before);
            }
            expectEquals(S::stretchLengthIndex(1.0f), 4);
            expectEquals(S::stretchLengthIndex(2.0f), 5);
            expectEquals(S::stretchLengthIndex(4.0f), 6);
            expectEquals(S::stretchLengthIndex(3.0f), 5, "nearest; a tie goes to the shorter");
        }

        const auto presets = FactoryPresets::getAll();

        beginTest("Every break has a Loop and a Slices preset that name it");
        {
            int found = 0;
            for (int n = 0; n < bank.breakCount(); ++n)
            {
                const auto* e = bank.entry(bank.breakIndex(n));
                for (const char* kind : { " Loop", " Slices" })
                {
                    const auto* p = find(presets, titleOf(*e) + kind);
                    expect(p != nullptr, titleOf(*e) + kind);
                    if (p == nullptr) continue;
                    ++found;
                    expect(p->engineType == EngineType::Sampler && p->isFactory);
                    expectEquals(p->sampleName, juce::String(e->name));
                    expectEquals((int) p->soundParams.at(S::SampleSelect), bank.breakIndex(n));
                    expect(PresetManager::getCategories().contains(p->category), "a category the PRESET screen has");
                    for (auto& [id, value] : p->soundParams)
                        expect(EngineParamRange::contains(id), "engine ids only");
                    // No artist, record or product names: "Break", the tempo, the kind.
                    expect(p->name.containsOnly("BreakLopSlics 0123456789"), p->name);
                }
            }
            expectEquals(found, 2 * bank.breakCount());

            // The presets that were there before still do not choose a sample.
            for (const char* old : { "Lo-Fi Tape", "Granular Pad", "Vinyl Scratch" })
            {
                const auto* p = find(presets, old);
                expect(p != nullptr && p->sampleName.isEmpty() && p->soundParams.count(S::SampleSelect) == 0, old);
            }
        }

        beginTest("A Loop preset plays the break round in time with the tempo, at any tempo");
        {
            for (int n = 0; n < bank.breakCount(); ++n)
            {
                const auto* e = bank.entry(bank.breakIndex(n));
                const auto* p = find(presets, titleOf(*e) + " Loop");
                if (p == nullptr) continue;
                for (float tempo : { 96.0f, 140.0f })
                {
                    auto s = engineWith(PresetManager::soundOf(*p), tempo);
                    expectEquals(s->getBankIndex(), bank.breakIndex(n));
                    expectEquals((int) s->getParameter(S::Mode), (int) S::Loop);
                    expect(s->getSliceCount() >= 4, "cut at the hits: " + juce::String(s->getSliceCount()) + " slices");

                    // One key held for three and a bit times round: the second and third time round
                    // are the same beat, a bar length of the tempo apart.
                    const double cycle = (double) e->bars * 240.0 / (double) tempo;
                    s->noteOn(60, 0.9f, 0);
                    const auto out = render(*s, 3.2 * cycle);
                    const int c = (int) std::lround(cycle * SR);
                    const auto second = envelope(out, c, 2 * c), third = envelope(out, 2 * c, 3 * c);
                    const double same = correlation(second, third);
                    // ... and not the same when read with a cycle a sixteenth too long.
                    const int off = (int) std::lround(cycle * SR * (1.0 + 1.0 / (16.0 * (double) e->bars)));
                    const double shifted = correlation(envelope(out, c, 2 * c), envelope(out, c + off, 2 * c + off));
                    logMessage(p->name + " at " + juce::String(tempo, 0) + ": " + juce::String(s->getSliceCount())
                               + " slices, speed " + juce::String(s->getStretchSpeed(), 3) + ", one round " + juce::String(cycle, 3)
                               + " s, round 2 against round 3: " + juce::String(same, 3) + " (a sixteenth out: "
                               + juce::String(shifted, 3) + "), level " + juce::String(rms(out.data() + c, c), 3));
                    expect(rms(out.data() + c, c) > 0.01, p->name + ": it sounds");
                    expect(same > 0.9, p->name + " repeats every " + juce::String(e->bars, 0) + " bars at " + juce::String(tempo, 0));
                    expect(shifted < same - 0.2, p->name + ": the length is the bars, not near them");
                    expectWithinAbsoluteError((double) s->getStretchSpeed(), (double) tempo / e->tempo(), 0.001);
                }
            }
        }

        beginTest("A Slices preset puts the hits on the keys from C4: each key its own slice, to its end");
        {
            for (int n = 0; n < bank.breakCount(); ++n)
            {
                const auto* e = bank.entry(bank.breakIndex(n));
                const auto* p = find(presets, titleOf(*e) + " Slices");
                if (p == nullptr) continue;
                auto s = engineWith(PresetManager::soundOf(*p));
                const int count = s->getSliceCount();
                expectEquals(s->getBankIndex(), bank.breakIndex(n));
                expect(count >= 8 && count <= 16, p->name + ": " + juce::String(count) + " slices for sixteen pads");
                expectEquals(s->getSlice(0).startSample, 0, "the first slice is the first beat");

                std::set<int> starts;
                int sounding = 0;
                double shortest = 1.0e9, longest = 0.0;
                for (int k = 0; k < count; ++k)
                {
                    const auto& slice = s->getSlice(k);
                    starts.insert(slice.startSample);
                    const double length = (double) (slice.endSample - slice.startSample) / e->sampleRate;
                    shortest = std::min(shortest, length);
                    longest = std::max(longest, length);

                    // A short tap: 1SHT plays the slice on after the key is let go.
                    s->noteOn(60 + k, 0.9f, 0);
                    render(*s, 0.004);
                    expectEquals(s->getPlayingSlice(), k, p->name + ", key " + juce::String(k));
                    s->noteOff(60 + k, 0);
                    const auto rest = render(*s, std::min(length, 0.08));
                    if (rms(rest.data(), (int) rest.size()) > 0.0005)
                        ++sounding;
                    s->allNotesOff();
                    render(*s, 0.05);
                }
                logMessage(p->name + ": " + juce::String(count) + " slices, " + juce::String(shortest * 1000.0, 0) + " .. "
                           + juce::String(longest * 1000.0, 0) + " ms, " + juce::String(sounding) + " sound after the key is up");
                expectEquals((int) starts.size(), count, "every slice starts somewhere else");
                expect(sounding >= count - 1, p->name + ": the slices sound to their end");
            }
        }

        beginTest("A preset finds its sample by name when the places in the bank have moved");
        {
            const int b090 = bank.indexOfBreak("B090"), kick = bank.indexOf("KICK"), tone = bank.indexOf("TONE");
            const auto* loop = find(presets, "Break 90 Loop");
            expect(loop != nullptr && b090 >= 0);
            if (loop != nullptr)
            {
                // The factory preset as another run would have built it (more or fewer user files).
                Preset moved = *loop;
                moved.soundParams[S::SampleSelect] = (float) kick;
                expectEquals((int) PresetManager::soundOf(moved).at(S::SampleSelect), b090);
                expectEquals(engineWith(PresetManager::soundOf(moved))->getBankIndex(), b090);
                // Through a file and back.
                const auto back = Preset::fromJSON(moved.toJSON());
                expectEquals(back.sampleName, juce::String("B090"));
                expectEquals((int) PresetManager::soundOf(back).at(S::SampleSelect), b090);
                Track track;
                PresetManager::applyToTrack(moved, track);
                expectEquals((int) track.soundParams.at(S::SampleSelect), b090);
            }

            // A sound the user saves remembers the name of its sample.
            Track track;
            track.engineType = EngineType::Sampler;
            track.soundParams[S::SampleSelect] = (float) b090;
            auto user = PresetManager::fromTrack(track, "User 1", "INIT");
            expectEquals(user.sampleName, juce::String("B090"));
            user.soundParams[S::SampleSelect] = (float) tone;           // the place it has in a later run
            expectEquals((int) PresetManager::soundOf(user).at(S::SampleSelect), b090);

            // A preset saved before names existed, and one whose sample is gone: the place as stored.
            Preset old;
            old.engineType = EngineType::Sampler;
            old.soundParams[S::SampleSelect] = (float) kick;
            expectEquals((int) PresetManager::soundOf(old).at(S::SampleSelect), kick);
            expect(old.toJSON().contains("sample") == false, "nothing new in the file of a preset without a sample name");
            old.sampleName = "GONE";
            expectEquals((int) PresetManager::soundOf(old).at(S::SampleSelect), kick);

            // Other engines: no sample name, nothing touched.
            Track fm;
            fm.engineType = EngineType::FM;
            fm.soundParams[24] = 3.0f;
            expect(PresetManager::fromTrack(fm, "F", "INIT").sampleName.isEmpty());
        }

        beginTest("PRESET: the highlighted break preset is what the track plays, found by name");
        {
            presetaudition::Rig rig(EngineType::Sampler);
            // A user preset whose stored place is stale (it points at TONE, its name says B157).
            Preset stale;
            stale.name = "Stale Place";
            stale.category = "PERC";
            stale.engineType = EngineType::Sampler;
            stale.soundParams[S::SampleSelect] = (float) bank.indexOf("TONE");
            stale.soundParams[S::Mode] = (float) S::Loop;
            stale.sampleName = "B157";
            rig.mgr.addPreset(stale);

            rig.screen->onActivate();
            int heardLoop = 0, heardSlices = 0;
            bool heardStale = false;
            for (int i = 0; i < 200; ++i)
            {
                if (const auto* p = rig.highlighted())
                {
                    auto* s = dynamic_cast<SamplerEngine*>(rig.audio.getEngine(0));
                    expect(s != nullptr);
                    if (s != nullptr && p->sampleName.isNotEmpty())
                    {
                        const int want = bank.indexOfBreak(p->sampleName);
                        expectEquals(s->getBankIndex(), want, p->name);
                        expectEquals((int) rig.seq.getPattern().getTrack(0).soundParams[S::SampleSelect], want, p->name);
                        if (p->name.endsWith(" Loop"))
                        {
                            ++heardLoop;
                            expect(s->getParameter(S::StretchSync) > 0.5f && (int) s->getParameter(S::StretchMode) == (int) S::StretchSlice, p->name);
                            expectEquals(S::stretchLengthBars()[(int) s->getParameter(S::StretchLength)], bank.entry(want)->bars, p->name);
                        }
                        else if (p->name.endsWith(" Slices"))
                        {
                            ++heardSlices;
                            expect(s->getSliceCount() >= 8 && s->getSliceCount() <= 16, p->name);
                            expect(s->getParameter(S::StretchSync) < 0.5f, p->name + ": nothing left over from the loop preset before it");
                        }
                        else if (p->name == "Stale Place")
                            heardStale = true;
                    }
                }
                const int at = rig.screen->getSelectedIndex();
                rig.screen->onPadPageNext();
                if (rig.screen->getSelectedIndex() == at)
                    break;
            }
            expectEquals(heardLoop, bank.breakCount());
            expectEquals(heardSlices, bank.breakCount());
            expect(heardStale, "the user's preset was in the list");
        }

        beginTest("A project finds its samples by name: a break, and an old project's sample");
        {
            const int b105 = bank.indexOfBreak("B105"), hat = bank.indexOf("HAT"), kick = bank.indexOf("KICK");
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            for (int t = 0; t < 2; ++t)
            {
                audio.addEngine(t, AudioEngine::createEngine(EngineType::Sampler));
                for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                    seq.getPattern(p).getTrack(t).engineType = EngineType::Sampler;
            }
            auto set = [&](int t, int id, float v)
            {
                audio.setTrackParameter(t, id, v);
                seq.getPattern().getTrack(t).soundParams[id] = v;
            };
            set(0, S::SampleSelect, (float) b105);
            set(0, S::StretchSync, 1.0f);
            set(1, S::SampleSelect, (float) hat);

            auto saved = ProjectBridge::capture({ seq, audio }, "BREAK");
            auto& tracks = saved.patterns[(size_t) saved.activePattern].tracks;
            expectEquals(tracks[0].sampleName, juce::String("B105"));
            expectEquals(tracks[1].sampleName, juce::String("HAT"));
            // The file as a run with other places would read it: both stored places are wrong.
            tracks[0].soundParams[S::SampleSelect] = (float) kick;
            tracks[1].soundParams[S::SampleSelect] = (float) b105;
            const auto reloaded = ProjectData::fromVar(saved.toVar());

            SequencerCore seq2;
            AudioEngine audio2;
            audio2.setSequencer(&seq2);
            ProjectBridge::apply(reloaded, { seq2, audio2 });
            auto* s0 = dynamic_cast<SamplerEngine*>(audio2.getEngine(0));
            auto* s1 = dynamic_cast<SamplerEngine*>(audio2.getEngine(1));
            expect(s0 != nullptr && s1 != nullptr);
            if (s0 != nullptr && s1 != nullptr)
            {
                expectEquals(s0->getBankIndex(), b105);
                expect(s0->getParameter(S::StretchSync) > 0.5f);
                expectEquals(S::stretchLengthBars()[(int) s0->getParameter(S::StretchLength)], 4.0f);
                expectEquals(s1->getBankIndex(), hat, "the old project's sample, not the break now at its number");
            }
        }
    }
};

static BreakbeatTests breakbeatTests;
