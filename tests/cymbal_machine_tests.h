#pragma once

#include "../src/engine/DrumEngine.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/project/CymbalPresets.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/ProjectBridge.h"
#include "../src/ui/screens/DrumPadScreen.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include <juce_core/juce_core.h>
#include <complex>
#include <vector>

// The cymbal machine (drum type CSYN, src/engine/CymbalMachine.h): its wiring (type, page, cells,
// locks, projects, screens, kits) and its sound, measured. Nobody can listen in a unit test, so
// the requirements are the ones a cymbal has that can be counted: the highs die before the lows,
// the spectrum is dense and irregular, a ride has a ping above its wash, a tam-tam swells after
// the stroke, a harder hit is brighter and longer, nothing repeats like a buzzer. The same
// measures of the older CYMB type are taken beside them: they are what was wrong with it.
namespace cymbaltest
{
    inline void fft(std::vector<std::complex<double>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double angle = -2.0 * 3.14159265358979323846 / (double) len;
            const std::complex<double> wl(std::cos(angle), std::sin(angle));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w(1.0);
                for (size_t j = 0; j < len / 2; ++j)
                {
                    const auto u = a[i + j], v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    // Power spectrum of n samples from `start` (Hann window); bin i is i * sr / n Hz.
    inline std::vector<double> spectrum(const std::vector<float>& x, size_t start, size_t n)
    {
        std::vector<std::complex<double>> a(n);
        for (size_t i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double) i / (double) n);
            a[i] = (start + i < x.size() ? (double) x[start + i] : 0.0) * w;
        }
        fft(a);
        std::vector<double> p(n / 2);
        for (size_t i = 0; i < n / 2; ++i) p[i] = std::norm(a[i]);
        return p;
    }

    inline double centroid(const std::vector<float>& x, double seconds, double sr, size_t n = 2048)
    {
        const auto p = spectrum(x, (size_t) (seconds * sr), n);
        double num = 0.0, den = 0.0;
        for (size_t i = 1; i < p.size(); ++i)
        {
            num += (double) i * sr / (double) n * p[i];
            den += p[i];
        }
        return den > 1.0e-18 ? num / den : 0.0;
    }

    inline double rms(const std::vector<float>& x, double from, double to, double sr)
    {
        const size_t a = (size_t) (from * sr), b = std::min(x.size(), (size_t) (to * sr));
        double s = 0.0;
        for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
        return std::sqrt(s / (double) std::max<size_t>(1, b - a));
    }

    // Bands: below 1 kHz, 1-4 kHz, 4-10 kHz, above 10 kHz, everything.
    struct Bands
    {
        double t60[5] = {};         // seconds until the band has fallen 60 dB below its loudest moment for good
        double loudest[5] = {};     // when that moment was
        double level[5] = {};       // its power then
    };

    inline Bands bands(const std::vector<float>& x, double sr, size_t hop = 512)
    {
        const size_t n = 1024;
        std::vector<double> e[5];
        for (size_t s = 0; s + n <= x.size(); s += hop)
        {
            const auto p = spectrum(x, s, n);
            double b[5] = {};
            for (size_t i = 1; i < p.size(); ++i)
            {
                const double f = (double) i * sr / (double) n;
                b[f < 1000.0 ? 0 : f < 4000.0 ? 1 : f < 10000.0 ? 2 : 3] += p[i];
                b[4] += p[i];
            }
            for (int k = 0; k < 5; ++k) e[k].push_back(b[k]);
        }
        Bands out;
        for (int k = 0; k < 5; ++k)
        {
            double mx = 0.0;
            size_t at = 0;
            for (size_t i = 0; i < e[k].size(); ++i)
                if (e[k][i] > mx) { mx = e[k][i]; at = i; }
            size_t last = at;
            for (size_t i = at; i < e[k].size(); ++i)
                if (e[k][i] > mx * 1.0e-6) last = i;
            out.t60[k] = (double) ((last + 1) * hop) / sr;
            out.loudest[k] = (double) (at * hop) / sr;
            out.level[k] = mx;
        }
        return out;
    }

    // The partials that stand out between 300 Hz and 12 kHz in 32768 samples from `seconds`: local
    // maxima within 40 dB of the strongest and 6 dB above the lowest point nearby on either side.
    inline std::vector<double> partials(const std::vector<float>& x, double sr, double seconds, std::vector<double>* powers = nullptr)
    {
        const size_t n = 32768;
        const auto p = spectrum(x, (size_t) (seconds * sr), n);
        const size_t lo = (size_t) (300.0 * (double) n / sr), hi = std::min(p.size() - 9, (size_t) (12000.0 * (double) n / sr));
        double mx = 0.0;
        for (size_t i = lo; i < hi; ++i) mx = std::max(mx, p[i]);
        std::vector<double> at;
        for (size_t i = lo + 8; i < hi; ++i)
        {
            if (p[i] < mx * 1.0e-4) continue;
            bool top = true;
            for (int d = -3; d <= 3; ++d) if (d != 0 && p[(size_t) ((int) i + d)] > p[i]) top = false;
            if (! top) continue;
            double l = p[i], r = p[i];
            for (size_t d = 1; d <= 8; ++d) { l = std::min(l, p[i - d]); r = std::min(r, p[i + d]); }
            if (p[i] > 4.0 * std::max(l, r))
            {
                // (the place between bins, from the three around the top)
                const double a = std::log(p[i - 1] + 1.0e-30), b = std::log(p[i] + 1.0e-30), c = std::log(p[i + 1] + 1.0e-30);
                const double shift = 0.5 * (a - c) / (a - 2.0 * b + c);
                at.push_back(((double) i + shift) * sr / (double) n);
                if (powers != nullptr) powers->push_back(p[i]);
            }
        }
        return at;
    }

    // How strongly the waveform repeats itself at a lag of 0.4 .. 12 ms, 0.1 .. 0.4 s after the
    // stroke: 1 = a periodic tone (a buzzer), near 0 = noise.
    inline double repeat(const std::vector<float>& x, double sr)
    {
        const size_t from = (size_t) (0.1 * sr), n = (size_t) (0.3 * sr), maxLag = (size_t) (0.012 * sr);
        if (from + n + maxLag >= x.size()) return 0.0;
        double e = 0.0;
        for (size_t i = 0; i < n; ++i) e += (double) x[from + i] * x[from + i];
        if (e < 1.0e-12) return 0.0;
        double best = 0.0;
        for (size_t lag = (size_t) (0.0004 * sr); lag < maxLag; ++lag)
        {
            double c = 0.0, e2 = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                c += (double) x[from + i] * x[from + i + lag];
                e2 += (double) x[from + i + lag] * x[from + i + lag];
            }
            best = std::max(best, c / std::sqrt(e * e2));
        }
        return best;
    }

    inline double dB(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

    inline void apply(DrumVoice& v, const CymbalPresets::Sound& s)
    {
        using M = CymbalMachine;
        v.setType(DrumSynthType::CymbalSynth);
        v.setTune(s.tune); v.setDecay(s.decay); v.setTone(s.tone); v.setDrive(s.drive); v.setSnap(s.snap); v.setLevel(s.level);
        v.setCymbalParam(M::ParModel, (float) s.model); v.setCymbalParam(M::ParPing, s.ping);
        v.setCymbalParam(M::ParColour, s.colour);       v.setCymbalParam(M::ParNoise, s.noise);
        v.setCymbalParam(M::ParDamp, s.damp);           v.setCymbalParam(M::ParBloom, s.bloom);
        v.setCymbalParam(M::ParVelSens, s.velSens);     v.setCymbalParam(M::ParLofi, s.lofi);
    }

    inline std::vector<float> render(DrumVoice& v, double seconds, double sr, float velocity = 1.0f)
    {
        v.noteOn(velocity);
        std::vector<float> out((size_t) (seconds * sr));
        for (auto& s : out) s = v.process();
        return out;
    }

    inline std::vector<float> render(const CymbalPresets::Sound& s, double seconds, double sr = 48000.0, float velocity = 1.0f,
                                     const std::function<void(DrumVoice&)>& change = nullptr)
    {
        DrumVoice v;
        v.setSampleRate(sr);
        apply(v, s);
        if (change) change(v);
        return render(v, seconds, sr, velocity);
    }

    inline double largestStep(const std::vector<float>& x, size_t from, size_t to)
    {
        double m = 0.0;
        for (size_t i = std::max<size_t>(1, from); i < to && i < x.size(); ++i)
            m = std::max(m, (double) std::abs(x[i] - x[i - 1]));
        return m;
    }
}

class CymbalMachineTests : public juce::UnitTest
{
public:
    CymbalMachineTests() : UnitTest("Cymbal Machine") {}

    using M = CymbalMachine;
    using D = DrumEngine;
    using Sound = CymbalPresets::Sound;

    static Sound with(Sound s, int param, float value)
    {
        switch (param)
        {
            case M::ParModel:   s.model = (int) value; break;
            case M::ParPing:    s.ping = value; break;
            case M::ParColour:  s.colour = value; break;
            case M::ParNoise:   s.noise = value; break;
            case M::ParDamp:    s.damp = value; break;
            case M::ParBloom:   s.bloom = value; break;
            case M::ParVelSens: s.velSens = value; break;
            default:            s.lofi = value; break;
        }
        return s;
    }

    static double difference(const std::vector<float>& a, const std::vector<float>& b)
    {
        double d = 0.0, e = 0.0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            d += ((double) a[i] - b[i]) * ((double) a[i] - b[i]);
            e += (double) a[i] * a[i];
        }
        return std::sqrt(d / std::max(e, 1.0e-20));
    }

    static bool finite(const std::vector<float>& x, float bound)
    {
        for (float s : x)
            if (! std::isfinite(s) || std::abs(s) > bound)
                return false;
        return true;
    }

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        Rig()
        {
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Drum));
            seq.getPattern().getTrack(0).engineType = EngineType::Drum;
        }
        DrumEngine& drums() { return *dynamic_cast<DrumEngine*>(audio.getEngine(0)); }
        Track& track() { return seq.getPattern().getTrack(0); }
    };

    static std::vector<float> renderPad(DrumEngine& e, int pad, double seconds, double sr = 48000.0, float velocity = 1.0f)
    {
        e.prepareToPlay(sr, 64);
        e.noteOn(pad, velocity);
        juce::AudioBuffer<float> buf(2, 64);
        juce::MidiBuffer midi;
        std::vector<float> out;
        for (int b = 0; b < (int) (seconds * sr) / 64; ++b)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < 64; ++i)
                out.push_back(buf.getSample(0, i) + buf.getSample(1, i));
        }
        return out;
    }

    void runTest() override
    {
        using namespace cymbaltest;
        const double sr = 48000.0;

        // ================================================================== wiring
        beginTest("The type is appended (CSYN = 15) and has its page of eight cells, ids 528 + pad * 8 + n");
        {
            expectEquals((int) DrumSynthType::KickSynth, 14, "the older types keep their numbers");
            expectEquals((int) DrumSynthType::Cymbal, 8);
            expectEquals((int) DrumSynthType::CymbalSynth, 15);
            expectEquals(D::NUM_DRUM_TYPES, 16);
            expectEquals(juce::String(D::typeNames()[15]), juce::String("CSYN"));
            expectEquals(juce::String(D::drumTypeAbbrev(DrumSynthType::CymbalSynth)), juce::String("CS"));
            expectEquals(CymbalPresets::CYMBAL_TYPE, (int) DrumSynthType::CymbalSynth);
            expectEquals(CymbalPresets::CYMBAL_BASE, D::CYMBAL_BASE);
            expectEquals(D::CYMBAL_BASE, 528);

            const char* labels[8] = { "MODL", "PING", "COLR", "NOIS", "DAMP", "BLOM", "VSNS", "LOFI" };
            DrumEngine e;
            for (int pad = 0; pad < D::NUM_VOICES; ++pad)
            {
                const auto page = D::cymbalPage(pad);
                expectEquals(page.name, juce::String("CYM"));
                for (int n = 0; n < 8; ++n)
                {
                    const auto& spec = page.params[(size_t) n];
                    expectEquals(juce::String(spec.label), juce::String(labels[n]));
                    expectEquals(spec.id, 528 + pad * 8 + n);
                    expect(EngineParamRange::contains(spec.id) && D::isCymbalParam(spec.id) && ! D::isKickParam(spec.id));
                    expect(! e.isRebuildParam(spec.id), "the cell can be locked on a step");
                    // (Top pads 7 and 8 of the starting kit are the machine's crash and ride: below.)
                    if (pad != 6 && pad != 7)
                        expectWithinAbsoluteError(e.getParameter(spec.id), M::paramDefault(n), 1.0e-6f, "its default is there from the start");
                    expectWithinAbsoluteError(spec.defaultVal, M::paramDefault(n), 1.0e-6f);
                }
                const auto& model = page.params[0];
                expect(model.kind == ParamKind::Choice && model.numChoices == M::NumModels);
            }
            // The starting kit: top pad 7 is the crash, top pad 8 the ride, as the ready settings have them.
            for (int k = 0; k < 2; ++k)
            {
                const int pad = 6 + k;
                const auto& s = k == 0 ? CymbalPresets::crash : CymbalPresets::ride;
                expectEquals((int) e.getParameter(pad * 8 + 7), (int) DrumSynthType::CymbalSynth, "type");
                expectEquals(e.getParameter(pad * 8 + 0), s.tune, "TUNE");
                expectEquals(e.getParameter(pad * 8 + 1), s.decay, "DEC");
                const float want[8] = { (float) s.model, s.ping, s.colour, s.noise, s.damp, s.bloom, s.velSens, s.lofi };
                for (int n = 0; n < 8; ++n)
                    expectEquals(e.getParameter(D::CYMBAL_BASE + pad * 8 + n), want[n], "pad " + juce::String(pad + 1) + " cell " + juce::String(n));
            }
            const char* models[M::NumModels] = { "METL", "RING", "RIDE", "BELL", "CRSH", "CHNA", "SPLS", "GONG", "TAM" };
            for (int i = 0; i < M::NumModels; ++i)
                expectEquals(juce::String(M::modelNames()[i]), juce::String(models[i]), "models keep their numbers (projects store them)");

            // In the engine's list: a DR, a KICK and a CYM page for each pad, then STRIP, LFO1, LFO2.
            const auto pages = e.getParamPages();
            expectEquals((int) pages.size(), D::PAGES_PER_PAD * D::NUM_VOICES + 3);
            expectEquals(pages[(size_t) (2 * D::NUM_VOICES)].name, juce::String("CYM"));
            expectEquals(pages[pages.size() - 3].name, juce::String("STRIP"));
            expectEquals(pages[pages.size() - 1].name, juce::String("LFO2"));
        }

        beginTest("Every cell is applied by setParameter and returned by getParameter, limited to its range");
        {
            DrumEngine e;
            for (int pad : { 0, 7, 15 })
                for (int n = 0; n < M::NumParams; ++n)
                {
                    const auto spec = D::cymbalSpec(pad, n);
                    for (float v : { spec.minVal, spec.maxVal, 0.5f * (spec.minVal + spec.maxVal), spec.defaultVal })
                    {
                        e.setParameter(spec.id, v);
                        const float held = n == M::ParModel ? std::round(v) : v;
                        expectWithinAbsoluteError(e.getParameter(spec.id), held, 1.0e-6f);
                        expectWithinAbsoluteError(e.getVoice(pad).getCymbalParam(n), held, 1.0e-6f, "and it reached the voice");
                    }
                    e.setParameter(spec.id, spec.maxVal + 5.0f);
                    expectWithinAbsoluteError(e.getParameter(spec.id), spec.maxVal, 1.0e-6f);
                    e.setParameter(spec.id, spec.minVal - 5.0f);
                    expectWithinAbsoluteError(e.getParameter(spec.id), spec.minVal, 1.0e-6f);
                }
            // One pad's cells are its own; the kick page beside them is untouched.
            DrumEngine f;
            f.setParameter(D::CYMBAL_BASE + 3 * 8 + M::ParBloom, 0.9f);
            expectWithinAbsoluteError(f.getParameter(D::CYMBAL_BASE + 4 * 8 + M::ParBloom), M::paramDefault(M::ParBloom), 1.0e-6f);
            expectWithinAbsoluteError(f.getParameter(D::KICK_BASE + 3 * 8 + 5), DrumVoice::kickDefault(5), 1.0e-6f);
        }

        // ================================================================== every cell is heard
        beginTest("Every cell of the CYM page changes the sound on every model, and so do the pad's own cells");
        {
            for (int model = 0; model < M::NumModels; ++model)
            {
                Sound base = CymbalPresets::ride;
                base.model = model;
                base.lofi = 0.0f;
                const juce::String name = M::modelNames()[model];
                for (int n = M::ParPing; n < M::NumParams; ++n)
                {
                    // (VSNS is heard on a hit that is not a full one.)
                    const float vel = n == M::ParVelSens ? 0.5f : 1.0f;
                    const auto lo = render(with(base, n, 0.0f), 0.6, 44100.0, vel), hi = render(with(base, n, 1.0f), 0.6, 44100.0, vel);
                    expect(finite(lo, 4.0f) && finite(hi, 4.0f), name + " cell " + juce::String(n) + " finite");
                    expect(difference(lo, hi) > 0.05, name + " " + D::cymbalSpec(0, n).label + " does nothing: " + juce::String(difference(lo, hi), 4));
                }
                struct Own { const char* label; std::function<void(DrumVoice&)> lo, hi; };
                const Own own[] = {
                    { "TUNE", [](DrumVoice& v) { v.setTune(4000.0f); }, [](DrumVoice& v) { v.setTune(10000.0f); } },
                    { "DEC",  [](DrumVoice& v) { v.setDecay(0.1f); },   [](DrumVoice& v) { v.setDecay(2.0f); } },
                    { "TONE", [](DrumVoice& v) { v.setTone(0.0f); },    [](DrumVoice& v) { v.setTone(1.0f); } },
                    { "DRIV", [](DrumVoice& v) { v.setDrive(0.0f); },   [](DrumVoice& v) { v.setDrive(1.0f); } },
                    { "SNAP", [](DrumVoice& v) { v.setSnap(0.0f); },    [](DrumVoice& v) { v.setSnap(1.0f); } },
                    { "LEVL", [](DrumVoice& v) { v.setLevel(0.2f); },   [](DrumVoice& v) { v.setLevel(1.0f); } } };
                for (auto& cell : own)
                {
                    const auto lo = render(base, 0.6, 44100.0, 1.0f, cell.lo), hi = render(base, 0.6, 44100.0, 1.0f, cell.hi);
                    expect(difference(lo, hi) > 0.05, name + " " + cell.label + " does nothing");
                }
            }
            // The nine models are nine different sounds.
            std::vector<std::vector<float>> all;
            for (int model = 0; model < M::NumModels; ++model)
                all.push_back(render(with(CymbalPresets::ride, M::ParModel, (float) model), 0.5));
            for (size_t a = 0; a < all.size(); ++a)
                for (size_t b = a + 1; b < all.size(); ++b)
                    expect(difference(all[a], all[b]) > 0.3, juce::String(M::modelNames()[a]) + " and " + M::modelNames()[b] + " are the same");
        }

        beginTest("The cells do what their names say");
        {
            // PING: only the partials at the top, only the wash at the bottom.
            {
                const auto pingOnly = partials(render(with(CymbalPresets::ride, M::ParPing, 1.0f), 1.0), sr, 0.05);
                const auto washOnly = partials(render(with(CymbalPresets::ride, M::ParPing, 0.0f), 1.0), sr, 0.05);
                expect(pingOnly.size() <= 40 && washOnly.size() >= 200,
                       "PING 1 / 0: " + juce::String((int) pingOnly.size()) + " / " + juce::String((int) washOnly.size()) + " partials");
            }
            // COLR: the wash moves up.
            {
                const double dark = centroid(render(with(CymbalPresets::crash, M::ParColour, 0.0f), 0.5), 0.1, sr);
                const double bright = centroid(render(with(CymbalPresets::crash, M::ParColour, 1.0f), 0.5), 0.1, sr);
                expect(bright > 1.8 * dark, "COLR: " + juce::String(dark, 0) + " -> " + juce::String(bright, 0) + " Hz");
            }
            // NOIS: from a few dozen pure partials to a wash without lines that stay.
            {
                const auto pure = partials(render(with(CymbalPresets::crash, M::ParNoise, 0.0f), 1.0), sr, 0.05);
                const auto hiss = partials(render(with(CymbalPresets::crash, M::ParNoise, 1.0f), 1.0), sr, 0.05);
                expect(pure.size() <= M::NUM_MODES + 4 && hiss.size() >= 300,
                       "NOIS 0 / 1: " + juce::String((int) pure.size()) + " / " + juce::String((int) hiss.size()) + " partials");
            }
            // DAMP: at 0 the top lasts as long as the bottom; at the top of the cell a small part of it.
            {
                const auto together = bands(render(with(CymbalPresets::crash, M::ParDamp, 0.0f), 9.0), sr);
                const auto apart = bands(render(with(CymbalPresets::crash, M::ParDamp, 1.0f), 9.0), sr);
                expect(together.t60[3] > 0.7 * together.t60[1], "DAMP 0: " + juce::String(together.t60[3], 2) + " s above 10 kHz, " + juce::String(together.t60[1], 2) + " s at 1-4 kHz");
                expect(apart.t60[3] < 0.3 * apart.t60[1], "DAMP 1: " + juce::String(apart.t60[3], 2) + " s above 10 kHz, " + juce::String(apart.t60[1], 2) + " s at 1-4 kHz");
            }
            // BLOM: the loudest moment of a tam-tam's upper bands moves away from the stroke.
            {
                const auto dry = bands(render(with(CymbalPresets::tamTam, M::ParBloom, 0.0f), 4.0), sr);
                const auto bloom = bands(render(with(CymbalPresets::tamTam, M::ParBloom, 1.0f), 4.0), sr);
                expect(dry.loudest[1] < 0.1 && bloom.loudest[1] > 0.4,
                       "BLOM 0 / 1: 1-4 kHz loudest at " + juce::String(dry.loudest[1], 2) + " / " + juce::String(bloom.loudest[1], 2) + " s");
            }
            // VSNS: at 0 a soft hit is the same sound, only quieter; at the top it is duller and shorter.
            {
                Sound plain = with(CymbalPresets::crash, M::ParVelSens, 0.0f), full = with(CymbalPresets::crash, M::ParVelSens, 1.0f);
                plain.bloom = full.bloom = 0.0f;        // (the cascade is its own part of the velocity)
                const auto hard = render(plain, 6.0), soft = render(plain, 6.0, sr, 0.4f);
                expectWithinAbsoluteError(centroid(soft, 0.1, sr) / centroid(hard, 0.1, sr), 1.0, 0.03, "VSNS 0: as bright");
                expectWithinAbsoluteError(dB(rms(soft, 0.0, 0.5, sr) / rms(hard, 0.0, 0.5, sr)), dB(0.4), 0.5, "VSNS 0: quieter by the velocity");
                const auto softer = render(full, 6.0, sr, 0.4f), harder = render(full, 6.0, sr, 1.0f);
                expect(centroid(softer, 0.1, sr) < 0.7 * centroid(harder, 0.1, sr), "VSNS 1: a soft hit is duller");
                expect(bands(softer, sr).t60[4] < 0.8 * bands(harder, sr).t60[4], "VSNS 1: and shorter");
            }
            // LOFI: the sound is held between readings (runs of equal samples), and off at 0.
            {
                auto runs = [](const std::vector<float>& x)
                {
                    int same = 0;
                    for (size_t i = 4801; i < 24000; ++i) same += x[i] == x[i - 1] ? 1 : 0;
                    return (double) same / 19200.0;
                };
                expect(runs(render(with(CymbalPresets::ride, M::ParLofi, 0.0f), 0.5)) < 0.01);
                expect(runs(render(with(CymbalPresets::ride, M::ParLofi, 0.6f), 0.5)) > 0.5);
            }
            // TUNE: the whole instrument, an octave for an octave.
            {
                Sound bell = CymbalPresets::rideBell;
                std::vector<double> powers;
                bell.tune = 8000.0f;
                auto at = partials(render(bell, 1.0), sr, 0.02, &powers);
                const double strongest = at[(size_t) (std::max_element(powers.begin(), powers.end()) - powers.begin())];
                bell.tune = 4000.0f;
                powers.clear();
                at = partials(render(bell, 1.0), sr, 0.02, &powers);
                bool found = false;
                for (double f : at) found = found || std::abs(f - strongest * 0.5) < 3.0;
                expect(found, "the strongest partial (" + juce::String(strongest, 1) + " Hz) is an octave lower at half the TUNE");
            }
            // DEC: seconds for seconds.
            {
                Sound a = CymbalPresets::crash, b = CymbalPresets::crash;
                a.decay = 0.5f; b.decay = 1.5f;
                const double shortT = bands(render(a, 4.0), sr).t60[4], longT = bands(render(b, 12.0), sr).t60[4];
                expectWithinAbsoluteError(longT / shortT, 3.0, 0.6, "DEC x 3: " + juce::String(shortT, 2) + " -> " + juce::String(longT, 2) + " s");
            }
            // TONE: brightness, both ways from the middle, also while it sounds (pad pressure, an LFO).
            {
                Sound dull = CymbalPresets::crash, mid = dull, bright = dull;
                dull.tone = 0.0f; bright.tone = 1.0f;
                const double c0 = centroid(render(dull, 0.5), 0.1, sr), c1 = centroid(render(mid, 0.5), 0.1, sr), c2 = centroid(render(bright, 0.5), 0.1, sr);
                expect(c0 < 0.6 * c1 && c2 > 1.15 * c1, "TONE: " + juce::String(c0, 0) + " / " + juce::String(c1, 0) + " / " + juce::String(c2, 0) + " Hz");
            }
            // SNAP: a mallet lands softly, a hard tip with a tick.
            {
                Sound mallet = CymbalPresets::ride, tip = mallet;
                mallet.snap = 0.0f; tip.snap = 1.0f;
                const auto soft = render(mallet, 0.5), hard = render(tip, 0.5);
                expect(rms(hard, 0.0, 0.003, sr) > 4.0 * rms(soft, 0.0, 0.003, sr), "SNAP: the first 3 ms");
                expect(centroid(hard, 0.0, sr, 256) > 1.1 * centroid(soft, 0.0, sr, 256),
                       "SNAP: a brighter start, " + juce::String(centroid(soft, 0.0, sr, 256), 0) + " -> " + juce::String(centroid(hard, 0.0, sr, 256), 0) + " Hz");
            }
        }

        // ================================================================== the sounds
        beginTest("Crash: wide and dense, the top arrives after the stroke, the highs die first");
        {
            const auto x = render(CymbalPresets::crash, 12.0);
            const auto b = bands(x, sr);
            logMessage("crash T60 <1k " + juce::String(b.t60[0], 2) + "  1-4k " + juce::String(b.t60[1], 2) + "  4-10k " + juce::String(b.t60[2], 2)
                       + "  >10k " + juce::String(b.t60[3], 2) + " s; centroid " + juce::String(centroid(x, 0.05, sr), 0) + " -> " + juce::String(centroid(x, 1.0, sr), 0) + " Hz");
            expect(b.t60[3] < b.t60[2] && b.t60[2] < b.t60[1] && b.t60[1] <= b.t60[0] + 0.5, "the higher the band, the sooner it is gone");
            expect(b.t60[3] > 0.8 && b.t60[3] < 3.0, "above 10 kHz: one to three seconds");
            expect(b.t60[1] > 3.0 && b.t60[4] < 10.0, "the body: several seconds");
            expect(centroid(x, 0.05, sr) > 3500.0, "bright at the start");
            expect(centroid(x, 1.0, sr) < 0.7 * centroid(x, 0.05, sr), "darker a second later");
            expect(partials(x, sr, 0.05).size() >= 400, "hundreds of partials");
            expect(repeat(x, sr) < 0.3, "nothing in it repeats: " + juce::String(repeat(x, sr), 2));
            expect(rms(x, 0.0, 0.002, sr) > 0.3 * rms(x, 0.01, 0.05, sr), "the stroke is there at once");
            expect(rms(x, 0.01, 0.05, sr) > 1.15 * rms(x, 0.0, 0.005, sr), "and it builds for a moment after it");
            double mean = 0.0;
            for (float s : x) mean += s;
            expect(std::abs(mean / (double) x.size()) < 1.0e-4, "no DC");
        }

        beginTest("Ride: a ping of defined partials above a long, quieter wash; the house ride adds the grain");
        {
            for (auto* sound : { &CymbalPresets::ride, &CymbalPresets::houseRide })
            {
                const juce::String name = sound->name;
                const auto x = render(*sound, 10.0);
                const double ping = dB(rms(x, 0.0, 0.03, sr) / rms(x, 0.03, 0.33, sr));
                const auto b = bands(x, sr);
                logMessage(name + ": ping " + juce::String(ping, 1) + " dB over the wash; T60 1-4k " + juce::String(b.t60[1], 2) + "  4-10k "
                           + juce::String(b.t60[2], 2) + " s; centroid " + juce::String(centroid(x, 0.05, sr), 0) + " Hz");
                expect(ping > 5.0, name + ": the first 30 ms stand " + juce::String(ping, 1) + " dB above the next 300");
                expect(b.t60[2] > 1.5 && b.t60[1] > b.t60[2], name + ": the wash rings on, its highs less long");
                expect(partials(x, sr, 0.05).size() >= 300, name + ": the wash is dense");
                expect(centroid(x, 0.05, sr) > 3000.0 && centroid(x, 0.05, sr) < 8000.0, name + ": bright, not a hiss");
            }
            // The ping's partials: with the wash off they are the model's twelve, at their pitches.
            Sound pingOnly = with(CymbalPresets::ride, M::ParPing, 1.0f);
            pingOnly.snap = 0.5f;
            std::vector<double> powers;
            const auto at = partials(render(pingOnly, 1.0), sr, 0.0, &powers);
            int found = 0;
            for (double want : { 2078.0, 2811.0, 3594.0, 4467.0, 5486.0 })
                for (double f : at)
                    found += std::abs(f - want) < 4.0 ? 1 : 0;
            expectEquals(found, 5, "the five strongest ping partials are where the model puts them");
            // They are no harmonic series (a pitched buzz): their spacings are all different.
            // And the wash under the ping is the larger part of the sound after 300 ms.
            const auto full = render(CymbalPresets::ride, 2.0), noWash = render(pingOnly, 2.0);
            expect(rms(full, 0.5, 1.0, sr) > 2.0 * rms(noWash, 0.5, 1.0, sr), "after half a second the wash carries the sound");
        }

        beginTest("Ride bell: a few strong, long partials");
        {
            const auto x = render(CymbalPresets::rideBell, 8.0);
            std::vector<double> powers;
            const auto at = partials(x, sr, 0.05, &powers);
            expect(at.size() >= 6 && at.size() <= 80, "a bell, not a wash: " + juce::String((int) at.size()) + " partials");
            const double strongest = at[(size_t) (std::max_element(powers.begin(), powers.end()) - powers.begin())];
            expect(std::abs(strongest - 931.0) < 12.0 || std::abs(strongest - 1836.0) < 12.0 || std::abs(strongest - 1352.0) < 12.0,
                   "its strongest partial is one of the bell's: " + juce::String(strongest, 1) + " Hz");
            expect(bands(x, sr).t60[4] > 2.0, "it rings");
        }

        beginTest("Analog cymbal: six square oscillators, two bands, the upper one shorter");
        {
            const auto x = render(CymbalPresets::analogCymbal, 4.0);
            std::vector<double> powers;
            const auto at = partials(x, sr, 0.05, &powers);
            expect(at.size() >= 40, "the squares' overtones: " + juce::String((int) at.size()));
            // Every strong line is an odd multiple of one of the six pitches (a square has no even ones).
            std::vector<size_t> order(at.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return powers[a] > powers[b]; });
            int explained = 0, looked = 0;
            for (size_t i = 0; i < order.size() && looked < 30; ++i, ++looked)
            {
                const double f = at[order[i]];
                bool odd = false;
                for (double base : { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 })
                {
                    const double k = std::round((f / base - 1.0) * 0.5) * 2.0 + 1.0;
                    odd = odd || std::abs(f - k * base) < 6.0;
                }
                explained += odd ? 1 : 0;
            }
            expect(explained >= looked * 8 / 10, juce::String(explained) + " of the " + juce::String(looked) + " strongest lines are the oscillators' overtones");
            const auto b = bands(x, sr);
            expect(b.t60[3] < b.t60[1], "the top band is gone before the lower one");
            expect(centroid(x, 0.01, sr) > 4500.0, "bright at the stroke");
            // RING multiplies them: far more lines, none of them a square's own.
            expect(partials(render(CymbalPresets::ringCymbal, 4.0), sr, 0.05).size() > 2 * at.size());
        }

        beginTest("Tam-tam and gong: the tam-tam swells and brightens after the stroke; the gong's pitch falls as it settles");
        {
            const auto x = render(CymbalPresets::tamTam, 24.0);
            double loudest = 0.0, when = 0.0;
            for (double t = 0.0; t < 5.0; t += 0.025)
                if (rms(x, t, t + 0.05, sr) > loudest) { loudest = rms(x, t, t + 0.05, sr); when = t; }
            logMessage("tam-tam: loudest at " + juce::String(when, 2) + " s, x" + juce::String(loudest / rms(x, 0.0, 0.05, sr), 2)
                       + " the first 50 ms; centroid " + juce::String(centroid(x, 0.05, sr), 0) + " -> " + juce::String(centroid(x, 1.0, sr), 0) + " Hz");
            expect(when > 0.3 && when < 3.0, "its loudest moment comes after the stroke: " + juce::String(when, 2) + " s");
            expect(loudest > 1.3 * rms(x, 0.0, 0.05, sr), "and is louder than the stroke");
            expect(centroid(x, 1.0, sr) > 2.5 * centroid(x, 0.05, sr), "brighter a second after the stroke than at it");
            expect(centroid(x, 8.0, sr) < centroid(x, 1.0, sr), "and darker again later");
            expect(rms(x, 15.0, 15.5, sr) > 1.0e-3 * loudest, "still ringing after fifteen seconds");
            const auto b = bands(x, sr, 2048);
            expect(b.t60[2] < b.t60[1] && b.t60[1] < b.t60[0], "the shimmer goes first, the low cluster last");

            auto lowest = [&](const std::vector<float>& g, double seconds)
            {
                std::vector<double> powers;
                const auto at = partials(g, sr, seconds, &powers);
                double best = 0.0, f = 0.0;
                for (size_t i = 0; i < at.size(); ++i)
                    if (at[i] < 450.0 && powers[i] > best) { best = powers[i]; f = at[i]; }
                return f;
            };
            // (Its second partial, 393 Hz: the lowest is a pair that beats.)
            const auto g = render(CymbalPresets::gong, 6.0);
            const double early = lowest(g, 0.0), late = lowest(g, 3.0);
            const double semitones = 12.0 * std::log2(early / late);
            logMessage("gong: " + juce::String(early, 1) + " Hz at the stroke, " + juce::String(late, 1) + " Hz three seconds on");
            expect(semitones > 0.05 && semitones < 1.0, "the pitch falls by a fraction of a semitone: " + juce::String(semitones, 2));
            const auto still = render(with(CymbalPresets::gong, M::ParBloom, 0.0f), 6.0);
            expect(std::abs(12.0 * std::log2(lowest(still, 0.0) / lowest(still, 3.0))) < 0.03, "without BLOM it does not glide");
            expect(bands(g, sr, 2048).t60[4] > 5.5 || rms(g, 5.0, 5.5, sr) > 1.0e-3 * rms(g, 0.0, 0.5, sr), "a gong rings for a long time");
        }

        beginTest("China and splash: shorter than the crash; the splash higher");
        {
            const auto crash = render(CymbalPresets::crash, 10.0), china = render(CymbalPresets::china, 6.0), splash = render(CymbalPresets::splash, 6.0);
            expect(bands(china, sr).t60[4] < 0.6 * bands(crash, sr).t60[4]);
            expect(bands(splash, sr).t60[4] < 0.6 * bands(crash, sr).t60[4]);
            expect(centroid(splash, 0.05, sr) > 1.1 * centroid(crash, 0.05, sr));
            expect(partials(china, sr, 0.05).size() >= 300 && repeat(china, sr) < 0.3);
        }

        beginTest("A harder hit is louder, brighter and longer (every ready-made sound)");
        {
            for (auto* sound : CymbalPresets::all)
            {
                const bool gong = sound->model >= M::Gong;
                const double seconds = gong ? 4.0 : 9.0;
                const auto hard = render(*sound, seconds), soft = render(*sound, seconds, sr, 0.4f);
                const juce::String name = sound->name;
                expect(dB(rms(soft, 0.0, 0.5, sr) / rms(hard, 0.0, 0.5, sr)) < -7.5, name + ": quieter");
                // (At the time each has got going: a gong's brightness comes a moment after the stroke.)
                const double at = sound->model >= M::Gong ? 1.0 : 0.05;
                expect(centroid(soft, at, sr) < 0.97 * centroid(hard, at, sr),
                       name + ": duller, " + juce::String(centroid(soft, at, sr), 0) + " against " + juce::String(centroid(hard, at, sr), 0) + " Hz");
                // Shorter: it is 60 dB down sooner (the gongs, which ring for half a minute: less of
                // the soft hit is left after three seconds, level aside).
                if (gong)
                    expect(rms(soft, 3.0, 3.3, sr) / rms(soft, 0.0, 0.3, sr) < rms(hard, 3.0, 3.3, sr) / rms(hard, 0.0, 0.3, sr), name + ": shorter");
                else
                    expect(bands(soft, sr).t60[4] < 0.97 * bands(hard, sr).t60[4],
                           name + ": shorter, " + juce::String(bands(soft, sr).t60[4], 2) + " against " + juce::String(bands(hard, sr).t60[4], 2) + " s");
            }
        }

        beginTest("What was wrong with the older CYMB type, in the same measures");
        {
            // The starting kit's top pad 7 as it was before the machine took it: CYMB, TUNE 7000, DEC 1, TONE 0.6.
            DrumVoice v;
            v.setSampleRate(sr);
            v.setType(DrumSynthType::Cymbal);
            v.setTune(7000.0f); v.setDecay(1.0f); v.setTone(0.6f); v.setLevel(0.8f);
            const auto old = render(v, 6.0, sr);
            const auto b = bands(old, sr);
            const auto lines = partials(old, sr, 0.05);
            logMessage("CYMB: T60 1-4k " + juce::String(b.t60[1], 2) + "  4-10k " + juce::String(b.t60[2], 2) + "  >10k " + juce::String(b.t60[3], 2)
                       + " s; centroid " + juce::String(centroid(old, 0.05, sr), 0) + " -> " + juce::String(centroid(old, 1.0, sr), 0) + " Hz; "
                       + juce::String((int) lines.size()) + " partials; repeat " + juce::String(repeat(old, sr), 2)
                       + "; first 30 ms against the next 300: " + juce::String(dB(rms(old, 0.0, 0.03, sr) / rms(old, 0.03, 0.33, sr)), 1) + " dB");
            expect(std::abs(b.t60[3] - b.t60[1]) < 0.1 * b.t60[1], "every band dies at the same time (one envelope)");
            expectWithinAbsoluteError(centroid(old, 1.0, sr) / centroid(old, 0.05, sr), 1.0, 0.03, "its brightness never changes");
            expect(lines.size() < 30, "a handful of lines between 300 Hz and 12 kHz (its eight squares start at TUNE)");
            expect(repeat(old, sr) > 0.9, "it repeats itself exactly: a buzz");
            expect(centroid(old, 0.05, sr) > 10000.0, "nearly all of it lies above 10 kHz");

            const auto crash = render(CymbalPresets::crash, 6.0);
            const auto c = bands(crash, sr);
            expect(c.t60[3] < 0.5 * c.t60[1] && partials(crash, sr, 0.05).size() > 10 * lines.size() && repeat(crash, sr) < 0.3,
                   "the machine's crash is different in each of them");
        }

        // ================================================================== robustness
        beginTest("Finite, bounded and free of DC for every model, sample rate and corner of the cells; restruck, choked, squeezed");
        {
            juce::Random random(20261114);
            int renders = 0;
            for (double rate : { 44100.0, 48000.0, 96000.0 })
                for (int model = 0; model < M::NumModels; ++model)
                    for (int corner = 0; corner < 7; ++corner)
                    {
                        DrumVoice v;
                        v.setSampleRate(rate);
                        v.setType(DrumSynthType::CymbalSynth);
                        v.setCymbalParam(M::ParModel, (float) model);
                        auto pick = [&](float lo, float hi)
                        {
                            const int r = random.nextInt(4);
                            return r == 0 ? lo : r == 1 ? hi : lo + (hi - lo) * random.nextFloat();
                        };
                        for (int n = M::ParPing; n < M::NumParams; ++n)
                            v.setCymbalParam(n, pick(0.0f, 1.0f));
                        if (corner < 2 || corner >= 5)
                            v.setCymbalParam(M::ParLofi, 0.0f);
                        v.setTune(corner == 0 ? 20.0f : corner == 1 ? 12000.0f : corner >= 5 ? pick(4000.0f, 12000.0f) : pick(20.0f, 12000.0f));
                        v.setDecay(corner == 2 ? 0.01f : corner == 3 ? 4.0f : corner >= 5 ? pick(0.3f, 4.0f) : pick(0.01f, 4.0f));
                        v.setTone(pick(0.0f, 1.0f));
                        v.setDrive(pick(0.0f, 1.0f));
                        v.setSnap(pick(0.0f, 1.0f));
                        v.setLevel(1.0f);
                        v.setAftertouchDest(corner % 2 == 0 ? DrumVoiceATDest::Decay : DrumVoiceATDest::Pitch);
                        const int n = (int) (0.35 * rate);
                        std::vector<float> x((size_t) n);
                        v.noteOn(1.0f);
                        for (int i = 0; i < n; ++i)
                        {
                            if (i == n / 4) v.noteOn(pick(0.05f, 1.0f));
                            if (i == n / 3) v.applyAftertouch(1.0f);
                            if (i == n / 2) { v.setBendRatio(1.12f); v.noteOn(1.0f); }
                            if (i == (n * 3) / 4) v.choke();
                            x[(size_t) i] = v.process();
                        }
                        ++renders;
                        const juce::String where = juce::String(M::modelNames()[model]) + " at " + juce::String(rate, 0) + " corner " + juce::String(corner);
                        expect(finite(x, 6.0f), where + ": not finite or out of bounds");
                        for (float s : x)
                            if (s != 0.0f && std::abs(s) < 1.0e-30f) { expect(false, where + ": a denormal"); break; }
                        // (DC: where the sound has many cycles in the render and a tail to speak of.)
                        if (corner >= 5 && model < M::Gong)
                        {
                            double mean = 0.0, peak = 0.0;
                            for (float s : x) { mean += s; peak = std::max(peak, (double) std::abs(s)); }
                            expect(std::abs(mean / (double) n) < 0.01 * std::max(peak, 1.0e-6) + 1.0e-5, where + ": DC");
                        }
                    }
            expectEquals(renders, 3 * M::NumModels * 7);
        }

        beginTest("The ready-made sounds are as loud as the other drums, free of DC, and the same at every sample rate");
        {
            // The starting kit's snare at full velocity, for comparison.
            DrumEngine kit;
            const auto snare = renderPad(kit, 12, 0.5);
            double snarePeak = 0.0;
            for (float s : snare) snarePeak = std::max(snarePeak, (double) std::abs(s));
            for (auto* sound : CymbalPresets::all)
            {
                const auto x = render(*sound, 1.5);
                double peak = 0.0;
                for (float s : x) peak = std::max(peak, (double) std::abs(s));
                // (renderPad sums both sides of a centred voice: x 1.41.)
                const double asInKit = peak * 1.4142;
                logMessage(juce::String(sound->name) + ": peak " + juce::String(peak, 3) + ", rms of the first 300 ms " + juce::String(rms(x, 0.0, 0.3, sr), 3)
                           + " (the kit's snare: peak " + juce::String(snarePeak / 1.4142, 3) + ")");
                expect(asInKit > 0.25 * snarePeak && asInKit < 2.0 * snarePeak, juce::String(sound->name) + ": level out of line with the kit");
                expect(peak < 1.0, juce::String(sound->name) + ": no clipping at full velocity");
                double mean = 0.0;
                for (float s : x) mean += s;
                expect(std::abs(mean / (double) x.size()) < 1.0e-3 * peak, juce::String(sound->name) + ": DC " + juce::String(mean / (double) x.size(), 6));

                // 44.1 and 96 kHz: the same length and brightness.
                if (sound->lofi == 0.0f)
                {
                    const auto low = render(*sound, 1.5, 44100.0), high = render(*sound, 1.5, 96000.0);
                    expectWithinAbsoluteError(dB(rms(low, 0.0, 1.0, 44100.0) / rms(high, 0.0, 1.0, 96000.0)), 0.0, 1.5, juce::String(sound->name) + ": level across rates");
                    expectWithinAbsoluteError(centroid(low, 0.1, 44100.0, 2048) / centroid(high, 0.1, 96000.0, 4096), 1.0, 0.2, juce::String(sound->name) + ": brightness across rates");
                }
            }
        }

        beginTest("It ends: the voice goes idle after its tail, with exact silence and no step");
        {
            for (int model = 0; model < M::NumModels; ++model)
            {
                DrumVoice v;
                v.setSampleRate(sr);
                Sound s = CymbalPresets::ride;
                s.model = model;
                s.decay = 0.2f;
                apply(v, s);
                const double seconds = model >= M::Gong ? 14.0 : 5.0;
                const auto x = render(v, seconds, sr);
                expect(! v.isActive(), juce::String(M::modelNames()[model]) + ": still active after " + juce::String(seconds, 0) + " s at DEC 0.2");
                size_t last = 0;
                for (size_t i = 0; i < x.size(); ++i) if (x[i] != 0.0f) last = i;
                expect(last + 1 < x.size(), "silence at the end");
                double peak = 0.0;
                for (float f : x) peak = std::max(peak, (double) std::abs(f));
                expect(std::abs(x[last]) < 1.0e-3 * peak, juce::String(M::modelNames()[model]) + ": the tail ends at " + juce::String(dB(std::abs(x[last]) / peak), 0) + " dB");
            }
        }

        beginTest("No click: the stroke starts from nothing, a second stroke adds to the first, a choke is a fast fade");
        {
            for (auto* sound : CymbalPresets::all)
            {
                const juce::String name = sound->name;
                // A soft mallet has no tick of its own: whatever jumps would be a fault.
                Sound soft = *sound;
                soft.snap = 0.0f;
                soft.lofi = 0.0f;
                DrumVoice v;
                v.setSampleRate(sr);
                apply(v, soft);
                std::vector<float> x;
                v.noteOn(1.0f);
                for (int i = 0; i < 9600; ++i) x.push_back(v.process());
                double peak = 0.0;
                for (float f : x) peak = std::max(peak, (double) std::abs(f));
                expect(std::abs(x[0]) < 0.02 * peak, name + ": starts at " + juce::String(std::abs(x[0]) / peak, 3) + " of its peak");

                // Struck again while it rings, then choked.
                const size_t second = x.size();
                v.noteOn(1.0f);
                for (int i = 0; i < 9600; ++i) x.push_back(v.process());
                const double usual = largestStep(x, second - 2400, second);
                expect(largestStep(x, second, second + 3) < 1.5 * usual + 1.0e-4,
                       name + ": the second stroke steps " + juce::String(largestStep(x, second, second + 3), 4) + " (its ringing: " + juce::String(usual, 4) + ")");
                expect(v.isActive());
                const size_t choked = x.size();
                const double before = rms(x, (double) (choked - 2400) / sr, (double) choked / sr, sr);
                v.choke();
                for (int i = 0; i < 24000; ++i) x.push_back(v.process());
                expect(largestStep(x, choked, choked + 3) < 1.5 * largestStep(x, choked - 2400, choked) + 1.0e-4, name + ": the choke does not step");
                const double after = rms(x, (double) (choked + 3840) / sr, (double) (choked + 4800) / sr, sr);
                expect(after < 0.02 * before, name + ": 80 ms after the choke it is " + juce::String(dB(after / before), 0) + " dB down");
                expect(! v.isActive(), name + ": and the voice is idle half a second later");
            }

            // A roll builds the wash (what rings is added to), within limits.
            DrumVoice one, roll;
            for (auto* v : { &one, &roll }) { v->setSampleRate(sr); apply(*v, CymbalPresets::ride); }
            std::vector<float> single, rolled;
            one.noteOn(0.8f);
            for (int i = 0; i < 72000; ++i)
            {
                if (i % 6000 == 0 && i < 48000) roll.noteOn(0.8f);
                single.push_back(one.process());
                rolled.push_back(roll.process());
            }
            // (0.4 s after a stroke the ping has gone and the wash is what is left: after the eighth
            // stroke, at 0.875 s, against after the only one.)
            const double washOne = rms(single, 0.4, 0.5, sr), washRoll = rms(rolled, 1.275, 1.375, sr);
            expect(washRoll > 1.25 * washOne && washRoll < 3.0 * washOne, "eight strokes: the wash is x" + juce::String(washRoll / washOne, 2) + " that of one");
            expect(finite(rolled, 2.0f));
            // No two strokes of the roll are the same samples (each finds the cymbal in another state).
            expect(difference(std::vector<float>(rolled.begin() + 12000, rolled.begin() + 14000),
                              std::vector<float>(rolled.begin() + 18000, rolled.begin() + 20000)) > 0.1);
            // A soft stroke after a hard one does not turn the hard one's tail down.
            DrumVoice hardSoft;
            hardSoft.setSampleRate(sr);
            Sound mallet = CymbalPresets::crash;
            mallet.snap = 0.0f;
            apply(hardSoft, mallet);
            std::vector<float> hs;
            hardSoft.noteOn(1.0f);
            for (int i = 0; i < 12000; ++i) hs.push_back(hardSoft.process());
            hardSoft.noteOn(0.1f);
            for (int i = 0; i < 2400; ++i) hs.push_back(hardSoft.process());
            expect(rms(hs, 0.25, 0.30, sr) > 0.7 * rms(hs, 0.20, 0.25, sr), "the tail carries on under a soft stroke");
        }

        beginTest("In the kit: choke groups, pad pressure, pitch bend, LFOs and the mod strip reach it");
        {
            // The starting kit's hats are a choke group: a cymbal machine on the open hat's pad is cut by the closed hat.
            DrumEngine e;
            e.prepareToPlay(sr, 64);
            e.setParameter(15 * 8 + D::TYPE, (float) (int) DrumSynthType::CymbalSynth);
            e.setParameter(15 * 8 + D::DECAY, 1.5f);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            auto run = [&](double seconds, std::vector<float>* out = nullptr)
            {
                for (int b = 0; b < (int) (seconds * sr) / 64; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    if (out != nullptr)
                        for (int i = 0; i < 64; ++i) out->push_back(buf.getSample(0, i) + buf.getSample(1, i));
                }
            };
            e.noteOn(15, 1.0f);
            e.noteOff(15);                       // a tap: a one-shot, it rings on
            run(0.3);
            expect(e.getVoice(15).isActive(), "letting the pad go does not stop it");
            e.setParameter(14 * 8 + D::LEVEL, 0.0f);
            e.noteOn(14, 1.0f);
            run(0.5);
            expect(! e.getVoice(15).isActive(), "the closed hat chokes it");

            // Pad pressure (the kit's default: DEC) lets it ring on; on TUNE it bends the ringing cymbal up.
            auto pressed = [&](DrumATDest dest, bool squeeze)
            {
                DrumEngine k;
                k.prepareToPlay(sr, 64);
                CymbalPresets::ParamValues p;
                CymbalPresets::set(p, 0, CymbalPresets::rideBell, 0.5f);
                for (auto& [id, value] : p) k.setParameter(id, value);
                k.setParameter(D::DECAY, 0.3f);
                k.setSlotAftertouchDest(0, dest);
                k.setSlotAftertouchDepth(0, 1.0f);
                juce::AudioBuffer<float> b2(2, 64);
                std::vector<float> out;
                k.noteOn(0, 1.0f);
                for (int b = 0; b < 1500; ++b)
                {
                    if (squeeze && b == 15) k.aftertouch(0, 1.0f);
                    b2.clear();
                    k.processBlock(b2, midi);
                    for (int i = 0; i < 64; ++i) out.push_back(b2.getSample(0, i));
                }
                return out;
            };
            const auto free = pressed(DrumATDest::Decay, false), held = pressed(DrumATDest::Decay, true);
            expect(rms(held, 1.0, 1.5, sr) > 3.0 * rms(free, 1.0, 1.5, sr), "squeezed, it rings on");
            auto strongestAt = [&](const std::vector<float>& x, double seconds)
            {
                std::vector<double> powers;
                const auto at = partials(x, sr, seconds, &powers);
                return at.empty() ? 0.0 : at[(size_t) (std::max_element(powers.begin(), powers.end()) - powers.begin())];
            };
            const auto up = pressed(DrumATDest::Pitch, true);
            expectWithinAbsoluteError(strongestAt(up, 0.3) / strongestAt(free, 0.3), 2.0, 0.05, "pressure on TUNE: an octave up while it rings");

            // The pitch strip bends the ringing cymbal (two semitones up at the top).
            DrumEngine bend;
            bend.prepareToPlay(sr, 64);
            {
                CymbalPresets::ParamValues p;
                CymbalPresets::set(p, 0, CymbalPresets::rideBell, 0.5f);
                for (auto& [id, value] : p) bend.setParameter(id, value);
            }
            std::vector<float> bent;
            bend.noteOn(0, 1.0f);
            juce::AudioBuffer<float> b3(2, 64);
            for (int b = 0; b < 1200; ++b)
            {
                if (b == 30) bend.pitchBend(1.0f);
                b3.clear();
                bend.processBlock(b3, midi);
                for (int i = 0; i < 64; ++i) bent.push_back(b3.getSample(0, i));
            }
            expectWithinAbsoluteError(12.0 * std::log2(strongestAt(bent, 0.3) / strongestAt(free, 0.3)), 2.0, 0.1, "pitch strip: two semitones");

            // An LFO on TONE moves its brightness while it sounds; the strip's default route (DEC) lengthens it.
            auto withStrip = [&](float strip)
            {
                DrumEngine k;
                CymbalPresets::ParamValues p;
                CymbalPresets::set(p, 0, CymbalPresets::crash, 0.5f);
                for (auto& [id, value] : p) k.setParameter(id, value);
                k.setParameter(D::DECAY, 0.3f);
                k.prepareToPlay(sr, 64);
                k.modWheel(strip);
                return renderPad(k, 0, 2.0);
            };
            const auto rest = withStrip(0.0f), lifted = withStrip(1.0f);
            expect(rms(lifted, 1.0, 1.5, sr) > 2.0 * rms(rest, 1.0, 1.5, sr), "the mod strip (DEC) lengthens it");
        }

        beginTest("The other fifteen types do not hear the CYM page");
        {
            for (int type = 0; type < (int) DrumSynthType::CymbalSynth; ++type)
            {
                DrumVoice a, b;
                for (auto* v : { &a, &b }) { v->setSampleRate(44100.0); v->setType((DrumSynthType) type); v->setTune(200.0f); }
                for (int n = 0; n < M::NumParams; ++n)
                    b.setCymbalParam(n, n == 0 ? 7.0f : 0.93f);
                a.noteOn(1.0f); b.noteOn(1.0f);
                bool identical = true;
                for (int i = 0; i < 4410; ++i) identical = identical && a.process() == b.process();
                expect(identical, juce::String(D::typeNames()[type]));
            }
        }

        // ================================================================== locks, projects, screens, kits
        beginTest("A cell is stored with the track, locked on a step and returned afterwards");
        {
            Rig rig;
            const int pad = 9;
            const int typeId = pad * 8 + D::TYPE, bloomId = D::CYMBAL_BASE + pad * 8 + M::ParBloom, modelId = D::CYMBAL_BASE + pad * 8 + M::ParModel;
            rig.audio.setTrackParameter(0, typeId, (float) (int) DrumSynthType::CymbalSynth);
            rig.audio.setTrackParameter(0, modelId, (float) M::Crash);
            rig.audio.setTrackParameter(0, bloomId, 0.25f);
            expectWithinAbsoluteError(rig.drums().getParameter(bloomId), 0.25f, 1.0e-6f, "it reaches the engine through the track");
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, bloomId), 0.25f, 1.0e-6f);

            auto& locked = rig.track().getStep(2);
            locked.paramLocks[bloomId] = 0.9f;
            locked.paramLocks[modelId] = (float) M::TamTam;
            rig.audio.applyStepLocks(0, locked);
            expectWithinAbsoluteError(rig.drums().getParameter(bloomId), 0.9f, 1.0e-6f, "the lock is applied");
            expectWithinAbsoluteError(rig.drums().getParameter(modelId), (float) M::TamTam, 1.0e-6f);
            rig.audio.applyStepLocks(0, rig.track().getStep(3));
            expectWithinAbsoluteError(rig.drums().getParameter(bloomId), 0.25f, 1.0e-6f, "and gone on the next step");
            expectWithinAbsoluteError(rig.drums().getParameter(modelId), (float) M::Crash, 1.0e-6f);
        }

        beginTest("A project keeps the type and every cell of the page, and the sound");
        {
            Rig rig;
            CymbalPresets::ParamValues sound;
            CymbalPresets::set(sound, 9, CymbalPresets::houseRide, 0.6f);
            CymbalPresets::set(sound, 3, CymbalPresets::tamTam, 0.5f);
            for (auto& [id, value] : sound)
                rig.audio.setTrackParameter(0, id, value);
            rig.track().getStep(5).paramLocks[D::CYMBAL_BASE + 9 * 8 + M::ParLofi] = 0.8f;

            const auto data = ProjectData::fromJSON(ProjectBridge::capture({ rig.seq, rig.audio }, "CYMBALS").toJSON());
            Rig back;
            ProjectBridge::apply(data, { back.seq, back.audio });
            for (auto& [id, value] : sound)
                expectWithinAbsoluteError(back.audio.getTrackParameter(0, id), value, 1.0e-4f, "id " + juce::String(id));
            expect(back.drums().isCymbalMachine(9) && back.drums().isCymbalMachine(3));
            expect(back.track().getStep(5).paramLocks.count(D::CYMBAL_BASE + 9 * 8 + M::ParLofi) == 1, "the lock too");
            const auto before = renderPad(rig.drums(), 9, 0.5), after = renderPad(back.drums(), 9, 0.5);
            expect(rms(before, 0.0, 0.5, sr) > 0.01 && difference(before, after) < 1.0e-3, "the reloaded pad is the same sound");
        }

        beginTest("DRUMS and PARAM show the CYM page for a pad whose TYPE is CSYN, and their knobs edit it");
        {
            DrumEngine engine;
            DrumPadScreen screen(&engine);
            screen.onActivate();
            screen.selectPad(3);
            expectEquals(screen.getPageCount(), 1, "an ordinary pad has one page");
            engine.setParameter(3 * 8 + D::TYPE, (float) (int) DrumSynthType::CymbalSynth);
            expectEquals(screen.getPageCount(), 2);
            expectEquals(screen.getSubPage(), juce::String("P4 CSYN"));
            screen.onArrowRight();
            expectEquals(screen.getPageIndex(), 1);
            const char* shortNames[8] = { "MD", "PG", "CL", "NS", "DP", "BL", "VS", "LF" };
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(screen.getEncoderName(i), juce::String(shortNames[i]));
                expect(screen.getEncoderReadout(i).isNotEmpty());
                const int id = D::CYMBAL_BASE + 3 * 8 + i;
                const float was = engine.getParameter(id);
                screen.onEncoderChange(i, 3);
                expect(engine.getParameter(id) > was, juce::String(shortNames[i]) + " turns");
            }
            expect(screen.getEncoderReadout(0).startsWith("MODEL "), screen.getEncoderReadout(0));
            screen.onArrowRight();
            expectEquals(screen.getPageIndex(), 0, "and back to the pad's own page");
            screen.onArrowRight();
            screen.selectPad(8);                    // the kick machine: its own second page, not the cymbal's
            expectEquals(screen.getEncoderName(0), juce::String("SW"));
            screen.selectPad(0);
            expectEquals(screen.getPageIndex(), 0, "an ordinary pad shows its one page");
            auto same = [](Screen::PadColour a, Screen::PadColour b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
            expect(same(DrumPadScreen::typeColour(DrumSynthType::CymbalSynth), DrumPadScreen::typeColour(DrumSynthType::Cymbal)), "cymbals are blue");

            // PARAM: DR n, then CYM, then the kit's STRIP and LFO pages.
            Rig rig;
            ParamPageScreen param(rig.seq, &rig.audio);
            rig.drums().setSelectedVoice(5);
            const int plain = param.getPageCount();
            rig.audio.setTrackParameter(0, 5 * 8 + D::TYPE, (float) (int) DrumSynthType::CymbalSynth);
            expectEquals(param.getPageCount(), plain + 1);
            juce::StringArray names;
            for (int i = 0; i < param.getPageCount(); ++i)
            {
                param.setCurrentPageIndex(i);
                names.add(param.getPageName());
            }
            const int at = names.indexOf("CYM");
            expect(at > 0 && names[at - 1] == "DR 6" && names[at + 1] == "STRIP", names.joinIntoString(" "));
            param.setCurrentPageIndex(at);
            const char* labels[8] = { "MODL", "PING", "COLR", "NOIS", "DAMP", "BLOM", "VSNS", "LOFI" };
            for (int i = 0; i < 8; ++i)
                expectEquals(param.getEncoderName(i), juce::String(labels[i]));
            const int pingId = param.getCurrentPage().params[1].id;
            expectEquals(pingId, D::CYMBAL_BASE + 5 * 8 + M::ParPing);
            param.onEncoderChange(1, 5);
            expect(rig.drums().getParameter(pingId) > 0.5f, "PING reaches the engine");
            expect(rig.track().soundParams.count(pingId) == 1, "and is stored with the sound");
            rig.drums().setSelectedVoice(1);        // an ordinary pad: no CYM page
            expectEquals(param.getPageCount(), plain);
        }

        beginTest("Two factory kits carry the ready-made sounds");
        {
            const auto presets = FactoryPresets::getAll();
            auto find = [&](const char* name) -> const Preset*
            {
                for (auto& p : presets)
                    if (p.name == name) return &p;
                return nullptr;
            };
            const Preset* works = find(CymbalPresets::worksName);
            const Preset* house = find(CymbalPresets::houseName);
            expect(works != nullptr && house != nullptr);
            if (works == nullptr || house == nullptr) return;
            for (auto* preset : { works, house })
            {
                expect(preset->engineType == EngineType::Drum && preset->category == "KIT" && preset->isFactory);
                DrumEngine e;
                for (auto& [id, value] : preset->soundParams)
                {
                    e.setParameter(id, value);
                    expectWithinAbsoluteError(e.getParameter(id), value, 1.0e-4f, preset->name + ": id " + juce::String(id) + " is not a cell");
                }
                int machines = 0;
                for (int pad = 0; pad < D::NUM_VOICES; ++pad)
                {
                    if (! e.isCymbalMachine(pad))
                        continue;
                    ++machines;
                    expectEquals(e.getChokeGroup(pad), 0, preset->name + ": no cymbal on the hats' pads (they cut each other off)");
                    DrumEngine fresh;
                    for (auto& [id, value] : preset->soundParams) fresh.setParameter(id, value);
                    const auto x = renderPad(fresh, pad, 0.5);
                    expect(rms(x, 0.0, 0.5, sr) > 0.01 && finite(x, 1.5f), preset->name + " pad " + juce::String(pad + 1) + " sounds");
                }
                expectEquals(machines, preset == works ? 10 : 5);
            }
            // Cymbal Works: every ready-made sound once, on the pads the manual names.
            DrumEngine e;
            for (auto& [id, value] : works->soundParams) e.setParameter(id, value);
            const int models[16] = { -1, -1, -1, -1, M::Splash, M::Ring, M::Gong, M::TamTam, M::Metal, M::Ride, M::Ride, M::Bell, M::Crash, M::China, -1, -1 };
            for (int pad = 0; pad < 16; ++pad)
            {
                expect(e.isCymbalMachine(pad) == (models[pad] >= 0), "pad " + juce::String(pad + 1));
                if (models[pad] >= 0)
                    expectEquals((int) e.getParameter(D::CYMBAL_BASE + pad * 8), models[pad], "pad " + juce::String(pad + 1));
            }
            expect(e.getParameter(D::CYMBAL_BASE + 9 * 8 + M::ParLofi) > 0.3f, "bottom pad 2 is the house ride");
            // The starting kit: only top pads 7 and 8 are cymbal machines (its crash and ride).
            DrumEngine starting;
            for (int pad = 0; pad < 16; ++pad)
                expect(starting.isCymbalMachine(pad) == (pad == 6 || pad == 7), "pad " + juce::String(pad + 1));
        }
    }
};

static CymbalMachineTests cymbalMachineTests;
