#pragma once

// -----------------------------------------------------------------------------
// KICK POINTS: FMKK, KICK and MKIK as points of the kick machine (KSYN).
//
// The three kicks were sine sweeps of their own (tests/old/OldDrumVoice.h keeps them as they
// were). They are the kick machine now, with a second page, KCK2 (KIND LIN RES LPSW LPKT SAT EDGE
// PATH; every cell at 0 = the kick machine as it was), and DrumVoice::kickPoint turns a kick written
// with an old type's cells into the kick machine's. Measured here, old against new:
//   the three types       one cell at a time around a middle setting: TUNE, DEC, TONE, DRIV,
//                         SNAP, velocity; and a few settings far from it
//   the factory kits      every kick pad of every kit as it was written, the old engine with the
//                         old type against the new engine with the preset as it is now
//   the default kit       its pads 10 and 11 (KICK, MKIK)
//   a project saved before  a pad of an old kick TYPE with the KICK page as the kick machine's own
//                         and KCK2 at 0 (what such a pad saved) is the kick it was
//   the sample bank       its kick hits (KICK, and the breaks' kicks at their velocities)
// and: the page and its ids; TYPE turned to a point; each KCK2 cell heard; a KCK2 cell set under a
// ringing kick changes nothing of that hit (read at the strike); the KSYN that was untouched.
//
// The verdicts (fixed before the first run). MATCH: the loudness of the whole render within
// 0.5 dB, every 50 ms frame (where either is above -50 dB of the peak frame) within 1 dB, the
// first 10 ms within 1 dB, the brightness (power-weighted mean frequency) within 5 % in each of
// three windows (0-30 ms: the sweep and the click; 30-150 ms; 150 ms to the end). CLOSE: 1 dB,
// 2 dB, 2 dB, 10 % (the wave-1 rule's numbers, on finer frames and three windows). LOST: else.
//
// NEPTUNE_KICK_PRINT=1 prints the table; NEPTUNE_KICK_ATLAS=<folder> writes every pair as WAV
// (.ralph/atlas/KICKS/); NEPTUNE_KICK_BENCH=1 (Release binary) times old and new.
// -----------------------------------------------------------------------------
#include <juce_core/juce_core.h>
#include "../src/engine/DrumEngine.h"
#include "../src/engine/SampleBank.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/PresetManager.h"
#include "old/OldDrumEngine.h"
#include "fast_measure.h"
#include <complex>

class KickPointsTests : public juce::UnitTest
{
public:
    KickPointsTests() : UnitTest("Kick Points") {}

    using D = DrumEngine;
    using V = DrumVoice;
    using O = OldDrumEngine;
    using OV = OldDrumVoice;
    using T = DrumSynthType;
    static constexpr double SR = 48000.0;

    struct Hit
    {
        T type;
        float tune, decay, tone, drive, snap, level, velocity;
    };
    static double secondsFor(const Hit& h) { return std::min(3.0, 0.25 + 1.6 * (double) h.decay); }

    // ---- the voice alone -------------------------------------------------------------------------
    static std::vector<float> renderOld(const Hit& h)
    {
        OV v;
        v.setSampleRate(SR);
        v.setType(h.type);
        v.setTune(h.tune); v.setDecay(h.decay); v.setTone(h.tone); v.setDrive(h.drive); v.setSnap(h.snap); v.setLevel(h.level);
        v.noteOn(h.velocity);
        std::vector<float> out((size_t) (secondsFor(h) * SR));
        for (auto& s : out)
            s = v.process();
        return out;
    }
    static std::vector<float> renderNew(const Hit& h)
    {
        V v;
        v.setSampleRate(SR);
        v.setType(h.type);
        v.applyKickPoint(V::kickPoint(h.type, h.tune, h.decay, h.tone, h.drive, h.snap, h.level));
        v.noteOn(h.velocity);
        std::vector<float> out((size_t) (secondsFor(h) * SR));
        for (auto& s : out)
            s = v.process();
        return out;
    }

    // ---- an engine, one pad, one hit -------------------------------------------------------------
    using Setup = std::function<void(EngineBase&)>;
    static std::vector<float> renderPad(EngineBase& e, int pad, float velocity, double seconds)
    {
        e.prepareToPlay(SR, 256);
        e.setTempo(120.0f);
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        std::vector<float> out;
        const long total = (long) (seconds * SR);
        for (long pos = 0; pos < total; pos += 256)
        {
            if (pos == 0)
                e.noteOn(pad, velocity, -1);
            if (pos == 256 * 20)
                e.noteOff(pad, -1);
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < 256; ++i)
                out.push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
        }
        return out;
    }

    // ---- measuring --------------------------------------------------------------------------------
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

    // The power-weighted mean frequency of x[from, to): a Hann window, zero-padded.
    static double centroid(const std::vector<float>& x, size_t from, size_t to)
    {
        to = std::min(to, x.size());
        if (to <= from + 16) return 0.0;
        const size_t len = to - from;
        size_t n = 16384;
        while (n < 2 * len) n *= 2;
        std::vector<std::complex<double>> a(n);
        for (size_t i = 0; i < len; ++i)
            a[i] = (double) x[from + i] * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * (double) i / (double) (len - 1)));
        fastmeasure::fft(a);
        double num = 0.0, den = 0.0;
        for (size_t k = 1; k < n / 2; ++k)
        {
            const double p = std::norm(a[k]);
            num += p * (double) k * SR / (double) n;
            den += p;
        }
        return den > 0.0 ? num / den : 0.0;
    }

    struct Compared
    {
        double level = 0.0, curve = 0.0, onset = 0.0, residual = 0.0;
        double bright[3] = { 0.0, 0.0, 0.0 };
        double worstBright() const { return std::max({ std::abs(bright[0]), std::abs(bright[1]), std::abs(bright[2]) }); }
    };
    static Compared compare(const std::vector<float>& a, const std::vector<float>& b)
    {
        Compared c;
        const size_t n = std::min(a.size(), b.size());
        c.level = db(rms(b, 0, n) / std::max(rms(a, 0, n), 1.0e-12));
        const size_t onset = (size_t) (0.010 * SR);
        c.onset = std::abs(db(rms(b, 0, onset) / std::max(rms(a, 0, onset), 1.0e-12)));
        const size_t frame = (size_t) (0.05 * SR);
        double peak = 0.0;
        for (size_t f = 0; f + frame <= n; f += frame)
            peak = std::max({ peak, rms(a, f, f + frame), rms(b, f, f + frame) });
        for (size_t f = 0; f + frame <= n; f += frame)
        {
            const double x = rms(a, f, f + frame), y = rms(b, f, f + frame);
            if (std::max(x, y) > peak * 0.00316)
                c.curve = std::max(c.curve, std::abs(db(y / std::max(x, 1.0e-12))));
        }
        double diff = 0.0, ref = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            diff += (double) (b[i] - a[i]) * (double) (b[i] - a[i]);
            ref += (double) a[i] * (double) a[i];
        }
        c.residual = ref > 0.0 ? std::max(-200.0, 10.0 * std::log10(std::max(diff / ref, 1.0e-20))) : 0.0;
        const size_t edges[4] = { 0, (size_t) (0.03 * SR), (size_t) (0.15 * SR), n };
        for (int w = 0; w < 3; ++w)
        {
            const double x = centroid(a, edges[w], edges[w + 1]), y = centroid(b, edges[w], edges[w + 1]);
            c.bright[w] = x > 0.0 ? y / x - 1.0 : 0.0;
        }
        return c;
    }
    static const char* verdict(const Compared& c)
    {
        if (std::abs(c.level) <= 0.5 && c.curve <= 1.0 && c.onset <= 1.0 && c.worstBright() <= 0.05) return "MATCH";
        if (std::abs(c.level) <= 1.0 && c.curve <= 2.0 && c.onset <= 2.0 && c.worstBright() <= 0.10) return "CLOSE";
        return "LOST";
    }

    static bool printing() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_KICK_PRINT", "").isNotEmpty(); }
    static juce::String atlas() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_KICK_ATLAS", ""); }
    static void writeWav(const juce::File& file, const std::vector<float>& s)
    {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (! out.openedOk()) return;
        const int frames = (int) s.size(), bytes = frames * 2;
        out.write("RIFF", 4); out.writeInt(36 + bytes); out.write("WAVEfmt ", 8);
        out.writeInt(16); out.writeShort(1); out.writeShort(1); out.writeInt((int) SR); out.writeInt((int) SR * 2);
        out.writeShort(2); out.writeShort(16); out.write("data", 4); out.writeInt(bytes);
        for (float v : s)
            out.writeShort((short) std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    // Per type: rows, and how many of each verdict.
    struct Tally { int rows = 0, match = 0, close = 0, lost = 0; };
    std::map<juce::String, Tally> tally;

    Compared judge(const juce::String& group, const juce::String& name, const std::vector<float>& a, const std::vector<float>& b)
    {
        const Compared c = compare(a, b);
        const juce::String v = verdict(c);
        auto& t = tally[group];
        ++t.rows;
        (v == "MATCH" ? t.match : (v == "CLOSE" ? t.close : t.lost))++;
        if (printing())
            logMessage(juce::String::formatted("KICKS | %-5s | %-44s | %-5s | level %+5.2f dB | frames %4.2f dB | onset %4.2f dB | bright %+5.1f %+5.1f %+5.1f %% | residual %6.1f dB",
                                               group.toRawUTF8(), name.toRawUTF8(), v.toRawUTF8(), c.level, c.curve, c.onset,
                                               100.0 * c.bright[0], 100.0 * c.bright[1], 100.0 * c.bright[2], c.residual));
        if (atlas().isNotEmpty())
        {
            const juce::File folder(atlas());
            folder.createDirectory();
            const juce::String file = (group + "_" + name).replaceCharacters(" >/*,()+=.", "__________").replace("__", "_");
            writeWav(folder.getChildFile(file + "_old.wav"), a);
            writeWav(folder.getChildFile(file + "_new.wav"), b);
        }
        expect(rms(a, 0, a.size()) > 1.0e-5, name + ": the old kick sounds");
        for (float s : b)
            if (! std::isfinite(s) || std::abs(s) > 8.0f)
            {
                expect(false, name + ": finite and bounded");
                break;
            }
        return c;
    }

    static const char* typeName(T t) { return t == T::FMKick ? "FMKK" : (t == T::AnalogKick ? "KICK" : "MKIK"); }

    // ---- the tests ---------------------------------------------------------------------------------
    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_KICK_BENCH", "").isNotEmpty())
        {
            bench();
            return;
        }
        testTypes();
        testKits();
        testSavedBefore();
        testSampleBank();
        testPage();
        testCells();
        testUntouched();
        juce::String sum;
        for (auto& [g, t] : tally)
            sum << g << ": " << t.rows << " rows, " << t.match << " MATCH, " << t.close << " CLOSE, " << t.lost << " LOST; ";
        logMessage("KICKS " + sum);
    }

    // The three types, one cell at a time around the middle, and some settings far from it.
    void testTypes()
    {
        beginTest("Old against new: FMKK, KICK and MKIK on the voice alone, TUNE / DEC / TONE / DRIV / SNAP / velocity");
        for (T type : { T::FMKick, T::AnalogKick, T::ModCyclesKick })
        {
            const juce::String g = typeName(type);
            const Hit mid { type, 50.0f, 0.5f, 0.5f, 0.0f, 0.5f, 0.9f, 1.0f };
            std::vector<std::pair<juce::String, Hit>> hits;
            hits.push_back({ "middle", mid });
            for (float v : { 35.0f, 70.0f, 110.0f, 160.0f }) { Hit h = mid; h.tune = v; hits.push_back({ "TUNE " + juce::String(v, 0), h }); }
            for (float v : { 0.08f, 0.25f, 1.0f, 2.0f })     { Hit h = mid; h.decay = v; hits.push_back({ "DEC " + juce::String(v, 2), h }); }
            for (float v : { 0.0f, 0.25f, 0.75f, 1.0f })     { Hit h = mid; h.tone = v; hits.push_back({ "TONE " + juce::String(v, 2), h }); }
            for (float v : { 0.2f, 0.5f, 1.0f })             { Hit h = mid; h.drive = v; hits.push_back({ "DRIV " + juce::String(v, 2), h }); }
            for (float v : { 0.0f, 0.25f, 0.7f, 1.0f })      { Hit h = mid; h.snap = v; hits.push_back({ "SNAP " + juce::String(v, 2), h }); }
            for (float v : { 0.2f, 0.6f })                   { Hit h = mid; h.velocity = v; hits.push_back({ "vel " + juce::String(v, 1), h }); }
            hits.push_back({ "far: TUNE 40 DEC 1.5 TONE 0.9 DRIV 0.6 SNAP 0.85", { type, 40.0f, 1.5f, 0.9f, 0.6f, 0.85f, 0.9f, 0.8f } });
            hits.push_back({ "far: TUNE 90 DEC 0.12 TONE 0.2 DRIV 0.3 SNAP 0.3 vel 0.4", { type, 90.0f, 0.12f, 0.2f, 0.3f, 0.3f, 0.9f, 0.4f } });
            for (auto& [name, h] : hits)
                judge(g, name, renderOld(h), renderNew(h));
        }
    }

    // Every factory kit's kick pads, as written, against the kits as they are; the default kit's two.
    struct KitPad { const char* kit; int pad; T type; float tune, decay, tone, drive, snap, level, pan; };
    static const std::vector<KitPad>& oldKitPads()
    {
        // (src/project/FactoryPresets.h and DrumEngine::loadDefaultKit at 3f77ffb.)
        static const std::vector<KitPad> pads = {
            { "808 Kit 2", 8, T::AnalogKick, 52.0f, 0.6f, 0.5f, 0.0f, 0.5f, 0.9f, 0.5f },
            { "808 Kit 2", 9, T::AnalogKick, 52.0f, 0.8f, 0.5f, 0.0f, 0.5f, 0.85f, 0.5f },
            { "FM Kit", 8, T::FMKick, 48.0f, 0.8f, 0.5f, 0.3f, 0.7f, 0.9f, 0.5f },
            { "FM Kit", 9, T::FMKick, 48.0f, 0.6f, 0.5f, 0.2f, 0.5f, 0.85f, 0.5f },
            { "Electro Kit", 8, T::ModCyclesKick, 55.0f, 0.7f, 0.6f, 0.4f, 0.6f, 0.9f, 0.5f },
            { "Electro Kit", 9, T::ModCyclesKick, 55.0f, 0.5f, 0.4f, 0.3f, 0.5f, 0.85f, 0.5f },
            { "Industrial Kit", 8, T::FMKick, 42.0f, 0.9f, 0.5f, 0.6f, 0.8f, 0.9f, 0.5f },
            { "Industrial Kit", 9, T::FMKick, 42.0f, 0.7f, 0.5f, 0.5f, 0.7f, 0.85f, 0.5f },
            { "Lo-Fi Kit", 8, T::AnalogKick, 50.0f, 0.5f, 0.4f, 0.6f, 0.7f, 0.9f, 0.5f },
            { "Lo-Fi Kit", 9, T::AnalogKick, 45.0f, 0.4f, 0.3f, 0.5f, 0.6f, 0.85f, 0.5f },
            { "default kit", 9, T::AnalogKick, 52.0f, 0.6f, 0.5f, 0.0f, 0.5f, 0.9f, 0.5f },
            { "default kit", 10, T::ModCyclesKick, 55.0f, 0.7f, 0.6f, 0.4f, 0.6f, 0.9f, 0.5f } };
        return pads;
    }
    static void oldPad(EngineBase& e, const KitPad& k)
    {
        const int b = k.pad * D::PARAMS_PER_VOICE;
        e.setParameter(b + D::TYPE, (float) (int) k.type);
        e.setParameter(b + D::TUNE, k.tune); e.setParameter(b + D::DECAY, k.decay); e.setParameter(b + D::TONE, k.tone);
        e.setParameter(b + D::DRIVE, k.drive); e.setParameter(b + D::SNAP, k.snap); e.setParameter(b + D::LEVEL, k.level);
        e.setParameter(b + D::PAN, k.pan);
    }

    void testKits()
    {
        beginTest("Old against new: every factory kit's kicks as written, and the default kit's two, through the engine");
        std::map<juce::String, Preset> kits;
        for (auto& p : FactoryPresets::getAll())
            if (p.engineType == EngineType::Drum)
                kits[p.name] = p;
        for (const auto& k : oldKitPads())
        {
            const juce::String name = juce::String(k.kit) + " pad " + juce::String(k.pad + 1);
            O older;
            oldPad(older, k);
            D newer;
            if (juce::String(k.kit) != "default kit")
            {
                expect(kits.count(k.kit) == 1, juce::String(k.kit) + " is a factory kit");
                for (auto& [pid, value] : PresetManager::soundOf(kits[k.kit]))
                    newer.setParameter(pid, value);
            }
            // TYPE keeps the old kick's number; the pad is the kick machine.
            expectEquals((int) newer.getParameter(k.pad * D::PARAMS_PER_VOICE + D::TYPE), (int) k.type, name + ": TYPE");
            expect(newer.isKickMachine(k.pad), name + ": a kick machine");
            const double seconds = std::min(3.0, 0.3 + 1.6 * (double) k.decay);
            for (float vel : { 1.0f, 0.5f })
                judge("KITS", name + " vel " + juce::String(vel, 1), renderPad(older, k.pad, vel, seconds), renderPad(newer, k.pad, vel, seconds));
        }
        // The default kit's DrumKit (what getCurrentKit gives) loaded again is the same kit.
        {
            D a, b;
            b.loadKit(a.getCurrentKit());
            for (int pad : { 8, 9, 10 })
            {
                const auto x = renderPad(a, pad, 0.9f, 1.2), y = renderPad(b, pad, 0.9f, 1.2);
                expect(x == y, "the default kit's DrumKit loaded again: pad " + juce::String(pad + 1) + " the same samples");
            }
        }
    }

    // A pad of an old kick TYPE saved before KCK2: the KICK page as the kick machine's own, KCK2 at
    // 0, in the order a project sets them (ids up).
    void testSavedBefore()
    {
        beginTest("A kick saved before the kick machine had KCK2 is the kick it was");
        for (const auto& k : oldKitPads())
        {
            ParamMap saved;
            const int b = k.pad * D::PARAMS_PER_VOICE;
            saved[b + D::TUNE] = k.tune; saved[b + D::DECAY] = k.decay; saved[b + D::TONE] = k.tone; saved[b + D::DRIVE] = k.drive;
            saved[b + D::SNAP] = k.snap; saved[b + D::LEVEL] = k.level; saved[b + D::PAN] = k.pan; saved[b + D::TYPE] = (float) (int) k.type;
            for (int n = 0; n < V::NumKickParams; ++n)
                saved[D::KICK_BASE + k.pad * V::NumKickParams + n] = V::kickDefault(n);
            for (int n = 0; n < D::SOUND_CELLS; ++n)
                saved[D::SOUND_BASE + k.pad * D::SOUND_CELLS + n] = 0.0f;
            const juce::String name = juce::String(k.kit) + " pad " + juce::String(k.pad + 1);
            O older;
            oldPad(older, k);
            // On a fresh kit (whose pad may already be that TYPE: the default kit's 10 and 11) ...
            D fresh;
            for (auto& [pid, value] : saved)
                fresh.setParameter(pid, value);
            // ... and on a kit whose pad was something else.
            D other;
            other.setParameter(b + D::TYPE, (float) (int) T::Clap);
            for (auto& [pid, value] : saved)
                other.setParameter(pid, value);
            const double seconds = std::min(3.0, 0.3 + 1.6 * (double) k.decay);
            const auto old = renderPad(older, k.pad, 1.0f, seconds);
            const auto x = renderPad(fresh, k.pad, 1.0f, seconds), y = renderPad(other, k.pad, 1.0f, seconds);
            expect(x == y, name + ": the same whatever the pad was");
            const Compared c = judge("SAVED", name, old, x);
            expect(juce::String(verdict(c)) != "LOST", name + ": " + verdict(c));
            // What it saves now is not taken for an old kick again.
            ParamMap now;
            for (auto& page : fresh.getParamPages())
                for (auto& p : page.params)
                    if (p.isValid())
                        now[p.id] = fresh.getParameter(p.id);
            D again;
            for (auto& [pid, value] : now)
                again.setParameter(pid, value);
            expect(renderPad(again, k.pad, 1.0f, seconds) == x, name + ": saved again and loaded, the same samples");
        }
    }

    // The sample bank's hits that were rendered with the old kicks.
    void testSampleBank()
    {
        beginTest("Old against new: the sample bank's kick hits (KICK, the breaks' kicks)");
        // (SampleBank::buildFactory, renderBreak: their own arguments.)
        struct BankHit { const char* name; T type; float tune, decay, tone, drive, snap, seconds, velocity; };
        const BankHit hits[] = {
            { "KICK", T::FMKick, 48.0f, 0.8f, 0.5f, 0.3f, 0.7f, 0.9f, 1.0f },
            { "break kick vel 1", T::AnalogKick, 52.0f, 0.35f, 0.45f, 0.35f, 0.6f, 0.5f, 1.0f },
            { "break kick vel 0.85", T::AnalogKick, 52.0f, 0.35f, 0.45f, 0.35f, 0.6f, 0.5f, 0.85f },
            { "break kick vel 0.6", T::AnalogKick, 52.0f, 0.35f, 0.45f, 0.35f, 0.6f, 0.5f, 0.6f } };
        for (const auto& h : hits)
        {
            const auto a = SampleBank::renderHit<OV>(h.type, h.tune, h.decay, h.tone, h.drive, h.snap, h.seconds, h.velocity);
            const auto b = SampleBank::renderHit<V>(h.type, h.tune, h.decay, h.tone, h.drive, h.snap, h.seconds, h.velocity);
            // (rendered at the bank's 44.1 kHz; measured at 48 as everything here: the ratios hold)
            judge("BANK", h.name, a, b);
        }
    }

    void testPage()
    {
        beginTest("KCK2: a kick pad's second page (its sound page ids), set and read back; TYPE turned to a point");
        D e;
        const int pad = 8;
        expect(e.isKickMachine(pad) && e.hasSoundPage(pad) && e.soundPage(pad).name == "KCK2");
        const char* labels[8] = { "KIND", "LIN", "RES", "LPSW", "LPKT", "SAT", "EDGE", "PATH" };
        for (int n = 0; n < 8; ++n)
        {
            const auto spec = e.soundPage(pad).params[(size_t) n];
            expectEquals(juce::String(spec.label), juce::String(labels[n]));
            expectEquals(spec.id, D::SOUND_BASE + pad * D::SOUND_CELLS + n);
            expectEquals(spec.defaultVal, 0.0f, juce::String(labels[n]) + ": 0 is the kick machine as it was");
            const float v = spec.isDiscrete() ? 1.0f : spec.minVal + 0.37f * (spec.maxVal - spec.minVal);
            e.setParameter(spec.id, v);
            expectWithinAbsoluteError(e.getParameter(spec.id), v, 1.0e-6f);
            expectWithinAbsoluteError(e.getVoice(pad).getKick2Param(n), v, 1.0e-6f);
        }
        // The page is in getParamPages for every kick pad: KSYN and the three points.
        for (T t : { T::FMKick, T::AnalogKick, T::ModCyclesKick, T::KickSynth })
        {
            e.setParameter(3 * D::PARAMS_PER_VOICE + D::TYPE, (float) (int) t);
            expect(e.isKickMachine(3) && e.soundPage(3).name == "KCK2", juce::String(typeName(t)) + ": KCK2");
        }
        // TYPE turned to FMKK: the KICK and KCK2 pages start at the FM kick; DR cells stay.
        {
            D k;
            const int b = 12 * D::PARAMS_PER_VOICE;
            k.setParameter(b + D::TONE, 0.33f);
            k.setParameter(b + D::TYPE, (float) (int) T::FMKick);
            const auto p = V::kickPointOf(T::FMKick);
            for (int n = 0; n < V::NumKickParams; ++n)
                expectWithinAbsoluteError(k.getParameter(D::KICK_BASE + 12 * V::NumKickParams + n), p.kick[(size_t) n], 1.0e-6f);
            for (int n = 0; n < V::NumKick2Params; ++n)
                expectWithinAbsoluteError(k.getParameter(D::SOUND_BASE + 12 * D::SOUND_CELLS + n), p.kick2[(size_t) n], 1.0e-6f);
            expectWithinAbsoluteError(k.getParameter(b + D::TONE), 0.33f, 1.0e-6f, "TONE stays as it was");
            // ... to KSYN from a point: the kick machine's own cells; from CLAP its cells stay.
            k.setParameter(b + D::TYPE, (float) (int) T::KickSynth);
            for (int n = 0; n < V::NumKickParams; ++n)
                expectWithinAbsoluteError(k.getParameter(D::KICK_BASE + 12 * V::NumKickParams + n), V::kickDefault(n), 1.0e-6f);
            k.setParameter(D::KICK_BASE + 12 * V::NumKickParams + V::KickSweep, 50.0f);
            k.setParameter(b + D::TYPE, (float) (int) T::Clap);
            k.setParameter(b + D::TYPE, (float) (int) T::KickSynth);
            expectWithinAbsoluteError(k.getParameter(D::KICK_BASE + 12 * V::NumKickParams + V::KickSweep), 50.0f, 1.0e-6f,
                                      "a KSYN pad turned away and back keeps its KICK page");
        }
    }

    // A pad of each kick as the kits have it, played with one KCK2 cell moved: each is heard.
    void testCells()
    {
        beginTest("Each KCK2 cell is heard on the kick machine; one set under a ringing kick leaves that hit alone");
        auto play = [](int cell, float value, double changeAt = -1.0)
        {
            D e;
            const int pad = 8;      // the kick machine of the default kit
            if (cell >= 0 && changeAt < 0.0)
                e.setParameter(D::SOUND_BASE + pad * D::SOUND_CELLS + cell, value);
            e.prepareToPlay(SR, 256);
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            std::vector<float> out;
            for (long pos = 0; pos < (long) (0.8 * SR); pos += 256)
            {
                if (pos == 0) e.noteOn(pad, 0.9f, -1);
                if (cell >= 0 && changeAt >= 0.0 && pos == (long) (changeAt * SR) / 256 * 256)
                    e.setParameter(D::SOUND_BASE + pad * D::SOUND_CELLS + cell, value);
                buf.clear();
                e.processBlock(buf, midi);
                for (int i = 0; i < 256; ++i)
                    out.push_back(buf.getSample(0, i));
            }
            return out;
        };
        const auto plain = play(-1, 0.0f);
        const float values[8] = { 1.0f, 1.0f, 1.0f, 9.0f, 1.0f, 0.8f, 1.0f, 1.0f };
        const char* labels[8] = { "KIND", "LIN", "RES", "LPSW", "LPKT", "SAT", "EDGE", "PATH" };
        for (int n = 0; n < 8; ++n)
        {
            const auto moved = play(n, values[n]);
            double diff = 0.0, ref = 0.0;
            for (size_t i = 0; i < plain.size(); ++i)
            {
                diff += (double) (moved[i] - plain[i]) * (double) (moved[i] - plain[i]);
                ref += (double) plain[i] * (double) plain[i];
            }
            const double residual = 10.0 * std::log10(std::max(diff / ref, 1.0e-20));
            if (printing())
                logMessage(juce::String::formatted("KICKS | cell %s at %.2f: residual %.1f dB", labels[n], values[n], residual));
            expect(residual > -40.0, juce::String(labels[n]) + " is heard (residual " + juce::String(residual, 1) + " dB)");
            // Set while the hit rings: read at the strike, so nothing of that hit moves (no jump).
            expect(play(n, values[n], 0.1) == plain, juce::String(labels[n]) + " set under a ringing kick: that hit unchanged");
        }
    }

    // The kick machine with KCK2 at 0 against the old voice's: the same samples.
    void testUntouched()
    {
        beginTest("KSYN with KCK2 at 0: the same samples as the kick machine before");
        for (float snap : { 0.2f, 0.7f })
            for (float drive : { 0.0f, 0.5f })
            {
                OV a;
                V b;
                a.setSampleRate(SR); b.setSampleRate(SR);
                a.setType(T::KickSynth); b.setType(T::KickSynth);
                a.setTune(48.0f); b.setTune(48.0f);
                a.setDecay(0.5f); b.setDecay(0.5f);
                a.setTone(0.7f); b.setTone(0.7f);
                a.setDrive(drive); b.setDrive(drive);
                a.setSnap(snap); b.setSnap(snap);
                a.setLevel(0.9f); b.setLevel(0.9f);
                a.setKickParam(V::KickKnock, 0.6f); b.setKickParam(V::KickKnock, 0.6f);
                a.setKickParam(V::KickHold, 0.02f); b.setKickParam(V::KickHold, 0.02f);
                a.noteOn(0.8f); b.noteOn(0.8f);
                bool same = true;
                for (int i = 0; i < (int) (1.0 * SR); ++i)
                    same = same && a.process() == b.process();
                expect(same, "KSYN SNAP " + juce::String(snap, 1) + " DRIV " + juce::String(drive, 1) + ": the same samples");
            }
    }

    // Release binary: old voice's three kicks against the kick machine at them, alternated.
    void bench()
    {
        beginTest("Cost: a kick, old against new, alternated, best of five (Release binary)");
        for (T type : { T::FMKick, T::AnalogKick, T::ModCyclesKick, T::KickSynth })
        {
            const Hit h { type, 50.0f, 0.5f, 0.5f, 0.3f, 0.6f, 0.9f, 1.0f };
            double bestOld = 1.0e9, bestNew = 1.0e9;
            float sink = 0.0f;
            for (int round = 0; round < 5; ++round)
            {
                for (int which = 0; which < 2; ++which)
                {
                    const auto t0 = juce::Time::getMillisecondCounterHiRes();
                    for (int k = 0; k < 40; ++k)
                    {
                        const auto r = which == 0 ? renderOld(h) : renderNew(h);
                        sink += r[r.size() / 3];
                    }
                    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                    (which == 0 ? bestOld : bestNew) = std::min(which == 0 ? bestOld : bestNew, ms);
                }
            }
            logMessage(juce::String::formatted("KICKS cost %s: old %.1f ms, new %.1f ms (40 hits of %.2f s), new / old %.2f%s",
                                               type == T::KickSynth ? "KSYN" : typeName(type), bestOld, bestNew, secondsFor(h), bestNew / bestOld,
                                               sink == 12345.0f ? "!" : ""));
        }
    }
};

static KickPointsTests kickPointsTests;
