#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <set>
#include <vector>
#include "../src/dsp/Saturation.h"
#include "../src/dsp/effects/Waveshaper.h"
#include "../src/dsp/effects/AmpSim.h"
#include "../src/dsp/effects/PerfEffects.h"
#include "../src/engine/AudioEngine.h"
#include "../src/ui/screens/TrackFxScreen.h"
#include "../src/ui/screens/PerfFxScreen.h"
#include "../src/ui/ScreenManager.h"
#include "../src/ui/GrammarHooks.h"
#include "../src/project/ProjectBridge.h"
#include "fast_measure.h"

// The saturation curves (src/dsp/Saturation.h) and the three effects that offer them: the waveshaper's
// CURV, the amp's SAT and the dub echo's SAT; and the cells beyond an effect's four (TRK FX page FX2,
// PERF's MORE page), from the knob to the project file.
class SaturationTests : public juce::UnitTest
{
public:
    SaturationTests() : UnitTest("Saturation") {}

    using Signal = std::vector<float>;
    static constexpr double SR = 48000.0;

    static float rms(const Signal& s, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to && i < s.size(); ++i) sum += (double) s[i] * (double) s[i];
        return to > from ? (float) std::sqrt(sum / (double) (to - from)) : 0.0f;
    }
    static float peak(const Signal& s)
    {
        float p = 0.0f;
        for (float v : s) p = std::max(p, std::abs(v));
        return p;
    }
    static bool finite(const Signal& s)
    {
        for (float v : s) if (! std::isfinite(v)) return false;
        return true;
    }
    static float mean(const Signal& s, size_t from, size_t to)
    {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) sum += s[i];
        return (float) (sum / (double) (to - from));
    }

    // A 100 Hz sine of level `amp` through `shaper`: a whole number of cycles, so harmonic h is exact.
    template <typename Shaper>
    static Signal shaped(Shaper&& shaper, float amp)
    {
        Signal s(4800);
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = shaper(amp * (float) std::sin(6.283185307179586 * 100.0 * (double) i / SR));
        return s;
    }
    static double harmonic(const Signal& s, int h) { return fastmeasure::harmonic(s, 100.0, h, SR); }

    // An effect fed `in` on both channels; the left comes back.
    static Signal through(EffectBase& fx, const Signal& in)
    {
        Signal out(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            float l = in[i], r = in[i];
            fx.process(l, r);
            out[i] = l;
        }
        return out;
    }
    static Signal sine(double seconds, double hz, float amp)
    {
        Signal s((size_t) (seconds * SR));
        for (size_t i = 0; i < s.size(); ++i) s[i] = amp * (float) std::sin(6.283185307179586 * hz * (double) i / SR);
        return s;
    }
    Signal noise(double seconds, float amp)
    {
        Signal s((size_t) (seconds * SR));
        for (auto& v : s) v = (random.nextFloat() * 2.0f - 1.0f) * amp;
        return s;
    }
    static Signal silence(double seconds) { return Signal((size_t) (seconds * SR), 0.0f); }

    struct Plain : Screen
    {
        juce::String getName() const override { return "OTHER"; }
        void render(DisplayRenderer&) override {}
    };
    // The instrument with TRK FX and PERF behind ScreenManager, as the controller reaches them.
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        ScreenManager mgr;
        TrackFxScreen* fx = nullptr;
        PerfFxScreen* perf = nullptr;
        juce::int64 now = 1000;

        Rig()
        {
            seq.prepareToPlay(SR, 64);
            audio.setSequencer(&seq);
            for (int t = 0; t < seq.getPattern().getTrackCount(); ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(SR, 64);
            mgr.setSequencer(&seq);
            mgr.setClock([this] { return now; });
            mgr.setHelpBusyWindow(0);
            mgr.setEngineHooks(GrammarHooks::make(seq, audio));
            auto f = std::make_unique<TrackFxScreen>(seq, &audio);
            fx = f.get();
            mgr.addScreen(std::move(f));
            auto p = std::make_unique<PerfFxScreen>(audio, [this] { return seq.getSelectedTrack(); },
                                                    [this] { return seq.getPattern().getTrackCount(); });
            perf = p.get();
            mgr.addScreen(std::move(p));
            mgr.projectLoaded();
        }

        void turn(int knob, int clicks) { mgr.controlChangeReceived(16, 0x15 + knob, clicks < 0 ? 128 + clicks : clicks); }
        static int extraId(int slot, int index = 0) { return AudioEngine::INSERT_EXTRA_BASE + slot * EffectBase::MAX_EXTRAS + index; }
    };

    juce::Random random { 11 };

    void runTest() override
    {
        using namespace sat;

        beginTest("Every curve: 0 for 0, finite and held for any input, a name of its own; the first four keep their numbers");
        {
            std::set<juce::String> seen;
            for (int c = 0; c < Count; ++c)
            {
                const juce::String name = names()[c];
                expect(name.isNotEmpty() && name.length() <= 4 && seen.insert(name).second, "name of curve " + juce::String(c));
                expectEquals(shape(c, 0.0f), 0.0f, name + ": 0 for 0");
                const bool open = c == Soft || c == Asym;       // the two without a ceiling
                float worst = 0.0f;
                bool ok = true;
                for (float x = -60.0f; x <= 60.0f; x += 0.013f)
                {
                    const float y = shape(c, x);
                    ok = ok && std::isfinite(y);
                    worst = std::max(worst, std::abs(y));
                }
                for (float x : { 1.0e4f, -1.0e4f, 1.0e30f, -1.0e30f, 3.0e38f, -3.0e38f })
                    ok = ok && std::isfinite(shape(c, x)) && (open || std::abs(shape(c, x)) <= 2.0f);
                expect(ok, name + ": finite everywhere");
                expect(worst <= (open ? 1.5f * 60.0f / 9.0f + 1.0f : 2.0f), name + ": held, " + juce::String(worst));
            }
            expectEquals(juce::String(names()[0]), juce::String("SOFT"));
            expectEquals(juce::String(names()[1]), juce::String("HARD"));
            expectEquals(juce::String(names()[2]), juce::String("FOLD"));
            expectEquals(juce::String(names()[3]), juce::String("ASYM"));
            expectEquals((int) Tanh, 4);
            expectEquals((int) Fast, 5);
            expectEquals(valid(-3), 0);
            expectEquals(valid(999), (int) Count - 1);
        }

        beginTest("What each curve does to a sine: the harmonics, quiet, at the knee and driven; even harmonics only from the lopsided ones");
        {
            // Curves that treat both halves of the wave alike can make no even harmonic.
            const std::set<int> lopsided { Asym, Tube, Rectify, HalfWave, Even };
            logMessage("curve  level  fund    h2      h3      h4      h5     (level of each harmonic; input sine at 0.3, 1 and 4)");
            for (int c = 0; c < Count; ++c)
            {
                for (float amp : { 0.3f, 1.0f, 4.0f })
                {
                    const auto s = shaped([c](float x) { return shape(c, x); }, amp);
                    juce::String line = juce::String(names()[c]).paddedRight(' ', 6) + " " + juce::String(amp, 1).paddedRight(' ', 5);
                    for (int h = 1; h <= 5; ++h)
                        line += "  " + juce::String(harmonic(s, h), 4);
                    logMessage(line);
                    const double even = harmonic(s, 2) + harmonic(s, 4);
                    if (lopsided.count(c) == 0)
                        expect(even < 1.0e-4, juce::String(names()[c]) + " is even-handed: " + juce::String(even));
                    else if (amp >= 1.0f)
                        expect(even > 0.02, juce::String(names()[c]) + " makes even harmonics: " + juce::String(even));
                }
                // Quiet, a curve passes the sound as it is (its slope at 0), except the three that say otherwise.
                const auto quiet = shaped([c](float x) { return shape(c, x); }, 0.01f);
                const double gain = harmonic(quiet, 1) / 0.01;
                if (c == Crossover)      expect(gain < 0.01, "XOVR: a quiet sound vanishes");
                else if (c == Cheby3)    expect(gain > 2.8 && gain < 3.1, "CHB3: a quiet sound three times louder, " + juce::String(gain));
                else if (c == Asym)      expect(gain > 0.75 && gain < 1.55, "ASYM: between its two slopes, " + juce::String(gain));
                else if (c == Rectify || c == HalfWave) expect(gain < 0.6, juce::String(names()[c]) + ": half or none of the fundamental");
                else if (c == Fold)      expect(gain > 1.5 && gain < 1.6, "FOLD: pi / 2");
                else                     expect(gain > 0.97 && gain < 1.03, juce::String(names()[c]) + " passes a quiet sound: " + juce::String(gain));
            }
        }

        beginTest("Worked off-centre, every curve makes even harmonics, and silence is still silence");
        {
            for (int c = 0; c < Count; ++c)
            {
                Biased b;
                b.set(c, 0.25f);
                expectEquals(b(0.0f), 0.0f, juce::String(names()[c]) + ": 0 for 0");
                expect(b.slope >= 0.2f && std::isfinite(b.slope), juce::String(names()[c]) + ": a slope to divide by");
                const auto s = shaped(b, 1.0f);
                expect(harmonic(s, 2) > 0.004, juce::String(names()[c]) + " off-centre: second harmonic " + juce::String(harmonic(s, 2), 4));
            }
        }

        beginTest("Waveshaper: every CURV at both ends of DRIV and ASYM - finite, silent at rest, no steady offset; ASYM acts on every curve");
        {
            for (int c = 0; c < Count; ++c)
            {
                const juce::String name = names()[c];
                double evenOff = 0.0, evenOn = 0.0;
                for (int corner = 0; corner < 4; ++corner)
                {
                    Waveshaper w;
                    w.prepare(SR, 64);
                    w.setParameter(Waveshaper::Curve, (float) c);
                    w.setParameter(Waveshaper::Drive, (corner & 1) ? 1.0f : 0.05f);
                    w.setParameter(Waveshaper::Asymmetry, (corner & 2) ? 1.0f : 0.0f);
                    w.setParameter(Waveshaper::Tone, 1.0f);
                    w.setParameter(Waveshaper::Mix, 1.0f);
                    expectEquals((int) w.getParameter(Waveshaper::Curve), c);
                    expectEquals(peak(through(w, silence(0.05))), 0.0f, name + ": silent at rest");
                    const auto out = through(w, sine(1.0, 100.0, 0.5f));
                    expect(finite(out) && peak(out) < 4.0f, name + " corner " + juce::String(corner) + ": " + juce::String(peak(out)));
                    expect(std::abs(mean(out, 43200, 48000)) < 0.02f, name + " corner " + juce::String(corner) + ": offset " + juce::String(mean(out, 43200, 48000)));
                    if (corner == 0 || corner == 2)
                    {
                        Signal tail(out.begin() + 43200, out.end());
                        (corner == 0 ? evenOff : evenOn) = harmonic(tail, 2) / std::max(1.0e-9, (double) rms(tail, 0, tail.size()));
                    }
                }
                expect(std::abs(evenOn - evenOff) > 0.003, name + ": ASYM changes the second harmonic, " + juce::String(evenOff, 4) + " -> " + juce::String(evenOn, 4));
            }
            // The cell offers the whole list, and a value beyond it is held to it.
            Waveshaper w;
            expectEquals((int) w.getParamSpec(1).maxVal, (int) Count - 1);
            expectEquals(w.getParamSpec(1).numChoices, (int) Count);
            w.setParameter(Waveshaper::Curve, 999.0f);
            expectEquals((int) w.getParameter(Waveshaper::Curve), (int) Count - 1);
        }

        beginTest("Amp: SAT starts on TANH and reads back; every curve is finite, held and silent at rest, and is its own sound");
        {
            AmpSim plain;
            plain.prepare(SR, 64);
            const ParamSpec spec = plain.getExtraSpec(0);
            expect(spec.isValid() && juce::String(spec.label) == "SAT" && spec.numChoices == Count);
            expectEquals((int) spec.defaultVal, (int) Tanh);
            expectEquals((int) plain.getExtra(0), (int) Tanh);
            expect(! plain.getExtraSpec(1).isValid());
            const auto in = sine(0.5, 110.0, 0.3f);
            const auto reference = through(plain, in);
            for (int c = 0; c < Count; ++c)
            {
                AmpSim amp;
                amp.prepare(SR, 64);
                amp.setParameter(AmpSim::Gain, 0.7f);
                amp.setExtra(0, (float) c);
                expectEquals((int) amp.getExtra(0), c);
                expectEquals(peak(through(amp, silence(0.05))), 0.0f, juce::String(names()[c]) + ": silent at rest");
                const auto out = through(amp, in);
                expect(finite(out) && peak(out) < 4.0f, juce::String(names()[c]) + ": " + juce::String(peak(out)));
                if (c != Tanh)
                {
                    AmpSim same;
                    same.prepare(SR, 64);
                    same.setParameter(AmpSim::Gain, 0.7f);
                    Signal diff = through(same, in);
                    for (size_t i = 0; i < diff.size(); ++i) diff[i] -= out[i];
                    expect(rms(diff, 12000, 24000) > 1.0e-3f, juce::String(names()[c]) + " is not TANH: " + juce::String(rms(diff, 12000, 24000)));
                }
            }
            expect(rms(reference, 12000, 24000) > 0.01f);
        }

        beginTest("Dub echo: SAT starts on FAST; every curve passes a quiet repeat at unity and holds the loop at the top of FDBK");
        {
            DubDelay first;
            expectEquals((int) first.getExtraSpec(0).defaultVal, (int) Fast);
            expectEquals((int) first.getExtra(0), (int) Fast);
            auto echoLevel = [](int c)
            {
                DubDelay dub;
                dub.prepare(SR, 64);
                dub.setParameter(DubDelay::Time, 0.25f);
                dub.setParameter(DubDelay::Feedback, 0.0f);
                dub.setParameter(DubDelay::Tone, 0.5f);
                dub.setParameter(DubDelay::DriveAmt, 0.5f);
                dub.setExtra(0, (float) c);
                dub.reset();
                return rms(through(dub, sine(1.0, 600.0, 0.002f)), 24000, 43200);
            };
            const float reference = echoLevel(Fast);
            for (int c = 0; c < Count; ++c)
            {
                const juce::String name = names()[c];
                const float level = echoLevel(c);
                expect(level > 0.8f * reference && level < 1.25f * reference, name + ": a quiet repeat at unity, " + juce::String(level / reference, 3));
                for (float driv : { 0.0f, 1.0f })
                {
                    DubDelay dub;
                    dub.prepare(SR, 64);
                    dub.setParameter(DubDelay::Time, 0.12f);
                    dub.setParameter(DubDelay::Feedback, 1.0f);
                    dub.setParameter(DubDelay::DriveAmt, driv);
                    dub.setExtra(0, (float) c);
                    dub.reset();
                    expectEquals((int) dub.getExtra(0), c);
                    const auto loud = through(dub, noise(1.0, 0.7f));
                    Signal last;
                    for (int i = 0; i < 6; ++i) last = through(dub, silence(1.0));
                    expect(finite(loud) && finite(last) && peak(loud) < 8.0f && peak(last) < 8.0f,
                           name + " DRIV " + juce::String(driv, 0) + ": " + juce::String(peak(loud)) + " " + juce::String(peak(last)));
                }
            }
        }

        beginTest("TRK FX page FX2: the amp's SAT is on knob 5, reaches the effect, is saved with the project and comes back");
        {
            Rig r;
            r.audio.addInsertEffect(0, EffectType::WaveshaperEffect);
            r.audio.addInsertEffect(0, EffectType::AmpEffect);
            expect(AudioEngine::isInsertExtraParam(Rig::extraId(1)) && ! AudioEngine::isInsertSwitchParam(Rig::extraId(1)));
            expectEquals((int) r.audio.getTrackParameter(0, Rig::extraId(1)), (int) Tanh);
            expectEquals(r.audio.getTrackParameter(0, Rig::extraId(0)), 0.0f, "the waveshaper has no extra cell");

            int page = -1;
            for (int i = 0; i < r.fx->getPageKeyCount(); ++i)
                if (r.fx->getPageKeyName(i) == "FX2") page = i;
            expect(page >= 0, "TRK FX has the page FX2");
            r.fx->onPageKey(page);
            expectEquals(r.fx->getPageName(), juce::String("FX2"));
            r.turn(0, 1);                                   // SLOT: the amp
            r.turn(4, 6);                                   // SAT: six on from TANH
            expectEquals((int) r.audio.getTrackParameter(0, Rig::extraId(1)), (int) Tanh + 6);
            auto* amp = r.audio.getInsertChain(0)->getEffect(1);
            expectEquals((int) amp->getExtra(0), (int) Tanh + 6, "the effect has it");
            r.turn(5, 3);                                   // (no cell there: nothing happens)
            expectEquals((int) r.audio.getTrackParameter(0, Rig::extraId(1)), (int) Tanh + 6);

            ProjectBridge::Live live { r.seq, r.audio };
            const auto json = juce::JSON::toString(ProjectBridge::capture(live, "sat").toVar());
            Rig other;
            ProjectBridge::Live otherLive { other.seq, other.audio };
            ProjectBridge::apply(ProjectData::fromVar(juce::JSON::parse(json)), otherLive);
            expectEquals((int) other.audio.getTrackParameter(0, Rig::extraId(1)), (int) Tanh + 6, "back from the project");

            // Another effect in the slot starts with its own values, not the amp's.
            r.audio.setInsertEffect(0, 1, EffectType::AmpEffect);
            expectEquals((int) r.audio.getTrackParameter(0, Rig::extraId(1)), (int) Tanh);
        }

        beginTest("PERF's MORE page: the dub echo's SAT is on knob 3, reaches the effect, is saved, and an older list starts on FAST");
        {
            Rig r;
            r.mgr.setActiveScreen(1);
            auto& rack = r.audio.getPerfFx();
            rack.setKind(0, 0, PerfFx::DUB);
            expectEquals((int) rack.get(0, 0).extra[0], (int) Fast);
            PerfFx::Layer l = rack.get(0, 0);
            l.extra[0] = (float) Diode;
            rack.setLayer(0, 0, l);
            expectEquals((int) rack.get(0, 0).extra[0], (int) Diode);
            l.extra[0] = 999.0f;
            rack.setLayer(0, 0, l);
            expectEquals((int) rack.get(0, 0).extra[0], (int) Count - 1, "held to the list");

            // From the controller: three pages on is MORE, knob 3 is SAT.
            l.extra[0] = (float) Fast;
            rack.setLayer(0, 0, l);
            for (int i = 0; i < 3; ++i) { r.mgr.controlChangeReceived(1, 52, 127); r.mgr.controlChangeReceived(1, 52, 0); }
            expectEquals(r.perf->getSubPage(), juce::String("MORE"));
            r.turn(2, 3);
            expectEquals((int) rack.get(0, 0).extra[0], (int) Fast + 3, "knob 3 on MORE");
            l = rack.get(0, 0);
            l.extra[0] = 999.0f;
            rack.setLayer(0, 0, l);

            const auto flat = rack.toFlat();
            expectEquals((int) flat[5], PerfFx::LAYER_FIELDS);
            PerfFxRack other;
            other.setCreator(&AudioEngine::createPerfEffect, &AudioEngine::perfWays, nullptr);
            other.prepare(SR, 64);
            expect(other.fromFlat(flat));
            expectEquals((int) other.get(0, 0).extra[0], (int) Count - 1, "back from the list");

            // A list written before the extra cells: twenty fields a layer.
            std::vector<float> old { 2.0f, 1.0f, 16.0f, 4.0f, 3.0f, 20.0f };
            for (int pad = 0; pad < 16; ++pad)
            {
                old.insert(old.end(), { 0.0f, 0.0f, 0.0f });
                for (int layer = 0; layer < 4; ++layer)
                {
                    std::vector<float> fields(20, 0.0f);
                    if (pad == 0 && layer == 0) { fields[0] = (float) PerfFx::DUB; fields[3] = 1.0f; fields[4] = 0.3f; fields[5] = 0.5f; }
                    old.insert(old.end(), fields.begin(), fields.end());
                }
            }
            expect(other.fromFlat(old));
            expectEquals(other.get(0, 0).kind, (int) PerfFx::DUB);
            expectEquals((int) other.get(0, 0).extra[0], (int) Fast, "an older list: the kind's own start");
        }
    }
};

static SaturationTests saturationTests;
