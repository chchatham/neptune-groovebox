#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "../src/engine/EngineLfoSection.h"
#include "../src/engine/FMEngine.h"
#include "../src/engine/MacroOscEngine.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/WaveSeqEngine.h"
#include "../src/engine/SamplerEngine.h"
#include "../src/engine/DrumEngine.h"

// The two LFOs of the FM, macro-oscillator, wavetable, wave-sequencing, sampler and drum engines
// (EngineLfoSection), measured on audio rendered through each engine's own processBlock.
//
// Three measuring tools do most of the work:
//   * "the LFO equals the knob": an LFO held at a constant value (a square wave restarted by the
//     note, far too slow to flip) must sound like the destination's own knob moved by that amount;
//   * the LFO's value over time, recovered from a render with the LFO on AMP divided by the same
//     render without it (AMP is a plain gain of 1 + value);
//   * pitch from zero crossings and brightness from the energy of the first difference.
class EngineLfoTests : public juce::UnitTest
{
public:
    EngineLfoTests() : UnitTest("Engine LFOs") {}

    using S = EngineLfoSection;
    static constexpr double SR = 44100.0;
    static constexpr int BLOCK = 441;       // 10 ms: events on a 10 ms grid land exactly

    enum Shape { SIN = 0, TRI, SQR, SAW };
    enum Trig { FREE = 0, KEY, ONE, HALF, HOLD };

    // ------------------------------------------------------------------ rigs
    struct Rig
    {
        juce::String name;
        int note = 69;
        int secondNote = 76;
        bool voiceFilter = false;                               // has the shared per-voice filter
        std::function<std::unique_ptr<EngineBase>(bool bright)> make;   // a steady tone: plain or rich
        std::function<std::vector<double>(EngineBase&)> phases; // LFO 1's phase in every voice (empty: no access)
        int cutoffId = -1, resoId = -1;
    };

    static std::vector<float> periodic(bool saw)
    {
        // 441 Hz at 44.1 kHz: a period of exactly 100 samples, so the loop is seamless.
        std::vector<float> d(44100);
        for (size_t i = 0; i < d.size(); ++i)
        {
            const float p = (float) (i % 100) / 100.0f;
            d[i] = saw ? 0.6f * (2.0f * p - 1.0f) : 0.6f * std::sin(p * juce::MathConstants<float>::twoPi);
        }
        return d;
    }

    template <typename Engine>
    static std::vector<double> voicePhases(EngineBase& e, int count)
    {
        std::vector<double> out;
        for (int v = 0; v < count; ++v)
            out.push_back(static_cast<Engine&>(e).getVoice(v).getLfos().getLfo(0).getPhase());
        return out;
    }

    static std::vector<Rig> rigs()
    {
        std::vector<Rig> r;

        {
            Rig g; g.name = "FM"; g.voiceFilter = true;
            g.cutoffId = FMEngine::FilterBase + VoiceFilterSection::Cutoff;
            g.resoId = FMEngine::FilterBase + VoiceFilterSection::Resonance;
            g.make = [](bool bright)
            {
                auto e = std::make_unique<FMEngine>();
                if (bright)
                {
                    static const float ratios[6] = { 1.0f, 3.0f, 6.0f, 9.0f, 12.0f, 16.0f };
                    e->setParameter(FMEngine::AlgorithmParam, 5.0f);
                    for (int op = 0; op < 6; ++op)
                    {
                        e->setParameter(op * 10 + 0, ratios[op]);
                        e->setParameter(op * 10 + 1, 1.0f);
                        e->setParameter(op * 10 + 6, 1.0f);
                    }
                }
                else
                {
                    e->setParameter(FMEngine::Op2Level, 0.0f);     // one sine
                    e->setParameter(FMEngine::Op1Sustain, 1.0f);
                }
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            g.phases = [](EngineBase& e) { return voicePhases<FMEngine>(e, 8); };
            r.push_back(g);
        }
        {
            Rig g; g.name = "Macro"; g.voiceFilter = true;
            g.cutoffId = MacroOscEngine::FilterBase + VoiceFilterSection::Cutoff;
            g.resoId = MacroOscEngine::FilterBase + VoiceFilterSection::Resonance;
            g.make = [](bool)
            {
                auto e = std::make_unique<MacroOscEngine>();
                e->setParameter(MacroOscEngine::TimbreParam, 0.0f);    // a single saw
                e->setParameter(MacroOscEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            g.phases = [](EngineBase& e) { return voicePhases<MacroOscEngine>(e, 8); };
            r.push_back(g);
        }
        {
            Rig g; g.name = "Wavetable"; g.voiceFilter = true;
            g.cutoffId = WavetableEngine::FilterBase + VoiceFilterSection::Cutoff;
            g.resoId = WavetableEngine::FilterBase + VoiceFilterSection::Resonance;
            g.make = [](bool bright)
            {
                auto e = std::make_unique<WavetableEngine>();
                if (bright)
                    e->setParameter(WavetableEngine::Position, 1.0f);      // the bright end of the saw table
                else
                    e->setParameter(WavetableEngine::TableSelect, 3.0f);   // sine
                e->setParameter(WavetableEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            g.phases = [](EngineBase& e) { return voicePhases<WavetableEngine>(e, 8); };
            r.push_back(g);
        }
        {
            Rig g; g.name = "WaveSeq"; g.voiceFilter = true;
            g.cutoffId = WaveSeqEngine::FilterBase + VoiceFilterSection::Cutoff;
            g.resoId = WaveSeqEngine::FilterBase + VoiceFilterSection::Resonance;
            g.make = [](bool bright)
            {
                auto e = std::make_unique<WaveSeqEngine>();
                // One step at the played pitch: a steady tone.
                e->setParameter(WaveSeqEngine::SeqLength, 1.0f);
                e->setParameter(WaveSeqEngine::TimingLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::PitchLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::ShapeLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::LevelLaneLen, 1.0f);
                e->setParameter(WaveSeqEngine::StepPitch, 0.0f);
                e->setParameter(WaveSeqEngine::StepShape, bright ? 1.0f : 0.0f);
                e->setParameter(WaveSeqEngine::StepLevel, 1.0f);
                e->setParameter(WaveSeqEngine::Crossfade, 0.0f);
                e->setParameter(WaveSeqEngine::AmpSustain, 1.0f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            g.phases = [](EngineBase& e) { return voicePhases<WaveSeqEngine>(e, 8); };
            r.push_back(g);
        }
        {
            Rig g; g.name = "Sampler"; g.note = 60; g.secondNote = 60;
            g.cutoffId = SamplerEngine::FilterCutoff;
            g.resoId = SamplerEngine::FilterReso;
            g.make = [](bool bright)
            {
                auto e = std::make_unique<SamplerEngine>();
                const auto data = periodic(bright);
                e->loadSample(data.data(), (int) data.size(), 1, SR);
                e->setParameter(SamplerEngine::Mode, (float) SamplerEngine::Loop);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            r.push_back(g);
        }
        {
            Rig g; g.name = "Drum"; g.note = 0; g.secondNote = 1;
            g.make = [](bool)
            {
                auto e = std::make_unique<DrumEngine>();
                // Pads 1 and 2 are sustaining sines (the sub-bass voice); tune them into easy reach.
                e->setParameter(0 * 8 + DrumEngine::TUNE, 441.0f);
                e->setParameter(1 * 8 + DrumEngine::TUNE, 661.5f);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            g.phases = [](EngineBase& e) { return voicePhases<DrumEngine>(e, DrumEngine::NUM_VOICES); };
            r.push_back(g);
        }
        return r;
    }

    // ------------------------------------------------------------------ rendering
    struct Ev
    {
        enum Type { On, Off, AllOff };
        double time;
        Type type;
        int note;
    };

    struct Audio
    {
        std::vector<float> l, r;
    };

    static Audio render(EngineBase& e, double seconds, const std::vector<Ev>& events, int blockSize = BLOCK)
    {
        e.prepareToPlay(SR, blockSize);
        Audio out;
        juce::MidiBuffer midi;
        const int total = (int) std::lround(seconds * SR);
        for (int pos = 0; pos < total; pos += blockSize)
        {
            for (auto& ev : events)
            {
                const int at = (int) std::lround(ev.time * SR);
                if (at >= pos && at < pos + blockSize)
                {
                    if (ev.type == Ev::On) e.noteOn(ev.note, 0.8f, -1);
                    else if (ev.type == Ev::Off) e.noteOff(ev.note, -1);
                    else e.allNotesOff();
                }
            }
            juce::AudioBuffer<float> buf(2, blockSize);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < blockSize; ++i)
            {
                out.l.push_back(buf.getSample(0, i));
                out.r.push_back(buf.getSample(1, i));
            }
        }
        return out;
    }

    static void setLfo(EngineBase& e, int lfo, int dest, float depth, int shape, int trig, float rate,
                       bool sync = false, int division = LFO::DEFAULT_DIVISION, float fade = 0.0f)
    {
        e.setParameter(S::id(lfo, S::Rate), rate);
        e.setParameter(S::id(lfo, S::Shape), (float) shape);
        e.setParameter(S::id(lfo, S::Trigger), (float) trig);
        e.setParameter(S::id(lfo, S::Sync), sync ? 1.0f : 0.0f);
        e.setParameter(S::id(lfo, S::Division), (float) division);
        e.setParameter(S::id(lfo, S::Fade), fade);
        e.setParameter(S::id(lfo, S::Dest), (float) dest);
        e.setParameter(S::id(lfo, S::Depth), depth);
    }

    // An LFO that sits at +depth for the whole render: a square wave restarted by the note, with a
    // 50 s cycle.
    static void holdLfoAt(EngineBase& e, int lfo, int dest, float depth)
    {
        setLfo(e, lfo, dest, depth, SQR, KEY, 0.02f);
    }

    static const ParamSpec* destCell(const ParamPages& pages, int lfo)
    {
        for (auto& page : pages)
            if (auto* p = page.find(S::id(lfo, S::Dest)))
                return p;
        return nullptr;
    }

    // Index of a destination by its label, -1 if the engine does not have it.
    static int destIndex(EngineBase& e, const char* label)
    {
        const auto pages = e.getParamPages();
        if (auto* p = destCell(pages, 0))
            for (int i = 0; i < p->numChoices; ++i)
                if (juce::String(p->choices[i]) == label)
                    return i;
        return -1;
    }

    // ------------------------------------------------------------------ measuring
    static float rms(const std::vector<float>& x, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, x.size());
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) x[i] * x[i];
        return to > from ? (float) std::sqrt(sum / (double) (to - from)) : 0.0f;
    }

    // RMS of the difference relative to the RMS of b, both channels.
    static float relDiff(const Audio& a, const Audio& b)
    {
        double diff = 0.0, ref = 0.0;
        const size_t n = std::min(a.l.size(), b.l.size());
        for (size_t i = 0; i < n; ++i)
        {
            diff += (double) (a.l[i] - b.l[i]) * (a.l[i] - b.l[i]) + (double) (a.r[i] - b.r[i]) * (a.r[i] - b.r[i]);
            ref += (double) b.l[i] * b.l[i] + (double) b.r[i] * b.r[i];
        }
        return ref > 0.0 ? (float) std::sqrt(diff / ref) : (diff > 0.0 ? 1.0e9f : 0.0f);
    }

    static float maxAbsDiff(const Audio& a, const Audio& b)
    {
        float m = a.l.size() == b.l.size() ? 0.0f : 1.0e9f;
        for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); ++i)
            m = std::max({ m, std::abs(a.l[i] - b.l[i]), std::abs(a.r[i] - b.r[i]) });
        return m;
    }

    // The LFO's value at a time: the median of on / off - 1 over +-2 ms, where the reference is
    // loud enough to divide by.
    static float lfoValueAt(const std::vector<float>& on, const std::vector<float>& off, double seconds)
    {
        const int centre = (int) std::lround(seconds * SR);
        const int half = (int) (0.002 * SR);
        const int from = std::max(0, centre - half), to = std::min((int) off.size(), centre + half);
        float peak = 0.0f;
        for (int i = from; i < to; ++i)
            peak = std::max(peak, std::abs(off[(size_t) i]));
        std::vector<float> v;
        for (int i = from; i < to; ++i)
            if (peak > 1.0e-4f && std::abs(off[(size_t) i]) > 0.3f * peak)
                v.push_back(on[(size_t) i] / off[(size_t) i] - 1.0f);
        if (v.empty())
            return -99.0f;
        std::nth_element(v.begin(), v.begin() + (long) (v.size() / 2), v.end());
        return v[v.size() / 2];
    }

    // Size, in semitones, of the pitch movement at rateHz (from the times of the upward zero crossings).
    static float pitchWobble(const std::vector<float>& x, double fromSeconds, double toSeconds, double rateHz)
    {
        std::vector<double> times;
        for (int i = std::max(1, (int) (fromSeconds * SR)); i < std::min((int) x.size(), (int) (toSeconds * SR)); ++i)
            if (x[(size_t) i - 1] < 0.0f && x[(size_t) i] >= 0.0f)
                times.push_back(((double) i - 1.0 + (double) (-x[(size_t) i - 1]) / (double) (x[(size_t) i] - x[(size_t) i - 1])) / SR);
        if (times.size() < 8)
            return 0.0f;

        std::vector<double> t, semis;
        double mean = 0.0;
        for (size_t k = 1; k < times.size(); ++k)
        {
            t.push_back(0.5 * (times[k] + times[k - 1]));
            semis.push_back(12.0 * std::log2(1.0 / (times[k] - times[k - 1])));
            mean += semis.back();
        }
        mean /= (double) semis.size();

        // Weighted by the time each period covers, so high and low halves count equally.
        double re = 0.0, im = 0.0, weight = 0.0;
        for (size_t k = 0; k < t.size(); ++k)
        {
            const double w = times[k + 1] - times[k];
            re += w * (semis[k] - mean) * std::cos(juce::MathConstants<double>::twoPi * rateHz * t[k]);
            im += w * (semis[k] - mean) * std::sin(juce::MathConstants<double>::twoPi * rateHz * t[k]);
            weight += w;
        }
        return (float) (2.0 * std::sqrt(re * re + im * im) / weight);
    }

    // Energy of the first difference over the energy: rises with the amount of high frequencies.
    static float brightness(const std::vector<float>& x, double fromSeconds, double toSeconds)
    {
        double diff = 0.0, energy = 0.0;
        for (int i = std::max(1, (int) (fromSeconds * SR)); i < std::min((int) x.size(), (int) (toSeconds * SR)); ++i)
        {
            diff += (double) (x[(size_t) i] - x[(size_t) i - 1]) * (x[(size_t) i] - x[(size_t) i - 1]);
            energy += (double) x[(size_t) i] * x[(size_t) i];
        }
        return energy > 0.0 ? (float) (diff / energy) : 0.0f;
    }

    // "LFO at a constant value == the knob moved": relative difference between a render with the
    // LFO held at depth on dest and one with knob() applied instead; differs is set when the knob
    // itself changes the sound (so the comparison means something).
    using Setup = std::function<void(EngineBase&)>;
    float knobEquivalence(const Rig& rig, bool bright, int dest, float depth, const Setup& base, const Setup& knob,
                          const std::vector<Ev>& events, double seconds, bool* differs = nullptr)
    {
        auto withLfo = rig.make(bright);
        base(*withLfo);
        holdLfoAt(*withLfo, 1, dest, depth);       // LFO 2 here, LFO 1 elsewhere: both get used
        const auto a = render(*withLfo, seconds, events);

        auto withKnob = rig.make(bright);
        base(*withKnob);
        knob(*withKnob);
        const auto b = render(*withKnob, seconds, events);

        // The LFO must leave the stored value of every other parameter alone.
        if (differs != nullptr)
        {
            auto plain = rig.make(bright);
            base(*plain);
            *differs = relDiff(render(*plain, seconds, events), b) > 0.02f;
        }
        return relDiff(a, b);
    }

    // ------------------------------------------------------------------ the tests
    void runTest() override
    {
        const auto all = rigs();

        beginTest("pages: LFO1 and LFO2 are the last two pages, eight working cells, ids 158-173, round trip");
        for (auto& rig : all)
        {
            auto e = rig.make(false);
            const auto pages = e->getParamPages();
            expect(pages.size() >= 3, rig.name + " has pages");
            if (pages.size() < 3)
                continue;
            static const char* const labels[8] = { "RATE", "DPTH", "SHPE", "TRIG", "SYNC", "DIV", "FADE", "DEST" };
            static const float defaults[8] = { 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, (float) LFO::DEFAULT_DIVISION, 0.0f, 0.0f };

            for (int lfo = 0; lfo < 2; ++lfo)
            {
                const auto& page = pages[pages.size() - 2 + (size_t) lfo];
                expectEquals(page.name, juce::String(lfo == 0 ? "LFO1" : "LFO2"), rig.name + " page name");
                expectEquals(page.section, juce::String("MOD"), rig.name + " section");
                for (int c = 0; c < 8; ++c)
                {
                    const auto& p = page.params[(size_t) c];
                    const juce::String where = rig.name + " " + page.name + " " + labels[c];
                    expect(p.isValid(), where + " valid");
                    expectEquals(juce::String(p.label), juce::String(labels[c]), where + " label");
                    expectEquals(p.id, 158 + lfo * 8 + c, where + " id");
                    expect(EngineParamRange::contains(p.id), where + " in the engine id range");
                    expect(p.maxVal > p.minVal, where + " range");
                    expectWithinAbsoluteError(p.defaultVal, defaults[c], 1.0e-6f, where + " default in the description");
                    expectWithinAbsoluteError(e->getParameter(p.id), defaults[c], 1.0e-6f, where + " default value");
                    expect(e->getParameterName(p.id).isNotEmpty(), where + " has a name");

                    if (p.kind == ParamKind::Choice)
                    {
                        expect(p.choices != nullptr && p.numChoices == (int) p.maxVal + 1, where + " choices");
                        for (int k = 0; p.choices != nullptr && k < p.numChoices; ++k)
                        {
                            const juce::String text(p.choices[k]);
                            const int limit = c == 5 ? 5 : 4;      // division names run to five characters
                            expect(text.isNotEmpty() && text.length() <= limit && text == text.toUpperCase(),
                                   where + " choice " + text);
                        }
                    }

                    for (float v : { p.minVal, p.maxVal, p.fromNorm(0.5f), p.defaultVal })
                    {
                        e->setParameter(p.id, v);
                        expectWithinAbsoluteError(e->getParameter(p.id), v, 1.0e-5f, where + " round trip");
                    }
                    // Out-of-range values are held to the range.
                    e->setParameter(p.id, p.maxVal + 100.0f);
                    expectWithinAbsoluteError(e->getParameter(p.id), p.maxVal, 1.0e-5f, where + " upper limit");
                    e->setParameter(p.id, p.minVal - 100.0f);
                    expectWithinAbsoluteError(e->getParameter(p.id), p.minVal, 1.0e-5f, where + " lower limit");
                    e->setParameter(p.id, p.defaultVal);
                }
                expectEquals(page.params[5].numChoices, LFO::NUM_DIVISIONS, rig.name + " every division offered");
                expectEquals(juce::String(page.params[7].choices[0]), juce::String("OFF"), rig.name + " destination 0 is OFF");
                expect(page.params[7].numChoices >= 7 && page.params[7].numChoices <= S::MAX_DESTS, rig.name + " destination count");
            }

            // The LFO ids are on these two pages only, the STRIP page before them has 174-181, and
            // nothing strays into 182-189 or 150-157.
            for (size_t i = 0; i < pages.size(); ++i)
                for (auto& p : pages[i].params)
                {
                    expect(p.isValid(), rig.name + " " + pages[i].name + " has an empty cell");
                    expect(p.id < 150 || (p.id >= 158 && p.id <= 181) || DrumEngine::isKickParam(p.id) || DrumEngine::isCymbalParam(p.id), rig.name + " id outside its ranges: " + juce::String(p.id));
                    expect(S::ownsLfo(p.id) == (i + 2 >= pages.size()), rig.name + " " + pages[i].name + ": LFO ids only on the LFO pages");
                    expect(S::ownsStrip(p.id) == (i + 3 == pages.size()), rig.name + " " + pages[i].name + ": STRIP ids only on the STRIP page");
                }

            // The engine's own parameters are untouched by the new ids.
            auto fresh = rig.make(false);
            for (auto& page : pages)
                for (auto& p : page.params)
                    if (! S::owns(p.id))
                        expectWithinAbsoluteError(e->getParameter(p.id), fresh->getParameter(p.id), 1.0e-6f,
                                                  rig.name + " parameter " + juce::String(p.id) + " disturbed");
        }

        beginTest("macro-oscillator: the DEST cell names the four macros as the model does");
        {
            MacroOscEngine e;
            for (int model = 0; model < 8; ++model)
            {
                e.setParameter(MacroOscEngine::ModelParam, (float) model);
                const auto pages = e.getParamPages();
                auto* dest = destCell(pages, 0);
                expect(dest != nullptr);
                if (dest == nullptr)
                    continue;
                for (int k = 0; k < 4; ++k)
                    expectEquals(juce::String(dest->choices[MacroOscVoice::LfoTimbre + k]),
                                 juce::String(pages[0].params[(size_t) (1 + k)].label), "model " + juce::String(model));
            }
        }

        beginTest("DEST = OFF or DPTH = 0: the output is exactly the output without the section");
        for (auto& rig : all)
        {
            const std::vector<Ev> events = { { 0.0, Ev::On, rig.note }, { 0.1, Ev::On, rig.secondNote }, { 0.25, Ev::Off, rig.note } };
            for (bool bright : { false, true })
            {
                auto plain = rig.make(bright);
                const auto ref = render(*plain, 0.4, events);
                expect(rms(ref.l) > 0.01f, rig.name + " reference sounds");

                // Everything set, both destinations OFF.
                {
                    auto e = rig.make(bright);
                    setLfo(*e, 0, 0, 1.0f, SAW, KEY, 7.0f, true, 12, 0.3f);
                    setLfo(*e, 1, 0, -1.0f, SQR, HOLD, 11.0f);
                    e->setTempo(133.0f);
                    expectEquals(maxAbsDiff(render(*e, 0.4, events), ref), 0.0f, rig.name + " both OFF");
                }
                // Every destination with no depth.
                auto probe = rig.make(bright);
                const int numDests = destCell(probe->getParamPages(), 0)->numChoices;
                // The original destinations and every seventh of the page cells that follow them
                // (which share one mechanism; the "LFO Destinations" tests go through all of them).
                for (int d = 1; d < numDests; d += (d < 12 ? 1 : 7))
                {
                    auto e = rig.make(bright);
                    setLfo(*e, 0, d, 0.0f, SIN, FREE, 5.0f);
                    setLfo(*e, 1, numDests - d, 0.0f, SAW, KEY, 3.0f);
                    expectEquals(maxAbsDiff(render(*e, 0.4, events), ref), 0.0f,
                                 rig.name + " depth 0 on destination " + juce::String(d));
                }
                // Used, then switched off: the next note is the plain sound again.
                {
                    auto e = rig.make(bright);
                    const int numD = numDests;
                    for (int d = 1; d < numD; ++d)
                    {
                        setLfo(*e, 0, d, 0.7f, SIN, FREE, 9.0f);
                        e->noteOn(rig.note, 0.8f, -1);
                        (void) render(*e, 0.05, {});
                        e->allNotesOff();
                    }
                    e->setParameter(S::id(0, S::Dest), 0.0f);
                    auto other = rig.make(bright);
                    for (int d = 1; d < numD; ++d)
                    {
                        other->noteOn(rig.note, 0.8f, -1);
                        (void) render(*other, 0.05, {});
                        other->allNotesOff();
                    }
                    const std::vector<Ev> one = { { 0.0, Ev::On, rig.note } };
                    expect(relDiff(render(*e, 0.2, one), render(*other, 0.2, one)) < 1.0e-4f,
                           rig.name + " plain again after the LFO is switched off");
                }
            }
        }

        beginTest("AMP: the level follows the LFO at its rate (gain 1 + value), FREE runs from the start");
        for (auto& rig : all)
        {
            const std::vector<Ev> events = { { 0.0, Ev::On, rig.note } };
            auto plain = rig.make(false);
            const auto off = render(*plain, 0.6, events);
            auto e = rig.make(false);
            const int amp = destIndex(*e, "AMP");
            expect(amp > 0, rig.name + " has AMP");
            setLfo(*e, 0, amp, 0.5f, SIN, FREE, 5.0f);
            const auto on = render(*e, 0.6, events);
            for (double t = 0.05; t < 0.56; t += 0.03)
                expectWithinAbsoluteError(lfoValueAt(on.l, off.l, t),
                                          0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * 5.0 * t), 0.03f,
                                          rig.name + " tremolo at " + juce::String(t, 2));

            // A negative depth inverts it; a held +0.5 is exactly half as loud again.
            auto inv = rig.make(false);
            setLfo(*inv, 0, amp, -0.5f, SIN, FREE, 5.0f);
            const auto neg = render(*inv, 0.2, events);
            expectWithinAbsoluteError(lfoValueAt(neg.l, off.l, 0.05), -0.5f, 0.03f, rig.name + " negative depth");

            auto held = rig.make(false);
            holdLfoAt(*held, 1, amp, 0.5f);
            const auto loud = render(*held, 0.2, events);
            expectWithinAbsoluteError(rms(loud.l) / rms(std::vector<float>(off.l.begin(), off.l.begin() + (long) loud.l.size())),
                                      1.5f, 0.002f, rig.name + " gain 1.5");

            // Two LFOs on the same destination add up.
            auto both = rig.make(false);
            holdLfoAt(*both, 0, amp, 0.3f);
            holdLfoAt(*both, 1, amp, 0.4f);
            const auto sum = render(*both, 0.2, events);
            expectWithinAbsoluteError(lfoValueAt(sum.l, off.l, 0.1), 0.7f, 0.01f, rig.name + " two LFOs add");
        }

        beginTest("AMP and PAN: a square LFO does not step the level (no click at its edges)");
        for (auto& rig : all)
            for (const char* label : { "AMP", "PAN" })
            {
                // A sine with the LFO's edges every 3675 samples, each at a different point of the wave.
                // The largest step from one sample to the next in the 10 ms around the edges
                // against the largest anywhere else: a stepped gain is a jump of up to the whole wave.
                auto e = rig.make(false);
                const int dest = destIndex(*e, label);
                expect(dest > 0, rig.name + " has " + label);
                setLfo(*e, 0, dest, 0.8f, SQR, KEY, 6.0f);
                const auto on = render(*e, 1.05, { { 0.0, Ev::On, rig.note } });
                for (auto* ch : { &on.l, &on.r })
                {
                    float atEdges = 0.0f, elsewhere = 0.0f, peak = 0.0f;
                    for (size_t i = 2205; i < ch->size(); ++i)
                    {
                        const float step = std::abs((*ch)[i] - (*ch)[i - 1]);
                        const size_t fromEdge = (i + 220) % 3675;
                        float& worst = fromEdge < 441 ? atEdges : elsewhere;
                        worst = std::max(worst, step);
                        peak = std::max(peak, std::abs((*ch)[i]));
                    }
                    expect(peak > 0.01f, rig.name + " " + label + " sounds");
                    expect(atEdges <= elsewhere + 0.1f * peak, rig.name + " " + label + ": " + juce::String(atEdges, 4)
                           + " at the edges, " + juce::String(elsewhere, 4) + " elsewhere, peak " + juce::String(peak, 4));
                }
            }

        beginTest("PTCH: vibrato at the LFO rate, 12 semitones x depth squared");
        for (auto& rig : all)
        {
            const std::vector<Ev> events = { { 0.0, Ev::On, rig.note } };
            auto plain = rig.make(false);
            const auto off = render(*plain, 1.1, events);
            expect(pitchWobble(off.l, 0.1, 1.1, 5.0) < 0.01f, rig.name + " steady without the LFO");
            const int ptch = destIndex(*plain, "PTCH");
            expect(ptch > 0, rig.name + " has PTCH");

            for (float depth : { 0.1f, 0.3f, -0.3f })
            {
                auto e = rig.make(false);
                setLfo(*e, 0, ptch, depth, SIN, FREE, 5.0f);
                const auto on = render(*e, 1.1, events);
                const float expected = 12.0f * depth * depth;
                const float got = pitchWobble(on.l, 0.1, 1.1, 5.0);
                expect(std::abs(got - expected) < 0.12f * expected + 0.01f,
                       rig.name + " depth " + juce::String(depth, 2) + ": " + juce::String(got, 3) + " semitones, expected " + juce::String(expected, 3));
                // ... and at that rate, not another.
                expect(pitchWobble(on.l, 0.1, 1.1, 8.0) < 0.25f * got, rig.name + " wobble is at 5 Hz");
            }
        }

        beginTest("CUT: the filter sweeps with the LFO, +-4 octaves at full depth");
        for (auto& rig : all)
        {
            auto probe = rig.make(true);
            const int cut = destIndex(*probe, "CUT");
            if (rig.name == "Drum")
            {
                expect(cut < 0, "the drums have no CUT destination");
                continue;
            }
            expect(cut > 0, rig.name + " has CUT");
            const float base = rig.voiceFilter ? 800.0f : 0.53f;       // the sampler's cutoff is 0..1 (0.53 = about 780 Hz)
            const std::vector<Ev> events = { { 0.0, Ev::On, rig.note } };

            auto plain = rig.make(true);
            plain->setParameter(rig.cutoffId, base);
            const auto off = render(*plain, 1.0, events);
            auto e = rig.make(true);
            e->setParameter(rig.cutoffId, base);
            setLfo(*e, 0, cut, 0.5f, SIN, FREE, 2.0f);      // +-2 octaves, peak at 0.125 s, trough at 0.375 s
            const auto on = render(*e, 1.0, events);

            const float open = brightness(on.l, 0.10, 0.15), closed = brightness(on.l, 0.35, 0.40);
            const float open2 = brightness(on.l, 0.60, 0.65), closed2 = brightness(on.l, 0.85, 0.90);
            expect(open > 2.5f * closed, rig.name + " brighter at the LFO's peak: " + juce::String(open, 4) + " vs " + juce::String(closed, 4));
            expect(open2 > 2.5f * closed2, rig.name + " and again a cycle later");
            const float still = brightness(off.l, 0.10, 0.15) / brightness(off.l, 0.35, 0.40);
            expect(still > 0.8f && still < 1.25f, rig.name + " steady without the LFO");
            const float set = brightness(off.l, 0.10, 0.15);
            expect(open > 1.4f * set && closed < 0.85f * set,
                   rig.name + " moves both ways around the set cutoff: " + juce::String(closed, 4) + " < " + juce::String(set, 4) + " < " + juce::String(open, 4));
            expectEquals(e->getParameter(rig.cutoffId), base, rig.name + " stored cutoff untouched");
        }

        beginTest("common destinations held at a value equal their knobs: PTCH, CUT, RESO, PAN");
        for (auto& rig : all)
        {
            const std::vector<Ev> events = { { 0.0, Ev::On, rig.note } };
            auto probe = rig.make(true);
            const int ptch = destIndex(*probe, "PTCH"), cut = destIndex(*probe, "CUT");
            const int reso = destIndex(*probe, "RESO"), pan = destIndex(*probe, "PAN");
            const Setup nothing = [](EngineBase&) {};
            bool differs = false;

            // Depth 0.5 on pitch = 3 semitones.
            Setup tuneUp;
            if (rig.name == "FM") tuneUp = [](EngineBase& e) { e.setParameter(FMEngine::TuneParam, 3.0f); };
            else if (rig.name == "Macro") tuneUp = [](EngineBase& e) { e.setParameter(MacroOscEngine::TuneParam, 3.0f); };
            else if (rig.name == "Wavetable") tuneUp = [](EngineBase& e) { e.setParameter(WavetableEngine::TuneParam, 3.0f); };
            else if (rig.name == "WaveSeq") tuneUp = [](EngineBase& e) { e.setParameter(WaveSeqEngine::TuneParam, 3.0f); };
            else if (rig.name == "Sampler") tuneUp = [](EngineBase& e) { e.setParameter(SamplerEngine::PitchSemi, 3.0f); };
            else tuneUp = [](EngineBase& e) { e.setParameter(DrumEngine::TUNE, 441.0f * std::exp2(0.25f)); };
            float d = knobEquivalence(rig, false, ptch, 0.5f, nothing, tuneUp, events, 0.25, &differs);
            expect(differs && d < 0.02f, rig.name + " PTCH +0.5 = 3 semitones up (" + juce::String(d, 5) + ")");
            d = knobEquivalence(rig, false, ptch, -0.5f, nothing,
                                [&](EngineBase& e)
                                {
                                    if (rig.name == "FM") e.setParameter(FMEngine::TuneParam, -3.0f);
                                    else if (rig.name == "Macro") e.setParameter(MacroOscEngine::TuneParam, -3.0f);
                                    else if (rig.name == "Wavetable") e.setParameter(WavetableEngine::TuneParam, -3.0f);
                                    else if (rig.name == "WaveSeq") e.setParameter(WaveSeqEngine::TuneParam, -3.0f);
                                    else if (rig.name == "Sampler") e.setParameter(SamplerEngine::PitchSemi, -3.0f);
                                    else e.setParameter(DrumEngine::TUNE, 441.0f * std::exp2(-0.25f));
                                }, events, 0.25, &differs);
            expect(differs && d < 0.02f, rig.name + " PTCH -0.5 = 3 semitones down (" + juce::String(d, 5) + ")");

            if (cut > 0)
            {
                // Depth 0.5 = 2 octaves up.
                const int cutoffId = rig.cutoffId, resoId = rig.resoId;
                const bool hz = rig.voiceFilter;
                const Setup base = [=](EngineBase& e) { e.setParameter(cutoffId, hz ? 700.0f : 0.5f); e.setParameter(resoId, 0.2f); };
                d = knobEquivalence(rig, true, cut, 0.5f, base,
                                    [=](EngineBase& e) { e.setParameter(cutoffId, hz ? 2800.0f : 0.5f + 2.0f / 9.9657843f); },
                                    events, 0.25, &differs);
                expect(differs && d < 0.01f, rig.name + " CUT +0.5 = 2 octaves up (" + juce::String(d, 5) + ")");
                d = knobEquivalence(rig, true, cut, -0.25f, base,
                                    [=](EngineBase& e) { e.setParameter(cutoffId, hz ? 350.0f : 0.5f - 1.0f / 9.9657843f); },
                                    events, 0.25, &differs);
                expect(differs && d < 0.01f, rig.name + " CUT -0.25 = 1 octave down (" + juce::String(d, 5) + ")");

                expect(reso > 0, rig.name + " has RESO");
                d = knobEquivalence(rig, true, reso, 1.0f, base, [=](EngineBase& e) { e.setParameter(resoId, 0.7f); },
                                    events, 0.25, &differs);
                expect(differs && d < 0.01f, rig.name + " RESO +1 = resonance + 0.5 (" + juce::String(d, 5) + ")");
            }

            expect(pan > 0, rig.name + " has PAN");
            if (rig.name == "Sampler")
            {
                d = knobEquivalence(rig, false, pan, 0.5f, nothing, [](EngineBase& e) { e.setParameter(SamplerEngine::Pan, 0.5f); },
                                    events, 0.2, &differs);
                expect(differs && d < 1.0e-4f, "Sampler PAN = the pan knob");
            }
            else if (rig.name == "Drum")
            {
                d = knobEquivalence(rig, false, pan, 0.6f, nothing,
                                    [](EngineBase& e) { e.setParameter(DrumEngine::PAN, e.getParameter(DrumEngine::PAN) + 0.3f); },
                                    events, 0.2, &differs);
                expect(differs && d < 1.0e-4f, "Drum PAN = the voice's pan knob");
            }
            else
            {
                // Balance: +0.5 halves the left channel and leaves the right.
                auto plain = rig.make(false);
                const auto off = render(*plain, 0.2, events);
                auto e = rig.make(false);
                holdLfoAt(*e, 0, pan, 0.5f);
                const auto on = render(*e, 0.2, events);
                expectWithinAbsoluteError(rms(on.l) / rms(off.l), 0.5f, 0.001f, rig.name + " PAN right: left halved");
                expectWithinAbsoluteError(rms(on.r) / rms(off.r), 1.0f, 0.001f, rig.name + " PAN right: right kept");
                auto e2 = rig.make(false);
                holdLfoAt(*e2, 0, pan, -1.0f);
                const auto hardLeft = render(*e2, 0.2, events);
                expect(rms(hardLeft.r) < 1.0e-6f && std::abs(rms(hardLeft.l) / rms(off.l) - 1.0f) < 0.001f, rig.name + " PAN hard left");
            }
        }

        beginTest("FM destinations: INDX, FDBK, DETN");
        {
            const auto& rig = all[0];
            const std::vector<Ev> events = { { 0.0, Ev::On, 57 } };
            const Setup twoOp = [](EngineBase& e) { e.setParameter(FMEngine::Op2Level, 0.5f); e.setParameter(FMEngine::Op1Sustain, 1.0f); e.setParameter(FMEngine::Op2Sustain, 1.0f); };
            auto probe = rig.make(false);
            const int indx = destIndex(*probe, "INDX"), fdbk = destIndex(*probe, "FDBK"), detn = destIndex(*probe, "DETN");
            expect(indx > 0 && fdbk > 0 && detn > 0);
            bool differs = false;

            // Index x 1.6 = the modulator's level x 1.6 (it only feeds the carrier here).
            float d = knobEquivalence(rig, false, indx, 0.6f, twoOp, [](EngineBase& e) { e.setParameter(FMEngine::Op2Level, 0.8f); },
                                      events, 0.3, &differs);
            expect(differs && d < 1.0e-3f, "INDX +0.6 = modulator level x 1.6 (" + juce::String(d, 5) + ")");
            d = knobEquivalence(rig, false, indx, -1.0f, twoOp, [](EngineBase& e) { e.setParameter(FMEngine::Op2Level, 0.0f); },
                                events, 0.3, &differs);
            expect(differs && d < 1.0e-3f, "INDX -1 = no modulation");

            d = knobEquivalence(rig, false, fdbk, 0.4f, twoOp,
                                [](EngineBase& e) { for (int op = 0; op < 6; ++op) e.setParameter(op * 10 + 3, 0.4f); },
                                events, 0.3, &differs);
            expect(differs && d < 1.0e-3f, "FDBK +0.4 = every operator's feedback at 0.4 (" + juce::String(d, 5) + ")");

            d = knobEquivalence(rig, false, detn, 0.5f, twoOp,
                                [](EngineBase& e) { for (int op = 0; op < 6; ++op) e.setParameter(op * 10 + 2, (op & 1) == 0 ? 25.0f : -25.0f); },
                                events, 0.3, &differs);
            expect(differs && d < 0.01f, "DETN +0.5 = operators 25 cents apart each way (" + juce::String(d, 5) + ")");
        }

        beginTest("macro-oscillator destinations: the four macros");
        {
            const auto& rig = all[1];
            const std::vector<Ev> events = { { 0.0, Ev::On, 57 } };
            static const int knobs[4] = { MacroOscEngine::TimbreParam, MacroOscEngine::ColorParam,
                                          MacroOscEngine::HarmonicsParam, MacroOscEngine::MorphParam };
            for (int k = 0; k < 4; ++k)
            {
                int modelsMoved = 0;
                for (int model = 0; model < 8; ++model)
                {
                    const int knob = knobs[k];
                    const Setup base = [=](EngineBase& e)
                    {
                        e.setParameter(MacroOscEngine::ModelParam, (float) model);
                        for (int id : knobs) e.setParameter(id, 0.3f);
                    };
                    bool differs = false;
                    float d = knobEquivalence(rig, false, MacroOscVoice::LfoTimbre + k, 0.4f, base,
                                              [=](EngineBase& e) { e.setParameter(knob, 0.7f); }, events, 0.25, &differs);
                    expect(d < 1.0e-3f, "macro " + juce::String(k) + " model " + juce::String(model) + " +0.4 (" + juce::String(d, 5) + ")");
                    d = knobEquivalence(rig, false, MacroOscVoice::LfoTimbre + k, -0.2f, base,
                                        [=](EngineBase& e) { e.setParameter(knob, 0.1f); }, events, 0.25);
                    expect(d < 1.0e-3f, "macro " + juce::String(k) + " model " + juce::String(model) + " -0.2 (" + juce::String(d, 5) + ")");
                    modelsMoved += differs ? 1 : 0;
                }
                expect(modelsMoved >= 2, "macro " + juce::String(k) + " changes the sound in " + juce::String(modelsMoved) + " models");
            }
        }

        beginTest("wavetable destinations: POS, SPRD, SUB");
        {
            const auto& rig = all[2];
            const std::vector<Ev> events = { { 0.0, Ev::On, 57 } };
            struct Case { const char* label; int knob; float base, depth, moved; };
            for (auto c : { Case { "POS", WavetableEngine::Position, 0.2f, 0.5f, 0.7f },
                            Case { "POS", WavetableEngine::Position, 0.6f, -0.4f, 0.2f },
                            Case { "SPRD", WavetableEngine::UnisonSpread, 0.2f, 0.3f, 0.5f },
                            Case { "SUB", WavetableEngine::SubLevel, 0.2f, 0.5f, 0.7f } })
            {
                auto probe = rig.make(true);
                const int dest = destIndex(*probe, c.label);
                expect(dest > 0, juce::String(c.label) + " exists");
                bool differs = false;
                const float d = knobEquivalence(rig, true, dest, c.depth,
                                                [=](EngineBase& e) { e.setParameter(WavetableEngine::Position, 0.2f); e.setParameter(c.knob, c.base); },
                                                [=](EngineBase& e) { e.setParameter(c.knob, c.moved); }, events, 0.25, &differs);
                expect(differs && d < 1.0e-3f, juce::String(c.label) + " " + juce::String(c.depth, 1) + " = the knob moved (" + juce::String(d, 5) + ")");
            }

            // A moving position sweeps the table: brightness rises and falls with a triangle LFO.
            auto e = rig.make(true);
            e->setParameter(WavetableEngine::Position, 0.5f);
            setLfo(*e, 0, destIndex(*e, "POS"), 0.5f, TRI, KEY, 0.5f);      // -0.5 at 0 s, +0.5 at 1 s: position = time
            const auto sweep = render(*e, 1.0, events);
            const float dull = brightness(sweep.l, 0.005, 0.03), mid = brightness(sweep.l, 0.2, 0.25), top = brightness(sweep.l, 0.95, 1.0);
            expect(mid > 2.0f * dull && top >= mid && top > 6.0f * dull, "POS sweeps from the dull end to the bright end: " + juce::String(dull, 5)
                                                           + ", " + juce::String(mid, 5) + ", " + juce::String(top, 5));
            expectEquals(e->getParameter(WavetableEngine::Position), 0.5f, "stored position untouched");
        }

        beginTest("wave-sequencing destinations: RATE, XFAD, PRNG");
        {
            Rig rig = all[3];
            rig.make = [](bool) { return std::unique_ptr<EngineBase>(std::make_unique<WaveSeqEngine>()); };   // the default 8-step sequence
            const std::vector<Ev> events = { { 0.0, Ev::On, 57 } };
            auto probe = rig.make(false);
            bool differs = false;
            float d = knobEquivalence(rig, false, destIndex(*probe, "RATE"), 0.5f, [](EngineBase&) {},
                                      [](EngineBase& e) { e.setParameter(WaveSeqEngine::Rate, 2.0f); }, events, 0.8, &differs);
            expect(differs && d < 1.0e-3f, "RATE +0.5 = twice as fast (" + juce::String(d, 5) + ")");
            d = knobEquivalence(rig, false, destIndex(*probe, "RATE"), -0.5f, [](EngineBase&) {},
                                [](EngineBase& e) { e.setParameter(WaveSeqEngine::Rate, 0.5f); }, events, 0.8, &differs);
            expect(differs && d < 1.0e-3f, "RATE -0.5 = half as fast (" + juce::String(d, 5) + ")");
            d = knobEquivalence(rig, false, destIndex(*probe, "XFAD"), 0.4f, [](EngineBase&) {},
                                [](EngineBase& e) { e.setParameter(WaveSeqEngine::Crossfade, 0.7f); }, events, 0.8, &differs);
            expect(differs && d < 1.0e-3f, "XFAD +0.4 = crossfade 0.7 (" + juce::String(d, 5) + ")");
            d = knobEquivalence(rig, false, destIndex(*probe, "PRNG"), 0.5f, [](EngineBase&) {},
                                [](EngineBase& e) { e.setParameter(WaveSeqEngine::PitchRange, 1.5f); }, events, 0.8, &differs);
            expect(differs && d < 1.0e-3f, "PRNG +0.5 = pitch range 1.5 (" + juce::String(d, 5) + ")");
            d = knobEquivalence(rig, false, destIndex(*probe, "PRNG"), -1.0f, [](EngineBase&) {},
                                [](EngineBase& e) { e.setParameter(WaveSeqEngine::PitchRange, 0.0f); }, events, 0.8, &differs);
            expect(differs && d < 1.0e-3f, "PRNG -1 = a flat pitch lane");
        }

        beginTest("sampler destinations: STRT (read at the note), DRIV");
        {
            Rig rig = all[4];
            // A sample whose content depends on where it is read: a rising sweep.
            rig.make = [](bool)
            {
                auto e = std::make_unique<SamplerEngine>();
                std::vector<float> data(22050);
                double phase = 0.0;
                for (size_t i = 0; i < data.size(); ++i)
                {
                    phase += (100.0 + 3000.0 * (double) i / (double) data.size()) / SR;
                    data[i] = 0.6f * (float) std::sin(juce::MathConstants<double>::twoPi * phase);
                }
                e->loadSample(data.data(), (int) data.size(), 1, SR);
                return std::unique_ptr<EngineBase>(std::move(e));
            };
            const std::vector<Ev> events = { { 0.0, Ev::On, 60 } };
            auto probe = rig.make(false);
            bool differs = false;
            float d = knobEquivalence(rig, false, destIndex(*probe, "STRT"), 0.3f,
                                      [](EngineBase& e) { e.setParameter(SamplerEngine::StartPoint, 0.1f); },
                                      [](EngineBase& e) { e.setParameter(SamplerEngine::StartPoint, 0.4f); }, events, 0.2, &differs);
            expect(differs && d < 1.0e-4f, "STRT +0.3 = start point 0.4 (" + juce::String(d, 5) + ")");
            d = knobEquivalence(rig, false, destIndex(*probe, "DRIV"), 0.3f,
                                [](EngineBase& e) { e.setParameter(SamplerEngine::DriveAmt, 0.2f); },
                                [](EngineBase& e) { e.setParameter(SamplerEngine::DriveAmt, 0.5f); }, events, 0.2, &differs);
            expect(differs && d < 1.0e-4f, "DRIV +0.3 = drive 0.5 (" + juce::String(d, 5) + ")");

            // A free LFO on the start point: notes struck at different moments start in different places.
            auto e = rig.make(false);
            e->setParameter(SamplerEngine::StartPoint, 0.5f);
            setLfo(*e, 0, destIndex(*e, "STRT"), 0.4f, SIN, FREE, 1.0f);
            const auto two = render(*e, 1.0, { { 0.25, Ev::On, 60 }, { 0.45, Ev::AllOff, 0 }, { 0.75, Ev::On, 60 } });
            const float early = brightness(two.l, 0.26, 0.30), late = brightness(two.l, 0.76, 0.80);
            expect(early > 3.0f * late, "STRT: a note at the LFO's peak starts later in the sweep than one at its trough ("
                                        + juce::String(early, 4) + " vs " + juce::String(late, 4) + ")");
        }

        beginTest("drum destinations: DEC, TONE, DRIV, SNAP, PTCH on the kit's voices");
        {
            const auto& rig = all[5];
            auto probe = rig.make(false);
            struct Case { const char* label; int param; float depth; };
            for (auto c : { Case { "DEC", DrumEngine::DECAY, 0.5f }, Case { "DEC", DrumEngine::DECAY, -0.5f },
                            Case { "TONE", DrumEngine::TONE, 0.3f }, Case { "TONE", DrumEngine::TONE, -0.3f },
                            Case { "DRIV", DrumEngine::DRIVE, 0.3f }, Case { "SNAP", DrumEngine::SNAP, 0.3f },
                            Case { "PTCH", DrumEngine::TUNE, 0.5f } })
            {
                const int dest = destIndex(*probe, c.label);
                expect(dest > 0, juce::String(c.label) + " exists");
                int voicesMoved = 0;
                for (int voice : { 8, 10, 11, 12, 13, 14 })      // FM kick, mod kick, tom, snare, clap, closed hat
                {
                    const int id = voice * 8 + c.param;
                    const float base = probe->getParameter(id);
                    float moved = std::clamp(base + c.depth, 0.0f, 1.0f);
                    if (c.param == DrumEngine::DECAY) moved = base * std::exp2(2.0f * c.depth);
                    if (c.param == DrumEngine::TUNE) moved = base * std::exp2(12.0f * c.depth * c.depth / 12.0f);
                    bool differs = false;
                    const float d = knobEquivalence(rig, false, dest, c.depth, [](EngineBase&) {},
                                                    [=](EngineBase& e) { e.setParameter(id, moved); },
                                                    { { 0.0, Ev::On, voice } }, 0.3, &differs);
                    expect(d < 0.02f, juce::String(c.label) + " " + juce::String(c.depth, 1) + " on voice " + juce::String(voice + 1)
                                      + " = the knob moved (" + juce::String(d, 5) + ")");
                    voicesMoved += differs ? 1 : 0;
                }
                expect(voicesMoved >= 2, juce::String(c.label) + " changes " + juce::String(voicesMoved) + " of the six voices");
            }

            // KEY: a falling LFO restarted by every hit bends each hit the same way (a pitch drop).
            {
                auto e = rig.make(false);
                setLfo(*e, 0, destIndex(*e, "PTCH"), 1.0f, (int) LFO::Shape::ExpDown, KEY, 2.0f);     // +12 semitones falling away over the hit (a 0.5 s cycle)
                const auto hits = render(*e, 1.0, { { 0.0, Ev::On, 0 }, { 0.4, Ev::AllOff, 0 }, { 0.5, Ev::On, 0 } });
                auto plain = rig.make(false);
                const auto ref = render(*plain, 1.0, { { 0.0, Ev::On, 0 }, { 0.4, Ev::AllOff, 0 }, { 0.5, Ev::On, 0 } });
                auto crossings = [](const std::vector<float>& x, double from, double to)
                {
                    int n = 0;
                    for (int i = (int) (from * SR); i < (int) (to * SR); ++i)
                        n += (x[(size_t) i - 1] < 0.0f && x[(size_t) i] >= 0.0f) ? 1 : 0;
                    return n;
                };
                for (double start : { 0.0, 0.5 })
                {
                    const int head = crossings(hits.l, start + 0.005, start + 0.035);
                    const int tail = crossings(hits.l, start + 0.30, start + 0.33);
                    const int plainHead = crossings(ref.l, start + 0.005, start + 0.035);
                    expect(head > plainHead * 3 / 2, "hit at " + juce::String(start, 1) + " starts high (" + juce::String(head) + " vs " + juce::String(plainHead) + ")");
                    expect(std::abs(tail - plainHead) <= 1, "hit at " + juce::String(start, 1) + " lands on the set pitch");
                }
                // Both hits are the same sound.
                Audio first, second;
                first.l.assign(hits.l.begin(), hits.l.begin() + 13230);  first.r.assign(hits.r.begin(), hits.r.begin() + 13230);
                second.l.assign(hits.l.begin() + 22050, hits.l.begin() + 22050 + 13230);
                second.r.assign(hits.r.begin() + 22050, hits.r.begin() + 22050 + 13230);
                expect(relDiff(first, second) < 1.0e-3f, "KEY: every hit gets the same sweep");
            }

            // HOLD: each hit takes the LFO's value at that moment; here one hit rings long, the next is cut short.
            {
                auto e = rig.make(false);
                setLfo(*e, 0, destIndex(*e, "DEC"), 1.0f, SIN, HOLD, 1.0f);
                const int kick = 9;     // analog kick, decay 0.6 s
                const auto hits = render(*e, 1.4, { { 0.25, Ev::On, kick }, { 0.75, Ev::On, kick } });
                const float longHit = rms(hits.l, (size_t) (0.45 * SR), (size_t) (0.55 * SR));     // 0.2 s into a hit with decay x 4
                const float shortHit = rms(hits.l, (size_t) (0.95 * SR), (size_t) (1.05 * SR));    // 0.2 s into a hit with decay / 4
                expect(longHit > 0.1f && shortHit < 0.01f * longHit,
                       "HOLD on DEC: " + juce::String(longHit, 4) + " then " + juce::String(shortHit, 6));
                expectWithinAbsoluteError(e->getParameter(kick * 8 + DrumEngine::DECAY), 0.6f, 1.0e-6f, "stored decay untouched");
            }
        }

        beginTest("SYNC + DIV: the cycle lasts the division at the tempo, and follows a tempo change");
        for (auto& rig : all)
        {
            const std::vector<Ev> events = { { 0.0, Ev::On, rig.note } };
            auto plain = rig.make(false);
            const auto off = render(*plain, 0.8, events);
            struct Case { float bpm; int division; double cycle; };
            for (auto c : { Case { 120.0f, 8, 0.5 }, Case { 90.0f, 11, 1.0 / 3.0 }, Case { 150.0f, 14, 0.1 } })
            {
                auto e = rig.make(false);
                setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, SAW, KEY, 13.0f, true, c.division);
                e->setTempo(60.0f);
                e->setTempo(c.bpm);          // the last tempo counts
                const auto on = render(*e, 0.8, events);
                const juce::String where = rig.name + " " + juce::String(c.bpm, 0) + " bpm " + LFO::divisionName(c.division);
                // A rising ramp: -0.5 at the start, 0 half-way, just under +0.5 before the wrap, -0.5 after it.
                expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.5 * c.cycle), 0.0f, 0.03f, where + " middle");
                expectWithinAbsoluteError(lfoValueAt(on.l, off.l, c.cycle - 0.006), 0.5f - 0.006f / (float) c.cycle, 0.03f, where + " end");
                expectWithinAbsoluteError(lfoValueAt(on.l, off.l, c.cycle + 0.006), -0.5f + 0.006f / (float) c.cycle, 0.03f, where + " wrapped");
                expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 1.5 * c.cycle), 0.0f, 0.03f, where + " second cycle");
                // The Hz value is kept, and comes back when sync goes off.
                expectEquals(e->getParameter(S::id(0, S::Rate)), 13.0f, where + " rate kept");
            }
            auto e = rig.make(false);
            setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, SAW, KEY, 4.0f, true, 8);
            e->setTempo(140.0f);
            e->setParameter(S::id(0, S::Sync), 0.0f);
            const auto on = render(*e, 0.4, events);
            expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.125), 0.0f, 0.03f, rig.name + " sync off: 4 Hz again");
            expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.256), -0.476f, 0.03f, rig.name + " sync off: wraps at 0.25 s");
        }

        beginTest("TRIG: KEY restarts with the note, FREE does not; ONE and HALF stop; HOLD freezes per note");
        for (auto& rig : all)
        {
            const std::vector<Ev> twice = { { 0.0, Ev::On, rig.note }, { 0.3, Ev::AllOff, 0 }, { 0.5, Ev::On, rig.note } };
            auto plain = rig.make(false);
            const auto off = render(*plain, 0.8, twice);
            auto make = [&](int shape, int trig, float rate)
            {
                auto e = rig.make(false);
                setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, shape, trig, rate);
                return render(*e, 0.8, twice);
            };

            // A 1 Hz rising ramp (value = t - 0.5 in cycles).
            const auto key = make(SAW, KEY, 1.0f);
            expectWithinAbsoluteError(lfoValueAt(key.l, off.l, 0.1), -0.4f, 0.02f, rig.name + " KEY first note");
            expectWithinAbsoluteError(lfoValueAt(key.l, off.l, 0.6), -0.4f, 0.02f, rig.name + " KEY restarted by the second note");
            expectWithinAbsoluteError(lfoValueAt(key.l, off.l, 0.75), -0.25f, 0.02f, rig.name + " KEY runs on");

            const auto free = make(SAW, FREE, 1.0f);
            expectWithinAbsoluteError(lfoValueAt(free.l, off.l, 0.1), -0.4f, 0.02f, rig.name + " FREE first note");
            expectWithinAbsoluteError(lfoValueAt(free.l, off.l, 0.6), 0.1f, 0.02f, rig.name + " FREE kept running through the gap");
            expectWithinAbsoluteError(lfoValueAt(free.l, off.l, 0.75), 0.25f, 0.02f, rig.name + " FREE runs on");

            // One shot of a 5 Hz ramp: 0.2 s, then it stays at the top; the second note plays it again.
            const auto one = make(SAW, ONE, 5.0f);
            expectWithinAbsoluteError(lfoValueAt(one.l, off.l, 0.1), 0.0f, 0.03f, rig.name + " ONE half-way");
            expectWithinAbsoluteError(lfoValueAt(one.l, off.l, 0.22), 0.5f, 0.02f, rig.name + " ONE stopped at the end");
            expectWithinAbsoluteError(lfoValueAt(one.l, off.l, 0.29), 0.5f, 0.02f, rig.name + " ONE stays there");
            expectWithinAbsoluteError(lfoValueAt(one.l, off.l, 0.6), 0.0f, 0.03f, rig.name + " ONE again on the next note");
            expectWithinAbsoluteError(lfoValueAt(one.l, off.l, 0.78), 0.5f, 0.02f, rig.name + " ONE stopped again");

            const auto half = make(SAW, HALF, 5.0f);
            expectWithinAbsoluteError(lfoValueAt(half.l, off.l, 0.05), -0.25f, 0.03f, rig.name + " HALF a quarter in");
            expectWithinAbsoluteError(lfoValueAt(half.l, off.l, 0.15), 0.0f, 0.02f, rig.name + " HALF stopped half-way");
            expectWithinAbsoluteError(lfoValueAt(half.l, off.l, 0.29), 0.0f, 0.02f, rig.name + " HALF stays there");

            // HOLD on a 1 Hz sine that runs in the background: notes at 0.1 s and 0.6 s.
            const std::vector<Ev> held = { { 0.1, Ev::On, rig.note }, { 0.3, Ev::AllOff, 0 }, { 0.6, Ev::On, rig.note } };
            auto plainHeld = rig.make(false);
            const auto offHeld = render(*plainHeld, 0.8, held);
            auto e = rig.make(false);
            setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, SIN, HOLD, 1.0f);
            const auto hold = render(*e, 0.8, held);
            const float first = 0.5f * std::sin(juce::MathConstants<float>::twoPi * 0.1f);
            const float second = 0.5f * std::sin(juce::MathConstants<float>::twoPi * 0.6f);
            for (double t : { 0.12, 0.2, 0.28 })
                expectWithinAbsoluteError(lfoValueAt(hold.l, offHeld.l, t), first, 0.01f, rig.name + " HOLD constant in note 1");
            for (double t : { 0.62, 0.7, 0.78 })
                expectWithinAbsoluteError(lfoValueAt(hold.l, offHeld.l, t), second, 0.01f, rig.name + " HOLD a new value in note 2");
            expect(std::abs(first - second) > 0.5f);
        }

        beginTest("FADE: the depth rises linearly after each note-on");
        for (auto& rig : all)
        {
            const std::vector<Ev> twice = { { 0.0, Ev::On, rig.note }, { 0.5, Ev::AllOff, 0 }, { 0.6, Ev::On, rig.note } };
            auto plain = rig.make(false);
            const auto off = render(*plain, 0.9, twice);
            auto e = rig.make(false);
            setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, SQR, KEY, 0.02f, false, LFO::DEFAULT_DIVISION, 0.4f);
            const auto on = render(*e, 0.9, twice);
            expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.04), 0.05f, 0.01f, rig.name + " 10 % in");
            expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.2), 0.25f, 0.01f, rig.name + " half-way");
            expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.45), 0.5f, 0.01f, rig.name + " full depth");
            expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.7), 0.125f, 0.01f, rig.name + " fades in again on the next note");
        }

        beginTest("per voice: simultaneous notes have their own KEY phases; FREE is one LFO for all voices");
        for (auto& rig : all)
        {
            if (! rig.phases)
            {
                // The sampler's voices are not reachable: two voices play a constant, so the output
                // is the sum of their gains. Ramps restarted at 0 s and 0.3 s give -0.05 and -0.35
                // at 0.45 s: a mean of -0.2 (one shared LFO would give -0.35).
                auto dc = []
                {
                    auto e = std::make_unique<SamplerEngine>();
                    const std::vector<float> ones(44100, 0.5f);
                    e->loadSample(ones.data(), (int) ones.size(), 1, SR);
                    e->setParameter(SamplerEngine::Mode, (float) SamplerEngine::Loop);
                    return e;
                };
                const std::vector<Ev> two = { { 0.0, Ev::On, 60 }, { 0.3, Ev::On, 60 } };
                auto plain = dc();
                const auto off = render(*plain, 0.6, two);
                auto e = dc();
                setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, SAW, KEY, 1.0f);
                const auto on = render(*e, 0.6, two);
                expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.2), -0.3f, 0.01f, "Sampler one voice");
                expectWithinAbsoluteError(lfoValueAt(on.l, off.l, 0.45), -0.2f, 0.01f, "Sampler two voices, two phases");

                auto f = dc();
                setLfo(*f, 0, destIndex(*f, "AMP"), 0.5f, SAW, FREE, 1.0f);
                const auto freeRun = render(*f, 0.6, two);
                expectWithinAbsoluteError(lfoValueAt(freeRun.l, off.l, 0.45), -0.05f, 0.01f, "Sampler FREE: both voices on one phase");
                continue;
            }

            const std::vector<Ev> two = { { 0.0, Ev::On, rig.note }, { 0.3, Ev::On, rig.secondNote } };
            {
                auto e = rig.make(false);
                setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, SAW, KEY, 1.0f);
                (void) render(*e, 0.5, two);
                auto phases = rig.phases(*e);
                int atHalf = 0, atFifth = 0;
                for (double p : phases)
                {
                    atHalf += std::abs(p - 0.5) < 0.001 ? 1 : 0;
                    atFifth += std::abs(p - 0.2) < 0.001 ? 1 : 0;
                }
                // (Voices that have not been played run on from the start, like the first note's.)
                expectEquals(atFifth, 1, rig.name + " only the second note's LFO is 0.2 s into its cycle");
                expectEquals(atHalf, (int) phases.size() - 1, rig.name + " the first note's LFO is 0.5 s into its cycle");
            }
            for (int shape : { (int) SAW, (int) LFO::Shape::Random })
            {
                // Notes coming and going, in odd-sized blocks: every voice's LFO stays on the same phase.
                auto e = rig.make(false);
                setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, shape, FREE, 31.0f);
                setLfo(*e, 1, destIndex(*e, "PTCH"), 0.2f, SIN, HOLD, 0.7f);
                (void) render(*e, 1.0, { { 0.0, Ev::On, rig.note }, { 0.13, Ev::On, rig.secondNote }, { 0.21, Ev::Off, rig.note },
                                         { 0.4, Ev::AllOff, 0 }, { 0.55, Ev::On, rig.secondNote }, { 0.7, Ev::On, rig.note } }, 1237);
                const auto phases = rig.phases(*e);
                double lo = 1.0, hi = 0.0;
                for (double p : phases) { lo = std::min(lo, p); hi = std::max(hi, p); }
                expect(hi - lo < 1.0e-6, rig.name + " FREE phases agree across " + juce::String((int) phases.size()) + " voices (spread "
                                         + juce::String(hi - lo, 9) + ")");
                // ... and it is where an LFO that never stopped would be: 31 cycles in one second.
                const double expected = 31.0 * (double) (((int) (SR / 1237) + 1) * 1237) / SR;
                const double cyc = expected - std::floor(expected);
                expect(std::abs(phases[0] - cyc) < 1.0e-3 || std::abs(std::abs(phases[0] - cyc) - 1.0) < 1.0e-3,
                       rig.name + " FREE phase " + juce::String(phases[0], 4) + ", expected " + juce::String(cyc, 4));
            }
        }

        beginTest("changing the trigger mode puts every voice's LFO back in step");
        for (auto& rig : all)
        {
            if (! rig.phases)
                continue;
            auto e = rig.make(false);
            setLfo(*e, 0, destIndex(*e, "AMP"), 0.5f, SAW, KEY, 1.0f);
            (void) render(*e, 0.5, { { 0.0, Ev::On, rig.note }, { 0.3, Ev::On, rig.secondNote } });
            e->setParameter(S::id(0, S::Trigger), (float) FREE);
            (void) render(*e, 0.2, {});
            const auto phases = rig.phases(*e);
            double lo = 1.0, hi = 0.0;
            for (double p : phases) { lo = std::min(lo, p); hi = std::max(hi, p); }
            expect(hi - lo < 1.0e-6, rig.name + " in step after KEY -> FREE");
        }
    }
};
static EngineLfoTests EngineLfoTestsInstance;
