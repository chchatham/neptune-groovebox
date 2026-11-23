#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>
#include "../src/dsp/TempoDivisions.h"
#include "../src/dsp/LFO.h"
#include "../src/dsp/effects/EffectBase.h"
#include "../src/engine/AudioEngine.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/TrackFxScreen.h"

// One table of tempo divisions (dsp/TempoDivisions.h): an LFO's divisions and an effect's are
// views of it by name. What an LFO stored before keeps its length to the sample; every cell that
// offers divisions offers the whole table.
class TempoDivisionsTests : public juce::UnitTest
{
public:
    TempoDivisionsTests() : UnitTest("Tempo Divisions") {}

    // dsp/LFO.h's own table as it stood before the one table (3f77ffb), as the floats it used.
    static constexpr int OLD_COUNT = 17;
    static const char* oldName(int i)
    {
        static const char* const names[OLD_COUNT] = {
            "8BAR", "4BAR", "2BAR", "1BAR", "1/2D", "1/2", "1/4D", "1/2T", "1/4",
            "1/8D", "1/4T", "1/8", "1/16D", "1/8T", "1/16", "1/16T", "1/32" };
        return names[i];
    }
    static float oldBeats(int i)
    {
        static const float beats[OLD_COUNT] = {
            32.0f, 16.0f, 8.0f, 4.0f, 3.0f, 2.0f, 1.5f, 4.0f / 3.0f, 1.0f,
            0.75f, 2.0f / 3.0f, 0.5f, 0.375f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f };
        return beats[i];
    }

    // Samples from the first wrap of the phase to the n-th, as the old LFO stepped it:
    // phase += (double) (baseHz * 1.0f) / sr, baseHz = bpm / 60 / beats in float.
    static std::vector<int> oldWraps(float bpm, float beats, double sr, int wraps)
    {
        const float baseHz = bpm / 60.0f / beats;
        const double inc = (double) (baseHz * 1.0f) / sr;
        std::vector<int> at;
        double phase = 0.0;
        for (int i = 1; (int) at.size() < wraps && i < 40000000; ++i)
        {
            phase += inc;
            if (phase >= 1.0) { phase -= std::floor(phase); at.push_back(i); }
        }
        return at;
    }

    // The same wraps, read from an LFO (a rising saw: a wrap is a fall of more than one).
    static std::vector<int> lfoWraps(LFO& lfo, int wraps)
    {
        std::vector<int> at;
        float prev = -1.0f;
        for (int i = 1; (int) at.size() < wraps && i < 40000000; ++i)
        {
            const float v = lfo.process();
            if (v < prev - 1.0f) at.push_back(i);
            prev = v;
        }
        return at;
    }

    static bool isTableLabel(const char* s) { return tempo::indexOf(s) >= 0; }

    void runTest() override
    {
        beginTest("The table: shortest first, every label once, a view of all of it is whole");
        {
            for (int i = 1; i < tempo::COUNT; ++i) expect(tempo::beats(i) > tempo::beats(i - 1), tempo::labels()[i]);
            std::set<juce::String> seen;
            for (int i = 0; i < tempo::COUNT; ++i)
            {
                expect(seen.insert(tempo::labels()[i]).second, "once");
                expectEquals(tempo::indexOf(tempo::labels()[i]), i);
                expect(std::strlen(tempo::labels()[i]) <= 5, "fits a cell");
            }
            expectEquals(tempo::indexOf("1/3"), -1);
            expectEquals(tempo::indexOf(nullptr), -1);
            expect(! tempo::View<2>(std::array<const char*, 2> { "1/4", "1/4" }).valid(), "twice is not a view");
            expect(! tempo::View<1>(std::array<const char*, 1> { "1/5" }).valid(), "a name the table lacks is not a view");
            expect(tempo::View<1>(std::array<const char*, 1> { "1/4" }).valid() && ! tempo::View<1>(std::array<const char*, 1> { "1/4" }).whole());
            expect(LFO::DIVISIONS.whole());
        }

        beginTest("An LFO's divisions: the seventeen it stored mean what they meant, bit for bit; the five new ones follow");
        {
            expectEquals(LFO::NUM_DIVISIONS, tempo::COUNT);
            for (int i = 0; i < OLD_COUNT; ++i)
            {
                expectEquals(juce::String(LFO::divisionName(i)), juce::String(oldName(i)));
                expect(LFO::divisionBeats(i) == oldBeats(i), juce::String(oldName(i)) + ": the same float");
            }
            const char* added[] = { "1/32T", "1/64", "1/128", "16BAR", "1/32D" };
            for (int i = OLD_COUNT; i < LFO::NUM_DIVISIONS; ++i)
            {
                expectEquals(juce::String(LFO::divisionName(i)), juce::String(added[i - OLD_COUNT]));
                expect((double) LFO::divisionBeats(i) == (double) (float) tempo::beats(tempo::indexOf(added[i - OLD_COUNT])));
            }
            expectEquals(juce::String(LFO::divisionName(LFO::DEFAULT_DIVISION)), juce::String("1/4"));
            // The labels are the table's own strings (one copy of every name).
            for (int i = 0; i < LFO::NUM_DIVISIONS; ++i)
                expect(LFO::divisionLabels()[i] == tempo::labels()[LFO::DIVISIONS.tableIndex(i)]);
            // Out of range reads as the ends, as before.
            expectEquals(juce::String(LFO::divisionName(-3)), juce::String("8BAR"));
            expectEquals(juce::String(LFO::divisionName(99)), juce::String(LFO::divisionName(LFO::NUM_DIVISIONS - 1)));
        }

        beginTest("Every old stored index gives the same period to the sample (44.1 and 96 kHz, three tempi)");
        {
            for (double sr : { 44100.0, 96000.0 })
                for (float bpm : { 97.0f, 120.0f, 174.0f })
                    for (int d = 0; d < OLD_COUNT; ++d)
                    {
                        const int wraps = oldBeats(d) >= 8.0f ? 2 : 4;
                        if (oldBeats(d) > 8.0f && sr > 50000.0) continue;      // (time: the 44.1 kHz run has them)
                        LFO lfo;
                        lfo.setSampleRate(sr);
                        lfo.setShape(LFO::Shape::Saw);
                        lfo.setRate(3.7f);
                        lfo.setTempo(bpm);
                        lfo.setSyncDivision(d);
                        lfo.setTempoSync(true);
                        expect(lfo.getEffectiveRate() == bpm / 60.0f / oldBeats(d), "the same rate, bit for bit");
                        const auto was = oldWraps(bpm, oldBeats(d), sr, wraps);
                        const auto now = lfoWraps(lfo, wraps);
                        expect(was == now, juce::String(oldName(d)) + " @ " + juce::String(bpm) + " bpm, " + juce::String(sr)
                                               + ": wraps at " + (now.empty() ? juce::String("-") : juce::String(now.back()))
                                               + ", was " + (was.empty() ? juce::String("-") : juce::String(was.back())));
                    }
        }

        beginTest("The new divisions last their beats");
        {
            // (Their lengths written out here, not read back from the LFO: a wrong entry must fail.)
            const double addedBeats[] = { 1.0 / 12.0, 1.0 / 16.0, 1.0 / 32.0, 64.0, 3.0 / 16.0 };
            for (int d = OLD_COUNT; d < LFO::NUM_DIVISIONS; ++d)
            {
                expectWithinAbsoluteError((double) LFO::divisionBeats(d), addedBeats[d - OLD_COUNT], 1.0e-7);
                const double sr = 44100.0;
                const float bpm = 150.0f;
                LFO lfo;
                lfo.setSampleRate(sr);
                lfo.setShape(LFO::Shape::Saw);
                lfo.setTempo(bpm);
                lfo.setSyncDivision(d);
                lfo.setTempoSync(true);
                const auto now = lfoWraps(lfo, 2);
                expectEquals((int) now.size(), 2);
                if (now.size() == 2)
                {
                    const double expected = addedBeats[d - OLD_COUNT] * 60.0 / bpm * sr;
                    expect(std::abs((now[1] - now[0]) - expected) <= 1.0, juce::String(LFO::divisionName(d)));
                }
            }
        }

        beginTest("An effect's divisions are the whole table, in its order");
        {
            expectEquals((int) FxSync::NUM_DIVISIONS, tempo::COUNT);
            expect(FxSync::divisionLabels() == tempo::labels());
            for (int i = 0; i < tempo::COUNT; ++i) expect(FxSync::divisionBeats(i) == tempo::beats(i));
            // and the same lengths as an LFO's, entry for entry by name
            for (int i = 0; i < LFO::NUM_DIVISIONS; ++i)
            {
                const int e = tempo::indexOf(LFO::divisionName(i));
                expect(e >= 0 && (float) FxSync::divisionBeats(e) == LFO::divisionBeats(i));
            }
        }

        beginTest("Every cell that offers divisions offers the whole table, and keeps the last of it");
        {
            int lfoCells = 0;
            std::set<juce::String> privateLists;
            for (int t = 0; t < ENGINE_TYPE_COUNT; ++t)
            {
                const auto type = (EngineType) t;
                if (type == EngineType::MIDITrack) continue;
                auto e = AudioEngine::createEngine(type);
                if (e == nullptr) continue;
                e->prepareToPlay(44100.0, 64);
                std::vector<ParamSpec> cells;
                for (auto& page : e->getParamPages())
                    for (auto& p : page.params)
                        if (p.isValid()) cells.push_back(p);
                if (type == EngineType::UnifiedVoice)
                    for (int l = 0; l < 4; ++l)       // (its LFO pages are built by the PARAM screen: ids 130-145)
                        cells.push_back({ "RATE", ModParam::lfoExtParamId(l, ModParam::LFO_DIV), 0, (float) (LFO::NUM_DIVISIONS - 1),
                                          (float) LFO::DEFAULT_DIVISION, ParamKind::Choice, "", LFO::divisionLabels(), LFO::NUM_DIVISIONS });
                for (auto& p : cells)
                {
                    if (p.kind != ParamKind::Choice || p.choices == nullptr) continue;
                    int fromTable = 0;
                    for (int i = 0; i < p.numChoices; ++i)
                        if (p.choices[i] != nullptr && isTableLabel(p.choices[i])) ++fromTable;
                    const bool view = p.choices == LFO::divisionLabels() || p.choices == FxSync::divisionLabels();
                    const juce::String where = juce::String((int) type) + " " + p.label + " (" + juce::String(p.id) + ")";
                    if (! view)
                    {
                        // A private list of lengths, known and kept apart: the wavetable's scan LEN and
                        // the sampler's stretch LEN are lengths of a sweep and of a sample (whole bars
                        // without dots or triplets, LEN's own labels "16BR", "1", "32"), not divisions of
                        // a cycle. Any other list that looks like divisions fails here.
                        if (fromTable >= 3)
                        {
                            const bool known = juce::String(p.label) == "LEN"
                                               && (type == EngineType::Wavetable || type == EngineType::Sampler);
                            expect(known, "a private list of divisions: engine " + where);
                            privateLists.insert(where);
                        }
                        continue;
                    }
                    ++lfoCells;
                    expectEquals(p.numChoices, tempo::COUNT, where);
                    expectEquals((int) std::lround(p.maxVal), tempo::COUNT - 1, where);
                    // The last division is kept, not clamped to the old seventeen.
                    e->setParameter(p.id, (float) (tempo::COUNT - 1));
                    expectEquals((int) std::lround(e->getParameter(p.id)), tempo::COUNT - 1, where + " keeps the last division");
                    e->setParameter(p.id, (float) LFO::DEFAULT_DIVISION);
                }
            }
            logMessage("division cells on the engines' pages: " + juce::String(lfoCells));
            expect(lfoCells >= 8, "LFO division cells found: " + juce::String(lfoCells));
            expectEquals((int) privateLists.size(), 2, "the wavetable's and the sampler's LEN");
        }

        // The cells the screens build themselves (PARAM's MOD page for the unified voice, TRK FX's
        // synced RATE; PERF's DIV is held in "Perf FX"): read from the screens, turned to the end.
        beginTest("The screens' own division cells offer the whole table and turn to its end");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            ParamPageScreen param(seq, &audio);
            TrackFxScreen fx(seq, &audio);

            auto find = [](const ParamPage& page, const char* label) -> int
            {
                for (int i = 0; i < 8; ++i)
                    if (page.params[(size_t) i].isValid() && juce::String(page.params[(size_t) i].label) == label) return i;
                return -1;
            };

            // PARAM, MOD: RATE is the division while SYNC is on.
            audio.setTrackParameter(0, ModParam::lfoParamId(0, ModParam::LFO_SYNC), 1.0f);
            expect(param.selectPageByName("MOD"), "the unified voice has a MOD page");
            const int rateKnob = find(param.getCurrentPage(), "RATE");
            expect(rateKnob >= 0);
            if (rateKnob >= 0)
            {
                const auto rate = param.getCurrentPage().params[(size_t) rateKnob];
                expect(rate.kind == ParamKind::Choice && rate.choices == LFO::divisionLabels(), "PARAM's RATE is the LFO's view");
                expectEquals(rate.numChoices, tempo::COUNT, "PARAM's RATE offers the whole table");
                expectEquals((int) std::lround(rate.maxVal), tempo::COUNT - 1);
                param.onEncoderChange(rateKnob, 60);
                const int div = (int) std::lround(audio.getTrackParameter(0, ModParam::lfoExtParamId(0, ModParam::LFO_DIV)));
                expectEquals(div, LFO::NUM_DIVISIONS - 1, "turned to the view's end (1/32D)");
            }

            // TRK FX: a synced effect's RATE.
            audio.addInsertEffect(0, EffectType::WahEffect);
            expect(fx.selectPageByName("FX"));
            audio.setTrackParameter(0, AudioEngine::INSERT_SYNC_BASE + 0, 1.0f);
            const int fxKnob = find(fx.getCurrentPage(), "RATE");
            expect(fxKnob >= 0);
            if (fxKnob >= 0)
            {
                const auto rate = fx.getCurrentPage().params[(size_t) fxKnob];
                expect(rate.kind == ParamKind::Choice && rate.choices == FxSync::divisionLabels(), "TRK FX's RATE is the effects' view");
                expectEquals(rate.numChoices, tempo::COUNT, "TRK FX's RATE offers the whole table");
                expectEquals((int) std::lround(rate.maxVal), tempo::COUNT - 1);
                fx.onEncoderChange(fxKnob, 60);
                expectEquals((int) std::lround(audio.getTrackParameter(0, AudioEngine::INSERT_DIV_BASE + 0)), tempo::COUNT - 1,
                             "turned to 16BAR, the table's last");
            }
        }

        // The unified voice plays an LFO with a WHL / VEL / PRES amount on its own classic path
        // (the voices' LFOBank), not the router: a new division must reach that path too.
        beginTest("A unified voice's LFO on either path plays the new divisions");
        {
            auto swings = [this](bool classic, int division)
            {
                auto e = AudioEngine::createEngine(EngineType::UnifiedVoice);
                const int block = 480;          // 10 ms
                e->prepareToPlay(48000.0, block);
                e->setTempo(120.0f);
                int amp = -1;
                for (int i = 0; i < (int) UnifiedVoice::LfoDest::Count; ++i)
                    if (juce::String(UnifiedVoice::lfoDestLabels()[i]) == "AMP") amp = i;
                expect(amp >= 0, "the unified voice's LFO can go to AMP");
                e->setParameter(ModParam::lfoParamId(0, ModParam::LFO_SHAPE), (float) (int) LFO::Shape::Square);
                e->setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEPTH), 1.0f);
                e->setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEST), (float) amp);
                e->setParameter(ModParam::lfoParamId(0, ModParam::LFO_SYNC), 1.0f);
                e->setParameter(ModParam::lfoExtParamId(0, ModParam::LFO_DIV), (float) division);
                if (classic) e->setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f);
                e->noteOn(57, 1.0f);
                juce::AudioBuffer<float> buf(2, block);
                juce::MidiBuffer midi;
                float prev = 0.0f;
                int n = 0;
                for (int b = 0; b < 60; ++b)
                {
                    buf.clear();
                    e->processBlock(buf, midi);
                    const float rms = buf.getRMSLevel(0, 0, block);
                    if (b >= 15 && prev > 1.0e-4f && (rms > prev * 1.5f || rms < prev / 1.5f)) ++n;
                    prev = rms;
                }
                return n;
            };
            const int slow = tempo::indexOf("16BAR"), fast = 16;     // 16BAR (new, index 20) against 1/32
            expectEquals(juce::String(LFO::divisionName(fast)), juce::String("1/32"));
            for (bool classic : { false, true })
            {
                const juce::String path = classic ? "classic path" : "router";
                const int f = swings(classic, fast);
                const int s = swings(classic, LFO::DIVISIONS.find(slow));
                expect(f >= 4, path + ": 1/32 moves the level (" + juce::String(f) + " swings)");
                expectEquals(s, 0, path + ": 16BAR holds it over half a second");
            }
        }
    }
};

static TempoDivisionsTests tempoDivisionsTests;
