#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <vector>
#include "fast_measure.h"
#include "mod_router_tests.h"
#include "old/OldMacroOscEngine.h"
#include "../src/engine/MacroOscEngine.h"
#include "../src/engine/AmpEnvelopeSection.h"
#include "../src/engine/ModRouter.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/PresetManager.h"

// -----------------------------------------------------------------------------
// "U4 Macro": the macro oscillator finished on the shared sections - BODY (VoiceBodySection.h),
// the per-note spread with its SEED (VoiceSpread.h, the ANLG page), the amp envelope's chain as a
// shared section (AmpEnvelopeSection.h) - against the engine as it was at 46fedbc
// (tests/old/OldMacroOscEngine.h: that header under another name).
//
//   pages and ids   ANLG 125-132 beside MACRO, BODY 133-140 after the filter; set and read back
//   the table       the 29 destinations that were there are where they were; BMIX .. BKTK after
//                   them; BOW hidden behind them
//   old against new every factory preset whose model draws no noise and the plain paths, bit for bit
//                   (48 kHz at blocks of 256, 100, 37; 44.1 kHz at 100; 96 kHz at 37); the envelope
//                   chain, ETIM / FTIM, stage curves, LFOs, strip, pressure, the bow's pressure route,
//                   RAND, the OSC source
//   the models' noise PLCK's burst, DUST's clicks, the whisper and the drum blend: drawn per note
//                   from the voice's generator (stream spread::Model). The draws themselves:
//                   uniform, a new one each note, repeated by SEED; the renders over many notes as
//                   loud and as bright as the old engine's
//   ANLG            each amount draws what it says, SEED repeats everything
//   BODY            off = the old voice; on, every cell is heard; it rings on after the note; it
//                   follows TUNE, bend and SLOP; a destination of the router
//   PLCK DUST BOW   each against the nearest BODY settings found (NEPTUNE_U4_MACRO_SEARCH=1, the
//                   Release binary, searches again and prints them): MATCH / CLOSE / LOST
//   the chain       AmpEnvelopeSection by itself: both policies
//   real time       no allocation while BODY and ANLG are on; finite at the ends
//
// NEPTUNE_U4_PRINT=1 prints the tables; NEPTUNE_U4_ATLAS=<folder> writes the PLCK / DUST / BOW pairs
// as WAV (mono, one file per preset: its eight notes one after the other); NEPTUNE_U4_MACRO_BENCH=1
// (Release binary) times old and new.
//
// THE VERDICTS (stated before the first comparison was run, 2026-11-22). MATCH: the same samples.
// CLOSE: all of
//   (a) the loudness of the whole render within 1 dB,
//   (b) the loudness over time (100 ms frames, where either is above -50 dB of the peak frame)
//       within 2 dB,
//   (c) the brightness (energy-weighted mean frequency) within 10 %,
//   (d) the noisiness (spectral flatness of the whole render, 50 Hz - 12 kHz) within 0.1,
//   (e) the impulsiveness (crest factor: peak over RMS of the whole render) within 3 dB,
//   (f) for the pitched models (PLCK, BOW): the strongest partial below 4 kHz within 3 %.
// (a) - (c) are the wave sequencer's rule; (d) - (f) were added for these three models, which a
// level and a centroid cannot tell from noise. LOST: anything else. The rule is shown to catch a
// deliberate loss ("the rule can fail").
// -----------------------------------------------------------------------------
class U4MacroTests : public juce::UnitTest
{
public:
    U4MacroTests() : UnitTest("U4 Macro") {}

    using M = MacroOscEngine;
    using V = MacroOscVoice;
    using O = OldMacroOscEngine;
    using R = mod::Router;
    static constexpr double SR = 48000.0;
    static constexpr int FB = M::FilterBase, SB = M::SpreadBase, BB = M::BodyBase;

    struct Stereo { std::vector<float> l, r; };
    enum Kind { On, Off, Pressure, Strip, Bend };
    struct Event { double at; Kind kind; int note; float value; };
    using Events = std::vector<Event>;
    using Setup = std::function<void(EngineBase&)>;

    // ---- rendering -----------------------------------------------------------------------------
    static void apply(EngineBase& e, const Event& ev)
    {
        switch (ev.kind)
        {
            case On:       e.noteOn(ev.note, ev.value, -1); break;
            case Off:      e.noteOff(ev.note, -1); break;
            case Pressure: e.aftertouch(ev.note, ev.value); break;
            case Strip:    e.setModWheel(ev.value); break;
            case Bend:     e.pitchBend(ev.value); break;
        }
    }

    // Events are applied at the first block that starts at or after their time.
    static Stereo render(EngineBase& e, const Events& events, double seconds, int block = 256, double sr = SR, bool prepare = true)
    {
        if (prepare)
            e.prepareToPlay(sr, block);
        const long total = (long) (seconds * sr);
        Stereo out;
        out.l.reserve((size_t) total);
        out.r.reserve((size_t) total);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        size_t next = 0;
        for (long pos = 0; pos < total; pos += block)
        {
            while (next < events.size() && events[next].at * sr <= (double) pos)
                apply(e, events[next++]);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
            {
                out.l.push_back(buf.getSample(0, i));
                out.r.push_back(buf.getSample(1, i));
            }
        }
        return out;
    }

    static Events phrase()
    {
        return { { 0.0, On, 48, 0.9f }, { 0.4, On, 60, 0.7f }, { 0.4, On, 64, 0.6f }, { 0.9, Off, 48, 0.0f },
                 { 1.4, On, 67, 1.0f }, { 2.0, Off, 60, 0.0f }, { 2.0, Off, 64, 0.0f }, { 2.6, Off, 67, 0.0f },
                 { 2.7, On, 55, 0.8f }, { 2.9, On, 55, 0.5f }, { 3.3, Off, 55, 0.0f } };
    }
    static Events oneNote(double length = 1.5, int note = 57, float velocity = 0.8f)
    {
        return { { 0.0, On, note, velocity }, { length, Off, note, 0.0f } };
    }

    // ---- measuring -----------------------------------------------------------------------------
    static bool same(const Stereo& a, const Stereo& b)
    {
        return a.l.size() == b.l.size() && std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0
            && std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
    }
    static std::vector<float> mono(const Stereo& s)
    {
        std::vector<float> m(s.l.size());
        for (size_t i = 0; i < m.size(); ++i)
            m[i] = 0.5f * (s.l[i] + s.r[i]);
        return m;
    }
    static double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        to = std::min(to, x.size());
        if (to <= from) return 0.0;
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) x[i] * (double) x[i];
        return std::sqrt(sum / (double) (to - from));
    }
    static double rms(const std::vector<float>& x) { return rms(x, 0, x.size()); }
    static double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }
    static bool finite(const Stereo& s, float bound = 16.0f)
    {
        for (auto* v : { &s.l, &s.r })
            for (float x : *v)
                if (! std::isfinite(x) || std::abs(x) > bound)
                    return false;
        return true;
    }

    // The long-term power spectrum (fastmeasure::frames gives sqrt |X| per bin, hop of half a frame).
    static std::vector<double> spectrum(const std::vector<float>& x, int fftSize = 4096)
    {
        std::vector<double> p((size_t) fftSize / 2, 0.0);
        if ((int) x.size() < fftSize)
            return p;
        for (auto& f : fastmeasure::frames(x, fftSize))
            for (size_t k = 0; k < std::min(p.size(), f.size()); ++k)
            {
                const double m = (double) f[k] * (double) f[k];
                p[k] += m * m;
            }
        return p;
    }
    // Spectral flatness over 50 Hz - 12 kHz: 1 = white noise, near 0 = a few partials.
    static double flatness(const std::vector<double>& p, double sr, int fftSize = 4096)
    {
        const double bin = sr / fftSize;
        double logSum = 0.0, sum = 0.0;
        int n = 0;
        for (size_t k = (size_t) std::ceil(50.0 / bin); k < p.size() && (double) k * bin <= 12000.0; ++k)
        {
            const double v = std::max(p[k], 1.0e-30);
            logSum += std::log(v);
            sum += v;
            ++n;
        }
        return n > 0 && sum > 0.0 ? std::exp(logSum / n) / (sum / n) : 0.0;
    }
    // The strongest partial below 4 kHz, Hz (parabolic between bins).
    static double strongest(const std::vector<double>& p, double sr, int fftSize = 4096)
    {
        const double bin = sr / fftSize;
        size_t best = (size_t) std::ceil(25.0 / bin);
        for (size_t k = best; k + 1 < p.size() && (double) k * bin < 4000.0; ++k)
            if (p[k] > p[best])
                best = k;
        const double a = std::sqrt(p[best - 1]), b = std::sqrt(p[best]), c = std::sqrt(p[std::min(best + 1, p.size() - 1)]);
        const double d = a - 2.0 * b + c;
        const double shift = std::abs(d) > 1.0e-30 ? 0.5 * (a - c) / d : 0.0;
        return ((double) best + std::clamp(shift, -0.5, 0.5)) * bin;
    }
    static std::vector<float> dcBlocked(std::vector<float> x, double sr)
    {
        const float k = (float) std::exp(-2.0 * 3.141592653589793 * 20.0 / sr);
        float xPrev = 0.0f, y = 0.0f;
        for (auto& v : x)
        {
            y = v - xPrev + k * y;
            xPrev = v;
            v = y;
        }
        return x;
    }
    static double crest(const std::vector<float>& x)
    {
        double peak = 0.0;
        for (float v : x)
            peak = std::max(peak, (double) std::abs(v));
        return db(peak / std::max(rms(x), 1.0e-12));
    }

    struct Compared
    {
        bool identical = false;
        double level = 0.0;         // (a) dB, new against old, the whole render
        double curve = 0.0;         // (b) dB, the largest difference of a 100 ms frame
        double bright = 0.0;        // (c) new against old - 1
        double flat = 0.0;          // (d) the difference of the spectral flatness
        double crest = 0.0;         // (e) dB, the difference of the crest factors
        double pitch = 0.0;         // (f) the strongest partial, new against old - 1
        double residual = 0.0;      // dB: the difference signal against the old one (not in the verdict)
        bool pitched = false;
    };
    static Compared compare(const Stereo& older, const Stereo& newer, double sr = SR, bool pitched = false)
    {
        Compared c;
        c.pitched = pitched;
        c.identical = same(older, newer);
        // (Measured after a 20 Hz high-pass: the old string carries a slowly decaying offset that is
        // not heard and read as the "strongest partial" and as darkness. Added after the first
        // search; the rule's limits are as stated, and "the rule can fail" still holds.)
        const auto a = dcBlocked(mono(older), sr), b = dcBlocked(mono(newer), sr);
        c.level = db(rms(b) / std::max(rms(a), 1.0e-9));
        {
            double diff = 0.0, ref = 0.0;
            for (size_t i = 0; i < std::min(older.l.size(), newer.l.size()); ++i)
            {
                diff += (double) (newer.l[i] - older.l[i]) * (double) (newer.l[i] - older.l[i]) + (double) (newer.r[i] - older.r[i]) * (double) (newer.r[i] - older.r[i]);
                ref += (double) older.l[i] * (double) older.l[i] + (double) older.r[i] * (double) older.r[i];
            }
            c.residual = ref > 0.0 ? std::max(-200.0, 10.0 * std::log10(std::max(diff / ref, 1.0e-20))) : 0.0;
        }
        const size_t frame = (size_t) (0.1 * sr);
        double peak = 0.0;
        for (size_t f = 0; f + frame <= a.size(); f += frame)
            peak = std::max({ peak, rms(a, f, f + frame), rms(b, f, f + frame) });
        for (size_t f = 0; f + frame <= a.size(); f += frame)
        {
            const double x = rms(a, f, f + frame), y = rms(b, f, f + frame);
            if (std::max(x, y) > peak * 0.00316)
                c.curve = std::max(c.curve, std::abs(db(y / std::max(x, 1.0e-9))));
        }
        if (c.identical)
            return c;
        float ra = 0, ca = 0, rb = 0, cb = 0;
        fastmeasure::rmsAndCentroid(a, 2048, sr, ra, ca);
        fastmeasure::rmsAndCentroid(b, 2048, sr, rb, cb);
        c.bright = ca > 0.0f ? (double) cb / (double) ca - 1.0 : 0.0;
        const auto pa = spectrum(a), pb = spectrum(b);
        c.flat = flatness(pb, sr) - flatness(pa, sr);
        c.crest = crest(b) - crest(a);
        if (pitched)
        {
            const double fa = strongest(pa, sr), fb = strongest(pb, sr);
            c.pitch = fa > 0.0 ? fb / fa - 1.0 : 0.0;
        }
        return c;
    }
    static bool close(const Compared& c)
    {
        return std::abs(c.level) <= 1.0 && c.curve <= 2.0 && std::abs(c.bright) <= 0.10 && std::abs(c.flat) <= 0.1
            && std::abs(c.crest) <= 3.0 && (! c.pitched || std::abs(c.pitch) <= 0.03);
    }
    static const char* verdict(const Compared& c)
    {
        if (c.identical) return "MATCH";
        return close(c) ? "CLOSE" : "LOST";
    }
    // How far past the CLOSE limits a comparison is: the largest of the six measures over its limit
    // (1 = at a limit; below 1 = CLOSE).
    static double over(const Compared& c)
    {
        return std::max({ std::abs(c.level) / 1.0, c.curve / 2.0, std::abs(c.bright) / 0.10, std::abs(c.flat) / 0.1,
                          std::abs(c.crest) / 3.0, c.pitched ? std::abs(c.pitch) / 0.03 : 0.0 });
    }
    static juce::String describe(const Compared& c)
    {
        return juce::String::formatted("%-5s | level %+6.2f dB | over time %5.2f dB | bright %+6.1f %% | flat %+5.2f | crest %+5.1f dB | pitch %+6.1f %% | residual %6.1f dB",
                                       verdict(c), c.level, c.curve, 100.0 * c.bright, c.flat, c.crest, 100.0 * c.pitch, c.residual);
    }

    static bool printing() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_PRINT", "").isNotEmpty(); }
    static juce::String atlas() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_ATLAS", ""); }

    static void writeWav(const juce::File& file, const std::vector<float>& m)
    {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (! out.openedOk()) return;
        const int frames = (int) m.size(), bytes = frames * 2;
        out.write("RIFF", 4); out.writeInt(36 + bytes); out.write("WAVEfmt ", 8);
        out.writeInt(16); out.writeShort(1); out.writeShort(1); out.writeInt((int) SR); out.writeInt((int) SR * 2);
        out.writeShort(2); out.writeShort(16); out.write("data", 4); out.writeInt(bytes);
        for (float v : m)
            out.writeShort((short) std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    // The same setup and the same gesture on the old engine and on the new one.
    Compared against(const juce::String& name, const Setup& setUp, const Events& events, double seconds,
                     int block = 256, double sr = SR)
    {
        auto older = std::make_unique<O>();
        auto newer = std::make_unique<M>();
        if (setUp) { setUp(*older); setUp(*newer); }
        const Stereo a = render(*older, events, seconds, block, sr), b = render(*newer, events, seconds, block, sr);
        const Compared c = compare(a, b, sr);
        if (printing())
            logMessage("U4-MACRO | " + name.paddedRight(' ', 44) + " | " + describe(c));
        expect(finite(b), name + ": finite and bounded");
        return c;
    }
    void expectMatch(const juce::String& name, const Setup& setUp, const Events& events, double seconds, int block = 256, double sr = SR)
    {
        const Compared c = against(name, setUp, events, seconds, block, sr);
        expect(c.identical, name + " at " + juce::String(sr / 1000.0, 1) + " kHz, blocks of " + juce::String(block) + ": " + describe(c));
    }

    // ---- setting up ----------------------------------------------------------------------------
    static void lfo(EngineBase& e, int i, int dest, float depth, float hz = 5.0f, int shape = 0, int trigger = 1)
    {
        e.setParameter(R::lfoId(i, R::Rate), hz);
        e.setParameter(R::lfoId(i, R::Shape), (float) shape);
        e.setParameter(R::lfoId(i, R::Trigger), (float) trigger);
        e.setParameter(R::lfoId(i, R::Sync), 0.0f);
        e.setParameter(R::lfoId(i, R::Fade), 0.0f);
        e.setParameter(R::lfoId(i, R::Depth), depth);
        e.setParameter(R::lfoId(i, R::DestCell), (float) dest);
    }
    static void slot(EngineBase& e, int k, int src, int dest, float amount, int via = 0)
    {
        e.setParameter(ModSlotParam::id(k, ModSlotParam::SRC), (float) src);
        e.setParameter(ModSlotParam::id(k, ModSlotParam::DEST), (float) dest);
        e.setParameter(ModSlotParam::id(k, ModSlotParam::AMT), amount);
        e.setParameter(ModSlotParam::id(k, ModSlotParam::VIA), (float) via);
    }
    static int place(const char* source) { return M::sources().indexOfLabel(source); }
    static int dest(const char* label, int model = 0) { return M::lfoDests(model).indexOfLabel(label); }
    static void openFilter(EngineBase& e, float hz = 1200.0f)
    {
        e.setParameter(FB + VoiceFilterSection::Cutoff, hz);
        e.setParameter(FB + VoiceFilterSection::Resonance, 0.3f);
    }
    static Setup preset(const Preset& p)
    {
        return [p](EngineBase& e)
        {
            for (auto& [id, value] : PresetManager::soundOf(p))
                e.setParameter(id, value);
        };
    }
    static std::vector<Preset> factoryPresets()
    {
        std::vector<Preset> out;
        for (auto& p : FactoryPresets::getAll())
            if (p.engineType == EngineType::MacroOsc)
                out.push_back(p);
        return out;
    }
    static float cell(const Preset& p, int id, float otherwise)
    {
        const auto sound = PresetManager::soundOf(p);
        const auto it = sound.find(id);
        return it != sound.end() ? it->second : otherwise;
    }
    static int modelOf(const Preset& p) { return (int) cell(p, M::ModelParam, 0.0f); }
    // A preset whose model draws noise: PLCK (its burst), DUST, and FORM or PLCK with MORP up.
    static bool drawsNoise(const Preset& p)
    {
        const int m = modelOf(p);
        return m == 5 || m == 6 || (m == 3 && cell(p, M::MorphParam, 0.0f) > 0.0f);
    }
    static V* sounding(M& e)
    {
        for (int v = 0; v < 8; ++v)
            if (e.getVoice(v).isActive())
                return &e.getVoice(v);
        return nullptr;
    }
    static void block(EngineBase& e, int n = 256)
    {
        static juce::AudioBuffer<float> buf(2, 512);      // (made once: no allocation while counting)
        static juce::MidiBuffer midi;
        buf.setSize(2, n, false, false, true);
        buf.clear();
        e.processBlock(buf, midi);
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_MACRO_BENCH", "").isNotEmpty())
        {
            bench();
            return;
        }
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_MACRO_SEARCH", "").isNotEmpty())
        {
            search();
            return;
        }
        testPagesAndIds();
        testTable();
        testFactoryPresets();
        testPlainPaths();
        testOtherRates();
        testModelNoise();
        testSpread();
        testSeed();
        testBody();
        testRuleCanFail();
        testFoldedModels();
        testEnvelopeSection();
        testRealTime();
        testReview();
    }

    // ============================================================================================
    void testPagesAndIds()
    {
        beginTest("Pages: MACRO, ANLG, AMP, the filter's, BODY, then the router's; ids 125-140 set and read back");
        M e;
        const auto pages = e.getParamPages();
        expect(pages.size() > 5);
        expectEquals(juce::String(pages[0].name), juce::String("MACRO"));
        expectEquals(juce::String(pages[1].name), juce::String("ANLG"));
        expectEquals(juce::String(pages[2].name), juce::String("AMP"));
        int body = -1, firstRouter = -1;
        for (int i = 0; i < (int) pages.size(); ++i)
        {
            if (juce::String(pages[(size_t) i].name) == "BODY") body = i;
            for (auto& p : pages[(size_t) i].params)
                if (p.isValid() && R::owns(p.id) && firstRouter < 0) firstRouter = i;
        }
        expect(body > 2 && firstRouter == body + 2, "BODY last of the engine's own pages (+ BDY2), before the router's");
        for (int c = 0; c < 8; ++c)
        {
            expectEquals(pages[1].params[(size_t) c].id, SB + c);
            expectEquals(pages[(size_t) body].params[(size_t) c].id, BB + c);
        }
        // Every cell of the engine's own pages: outside the router's blocks, and read back as set.
        for (auto& page : pages)
            for (auto& p : page.params)
            {
                if (! p.isValid() || R::owns(p.id))
                    continue;
                expect(! (p.id >= 158 && p.id <= 189) && ! (p.id >= 700 && p.id <= 779), juce::String(p.label) + " outside the router's ids");
                if (p.id == M::ModelParam)
                    continue;
                const float v = p.kind == ParamKind::Choice || p.kind == ParamKind::Int ? std::round(0.5f * (p.minVal + p.maxVal))
                                                                                         : p.minVal + 0.37f * (p.maxVal - p.minVal);
                e.setParameter(p.id, v);
                expectWithinAbsoluteError(e.getParameter(p.id), v, 1.0e-4f, juce::String(p.label) + " read back");
            }
        expectEquals(e.getParameterName(SB + VoiceSpread::Seed), juce::String("Spread Seed"));
        expectEquals(e.getParameterName(BB + VoiceBody::Mix), juce::String("Body Mix"));
        // Out of range: held.
        e.setParameter(SB + VoiceSpread::Seed, 1000.0f);
        expectEquals(e.getParameter(SB + VoiceSpread::Seed), 127.0f);
        e.setParameter(BB + VoiceBody::Decay, 1000.0f);
        expectEquals(e.getParameter(BB + VoiceBody::Decay), 20.0f);
        // The model keeps its eight choices (nothing was folded away: see "PLCK DUST BOW").
        expectEquals((int) V::Model::Count, 8);
    }

    void testTable()
    {
        beginTest("The table: the 29 destinations that were there are where they were; BODY's eight after them; BOW hidden");
        for (int m = 0; m < 8; ++m)
        {
            const auto& t = M::lfoDests(m);
            const auto& old = O::lfoDests(m);
            expectEquals(t.visible(), old.visible() + 16, "sixteen more (BODY + BDY2)");
            for (int i = 0; i < old.visible(); ++i)
                expectEquals(juce::String(t[i].label), juce::String(old[i].label), "entry " + juce::String(i));
            for (int c = 0; c < 8; ++c)
            {
                expectEquals(juce::String(t[V::LfoBody + c].label), juce::String(VoiceBodySection::destLabel(c)));
                expectEquals(t[V::LfoBody + c].id, BB + c);
            }
            expectEquals(t.indexOfLabel("BOW"), (int) V::LfoBow);
            expect(t.visible() <= V::LfoBow, "BOW is not offered");
        }
        // The old route EXCITE is the bow here (what the engine's PressureMap said), and BOW is
        // what a pressure slot's DEST offers beside the table: one turn on AT ROUTE.
        {
            M engine;
            expectEquals(engine.pressureDestOf(AftertouchRouter::Destination::Excite), (int) V::LfoBow);
            const auto& t = M::lfoDests(0);
            expectEquals(juce::String(t.pressureLabels()[V::LfoBow]), juce::String("BOW"));
            expectEquals(juce::String(t.pressureMenu()[1]), juce::String("BOW"), "first after OFF on AT ROUTE");
            expectEquals(juce::String(t.pressureMenu()[2]), juce::String("VIB"));
        }
    }

    // ============================================================================================
    void testFactoryPresets()
    {
        beginTest("Old against new: every factory preset whose model draws no noise, bit for bit");
        int matched = 0, noisy = 0;
        for (auto& p : factoryPresets())
        {
            if (drawsNoise(p))
            {
                ++noisy;
                continue;
            }
            for (int block : { 256, 100, 37 })
                expectMatch(p.name, preset(p), phrase(), 4.0, block);
            ++matched;
        }
        expectEquals(matched, 23, "VA, FM, FOLD, HARM, BOW and FORM without the whisper");
        expectEquals(noisy, 11, "PLCK (5), DUST (5), Whisper Wind");
        expectMatch("Init (the engine as it is made)", nullptr, phrase(), 4.0);
    }

    void testPlainPaths()
    {
        beginTest("Old against new: the chain, envelope times, curves, LFOs, strip, pressure, the bow's route, RAND, OSC: bit for bit");
        struct Case { const char* name; Setup setUp; Events events; };
        auto with = [](Events ev, std::initializer_list<Event> more) { for (auto& m : more) ev.push_back(m); std::sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.at < b.at; }); return ev; };
        const int RAND = place("RAND"), ONE = place("ONE"), OSC = place("OSC"), ENV = place("ENV"), RND2 = place("RND2");
        const std::vector<Case> cases = {
            { "every model at its defaults", [](EngineBase&) {}, phrase() },
            { "CHAN SER, 3 stages, SUM", [](EngineBase& e) { e.setParameter(M::EnvChainMode, 1); e.setParameter(M::EnvChainStages, 3); e.setParameter(M::AmpRelease, 0.6f); }, phrase() },
            { "CHAN PAR, 4 stages, MAX, then ATK", [](EngineBase& e) { e.setParameter(M::EnvChainMode, 2); e.setParameter(M::EnvChainStages, 4); e.setParameter(M::EnvChainCombine, 1); e.setParameter(M::AmpAttack, 0.2f); e.setParameter(M::DecayParam, 0.7f); }, phrase() },
            { "CHAN on before STGS, then off", [](EngineBase& e) { e.setParameter(M::EnvChainStages, 2); e.setParameter(M::EnvChainMode, 2); e.setParameter(M::EnvChainMode, 0); }, phrase() },
            { "stage curves on ATK and REL, chain SER", [](EngineBase& e) { e.setParameter(M::EnvChainMode, 1); e.setParameter(StageCurveParam::id(0, StageCurveParam::Attack), 0.7f); e.setParameter(StageCurveParam::id(0, StageCurveParam::Release), -0.5f); }, phrase() },
            { "ONE > ETIM, chain PAR", [=](EngineBase& e) { e.setParameter(M::EnvChainMode, 2); slot(e, 0, ONE, V::LfoEnvTime, 0.4f); }, phrase() },
            { "ENV > FTIM, filter envelope", [=](EngineBase& e) { openFilter(e, 600.0f); e.setParameter(FB + VoiceFilterSection::EnvAmount, 0.6f); slot(e, 0, ENV, V::LfoFilterTime, -0.5f); }, phrase() },
            { "LFO1 > ATK and DEC (one voice's envelope), chain SER", [](EngineBase& e) { e.setParameter(M::EnvChainMode, 1); lfo(e, 0, dest("ATK"), 0.5f, 3.0f); lfo(e, 1, dest("DEC"), 0.5f, 2.0f); }, phrase() },
            { "LFO1 > TIMBRE, LFO2 > PAN (FM)", [](EngineBase& e) { e.setParameter(M::ModelParam, 1); lfo(e, 0, V::LfoTimbre, 0.5f, 4.0f); lfo(e, 1, V::LfoPan, 0.7f, 1.0f); }, phrase() },
            { "strip up (default routes)", [](EngineBase& e) { e.setParameter(M::ModelParam, 2); }, with(phrase(), { { 0.5, Strip, 0, 0.8f }, { 2.0, Strip, 0, 0.2f } }) },
            { "pressure (pad VOL), bend", [](EngineBase& e) { e.setParameter(M::ModelParam, 4); }, with(phrase(), { { 0.6, Pressure, 60, 0.7f }, { 1.0, Bend, 0, 0.5f }, { 2.4, Bend, 0, -1.0f } }) },
            { "RAND > PAN, RND2 > CUT (the randoms at SEED 0)", [=](EngineBase& e) { openFilter(e); slot(e, 0, RAND, V::LfoPan, 0.9f); slot(e, 1, RND2, V::LfoCutoff, 0.6f); }, phrase() },
            { "OSC > CUT (the voice's own source)", [=](EngineBase& e) { openFilter(e, 800.0f); slot(e, 0, OSC, V::LfoCutoff, 0.3f); }, phrase() },
            { "BOW with its pressure route", [](EngineBase& e) { e.setParameter(M::ModelParam, 7); }, with(phrase(), { { 0.6, Pressure, 60, 0.5f }, { 1.2, Pressure, 60, 0.0f }, { 1.5, Pressure, 67, 0.9f } }) },
            { "HARM, a note struck again under itself", [](EngineBase& e) { e.setParameter(M::ModelParam, 4); e.setParameter(M::MorphParam, 0.6f); },
              { { 0.0, On, 50, 0.8f }, { 0.3, On, 50, 0.4f }, { 0.6, Off, 50, 0.0f } } },
        };
        // The pressure route BOW (AT ROUTE: EXCITE) needs the track's router: both engines get one.
        for (auto& c : cases)
            for (int block : { 256, 100 })
                expectMatch(c.name, c.setUp, c.events, 4.0, block);
    }

    void testOtherRates()
    {
        beginTest("Old against new at 44.1 kHz (blocks of 100) and 96 kHz (blocks of 37): presets and plain paths, bit for bit");
        const auto presets = factoryPresets();
        const int ONE = place("ONE");
        for (double sr : { 44100.0, 96000.0 })
        {
            const int block = sr < 48000.0 ? 100 : 37;
            for (const char* name : { "Super Saw Lead", "FM Growl Bass", "Fold Sweep Pad", "Talking Lead", "Stiff Bell", "Solo Cello", "Glass Bow" })
                for (auto& p : presets)
                    if (p.name == name)
                        expectMatch(p.name, preset(p), phrase(), 3.5, block, sr);
            expectMatch("CHAN PAR 3, ONE > ETIM", [=](EngineBase& e) { e.setParameter(M::EnvChainMode, 2); e.setParameter(M::EnvChainStages, 3); slot(e, 0, ONE, V::LfoEnvTime, -0.3f); }, phrase(), 3.5, block, sr);
        }
    }

    // ============================================================================================
    // The models' noise. What changed: the seed of each noise was a function of the note (the dust's
    // ran on from 42 per voice); it is now a draw of the voice's generator (stream Model) at each
    // note. The noises themselves (an LCG, an xorshift) are the same code from another seed.
    void testModelNoise()
    {
        beginTest("The models' noise: seeds drawn per note - uniform, a new one each note, repeated by SEED");
        {
            M e;
            e.prepareToPlay(SR, 256);
            e.setParameter(M::ModelParam, 6);
            std::array<int, 16> topDust {}, topNoise {};
            std::map<uint32_t, int> seen;
            uint32_t lastDust = 0;
            int repeats = 0;
            const int notes = 2048;
            for (int n = 0; n < notes; ++n)
            {
                e.noteOn(60, 0.8f, -1);       // (the same note every time: the old seeds were the same)
                const V& v = e.getVoice(0);
                ++topDust[(size_t) (v.getDustSeed() >> 28)];
                ++topNoise[(size_t) (v.getNoiseSeed() >> 28)];
                repeats += v.getDustSeed() == lastDust ? 1 : 0;
                lastDust = v.getDustSeed();
                ++seen[v.getDustSeed()];
                e.allNotesOff();
            }
            auto chi = [notes](const std::array<int, 16>& h)
            {
                double x = 0.0;
                for (int c : h) x += (c - notes / 16.0) * (c - notes / 16.0) / (notes / 16.0);
                return x;
            };
            expect(chi(topDust) < 37.7 && chi(topNoise) < 37.7, "the top four bits uniform (chi-square, 15 dof, 0.1 %): "
                   + juce::String(chi(topDust), 1) + ", " + juce::String(chi(topNoise), 1));
            expectEquals(repeats, 0, "a new seed every note");
            expect((int) seen.size() > notes - 4, "and hardly ever one seen before");
        }
        {
            // SEED repeats them; another SEED gives others; the voices differ.
            auto seeds = [](int seed)
            {
                M e;
                e.prepareToPlay(SR, 256);
                e.setParameter(SB + VoiceSpread::Seed, (float) seed);
                std::vector<uint32_t> out;
                for (int n = 0; n < 16; ++n)
                {
                    e.noteOn(48 + n, 0.8f, -1);
                    for (int v = 0; v < 8; ++v)
                        out.push_back(e.getVoice(v).getNoiseSeed());
                }
                return out;
            };
            expect(seeds(3) == seeds(3), "SEED 3 twice: the same seeds");
            expect(seeds(3) != seeds(4), "SEED 4: others");
        }
        // Every noise of every model follows SEED, and the same note twice is not the same noise.
        for (auto [model, morph] : std::vector<std::pair<int, float>> { { 5, 0.0f }, { 5, 1.0f }, { 6, 0.6f }, { 3, 1.0f } })
        {
            auto run = [model = model, morph = morph](int seed)
            {
                M e;
                e.setParameter(M::ModelParam, (float) model);
                e.setParameter(M::MorphParam, morph);
                e.setParameter(M::AmpRelease, 0.01f);       // (the first note is over before the second)
                e.setParameter(SB + VoiceSpread::Seed, (float) seed);
                return render(e, { { 0.0, On, 57, 0.8f }, { 0.3, Off, 57, 0.0f }, { 0.4, On, 57, 0.8f }, { 0.7, Off, 57, 0.0f } }, 0.8);
            };
            const Stereo a = run(1), b = run(1), c = run(2);
            const size_t half = (size_t) (0.4 * SR);
            const std::vector<float> first(a.l.begin(), a.l.begin() + (long) half / 2), second(a.l.begin() + (long) half, a.l.begin() + (long) (half + half / 2));
            const juce::String what = "model " + juce::String(model) + ", MORP " + juce::String(morph, 1);
            expect(same(a, b) && ! same(a, c), what + ": SEED repeats the noise, another SEED changes it");
            expect(first != second, what + ": the same note again draws another noise");
        }
        // The renders over many notes: as loud and as bright, note for note, in the mean (the old
        // engine's seeds are fixed per note, the new ones change: the spread of each measure over
        // 32 notes must be alike, and so must its mean).
        for (auto& p : factoryPresets())
        {
            if (! drawsNoise(p))
                continue;
            auto stats = [&p](EngineBase& e)
            {
                preset(p)(e);
                std::vector<double> levels, brights;
                e.prepareToPlay(SR, 256);
                for (int n = 0; n < 32; ++n)
                {
                    const int note = 48 + (n * 7) % 24;
                    const Stereo s = render(e, oneNote(0.25, note, 0.8f), 0.35, 256, SR, false);
                    e.allNotesOff();
                    const auto m = mono(s);
                    float r = 0, c = 0;
                    fastmeasure::rmsAndCentroid(m, 1024, SR, r, c);
                    levels.push_back(db(rms(m)));
                    brights.push_back(c);
                }
                auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return s / (double) v.size(); };
                auto sd = [&](const std::vector<double>& v) { const double m = mean(v); double s = 0; for (double x : v) s += (x - m) * (x - m); return std::sqrt(s / (double) v.size()); };
                return std::array<double, 4> { mean(levels), sd(levels), mean(brights), sd(brights) };
            };
            O older;
            M newer;
            const auto a = stats(older), b = stats(newer);
            if (printing())
                logMessage(juce::String::formatted("U4-MACRO | noise %-16s | level %6.2f / %6.2f dB (sd %4.2f / %4.2f) | bright %6.0f / %6.0f Hz (sd %5.0f / %5.0f)",
                                                   p.name.toRawUTF8(), a[0], b[0], a[1], b[1], a[2], b[2], a[3], b[3]));
            const double dLevel = std::abs(b[0] - a[0]), dBright = std::abs(b[2] / std::max(a[2], 1.0) - 1.0);
            expect(dLevel < 0.75, p.name + ": the mean level of 32 notes within 0.75 dB of the old (" + juce::String(dLevel, 2) + ")");
            expect(dBright < 0.06, p.name + ": the mean brightness within 6 % (" + juce::String(100.0 * dBright, 1) + " %)");
            expect(b[1] < a[1] + 1.0 && b[3] < a[3] * 1.5 + 50.0, p.name + ": no wider spread from note to note");
        }
    }

    // ============================================================================================
    void testSpread()
    {
        beginTest("ANLG: each amount draws what it says");
        // Draws over 400 notes on one voice (the voice's state after each note-on).
        struct Draws { std::vector<double> cents, timbre, pan, gain, attackScale; };
        auto draw = [](int offset, float amount)
        {
            M e;
            e.prepareToPlay(SR, 256);
            e.setParameter(SB + offset, amount);
            Draws d;
            for (int n = 0; n < 400; ++n)
            {
                e.noteOn(60, 0.8f, -1);
                block(e, 32);
                V& v = e.getVoice(0);
                d.cents.push_back(1200.0 * std::log2(v.getFrequency() / dsp::midiNoteToHz(60)));
                d.timbre.push_back(v.getNoteTimbreOffset());
                d.pan.push_back(v.getPan());
                d.gain.push_back(v.getNoteGain());
                d.attackScale.push_back(v.getAmpEnvelope().getTimeScale());
                e.allNotesOff();
            }
            return d;
        };
        auto range = [](const std::vector<double>& v, double& lo, double& hi, double& mean, double& sd)
        {
            lo = 1e9; hi = -1e9; mean = 0.0;
            for (double x : v) { lo = std::min(lo, x); hi = std::max(hi, x); mean += x; }
            mean /= (double) v.size();
            sd = 0.0;
            for (double x : v) sd += (x - mean) * (x - mean);
            sd = std::sqrt(sd / (double) v.size());
        };
        double lo, hi, mean, sd;
        // Uniform on [-a, a]: sd a / sqrt 3; the ends nearly reached; the mean near 0.
        const auto slop = draw(VoiceSpread::Slop, 50.0f);
        range(slop.cents, lo, hi, mean, sd);
        expect(lo >= -50.01 && hi <= 50.01 && lo < -45.0 && hi > 45.0, "SLOP 50: within +-50 cents, both ends reached");
        expect(std::abs(sd - 28.87) < 3.0 && std::abs(mean) < 5.0, "SLOP: uniform (sd " + juce::String(sd, 2) + ", mean " + juce::String(mean, 2) + ")");
        const auto wave = draw(VoiceSpread::Wave, 1.0f);
        range(wave.timbre, lo, hi, mean, sd);
        expect(lo >= -0.5 && hi <= 0.5 && lo < -0.45 && hi > 0.45 && std::abs(sd - 0.2887) < 0.03, "WVAR 1: TIMBRE moved by up to half its range either way");
        const auto pan = draw(VoiceSpread::Pan, 1.0f);
        range(pan.pan, lo, hi, mean, sd);
        expect(lo >= -1.0 && hi <= 1.0 && lo < -0.9 && hi > 0.9 && std::abs(mean) < 0.1, "PVAR 1: each note's place, -1 .. 1");
        const auto gain = draw(VoiceSpread::Level, 0.5f);
        range(gain.gain, lo, hi, mean, sd);
        expect(lo >= 0.5 && hi <= 1.0 && lo < 0.53 && hi > 0.97, "LVAR 0.5: each note's gain 0.5 .. 1, never louder");
        const auto times = draw(VoiceSpread::EnvTime, 1.0f);
        range(times.attackScale, lo, hi, mean, sd);
        expect(lo >= 0.5 && hi <= 2.0 && lo < 0.55 && hi > 1.85, "EVAR 1: the envelope's times x 0.5 .. 2");
        // With the amounts at 0: nothing.
        const auto none = draw(VoiceSpread::Slop, 0.0f);
        range(none.cents, lo, hi, mean, sd);
        expect(std::abs(lo) < 1.0e-6 && std::abs(hi) < 1.0e-6, "SLOP 0: on pitch");

        // What is heard. PVAR: notes leave the centre; FVAR: the brightness of notes behind a
        // low-pass varies; PHAS: the first samples of a note differ; LVAR quieter.
        auto notes = [](const Setup& setUp, int count)
        {
            M e;
            setUp(e);
            e.prepareToPlay(SR, 256);
            std::vector<Stereo> out;
            for (int n = 0; n < count; ++n)
            {
                out.push_back(render(e, oneNote(0.2, 57, 0.8f), 0.25, 256, SR, false));
                e.allNotesOff();
            }
            return out;
        };
        {
            double side = 0.0;
            for (auto& s : notes([](EngineBase& e) { e.setParameter(SB + VoiceSpread::Pan, 1.0f); }, 12))
                side = std::max(side, std::abs(db(rms(s.l) / std::max(rms(s.r), 1.0e-9))));
            expect(side > 3.0, "PVAR: notes are placed off the centre (" + juce::String(side, 1) + " dB)");
        }
        {
            std::vector<double> c;
            for (auto& s : notes([](EngineBase& e) { openFilter(e, 800.0f); e.setParameter(SB + VoiceSpread::Cutoff, 1.0f); }, 12))
            {
                float r = 0, cen = 0;
                fastmeasure::rmsAndCentroid(mono(s), 1024, SR, r, cen);
                c.push_back(cen);
            }
            const auto mm = std::minmax_element(c.begin(), c.end());
            expect(*mm.second / std::max(*mm.first, 1.0) > 1.1, "FVAR 1: the brightness of notes behind a low-pass varies ("
                   + juce::String(*mm.first, 0) + " .. " + juce::String(*mm.second, 0) + " Hz)");
        }
        {
            auto first = notes([](EngineBase& e) { e.setParameter(SB + VoiceSpread::Phase, 1.0f); e.setParameter(M::AmpAttack, 0.001f); }, 6);
            auto fixed = notes([](EngineBase& e) { e.setParameter(M::AmpAttack, 0.001f); }, 6);
            bool differ = false, alike = true;
            for (size_t i = 1; i < first.size(); ++i)
            {
                differ = differ || std::abs(first[i].l[200] - first[0].l[200]) > 1.0e-3f;
                alike = alike && fixed[i].l[200] == fixed[0].l[200];
            }
            expect(differ && alike, "PHAS 1: notes start at other places of the cycle; PHAS 0: always the same");
        }
        {
            double quietest = 0.0;
            const auto plain = notes([](EngineBase&) {}, 1);
            for (auto& s : notes([](EngineBase& e) { e.setParameter(SB + VoiceSpread::Level, 1.0f); }, 12))
                quietest = std::min(quietest, db(rms(s.l) / rms(plain[0].l)));
            expect(quietest < -3.0, "LVAR 1: some notes are quieter (" + juce::String(quietest, 1) + " dB)");
        }
        {
            // The note's share is kept on every render path: PVAR where only the level is moved (a
            // slot on AMP), WVAR where TIMBRE is moved per sample (an LFO on it).
            const int ONE = place("ONE");
            double side = 0.0;
            for (auto& s : notes([=](EngineBase& e) { e.setParameter(SB + VoiceSpread::Pan, 1.0f); slot(e, 0, ONE, V::LfoAmp, 0.05f); }, 12))
                side = std::max(side, std::abs(db(rms(s.l) / std::max(rms(s.r), 1.0e-9))));
            expect(side > 3.0, "PVAR with a slot on AMP: still placed (" + juce::String(side, 1) + " dB)");
            auto wvar = [&](bool moved)
            {
                return notes([=](EngineBase& e)
                {
                    e.setParameter(M::ModelParam, 1);
                    e.setParameter(SB + VoiceSpread::Wave, 1.0f);
                    if (moved) lfo(e, 0, V::LfoTimbre, 0.002f, 0.1f);
                }, 6);
            };
            const auto still = wvar(false), moving = wvar(true);
            double worstDiff = -200.0;
            for (size_t i = 0; i < still.size(); ++i)
                worstDiff = std::max(worstDiff, compare(still[i], moving[i]).residual);
            expect(worstDiff < -25.0, "WVAR with an LFO on TIMBRE: each note keeps its offset (" + juce::String(worstDiff, 1) + " dB apart)");
        }
        {
            // The place itself, read from the voice: a restruck voice glides to its new place.
            M one(1);
            one.setParameter(SB + VoiceSpread::Pan, 1.0f);
            one.setParameter(M::AmpSustain, 1.0f);
            one.prepareToPlay(SR, 256);
            one.noteOn(57, 0.8f, -1);
            block(one, 64);
            bool glided = false;
            for (int n = 0; n < 8 && ! glided; ++n)
            {
                const float before = one.getVoice(0).getPan();
                one.noteOn(57, 0.8f, -1);
                block(one, 16);
                const float soon = one.getVoice(0).getPan();
                block(one, 512);
                const float later = one.getVoice(0).getPan();
                expect(std::abs(soon - before) <= 16.0f / (0.004f * (float) SR) + 1.0e-4f, "16 samples after the restrike, at most 16 steps of the glide away");
                glided = std::abs(later - before) > 0.3f;
            }
            expect(glided, "and it arrives at a new place");
        }
        {
            // A voice struck again while it sounds is carried to its new place, not jumped. (One
            // voice: the note must land on the voice that sounds.)
            M e(1);
            e.setParameter(SB + VoiceSpread::Pan, 1.0f);
            e.setParameter(M::TimbreParam, 0.0f);       // (one sine: what moves the side is the place)
            e.setParameter(M::HarmonicsParam, 0.0f);
            e.setParameter(M::AmpAttack, 0.001f);
            e.setParameter(M::AmpSustain, 1.0f);
            const Stereo s = render(e, { { 0.0, On, 57, 0.8f }, { 0.2, On, 57, 0.8f } }, 0.4);
            double jump = 0.0;
            for (size_t i = 1; i < s.l.size(); ++i)
                jump = std::max(jump, (double) std::abs((s.l[i] - s.r[i]) - (s.l[i - 1] - s.r[i - 1])));
            expect(jump < 0.06, "struck again under itself: the side moves smoothly (" + juce::String(jump, 3) + ")");
        }
    }

    void testSeed()
    {
        beginTest("SEED: the same seed and notes, the same render; another seed, another; at SEED 0 the old randoms");
        const int RAND = place("RAND"), RND3 = place("RND3");
        const Setup all = [=](EngineBase& e)
        {
            for (int o = 0; o < VoiceSpread::Seed; ++o)
                e.setParameter(SB + o, o == VoiceSpread::Slop ? 20.0f : 0.4f);
            openFilter(e);
            slot(e, 0, RAND, V::LfoTimbre, 0.5f);
            slot(e, 1, RND3, V::LfoCutoff, 0.4f);
            e.setParameter(M::ModelParam, 3);
            e.setParameter(M::MorphParam, 0.5f);         // (the whisper: a model's noise as well)
        };
        auto run = [&](int seed, bool again = false)
        {
            M e;
            all(e);
            e.setParameter(SB + VoiceSpread::Seed, (float) seed);
            if (again)
            {
                render(e, phrase(), 2.0);                // (another run first, then SEED set again)
                e.allNotesOff();
                e.setParameter(SB + VoiceSpread::Seed, (float) seed);
            }
            return render(e, phrase(), 4.0);
        };
        const Stereo a = run(7), b = run(7), c = run(8), d = run(7, true);
        expect(same(a, b), "SEED 7 twice: the same samples");
        expect(! same(a, c), "SEED 8: another render");
        expect(same(a, d), "setting SEED starts the sequence again");
        // RAND at SEED 0 is what the old engine drew (seeded 0 and the voice): a slot on it MATCHes.
        expectMatch("RAND > TIMBRE at SEED 0", [=](EngineBase& e) { slot(e, 0, RAND, V::LfoTimbre, 0.5f); }, phrase(), 4.0);
        // A SEED changes nothing that does not draw.
        expectMatch("VA at SEED 9, nothing drawn", [](EngineBase& e) { if (auto* m = dynamic_cast<M*>(&e)) m->setParameter(SB + VoiceSpread::Seed, 9.0f); }, phrase(), 4.0);
    }

    // ============================================================================================
    void testBody()
    {
        beginTest("BODY: off is the old voice; on, every cell is heard, it rings on after the note, it follows the pitch");
        // Off (MIX 0, whatever the other cells): the old voice, bit for bit.
        expectMatch("BODY cells set, MIX 0", [](EngineBase& e)
        {
            if (dynamic_cast<M*>(&e) == nullptr) return;
            e.setParameter(BB + VoiceBody::Morph, 3.0f);
            e.setParameter(BB + VoiceBody::Couple, 0.6f);
            e.setParameter(BB + VoiceBody::Tune, 7.0f);
        }, phrase(), 4.0);

        auto make = [](float mix)
        {
            auto e = std::make_unique<M>();
            e->setParameter(M::ModelParam, 1);
            e->setParameter(M::TimbreParam, 0.8f);
            e->setParameter(M::AmpAttack, 0.001f);
            e->setParameter(M::AmpDecay, 0.02f);
            e->setParameter(M::AmpSustain, 0.0f);
            e->setParameter(M::AmpRelease, 0.02f);
            e->setParameter(BB + VoiceBody::Mix, mix);
            e->setParameter(BB + VoiceBody::Decay, 1.5f);
            return e;
        };
        {
            auto plain = make(0.0f), body = make(1.0f);
            const Stereo a = render(*plain, oneNote(0.1, 57), 1.0), b = render(*body, oneNote(0.1, 57), 1.0);
            const auto ma = mono(a), mb = mono(b);
            const size_t late = (size_t) (0.5 * SR);
            expect(rms(ma, late, ma.size()) < 1.0e-5, "without BODY a 20 ms click is over");
            expect(rms(mb, late, mb.size()) > 1.0e-3, "with BODY it rings on (" + juce::String(db(rms(mb, late, mb.size())), 1) + " dB)");
            expect(finite(b), "finite");
            // The voice stays alive while the body rings, and is let go when it has rung out.
            auto e = make(1.0f);
            e->prepareToPlay(SR, 256);
            e->noteOn(57, 0.8f, -1);
            for (int i = 0; i < 20; ++i) block(*e);
            e->noteOff(57, -1);
            for (int i = 0; i < 40; ++i) block(*e);
            expect(sounding(*e) != nullptr, "alive at 1 s: its body rings");
            for (int i = 0; i < 2000; ++i) block(*e);
            expect(sounding(*e) == nullptr, "let go once the body has rung out");
        }
        {
            // Every cell is heard.
            auto plain = make(1.0f);
            const Stereo base = render(*plain, oneNote(0.3, 57), 1.0);
            for (auto [c, v] : std::vector<std::pair<int, float>> { { VoiceBody::Mix, 0.4f }, { VoiceBody::Morph, 3.0f }, { VoiceBody::Decay, 0.3f },
                                                                    { VoiceBody::Couple, 0.7f }, { VoiceBody::Damp, 0.8f }, { VoiceBody::Pos, 0.8f },
                                                                    { VoiceBody::Tune, 7.0f }, { VoiceBody::KeyTrack, 0.0f } })
            {
                auto e = make(1.0f);
                e->setParameter(BB + c, v);
                const Stereo s = render(*e, oneNote(0.3, 57), 1.0);
                const auto d = compare(base, s);
                expect(! d.identical && (std::abs(d.level) > 0.3 || d.curve > 0.5 || std::abs(d.bright) > 0.02 || d.residual > -20.0),
                       juce::String(VoiceBodySection::name(c)) + " is heard: " + describe(d));
            }
        }
        {
            // The body's pitch: the note, + TUNE, + the engine's TUNE / FINE / bend, + the note's SLOP.
            auto e = make(1.0f);
            e->setParameter(M::TuneParam, 5.0f);
            e->setParameter(BB + VoiceBody::Tune, 7.0f);
            e->prepareToPlay(SR, 256);
            e->noteOn(57, 0.8f, -1);
            block(*e);
            V* v = sounding(*e);
            expect(v != nullptr);
            if (v != nullptr)
                expectWithinAbsoluteError(v->getBody().frequency(), 440.0, 0.01, "57 + 5 + 7 = 69: 440 Hz");
            e->pitchBend(1.0f);
            block(*e);
            if (v != nullptr)
                expectWithinAbsoluteError(v->getBody().frequency(), 440.0 * std::exp2(2.0 / 12.0), 0.01, "and the bend (2 semitones)");
            auto s = make(1.0f);
            s->setParameter(SB + VoiceSpread::Slop, 50.0f);
            s->prepareToPlay(SR, 256);
            s->noteOn(57, 0.8f, -1);
            block(*s);
            if (V* w = sounding(*s))
                expectWithinAbsoluteError(w->getBody().frequency() / dsp::midiNoteToHz(57), w->getFrequency() / dsp::midiNoteToHz(57), 1.0e-4,
                                          "SLOP moves the body with the note");
        }
        {
            // A destination: LFO1 > BMIX moves the mix; a slot KEY > BDCY.
            auto still = make(0.5f), moving = make(0.5f);
            lfo(*moving, 0, dest("BMIX"), 0.8f, 3.0f);
            const auto d = compare(render(*still, oneNote(0.8, 57), 1.0), render(*moving, oneNote(0.8, 57), 1.0));
            expect(! d.identical && d.residual > -30.0, "LFO1 > BMIX is heard: " + describe(d));
            // ... around the cell's own value: a shallow LFO is a small change of the sound.
            auto shallow = make(0.5f);
            lfo(*shallow, 0, dest("BMIX"), 0.01f, 0.2f);
            const auto sh = compare(render(*make(0.5f), oneNote(0.8, 57), 1.0), render(*shallow, oneNote(0.8, 57), 1.0));
            expect(sh.residual < -25.0, "a shallow LFO on BMIX moves the mix around MIX as set (" + juce::String(sh.residual, 1) + " dB)");
            auto keyed = make(1.0f);
            slot(*keyed, 0, place("KEY"), dest("BDCY"), -1.0f);
            const auto k = compare(render(*make(1.0f), oneNote(0.1, 84), 1.0), render(*keyed, oneNote(0.1, 84), 1.0));
            expect(k.level < -1.0, "KEY > BDCY: a high note's body rings shorter (" + juce::String(k.level, 1) + " dB)");
        }
        {
            // Eight voices with BODY on, every model, the ends of every cell: bounded.
            for (int m = 0; m < 8; ++m)
                for (float end : { 0.0f, 1.0f })
                {
                    M e;
                    e.setParameter(M::ModelParam, (float) m);
                    for (int id : { (int) M::TimbreParam, (int) M::ColorParam, (int) M::HarmonicsParam, (int) M::MorphParam })
                        e.setParameter(id, end);
                    for (auto& p : VoiceBodySection::makePage(BB).params)
                        e.setParameter(p.id, end > 0.5f ? p.maxVal : p.minVal);
                    e.setParameter(BB + VoiceBody::Mix, 1.0f);
                    Events chord;
                    for (int n = 0; n < 8; ++n)
                        chord.push_back({ 0.0, On, 20 + n * 13, 1.0f });
                    expect(finite(render(e, chord, 1.0, 100, 96000.0), 8.0f), "model " + juce::String(m) + ", cells at " + juce::String(end) + ": bounded");
                }
        }
    }

    // ============================================================================================
    // PLCK, DUST and BOW against the nearest BODY settings found. A replacement is a remaining model
    // (VA FM FOLD FORM HARM) with its four macros and envelope, the filter's cutoff, and BODY.
    struct Recipe
    {
        float model = 1, timbre = 0.5f, color = 0.5f, harmonics = 0.5f, morph = 0;
        float attack = 0.001f, decay = 0.02f, sustain = 0, release = 0.02f, cutoff = 20000, level = 0.8f;
        float mix = 1, body = 0, ring = 1, couple = 0, damp = 0, pos = 0, keyDecay = 0;
    };
    static constexpr int RECIPE_CELLS = 18;
    static float& field(Recipe& r, int i)
    {
        float* f[RECIPE_CELLS] = { &r.model, &r.timbre, &r.color, &r.harmonics, &r.morph, &r.attack, &r.decay, &r.sustain, &r.release,
                                   &r.cutoff, &r.level, &r.mix, &r.body, &r.ring, &r.couple, &r.damp, &r.pos, &r.keyDecay };
        return *f[i];
    }
    static void apply(EngineBase& e, const Recipe& r, float tune)
    {
        e.setParameter(M::ModelParam, r.model);
        e.setParameter(M::TimbreParam, r.timbre);
        e.setParameter(M::ColorParam, r.color);
        e.setParameter(M::HarmonicsParam, r.harmonics);
        e.setParameter(M::MorphParam, r.morph);
        e.setParameter(M::AmpAttack, r.attack);
        e.setParameter(M::AmpDecay, r.decay);
        e.setParameter(M::AmpSustain, r.sustain);
        e.setParameter(M::AmpRelease, r.release);
        e.setParameter(M::LevelParam, r.level);
        e.setParameter(M::TuneParam, tune);
        e.setParameter(FB + VoiceFilterSection::Cutoff, r.cutoff);
        e.setParameter(BB + VoiceBody::Mix, r.mix);
        e.setParameter(BB + VoiceBody::Morph, r.body);
        e.setParameter(BB + VoiceBody::Decay, r.ring);
        e.setParameter(BB + VoiceBody::Couple, r.couple);
        e.setParameter(BB + VoiceBody::Damp, r.damp);
        e.setParameter(BB + VoiceBody::Pos, r.pos);
        if (r.keyDecay != 0.0f)
            slot(e, 0, place("KEY"), dest("BDCY", (int) r.model), r.keyDecay);
    }

    // The gesture: four notes two octaves apart at two velocities, each alone, held 1.2 s, 2.4 s each.
    static constexpr double ROW_SECONDS = 2.4, HOLD = 1.2;
    static std::vector<std::pair<int, float>> rows() { return { { 36, 1.0f }, { 48, 0.5f }, { 48, 1.0f }, { 60, 0.5f }, { 60, 1.0f }, { 72, 0.5f }, { 72, 1.0f }, { 84, 0.7f } }; }
    template <typename Engine>
    static std::vector<Stereo> renderRows(const std::function<void(EngineBase&)>& setUp, uint32_t seedNotes = 0)
    {
        std::vector<Stereo> out;
        for (auto [note, velocity] : rows())
        {
            Engine e;
            setUp(e);
            juce::ignoreUnused(seedNotes);
            out.push_back(render(e, oneNote(HOLD, note, velocity), ROW_SECONDS));
        }
        return out;
    }
    static std::vector<Compared> compareRows(const std::vector<Stereo>& older, const std::vector<Stereo>& newer, bool pitched)
    {
        std::vector<Compared> out;
        for (size_t i = 0; i < older.size(); ++i)
            out.push_back(compare(older[i], newer[i], SR, pitched));
        return out;
    }
    static double worst(const std::vector<Compared>& rows)
    {
        double w = 0.0;
        for (auto& c : rows) w = std::max(w, over(c));
        return w;
    }

    // The nearest settings found (NEPTUNE_U4_MACRO_SEARCH, 2026-11-22), one per factory preset.
    static Recipe nearest(const juce::String& name);

    void testRuleCanFail()
    {
        beginTest("The CLOSE rule can fail: a deliberate loss on the old engine is LOST, the old engine against itself is MATCH");
        const auto presets = factoryPresets();
        auto find = [&presets](const char* name) { for (auto& p : presets) if (p.name == name) return p; return presets.front(); };
        auto loss = [&](const char* name, int id, float delta, bool pitched)
        {
            const Preset p = find(name);
            const auto a = renderRows<O>(preset(p));
            const auto b = renderRows<O>([&](EngineBase& e) { preset(p)(e); e.setParameter(id, e.getParameter(id) + delta); });
            const double w = worst(compareRows(a, b, pitched));
            expect(w > 1.0, juce::String(name) + ", cell " + juce::String(id) + " moved by " + juce::String(delta) + ": LOST (" + juce::String(w, 2) + " x the limits)");
        };
        loss("Nylon Guitar", M::TimbreParam, -0.3f, true);        // a brighter string
        loss("Nylon Guitar", M::HarmonicsParam, -0.4f, true);     // a shorter one
        loss("Vinyl Crackle", M::TimbreParam, 0.5f, false);       // crackle to hiss
        loss("Solo Cello", M::ColorParam, 0.5f, true);            // the bow moved along the string
        // Crackle against a steady hiss of the same level and colour: the crest factor tells them apart.
        const auto crackle = renderRows<O>(preset(find("Vinyl Crackle")));
        const auto hiss = renderRows<M>([](EngineBase& e) { e.setParameter(M::ModelParam, 3); e.setParameter(M::MorphParam, 1.0f); e.setParameter(M::HarmonicsParam, 0.0f); });
        double crestGap = 0.0;
        for (auto& c : compareRows(crackle, hiss, false)) crestGap = std::max(crestGap, std::abs(c.crest));
        expect(crestGap > 3.0, "crackle against hiss: the crest factors " + juce::String(crestGap, 1) + " dB apart");
        const auto again = renderRows<O>(preset(find("Solo Cello")));
        expect(worst(compareRows(again, renderRows<O>(preset(find("Solo Cello"))), true)) == 0.0, "the old engine against itself: MATCH");
    }

    void testFoldedModels()
    {
        beginTest("PLCK, DUST, BOW: every factory preset against the nearest BODY settings (MATCH / CLOSE / LOST)");
        std::map<int, int> closeCount, lostCount;
        for (auto& p : factoryPresets())
        {
            const int m = modelOf(p);
            if (m < 5)
                continue;
            const bool pitched = m != 6;
            const Recipe r = nearest(p.name);
            const float tune = cell(p, M::TuneParam, 0.0f);
            const auto older = renderRows<O>(preset(p));
            const auto newer = renderRows<M>([&](EngineBase& e) { apply(e, r, tune); });
            const auto c = compareRows(older, newer, pitched);
            int lost = 0;
            for (size_t i = 0; i < c.size(); ++i)
            {
                lost += close(c[i]) ? 0 : 1;
                if (printing())
                    logMessage("U4-MACRO | " + (p.name + " " + juce::String(rows()[i].first) + " v" + juce::String(rows()[i].second, 1)).paddedRight(' ', 26) + " | " + describe(c[i]));
                expect(finite(newer[i]), p.name + ": the replacement is bounded");
            }
            if (printing())
                logMessage("U4-MACRO | " + p.name.paddedRight(' ', 26) + " | " + (lost == 0 ? "CLOSE" : "LOST ") + " | " + juce::String(lost) + " of " + juce::String((int) c.size())
                           + " rows LOST | worst " + juce::String(worst(c), 2) + " x the limits");
            (lost == 0 ? closeCount : lostCount)[m]++;
            if (atlas().isNotEmpty())
            {
                const juce::File folder(atlas());
                folder.createDirectory();
                std::vector<float> a, b;
                for (size_t i = 0; i < older.size(); ++i)
                {
                    const auto ma = mono(older[i]), mb = mono(newer[i]);
                    a.insert(a.end(), ma.begin(), ma.end());
                    b.insert(b.end(), mb.begin(), mb.end());
                }
                const juce::String file = p.name.replaceCharacters(" >/*,", "_____");
                writeWav(folder.getChildFile(file + "_old.wav"), a);
                writeWav(folder.getChildFile(file + "_new.wav"), b);
            }
        }
        // What the measurements said (2026-11-22): every preset of the three models LOST, so all
        // three stay in the product as they are (the owner's rule) and go to the owner.
        for (int m : { 5, 6, 7 })
        {
            expectEquals(closeCount[m] + lostCount[m], m == 7 ? 4 : 5, "every preset of model " + juce::String(m) + " compared");
            // (Should a preset ever come out CLOSE, its model could go: this line is then changed
            // together with the model's removal, never alone.)
            expectEquals(closeCount[m], 0, "model " + juce::String(m) + ": the verdict as reported (LOST: kept)");
            if (printing())
                logMessage("U4-MACRO | model " + juce::String(m) + ": " + juce::String(closeCount[m]) + " CLOSE, " + juce::String(lostCount[m]) + " LOST");
        }
        // Kept: the three models are still there, and a stored model 5 / 6 / 7 still plays them.
        for (int m : { 5, 6, 7 })
        {
            M e;
            e.setParameter(M::ModelParam, (float) m);
            expect(e.getVoice(0).getModel() == (V::Model) m);
            expect(rms(mono(render(e, oneNote(0.5, 57), 0.6))) > 1.0e-3, "model " + juce::String(m) + " sounds");
        }
    }

    // ============================================================================================
    void testEnvelopeSection()
    {
        beginTest("AmpEnvelopeSection by itself: BESIDE (macro, RSN) and INSTEAD (the wave sequencer's way)");
        struct Voice { AmpEnvelope env; AmpEnvelope& getAmpEnvelopeSection() { return env; } };
        auto runOut = [](AmpEnvelope& e, int samples) { bool on = true; float last = 0.0f; for (int i = 0; i < samples && on; ++i) last = e.process(on); return std::make_pair(on, last); };
        {
            std::vector<Voice> voices(2);
            AmpEnvelopeSection s(AmpEnvelope::Policy::Beside);
            auto each = AmpEnvelopeSection::each(voices);
            s.prepareVoices(each);
            for (auto& v : voices) v.env.setSampleRate(SR);
            s.setCell(AmpEnvelope::Attack, 0.01f, each);
            s.setCell(AmpEnvelope::Release, 0.05f, each);
            s.setMode(1, each, SR);
            expectEquals(voices[1].env.chain().getStageCount(), 2, "CHAN on: two copies made");
            s.setStages(9, each, SR);
            expectEquals(voices[0].env.chain().getStageCount(), 4, "held to four");
            s.setCell(AmpEnvelope::Decay, 0.123f, each);
            expectEquals(voices[0].env.chain().getEnvelopeMut(3).getParams().decay, 0.123f, "a cell reaches every stage");
            auto& e = voices[0].env;
            e.noteOn();
            expect(e.envelope().getStage() != Envelope::Stage::Idle, "BESIDE: the amp envelope runs beside the chain");
            runOut(e, 4800);
            e.noteOff();
            expect(! runOut(e, (int) SR * 2).first, "and the voice ends once both have");
            s.setMode(0, each, SR);
            expect(! voices[0].env.chained());
        }
        {
            std::vector<Voice> voices(1);
            AmpEnvelopeSection s(AmpEnvelope::Policy::Instead, 8);
            auto each = AmpEnvelopeSection::each(voices);
            s.prepareVoices(each);
            voices[0].env.setSampleRate(SR);
            s.setMode(2, each, SR);
            auto& e = voices[0].env;
            e.noteOn();
            expect(e.envelope().getStage() == Envelope::Stage::Idle, "INSTEAD: the amp envelope is not started");
            const auto r = runOut(e, 4800);
            expect(r.first && r.second > 0.1f, "the chain gives the level");
            e.noteOff();
            expect(! runOut(e, (int) SR * 2).first, "and ends the voice by itself");
        }
    }

    void testRealTime()
    {
        beginTest("Real time: no allocation while BODY, ANLG, the chain and the slots are on");
        M e;
        e.prepareToPlay(SR, 256);
        for (int o = 0; o < VoiceSpread::Seed; ++o)
            e.setParameter(SB + o, 0.5f);
        e.setParameter(BB + VoiceBody::Mix, 0.7f);
        e.setParameter(BB + VoiceBody::Couple, 0.5f);
        e.setParameter(M::EnvChainMode, 2);
        slot(e, 0, place("RAND"), dest("BMRP"), 0.5f);
        lfo(e, 0, dest("BDMP"), 0.5f);
        block(e);
        malloc_logger = ModRouterAllocations::count;
        ModRouterAllocations::events = 0;
        ModRouterAllocations::counting = true;
        for (int n = 0; n < 24; ++n)
        {
            e.noteOn(40 + n, 0.8f, -1);
            block(e);
            if (n % 3 == 0) e.noteOff(40 + n - 2, -1);
        }
        for (int i = 0; i < 40; ++i) block(e);
        ModRouterAllocations::counting = false;
        malloc_logger = nullptr;
        expectEquals(ModRouterAllocations::events, 0, "no malloc or free on the audio path");
    }

    // ============================================================================================
    // Added by the U4-MACRO review (2026-11-22): each case below was a mutation of the product code
    // that every test above let through, or a defect found reading the diff.
    void testReview()
    {
        beginTest("Review: a place glides only where a voice is heard; EVAR on both envelopes; WVAR kept by TIMBRE; PAN and PVAR held to the sides");
        // A note on a voice whose body still rings (the model has ended, BODY has not) is carried
        // to its new place like any voice that sounds: the ringing body must not jump sides.
        {
            bool tried = false;
            for (int attempt = 0; attempt < 16 && ! tried; ++attempt)
            {
                M e(1);
                e.setParameter(SB + VoiceSpread::Seed, (float) attempt);
                e.setParameter(SB + VoiceSpread::Pan, 1.0f);
                e.setParameter(M::ModelParam, 1);
                e.setParameter(M::AmpAttack, 0.001f);
                e.setParameter(M::AmpDecay, 0.02f);
                e.setParameter(M::AmpSustain, 0.0f);
                e.setParameter(M::AmpRelease, 0.02f);
                e.setParameter(BB + VoiceBody::Mix, 1.0f);
                e.setParameter(BB + VoiceBody::Decay, 4.0f);
                e.prepareToPlay(SR, 256);
                e.noteOn(57, 0.8f, -1);
                block(e, 256);
                e.noteOff(57, -1);
                for (int i = 0; i < 60; ++i) block(e, 256);         // 0.3 s: the model is over, the body rings
                const V& v = e.getVoice(0);
                if (! v.isActive())
                    continue;
                const float before = v.getPan();
                e.noteOn(60, 0.8f, -1);
                block(e, 16);
                const float soon = e.getVoice(0).getPan();
                block(e, 1024);
                const float later = e.getVoice(0).getPan();
                if (std::abs(later - before) < 0.3f)
                    continue;                                        // (this seed's new place was near the old)
                tried = true;
                expect(std::abs(soon - before) <= 16.0f / (0.004f * (float) SR) + 1.0e-4f,
                       "a ringing body struck again glides to its new place (moved " + juce::String(std::abs(soon - before), 3) + " in 16 samples)");
            }
            expect(tried, "a restrike that moves the place was found");
        }
        // A fresh voice starts at its place (no glide from the centre).
        {
            M e(1);
            e.setParameter(SB + VoiceSpread::Pan, 1.0f);
            e.setParameter(M::AmpSustain, 1.0f);
            e.prepareToPlay(SR, 256);
            e.noteOn(57, 0.8f, -1);
            block(e, 16);
            const float first = e.getVoice(0).getPan();
            block(e, 1024);
            expect(std::abs(first) > 1.0e-3f && first == e.getVoice(0).getPan(), "a new note is at its place from its first sample");
        }
        // PVAR turned to 0 under a sounding, placed voice, then the voice struck again: it glides
        // back to the centre (it does not jump there).
        {
            bool tried = false;
            for (int attempt = 0; attempt < 16 && ! tried; ++attempt)
            {
                M e(1);
                e.setParameter(SB + VoiceSpread::Seed, (float) attempt);
                e.setParameter(SB + VoiceSpread::Pan, 1.0f);
                e.setParameter(M::TimbreParam, 0.0f);
                e.setParameter(M::HarmonicsParam, 0.0f);
                e.setParameter(M::AmpAttack, 0.001f);
                e.setParameter(M::AmpSustain, 1.0f);
                e.prepareToPlay(SR, 256);
                e.noteOn(57, 0.8f, -1);
                block(e, 256);
                if (std::abs(e.getVoice(0).getPan()) < 0.5f)
                    continue;
                tried = true;
                e.setParameter(SB + VoiceSpread::Pan, 0.0f);
                juce::AudioBuffer<float> buf(2, 512);
                juce::MidiBuffer midi;
                float lastSide = 0.0f, jump = 0.0f;
                bool firstSample = true;
                for (int b = 0; b < 8; ++b)
                {
                    if (b == 2) e.noteOn(57, 0.8f, -1);
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 512; ++i)
                    {
                        const float side = buf.getSample(0, i) - buf.getSample(1, i);
                        if (! firstSample) jump = std::max(jump, std::abs(side - lastSide));
                        lastSide = side;
                        firstSample = false;
                    }
                }
                expect(jump < 0.06f, "PVAR 0 and struck again: the side signal glides to the centre (" + juce::String(jump, 3) + ")");
            }
            expect(tried, "a placed note was found");
        }
        // EVAR: the filter envelope takes the note's factor as the amp envelope does; with ETIM or
        // FTIM routed, each envelope is the routed factor times the note's.
        {
            auto ratios = [](int routed)
            {
                M e;
                e.prepareToPlay(SR, 256);
                e.setParameter(SB + VoiceSpread::EnvTime, 1.0f);
                e.setParameter(FB + VoiceFilterSection::Decay, 0.4f);      // (the filter's scale read as its decay over this)
                if (routed >= 0)
                    slot(e, 0, place("ONE"), routed, 0.5f);
                std::vector<double> amp, flt;
                for (int n = 0; n < 40; ++n)
                {
                    e.noteOn(60, 0.8f, -1);
                    block(e, 32);
                    V& v = e.getVoice(0);
                    amp.push_back(v.getAmpEnvelope().getTimeScale());
                    flt.push_back(v.getFilter().getEnvelope().getParams().decay / 0.4);
                    e.allNotesOff();
                }
                return std::make_pair(amp, flt);
            };
            auto check = [this](const std::pair<std::vector<double>, std::vector<double>>& r, const juce::String& what)
            {
                const auto& [amp, flt] = r;
                const auto mm = std::minmax_element(amp.begin(), amp.end());
                bool constant = true;
                for (size_t i = 0; i < amp.size(); ++i)
                    constant = constant && std::abs(flt[i] / amp[i] - flt[0] / amp[0]) < 1.0e-4;
                expect(*mm.second / *mm.first > 1.5 && constant, what + ": every note's own factor on both envelopes ("
                       + juce::String(*mm.first, 3) + " .. " + juce::String(*mm.second, 3) + ")");
            };
            check(ratios(-1), "EVAR");
            check(ratios(V::LfoFilterTime), "EVAR with ONE > FTIM");
            check(ratios(V::LfoEnvTime), "EVAR with ONE > ETIM");
        }
        // WVAR: TIMBRE turned under a sounding note keeps the note's offset; and it is held to 0 .. 1.
        {
            M e(1);
            e.setParameter(M::ModelParam, 1);
            e.setParameter(SB + VoiceSpread::Wave, 1.0f);
            e.setParameter(M::TimbreParam, 0.9f);
            e.prepareToPlay(SR, 256);
            e.noteOn(57, 0.8f, -1);
            const float offset = e.getVoice(0).getNoteTimbreOffset();
            e.setParameter(M::TimbreParam, 0.4f);
            expectWithinAbsoluteError(e.getVoice(0).getTimbre(), juce::jlimit(0.0f, 1.0f, 0.4f + offset), 1.0e-6f,
                                      "TIMBRE turned under the note: the cell plus the note's WVAR");
            for (float set : { 0.0f, 1.0f })
            {
                M f(1);
                f.setParameter(SB + VoiceSpread::Wave, 1.0f);
                f.setParameter(M::TimbreParam, set);
                f.prepareToPlay(SR, 256);
                float lo = 1.0f, hi = 0.0f;
                for (int n = 0; n < 64; ++n)
                {
                    f.noteOn(57, 0.8f, -1);
                    lo = std::min(lo, f.getVoice(0).getTimbre());
                    hi = std::max(hi, f.getVoice(0).getTimbre());
                    f.allNotesOff();
                }
                expect(lo >= 0.0f && hi <= 1.0f && hi > lo, "WVAR 1 at TIMBRE " + juce::String(set, 0) + ": held to 0 .. 1 ("
                       + juce::String(lo, 3) + " .. " + juce::String(hi, 3) + ")");
            }
        }
        // PAN routed at full depth beside PVAR: the place stays between the sides (never a channel
        // turned upside down).
        {
            M one(1);       // (one voice: summed voices may cancel on one side)
            one.setParameter(SB + VoiceSpread::Pan, 1.0f);
            one.setParameter(M::AmpSustain, 1.0f);
            lfo(one, 1, V::LfoPan, 1.0f, 3.0f);
            int flipped = 0;
            for (int n = 0; n < 6; ++n)
            {
                const Stereo t = render(one, { { 0.0, On, 57, 0.8f } }, 0.4);
                for (size_t i = 0; i < t.l.size(); ++i)
                    flipped += (t.l[i] * t.r[i] < -1.0e-9f) ? 1 : 0;
                one.allNotesOff();
            }
            expectEquals(flipped, 0, "one voice: its two channels never of opposite sign");
        }
        // PHAS on FM (the carrier's and the modulator's phases) as well as on the saws.
        {
            auto notes = [](bool phas)
            {
                M e(1);
                e.setParameter(M::ModelParam, 1);
                e.setParameter(M::AmpAttack, 0.001f);
                if (phas) e.setParameter(SB + VoiceSpread::Phase, 1.0f);
                e.prepareToPlay(SR, 256);
                std::vector<float> at;
                for (int n = 0; n < 6; ++n)
                {
                    at.push_back(render(e, oneNote(0.1, 57, 0.8f), 0.12, 256, SR, false).l[200]);
                    e.allNotesOff();
                }
                return at;
            };
            const auto with = notes(true), without = notes(false);
            bool differ = false, alike = true;
            for (size_t i = 1; i < with.size(); ++i)
            {
                differ = differ || std::abs(with[i] - with[0]) > 1.0e-3f;
                alike = alike && without[i] == without[0];
            }
            expect(differ && alike, "PHAS 1 on FM: notes start at other places of the cycle");
        }
        // The CLOSE rule can also PASS: a change nobody would call a loss (0.4 dB quieter) on the
        // old engine is CLOSE, so a LOST is not the rule failing everything.
        {
            const auto presets = factoryPresets();
            for (const char* name : { "Nylon Guitar", "Solo Cello" })
                for (auto& p : presets)
                    if (p.name == name)
                    {
                        const auto a = renderRows<O>(preset(p));
                        const auto b = renderRows<O>([&](EngineBase& e) { preset(p)(e); e.setParameter(M::LevelParam, e.getParameter(M::LevelParam) * 0.955f); });
                        const double w = worst(compareRows(a, b, true));
                        expect(w < 1.0, juce::String(name) + " 0.4 dB quieter: CLOSE (" + juce::String(w, 2) + " x the limits)");
                        // ... and a deliberate loss of the review's own: a semitone sharp is LOST by (f).
                        const auto c = renderRows<O>([&](EngineBase& e) { preset(p)(e); e.setParameter(M::TuneParam, e.getParameter(M::TuneParam) + 1.0f); });
                        const auto cc = compareRows(a, c, true);
                        double pitchOver = 0.0;
                        for (auto& r : cc) pitchOver = std::max(pitchOver, std::abs(r.pitch) / 0.03);
                        expect(pitchOver > 1.0, juce::String(name) + " a semitone sharp: LOST by its pitch (" + juce::String(pitchOver, 2) + " x)");
                    }
        }
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_REVIEW", "").isNotEmpty())
        {
            // Measurement only: the same model, the same preset, old seeds against new (the one
            // difference the noise draw makes). Says whether the rule can call a noisy model CLOSE at all.
            for (auto& p : factoryPresets())
            {
                const int m = modelOf(p);
                if (m != 5 && m != 6)
                    continue;
                const auto c = compareRows(renderRows<O>(preset(p)), renderRows<M>(preset(p)), m == 5);
                int lost = 0;
                for (auto& r : c) lost += close(r) ? 0 : 1;
                logMessage("U4-MACRO REVIEW | old seeds vs new seeds | " + p.name.paddedRight(' ', 18) + " | " + juce::String(lost) + " of 8 rows LOST | worst "
                           + juce::String(worst(c), 2) + " x | e.g. row 0: " + describe(c[0]));
            }
        }
    }

    // ============================================================================================
    // NEPTUNE_U4_MACRO_SEARCH=1 (Release binary): coordinate descent from a start per model over the
    // recipe's cells, minimising the worst row's distance past the CLOSE limits. Prints each
    // preset's best recipe in the form nearest() takes.
    void search()
    {
        beginTest("Search (NEPTUNE_U4_MACRO_SEARCH)");
        const std::vector<std::vector<float>> values = {
            { 0, 1, 2, 3, 4 },                                          // model
            { 0, 0.15f, 0.3f, 0.5f, 0.7f, 0.85f, 1 },                   // timbre
            { 0, 0.15f, 0.3f, 0.5f, 0.7f, 0.85f, 1 },                   // color
            { 0, 0.15f, 0.3f, 0.5f, 0.7f, 0.85f, 1 },                   // harmonics
            { 0, 0.15f, 0.3f, 0.5f, 0.7f, 0.85f, 1 },                   // morph
            { 0.001f, 0.003f, 0.01f, 0.03f, 0.1f, 0.3f, 0.8f },         // attack
            { 0.002f, 0.005f, 0.01f, 0.03f, 0.1f, 0.3f, 1, 3 },         // decay
            { 0, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1 },                     // sustain
            { 0.005f, 0.02f, 0.1f, 0.3f, 0.6f, 1.5f, 3 },               // release
            { 300, 800, 1500, 3000, 6000, 12000, 20000 },               // cutoff
            { 0.1f, 0.15f, 0.22f, 0.3f, 0.4f, 0.5f, 0.65f, 0.8f, 1 },   // level
            { 0.25f, 0.5f, 0.75f, 1 },                                  // mix
            { 0, 0.5f, 1, 1.5f, 2, 3, 4, 5 },                           // body
            { 0.05f, 0.1f, 0.2f, 0.4f, 0.7f, 1, 1.5f, 2.5f, 4, 7, 12 }, // ring
            { 0, 0.25f, 0.5f, 0.75f, 1 },                               // couple
            { -1, -0.6f, -0.3f, 0, 0.3f, 0.6f, 1 },                     // damp
            { -1, -0.5f, 0, 0.5f, 1 },                                  // pos
            { -1, -0.6f, -0.3f, 0, 0.3f },                              // keyDecay
        };
        const juce::String only = juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_MACRO_SEARCH", "");
        for (auto& p : factoryPresets())
        {
            const int m = modelOf(p);
            if (m < 5 || (only != "1" && ! p.name.contains(only)))
                continue;
            const bool pitched = m != 6;
            const float tune = cell(p, M::TuneParam, 0.0f);
            const auto older = renderRows<O>(preset(p));
            Recipe best = nearest(p.name);
            // (The mean of the rows' distances with the worst one: smoother to descend than the worst alone.)
            auto score = [&](const Recipe& r)
            {
                const auto c = compareRows(older, renderRows<M>([&](EngineBase& e) { apply(e, r, tune); }), pitched);
                double sum = 0.0;
                for (auto& x : c) sum += std::min(over(x), 100.0);
                return sum / (double) c.size() + 0.5 * std::min(worst(c), 100.0);
            };
            double bestScore = score(best);
            for (int pass = 0; pass < 2; ++pass)
            {
                bool improved = false;
                for (int f = 0; f < RECIPE_CELLS; ++f)
                    for (float v : values[(size_t) f])
                    {
                        Recipe r = best;
                        if (field(r, f) == v) continue;
                        field(r, f) = v;
                        const double s = score(r);
                        if (s < bestScore) { bestScore = s; best = r; improved = true; }
                    }
                std::printf("  %-18s pass %d: %.3f x the limits\n", p.name.toRawUTF8(), pass, bestScore);
                std::fflush(stdout);
                if (! improved) break;
            }
            const auto finalRows = compareRows(older, renderRows<M>([&](EngineBase& e) { apply(e, best, tune); }), pitched);
            for (auto& x : finalRows)
                std::printf("      %s\n", describe(x).toRawUTF8());
            std::printf("    if (name == \"%s\") return { %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g };  // %.2f\n",
                        p.name.toRawUTF8(), best.model, best.timbre, best.color, best.harmonics, best.morph, best.attack, best.decay, best.sustain,
                        best.release, best.cutoff, best.level, best.mix, best.body, best.ring, best.couple, best.damp, best.pos, best.keyDecay, bestScore);
            std::fflush(stdout);
        }
        expect(true);
    }

    // ============================================================================================
    template <typename Engine>
    static double measure(const Setup& setUp, float strip)
    {
        constexpr int BLOCK = 512, BLOCKS = 940;        // 10 s
        Engine e(8);
        e.prepareToPlay(SR, BLOCK);
        if (setUp) setUp(e);
        e.setModWheel(strip);
        static const int notes[8] = { 48, 52, 55, 59, 62, 65, 69, 72 };
        for (int n : notes)
            e.noteOn(n, 0.8f, -1);
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < BLOCKS; ++b)
        {
            buf.clear();
            e.processBlock(buf, midi);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return 100.0 * seconds / (BLOCKS * BLOCK / SR);
    }

    void bench()
    {
        beginTest("Cost (NEPTUNE_U4_MACRO_BENCH)");
        struct Case { const char* name; Setup setUp; float strip; bool oldToo; };
        auto model = [](int m) { return [m](EngineBase& e) { e.setParameter(M::ModelParam, (float) m); e.setParameter(M::AmpSustain, 1.0f); }; };
        const std::vector<Case> cases = {
            { "VA idle (nothing routed)", model(0), 0.0f, true },
            { "FM idle", model(1), 0.0f, true },
            { "HARM idle", model(4), 0.0f, true },
            { "PLCK idle", model(5), 0.0f, true },
            { "VA, chain PAR 4", [=](EngineBase& e) { model(0)(e); e.setParameter(M::EnvChainMode, 2); e.setParameter(M::EnvChainStages, 4); }, 0.0f, true },
            { "VA, LFO1 > TIMBRE, strip up", [=](EngineBase& e) { model(0)(e); lfo(e, 0, V::LfoTimbre, 0.4f); }, 0.6f, true },
            { "new: VA, every ANLG amount up", [=](EngineBase& e) { model(0)(e); for (int o = 0; o < VoiceSpread::Seed; ++o) e.setParameter(SB + o, 0.5f); }, 0.0f, false },
            { "new: VA, BODY on (STRG)", [=](EngineBase& e) { model(0)(e); e.setParameter(BB + VoiceBody::Mix, 1.0f); }, 0.0f, false },
            { "new: VA, BODY on, COUP 0.75", [=](EngineBase& e) { model(0)(e); e.setParameter(BB + VoiceBody::Mix, 1.0f); e.setParameter(BB + VoiceBody::Couple, 0.75f); }, 0.0f, false },
        };
        std::printf("Macro oscillator, 8 voices, 512-sample blocks, 48 kHz: %% of one core (best of 7, old and new alternately)\n");
        for (auto& c : cases)
        {
            double older = 1.0e9, newer = 1.0e9;
            for (int run = 0; run < 7; ++run)
            {
                if (c.oldToo)
                    older = std::min(older, measure<O>(c.setUp, c.strip));
                newer = std::min(newer, measure<M>(c.setUp, c.strip));
            }
            if (c.oldToo)
                std::printf("  %-40s old %6.3f %%   new %6.3f %%   (%+5.1f %%)\n", c.name, older, newer, 100.0 * (newer / older - 1.0));
            else
                std::printf("  %-40s old    -     new %6.3f %%\n", c.name, newer);
        }
        std::fflush(stdout);
        expect(true);
    }
};

// The nearest settings found per preset by search() (Release, 2026-11-22: two passes of coordinate
// descent over the recipe's 18 cells from the starts below; the number is the search's score, the
// mean distance past the limits with half the worst). The start where none was found.
inline U4MacroTests::Recipe U4MacroTests::nearest(const juce::String& name)
{
    if (name == "Bowed Bass") return { 0, 0.15, 1, 1, 0, 0.1, 0.1, 0, 0.3, 300, 0.5, 1, 5, 1.5, 0, -1, 0.5, 0 };  // 28.53
    if (name == "Dust Hat") return { 3, 0, 1, 1, 1, 0.001, 0.1, 0.1, 3, 300, 0.1, 0.25, 0, 0.2, 0, 0, 1, 0.3 };  // 60.79
    if (name == "Glass Bow") return { 2, 0, 0.3, 0.15, 0.5, 0.8, 3, 0, 0.6, 1500, 0.4, 1, 0, 2.5, 0, -1, -1, 0 };  // 8.52
    if (name == "Grain Cloud") return { 4, 0.5, 0.5, 0, 0, 0.8, 1, 0.9, 1.5, 300, 0.1, 0.5, 2, 0.4, 0.5, 0, 0, -0.3 };  // 14.20
    if (name == "Hiss Riser") return { 3, 1, 0, 0, 0.85, 0.1, 1, 0.1, 0.6, 300, 0.1, 0.25, 1, 0.05, 0, 0.3, 0, -1 };  // 26.70
    if (name == "Koto Snap") return { 3, 0.3, 0.3, 0.3, 1, 0.01, 0.002, 0, 1.5, 3000, 0.8, 0.25, 2, 1.5, 0, 0.3, 0.5, -1 };  // 71.29
    if (name == "Nylon Guitar") return { 3, 0.15, 0.85, 0.3, 1, 0.01, 0.002, 0, 1.5, 12000, 0.4, 0.25, 0, 0.7, 0, 0, -1, -1 };  // 74.08
    if (name == "Pitched Rain") return { 4, 0.5, 0.5, 0, 1, 0.3, 0.002, 0.5, 1.5, 300, 0.4, 0.75, 2, 0.2, 0.5, 0, 1, 0.3 };  // 7.16
    if (name == "Pluck Bass") return { 4, 0.3, 1, 0.15, 0, 0.001, 0.002, 0, 1.5, 800, 0.1, 0.25, 1, 0.4, 0.5, 0.3, 0, -1 };  // 30.62
    if (name == "Resonator Pluck") return { 1, 0.7, 0.3, 0.5, 1, 0.001, 0.01, 0, 1.5, 20000, 0.15, 0.25, 2, 1.5, 0, -1, 0, -0.3 };  // 75.93
    if (name == "Scrape FX") return { 3, 0.15, 1, 0, 0, 0.03, 1, 0, 0.3, 20000, 1, 1, 0, 1.5, 0.75, 0, 0, 0 };  // 24.06
    if (name == "Solo Cello") return { 4, 0, 0, 0, 0.7, 0.3, 0.03, 0, 1.5, 12000, 1, 1, 4, 1.5, 0.75, 1, 1, 0 };  // 23.76
    if (name == "String Drum") return { 3, 0.5, 0.15, 0.15, 0.7, 0.03, 0.01, 0, 1.5, 300, 0.22, 0.25, 4, 0.2, 0.75, 0, 0, -1 };  // 43.52
    if (name == "Vinyl Crackle") return { 3, 0.5, 0.5, 0.15, 1, 0.3, 1, 0.3, 1, 20000, 1, 0.25, 3, 0.2, 0, -1, 0.5, 0.3 };  // 13.15
    static const std::map<juce::String, int> modelOfPreset = {
        { "Resonator Pluck", 5 }, { "Nylon Guitar", 5 }, { "Pluck Bass", 5 }, { "Koto Snap", 5 }, { "String Drum", 5 },
        { "Grain Cloud", 6 }, { "Vinyl Crackle", 6 }, { "Pitched Rain", 6 }, { "Hiss Riser", 6 }, { "Dust Hat", 6 },
        { "Solo Cello", 7 }, { "Glass Bow", 7 }, { "Scrape FX", 7 }, { "Bowed Bass", 7 } };
    const auto it = modelOfPreset.find(name);
    const int m = it != modelOfPreset.end() ? it->second : 5;
    // Starts: a struck string (an FM noise burst into STRG), dust (the whisper into a short comb),
    // a bowed string (a saw gripping STRG).
    if (m == 5)
        return { 1, 1, 0.3f, 0.5f, 1, 0.001f, 0.01f, 0, 0.02f, 20000, 0.8f, 1, 0, 1.5f, 0, 0, 0, 0 };
    if (m == 6)
        return { 3, 0.5f, 0.5f, 0.0f, 1, 0.3f, 1, 1, 1, 20000, 0.8f, 1, 2, 0.1f, 0, 0, 0, 0 };
    return { 0, 0, 0.5f, 0.5f, 0, 0.1f, 0.3f, 0.9f, 0.3f, 20000, 0.8f, 1, 0, 1.5f, 0.75f, 0, 0, 0 };
}

static U4MacroTests u4MacroTests;
