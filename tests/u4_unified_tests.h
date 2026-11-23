#pragma once

#include "old/OldUnifiedVoice.h"
#include "../src/engine/UnifiedVoiceEngine.h"
#include "../src/project/PresetManager.h"
#include "../src/project/FactoryMultis.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <map>
#include <set>
#include <vector>

// -----------------------------------------------------------------------------
// U4-UV: the unified voice on the modulation router (ModRouter.h, PER ENGINE "unified voice").
//
// Old against new: tests/old/OldUnifiedVoice.h is UnifiedVoice.h and UnifiedVoiceEngine.h as
// they were at 46fedbc (namespace olduv). Both are given the same cells and the same gesture
// (keys, a chord, pad pressure, the mod strip, bend), the system random put in the same state
// before each render (the per-note spread, PHAS SLOP WSHP FVAR EVAR, is drawn from it in the same
// order by both), and their samples are compared:
//   MATCH  the same samples, bit for bit
//   CLOSE  stated before the first run, never changed after it: over the whole render the level
//          (RMS) within 1.0 dB; the level in 20 ms windows (those of the old render above -60 dBFS)
//          within 1.5 dB on average and 6 dB at worst; the brightness (RMS of the first difference
//          over RMS) within 10 %. The null (new - old) is reported, not judged: a phase that moved
//          leaves no null and the same sound.
//   LOST   anything else. "The rule can fail" below shows that it calls a deliberate loss LOST.
// What is CLOSE or LOST is written as WAV pairs to NEPTUNE_U4_ATLAS (.ralph/atlas/U4-UV/).
//   NEPTUNE_UV_PRINT=1  prints every row
//   NEPTUNE_UV_BENCH=1  the cost (Release binary): old against new, alternated, best of seven
//   NEPTUNE_UV_FLAKE=1  the "Preset Audition" flake on unified-voice presets, measured
// -----------------------------------------------------------------------------
namespace u4uv
{
    inline juce::String env(const char* name) { return juce::SystemStats::getEnvironmentVariable(name, {}); }

    // The system random in a known state (as "P01 Keyboard" does; a Debug build asserts once).
    inline void seedSystemRandom(juce::int64 state)
    {
        auto& rng = juce::Random::getSystemRandom();
        juce::Random copy(rng.getSeed());
        const juce::int64 drawn = copy.nextInt64();
        rng.combineSeed(state ^ drawn ^ copy.getSeed());
        if (rng.getSeed() != state)
            rng.setSeed(state);
    }

    // The gesture: every part of the voice's own path a player reaches.
    struct Play
    {
        double seconds = 2.4;
        bool chord = true, pressure = true, strip = true, bend = true;
        std::function<float (int)> input;           // the track input (both sides), or none
    };

    // Left then right, `seconds` long.
    inline std::vector<float> render(EngineBase& e, double sr, int block, const Play& play, juce::int64 seed = 0x5eed1234)
    {
        e.prepareToPlay(sr, block);
        e.setTempo(120.0f);
        seedSystemRandom(seed);
        const int total = (int) (play.seconds * sr);
        std::vector<float> l, r;
        l.reserve((size_t) total + (size_t) block);
        r.reserve((size_t) total + (size_t) block);
        std::vector<float> inL((size_t) block), inR((size_t) block);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        auto at = [&](double t) { return (int) (t * sr); };
        bool on48 = false, on55 = false, on60 = false, on84 = false, off = false, off84 = false;
        for (int done = 0; done < total; done += block)
        {
            const double t = (double) done / sr;
            if (! on48 && done >= at(0.0))  { e.noteOn(48, 0.9f); on48 = true; }
            if (play.chord && ! on55 && done >= at(0.25)) { e.noteOn(55, 0.6f); e.noteOn(60, 0.75f); on55 = on60 = true; }
            if (! on84 && done >= at(1.0))  { e.noteOn(84, 1.0f); on84 = true; }
            if (on84 && ! off84 && done >= at(1.15)) { e.noteOff(84); off84 = true; }
            if (play.pressure && t >= 0.5 && t < 1.2)
                e.aftertouch(55, (float) std::min(1.0, (t - 0.5) / 0.3) * (t < 1.0 ? 1.0f : 0.4f));
            if (play.strip)
                e.setModWheel(t < 0.9 ? 0.0f : (t < 1.2 ? (float) ((t - 0.9) / 0.3) : (t < 1.5 ? (float) ((1.5 - t) / 0.3) : 0.0f)));
            if (play.bend)
                e.pitchBend(t < 1.6 ? 0.0f : (t < 1.75 ? 0.5f : (t < 1.85 ? -0.3f : 0.0f)));
            if (! off && done >= at(1.9)) { e.noteOff(48); e.noteOff(55); e.noteOff(60); off = true; }
            AudioInput in;
            if (play.input)
            {
                for (int i = 0; i < block; ++i)
                    inL[(size_t) i] = inR[(size_t) i] = play.input(done + i);
                in.left = inL.data();
                in.right = inR.data();
                in.frames = block;
                in.envelope = 0.5f;
            }
            e.setAudioInput(in);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block; ++i)
            {
                l.push_back(buf.getSample(0, i));
                r.push_back(buf.getSample(1, i));
            }
        }
        l.resize((size_t) total);
        r.resize((size_t) total);
        l.insert(l.end(), r.begin(), r.end());
        return l;
    }

    struct Verdict
    {
        juce::String word;
        double level = 0.0, envelopeMean = 0.0, envelopeWorst = 0.0, brightness = 0.0, null = 0.0;
        bool identical = false, finite = true;
    };

    inline double rmsOf(const float* x, size_t n)
    {
        double s = 0.0;
        for (size_t i = 0; i < n; ++i) s += (double) x[i] * x[i];
        return std::sqrt(s / (double) std::max<size_t>(1, n));
    }
    inline double db(double v) { return 20.0 * std::log10(v + 1.0e-12); }

    // The rule above (both sides, the worse of the two).
    inline Verdict compare(const std::vector<float>& a, const std::vector<float>& b, double sr)
    {
        Verdict v;
        v.identical = a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
        for (float x : b) v.finite = v.finite && std::isfinite(x);
        const size_t n = a.size() / 2;
        const size_t win = (size_t) (0.02 * sr);
        double nullSum = 0.0, oldSum = 0.0;
        for (int side = 0; side < 2; ++side)
        {
            const float* x = a.data() + side * n;
            const float* y = b.data() + side * n;
            const double la = rmsOf(x, n), lb = rmsOf(y, n);
            if (la < 1.0e-6 && lb < 1.0e-6)
                continue;
            v.level = std::max(v.level, std::abs(db(lb) - db(la)));
            double sum = 0.0;
            int count = 0;
            for (size_t w = 0; w + win <= n; w += win)
            {
                const double wa = rmsOf(x + w, win);
                if (db(wa) < -60.0)
                    continue;
                const double d = std::abs(db(rmsOf(y + w, win)) - db(wa));
                sum += d;
                ++count;
                v.envelopeWorst = std::max(v.envelopeWorst, d);
            }
            v.envelopeMean = std::max(v.envelopeMean, count > 0 ? sum / count : 0.0);
            auto bright = [n](const float* s)
            {
                double d = 0.0, e = 0.0;
                for (size_t i = 1; i < n; ++i) { const double q = (double) s[i] - s[i - 1]; d += q * q; e += (double) s[i] * s[i]; }
                return std::sqrt(d / (e + 1.0e-20));
            };
            const double ba = bright(x), bb = bright(y);
            v.brightness = std::max(v.brightness, std::abs(bb / (ba + 1.0e-20) - 1.0));
            for (size_t i = 0; i < n; ++i) { const double q = (double) y[i] - x[i]; nullSum += q * q; oldSum += (double) x[i] * x[i]; }
        }
        v.null = 10.0 * std::log10((nullSum + 1.0e-30) / (oldSum + 1.0e-30));
        const bool close = v.finite && v.level <= 1.0 && v.envelopeMean <= 1.5 && v.envelopeWorst <= 6.0 && v.brightness <= 0.10;
        v.word = v.identical ? "MATCH" : (close ? "CLOSE" : "LOST");
        return v;
    }

    inline void writeWav(const juce::String& name, const std::vector<float>& sound, double sr)
    {
        const auto dir = env("NEPTUNE_U4_ATLAS");
        if (dir.isEmpty() || sound.empty())
            return;
        juce::File folder(dir);
        folder.createDirectory();
        juce::File file = folder.getChildFile(juce::File::createLegalFileName(name) + ".wav");
        file.deleteFile();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), sr, 2, 24, {}, 0));
        if (writer == nullptr)
            return;
        stream.release();
        const size_t n = sound.size() / 2;
        const float* channels[2] = { sound.data(), sound.data() + n };
        writer->writeFromFloatArrays(channels, 2, (int) n);
    }

    // A sound given to an engine as a stored track gives it: the architecture first, then the
    // engine's cells in id order (the project and preset path).
    inline void applySound(EngineBase& e, const ParamMap& sound)
    {
        auto arch = sound.find(UnifiedParam::ARCH);
        if (arch != sound.end())
            e.setParameter(UnifiedParam::ARCH, arch->second);
        for (auto& [id, value] : sound)
            if (id != UnifiedParam::ARCH && (EngineParamRange::contains(id)))
                e.setParameter(id, value);
    }

    // A case: what is set on both engines (the same cells), and how it is played.
    struct Case
    {
        juce::String name;
        std::function<void (EngineBase&, bool isNew)> set;
        Play play;
        juce::String expected;      // "MATCH": must be the same samples; "CLOSE": may not be LOST
        bool heard = true;          // the new render differs from the plain sound's (the case tests something)
        double maxNull = 0.0;       // below 0: the null must be at least this deep (see featureCases)
    };

    // The free slots' DEST of the old engine (the LFOs' list, then AMT1 ..) and of the new one (the
    // router's table, PM RING SYNC ETIM FTIM, L1RT .., then AMT1 ..): the same entry by name.
    inline int amountDest(bool isNew, int slot)
    {
        return isNew ? UnifiedVoice::NUM_ROUTER_DESTS + 8 + slot : (int) UnifiedVoice::LfoDest::Count + slot;
    }
    inline void slot(EngineBase& e, int s, int src, int dest, float amount, int via = 0)
    {
        e.setParameter(ModSlotParam::id(s, ModSlotParam::SRC), (float) src);
        e.setParameter(ModSlotParam::id(s, ModSlotParam::DEST), (float) dest);
        e.setParameter(ModSlotParam::id(s, ModSlotParam::AMT), amount);
        e.setParameter(ModSlotParam::id(s, ModSlotParam::VIA), (float) via);
    }
    inline void lfo(EngineBase& e, int l, UnifiedVoice::LfoDest dest, float depth, float hz, LFO::Shape shape = LFO::Shape::Sine,
                    LFO::TriggerMode trig = LFO::TriggerMode::KeySync)
    {
        if (e.getParameter(ModParam::LFO_COUNT) < (float) (l + 1))
            e.setParameter(ModParam::LFO_COUNT, (float) (l + 1));
        e.setParameter(ModParam::lfoParamId(l, ModParam::LFO_RATE), hz);
        e.setParameter(ModParam::lfoParamId(l, ModParam::LFO_SHAPE), (float) (int) shape);
        e.setParameter(ModParam::lfoParamId(l, ModParam::LFO_TRIG), (float) (int) trig);
        e.setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEPTH), depth);
        e.setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEST), (float) (int) dest);
    }
    inline int paramDest(const char* label)
    {
        for (int i = 0; i < (int) UnifiedVoice::LfoDest::Count; ++i)
            if (juce::String(UnifiedVoice::lfoDestLabels()[i]) == label)
                return i;
        jassertfalse;
        return 0;
    }

    inline std::function<float (int)> sineInput(double hz, float amp, double sr)
    {
        return [=](int n) { return amp * (float) std::sin(6.283185307179586 * hz * (double) n / sr); };
    }
}

class U4UnifiedTests : public juce::UnitTest
{
public:
    U4UnifiedTests() : UnitTest("U4 Unified") {}

    using Case = u4uv::Case;
    using D = UnifiedVoice::LfoDest;

    struct Tally { int match = 0, close = 0, lost = 0; };

    // Old against new for one case at one rate and block: the verdict, judged and (where asked) printed.
    u4uv::Verdict pair(const Case& c, double sr, int block, Tally& tally, bool atlas = true)
    {
        olduv::UnifiedVoiceEngine oldEngine;
        UnifiedVoiceEngine newEngine;
        c.set(oldEngine, false);
        c.set(newEngine, true);
        const auto a = u4uv::render(oldEngine, sr, block, c.play);
        const auto b = u4uv::render(newEngine, sr, block, c.play);
        const auto v = u4uv::compare(a, b, sr);
        const juce::String where = c.name + " @" + juce::String((int) sr) + "/" + juce::String(block);
        expect(u4uv::rmsOf(a.data(), a.size()) > 1.0e-4, where + ": the old render sounds");
        expect(v.finite, where + ": finite");
        if (c.expected == "MATCH")
            expect(v.identical, where + ": the same samples, bit for bit (" + v.word + ", null " + juce::String(v.null, 1) + " dB)");
        else
            expect(v.word != "LOST", where + ": " + v.word + " level " + juce::String(v.level, 2) + " dB, envelope "
                   + juce::String(v.envelopeMean, 2) + " / " + juce::String(v.envelopeWorst, 2) + " dB, brightness "
                   + juce::String(v.brightness * 100.0, 1) + " %");
        if (v.word == "MATCH") ++tally.match; else if (v.word == "CLOSE") ++tally.close; else ++tally.lost;
        if (u4uv::env("NEPTUNE_UV_PRINT").isNotEmpty())
            logMessage(juce::String(v.word).paddedRight(' ', 6) + where.paddedRight(' ', 52) + " level " + juce::String(v.level, 2)
                       + " env " + juce::String(v.envelopeMean, 2) + "/" + juce::String(v.envelopeWorst, 2) + " bright "
                       + juce::String(v.brightness * 100.0, 1) + "% null " + juce::String(v.null, 1) + " dB");
        if (c.maxNull < 0.0)
            expect(v.null <= c.maxNull, where + ": the old arithmetic up to rounding and the router's step (null " + juce::String(v.null, 1) + " dB)");
        if (c.heard)
        {
            // The case tests something: the new render is not the plain sound's.
            UnifiedVoiceEngine plain;
            u4uv::Play quiet = c.play;
            const auto p = u4uv::render(plain, sr, block, quiet);
            expect(p != b, where + ": heard (the new render differs from the plain sound)");
        }
        if (atlas && ! v.identical)
        {
            u4uv::writeWav(where + "_old", a, sr);
            u4uv::writeWav(where + "_new", b, sr);
        }
        return v;
    }

    // A sound that sets a cell of the router - a route of the STRIP page (158-165) or of the XIN
    // page (166-173), a free slot (700 up) - plays what the old engine had no way of playing (it
    // has no such pages: the strip there is its built-in vibrato, and the ids mean nothing to it):
    // there is no old sound to hold it against. "Fusion Presets" and "Factory Multis" measure
    // what those routes do.
    static bool usesTheRouter(const ParamMap& sound)
    {
        for (auto& [id, value] : sound)
            if (value != 0.0f && ((id >= UnifiedVoiceEngine::STRIP_IDS && id < UnifiedVoiceEngine::XIN_IDS + 8) || mod::Router::ownsSlotBlock(id)))
                return true;
        return false;
    }

    static std::vector<Case> presetCases()
    {
        std::vector<Case> out;
        PresetManager all;
        for (int i = 0; i < all.getPresetCount(); ++i)
        {
            const auto& p = all.getPreset(i);
            if (p.engineType != EngineType::UnifiedVoice || usesTheRouter(p.soundParams))
                continue;
            const ParamMap sound = PresetManager::soundOf(p);
            out.push_back({ "preset " + p.name, [sound](EngineBase& e, bool) { u4uv::applySound(e, sound); }, {}, "CLOSE" });
        }
        for (auto& multi : FactoryMultis::getAll())
            for (auto& m : multi.members)
                if (m.preset.engineType == EngineType::UnifiedVoice && ! usesTheRouter(m.preset.soundParams))
                {
                    const ParamMap sound = PresetManager::soundOf(m.preset);
                    out.push_back({ "multi " + multi.name + " " + m.preset.name, [sound](EngineBase& e, bool) { u4uv::applySound(e, sound); }, {}, "CLOSE" });
                }
        return out;
    }

    // Every feature the port touched, alone on the init sound (two saws, the filter, both envelopes).
    static void brighten(EngineBase& e) { e.setParameter(UnifiedParam::CUTOFF, 3000.0f); e.setParameter(UnifiedParam::RESONANCE, 0.3f); }

    static std::vector<Case> featureCases()
    {
        using namespace UnifiedParam;
        using u4uv::lfo;
        using u4uv::slot;
        std::vector<Case> c;
        auto add = [&c](const char* name, std::function<void (EngineBase&, bool)> f, const char* expected, u4uv::Play play = {}, bool heard = true)
        {
            c.push_back({ name, std::move(f), play, expected, heard });
        };

        // ---- What must stay bit for bit: the classic path, the voice's own pressure / strip / bend, EVAR
        add("plain init sound", [](EngineBase&, bool) {}, "MATCH", {}, false);
        add("EVAR FVAR SLOP WSHP up, stage curves (setTimeScale for the scaled copy)", [](EngineBase& e, bool)
        {
            e.setParameter(ENV_VAR, 1.0f); e.setParameter(FILTER_VAR, 0.6f); e.setParameter(SLOP, 20.0f); e.setParameter(WAVE_VAR, 0.5f);
            e.setParameter(AMP_ATTACK, 0.2f); e.setParameter(AMP_DECAY, 0.4f); e.setParameter(AMP_SUSTAIN, 0.4f); e.setParameter(AMP_RELEASE, 0.5f);
            e.setParameter(FENV_ATTACK, 0.05f); e.setParameter(FENV_DECAY, 0.6f); e.setParameter(FILTER_ENV, 0.5f);
            e.setParameter(StageCurveParam::id(0, StageCurveParam::Decay), 0.6f);
            e.setParameter(StageCurveParam::id(1, StageCurveParam::Attack), -0.5f);
        }, "MATCH");
        add("EVAR set, then 0 (the draw that restores)", [](EngineBase& e, bool) { e.setParameter(ENV_VAR, 0.8f); e.setParameter(ENV_VAR, 0.0f); }, "MATCH", {}, false);
        add("classic: LFO1 WHL 0.6 on CUT (WheelRoute::to)", [](EngineBase& e, bool)
        {
            lfo(e, 0, D::Cutoff, 0.0f, 40.0f, LFO::Shape::ExpUp, LFO::TriggerMode::Single);
            e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.6f);
        }, "MATCH");
        add("classic: LFO1 VEL 0.5 on PTCH, LFO2 PRES 0.7 on PW", [](EngineBase& e, bool)
        {
            lfo(e, 0, D::Pitch, 0.0f, 5.0f);
            e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_VEL), 0.5f);
            lfo(e, 1, D::PulseWidth, 0.1f, 3.0f, LFO::Shape::Triangle);
            e.setParameter(ModParam::lfoExt2ParamId(1, ModParam::LFO_PRES), 0.7f);
        }, "MATCH");
        add("classic: LFO1 WHL on MORP (a page cell) and LFO2 on RAT1", [](EngineBase& e, bool)
        {
            e.setParameter(MODEL_A, (float) FilterSlot::indexFromModel(FilterModel::SEM)); lfo(e, 0, (D) u4uv::paramDest("MORP"), 0.2f, 2.0f);
            e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f);
            lfo(e, 1, D::Lfo1Rate, 0.0f, 0.5f, LFO::Shape::Square);
            e.setParameter(ModParam::lfoExt2ParamId(1, ModParam::LFO_WHEEL), 0.8f);
        }, "MATCH");
        add("extra envelopes: chain on CUT", [](EngineBase& e, bool)
        {
            e.setParameter(ModParam::ENV_COUNT, 2.0f);
            e.setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), 1.0f);
            e.setParameter(ModParam::envParamId(0, ModParam::ENV_DEPTH), 0.6f);
        }, "MATCH");
        add("extra envelopes: DEST RESO (an LFO destination)", [](EngineBase& e, bool)
        {
            e.setParameter(ModParam::ENV_COUNT, 1.0f);
            e.setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), 4.0f);
            e.setParameter(ModParam::envParamId(0, ModParam::ENV_DEPTH), 0.8f);
        }, "MATCH");
        add("pressure: VOL (the default route) and the strip's own vibrato", [](EngineBase&, bool) {}, "MATCH", {}, false);
        add("pressure routes CUT FM TIMB WAVE VIB", [](EngineBase& e, bool)
        {
            auto* r = new AftertouchRouter();       // (kept for the engine's life: a test's leak)
            r->setRoute(0, { AftertouchRouter::Destination::FilterCutoff, 0.5f, false });
            r->setRoute(1, { AftertouchRouter::Destination::FMDepth, 0.4f, false });
            r->setRoute(2, { AftertouchRouter::Destination::MacroTimbre, 0.6f, true });
            r->setRoute(3, { AftertouchRouter::Destination::Vibrato, 0.5f, false });
            e.setAftertouchRouter(r);
        }, "MATCH");
        add("legacy XIN RING without an input: the voice alone", [](EngineBase& e, bool)
        {
            e.setParameter(XIN, 2.0f);
            e.setParameter(XIN_DEPTH, 0.8f);
        }, "MATCH", {}, false);

        // ---- What the router now does (CLOSE: its control rate, its leads, its sums)
        add("LFO1 > PTCH 0.1, 5 Hz", [](EngineBase& e, bool) { lfo(e, 0, D::Pitch, 0.1f, 5.0f); }, "CLOSE");
        add("LFO1 > CUT 0.5, 2 Hz triangle", [](EngineBase& e, bool) { brighten(e); lfo(e, 0, D::Cutoff, 0.5f, 2.0f, LFO::Shape::Triangle); }, "CLOSE");
        add("LFO1 > AMP 0.6, 4 Hz square", [](EngineBase& e, bool) { lfo(e, 0, D::Amp, 0.6f, 4.0f, LFO::Shape::Square); }, "CLOSE");
        add("LFO1 > PW 0.8, 0.5 Hz", [](EngineBase& e, bool) { lfo(e, 0, D::PulseWidth, 0.8f, 0.5f); }, "CLOSE");
        add("LFO1 > PAN 1, 1 Hz", [](EngineBase& e, bool) { lfo(e, 0, D::Pan, 1.0f, 1.0f); }, "CLOSE");
        add("LFO1 > MIX 0.8, 1.5 Hz", [](EngineBase& e, bool) { lfo(e, 0, D::OscMix, 0.8f, 1.5f); }, "CLOSE");
        add("LFO1 > RESO 0.8, 3 Hz", [](EngineBase& e, bool) { brighten(e); lfo(e, 0, D::Resonance, 0.8f, 3.0f); }, "CLOSE");
        add("LFO1 > XMOD 0.7, 1 Hz (oscillator 2 out of the mix)", [](EngineBase& e, bool)
        {
            e.setParameter(MIX, 0.0f); e.setParameter(WAVE2, 0.0f); lfo(e, 0, D::CrossMod, 0.7f, 1.0f);
        }, "CLOSE");
        add("LFO1 > LVL1 0.9, LFO2 > NOIS (noise 0.3)", [](EngineBase& e, bool)
        {
            e.setParameter(NOISE, 0.3f); lfo(e, 0, D::Osc1Level, 0.9f, 2.0f); lfo(e, 1, D::NoiseLevel, 0.8f, 3.0f);
        }, "CLOSE");
        add("LFO1 > CUTB, LFO2 > PIT2 (two filters)", [](EngineBase& e, bool)
        {
            e.setParameter(MODEL_B, 0.0f); e.setParameter(ROUTING, 2.0f); e.setParameter(CUTOFF, 2000.0f);
            lfo(e, 0, D::CutoffB, 0.4f, 1.0f); lfo(e, 1, D::Osc2Pitch, 0.05f, 4.0f);
        }, "CLOSE");
        add("LFO1 > FFM, filter FM 0.2", [](EngineBase& e, bool) { e.setParameter(FILTER_FM, 0.2f); lfo(e, 0, D::FilterFM, 0.5f, 1.0f); }, "CLOSE");
        add("LFO1 > MORP (a page cell, SEM filter)", [](EngineBase& e, bool) { brighten(e); e.setParameter(MODEL_A, (float) FilterSlot::indexFromModel(FilterModel::SEM)); lfo(e, 0, (D) u4uv::paramDest("MORP"), 0.8f, 1.0f); }, "CLOSE");
        add("LFO1 > DET2 and LFO2 > DRIV (page cells)", [](EngineBase& e, bool)
        {
            lfo(e, 0, (D) u4uv::paramDest("DET2"), 0.3f, 0.7f); lfo(e, 1, (D) u4uv::paramDest("DRIV"), 0.6f, 2.0f);
        }, "CLOSE");
        add("LFO1 > 1A on a wavetable slot (position)", [](EngineBase& e, bool)
        {
            e.setParameter(WAVE1, 6.0f); e.setParameter(MIX, 0.0f); lfo(e, 0, (D) u4uv::paramDest("1A"), 0.8f, 1.0f);
        }, "CLOSE");
        add("LFO1 > 1B on an FM slot (ratio)", [](EngineBase& e, bool)
        {
            e.setParameter(WAVE1, 7.0f); e.setParameter(MIX, 0.0f); e.setParameter(SRC_A1, 0.5f);
            lfo(e, 0, (D) u4uv::paramDest("1B"), 0.4f, 0.5f, LFO::Shape::Triangle);
        }, "CLOSE");
        add("LFO1 > ATK and LFO2 > FDEC (envelope cells)", [](EngineBase& e, bool)
        {
            brighten(e); e.setParameter(FILTER_ENV, 0.5f);
            lfo(e, 0, (D) u4uv::paramDest("ATK"), 0.5f, 0.3f); lfo(e, 1, (D) u4uv::paramDest("FDEC"), 0.5f, 0.4f);
        }, "CLOSE");
        add("LFO2 > RAT1 while LFO1 > CUT", [](EngineBase& e, bool)
        {
            brighten(e); lfo(e, 0, D::Cutoff, 0.5f, 3.0f); lfo(e, 1, D::Lfo1Rate, 0.6f, 0.4f);
        }, "CLOSE");
        add("LFO2 > DPT1 while LFO1 > AMP", [](EngineBase& e, bool)
        {
            lfo(e, 0, D::Amp, 0.5f, 5.0f); lfo(e, 1, D::Lfo1Depth, 0.8f, 0.5f, LFO::Shape::Triangle);
        }, "CLOSE");
        add("LFO1 tempo-synced, delay and fade, key-tracked > CUT", [](EngineBase& e, bool)
        {
            brighten(e); lfo(e, 0, D::Cutoff, 0.6f, 2.0f);
            e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_SYNC), 1.0f);
            e.setParameter(ModParam::lfoExtParamId(0, ModParam::LFO_DIV), 9.0f);
            e.setParameter(ModParam::lfoExtParamId(0, ModParam::LFO_DELAY), 0.2f);
            e.setParameter(ModParam::lfoExtParamId(0, ModParam::LFO_FADE), 0.3f);
            e.setParameter(ModParam::lfoExtParamId(0, ModParam::LFO_PHASE), 0.25f);
            e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_KTRK), 0.5f);
        }, "CLOSE");
        add("slot VEL > CUT 0.6", [](EngineBase& e, bool) { brighten(e); slot(e, 0, ModSrc::Velocity, (int) D::Cutoff, 0.6f); }, "CLOSE");
        add("slot ENV > PW 0.7", [](EngineBase& e, bool) { slot(e, 0, ModSrc::Env, (int) D::PulseWidth, 0.7f); }, "CLOSE");
        add("slot FENV > MORP, slot KEY > CUTB", [](EngineBase& e, bool)
        {
            brighten(e); e.setParameter(MODEL_B, 0.0f); e.setParameter(ROUTING, 1.0f);
            e.setParameter(MODEL_A, (float) FilterSlot::indexFromModel(FilterModel::SEM)); slot(e, 0, ModSrc::FilterEnv, u4uv::paramDest("MORP"), 0.7f); slot(e, 1, ModSrc::Key, (int) D::CutoffB, -0.5f);
        }, "CLOSE");
        add("slot LFO1 > AMP (LFO1 aimed nowhere), slot LFO2 > PTCH via VEL", [](EngineBase& e, bool)
        {
            lfo(e, 0, D::Off, 0.3f, 6.0f); lfo(e, 1, D::Off, 0.3f, 5.0f);
            slot(e, 0, ModSrc::Lfo1, (int) D::Amp, 0.5f); slot(e, 1, ModSrc::Lfo2, (int) D::Pitch, 0.1f, ModSrc::Velocity);
        }, "CLOSE");
        add("slot PRES > AMP and STRP > CUT", [](EngineBase& e, bool)
        {
            brighten(e); slot(e, 0, ModSrc::Pressure, (int) D::Amp, -0.6f); slot(e, 1, ModSrc::Strip, (int) D::Cutoff, 0.5f);
        }, "CLOSE");
        add("slot LFO1 > AMT2 while slot 2 VEL > CUT", [](EngineBase& e, bool isNew)
        {
            brighten(e); lfo(e, 0, D::Off, 0.3f, 3.0f);
            slot(e, 0, ModSrc::Lfo1, u4uv::amountDest(isNew, 1), 0.6f); slot(e, 1, ModSrc::Velocity, (int) D::Cutoff, 0.4f);
        }, "CLOSE");
        add("slot IN (the follower) > CUT", [](EngineBase& e, bool) { brighten(e); slot(e, 0, ModSrc::Input, (int) D::Cutoff, 0.6f); }, "CLOSE",
            [] { u4uv::Play p; p.input = u4uv::sineInput(220.0, 0.5f, 48000.0); return p; }());
        add("legacy XIN PM 0.6, a 220 Hz input", [](EngineBase& e, bool) { e.setParameter(XIN, 1.0f); e.setParameter(XIN_DEPTH, 0.6f); }, "CLOSE",
            [] { u4uv::Play p; p.input = u4uv::sineInput(220.0, 0.7f, 48000.0); return p; }());
        add("legacy XIN RING 0.8, a 300 Hz input", [](EngineBase& e, bool) { e.setParameter(XIN, 2.0f); e.setParameter(XIN_DEPTH, 0.8f); }, "CLOSE",
            [] { u4uv::Play p; p.input = u4uv::sineInput(300.0, 0.8f, 48000.0); return p; }());
        add("legacy XIN SYNC 0.7, a 130 Hz input", [](EngineBase& e, bool)
        {
            e.setParameter(XIN, 3.0f); e.setParameter(XIN_DEPTH, 0.7f); e.setParameter(MIX, 0.0f);
        }, "CLOSE", [] { u4uv::Play p; p.input = u4uv::sineInput(130.0, 0.8f, 48000.0); return p; }());

        // Added after the first mutation run (M22, M25 lived through the CLOSE rule, which hears
        // level, envelope and brightness, not a wavetable position or a PM index): the rows whose
        // old and new arithmetic are the same up to rounding and the control-rate step (their null
        // was -59 to -147 dB in the first run) must stay below -40 dB. The CLOSE rule is unchanged.
        for (auto& k : c)
            for (const char* strict : { "LFO1 > CUT 0.5", "LFO1 > PW", "LFO1 > PAN", "LFO1 > MIX", "LFO1 > XMOD", "LFO1 > LVL1",
                                        "LFO1 > CUTB", "LFO1 > FFM", "LFO1 > 1A", "LFO1 > RESO", "legacy XIN PM", "legacy XIN RING 0.8",
                                        "legacy XIN SYNC", "slot ENV > PW", "LFO1 > PTCH", "LFO1 > ATK" })
                if (k.name.startsWith(strict))
                    k.maxNull = -40.0;
        return c;
    }

    void runTest() override
    {
        using namespace UnifiedParam;
        const double SR = 48000.0;

        beginTest("Ids and pages: the LFO cells where they were, STRIP at 158 and XIN at 166, the slots at 700");
        {
            UnifiedVoiceEngine e;
            juce::StringArray names;
            const auto pages = e.getParamPages();
            for (auto& p : pages) names.add(p.name);
            expect(! names.contains("XIN") || names.indexOf("XIN") > names.indexOf("AMP"), "the old XIN page is gone; the router's comes after the voice's own");
            expectEquals(names[names.size() - 1], juce::String("STRIP"));
            expectEquals(names.indexOf("STRIP"), names.indexOf("LSHP") + 1);
            expectEquals(names.indexOf("SLT1"), names.indexOf("XIN") + 1);
            for (auto& p : pages)
            {
                if (p.name == "STRIP")
                    for (int c = 0; c < 8; ++c) expectEquals(p.params[(size_t) c].id, 158 + c);
                if (p.name == "XIN")
                    for (int c = 0; c < 8; ++c) expectEquals(p.params[(size_t) c].id, 166 + c);
                if (p.name == "STRIP" || p.name == "XIN")
                    for (auto& cell : p.params)
                    {
                        expectEquals(e.getParameter(cell.id), cell.defaultVal, p.name + " " + cell.label + " starts at its default");
                        expect(e.getParameterName(cell.id).isNotEmpty());
                    }
                for (auto& cell : p.params)
                    expect(! cell.isValid() || cell.id < 174 || cell.id >= 190, "no page cell on the stored LFO ids 174-189");
            }
            // The STRIP and XIN cells are the router's.
            e.setParameter(158, 2.0f);
            e.setParameter(159, 0.4f);
            expectEquals(e.getRouter().get(mod::Router::stripDestId(0)), 2.0f);
            expectEquals(e.getParameter(159), 0.4f);
            e.setParameter(166 + mod::Router::XinDest1, (float) UnifiedVoice::RPm);
            expectEquals(e.getRouter().get(mod::Router::xinId(mod::Router::XinDest1)), (float) UnifiedVoice::RPm);
            // The stored LFO cells WHL / VEL / KTRK / PRES at 174-189 stay the engine's.
            e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f);
            expectEquals(e.getParameter(174), 0.5f);
            expect(e.lfoOnClassicPath(0), "an LFO with WHL runs the classic path");
            expectEquals(e.getRouter().get(mod::Router::stripDestId(0)), 2.0f, "and the strip page is not touched by it");
            e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.0f);
            expect(! e.lfoOnClassicPath(0), "... and the router's once the amount is 0");
        }

        beginTest("The router's table is the LFOs' list, index for index, then PM RING SYNC ETIM FTIM");
        {
            const auto& t = UnifiedVoiceEngine::routerDests();
            expectEquals(t.visible(), (int) UnifiedVoice::NUM_ROUTER_DESTS);
            for (int i = 0; i < (int) D::Count; ++i)
                expectEquals(juce::String(t.labels()[i]), juce::String(UnifiedVoice::lfoDestLabels()[i]), "entry " + juce::String(i));
            const char* const added[5] = { "PM", "RING", "SYNC", "ETIM", "FTIM" };
            for (int i = 0; i < 5; ++i)
                expectEquals(juce::String(t.labels()[(int) D::Count + i]), juce::String(added[i]));
            expectEquals(t.ampTime(), (int) UnifiedVoice::REnvTime);
            expectEquals(t.filterTime(), (int) UnifiedVoice::RFilterTime);
            expectEquals(t.pitch(), (int) UnifiedVoice::RPitch);
            for (int i = 0; i < UnifiedVoice::NUM_PARAM_DESTS; ++i)
            {
                const int at = UnifiedVoice::RFirstParam + i;
                const bool src = at >= UnifiedVoice::RSrcA1 && at <= UnifiedVoice::RSrcB3;
                expectEquals(t[at].id, src ? -1 : UnifiedVoice::PARAM_DESTS[i].id, juce::String(UnifiedVoice::PARAM_DESTS[i].label));
            }
            for (int l = 0; l < 4; ++l)
            {
                expect(t[(int) D::Lfo1Rate + l].cross == 1 && t[(int) D::Lfo1Rate + l].lfo == l);
                expect(t[(int) D::Lfo1Depth + l].cross == 2 && t[(int) D::Lfo1Depth + l].lfo == l);
            }
            expectEquals(UnifiedVoiceEngine::sourceCount(), 43);
            const auto& s = mod::SourceTable::standard(4, 4);
            for (int i = 0; i <= ModSrc::Input; ++i)
                expectEquals(juce::String(s.labels()[i]), juce::String(ModSrc::labels()[i]), "the old SRC list up to IN, place for place");
            expectEquals(s.indexOf(mod::Src::Lfo3), 17, "LFO3 moved from 11 (format 3 maps it)");
            expectEquals(s.indexOf(mod::Src::Env2), 19);
        }

        beginTest("The rule can fail: a deliberate loss is LOST");
        {
            Tally t;
            olduv::UnifiedVoiceEngine a;
            UnifiedVoiceEngine b, c;
            b.setParameter(CUTOFF, 4000.0f);
            c.setParameter(CUTOFF, 2000.0f);         // one octave down
            const auto x = u4uv::render(b, SR, 512, {});
            const auto y = u4uv::render(c, SR, 512, {});
            expect(u4uv::compare(x, y, SR).word == "LOST", "the cutoff an octave down: " + u4uv::compare(x, y, SR).word);
            UnifiedVoiceEngine d;
            u4uv::lfo(d, 0, D::Amp, 0.6f, 4.0f, LFO::Shape::Square);
            const auto z = u4uv::render(d, SR, 512, {});
            const auto plain = u4uv::render(a, SR, 512, {});
            expect(u4uv::compare(plain, z, SR).word == "LOST", "a tremolo where there was none: " + u4uv::compare(plain, z, SR).word);
            UnifiedVoiceEngine f;
            f.setParameter(LEVEL, 0.8f * 0.85f);     // -1.4 dB
            expect(u4uv::compare(plain, u4uv::render(f, SR, 512, {}), SR).word == "LOST", "1.4 dB quieter");
            UnifiedVoiceEngine g;
            expect(u4uv::compare(plain, u4uv::render(g, SR, 512, {}), SR).word == "MATCH", "the same is MATCH");
        }

        beginTest("Every unified-voice factory preset, old against new, 48 kHz");
        {
            Tally t;
            const auto cases = presetCases();
            for (auto& c : cases)
                pair(c, SR, 512, t);
            logMessage("presets: " + juce::String(cases.size()) + " rows, " + juce::String(t.match) + " MATCH, " + juce::String(t.close)
                       + " CLOSE, " + juce::String(t.lost) + " LOST");
            expect(cases.size() >= 24, "23 factory presets and the multi's member at 46fedbc");
            expectEquals(t.lost, 0);
        }

        beginTest("Every folded feature, old against new, 48 kHz");
        {
            Tally t;
            const auto cases = featureCases();
            for (auto& c : cases)
                pair(c, SR, 512, t);
            logMessage("features: " + juce::String(cases.size()) + " rows, " + juce::String(t.match) + " MATCH, " + juce::String(t.close)
                       + " CLOSE, " + juce::String(t.lost) + " LOST");
            expectEquals(t.lost, 0);
        }

        beginTest("44.1 and 96 kHz, host blocks of 37 and 100 (not whole sub-blocks)");
        {
            Tally t;
            const auto cases = featureCases();
            const auto presets = presetCases();
            std::vector<Case> some;
            for (auto& c : cases)
                if (c.name.startsWith("plain") || c.name.startsWith("EVAR") || c.name.startsWith("classic: LFO1 WHL")
                    || c.name.startsWith("pressure: VOL") || c.name.startsWith("LFO1 > CUT") || c.name.startsWith("slot VEL")
                    || c.name.startsWith("LFO1 > AMP") || c.name.startsWith("legacy XIN RING 0.8"))
                    some.push_back(c);
            for (size_t i = 0; i < presets.size(); i += 9)
                some.push_back(presets[i]);
            for (auto& c : some)
            {
                Case at44 = c, at96 = c;
                if (c.play.input)
                {
                    at44.play.input = u4uv::sineInput(300.0, 0.8f, 44100.0);
                    at96.play.input = u4uv::sineInput(300.0, 0.8f, 96000.0);
                }
                pair(at44, 44100.0, 37, t);
                pair(at96, 96000.0, 100, t);
            }
            logMessage("other rates: " + juce::String(2 * some.size()) + " rows, " + juce::String(t.match) + " MATCH, " + juce::String(t.close)
                       + " CLOSE, " + juce::String(t.lost) + " LOST");
            expectEquals(t.lost, 0);
        }

        beginTest("EVAR through Envelope::setTimeScale is the scaled copy, bit for bit, at every EVAR");
        {
            // The envelope alone: the old voice's scaled params against the new one's time scale.
            for (float evar : { 0.1f, 0.5f, 1.0f })
                for (int note = 0; note < 3; ++note)
                {
                    olduv::UnifiedVoiceEngine a;
                    UnifiedVoiceEngine b;
                    for (EngineBase* e : { (EngineBase*) &a, (EngineBase*) &b })
                    {
                        e->setParameter(ENV_VAR, evar);
                        e->setParameter(AMP_ATTACK, 0.003f + 0.1f * (float) note);
                        e->setParameter(AMP_DECAY, 0.002f);        // (under the 1 ms floor once scaled down)
                        e->setParameter(AMP_RELEASE, 0.25f);
                        e->setParameter(FENV_DECAY, 0.0015f);
                    }
                    u4uv::Play p;
                    p.strip = p.pressure = p.bend = false;
                    const auto x = u4uv::render(a, SR, 64, p, 77 + note);
                    const auto y = u4uv::render(b, SR, 64, p, 77 + note);
                    expect(x == y, "EVAR " + juce::String(evar) + " note " + juce::String(note));
                }
            // And ETIM on top multiplies the note's factor (new: there was no ETIM).
            UnifiedVoiceEngine e;
            u4uv::slot(e, 0, ModSrc::Velocity, UnifiedVoice::REnvTime, 1.0f);
            e.prepareToPlay(SR, 64);
            e.noteOn(60, 1.0f, -1);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            e.processBlock(buf, midi);
            expectWithinAbsoluteError(e.getVoice(0).getAmpEnvelope().getTimeScale(), 8.0f, 1.0e-3f, "VEL 1 on ETIM at 100: eight times as long");
            expectEquals(e.getVoice(0).getFilterEnvelope().getTimeScale(), 1.0f, "ETIM leaves the filter's envelope");
            UnifiedVoiceEngine f;
            u4uv::slot(f, 0, ModSrc::Velocity, UnifiedVoice::RFilterTime, -1.0f);
            f.setParameter(ENV_VAR, 0.5f);
            f.prepareToPlay(SR, 64);
            u4uv::seedSystemRandom(99);
            f.noteOn(60, 1.0f, -1);
            f.processBlock(buf, midi);
            const float evar = f.getVoice(0).getAmpEnvelope().getTimeScale();
            expect(evar != 1.0f, "EVAR drew a factor");
            expectWithinAbsoluteError(f.getVoice(0).getFilterEnvelope().getTimeScale(), evar / 8.0f, 1.0e-4f, "FTIM x the note's EVAR factor");
            // The slot taken away: the factor goes back to EVAR's alone (the set value, 1, handed back).
            f.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
            for (int b = 0; b < 4; ++b) f.processBlock(buf, midi);
            expectWithinAbsoluteError(f.getVoice(0).getFilterEnvelope().getTimeScale(), evar, 1.0e-6f, "let go: EVAR's factor alone");
        }

        beginTest("New with the router: pad pressure on PAN; the STRIP page takes the strip from the built-in vibrato");
        {
            // AT ROUTE PAN did nothing on this engine; now it is a slot (setPressureRoutes).
            auto balance = [&](bool routed)
            {
                UnifiedVoiceEngine e;
                AftertouchRouter routes;
                routes.setRoute(0, { AftertouchRouter::Destination::PanSpread, 1.0f, false });
                if (routed)
                    e.setAftertouchRouter(&routes);
                e.prepareToPlay(SR, 256);
                e.noteOn(60, 0.8f, -1);
                juce::AudioBuffer<float> buf(2, 256);
                juce::MidiBuffer midi;
                double l = 0.0, r = 0.0;
                for (int b = 0; b < 100; ++b)
                {
                    e.aftertouch(60, 1.0f);
                    buf.clear();
                    e.processBlock(buf, midi);
                    if (b >= 40)
                        for (int i = 0; i < 256; ++i) { l += std::abs(buf.getSample(0, i)); r += std::abs(buf.getSample(1, i)); }
                }
                e.setAftertouchRouter(nullptr);
                return l / (r + 1.0e-12);
            };
            expectWithinAbsoluteError(balance(false), 1.0, 0.01, "the default route (VOL): centred");
            expect(balance(true) < 0.05, "PAN 100 at full pressure: the voice on the right (" + juce::String(balance(true), 3) + ")");

            // A sine, the filter open, the strip held up: the built-in vibrato moves its pitch; with a
            // route on the STRIP page (here a tiny one, on RESO) the strip does that instead.
            auto wobble = [&](bool stripRoute)
            {
                UnifiedVoiceEngine e;
                e.setParameter(WAVE1, 0.0f);
                e.setParameter(MIX, 0.0f);
                e.setParameter(CUTOFF, 20000.0f);
                e.setParameter(DRIFT, 0.0f);
                for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                    e.getVoice(v).getAnalogCharacter().enabled = false;
                if (stripRoute)
                {
                    e.setParameter(UnifiedVoiceEngine::STRIP_IDS + 0, (float) UnifiedVoice::RReso);
                    e.setParameter(UnifiedVoiceEngine::STRIP_IDS + 1, 0.01f);
                }
                // One note, the strip held at the top for a second; the last half second measured.
                e.prepareToPlay(SR, 256);
                e.noteOn(57, 0.8f, -1);
                std::vector<float> out;
                juce::AudioBuffer<float> buf(2, 256);
                juce::MidiBuffer midi;
                for (int blk = 0; blk < (int) (1.0 * SR / 256); ++blk)
                {
                    e.setModWheel(1.0f);
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 256; ++i) out.push_back(buf.getSample(0, i));
                }
                std::vector<double> at;
                for (size_t i = out.size() / 2; i < out.size(); ++i)
                    if (out[i - 1] < 0.0f && out[i] >= 0.0f)
                        at.push_back((double) i - (double) out[i] / (double) (out[i] - out[i - 1]));
                double mean = 0.0, var = 0.0;
                for (size_t i = 1; i < at.size(); ++i) mean += at[i] - at[i - 1];
                mean /= (double) std::max<size_t>(1, at.size() - 1);
                for (size_t i = 1; i < at.size(); ++i) var += (at[i] - at[i - 1] - mean) * (at[i] - at[i - 1] - mean);
                return std::sqrt(var / (double) std::max<size_t>(1, at.size() - 1)) / mean;
            };
            const double vib = wobble(false), routed = wobble(true);
            expect(vib > 3.0 * routed && vib > 0.002, "the built-in vibrato " + juce::String(vib, 4) + ", with a STRIP route " + juce::String(routed, 4));
        }

        beginTest("EVAR turned to 0 between notes: the draw that puts the times back is made as it was (the system random's order)");
        {
            auto run = [&](EngineBase& e)
            {
                e.prepareToPlay(SR, 128);
                e.setParameter(ENV_VAR, 0.8f);
                e.setParameter(AMP_RELEASE, 0.05f);
                u4uv::seedSystemRandom(4242);
                std::vector<float> out;
                juce::AudioBuffer<float> buf(2, 128);
                juce::MidiBuffer midi;
                auto blocks = [&](int count) { for (int b = 0; b < count; ++b) { buf.clear(); e.processBlock(buf, midi); out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 128); } };
                e.noteOn(60, 0.8f);
                blocks(60);
                e.noteOff(60);
                blocks(30);
                e.setParameter(ENV_VAR, 0.0f);      // the next note draws once more, to put the times back
                for (int n : { 62, 65, 67 })
                {
                    e.noteOn(n, 0.7f);
                    blocks(40);
                    e.noteOff(n);
                    blocks(20);
                }
                return out;
            };
            olduv::UnifiedVoiceEngine a;
            UnifiedVoiceEngine b;
            expect(run(a) == run(b), "the same samples, bit for bit");
        }

        beginTest("Pitch on the router: an LFO's depth on PTCH and PIT2 is 12 semitones a unit, as it was");
        {
            // A sine, nothing else moving; the LFO a slow sine. The swing of the pitch is read from
            // the zero crossings (the period at its longest and shortest), old against new.
            auto swing = [&](EngineBase& e, bool second)
            {
                e.setParameter(WAVE1, 0.0f);
                e.setParameter(WAVE2, 0.0f);
                e.setParameter(MIX, second ? 1.0f : 0.0f);
                e.setParameter(CUTOFF, 20000.0f);
                e.setParameter(DRIFT, 0.0f);
                e.setParameter(SOFT_START, 0.0f);
                e.setParameter(SLOP, 0.0f);
                u4uv::lfo(e, 0, second ? D::Osc2Pitch : D::Pitch, 0.1f, 1.0f);
                e.prepareToPlay(SR, 256);
                e.noteOn(69, 0.8f);
                std::vector<float> out;
                juce::AudioBuffer<float> buf(2, 256);
                juce::MidiBuffer midi;
                for (int b = 0; b < (int) (2.0 * SR / 256); ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 256);
                }
                double longest = 0.0, shortest = 1.0e9, last = -1.0;
                for (size_t i = out.size() / 4; i < out.size(); ++i)
                    if (out[i - 1] < 0.0f && out[i] >= 0.0f)
                    {
                        const double at = (double) i - (double) out[i] / (double) (out[i] - out[i - 1]);
                        if (last >= 0.0) { longest = std::max(longest, at - last); shortest = std::min(shortest, at - last); }
                        last = at;
                    }
                return 12.0 * std::log2(longest / shortest);      // semitones, top to bottom
            };
            for (bool second : { false, true })
            {
                olduv::UnifiedVoiceEngine a;
                UnifiedVoiceEngine b;
                const double was = swing(a, second), now = swing(b, second);
                expect(was > 2.0 && std::abs(now / was - 1.0) < 0.05,
                       juce::String(second ? "PIT2" : "PTCH") + ": " + juce::String(was, 3) + " semitones then, " + juce::String(now, 3) + " now");
            }
        }

        beginTest("The level under pad pressure: the router's AMP adds to the pressure route, as the LFO's AMP added");
        {
            // A held saw, pad pressure 0.5 on VOL (the default route), a slow sine LFO on AMP: the
            // gain is 1 + 0.5 + LFO either way; a product, (1 + 0.5) x (1 + LFO), is not what it was.
            auto run = [&](EngineBase& e)
            {
                u4uv::lfo(e, 0, D::Amp, 0.4f, 2.0f);
                e.setParameter(AMP_SUSTAIN, 1.0f);
                e.prepareToPlay(SR, 256);
                u4uv::seedSystemRandom(77);
                e.noteOn(57, 0.8f);
                std::vector<float> out;
                juce::AudioBuffer<float> buf(2, 256);
                juce::MidiBuffer midi;
                for (int b = 0; b < (int) (1.5 * SR / 256); ++b)
                {
                    e.aftertouch(57, 0.5f);
                    buf.clear();
                    e.processBlock(buf, midi);
                    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 256);
                }
                out.erase(out.begin(), out.begin() + (long) (0.3 * SR));
                out.insert(out.end(), out.begin(), out.end());      // (compare() takes two sides)
                return out;
            };
            olduv::UnifiedVoiceEngine a;
            UnifiedVoiceEngine b;
            const auto v = u4uv::compare(run(a), run(b), SR);
            expect(v.envelopeMean < 0.3, "the level in 20 ms windows within " + juce::String(v.envelopeMean, 3) + " dB on average");
        }

        beginTest("What the voice keeps for the router: a WT slot's table moves by 8 a unit, idle voices' LFOs keep time, oscillator 2 runs for XMOD");
        {
            // ONE > 1B at 0.5 on a wavetable slot whose table is 2: the table is 2 + 0.5 x 8 (the old
            // routes' TABLE_ROUTE_SPAN, whatever the list's length), and an FM slot's ratio 3 + 0.5 x 11.
            for (int kind : { 6, 7 })
            {
                UnifiedVoiceEngine e;
                e.setParameter(WAVE1, (float) kind);
                e.setParameter(SRC_B1, kind == 6 ? 2.0f : 3.0f);
                u4uv::slot(e, 0, e.getRouter().sourceIndex(mod::Src::One), UnifiedVoice::RSrcB1, 0.5f);
                e.prepareToPlay(SR, 64);
                e.noteOn(60, 0.8f, -1);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                for (int b = 0; b < 8; ++b) e.processBlock(buf, midi);
                expectWithinAbsoluteError(e.getVoice(0).getOsc(0).b, kind == 6 ? 2.0f + 0.5f * UnifiedVoice::TABLE_ROUTE_SPAN : 3.0f + 5.5f, 1.0e-4f,
                                          kind == 6 ? "WT: table 6" : "FM: ratio 8.5");
                u4uv::slot(e, 0, 0, 0, 0.0f);
                for (int b = 0; b < 8; ++b) e.processBlock(buf, midi);
                expectEquals(e.getVoice(0).getOsc(0).b, kind == 6 ? 2.0f : 3.0f, "let go: the set value");
            }
            {
                // ONE > 1A at 0.5 on a wavetable slot at position 0.2: 0.7 (the cell's whole range a unit).
                UnifiedVoiceEngine e;
                e.setParameter(WAVE1, 6.0f);
                e.setParameter(SRC_A1, 0.2f);
                u4uv::slot(e, 0, e.getRouter().sourceIndex(mod::Src::One), UnifiedVoice::RSrcA1, 0.5f);
                e.prepareToPlay(SR, 64);
                e.noteOn(60, 0.8f, -1);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                for (int b = 0; b < 8; ++b) e.processBlock(buf, midi);
                expectWithinAbsoluteError(e.getVoice(0).getOsc(0).a, 0.7f, 1.0e-5f, "WT position 0.2 + 0.5");
            }
            // A free-running LFO of the router keeps time in a voice that is not sounding: its phase
            // at a later note is where the time since the engine began puts it.
            {
                UnifiedVoiceEngine e(1);
                u4uv::lfo(e, 0, D::Amp, 0.3f, 1.25f, LFO::Shape::Sine, LFO::TriggerMode::Free);
                e.prepareToPlay(SR, 64);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                int samples = 0;
                auto blocks = [&](int n) { for (int b = 0; b < n; ++b) { e.processBlock(buf, midi); samples += 64; } };
                blocks(10);
                e.noteOn(60, 0.8f, -1);
                blocks(200);
                e.allNotesOff();
                blocks(523);            // silent: the voice is idle
                e.noteOn(62, 0.8f, -1);
                blocks(1);
                const double expected = std::fmod(1.25 * samples / SR, 1.0);
                const double phase = e.getVoice(0).getRouterVoice().getLfo(0).getPhase();
                const double off = std::abs(phase - expected);
                expect(std::min(off, 1.0 - off) < 2.0e-3, "phase " + juce::String(phase, 4) + ", the clock says " + juce::String(expected, 4));
            }
            // XMOD from the router with oscillator 2 out of the mix: oscillator 2 runs.
            {
                UnifiedVoiceEngine e;
                e.setParameter(MIX, 0.0f);
                e.prepareToPlay(SR, 64);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                e.processBlock(buf, midi);
                expect(! e.getVoice(0).getOsc(1).enabled, "out of the mix: oscillator 2 rests");
                u4uv::lfo(e, 0, D::CrossMod, 0.5f, 1.0f);
                e.processBlock(buf, midi);
                expect(e.getVoice(0).getOsc(1).enabled, "an LFO of the router on XMOD: oscillator 2 runs");
            }
        }

        beginTest("The per-note randoms: RAND drawn as the old slots drew it (uniform, every voice its own, independent of the note)");
        {
            // The draws themselves, through the router voice (new) and the old slots' formula
            // (EngineModSection.h at 46fedbc, VoiceModSlots::noteOn), 4000 notes each.
            UnifiedVoiceEngine e;
            const auto& router = e.getRouter();
            std::vector<float> draws, otherVoice;
            for (int n = 0; n < 4000; ++n)
            {
                auto& v0 = e.getVoice(0).getRouterVoice();
                auto& v1 = e.getVoice(1).getRouterVoice();
                v0.noteOn(router, 30 + n % 60, 0.8f);
                v1.noteOn(router, 30 + n % 60, 0.8f);
                draws.push_back(v0.source(mod::Src::Random));
                otherVoice.push_back(v1.source(mod::Src::Random));
            }
            std::vector<float> old;
            uint32_t state = 0x1234567u;
            for (int n = 0; n < 4000; ++n)
            {
                state = state * 1664525u + 1013904223u + (uint32_t) (30 + n % 60) * 2654435761u;
                old.push_back((float) ((state >> 8) & 0xffffu) / 32767.5f - 1.0f);
            }
            auto stats = [](std::vector<float> x, double& mean, double& sd, double& ks)
            {
                mean = std::accumulate(x.begin(), x.end(), 0.0) / (double) x.size();
                double v = 0.0;
                for (float s : x) v += (s - mean) * (s - mean);
                sd = std::sqrt(v / (double) x.size());
                std::sort(x.begin(), x.end());
                ks = 0.0;       // against the uniform -1..1
                for (size_t i = 0; i < x.size(); ++i)
                    ks = std::max(ks, std::abs((double) (i + 1) / (double) x.size() - 0.5 * ((double) x[i] + 1.0)));
            };
            double m0, s0, k0, m1, s1, k1;
            stats(old, m0, s0, k0);
            stats(draws, m1, s1, k1);
            logMessage("RAND old: mean " + juce::String(m0, 3) + " sd " + juce::String(s0, 3) + " KS " + juce::String(k0, 4)
                       + "; new: mean " + juce::String(m1, 3) + " sd " + juce::String(s1, 3) + " KS " + juce::String(k1, 4));
            const double ksLimit = 1.63 / std::sqrt(4000.0);       // 1 %
            expect(k0 < ksLimit && k1 < ksLimit, "both uniform");
            expect(std::abs(m1) < 0.05 && std::abs(s1 - 1.0 / std::sqrt(3.0)) < 0.03);
            double corr = 0.0;
            for (size_t i = 0; i < draws.size(); ++i) corr += (double) draws[i] * otherVoice[i];
            corr /= (double) draws.size() * s1 * s1;
            expect(std::abs(corr) < 0.06, "two voices draw independently (" + juce::String(corr, 3) + ")");
            int sameAsBefore = 0;
            for (size_t i = 1; i < draws.size(); ++i) sameAsBefore += draws[i] == draws[i - 1] ? 1 : 0;
            expect(sameAsBefore < 4, "a new value every note");
            // Repeatable: a new engine draws the same series (the old one was not, from run to run).
            UnifiedVoiceEngine again;
            auto& w = again.getVoice(0).getRouterVoice();
            w.noteOn(again.getRouter(), 30, 0.8f);
            expectEquals(w.source(mod::Src::Random), draws[0], "a render repeats");
        }

        beginTest("Legacy XIN cells become the XIN page's first route; OFF takes it away");
        {
            UnifiedVoiceEngine e;
            auto cell = [&e](int c) { return e.getRouter().get(mod::Router::xinId(c)); };
            e.setParameter(XIN, 1.0f);
            e.setParameter(XIN_DEPTH, 0.6f);
            expectEquals(cell(mod::Router::XinDest1), (float) UnifiedVoice::RPm);
            expectEquals(cell(mod::Router::XinAmount1), 0.6f);
            e.setParameter(XIN, 3.0f);
            expectEquals(cell(mod::Router::XinDest1), (float) UnifiedVoice::RSync);
            expectWithinAbsoluteError(cell(mod::Router::XinAmount1), UnifiedVoice::SYNC_LEVEL / UnifiedVoice::xinSyncThreshold(0.6f), 1.0e-6f);
            e.setParameter(XIN, 2.0f);
            expectEquals(cell(mod::Router::XinDest1), (float) UnifiedVoice::RRing);
            e.setParameter(XIN, 0.0f);
            expectEquals(cell(mod::Router::XinDest1), 0.0f);
            expectEquals(cell(mod::Router::XinAmount1), 0.0f);
            expectEquals(e.getParameter(XIN_DEPTH), 0.6f, "the stored cells keep what was stored");
        }

        beginTest("Nothing routed costs no router work: the voice's tick says nothing moves");
        {
            UnifiedVoiceEngine e;
            const auto plain = u4uv::render(e, SR, 512, {});
            expect(! e.getVoice(0).getRouterVoice().moved(UnifiedVoice::RCutoff));
            UnifiedVoiceEngine f;
            u4uv::slot(f, 0, ModSrc::Velocity, (int) D::Cutoff, 0.0f);      // a slot without an amount
            expect(u4uv::render(f, SR, 512, {}) == plain, "AMT 0 is no slot, sample for sample");
            // (The rows that come out MATCH while the router moves something are MATCH because the
            // arithmetic meets: VEL > CUT is a constant a note, added where the old path added it to
            // terms that are 0 here. That the slot acts at all:)
            UnifiedVoiceEngine g, h;
            brighten(g);
            brighten(h);
            u4uv::slot(h, 0, ModSrc::Velocity, (int) D::Cutoff, 0.6f);
            const auto without = u4uv::render(g, SR, 512, {}), with = u4uv::render(h, SR, 512, {});
            expect(u4uv::compare(without, with, SR).word == "LOST", "VEL > CUT opens the filter");
        }

        review();

        if (u4uv::env("NEPTUNE_UV_FLAKE").isNotEmpty())
            flake();
        if (u4uv::env("NEPTUNE_UV_BENCH").isNotEmpty())
            bench();
    }

    // ---- "Preset Audition" on unified-voice presets: how often the test's own rule fails ----------
    // It plays four notes on a fresh engine and four on the audition engine, and asks that the
    // closest heard / fresh pair lie within max(3 dB, the widest fresh / fresh pair). Here the rule
    // is run on two FRESH engines (the same sound by construction) many times per preset.
    void flake();

    // ---- Cost: 8 voices, old against new, alternated, best of seven (run in the Release binary) --
    void bench();

    // ---- The independent review's tests (U4-UV review) -------------------------------------------
    void review();
};

inline void U4UnifiedTests::review()
{
    using namespace UnifiedParam;
    using u4uv::lfo;
    using u4uv::slot;
    const double SR = 48000.0;
    const bool print = u4uv::env("NEPTUNE_UV_PRINT").isNotEmpty();
    juce::AudioBuffer<float> buf(2, 64);
    juce::MidiBuffer midi;

    beginTest("Review: the router holds every cell's set value as the engine has it (ATK2 .. REL5 after the init patch)");
    {
        const auto& table = UnifiedVoiceEngine::routerDests();
        auto check = [&](UnifiedVoiceEngine& e, const juce::String& when)
        {
            int wrong = 0;
            for (int d = 1; d < table.size(); ++d)
                if (table[d].id >= 0 && e.getRouter().baseOf(d) != e.getParameter(table[d].id))
                {
                    ++wrong;
                    logMessage("  " + when + ": " + juce::String(table[d].label) + " router " + juce::String(e.getRouter().baseOf(d))
                               + " engine " + juce::String(e.getParameter(table[d].id)));
                }
            expectEquals(wrong, 0, when + ": every cell's base");
        };
        UnifiedVoiceEngine fresh;
        check(fresh, "a new engine");
        PresetManager all;
        int checked = 0;
        for (int i = 0; i < all.getPresetCount() && checked < 6; ++i)
            if (all.getPreset(i).engineType == EngineType::UnifiedVoice)
            {
                UnifiedVoiceEngine e;
                u4uv::applySound(e, PresetManager::soundOf(all.getPreset(i)));
                check(e, all.getPreset(i).name);
                ++checked;
            }
        // What that means for a sound: a slot on ATK2 moves the set 10 ms, and gives the 10 ms back.
        UnifiedVoiceEngine e;
        e.setParameter(ModParam::ENV_COUNT, 1.0f);
        const int atk2 = table.indexOfId(ModParam::envParamId(0, ModParam::ENV_ATK));
        expect(atk2 > 0, "ATK2 is a destination");
        slot(e, 0, ModSrc::Velocity, atk2, 0.1f);
        e.prepareToPlay(SR, 64);
        e.noteOn(60, 1.0f, -1);
        for (int b = 0; b < 8; ++b) { buf.clear(); e.processBlock(buf, midi); }
        const float moved = e.getVoice(0).getEnvelopeChain().getEnvelope(0).getParams().attack;
        expectWithinAbsoluteError(moved, 0.01f * std::exp2(0.1f * 0.5f * std::log2(10.0f / 0.001f)), 0.002f,
                                  "VEL 1 at 0.1 on ATK2: about 0.016 s, around the set 0.01 s");
        e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
        for (int b = 0; b < 8; ++b) { buf.clear(); e.processBlock(buf, midi); }
        expectEquals(e.getVoice(0).getEnvelopeChain().getEnvelope(0).getParams().attack, 0.01f, "let go: the set 0.01 s");
    }

    beginTest("Review: an LFO aimed at a classic LFO's RAT / DPT (or a classic LFO aimed at another's) is heard, as before");
    {
        Tally t;
        std::vector<Case> cases;
        cases.push_back({ "LFO1 WHL 0.5 on CUT (classic), LFO2 > RAT1 0.8", [](EngineBase& e, bool)
        {
            brighten(e); lfo(e, 0, D::Cutoff, 0.4f, 3.0f); e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f);
            lfo(e, 1, D::Lfo1Rate, 0.8f, 0.4f);
        }, {}, "MATCH" });
        cases.push_back({ "LFO1 > CUT 0.5, LFO2 WHL 0.6 (classic) > RAT1", [](EngineBase& e, bool)
        {
            brighten(e); lfo(e, 0, D::Cutoff, 0.5f, 3.0f);
            lfo(e, 1, D::Lfo1Rate, 0.0f, 0.4f, LFO::Shape::Square); e.setParameter(ModParam::lfoExt2ParamId(1, ModParam::LFO_WHEEL), 0.6f);
        }, {}, "MATCH" });
        cases.push_back({ "LFO1 VEL 0.4 on AMP (classic), LFO3 > DPT1 0.8", [](EngineBase& e, bool)
        {
            lfo(e, 0, D::Amp, 0.2f, 5.0f); e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_VEL), 0.4f);
            lfo(e, 2, D::Lfo1Depth, 0.8f, 0.5f, LFO::Shape::Triangle);
        }, {}, "MATCH" });
        for (auto& c : cases)
            pair(c, SR, 512, t);
        expectEquals(t.match, (int) cases.size());
        // The LFO aimed at the classic one's rate, alone, changes the sound (it is not a dead route).
        UnifiedVoiceEngine with, without;
        for (auto* e : { &with, &without })
        {
            brighten(*e); lfo(*e, 0, D::Cutoff, 0.4f, 3.0f); e->setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f);
        }
        lfo(with, 1, D::Lfo1Rate, 0.8f, 0.4f);
        expect(u4uv::render(with, SR, 512, {}) != u4uv::render(without, SR, 512, {}), "LFO2 > RAT1 of a classic LFO1 is heard");
        expect(with.lfoOnClassicPath(1), "LFO2 joins LFO1 on the classic path");
        // Taken off again: both go back to the router, and the sound is a fresh engine's.
        with.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.0f);
        expect(! with.lfoOnClassicPath(0) && ! with.lfoOnClassicPath(1), "WHL cleared: both on the router");
    }

    beginTest("Review: WHL set and cleared leaves the LFO on the router as if it had never been classic");
    {
        UnifiedVoiceEngine a, b;
        for (auto* e : { &a, &b }) { brighten(*e); lfo(*e, 0, D::Cutoff, 0.5f, 2.0f, LFO::Shape::Triangle); }
        a.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.5f);
        expect(a.lfoOnClassicPath(0));
        a.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.0f);
        expect(! a.lfoOnClassicPath(0));
        expect(u4uv::render(a, SR, 512, {}) == u4uv::render(b, SR, 512, {}), "the same samples");
    }

    beginTest("Review: ENV2 as a source follows the first extra envelope; ENV3 without a second is 0");
    {
        const auto sources = mod::SourceTable::standard(ModParam::MAX_LFOS, ModParam::MAX_ENVS);
        const int env2 = sources.indexOf(mod::Src::Env2), env3 = sources.indexOf(mod::Src::Env3);
        expect(env2 > 0 && env3 > 0);
        auto make = [&](int src)
        {
            auto e = std::make_unique<UnifiedVoiceEngine>();
            e->setParameter(CUTOFF, 400.0f);
            e->setParameter(ModParam::ENV_COUNT, 1.0f);
            e->setParameter(ModParam::envParamId(0, ModParam::ENV_TARGET), 3.0f);     // (custom, DEST off: the chain itself moves nothing)
            e->setParameter(ModParam::envParamId(0, ModParam::ENV_ATK), 0.4f);
            if (src > 0)
                slot(*e, 0, src, (int) D::Cutoff, 0.8f);
            return e;
        };
        const auto plain = u4uv::render(*make(0), SR, 512, {});
        const auto byEnv2 = u4uv::render(*make(env2), SR, 512, {});
        const auto byEnv3 = u4uv::render(*make(env3), SR, 512, {});
        expect(u4uv::compare(plain, byEnv2, SR).word == "LOST", "ENV2 > CUT opens the filter as the envelope rises");
        expect(byEnv3 == plain, "ENV3 with one extra envelope: 0, the plain sound");
    }

    beginTest("Review: old against new re-measured, a third of the rows, another seed, blocks of 300, no chord");
    {
        auto feats = featureCases();
        auto presets = presetCases();
        std::vector<Case> some;
        for (size_t i = 0; i < feats.size(); i += 3) some.push_back(feats[i]);
        for (size_t i = 1; i < presets.size(); i += 3) some.push_back(presets[i]);
        int match = 0, close = 0, lost = 0;
        for (auto& c : some)
        {
            olduv::UnifiedVoiceEngine o;
            UnifiedVoiceEngine n;
            c.set(o, false);
            c.set(n, true);
            u4uv::Play p = c.play;
            p.chord = false;
            const auto a = u4uv::render(o, SR, 300, p, 0x0dd5eed);
            const auto b = u4uv::render(n, SR, 300, p, 0x0dd5eed);
            const auto v = u4uv::compare(a, b, SR);
            if (c.expected == "MATCH") expect(v.identical, c.name + ": MATCH at another seed and block");
            else                       expect(v.word != "LOST", c.name + ": " + v.word);
            if (c.maxNull < 0.0) expect(v.null <= c.maxNull, c.name + ": null " + juce::String(v.null, 1));
            (v.word == "MATCH" ? match : (v.word == "CLOSE" ? close : lost))++;
            if (print)
                logMessage("  re-measured " + v.word.paddedRight(' ', 6) + c.name.paddedRight(' ', 60) + " level " + juce::String(v.level, 2)
                           + " env " + juce::String(v.envelopeMean, 2) + "/" + juce::String(v.envelopeWorst, 2) + " bright "
                           + juce::String(v.brightness * 100.0, 1) + "% null " + juce::String(v.null, 1) + " dB");
        }
        logMessage("re-measured: " + juce::String((int) some.size()) + " rows, " + juce::String(match) + " MATCH, " + juce::String(close)
                   + " CLOSE, " + juce::String(lost) + " LOST");
        expectEquals(lost, 0);
        // XIN PM and SYNC per sample at other rates and blocks that are not whole sub-blocks.
        for (auto& c : feats)
            if (c.name.startsWith("legacy XIN PM") || c.name.startsWith("legacy XIN SYNC"))
                for (auto [sr, block] : { std::pair<double, int> { 44100.0, 37 }, std::pair<double, int> { 96000.0, 100 } })
                {
                    Case k = c;
                    k.play.input = u4uv::sineInput(c.name.contains("PM") ? 220.0 : 130.0, 0.8f, sr);
                    Tally t;
                    const auto v = pair(k, sr, block, t, false);
                    expect(v.null <= -40.0, k.name + " @" + juce::String((int) sr) + "/" + juce::String(block) + ": null " + juce::String(v.null, 1));
                }
    }

    beginTest("Review: legacy XIN SYNC arms on a shallow dip just past the old threshold (not twice as deep)");
    {
        // XDPT 0.7: the old threshold is 0.094 of full scale; the input dips to -0.12 only.
        const auto dip = [](int n) { return (n % 200) < 100 ? 0.5f : -0.12f; };
        Case c { "legacy XIN SYNC 0.7, an input that dips to -0.12", [](EngineBase& e, bool)
        {
            e.setParameter(XIN, 3.0f); e.setParameter(XIN_DEPTH, 0.7f); e.setParameter(MIX, 0.0f);
        }, {}, "CLOSE" };
        c.play.input = dip;
        Tally t;
        const auto v = pair(c, SR, 512, t, false);
        expect(v.null <= -40.0, "null " + juce::String(v.null, 1));
        UnifiedVoiceEngine on, off;
        for (auto* e : { &on, &off }) { e->setParameter(XIN_DEPTH, 0.7f); e->setParameter(MIX, 0.0f); }
        on.setParameter(XIN, 3.0f);
        expect(u4uv::render(on, SR, 512, c.play) != u4uv::render(off, SR, 512, c.play), "the dip arms SYNC: it restarts the oscillator");
    }

    beginTest("Review: what the router lets go of goes back (an idle voice's RESO, a held note's 1A); ETIM x EVAR; WVIB stops the built-in vibrato");
    {
        // RESO moved by a slot while a note sounded; the slot taken away while the voice is idle:
        // the next note has the set resonance (the idle voice was given RESO 0 back).
        auto secondNote = [&](bool withSlot)
        {
            UnifiedVoiceEngine e;
            e.setParameter(CUTOFF, 1500.0f);
            e.setParameter(RESONANCE, 0.1f);
            e.setParameter(DRIFT, 0.0f);
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                e.getVoice(v).getAnalogCharacter().enabled = false;
            if (withSlot)
                slot(e, 0, ModSrc::Velocity, (int) D::Resonance, 0.8f);
            e.prepareToPlay(SR, 64);
            auto run = [&](double s, std::vector<float>* out)
            {
                for (int b = 0; b < (int) (s * SR / 64); ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    if (out != nullptr) for (int i = 0; i < 64; ++i) out->push_back(buf.getSample(0, i));
                }
            };
            e.noteOn(60, 1.0f, -1);
            run(0.2, nullptr);
            e.noteOff(60, -1);
            run(1.5, nullptr);
            if (withSlot)
                e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
            run(0.1, nullptr);
            std::vector<float> out;
            e.noteOn(60, 1.0f, -1);
            run(0.3, &out);
            return out;
        };
        const auto a = secondNote(true), b = secondNote(false);
        double diff = 0.0, ref = 0.0;
        for (size_t i = 0; i < a.size(); ++i) { diff += (double) (a[i] - b[i]) * (a[i] - b[i]); ref += (double) b[i] * b[i]; }
        const double nullDb = 10.0 * std::log10((diff + 1.0e-30) / (ref + 1.0e-30));
        expect(nullDb < -60.0, "the next note has the set RESO: null " + juce::String(nullDb, 1) + " dB");

        // A wavetable's 1A moved by a slot under a held note, the slot taken away: the set position.
        {
            UnifiedVoiceEngine e;
            e.setParameter(WAVE1, 6.0f);
            e.setParameter(SRC_A1, 0.2f);
            slot(e, 0, ModSrc::Velocity, UnifiedVoice::RSrcA1, 0.5f);
            e.prepareToPlay(SR, 64);
            e.noteOn(60, 1.0f, -1);
            for (int b2 = 0; b2 < 20; ++b2) { buf.clear(); e.processBlock(buf, midi); }
            expectWithinAbsoluteError(e.getVoice(0).getOsc(0).a, 0.7f, 0.01f, "VEL 1 at 0.5 on 1A: 0.2 + 0.5");
            e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
            for (int b2 = 0; b2 < 40; ++b2) { buf.clear(); e.processBlock(buf, midi); }
            expectEquals(e.getVoice(0).getOsc(0).a, 0.2f, "let go under the held note: the set 0.2");
            // Let go at once while the voice is idle (mod::Voice::idle drops every route without a
            // fade): the next note starts from the set 0.2, not where the slot left it.
            e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.5f);
            for (int b2 = 0; b2 < 20; ++b2) { buf.clear(); e.processBlock(buf, midi); }
            e.noteOff(60, -1);
            for (int b2 = 0; b2 < 1200; ++b2) { buf.clear(); e.processBlock(buf, midi); }
            expect(! e.getVoice(0).isActive(), "the voice is idle");
            e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
            for (int b2 = 0; b2 < 4; ++b2) { buf.clear(); e.processBlock(buf, midi); }
            e.noteOn(60, 1.0f, -1);
            buf.clear();
            e.processBlock(buf, midi);
            expectEquals(e.getVoice(0).getOsc(0).a, 0.2f, "the next note: the set 0.2");
        }

        // ETIM on top of EVAR: the amp envelope's factor is the note's EVAR factor x ETIM.
        {
            UnifiedVoiceEngine e;
            slot(e, 0, ModSrc::Velocity, UnifiedVoice::REnvTime, 1.0f);
            e.setParameter(ENV_VAR, 0.5f);
            e.prepareToPlay(SR, 64);
            u4uv::seedSystemRandom(1234);
            e.noteOn(60, 1.0f, -1);
            buf.clear();
            e.processBlock(buf, midi);
            const float evar = e.getVoice(0).getFilterEnvelope().getTimeScale();
            expect(evar != 1.0f, "EVAR drew a factor");
            expectWithinAbsoluteError(e.getVoice(0).getAmpEnvelope().getTimeScale(), evar * 8.0f, 1.0e-3f, "ETIM x the note's EVAR factor");
        }

        // WVIB (the STRIP page's own vibrato) takes the strip from the built-in 0.5-semitone vibrato.
        auto wobble = [&](float wvib)
        {
            UnifiedVoiceEngine e;
            e.setParameter(WAVE1, 0.0f);
            e.setParameter(MIX, 0.0f);
            e.setParameter(CUTOFF, 20000.0f);
            e.setParameter(DRIFT, 0.0f);
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                e.getVoice(v).getAnalogCharacter().enabled = false;
            e.setParameter(UnifiedVoiceEngine::STRIP_IDS + (mod::Router::STRIP_VIBRATO - mod::Router::STRIP_BASE), wvib);
            e.prepareToPlay(SR, 256);
            e.noteOn(57, 0.8f, -1);
            std::vector<float> out;
            juce::AudioBuffer<float> b256(2, 256);
            for (int blk = 0; blk < (int) (1.0 * SR / 256); ++blk)
            {
                e.setModWheel(1.0f);
                b256.clear();
                e.processBlock(b256, midi);
                for (int i = 0; i < 256; ++i) out.push_back(b256.getSample(0, i));
            }
            std::vector<double> at;
            for (size_t i = out.size() / 2; i < out.size(); ++i)
                if (out[i - 1] < 0.0f && out[i] >= 0.0f)
                    at.push_back((double) i - (double) out[i] / (double) (out[i] - out[i - 1]));
            double mean = 0.0, var = 0.0;
            for (size_t i = 1; i < at.size(); ++i) mean += at[i] - at[i - 1];
            mean /= (double) std::max<size_t>(1, at.size() - 1);
            for (size_t i = 1; i < at.size(); ++i) var += (at[i] - at[i - 1] - mean) * (at[i] - at[i - 1] - mean);
            return std::sqrt(var / (double) std::max<size_t>(1, at.size() - 1)) / mean;
        };
        const double builtIn = wobble(0.0f), tiny = wobble(0.01f);
        expect(builtIn > 3.0 * tiny && builtIn > 0.002, "built-in " + juce::String(builtIn, 4) + ", WVIB 0.01 st " + juce::String(tiny, 4));
    }

    beginTest("STRIP page: ARCH presets seed the defaults; the strip opens the filter through the router");
    {
        // A new engine has the STRIP page empty (backward compat: old projects have no ids 158-165).
        {
            UnifiedVoiceEngine e;
            expectEquals(e.getParameter(UnifiedVoiceEngine::STRIP_IDS + 0), 0.0f, "new engine: DST1 OFF");
            expectEquals(e.getParameter(UnifiedVoiceEngine::STRIP_IDS + 1), 0.0f, "new engine: AMT1 0");
            expectEquals(e.getParameter(UnifiedVoiceEngine::STRIP_IDS + (mod::Router::STRIP_VIBRATO - mod::Router::STRIP_BASE)), 0.0f, "new engine: WVIB 0");
        }
        // After an ARCH preset is applied, the STRIP page has CUT +50 % and WVIB 0.25.
        for (int a = 0; a < (int) ArchitecturePresetId::Count; ++a)
        {
            UnifiedVoiceEngine e;
            e.applyArchitecturePreset(static_cast<ArchitecturePresetId>(a));
            const float dst = e.getParameter(UnifiedVoiceEngine::STRIP_IDS + 0);
            const float amt = e.getParameter(UnifiedVoiceEngine::STRIP_IDS + 1);
            const float wvib = e.getParameter(UnifiedVoiceEngine::STRIP_IDS + (mod::Router::STRIP_VIBRATO - mod::Router::STRIP_BASE));
            expect(dst > 0.0f, "ARCH " + juce::String(a) + ": DST1 is not OFF (" + juce::String(dst) + ")");
            expect(amt != 0.0f, "ARCH " + juce::String(a) + ": AMT1 is not 0 (" + juce::String(amt) + ")");
            expect(wvib > 0.0f, "ARCH " + juce::String(a) + ": WVIB is set (" + juce::String(wvib) + ")");
        }
        // The route acts through the router: a turn on CUT (the default) changes the sound
        // when the strip is up and WHL is not set. Render two notes (strip down, strip up) with
        // an ARCH preset; the strip must change the brightness (the cutoff opens).
        {
            UnifiedVoiceEngine e;
            e.applyArchitecturePreset(ArchitecturePresetId::MonoVA);
            // Clear the WHL amounts so the strip goes through the STRIP page, not the LFOs.
            for (int l = 0; l < ModParam::MAX_LFOS; ++l)
                e.setParameter(ModParam::lfoExt2ParamId(l, ModParam::LFO_WHEEL), 0.0f);
            e.setParameter(UnifiedParam::DRIFT, 0.0f);
            for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                e.getVoice(v).getAnalogCharacter().enabled = false;
            e.prepareToPlay(SR, 256);
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            auto render = [&](float stripVal)
            {
                e.noteOn(60, 0.8f, -1);
                std::vector<float> samples;
                for (int b = 0; b < (int) (0.5 * SR / 256); ++b)
                {
                    e.setModWheel(stripVal);
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 256; ++i)
                        samples.push_back(buf.getSample(0, i));
                }
                e.noteOff(60, -1);
                return samples;
            };
            const auto down = render(0.0f);
            const auto up = render(1.0f);
            // Measure brightness: RMS of first difference over RMS (higher = brighter).
            auto bright = [](const std::vector<float>& s)
            {
                const size_t skip = s.size() / 4;
                double sig = 0.0, diff = 0.0;
                for (size_t i = skip + 1; i < s.size(); ++i)
                {
                    sig += (double) s[i] * (double) s[i];
                    const double d = (double) s[i] - (double) s[i - 1];
                    diff += d * d;
                }
                return std::sqrt(diff / std::max(1.0, sig));
            };
            const double bDown = bright(down), bUp = bright(up);
            expect(bUp > bDown * 1.1, "strip up brightens the sound through CUT: " + juce::String(bDown, 4) + " -> " + juce::String(bUp, 4));
        }
    }

    beginTest("Review: each part of the CLOSE rule, alone, calls a deliberate loss LOST");
    {
        olduv::UnifiedVoiceEngine o;
        const auto base = u4uv::render(o, SR, 512, {});
        const size_t n = base.size() / 2;
        auto each = [&](std::function<float (size_t i, float x)> f)
        {
            std::vector<float> out(base.size());
            for (int side = 0; side < 2; ++side)
                for (size_t i = 0; i < n; ++i)
                    out[(size_t) side * n + i] = f(i, base[(size_t) side * n + i]);
            return out;
        };
        auto describe = [](const u4uv::Verdict& v)
        {
            return v.word + " level " + juce::String(v.level, 2) + " env " + juce::String(v.envelopeMean, 2) + "/" + juce::String(v.envelopeWorst, 2)
                   + " bright " + juce::String(v.brightness * 100.0, 1) + "%";
        };
        // 1. Level: 1.1 dB quieter, nothing else.
        auto v = u4uv::compare(base, each([](size_t, float x) { return x * 0.88f; }), SR);
        expect(v.word == "LOST" && v.level > 1.0 && v.envelopeMean <= 1.5 && v.envelopeWorst <= 6.0 && v.brightness <= 0.10, "level alone: " + describe(v));
        // 2. Worst window: 60 ms at -9 dB in the middle of the chord.
        const size_t from = (size_t) (0.6 * SR), to = from + (size_t) (0.06 * SR);
        v = u4uv::compare(base, each([&](size_t i, float x) { return i >= from && i < to ? x * 0.35f : x; }), SR);
        expect(v.word == "LOST" && v.level <= 1.0 && v.envelopeMean <= 1.5 && v.envelopeWorst > 6.0 && v.brightness <= 0.10, "worst window alone: " + describe(v));
        // 3. Mean of the windows: a 7 Hz tremolo of +-40 %.
        v = u4uv::compare(base, each([&](size_t i, float x) { return x * (1.0f + 0.4f * (float) std::sin(6.283185307 * 7.0 * (double) (i % n) / SR)); }), SR);
        expect(v.word == "LOST" && v.level <= 1.0 && v.envelopeMean > 1.5 && v.envelopeWorst <= 6.0 && v.brightness <= 0.10, "window mean alone: " + describe(v));
        // 4. Brightness: a gentle one-pole low-pass, the level put back.
        bool found = false;
        for (double fc : { 12000.0, 9000.0, 7000.0, 5000.0, 4000.0, 3000.0 })
        {
            const float a = (float) (1.0 - std::exp(-6.283185307 * fc / SR));
            std::vector<float> out(base.size());
            for (int side = 0; side < 2; ++side)
            {
                float y = 0.0f;
                for (size_t i = 0; i < n; ++i) { y += a * (base[(size_t) side * n + i] - y); out[(size_t) side * n + i] = y; }
                const double g = u4uv::rmsOf(base.data() + side * n, n) / (u4uv::rmsOf(out.data() + side * n, n) + 1.0e-12);
                for (size_t i = 0; i < n; ++i) out[(size_t) side * n + i] *= (float) g;
            }
            v = u4uv::compare(base, out, SR);
            if (print) logMessage("  low-pass " + juce::String(fc) + " Hz: " + describe(v));
            if (v.brightness > 0.10)
            {
                expect(v.word == "LOST" && v.level <= 1.0 && v.envelopeMean <= 1.5 && v.envelopeWorst <= 6.0, "brightness alone: " + describe(v));
                found = true;
                break;
            }
        }
        expect(found, "a low-pass the brightness rule hears");
    }
}

inline void U4UnifiedTests::flake()
{
    beginTest("Preset Audition's rule on unified-voice presets: two fresh engines, many trials");
    PresetManager all;
    auto features = [](EngineBase& e, int note)
    {
        const double sr = 48000.0;
        const int block = 64;
        std::vector<float> mono;
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        auto run = [&](double seconds)
        {
            for (int b = 0; b < (int) (seconds * sr) / block; ++b)
            {
                buf.clear();
                e.processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    mono.push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
            }
        };
        e.noteOn(note, 0.8f);
        run(0.6);
        e.noteOff(note);
        run(0.3);
        e.allNotesOff();
        run(0.3);
        mono.resize((size_t) (0.9 * sr));
        std::vector<float> f;
        auto dbp = [](double power) { return (float) (10.0 * std::log10(power + 1.0e-7)); };
        const size_t win = mono.size() / 12;
        for (size_t w = 0; w < 12; ++w)
        {
            double sum = 0.0;
            for (size_t i = w * win; i < (w + 1) * win; ++i) sum += (double) mono[i] * mono[i];
            f.push_back(dbp(sum / (double) win));
        }
        double total[10] = {}, lp[10] = {};
        for (float x : mono)
        {
            double prev = x;
            for (int b = 9; b >= 0; --b)
            {
                const double fc = 31.25 * std::pow(2.0, b);
                const double a = 1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * fc / sr);
                lp[b] += a * (x - lp[b]);
                const double band = prev - lp[b];
                total[b] += band * band;
                prev = lp[b];
            }
        }
        for (double t : total) f.push_back(dbp(t / (double) mono.size()));
        return f;
    };
    auto distance = [](const std::vector<float>& a, const std::vector<float>& b, int& worstAt)
    {
        float worst = 0.0f;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (a[i] > -55.0f || b[i] > -55.0f)
                if (std::abs(a[i] - b[i]) > worst) { worst = std::abs(a[i] - b[i]); worstAt = (int) i; }
        return worst;
    };
    const int trials = 12;
    int failures = 0, total = 0;
    std::map<int, int> failedFeature;
    for (int i = 0; i < all.getPresetCount(); ++i)
    {
        const auto& p = all.getPreset(i);
        if (p.engineType != EngineType::UnifiedVoice)
            continue;
        const ParamMap sound = PresetManager::soundOf(p);
        int failed = 0;
        for (int trial = 0; trial < trials; ++trial)
        {
            UnifiedVoiceEngine a, b;
            for (UnifiedVoiceEngine* e : { &a, &b })
            {
                e->prepareToPlay(48000.0, 64);
                u4uv::applySound(*e, sound);
            }
            std::vector<std::vector<float>> fresh, heard;
            for (int n = 0; n < 4; ++n)
            {
                fresh.push_back(features(a, 57));
                heard.push_back(features(b, 57));
            }
            float natural = 0.0f, d = 1000.0f;
            int at = -1, dummy = -1;
            for (size_t x = 0; x < 4; ++x)
                for (size_t y = 0; y < 4; ++y)
                {
                    if (x < y) natural = std::max(natural, distance(fresh[x], fresh[y], dummy));
                    int w = -1;
                    const float dd = distance(heard[x], fresh[y], w);
                    if (dd < d) { d = dd; at = w; }
                }
            ++total;
            if (d > std::max(3.0f, natural))
            {
                ++failed;
                ++failedFeature[at];
                logMessage("  " + p.name + " trial " + juce::String(trial) + ": closest pair " + juce::String(d, 1) + " dB at feature "
                           + juce::String(at) + " (fresh pairs up to " + juce::String(natural, 1) + ")");
            }
        }
        failures += failed;
    }
    juce::String where;
    for (auto& [f, n] : failedFeature) where << " feature " << f << ": " << n;
    logMessage("Preset Audition's rule on two fresh engines: " + juce::String(failures) + " of " + juce::String(total) + " trials fail;" + where);
}

inline void U4UnifiedTests::bench()
{
    beginTest("Cost: 8 voices, 48 kHz, blocks of 512, old against new, alternated, best of seven");
    using clock = std::chrono::steady_clock;
    struct Row { const char* name; std::function<void (EngineBase&)> set; };
    using namespace UnifiedParam;
    const Row rows[] = {
        { "plain (nothing routed)", [](EngineBase&) {} },
        { "plain, analog off", [](EngineBase& e) { e.setParameter(DRIFT, 0.0f); } },
        { "classic LFO1 WHL > CUT, strip up", [](EngineBase& e) { u4uv::lfo(e, 0, D::Cutoff, 0.0f, 40.0f, LFO::Shape::ExpUp, LFO::TriggerMode::Single);
                                                               e.setParameter(ModParam::lfoExt2ParamId(0, ModParam::LFO_WHEEL), 0.6f); e.setModWheel(0.7f); } },
        { "LFO1 > CUT", [](EngineBase& e) { u4uv::lfo(e, 0, D::Cutoff, 0.5f, 2.0f); } },
        { "LFO1 > PTCH, LFO2 > AMP", [](EngineBase& e) { u4uv::lfo(e, 0, D::Pitch, 0.1f, 5.0f); u4uv::lfo(e, 1, D::Amp, 0.5f, 3.0f); } },
        { "slot VEL > CUT, slot LFO1 > PW", [](EngineBase& e) { u4uv::slot(e, 0, ModSrc::Velocity, (int) D::Cutoff, 0.5f); u4uv::lfo(e, 0, D::Off, 0.3f, 3.0f);
                                                              u4uv::slot(e, 1, ModSrc::Lfo1, (int) D::PulseWidth, 0.5f); } },
    };
    const int block = 512, blocks = 400;     // 4.3 s of audio
    for (auto& row : rows)
    {
        double best[2] = { 1.0e9, 1.0e9 };
        for (int rep = 0; rep < 7; ++rep)
            for (int which = 0; which < 2; ++which)
            {
                std::unique_ptr<EngineBase> e;
                if (which == 0) e = std::make_unique<olduv::UnifiedVoiceEngine>();
                else            e = std::make_unique<UnifiedVoiceEngine>();
                e->prepareToPlay(48000.0, block);
                row.set(*e);
                for (int n = 0; n < 8; ++n)
                    e->noteOn(48 + 3 * n, 0.8f);
                juce::AudioBuffer<float> buf(2, block);
                juce::MidiBuffer midi;
                const auto t0 = clock::now();
                for (int b = 0; b < blocks; ++b)
                {
                    buf.clear();
                    e->processBlock(buf, midi);
                }
                const double s = std::chrono::duration<double>(clock::now() - t0).count();
                best[which] = std::min(best[which], 100.0 * s / ((double) blocks * block / 48000.0));
            }
        logMessage(juce::String(row.name).paddedRight(' ', 36) + " old " + juce::String(best[0], 3) + " %  new " + juce::String(best[1], 3)
                   + " %  new/old " + juce::String(best[1] / best[0], 3));
    }
}

static U4UnifiedTests u4UnifiedTests;
