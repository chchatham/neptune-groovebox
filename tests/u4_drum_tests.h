#pragma once

#include <juce_core/juce_core.h>
#include "../src/engine/DrumEngine.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/PresetManager.h"
#include "../src/project/presets/ResonatorKits.h"
#include "old/OldDrumEngine.h"
#include "fast_measure.h"
#include "kick_points_tests.h"         // (the old kits' kicks as written)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

// -----------------------------------------------------------------------------
// U4-DRUM: the drum kit on the modulation router (src/engine/ModRouter.h: one router for the kit,
// each drum its own mod::Voice and its own set values of the kick machine's and the resonator
// drum's cells) and its oscillators on PhaseOsc (each pad a WAVE; PM and WPOS destinations),
// against the kit as it was (tests/old/OldDrumEngine.h, OldDrumVoice.h: the headers of 46fedbc
// under other names, on EngineLfoSection).
//
//   the table       46 destinations; the 42 that were there are where they were; PM WPOS ETIM FTIM
//   pages and ids   the router's pages (XIN new), WAV1 / WAV2, every cell set and read back
//   old against new every drum type (the kit's fourteen, and the four only setType reaches, on the
//                   voice alone) at three velocities, every factory kit, the default kit, the four
//                   resonator kits - plain bit for bit; 44.1 and 96 kHz with blocks of 37 and 100
//   with modulation every kit under LFOs and the strip; each destination; the slots; pressure;
//                   events that come before the strike in the same block
//   the randoms     RAND drawn from the one generator: repeats on a new kit, its spread against the old
//   new             WAVE, PM from the track input, WPOS, ETIM / FTIM
//   real time       finite and bounded at the ends
//
// NEPTUNE_U4_PRINT=1 prints the table; NEPTUNE_U4_ATLAS=<folder> writes the pairs that are not
// bit-identical as WAV (.ralph/atlas/U4-DRUM/); NEPTUNE_U4_DRUM_BENCH=1 (Release binary) times old
// and new.
//
// The verdicts (the rule of the wave-1 ports, fixed before the first run). MATCH: the same
// samples. CLOSE: the loudness of the whole render within 1 dB, the loudness over time (100 ms
// frames, where either is above -50 dB of the peak frame) within 2 dB, the brightness (energy-
// weighted mean frequency) within 10 %. LOST: anything else.
// -----------------------------------------------------------------------------
class U4DrumTests : public juce::UnitTest
{
public:
    U4DrumTests() : UnitTest("U4 Drum") {}

    using D = DrumEngine;
    using O = OldDrumEngine;
    using V = DrumVoice;
    using R = mod::Router;
    static constexpr double SR = 48000.0;

    struct Stereo { std::vector<float> l, r; };
    enum Kind { On, Off, Pressure, Strip, Bend, Param };
    struct Event { double at; Kind kind; int note; float value; int id = 0; };
    using Events = std::vector<Event>;
    using Setup = std::function<void(EngineBase&)>;
    using Input = std::function<float(long)>;

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
            case Param:    e.setParameter(ev.id, ev.value); break;
        }
    }

    // Events are applied at the first block that starts at or after their time, in their order.
    static Stereo render(EngineBase& e, const Events& events, double seconds, int block = 256, double sr = SR,
                         const Input* input = nullptr)
    {
        e.prepareToPlay(sr, block);
        e.setTempo(120.0f);
        const long total = (long) (seconds * sr);
        Stereo out;
        out.l.reserve((size_t) total + (size_t) block);
        out.r.reserve((size_t) total + (size_t) block);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        std::vector<float> in((size_t) block);
        size_t next = 0;
        for (long pos = 0; pos < total; pos += block)
        {
            while (next < events.size() && events[next].at * sr <= (double) pos)
                apply(e, events[next++]);
            if (input != nullptr)
            {
                for (int i = 0; i < block; ++i)
                    in[(size_t) i] = (*input)(pos + i);
                AudioInput ai;
                ai.left = ai.right = in.data();
                ai.frames = block;
                ai.envelope = 0.5f;
                e.setAudioInput(ai);
            }
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

    // ---- measuring (as the wave-1 ports) ----------------------------------------------------------
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
    static double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }
    static bool finite(const Stereo& s, float bound = 16.0f)
    {
        for (auto* v : { &s.l, &s.r })
            for (float x : *v)
                if (! std::isfinite(x) || std::abs(x) > bound)
                    return false;
        return true;
    }

    struct Compared
    {
        bool identical = false;
        double level = 0.0, curve = 0.0, bright = 0.0, residual = 0.0;
    };
    static Compared compare(const Stereo& older, const Stereo& newer, double sr = SR)
    {
        Compared c;
        c.identical = same(older, newer);
        const auto a = mono(older), b = mono(newer);
        c.level = db(rms(b, 0, b.size()) / std::max(rms(a, 0, a.size()), 1.0e-9));
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
        float ra = 0, ca = 0, rb = 0, cb = 0;
        fastmeasure::rmsAndCentroid(a, 2048, sr, ra, ca);
        fastmeasure::rmsAndCentroid(b, 2048, sr, rb, cb);
        c.bright = ca > 0.0f ? (double) cb / (double) ca - 1.0 : 0.0;
        return c;
    }
    static const char* verdict(const Compared& c)
    {
        if (c.identical) return "MATCH";
        if (std::abs(c.level) <= 1.0 && c.curve <= 2.0 && std::abs(c.bright) <= 0.10) return "CLOSE";
        return "LOST";
    }
    static bool lost(const Compared& c) { return juce::String(verdict(c)) == "LOST"; }

    static bool printing() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_PRINT", "").isNotEmpty(); }
    static juce::String atlas() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_ATLAS", ""); }

    static void writeWav(const juce::File& file, const Stereo& s, double sr)
    {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (! out.openedOk()) return;
        const int frames = (int) s.l.size(), bytes = frames * 4;
        out.write("RIFF", 4); out.writeInt(36 + bytes); out.write("WAVEfmt ", 8);
        out.writeInt(16); out.writeShort(1); out.writeShort(2); out.writeInt((int) sr); out.writeInt((int) sr * 4);
        out.writeShort(4); out.writeShort(16); out.write("data", 4); out.writeInt(bytes);
        for (int i = 0; i < frames; ++i)
            for (float v : { s.l[(size_t) i], s.r[(size_t) i] })
                out.writeShort((short) std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    int rows = 0, matches = 0, closes = 0, losts = 0;

    // The same setup and the same gesture on the old kit and on the new one.
    Compared against(const juce::String& name, const Setup& setUp, const Events& events, double seconds,
                     int block = 256, double sr = SR, const Input* input = nullptr, bool mustSound = true)
    {
        auto older = std::make_unique<O>();
        auto newer = std::make_unique<D>();
        if (setUp) { setUp(*older); setUp(*newer); }
        const Stereo a = render(*older, events, seconds, block, sr, input), b = render(*newer, events, seconds, block, sr, input);
        const Compared c = compare(a, b, sr);
        ++rows;
        (c.identical ? matches : (lost(c) ? losts : closes))++;
        if (printing())
            logMessage(juce::String::formatted("U4-DRUM | %-46s | %-5s | level %+6.2f dB | over time %5.2f dB | bright %+6.1f %% | residual %6.1f dB",
                                               name.toRawUTF8(), verdict(c), c.level, c.curve, 100.0 * c.bright, c.residual));
        if (atlas().isNotEmpty() && ! c.identical)
        {
            const juce::File folder(atlas());
            folder.createDirectory();
            const juce::String file = name.replaceCharacters(" >/*,()+", "________").replace("__", "_");
            writeWav(folder.getChildFile(file + "_old.wav"), a, sr);
            writeWav(folder.getChildFile(file + "_new.wav"), b, sr);
        }
        expect(finite(b), name + ": finite and bounded");
        if (mustSound)
            expect(rms(mono(a), 0, a.l.size()) > 1.0e-5, name + ": the old kit sounds (a comparison of silences proves nothing)");
        return c;
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
    static int dest(const char* label) { return D::lfoDests().indexOfLabel(label); }
    static int id(int pad, int param) { return pad * D::PARAMS_PER_VOICE + param; }

    // A pad of each type as a kit would have it (dead_controls_tests.h's table).
    struct DrumSound { float tune, decay; };
    static DrumSound drumSound(int type)
    {
        // [FOLD] 10 types offered (TOM, SNAR, FMSN, CSYN are resonator drum sounds now).
        static const DrumSound sounds[D::NUM_DRUM_TYPES] = {
            { 50, 0.5f }, { 52, 0.6f }, { 55, 0.7f }, { 1200, 0.3f }, { 5000, 0.2f }, { 40, 0.1f },
            { 48, 0.5f }, { 7000, 1.0f }, { 100, 0.3f }, { 8000, 0.04f } };
        return sounds[std::clamp(type, 0, D::NUM_DRUM_TYPES - 1)];
    }
    static Setup padAs(int pad, int type, float tone = 0.5f, float drive = 0.2f, float snap = 0.6f)
    {
        return [=](EngineBase& e)
        {
            e.setParameter(id(pad, D::TYPE), (float) type);
            e.setParameter(id(pad, D::TUNE), drumSound(type).tune);
            e.setParameter(id(pad, D::DECAY), drumSound(type).decay);
            e.setParameter(id(pad, D::TONE), tone);
            e.setParameter(id(pad, D::DRIVE), drive);
            e.setParameter(id(pad, D::SNAP), snap);
            asWritten(e, pad, type);
        };
    }
    // [KICKS] FMKK, KICK and MKIK are points of the kick machine: a pad of one of them written
    // with the old type's cells is that kick as the kick machine once its KICK page is the
    // machine's own and its KCK2 page 0 (what a project saved before held: DrumEngine::
    // restoreSavedKick). On the old kit these ids are nothing a kick pad hears. Such rows are
    // CLOSE, not bit for bit: "Kick Points" measures them on its own, finer rule.
    static bool isKick(int type) { return DrumVoice::isKickPoint((DrumSynthType) type); }
    static void asWritten(EngineBase& e, int pad, int type)
    {
        if (! isKick(type))
            return;
        for (int n = 0; n < V::NumKickParams; ++n)
            e.setParameter(D::KICK_BASE + pad * V::NumKickParams + n, V::kickDefault(n));
        for (int n = 0; n < D::SOUND_CELLS; ++n)
            e.setParameter(D::SOUND_BASE + pad * D::SOUND_CELLS + n, 0.0f);
    }
    // A kit as it was written: on the old kit its kick pads get the old type's cells back (the
    // preset holds them as the kick machine's now: KickPointsTests::oldKitPads()).
    static Setup kitAsWritten(const Preset& preset)
    {
        return [preset](EngineBase& e)
        {
            presetSetup(preset)(e);
            if (dynamic_cast<O*>(&e) != nullptr)
                for (const auto& k : KickPointsTests::oldKitPads())
                    if (preset.name == k.kit)
                        KickPointsTests::oldPad(e, k);
        };
    }
    static bool hasMkik(const Preset& preset)
    {
        if (preset.name == "Default kit")
            return true;
        for (const auto& k : KickPointsTests::oldKitPads())
            if (preset.name == k.kit && k.type == DrumSynthType::ModCyclesKick)
                return true;
        return false;
    }
    static bool hasKicks(const Preset& preset)
    {
        if (preset.name == "Default kit")
            return true;
        for (const auto& k : KickPointsTests::oldKitPads())
            if (preset.name == k.kit)
                return true;
        return false;
    }
    // Kits whose cymbal-synth (CSYN) sounds were folded into the resonator drum:
    // their renders are now CLOSE, not bit-identical.
    static bool hasFoldedCymbals(const Preset& preset)
    {
        return preset.name == CymbalPresets::worksName
            || preset.name == CymbalPresets::houseName;
    }
    // Two hits of a pad, the second while the first rings, each let go after 0.15 s.
    static Events twoHits(int pad, float velocity)
    {
        return { { 0.0, On, pad, velocity }, { 0.15, Off, pad, 0.0f }, { 0.45, On, pad, velocity * 0.7f }, { 0.6, Off, pad, 0.0f } };
    }
    // Every pad of the kit in turn, at four velocities, the last four pads pressed after the hit.
    static Events wholeKit()
    {
        Events ev;
        const float vel[4] = { 1.0f, 0.55f, 0.8f, 0.3f };
        for (int p = 0; p < 16; ++p)
        {
            const double t = 0.14 * p;
            ev.push_back({ t, On, p, vel[p % 4] });
            if (p >= 12)
                ev.push_back({ t + 0.03, Pressure, p, 0.6f });
            ev.push_back({ t + 0.09, Off, p, 0.0f });
        }
        // ... and a chord of kick, snare and hat at the end.
        for (int p : { 8, 12, 14 })
            ev.push_back({ 2.4, On, p, 0.9f });
        for (int p : { 8, 12, 14 })
            ev.push_back({ 2.5, Off, p, 0.0f });
        return ev;
    }

    static std::vector<Preset> factoryKits()
    {
        std::vector<Preset> out;
        for (auto& p : FactoryPresets::getAll())
            if (p.engineType == EngineType::Drum)
                out.push_back(p);
        return out;
    }
    static Setup presetSetup(const Preset& preset)
    {
        return [preset](EngineBase& e)
        {
            for (auto& [pid, value] : PresetManager::soundOf(preset))
                e.setParameter(pid, value);
        };
    }

    // ---- the tests -----------------------------------------------------------------------------
    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_DRUM_BENCH", "").isNotEmpty())
        {
            bench();
            return;
        }
        testTable();
        testPagesAndIds();
        testVoicesAlone();
        testTypesInTheKit();
        testKits();
        testOtherRates();
        testBeforeTheStrike();
        testModulation();
        testRandoms();
        testWave();
        testPhaseModulation();
        testEnvelopeTimes();
        testEdges();
        testReview();
        logMessage(juce::String::formatted("U4-DRUM: %d rows old against new: %d MATCH, %d CLOSE, %d LOST", rows, matches, closes, losts));
    }

    void testTable()
    {
        beginTest("The table: the 42 destinations that were there are where they were, then PM WPOS ETIM FTIM");
        const auto& t = D::lfoDests();
        const auto& old = O::lfoDests();
        expectEquals(t.size(), (int) V::NumModDests);
        expectEquals(t.size(), 46);
        expectEquals(old.size(), 42);
        for (int i = 0; i < old.size(); ++i)
            expectEquals(juce::String(t.labels()[i]), juce::String(old.labels()[i]), "destination " + juce::String(i));
        const char* const added[4] = { "PM", "WPOS", "ETIM", "FTIM" };
        for (int i = 0; i < 4; ++i)
            expectEquals(juce::String(t.labels()[42 + i]), juce::String(added[i]));
        // (What the engine only asserts in a Debug build: the places the voice reads by number.)
        expectEquals(t.indexOfLabel("LRAT"), (int) V::ModLfoRate);
        expectEquals(t.indexOfLabel("LDPT"), (int) V::ModLfoDepth);
        expectEquals(t.indexOfLabel("PM"), (int) V::ModPhase);
        expectEquals(t.ampTime(), (int) V::ModEnvTime);
        expectEquals(t.filterTime(), (int) V::ModFilterTime);
        // Per sample: what the voice reads sample by sample.
        for (int d : { (int) V::ModPitch, (int) V::ModTone, (int) V::ModDrive, (int) V::ModAmp, (int) V::ModPan, (int) V::ModPhase, (int) V::ModWavePos })
            expect(t[d].perSample, juce::String(t.labels()[d]) + " per sample");
        for (int d : { (int) V::ModDecay, (int) V::ModSnap, (int) V::ModKickFirst, (int) V::ModRsnFirst, (int) V::ModEnvTime })
            expect(! t[d].perSample, juce::String(t.labels()[d]) + " at control rate");
        // The kick machine's cells are pad 1's ids; each drum reads its own (useBase).
        for (int k = 0; k < V::NumKickParams; ++k)
            expectEquals(t[V::ModKickFirst + k].id, D::KICK_BASE + k);
        // The voice's own set values hold the table's bases: 1 for the factors (PTCH DEC AMP ETIM FTIM).
        D e;
        for (int d : { (int) V::ModPitch, (int) V::ModDecay, (int) V::ModAmp, (int) V::ModEnvTime, (int) V::ModFilterTime })
            expectEquals(e.getVoice(3).modBaseOf(d), 1.0f, juce::String(t.labels()[d]));
        e.setParameter(D::KICK_BASE + 5 * V::NumKickParams + V::KickSweep, 50.0f);
        expectEquals(e.getVoice(5).modBaseOf(V::ModKickFirst + V::KickSweep), 50.0f, "pad 6's SWEP is its own");
        expectEquals(e.getVoice(4).modBaseOf(V::ModKickFirst + V::KickSweep), V::kickDefault(V::KickSweep), "pad 5's is not moved");
        // The strip's defaults: DEC and DRIV, as before.
        expectEquals(e.getParameter(R::stripDestId(0)), (float) V::ModDecay);
        expectWithinAbsoluteError(e.getParameter(R::stripAmountId(0)), 0.7925f, 1.0e-6f);
        expectEquals(e.getParameter(R::stripDestId(1)), (float) V::ModDrive);
        expectWithinAbsoluteError(e.getParameter(R::stripAmountId(1)), 0.5f, 1.0e-6f);
    }

    void testPagesAndIds()
    {
        beginTest("Pages and ids: the pads' 48, WAV1 WAV2, then XIN, SLT1, LSHP, STRIP, LFO1, LFO2; every cell set and read back");
        D e;
        const auto pages = e.getParamPages();
        const int pads = D::PAGES_PER_PAD * D::NUM_VOICES;
        expectEquals(juce::String(pages[(size_t) pads].name), juce::String("WAV1"));
        expectEquals(juce::String(pages[(size_t) pads + 1].name), juce::String("WAV2"));
        expectEquals(juce::String(pages[(size_t) pads + 2].name), juce::String("XIN"));
        expectEquals(juce::String(pages.back().name), juce::String("LFO2"));
        expectEquals(juce::String(pages[pages.size() - 2].name), juce::String("LFO1"));
        expectEquals(juce::String(pages[pages.size() - 3].name), juce::String("STRIP"));
        for (size_t i = (size_t) pads; i < pages.size(); ++i)
            for (auto& p : pages[i].params)
            {
                if (! p.isValid()) continue;
                float v = p.isDiscrete() ? std::min(p.maxVal, p.minVal + 1.0f) : p.minVal + 0.37f * (p.maxVal - p.minVal);
                // (A place a list keeps and offers no more - a pad's WAVE had CYCL at 1, which played
                // the sine and is read back as SINE - is not what a knob can set: the next one that is.)
                while (! p.isOffered((int) v) && v < p.maxVal)
                    v += 1.0f;
                e.setParameter(p.id, v);
                expectWithinAbsoluteError(e.getParameter(p.id), v, 1.0e-4f, juce::String(pages[i].name) + " " + p.label);
                expect(p.id >= 128 && (p.id < 150 || (p.id >= 158 && p.id <= 189) || R::ownsSlotBlock(p.id)), "id in the router's or WAVE's block: " + juce::String(p.id));
            }
        for (int pad = 0; pad < 16; ++pad)
        {
            e.setParameter(D::WAVE_BASE + pad, (float) (2 + pad));
            expectEquals(e.getParameter(D::WAVE_BASE + pad), (float) (2 + pad));
            expectEquals(e.getVoice(pad).getWave(), 2 + pad);
        }
        e.setParameter(D::WAVE_BASE, 1.0e9f);
        expectEquals(e.getParameter(D::WAVE_BASE), (float) (phaseosc::waveCount() - 1), "WAVE held to its list");
        e.setParameter(D::WAVE_BASE, std::nanf(""));
        expectEquals(e.getParameter(D::WAVE_BASE), (float) (phaseosc::waveCount() - 1), "what is no number is not taken");
    }

    // Every drum type on the voice alone (what SampleBank renders its hits with), old against new.
    void testVoicesAlone()
    {
        beginTest("Old against new: every drum type on the voice alone (the sample bank's hits), three velocities, bit for bit");
        for (int type = 0; type <= (int) DrumSynthType::OpenHat; ++type)
            for (float vel : { 0.25f, 0.6f, 1.0f })
            {
                const auto t = (DrumSynthType) type;
                auto run = [&](auto& v)
                {
                    v.setSampleRate(SR);
                    v.setType(t);
                    v.setTune(type < D::NUM_DRUM_TYPES ? drumSound(type).tune : 600.0f);
                    v.setDecay(type < D::NUM_DRUM_TYPES ? drumSound(type).decay : 0.2f);
                    v.setTone(0.6f);
                    v.setDrive(0.3f);
                    v.setSnap(0.7f);
                    v.setLevel(0.9f);
                    if constexpr (std::is_same<std::decay_t<decltype(v)>, DrumVoice>::value)
                        if (isKick(type))
                            v.applyKickPoint(DrumVoice::kickPoint(t, drumSound(type).tune, drumSound(type).decay, 0.6f, 0.3f, 0.7f, 0.9f));
                    v.noteOn(vel);
                    std::vector<float> out((size_t) (0.6 * SR));
                    for (size_t i = 0; i < out.size(); ++i)
                    {
                        if (i == (size_t) (0.25 * SR))
                            v.noteOn(vel * 0.5f);        // again while it rings
                        out[i] = v.process();
                    }
                    return out;
                };
                OldDrumVoice a;
                DrumVoice b;
                const auto x = run(a), y = run(b);
                if (isKick(type))       // [KICKS] the kick as the kick machine: Kick Points' rule
                    expect(juce::String(KickPointsTests::verdict(KickPointsTests::compare(x, y))) != "LOST",
                           juce::String("type ") + juce::String(type) + " velocity " + juce::String(vel) + ": the kick as written");
                else
                    expect(x == y, juce::String("type ") + juce::String(type) + " velocity " + juce::String(vel));
                expect(rms(x, 0, x.size()) > 1.0e-5, juce::String("type ") + juce::String(type) + " sounds");
            }
    }

    void testTypesInTheKit()
    {
        beginTest("Old against new: every TYPE on a pad of the kit, three velocities, a second hit while it rings: bit for bit");
        for (int type = 0; type < D::NUM_DRUM_TYPES; ++type)
            for (float vel : { 0.25f, 0.6f, 1.0f })
            {
                const Compared c = against(juce::String("TYPE ") + D::typeNames()[type] + " vel " + juce::String(vel, 2),
                                           padAs(8, type), twoHits(8, vel), 1.2);
                expect(isKick(type) ? ! lost(c) : c.identical, juce::String(D::typeNames()[type]) + " at " + juce::String(vel) + ": " + verdict(c));
            }
        // The kick machine's own page, the bank's models, the resonator drum's sounds.
        for (float sweep : { 0.0f, 60.0f })
        {
            const Setup s = [sweep](EngineBase& e)
            {
                padAs(8, (int) DrumSynthType::KickSynth)(e);
                e.setParameter(D::KICK_BASE + 8 * 8 + V::KickSweep, sweep);
                e.setParameter(D::KICK_BASE + 8 * 8 + V::KickShape, 0.7f);
                e.setParameter(D::KICK_BASE + 8 * 8 + V::KickKnock, 0.8f);
                e.setParameter(D::KICK_BASE + 8 * 8 + V::KickHold, 0.05f);
            };
            const Compared c = against("KSYN SWEP " + juce::String(sweep), s, twoHits(8, 0.9f), 1.2);
            expect(c.identical, "KSYN: " + juce::String(verdict(c)));
        }
        for (int model = 0; model < OscBank::NumModels; ++model)
        {
            const Setup s = [model](EngineBase& e)
            {
                padAs(8, (int) DrumSynthType::OscBank)(e);
                e.setParameter(D::SOUND_BASE + 8 * 8 + OscBank::ParModel, (float) model);
            };
            const Compared c = against("BANK model " + juce::String(model), s, twoHits(8, 0.8f), 1.2);
            expect(c.identical, "BANK " + juce::String(model) + ": " + verdict(c));
        }
        int sounds = 0;
        D::soundNames(sounds);
        for (int snd = 0; snd < sounds; snd += 3)
        {
            const Setup s = [snd](EngineBase& e)
            {
                padAs(8, (int) DrumSynthType::Resonator)(e);
                e.setParameter(D::SOUND_BASE + 8 * 8 + D::RsnSound, (float) snd);
                e.setParameter(D::SOUND_BASE + 8 * 8 + D::RsnMorph, 0.5f);
            };
            const Compared c = against("RSN sound " + juce::String(snd), s, twoHits(8, 0.8f), 1.2);
            expect(c.identical, "RSN " + juce::String(snd) + ": " + verdict(c));
        }
    }

    void testKits()
    {
        beginTest("Old against new: every factory kit, the default kit and the four resonator kits, every pad: plain bit for bit; under the kit's modulation CLOSE");
        auto kits = factoryKits();
        expectEquals((int) kits.size(), 10, "the ten factory kits");
        Preset init;
        init.name = "Default kit";
        init.engineType = EngineType::Drum;
        kits.push_back(init);
        for (auto& kit : kits)
        {
            // [KICKS] a kit with kicks: those as written on the old kit, CLOSE ("Kick Points": MATCH)
            // [CSYN FOLD] kits whose CSYN sounds were folded into the resonator drum: CLOSE
            const Compared c = against(kit.name, kitAsWritten(kit), wholeKit(), 3.2);
            expect((hasKicks(kit) || hasFoldedCymbals(kit)) ? ! lost(c) : c.identical, kit.name + ": " + verdict(c));
        }
        // The resonator kits carry LFOs and slots of their own: CLOSE is the bound.
        for (auto& kit : resonatorKits())
        {
            const Compared c = against(kit.name, presetSetup(kit), wholeKit(), 3.2);
            expect(! lost(c), kit.name + ": " + verdict(c));
        }
        // Every kit under the kit's modulation: an LFO on each pad's pitch (KEY), one on TONE
        // (FREE), the strip at 0.6 (its default routes: DEC and DRIV).
        const Setup moved = [](EngineBase& e)
        {
            lfo(e, 0, V::ModPitch, 0.15f, 6.0f, 0, 1);
            lfo(e, 1, V::ModTone, 0.4f, 2.0f, 0, 0);
        };
        Events withStrip = wholeKit();
        withStrip.insert(withStrip.begin(), { 0.0, Strip, 0, 0.6f });
        for (auto& kit : kits)
        {
            const Setup both = [kit, moved](EngineBase& e) { kitAsWritten(kit)(e); moved(e); };
            const Compared c = against(kit.name + " (LFOs, strip)", both, withStrip, 3.2);
            if (! hasKicks(kit))
            {
                expect(! lost(c), kit.name + " under the kit's modulation: " + verdict(c));
                continue;
            }
            // [KICKS] On a kick of FMKK, KICK or MKIK, TONE was the click, the sweep's depth or the
            // resonance; on the kick machine at that kick it is the low-pass (the old meaning is a
            // cell of its KICK / KCK2 page). An LFO on TONE moves another thing there now: that
            // row is logged, and the router is judged with the second LFO on DRIV, whose law the
            // kicks keep (PATH DRUM).
            logMessage(kit.name + " under the kit's modulation, LFO on TONE (another meaning on its kicks): " + verdict(c));
            const Setup drive = [kit](EngineBase& e)
            {
                kitAsWritten(kit)(e);
                lfo(e, 0, V::ModPitch, 0.15f, 6.0f, 0, 1);
                lfo(e, 1, V::ModDrive, 0.4f, 2.0f, 0, 0);
            };
            const Compared d = against(kit.name + " (LFOs on PTCH and DRIV, strip)", drive, withStrip, 3.2);
            // (MKIK's DRIV was its own saturation before the envelope: SAT on the kick machine at
            // that kick, at the value written. What the strip or an LFO adds to DRIV goes to the
            // kit's common drive now, as on every other drum: logged, not judged, on its kits.)
            if (hasMkik(kit))
                logMessage(kit.name + " under the kit's modulation (DRIV; MKIK's DRIV is the common drive now): " + verdict(d));
            else
                expect(! lost(d), kit.name + " under the kit's modulation (DRIV): " + verdict(d));
        }
        // The tom that opens into a gong (an envelope slot on MRPH): how far the body has gone at
        // 5 ms, old and new.
        {
            const auto tomToGong = resonatorKits()[0];
            O older; D newer;
            presetSetup(tomToGong)(older); presetSetup(tomToGong)(newer);
            const Events hit = { { 0.0, On, 9, 1.0f } };
            render(older, hit, 0.005);
            render(newer, hit, 0.005);
            const float a = older.getVoice(9).getResonator().getResonator().more()[res::Hand];
            const float b = newer.getVoice(9).getResonator().getResonator().more()[res::Hand];
            logMessage("Tom To Gong Kit, the body's way to the gong 5 ms after the stroke: old " + juce::String(a, 3) + ", new " + juce::String(b, 3));
            expect(std::abs(a - b) < 0.1f, "within a tenth of the way");
        }
    }

    void testOtherRates()
    {
        beginTest("Old against new at 44.1 and 96 kHz in blocks of 37 and 100 (not whole sub-blocks): plain bit for bit, modulated CLOSE");
        const Setup ksyn = padAs(8, (int) DrumSynthType::KickSynth);
        // [FOLD] NoiseSnare folds to Resonator; use Clap for the second type.
        const Setup snare = padAs(12, (int) DrumSynthType::Clap);
        const Setup rsn = [](EngineBase& e) { padAs(8, (int) DrumSynthType::Resonator)(e); e.setParameter(D::SOUND_BASE + 64 + D::RsnSound, 3.0f); };
        for (double sr : { 44100.0, 96000.0 })
            for (int block : { 37, 100 })
            {
                const juce::String at = " at " + juce::String(sr / 1000.0, 1) + " kHz, blocks of " + juce::String(block);
                struct One { const char* name; Setup setUp; int pad; };
                for (const One& one : std::vector<One> { { "KSYN", ksyn, 8 }, { "SNAR", snare, 12 }, { "RSN", rsn, 8 } })
                {
                    const Compared c = against(juce::String(one.name) + at, one.setUp, twoHits(one.pad, 0.8f), 1.0, block, sr);
                    expect(c.identical, juce::String(one.name) + at + ": " + verdict(c));
                }
                // [KICKS] (the default kit's pads 10 and 11 are kick points: CLOSE)
                const Compared k = against("Default kit" + at, nullptr, wholeKit(), 3.2, block, sr);
                expect(! lost(k), "the default kit" + at + ": " + verdict(k));
                const Setup moved = [](EngineBase& e)
                {
                    padAs(0, (int) DrumSynthType::SubBass)(e);
                    e.setParameter(id(0, D::DECAY), 0.5f);
                    lfo(e, 0, V::ModPitch, 0.3f, 5.0f);
                    lfo(e, 1, V::ModAmp, 0.5f, 3.0f);
                };
                const Events held = { { 0.0, On, 0, 0.9f }, { 0.8, Off, 0, 0.0f } };
                const Compared m = against("SUB, LFOs on PTCH and AMP" + at, moved, held, 1.2, block, sr);
                expect(! lost(m), "LFOs" + at + ": " + verdict(m));
            }
    }

    // What arrives for a drum in the same block as its hit, after the note: the strike is at the
    // drum's next sub-block now, and these are kept for after it.
    void testBeforeTheStrike()
    {
        beginTest("Old against new: a note-off, a choke, pressure and a second hit in the block of the hit: bit for bit");
        struct Case { const char* name; Setup setUp; Events events; bool sounds = true; };
        const std::vector<Case> cases = {
            { "open hat tapped (off in the block of the hit: silent in both)", nullptr, { { 0.0, On, 15, 0.9f }, { 0.0, Off, 15, 0.0f } }, false },
            { "open hat choked by the closed hat in the same block", nullptr, { { 0.0, On, 15, 0.9f }, { 0.0, On, 14, 0.9f }, { 0.1, Off, 14, 0.0f }, { 0.2, Off, 15, 0.0f } } },
            { "open hat choked a block later", nullptr, { { 0.0, On, 15, 0.9f }, { 0.1, On, 14, 0.9f }, { 0.2, Off, 14, 0.0f }, { 0.3, Off, 15, 0.0f } } },
            { "SUB on and off in one block (silent in both)", nullptr, { { 0.0, On, 0, 0.8f }, { 0.0, Off, 0, 0.0f } }, false },
            { "pressure on DEC in the block of the hit", nullptr, { { 0.0, On, 9, 0.8f }, { 0.0, Pressure, 9, 0.7f }, { 0.3, Pressure, 9, 0.2f }, { 0.5, Off, 9, 0.0f } } },
            { "pressure on a resonator drum in the block of the hit", nullptr, { { 0.0, On, 11, 0.8f }, { 0.0, Pressure, 11, 0.9f }, { 0.4, Off, 11, 0.0f } } },
            { "pressure, then the note let go, in the block of the hit", nullptr, { { 0.0, On, 12, 0.8f }, { 0.0, Pressure, 12, 0.9f }, { 0.0, Off, 12, 0.0f } } },
            { "a gated sound (HATO on RSN) tapped", [](EngineBase& e)
              {
                  e.setParameter(id(10, D::TYPE), (float) (int) DrumSynthType::Resonator);
                  e.setParameter(D::SOUND_BASE + 80 + D::RsnSound, (float) resonatorSoundIndex("HATO"));
                  // [KICKS] (the default kit's pad 11 was MKIK at these; it is the kick machine at
                  // that kick now, whose cells say it another way)
                  e.setParameter(id(10, D::TONE), 0.6f);
                  e.setParameter(id(10, D::DRIVE), 0.4f);
                  e.setParameter(id(10, D::SNAP), 0.6f);
              }, { { 0.0, On, 10, 0.9f }, { 0.0, Off, 10, 0.0f } } },
        };
        for (auto& c : cases)
        {
            const Compared r = against(c.name, c.setUp, c.events, 1.0, 256, SR, nullptr, c.sounds);
            // [KICKS] (pad 10 of the default kit is the kick machine at KICK: CLOSE)
            const bool onKick = std::any_of(c.events.begin(), c.events.end(), [](const Event& ev) { return ev.note == 9; });
            expect(onKick ? ! lost(r) : r.identical, juce::String(c.name) + ": " + verdict(r));
        }
        // Two hits of one pad in one block: one hit, at the later velocity (the old kit struck
        // twice before a sample was heard: the same, but for the strokes that add).
        const Compared twice = against("KICK struck twice in one block", nullptr, { { 0.0, On, 9, 0.3f }, { 0.0, On, 9, 0.9f } }, 0.8);
        expect(! lost(twice), "a kick struck twice in a block: " + juce::String(verdict(twice)));     // [KICKS] (pad 10: CLOSE)
    }

    void testModulation()
    {
        beginTest("Old against new, the kit's modulation: every destination under an LFO, the strip, the slots, pressure: MATCH or CLOSE");
        struct Case { juce::String name; Setup setUp; Events events; };
        std::vector<Case> cases;
        const Events held = { { 0.0, On, 8, 0.9f }, { 0.25, Off, 8, 0.0f }, { 0.55, On, 8, 0.6f }, { 0.8, Off, 8, 0.0f } };
        // The seven, each on a drum that hears it all the way.
        const struct { int dest; int type; } fixed[] = {
            { V::ModPitch, (int) DrumSynthType::SubBass }, { V::ModDecay, (int) DrumSynthType::AnalogKick },
            // [FOLD] NoiseSnare and FMSnare replaced: they fold to Resonator.
            { V::ModTone, (int) DrumSynthType::Clap }, { V::ModDrive, (int) DrumSynthType::FMKick },
            { V::ModSnap, (int) DrumSynthType::Clap }, { V::ModAmp, (int) DrumSynthType::SubBass }, { V::ModPan, (int) DrumSynthType::SubBass } };
        for (auto f : fixed)
            for (int trigger : { 1, 0 })
                cases.push_back({ juce::String("LFO1 > ") + D::lfoDests().labels()[f.dest] + (trigger ? " (KEY)" : " (FREE)") + " on " + D::typeNames()[f.type],
                                  [f, trigger](EngineBase& e)
                                  {
                                      padAs(8, f.type)(e);
                                      e.setParameter(id(8, D::DECAY), std::max(0.4f, drumSound(f.type).decay));
                                      lfo(e, 0, f.dest, 0.5f, 7.0f, 0, trigger);
                                  }, held });
        // The kick machine's eight, on the kick machine.
        for (int k = 0; k < V::NumKickParams; ++k)
            cases.push_back({ juce::String("LFO1 > ") + D::lfoDests().labels()[V::ModKickFirst + k] + " on KSYN",
                              [k](EngineBase& e)
                              {
                                  padAs(8, (int) DrumSynthType::KickSynth)(e);
                                  lfo(e, 0, V::ModKickFirst + k, 0.6f, 4.0f, 0, 1);
                                  lfo(e, 1, V::ModKickFirst + k, -0.3f, 9.0f, 1, 0);
                              }, held });
        // The resonator drum's 24, on a resonator drum (a gong, which rings through the render).
        for (int r = 0; r < V::NUM_RSN_DESTS; ++r)
            cases.push_back({ juce::String("LFO1 > ") + D::lfoDests().labels()[V::ModRsnFirst + r] + " on RSN GONG",
                              [r](EngineBase& e)
                              {
                                  padAs(8, (int) DrumSynthType::Resonator)(e);
                                  e.setParameter(id(8, D::TUNE), 220.0f);
                                  e.setParameter(D::SOUND_BASE + 64 + D::RsnSound, (float) resonatorSoundIndex("GONG"));
                                  lfo(e, 0, V::ModRsnFirst + r, 0.4f, 3.0f, 0, 1);
                              }, held });
        // LRAT / LDPT.
        cases.push_back({ "LFO2 > LRAT, LFO1 > PTCH on SUB", [](EngineBase& e)
                          { padAs(8, (int) DrumSynthType::SubBass)(e); lfo(e, 0, V::ModPitch, 0.3f, 4.0f); lfo(e, 1, V::ModLfoRate, 0.5f, 1.0f); }, held });
        // The strip: its defaults, and a route of its own.
        const Events stripUp = { { 0.0, Strip, 0, 0.8f }, { 0.0, On, 9, 0.9f }, { 0.4, Strip, 0, 0.2f }, { 0.6, On, 9, 0.7f }, { 0.65, Off, 9, 0.0f } };
        cases.push_back({ "the strip's defaults (DEC, DRIV) on KICK", nullptr, stripUp });
        cases.push_back({ "the strip on TONE (STRIP DST3)", [](EngineBase& e)
                          // [FOLD] NoiseSnare -> Clap (a type that hears TONE).
                          { e.setParameter(R::stripDestId(2), (float) V::ModTone); e.setParameter(R::stripAmountId(2), 0.6f); padAs(9, (int) DrumSynthType::Clap)(e); }, stripUp });
        // Free slots.
        cases.push_back({ "slot ENV > AMP on SUB", [](EngineBase& e) { padAs(8, (int) DrumSynthType::SubBass)(e); slot(e, 0, mod::Src::Env, V::ModAmp, -0.6f); }, held });
        cases.push_back({ "slot VEL > TONE on CLAP", [](EngineBase& e) { padAs(8, (int) DrumSynthType::Clap)(e); slot(e, 0, mod::Src::Velocity, V::ModTone, 0.5f); }, held });
        cases.push_back({ "slot KEY > PTCH (the drum's place)", [](EngineBase& e) { padAs(8, (int) DrumSynthType::SubBass)(e); slot(e, 0, mod::Src::Key, V::ModPitch, 0.5f); }, held });
        cases.push_back({ "slot LFO1 > SWEP via VEL on KSYN", [](EngineBase& e)
                          { padAs(8, (int) DrumSynthType::KickSynth)(e); lfo(e, 0, 0, 0.0f, 6.0f); e.setParameter(R::lfoId(0, R::Depth), 1.0f);
                            slot(e, 0, mod::Src::Lfo1, V::ModKickFirst + V::KickSweep, 0.5f, mod::Src::Velocity); }, held });
        const Events pressed = { { 0.0, On, 8, 0.9f }, { 0.05, Pressure, 8, 0.4f }, { 0.15, Pressure, 8, 0.9f }, { 0.3, Pressure, 8, 0.2f }, { 0.45, Off, 8, 0.0f } };
        cases.push_back({ "slot PRES > DRIV on FMKK", [](EngineBase& e) { padAs(8, (int) DrumSynthType::FMKick)(e); e.setParameter(id(8, D::DECAY), 0.8f); slot(e, 0, mod::Src::Pressure, V::ModDrive, 0.8f); }, pressed });
        cases.push_back({ "slot PRES > ENRG on a bowed RSN (CYMB)", [](EngineBase& e)
                          { padAs(8, (int) DrumSynthType::Resonator)(e); e.setParameter(D::SOUND_BASE + 64 + D::RsnSound, (float) resonatorSoundIndex("CYMB"));
                            slot(e, 0, mod::Src::Pressure, V::ModRsnFirst + 2, 0.5f); }, pressed });
        // The pad's own pressure routes (DrumATDest: not the router's): unchanged.
        for (auto dest : { DrumATDest::Pitch, DrumATDest::Decay, DrumATDest::Filter, DrumATDest::Level, DrumATDest::Drive, DrumATDest::Snap, DrumATDest::Pan })
            cases.push_back({ "pad pressure on DrumATDest " + juce::String((int) dest), [dest](EngineBase& e)
                              {
                                  padAs(8, (int) DrumSynthType::FMKick)(e);
                                  if (auto* n = dynamic_cast<D*>(&e)) { n->setSlotAftertouchDest(8, dest); n->setSlotAftertouchDepth(8, 1.0f); }
                                  if (auto* o = dynamic_cast<O*>(&e)) { o->setSlotAftertouchDest(8, dest); o->setSlotAftertouchDepth(8, 1.0f); }
                              }, pressed });
        int bad = 0;
        for (auto& c : cases)
        {
            const Compared r = against(c.name, c.setUp, c.events, 1.2);
            if (lost(r))
                ++bad;
            expect(! lost(r), c.name + ": " + verdict(r) + " (level " + juce::String(r.level, 2) + " dB, over time " + juce::String(r.curve, 2)
                              + " dB, bright " + juce::String(100.0 * r.bright, 1) + " %)");
        }
        logMessage("modulation rows LOST: " + juce::String(bad));

        // PRES is this drum's own pad: pressed, a slot PRES > AMP lifts it; another pad pressed does not.
        {
            const Setup s = [](EngineBase& e)
            {
                padAs(0, (int) DrumSynthType::SubBass)(e);
                padAs(1, (int) DrumSynthType::SubBass)(e);
                e.setParameter(id(0, D::TONE), 0.0f);
                slot(e, 0, mod::Src::Pressure, V::ModAmp, 1.0f);
            };
            const Events plainHit = { { 0.0, On, 0, 0.8f } };
            const Events ownPad = { { 0.0, On, 0, 0.8f }, { 0.0, Pressure, 0, 1.0f } };
            const Events otherPad = { { 0.0, On, 0, 0.8f }, { 0.0, On, 1, 0.01f }, { 0.0, Pressure, 1, 1.0f } };
            auto level = [](const Stereo& st) { const auto m = mono(st); return rms(m, (size_t) (0.05 * SR), (size_t) (0.09 * SR)); };
            const double a = level(play(s, plainHit, 0.1)), b = level(play(s, ownPad, 0.1)), c = level(play(s, otherPad, 0.1));
            logMessage("PRES > AMP: own pad pressed " + juce::String(db(b / a), 2) + " dB, another pad pressed " + juce::String(db(c / a), 2) + " dB");
            expect(db(b / a) > 4.0, "the drum's own pad lifts it");
            expect(std::abs(db(c / a)) < 0.5, "another pad's pressure is not this drum's");
        }
    }

    // RAND: one value a hit, from the one per-note generator; repeats on a new kit; spread as before.
    void testRandoms()
    {
        beginTest("RAND: drawn at each hit from the one generator - a new kit repeats it, each drum its own; its spread is the old one's (a two-sample test on the draws)");
        // The draw as heard: RAND > AMP at 50 on a held sine, the level at 20 ms against the plain one.
        auto draws = [this](EngineBase& e, int hits)
        {
            juce::ignoreUnused(this);
            e.prepareToPlay(SR, 64);
            padAs(0, (int) DrumSynthType::SubBass)(e);
            e.setParameter(id(0, D::TONE), 0.0f);
            slot(e, 0, mod::Src::Random, V::ModAmp, 0.5f);
            std::vector<double> out;
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            for (int h = 0; h < hits; ++h)
            {
                e.noteOn(0, 1.0f);
                double sum = 0.0; int count = 0;
                for (int b = 0; b < 24; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    if (b >= 12)
                        for (int i = 0; i < 64; ++i) { sum += (double) buf.getSample(0, i) * buf.getSample(0, i); ++count; }
                }
                e.noteOff(0);
                for (int b = 0; b < 8; ++b) { buf.clear(); e.processBlock(buf, midi); }
                out.push_back(std::sqrt(sum / count));
            }
            return out;
        };
        D a, b;
        const auto x = draws(a, 40), y = draws(b, 40);
        expect(x == y, "two new kits: the same draws");
        bool varies = false;
        for (size_t i = 1; i < x.size(); ++i) varies = varies || std::abs(x[i] - x[0]) > 1.0e-3 * x[0];
        expect(varies, "a new value at each hit");
        // The spread: the levels as gains 1 + 0.5 RAND, normalised by their range, old against new
        // (Kolmogorov-Smirnov over 300 hits each; the bound is the 1 % critical value, 0.133).
        O o; D n;
        auto xs = draws(o, 300), ys = draws(n, 300);
        auto norm = [](std::vector<double>& v)
        {
            const double lo = *std::min_element(v.begin(), v.end()), hi = *std::max_element(v.begin(), v.end());
            for (auto& s : v) s = (s - lo) / std::max(hi - lo, 1.0e-12);
            std::sort(v.begin(), v.end());
        };
        const double oldRange = *std::max_element(xs.begin(), xs.end()) / std::max(*std::min_element(xs.begin(), xs.end()), 1.0e-12);
        const double newRange = *std::max_element(ys.begin(), ys.end()) / std::max(*std::min_element(ys.begin(), ys.end()), 1.0e-12);
        norm(xs); norm(ys);
        double ks = 0.0;
        for (double t = 0.0; t <= 1.0; t += 0.002)
        {
            const double fa = (double) (std::upper_bound(xs.begin(), xs.end(), t) - xs.begin()) / (double) xs.size();
            const double fb = (double) (std::upper_bound(ys.begin(), ys.end(), t) - ys.begin()) / (double) ys.size();
            ks = std::max(ks, std::abs(fa - fb));
        }
        logMessage("RAND > AMP: KS distance old / new " + juce::String(ks, 3) + "; loudest over softest hit, old " + juce::String(oldRange, 3)
                   + ", new " + juce::String(newRange, 3) + " (1.5 / 0.5 = 3 for the whole swing)");
        expect(ks < 0.133, "the same spread of draws");
        expect(std::abs(oldRange - newRange) < 0.25, "the same swing");
        // Each drum its own draws: two pads struck together differ.
        {
            D e;
            e.prepareToPlay(SR, 64);
            padAs(0, (int) DrumSynthType::SubBass)(e);
            padAs(1, (int) DrumSynthType::SubBass)(e);
            e.setParameter(id(1, D::TUNE), e.getParameter(id(0, D::TUNE)));
            slot(e, 0, mod::Src::Random, V::ModAmp, 0.5f);
            e.noteOn(0, 1.0f);
            e.noteOn(1, 1.0f);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            e.processBlock(buf, midi);
            expect(e.getVoice(0).getMod().source(mod::Src::Random) != e.getVoice(1).getMod().source(mod::Src::Random), "two drums, two draws");
        }
    }

    // ---- what is new ---------------------------------------------------------------------------
    static Stereo play(const Setup& setUp, const Events& events, double seconds, const Input* input = nullptr, int block = 256)
    {
        D e;
        if (setUp) setUp(e);
        return render(e, events, seconds, block, SR, input);
    }

    void testWave()
    {
        beginTest("WAVE: a table on a kick's oscillator is another sound and stays bounded; a drum that reads no wave is untouched; WPOS scans the table");
        const Events hit = { { 0.0, On, 8, 0.9f }, { 0.3, Off, 8, 0.0f } };
        const int table = 2 + 1;        // (the second built-in table)
        // [FOLD] Tom and FMSnare removed: they fold to Resonator (which reads no wave).
        for (int type : { (int) DrumSynthType::KickSynth, (int) DrumSynthType::SubBass, (int) DrumSynthType::AnalogKick })
        {
            const auto plain = play(padAs(8, type), hit, 0.6);
            const auto waved = play([&](EngineBase& e) { padAs(8, type)(e); e.setParameter(D::WAVE_BASE + 8, (float) table); }, hit, 0.6);
            const Compared c = compare(plain, waved);
            expect(! c.identical && finite(waved), juce::String(D::typeNames()[type]) + ": the wave is heard, bounded");
            // SINE is the sine.
            const auto sine = play([&](EngineBase& e) { padAs(8, type)(e); e.setParameter(D::WAVE_BASE + 8, 0.0f); }, hit, 0.6);
            expect(same(plain, sine), juce::String(D::typeNames()[type]) + ": SINE is the drum as it was");
        }
        // [FOLD] CSYN removed: it folds to Resonator (which is already in the list).
        for (int type : { (int) DrumSynthType::Clap, (int) DrumSynthType::OscBank, (int) DrumSynthType::Resonator })
        {
            const auto plain = play(padAs(8, type), hit, 0.6);
            const auto waved = play([&](EngineBase& e) { padAs(8, type)(e); e.setParameter(D::WAVE_BASE + 8, (float) table); }, hit, 0.6);
            expect(same(plain, waved), juce::String(D::typeNames()[type]) + ": reads no wave");
            D e;
            padAs(8, type)(e);
            expect(! e.getVoice(8).readsWave());
        }
        // Another pad's WAVE is not this pad's.
        {
            const auto plain = play(padAs(8, (int) DrumSynthType::SubBass), hit, 0.6);
            const auto other = play([&](EngineBase& e) { padAs(8, (int) DrumSynthType::SubBass)(e); e.setParameter(D::WAVE_BASE + 9, (float) table); }, hit, 0.6);
            expect(same(plain, other), "pad 10's WAVE leaves pad 9 alone");
        }
        // WPOS: a slot ONE > WPOS moves the place in a table of several frames.
        {
            const auto& tables = WavetableFactory::builtInTables();
            int multi = -1;
            for (int t = 0; t < (int) tables.size() && multi < 0; ++t)
                if (tables[(size_t) t]->numFrames > 4) multi = t;
            expect(multi >= 0, "a built-in table of several frames");
            const Setup base = [&](EngineBase& e) { padAs(8, (int) DrumSynthType::SubBass)(e); e.setParameter(id(8, D::TONE), 0.0f); e.setParameter(D::WAVE_BASE + 8, (float) (2 + multi)); };
            const auto at0 = play(base, hit, 0.4);
            const auto at1 = play([&](EngineBase& e) { base(e); slot(e, 0, mod::Src::One, V::ModWavePos, 1.0f); }, hit, 0.4);
            float r0 = 0, c0 = 0, r1 = 0, c1 = 0;
            fastmeasure::rmsAndCentroid(mono(at0), 2048, SR, r0, c0);
            fastmeasure::rmsAndCentroid(mono(at1), 2048, SR, r1, c1);
            logMessage("WPOS 0 against 1: brightness " + juce::String(c0, 0) + " Hz, " + juce::String(c1, 0) + " Hz");
            expect(! same(at0, at1) && std::abs(c1 - c0) > 0.02f * c0, "the place in the table is heard");
        }
    }

    void testPhaseModulation()
    {
        beginTest("PM: the track input on a kick's phase (XIN > PM) - sidebands at the input's distance, nothing while the input is silent, bounded at the limit");
        const Events hit = { { 0.0, On, 8, 0.9f }, { 0.5, Off, 8, 0.0f } };
        const Setup sub = [](EngineBase& e)
        {
            padAs(8, (int) DrumSynthType::SubBass)(e);
            e.setParameter(id(8, D::TUNE), 110.0f);
            e.setParameter(id(8, D::TONE), 0.0f);
            e.setParameter(id(8, D::DRIVE), 0.0f);
            e.setParameter(id(8, D::SNAP), 0.5f);
            e.setParameter(id(8, D::DECAY), 1.0f);
        };
        const Setup pm = [sub](EngineBase& e)
        {
            sub(e);
            e.setParameter(R::xinId(R::XinDest1), (float) V::ModPhase);
            e.setParameter(R::xinId(R::XinAmount1), 1.0f);
        };
        const Input silent = [](long) { return 0.0f; };
        const Input tone = [](long i) { return 0.8f * std::sin(2.0f * 3.14159265f * 330.0f * (float) i / (float) SR); };
        const auto plain = play(sub, hit, 0.4);
        const auto quiet = play(pm, hit, 0.4, &silent);
        expect(same(plain, quiet), "a silent input on PM: the drum as it is");
        const auto moved = play(pm, hit, 0.4, &tone);
        expect(finite(moved), "bounded");
        // The kick's energy at 110 + 330 = 440 Hz: up from nothing.
        auto band = [](const Stereo& s, double hz)
        {
            const auto m = mono(s);
            double re = 0.0, im = 0.0;
            for (size_t i = (size_t) (0.05 * SR); i < (size_t) (0.35 * SR); ++i)
            {
                const double ph = 2.0 * 3.141592653589793 * hz * (double) i / SR;
                re += m[i] * std::cos(ph); im += m[i] * std::sin(ph);
            }
            return std::sqrt(re * re + im * im);
        };
        const double before = band(plain, 440.0), after = band(moved, 440.0), carrier = band(plain, 110.0);
        logMessage("PM: 440 Hz at " + juce::String(db(before / carrier), 1) + " dB of the carrier without, " + juce::String(db(after / carrier), 1) + " dB with");
        expect(after > 10.0 * before && after > 0.05 * carrier, "a sideband where the input puts it");
        // At the limit: many slots on PM, a loud input.
        const auto deep = play([pm](EngineBase& e)
        {
            pm(e);
            for (int k = 0; k < 6; ++k) slot(e, k, mod::Src::Xin, V::ModPhase, 1.0f);
        }, hit, 0.4, &tone);
        expect(finite(deep, 4.0f), "PM at its limit: bounded");
        // ... on a drum that reads no wave: nothing.
        const Setup clap = [](EngineBase& e)
        {
            padAs(8, (int) DrumSynthType::Clap)(e);
            e.setParameter(R::xinId(R::XinDest1), (float) V::ModPhase);
            e.setParameter(R::xinId(R::XinAmount1), 1.0f);
        };
        expect(same(play(padAs(8, (int) DrumSynthType::Clap), hit, 0.4), play(clap, hit, 0.4, &tone)), "a clap has no oscillator to move");
    }

    void testEnvelopeTimes()
    {
        beginTest("ETIM / FTIM: the amp envelope and the drum's second envelope (the sweep, the body) as a factor");
        const Events hit = { { 0.0, On, 8, 0.9f } };
        auto tail = [](const Stereo& s)
        {
            const auto m = mono(s);
            return rms(m, (size_t) (0.25 * SR), (size_t) (0.35 * SR));
        };
        const Setup kick = [](EngineBase& e) { padAs(8, (int) DrumSynthType::AnalogKick)(e); e.setParameter(id(8, D::DECAY), 0.35f); };
        const auto plain = play(kick, hit, 0.5);
        const auto longer = play([kick](EngineBase& e) { kick(e); slot(e, 0, mod::Src::One, V::ModEnvTime, 0.6f); }, hit, 0.5);
        const auto shorter = play([kick](EngineBase& e) { kick(e); slot(e, 0, mod::Src::One, V::ModEnvTime, -0.6f); }, hit, 0.5);
        logMessage("ETIM: the tail at 0.3 s " + juce::String(db(tail(longer) / tail(plain)), 1) + " dB with +60, " + juce::String(db(tail(shorter) / std::max(tail(plain), 1e-12)), 1) + " dB with -60");
        expect(tail(longer) > 4.0 * tail(plain), "ETIM +60: the kick rings longer");
        expect(tail(shorter) < 0.5 * tail(plain), "ETIM -60: shorter");
        // FTIM on the kick machine: the sweep takes longer.
        const Setup ksyn = [](EngineBase& e) { padAs(8, (int) DrumSynthType::KickSynth)(e); e.setParameter(D::KICK_BASE + 64 + V::KickSweep, 48.0f); };
        const auto sweep = play(ksyn, hit, 0.3);
        const auto slow = play([ksyn](EngineBase& e) { ksyn(e); slot(e, 0, mod::Src::One, V::ModFilterTime, 0.8f); }, hit, 0.3);
        float r0 = 0, c0 = 0, r1 = 0, c1 = 0;
        auto early = [](const Stereo& s) { auto m = mono(s); m.resize((size_t) (0.08 * SR)); return m; };
        fastmeasure::rmsAndCentroid(early(sweep), 1024, SR, r0, c0);
        fastmeasure::rmsAndCentroid(early(slow), 1024, SR, r1, c1);
        logMessage("FTIM on KSYN: the first 80 ms at " + juce::String(c0, 0) + " Hz, " + juce::String(c1, 0) + " Hz with FTIM +80");
        expect(c1 > 1.15f * c0, "the sweep stays high for longer");
        // Let go, the time is 1 again: a hit after the slot has gone is the plain hit.
        D e;
        kick(e);
        slot(e, 0, mod::Src::One, V::ModEnvTime, 0.6f);
        render(e, hit, 0.3);
        e.setParameter(ModSlotParam::id(0, ModSlotParam::DEST), 0.0f);
        const auto after = render(e, hit, 0.5);
        expect(std::abs(db(tail(after) / tail(plain))) < 0.5, "the time scale is handed back at 1");
    }

    void testEdges()
    {
        beginTest("Edges: every destination of the table at both ends on every type, at 22.05 and 96 kHz: finite, bounded, at rest again");
        for (double sr : { 22050.0, 96000.0 })
            for (int type = 0; type < D::NUM_DRUM_TYPES; ++type)
                for (float amount : { 1.0f, -1.0f })
                {
                    D e;
                    padAs(8, type)(e);
                    e.setParameter(D::WAVE_BASE + 8, 7.0f);
                    for (int k = 0; k < 16; ++k)
                        slot(e, k, mod::Src::One, 1 + (k * 3 + (amount > 0 ? 0 : 1)) % (D::lfoDests().size() - 1), amount);
                    lfo(e, 0, V::ModPitch, 1.0f, 40.0f, 3, 0);
                    lfo(e, 1, V::ModPhase, 1.0f, 300.0f, 0, 0);
                    const Events ev = { { 0.0, Strip, 0, 1.0f }, { 0.0, On, 8, 1.0f }, { 0.01, Pressure, 8, 1.0f }, { 0.2, Off, 8, 0.0f } };
                    const auto out = render(e, ev, 0.4, 100, sr);
                    expect(finite(out, 16.0f), juce::String(D::typeNames()[type]) + " at " + juce::String(sr) + ", " + juce::String(amount));
                }
        // Every slot gone, the kit at rest is silent again and its voices let go.
        D e;
        lfo(e, 0, V::ModAmp, 1.0f, 5.0f);
        render(e, { { 0.0, On, 8, 1.0f } }, 0.2);
        e.setParameter(R::lfoId(0, R::DestCell), 0.0f);
        const auto rest = render(e, {}, 6.0);
        const auto m = mono(rest);
        expect(rms(m, m.size() - 4800, m.size()) < 1.0e-6, "silence once the hit has rung out");
        // ... as the old kit is after the same.
        O o;
        lfo(o, 0, V::ModAmp, 1.0f, 5.0f);
        render(o, { { 0.0, On, 8, 1.0f } }, 0.2);
        o.setParameter(R::lfoId(0, R::DestCell), 0.0f);
        render(o, {}, 6.0);
        for (int v = 0; v < D::NUM_VOICES; ++v)
            expect(e.getVoice(v).isActive() == o.getVoice(v).isActive(), "drum " + juce::String(v + 1) + " at rest as in the old kit: "
                   + (e.getVoice(v).isActive() ? "sounding" : "at rest"));
    }

    // ---- the review's (U4-DRUM review) -----------------------------------------------------------
    // Sample-wise equality (memcmp tells -0 from +0, which a sum of silences may give either way).
    static bool equalSamples(const Stereo& a, const Stereo& b)
    {
        if (a.l.size() != b.l.size()) return false;
        for (size_t i = 0; i < a.l.size(); ++i)
            if (a.l[i] != b.l[i] || a.r[i] != b.r[i]) return false;
        return true;
    }
    static Stereo sum(const Stereo& a, const Stereo& b)
    {
        Stereo s = a;
        for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] += b.l[i]; s.r[i] += b.r[i]; }
        return s;
    }

    void testReview()
    {
        beginTest("Review: two drums of the one router struck together, each its own cells; a choke and a roll under modulation; ETIM / FTIM base 1; WAVE swap; idle LFOs keep time");

        // 1. Two kick machines with their own SWEP and SHAP, two resonator drums with their own
        //    sounds, struck together under the kit's LFOs (KEY and FREE), a RAND slot and the strip:
        //    the kit is the sum of the drums struck alone, sample for sample (nothing of one drum's
        //    side of the router reaches the other).
        const Setup pair = [](EngineBase& e)
        {
            padAs(8, (int) DrumSynthType::KickSynth)(e);
            padAs(9, (int) DrumSynthType::KickSynth)(e);
            e.setParameter(D::KICK_BASE + 8 * 8 + V::KickSweep, 10.0f);
            e.setParameter(D::KICK_BASE + 9 * 8 + V::KickSweep, 60.0f);
            e.setParameter(D::KICK_BASE + 9 * 8 + V::KickShape, 0.8f);
            padAs(10, (int) DrumSynthType::Resonator)(e);
            padAs(11, (int) DrumSynthType::Resonator)(e);
            e.setParameter(D::SOUND_BASE + 10 * 8 + D::RsnSound, (float) resonatorSoundIndex("GONG"));
            e.setParameter(D::SOUND_BASE + 11 * 8 + D::RsnSound, (float) resonatorSoundIndex("CYMB"));
            lfo(e, 0, V::ModKickFirst + V::KickSweep, 0.3f, 6.0f, 0, 1);
            lfo(e, 1, V::ModRsnFirst, 0.3f, 2.0f, 0, 0);
            slot(e, 0, mod::Src::Random, V::ModKickFirst + V::KickShape, 0.2f);
            slot(e, 1, mod::Src::Velocity, V::ModTone, 0.3f);
        };
        auto hits = [](std::initializer_list<int> pads)
        {
            Events ev { { 0.0, Strip, 0, 0.5f } };
            for (int p : pads) ev.push_back({ 0.0, On, p, 0.9f });
            for (int p : pads) ev.push_back({ 0.3, Off, p, 0.0f });
            return ev;
        };
        const auto together = play(pair, hits({ 8, 9, 10, 11 }), 0.8);
        // (Summed in the kit's order: the mix adds the drums one after the other.)
        const auto apart = sum(sum(sum(play(pair, hits({ 8 }), 0.8), play(pair, hits({ 9 }), 0.8)),
                                   play(pair, hits({ 10 }), 0.8)), play(pair, hits({ 11 }), 0.8));
        expect(equalSamples(together, apart), "four drums struck together are the four struck alone, sample for sample");
        // ... and against the old kit (without the RAND slot: old and new draw differently). LOGGED,
        // NOT HELD: each of the four drums alone is CLOSE, but the second kick machine (SWEP 60
        // under LFO1 on SWEP) comes out with its phase moved (residual -1.8 dB alone: the router
        // moves SWEP every sub-block where the old kit did every sample), and layered on the first
        // kick the sum is another sound - measured LOST (level -2.2 dB, over time 3.2 dB, bright
        // +45 %). The pair is in the atlas for the owner; the old path is tests/old/.
        const Setup pairNoRand = [pair](EngineBase& e) { pair(e); e.setParameter(ModSlotParam::id(0, ModSlotParam::DEST), 0.0f); };
        const Compared c = against("review: two KSYN and two RSN struck together, modulated", pairNoRand, hits({ 8, 9, 10, 11 }), 0.8);
        logMessage(juce::String("review: two KSYN and two RSN together under modulation, old against new: ") + verdict(c)
                   + " (each drum alone CLOSE: the layered kicks' phase)");
        // Each kick machine moves around ITS OWN SWEP: a slot of a hundredth leaves each where it was.
        {
            const Setup own = [](EngineBase& e)
            {
                padAs(8, (int) DrumSynthType::KickSynth)(e);
                padAs(9, (int) DrumSynthType::KickSynth)(e);
                e.setParameter(D::KICK_BASE + 8 * 8 + V::KickSweep, 6.0f);
                e.setParameter(D::KICK_BASE + 9 * 8 + V::KickSweep, 66.0f);
            };
            for (int p : { 8, 9 })
            {
                const Events ev = { { 0.0, On, p, 0.9f } };
                const auto plain = play(own, ev, 0.3);
                const auto touched = play([own](EngineBase& e) { own(e); slot(e, 0, mod::Src::One, V::ModKickFirst + V::KickSweep, 0.01f); }, ev, 0.3);
                const Compared t = compare(plain, touched);
                expect(std::abs(t.bright) < 0.02 && std::abs(t.level) < 0.1,
                       "pad " + juce::String(p + 1) + ": SWEP + 1 % is near its own SWEP (bright " + juce::String(100.0 * t.bright, 1) + " %)");
            }
        }

        // 2. A choke under modulation: the open hat, moved by an LFO on PTCH and ETIM, cut by the
        //    closed hat; then open, closed, open again in one block (the last open hat sounds).
        {
            const Setup moved = [](EngineBase& e) { lfo(e, 0, V::ModPitch, 0.3f, 7.0f, 0, 1); slot(e, 0, mod::Src::Lfo2, V::ModEnvTime, 0.3f); lfo(e, 1, 0, 0.0f, 3.0f); e.setParameter(R::lfoId(1, R::Depth), 1.0f); };
            const Events choke = { { 0.0, On, 15, 0.9f }, { 0.12, On, 14, 0.9f }, { 0.2, Off, 14, 0.0f }, { 0.3, Off, 15, 0.0f } };
            const Compared m = against("review: open hat choked, LFO on PTCH, ETIM", moved, choke, 0.8);
            expect(! lost(m), juce::String("choke under modulation: ") + verdict(m));
            const Events again = { { 0.0, On, 15, 0.9f }, { 0.0, On, 14, 0.9f }, { 0.0, On, 15, 0.7f }, { 0.4, Off, 15, 0.0f }, { 0.4, Off, 14, 0.0f } };
            const Compared r = against("review: open, closed, open in one block", nullptr, again, 0.8);
            expect(r.identical, juce::String("open, closed, open in one block: ") + verdict(r));
        }

        // 3. A roll: twenty hits 25 ms apart, plain (bit for bit) and under an LFO on DEC (KEY) with
        //    the strip up; on the kick and on a resonator drum (whose strokes add).
        for (int pad : { 9, 11 })
        {
            Events roll;
            for (int k = 0; k < 20; ++k) { roll.push_back({ 0.025 * k, On, pad, 0.5f + 0.02f * (float) k }); roll.push_back({ 0.025 * k + 0.01, Off, pad, 0.0f }); }
            const Compared plain = against("review: a roll on pad " + juce::String(pad + 1), nullptr, roll, 1.2);
            expect(pad == 9 ? ! lost(plain) : plain.identical, "a roll, plain: " + juce::String(verdict(plain)));     // [KICKS] (pad 10: CLOSE)
            Events stripped = roll;
            stripped.insert(stripped.begin(), { 0.0, Strip, 0, 0.7f });
            const Compared m = against("review: a roll on pad " + juce::String(pad + 1) + ", LFO > DEC, strip",
                                       [](EngineBase& e) { lfo(e, 0, V::ModDecay, 0.4f, 9.0f, 0, 1); }, stripped, 1.2);
            expect(! lost(m), "a roll under modulation: " + juce::String(verdict(m)));
        }

        // 4. ETIM / FTIM are factors with base 1: on the resonator drum's own envelopes, moved while
        //    routed, 1 again once the slot is gone and the drum has rung out (before any new hit).
        {
            D e, plain;
            padAs(8, (int) DrumSynthType::Resonator)(e);
            padAs(8, (int) DrumSynthType::Resonator)(plain);
            slot(e, 0, mod::Src::One, V::ModEnvTime, 0.5f);
            slot(e, 1, mod::Src::One, V::ModFilterTime, -0.5f);
            render(e, { { 0.0, On, 8, 0.9f } }, 0.05);
            render(plain, { { 0.0, On, 8, 0.9f } }, 0.05);
            auto& rv = e.getVoice(8).getResonator();
            auto filterDecay = [](D& x) { return x.getVoice(8).getResonator().getFilter().getEnvelope().getParams().decay; };
            expect(rv.getAmpEnvelope().getTimeScale() > 1.5f, "ETIM +50 on RSN: its amp envelope slower (" + juce::String(rv.getAmpEnvelope().getTimeScale()) + ")");
            expect(filterDecay(e) < 0.67f * filterDecay(plain), "FTIM -50 on RSN: its filter envelope faster (" + juce::String(filterDecay(e) / filterDecay(plain), 3) + ")");
            e.setParameter(ModSlotParam::id(0, ModSlotParam::DEST), 0.0f);
            e.setParameter(ModSlotParam::id(1, ModSlotParam::DEST), 0.0f);
            e.allNotesOff();
            render(e, {}, 0.05);
            expect(rv.getAmpEnvelope().getTimeScale() == 1.0f && filterDecay(e) == filterDecay(plain),
                   "the slots gone and the drum at rest: both times 1 again");
            // How much ETIM +60 lengthens each resonator sound's tail (logged: a sound whose length is
            // its modes' and not its amp envelope's hears little of it).
            int heard = 0;
            juce::String quiet;
            for (int k = 0; k < (int) resonatorSounds().size(); ++k)
            {
                const Setup snd = [k](EngineBase& x) { padAs(8, (int) DrumSynthType::Resonator)(x); x.setParameter(D::SOUND_BASE + 64 + D::RsnSound, (float) k); x.setParameter(id(8, D::DECAY), 0.3f); };
                const Events hit = { { 0.0, On, 8, 0.9f }, { 0.05, Off, 8, 0.0f } };
                const auto a0 = mono(play(snd, hit, 0.8));
                const auto b0 = mono(play([snd](EngineBase& x) { snd(x); slot(x, 0, mod::Src::One, V::ModEnvTime, 0.6f); }, hit, 0.8));
                const double ta = rms(a0, (size_t) (0.4 * SR), (size_t) (0.7 * SR)), tb = rms(b0, (size_t) (0.4 * SR), (size_t) (0.7 * SR));
                if (tb > 1.41 * ta) ++heard; else quiet << resonatorSounds()[(size_t) k].tag << " ";
            }
            logMessage("review: ETIM +60 lengthens the tail (+3 dB at 0.4-0.7 s) of " + juce::String(heard) + " of " + juce::String((int) resonatorSounds().size())
                       + " resonator sounds; not: " + quiet);
        }

        // 5. Another WAVE under a ringing drum that reads one is carried off as a swapped sound;
        //    under one that reads none, nothing.
        for (int type : { (int) DrumSynthType::AnalogKick, (int) DrumSynthType::Clap })
        {
            D e;
            padAs(8, type)(e);
            render(e, { { 0.0, On, 8, 0.9f } }, 0.02);
            e.takeJump();
            e.setParameter(D::WAVE_BASE + 8, 3.0f);
            const int jump = e.takeJump();
            expect(jump == (type == (int) DrumSynthType::Clap ? (int) EngineBase::NoJump : (int) EngineBase::SoundSwapped),
                   juce::String(D::typeNames()[type]) + ": WAVE changed while ringing, jump " + juce::String(jump));
        }

        // 6. A drum at rest keeps its FREE LFO in time with the drums that sound.
        {
            D e;
            lfo(e, 0, V::ModPitch, 0.2f, 3.3f, 0, 0);
            render(e, { { 0.0, On, 0, 0.9f } }, 0.4);
            const double sounding = e.getVoice(0).getMod().getLfo(0).getPhase(), resting = e.getVoice(5).getMod().getLfo(0).getPhase();
            expect(std::abs(sounding - resting) < 1.0e-6, "FREE: a resting drum's LFO " + juce::String(resting, 4) + ", a sounding one's " + juce::String(sounding, 4));
        }

        // 8. The kick machine's cells as set stay set: after a block under an LFO on SWEP (ringing,
        //    then at rest with the LFO taken off), the cell reads its set value, and the next hit is
        //    the plain hit's sweep (the implementer's M9 survivor: restore() hands a kick cell to
        //    setControl, which does not take it, so only render's own hand-back keeps it).
        {
            D e;
            padAs(8, (int) DrumSynthType::KickSynth)(e);
            e.setParameter(D::KICK_BASE + 64 + V::KickSweep, 30.0f);
            lfo(e, 0, V::ModKickFirst + V::KickSweep, 0.8f, 13.0f, 0, 1);
            render(e, { { 0.0, On, 8, 0.9f } }, 0.03);
            expect(e.getVoice(8).getKickParam(V::KickSweep) == 30.0f, "SWEP under an LFO, ringing: the set value stays ("
                   + juce::String(e.getVoice(8).getKickParam(V::KickSweep)) + ")");
            e.setParameter(R::lfoId(0, R::DestCell), 0.0f);
            e.allNotesOff();
            render(e, {}, 0.05);
            expect(e.getVoice(8).getKickParam(V::KickSweep) == 30.0f, "... and at rest with the LFO gone");
        }

        // 9. Every pad-pressure route with the pressure in the block of the hit (before the strike):
        //    bit for bit as the old kit (the pan is the engine's, at once; the rest after the strike).
        for (auto dest : { DrumATDest::Pitch, DrumATDest::Decay, DrumATDest::Filter, DrumATDest::Level, DrumATDest::Drive, DrumATDest::Snap, DrumATDest::Pan })
        {
            const Setup s = [dest](EngineBase& e)
            {
                padAs(8, (int) DrumSynthType::KickSynth)(e);       // [KICKS] (was FMKK: a point of it now)
                if (auto* n = dynamic_cast<D*>(&e)) { n->setSlotAftertouchDest(8, dest); n->setSlotAftertouchDepth(8, 1.0f); }
                if (auto* o = dynamic_cast<O*>(&e)) { o->setSlotAftertouchDest(8, dest); o->setSlotAftertouchDepth(8, 1.0f); }
            };
            const Events ev = { { 0.0, On, 8, 0.9f }, { 0.0, Pressure, 8, 0.8f }, { 0.3, Off, 8, 0.0f } };
            const Compared c9 = against("review: pressure in the block of the hit, DrumATDest " + juce::String((int) dest), s, ev, 0.6);
            expect(c9.identical, "pressure before the strike, DrumATDest " + juce::String((int) dest) + ": " + verdict(c9));
        }

        // 10. Each CLOSE rule catches a loss built on purpose, the other two rules passing: the old
        //     kit's held SUB (a) 1.3 dB louder throughout - the whole-render rule (and the frames);
        //     (b) one 100 ms frame of the body 3 dB down - the frames' rule alone; (c) a quiet high
        //     tone mixed in, the level all but unchanged - the brightness rule alone.
        {
            O o;
            padAs(8, (int) DrumSynthType::SubBass)(o);
            o.setParameter(id(8, D::DECAY), 1.0f);
            const Stereo ref = render(o, { { 0.0, On, 8, 0.9f }, { 0.9, Off, 8, 0.0f } }, 1.0);
            Stereo louder = ref;
            for (auto* ch : { &louder.l, &louder.r }) for (auto& x : *ch) x *= (float) std::pow(10.0, 1.3 / 20.0);
            const Compared a = compare(ref, louder);
            expect(lost(a) && std::abs(a.level) > 1.0, "a 1.3 dB loss of level is LOST (level " + juce::String(a.level, 2) + " dB)");
            Stereo dip = ref;
            const size_t from = (size_t) (0.3 * SR), to = (size_t) (0.4 * SR);
            for (auto* ch : { &dip.l, &dip.r }) for (size_t i = from; i < to; ++i) (*ch)[i] *= (float) std::pow(10.0, -3.0 / 20.0);
            const Compared b = compare(ref, dip);
            expect(lost(b) && std::abs(b.level) <= 1.0 && std::abs(b.bright) <= 0.10,
                   "one frame 3 dB down is LOST by the frames' rule alone (level " + juce::String(b.level, 2) + " dB, over time " + juce::String(b.curve, 2) + " dB)");
            const double body = rms(mono(ref), 0, ref.l.size());
            Stereo bright = ref;
            const float amp = (float) (0.08 * body);
            for (size_t i = 0; i < bright.l.size(); ++i)
            {
                const float t = amp * std::sin(2.0f * 3.14159265f * 3000.0f * (float) i / (float) SR);
                bright.l[i] += t; bright.r[i] += t;
            }
            const Compared c3 = compare(ref, bright);
            expect(lost(c3) && std::abs(c3.level) <= 1.0 && c3.curve <= 2.0,
                   "a quiet 3 kHz tone is LOST by the brightness rule alone (bright " + juce::String(100.0 * c3.bright, 1) + " %, level "
                   + juce::String(c3.level, 2) + " dB, over time " + juce::String(c3.curve, 2) + " dB)");
        }

        // 11. PAN is heard in the stereo picture, which the CLOSE rule (measured on L + R) cannot
        //     see: the side signal (L - R) of an LFO on PAN, KEY and FREE, old against new within
        //     1 dB and far above the unmoved drum's; a slot ONE > PAN +100 puts the drum hard right.
        {
            auto side = [](const Stereo& s) { std::vector<float> d(s.l.size()); for (size_t i = 0; i < d.size(); ++i) d[i] = s.l[i] - s.r[i]; return rms(d, 0, d.size()); };
            const Events held = { { 0.0, On, 8, 0.9f }, { 0.6, Off, 8, 0.0f } };
            const Setup sub = [](EngineBase& e) { padAs(8, (int) DrumSynthType::SubBass)(e); e.setParameter(id(8, D::DECAY), 0.6f); };
            const double still = side(play(sub, held, 0.8));
            for (int trigger : { 1, 0 })
            {
                const Setup moved = [sub, trigger](EngineBase& e) { sub(e); lfo(e, 0, V::ModPan, 0.8f, 5.0f, 0, trigger); };
                O o; moved(o);
                const double a = side(render(o, held, 0.8)), b = side(play(moved, held, 0.8));
                expect(a > 10.0 * std::max(still, 1.0e-9) && std::abs(db(b / a)) < 1.0,
                       "LFO > PAN (" + juce::String(trigger ? "KEY" : "FREE") + "): the side signal old " + juce::String(db(a), 1) + " dB, new " + juce::String(db(b), 1) + " dB");
            }
            // (+100 is +0.5 on PAN 0.5: hard right; the drum's own PAN sets the gains at the strike.)
            const auto right = play([sub](EngineBase& e) { sub(e); slot(e, 0, mod::Src::One, V::ModPan, 1.0f); }, held, 0.8);
            const double l = rms(right.l, (size_t) (0.05 * SR), right.l.size()), r = rms(right.r, (size_t) (0.05 * SR), right.r.size());
            expect(r > 10.0 * l, "ONE > PAN +100: the drum hard right (left " + juce::String(db(l), 1) + " dB, right " + juce::String(db(r), 1) + " dB)");
        }

        // 7. PM is held to +-4 pi: five slots ONE > PM at 55 (5.5 pi asked) give 4 pi, a whole number
        //    of cycles - the plain drum, near enough (float rounding of the phase) - not 1.5 pi off.
        {
            const Setup sub = [](EngineBase& x) { padAs(8, (int) DrumSynthType::SubBass)(x); x.setParameter(id(8, D::TONE), 0.0f); };
            const Events ev = { { 0.0, On, 8, 0.9f }, { 0.3, Off, 8, 0.0f } };
            const auto plain = play(sub, ev, 0.4);
            const auto held = play([sub](EngineBase& x) { sub(x); for (int k = 0; k < 5; ++k) slot(x, k, mod::Src::One, V::ModPhase, 0.55f); }, ev, 0.4);
            const Compared c2 = compare(plain, held);
            logMessage("review: PM asked 5.5 pi, residual against the plain drum " + juce::String(c2.residual, 1) + " dB");
            expect(c2.residual < -40.0, "PM held at 4 pi: the plain drum");
        }
    }

    // ---- cost ----------------------------------------------------------------------------------
    void bench()
    {
        beginTest("Cost: eight pads ringing, old and new alternated, best of five (Release binary)");
        // mode 0 plain, 1 two LFOs, 2 (review) all sixteen pads with two LFOs and four slots
        // (one on the resonator drums' MRPH, one on the kick machine's SWEP), the strip up.
        auto timeOf = [](EngineBase& e, int mode)
        {
            e.prepareToPlay(SR, 256);
            if (mode >= 1)
            {
                lfo(e, 0, V::ModPitch, 0.2f, 5.0f);
                lfo(e, 1, V::ModTone, 0.3f, 3.0f, 0, 0);
            }
            if (mode == 2)
            {
                slot(e, 0, mod::Src::Lfo1, V::ModRsnFirst, 0.3f);
                slot(e, 1, mod::Src::Env, V::ModKickFirst + V::KickSweep, 0.3f);
                slot(e, 2, mod::Src::Velocity, V::ModDrive, 0.2f);
                slot(e, 3, mod::Src::Random, V::ModAmp, 0.2f);
                e.setModWheel(0.5f);
            }
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            const std::vector<int> pads = mode == 2 ? std::vector<int> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }
                                                    : std::vector<int> { 0, 8, 9, 10, 11, 12, 13, 14 };      // SUB, KSYN, KICK, MKIK, RSN TOM, RSN SNAR, CLAP, HAT
            const auto start = juce::Time::getHighResolutionTicks();
            for (int rep = 0; rep < 40; ++rep)
            {
                for (int p : pads) e.noteOn(p, 0.9f);
                for (int b = 0; b < 40; ++b) { buf.clear(); e.processBlock(buf, midi); }
                for (int p : pads) e.noteOff(p);
            }
            return juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start);
        };
        for (int mode : { 0, 1, 2 })
        {
            double bestOld = 1e9, bestNew = 1e9;
            for (int round = 0; round < 5; ++round)
            {
                { O o; bestOld = std::min(bestOld, timeOf(o, mode)); }
                { D n; bestNew = std::min(bestNew, timeOf(n, mode)); }
            }
            const double audio = 40.0 * 40.0 * 256.0 / SR;
            logMessage(juce::String::formatted("U4-DRUM cost %s: old %.3f ms, new %.3f ms for %.2f s of audio (new / old %.3f)",
                                               mode == 0 ? "plain   " : mode == 1 ? "two LFOs" : "16 pads, 2 LFOs, 4 slots, strip", bestOld * 1000.0, bestNew * 1000.0, audio, bestNew / bestOld));
        }
    }
};

static U4DrumTests u4DrumTests;
