#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/AftertouchRouter.h"
#include "../src/project/FactoryPresets.h"
#include "old/OldWavetableEngine.h"
#include "fast_measure.h"

// -----------------------------------------------------------------------------
// The wavetable engine's XIN page as it was before phase U4 (cells 65-72: XIN OFF / PM / POS,
// XDPT, XENV, XVEL, XWHL, XRCT, XLP, XHP), set on the engine as it is now: router slots.
// This is the mapping the engine's header describes, written out; a loader for sounds stored
// with the old cells (format 2) would do exactly this.
//
//   the route      free slot 7: SRC XIN (XFLT, the engine's own two-pole filters, when XLP / XHP
//                  are not open), DEST PM or POS, AMT = XDPT x (1 - XENV) x (1 - XVEL)
//   XENV, XVEL     slots 8-10 on AMT7: ENV at XDPT x XENV x (1 - XVEL), VEL at XDPT x (1 - XENV) x
//                  XVEL, ENV via VEL at XDPT x XENV x XVEL (the old product, term by term)
//   XWHL           slot 11: STRP on AMT7 at XWHL
//   XRCT           slots 12-16, the same five with the source XRCT; the two routes share the depth
//                  (1 - XRCT) : XRCT, by its square root on PM, whose amount counts squared
//   XLP, XHP       the XIN page's LPF and HPF
// What it cannot give: XWHL scaled by XENV / XVEL (the strip's share is added as it is).
// -----------------------------------------------------------------------------
struct WavetableXinCells
{
    enum Id { Mode = 65, Depth, Env, Vel, Strip, Rectify, LowPass, HighPass };
    enum { Off = 0, Phase, Position };
    static constexpr int FIRST_SLOT = 6;        // free slots 7 .. 16
    static constexpr float LP_OPEN = 20000.0f, HP_OPEN = 20.0f;

    static bool owns(int id) { return id >= Mode && id <= HighPass; }

    int mode = Off;
    float depth = 0.5f, env = 0.0f, vel = 0.0f, strip = 0.0f, rect = 0.0f, lowPass = LP_OPEN, highPass = HP_OPEN;

    void set(EngineBase& e, int id, float value)
    {
        switch (id)
        {
            case Mode:      mode = juce::jlimit(0, 2, (int) std::lround(value)); break;
            case Depth:     depth = juce::jlimit(0.0f, 1.0f, value); break;
            case Env:       env = juce::jlimit(0.0f, 1.0f, value); break;
            case Vel:       vel = juce::jlimit(0.0f, 1.0f, value); break;
            case Strip:     strip = juce::jlimit(0.0f, 1.0f, value); break;
            case Rectify:   rect = juce::jlimit(0.0f, 1.0f, value); break;
            case LowPass:   lowPass = juce::jlimit(20.0f, LP_OPEN, value); break;
            case HighPass:  highPass = juce::jlimit(HP_OPEN, 5000.0f, value); break;
            default:        return;
        }
        apply(e);
    }

    void apply(EngineBase& e) const
    {
        using R = mod::Router;
        const auto& table = WavetableEngine::lfoDests();
        const auto& sources = WavetableEngine::sources();
        const auto place = [&sources](int source) { return (float) sources.indexOf(source); };
        const auto slot = [&e](int s, float src, float dest, float amount, float via)
        {
            e.setParameter(ModSlotParam::id(s, ModSlotParam::SRC), src);
            e.setParameter(ModSlotParam::id(s, ModSlotParam::DEST), dest);
            e.setParameter(ModSlotParam::id(s, ModSlotParam::AMT), amount);
            e.setParameter(ModSlotParam::id(s, ModSlotParam::VIA), via);
        };
        const int dest = mode == Phase ? WavetableEngine::dests().phase : (mode == Position ? (int) WavetableVoice::LfoPosition : 0);
        const bool low = lowPass < LP_OPEN, high = highPass > HP_OPEN;
        const int input = low || high ? (int) mod::Src::Published1 + (int) WavetableVoice::PubFiltered : (int) mod::Src::Xin;
        e.setParameter(R::xinId(R::XinLowPass), lowPass);
        e.setParameter(R::xinId(R::XinHighPass), highPass);
        for (int k = 0; k < 2; ++k)
        {
            const int s = FIRST_SLOT + k * 5;
            float share = k == 0 ? 1.0f - rect : rect;
            if (mode == Phase)
                share = std::sqrt(share);
            if (dest == 0 || share <= 0.0f)
            {
                for (int i = 0; i < 5; ++i)
                    slot(s + i, 0.0f, 0.0f, 0.0f, 0.0f);
                continue;
            }
            const float d = depth * share;
            const float amountCell = (float) (table.visible() + 2 * table.numLfos() + s);     // AMTn in a free slot's DEST
            slot(s, place(k == 0 ? input : (int) mod::Src::Published1 + (int) WavetableVoice::PubRect), (float) dest,
                 d * (1.0f - env) * (1.0f - vel), 0.0f);
            slot(s + 1, place(mod::Src::Env), amountCell, d * env * (1.0f - vel), 0.0f);
            slot(s + 2, place(mod::Src::Velocity), amountCell, d * (1.0f - env) * vel, 0.0f);
            slot(s + 3, place(mod::Src::Env), amountCell, d * env * vel, place(mod::Src::Velocity));
            slot(s + 4, place(mod::Src::Strip), amountCell, strip * share, 0.0f);
        }
    }
};

// -----------------------------------------------------------------------------
// Phase U4-WT: the wavetable engine on the modulation router (mod::Router) and on dsp/PhaseOsc.h.
//
// OLD AGAINST NEW. The engine as it was (tests/old/OldWavetableEngine.h, with its own LFO section,
// mod slots, XIN cells, vibrato oscillator, pressure block and RAND generator) and the engine as
// it is play the same gesture with the same cells, for every wavetable factory preset and for each
// feature that was folded into the router. The verdict is fixed here, before any result was seen:
//
//   MATCH   every sample of both channels equal
//   CLOSE   level within 1 dB, the 50 ms envelope within 3 dB wherever the old sound is above
//           -30 dB of its loudest frame, the energy-weighted mean frequency within 10 %, and the
//           spectral frames (fastmeasure::difference, 0 = the same, 1 = nothing in common) within
//           0.15: each channel by itself, the worse one counts
//   LOST    anything else
//
// Two families cannot be judged that way and say so in their name: what is drawn at random (the
// RAND scan's places, the source RAND) comes from another generator now, so the SEQUENCE of values
// differs by design; they are judged by what the randomness is for (STAT rows).
//
// Every row is logged ("[U4-WT] ..."), and with NEPTUNE_ATLAS_DIR set every row that is not a
// MATCH (and whose difference is not more than 90 dB under the sound) is written there as
// <name>_old.wav / <name>_new.wav.
//
// NEPTUNE_U4_WT_BENCH=1 (Release test binary): what eight voices cost, old against new
// (NEPTUNE_U4_WT_BENCH_ONLY / _RUNS: one case, more turns).
// -----------------------------------------------------------------------------
class U4WavetableTests : public juce::UnitTest
{
public:
    U4WavetableTests() : UnitTest("U4 Wavetable") {}

    using W = WavetableEngine;
    using O = oldwt::WavetableEngine;
    using X = WavetableXinCells;
    using R = mod::Router;
    using VF = VoiceFilterSection;
    static constexpr double SR = 48000.0;

    struct Stereo { std::vector<float> l, r; };
    using Patch = std::vector<std::pair<int, float>>;
    using Setup = std::function<void(EngineBase&)>;

    struct Gesture
    {
        double seconds = 1.0;
        int block = 64;
        struct Note { int note; float velocity; double on, off; };
        std::vector<Note> notes { { 57, 0.9f, 0.0, 0.7 }, { 64, 0.6f, 0.0, 0.7 } };
        std::function<float(double)> strip, pressure, bend;    // over time (seconds); none = untouched
        std::function<float(long long)> input;                  // per sample; none = nothing connected
        const AftertouchRouter* routes = nullptr;               // none = every route off
    };

    template <typename Engine>
    static Stereo render(Engine& e, const Gesture& g)
    {
        AftertouchRouter off;
        off.clear();
        e.setAftertouchRouter(g.routes != nullptr ? g.routes : &off);
        e.prepareToPlay(SR, g.block);
        const int total = (int) (g.seconds * SR);
        Stereo out;
        out.l.reserve((size_t) total + (size_t) g.block);
        out.r.reserve((size_t) total + (size_t) g.block);
        juce::AudioBuffer<float> buf(2, g.block);
        juce::MidiBuffer midi;
        std::vector<float> in((size_t) g.block, 0.0f);
        long long clock = 0;
        for (int b = 0; b * g.block < total; ++b)
        {
            const double t0 = (double) b * g.block / SR, t1 = (double) (b + 1) * g.block / SR;
            for (auto& n : g.notes)
            {
                if (n.off >= t0 && n.off < t1) e.noteOff(n.note, -1);
                if (n.on >= t0 && n.on < t1)   e.noteOn(n.note, n.velocity, -1);
            }
            if (g.strip)    e.setModWheel(g.strip(t0));
            if (g.bend)     e.pitchBend(g.bend(t0));
            if (g.pressure)
                for (auto& n : g.notes)
                    if (t0 >= n.on && t0 < n.off)
                        e.aftertouch(n.note, g.pressure(t0));
            if (g.input)
            {
                for (int i = 0; i < g.block; ++i)
                    in[(size_t) i] = g.input(clock + i);
                e.setAudioInput({ in.data(), in.data(), g.block, 0.5f });
            }
            else
                e.setAudioInput({});
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < g.block; ++i)
            {
                out.l.push_back(buf.getSample(0, i));
                out.r.push_back(buf.getSample(1, i));
            }
            clock += g.block;
        }
        e.setAftertouchRouter(nullptr);
        return out;
    }

    static double rmsOf(const std::vector<float>& x, size_t from = 0, size_t to = (size_t) -1)
    {
        to = std::min(to, x.size());
        if (to <= from)
            return 0.0;
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt(sum / (double) (to - from));
    }
    static double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

    struct Verdict
    {
        juce::String word = "MATCH";
        double null = -240.0, level = 0.0, envelope = 0.0, centroid = 1.0, spectral = 0.0;
        bool lost() const { return word == "LOST"; }
        bool match() const { return word == "MATCH"; }
        juce::String text() const
        {
            if (match())
                return "MATCH";
            return word + "  null " + juce::String(null, 1) + " dB, level " + juce::String(level, 2) + " dB, envelope "
                 + juce::String(envelope, 2) + " dB, mean frequency x" + juce::String(centroid, 3) + ", spectral " + juce::String(spectral, 3);
        }
    };

    static constexpr double CLOSE_LEVEL_DB = 1.0, CLOSE_ENVELOPE_DB = 3.0, CLOSE_CENTROID = 0.10, CLOSE_SPECTRAL = 0.15;
    static constexpr double ATLAS_NULL_DB = -90.0;

    static Verdict compare(const Stereo& a, const Stereo& b)
    {
        Verdict v;
        if (a.l == b.l && a.r == b.r)
            return v;
        v.null = -240.0;
        const std::vector<float>* olds[2] = { &a.l, &a.r };
        const std::vector<float>* news[2] = { &b.l, &b.r };
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto& o = *olds[ch];
            const auto& n = *news[ch];
            const double ro = rmsOf(o), rn = rmsOf(n);
            if (ro < 1.0e-6 && rn < 1.0e-6)
                continue;
            std::vector<float> d(o.size());
            for (size_t i = 0; i < o.size(); ++i)
                d[i] = n[i] - o[i];
            v.null = std::max(v.null, db(rmsOf(d) / std::max(ro, 1.0e-12)));
            const double level = db(rn / std::max(ro, 1.0e-12));
            if (std::abs(level) > std::abs(v.level))
                v.level = level;
            // The envelope: 50 ms frames, wherever the old sound is above -30 dB of its loudest.
            const size_t frame = (size_t) (0.05 * SR);
            double loudest = 0.0;
            for (size_t f = 0; f + frame <= o.size(); f += frame)
                loudest = std::max(loudest, rmsOf(o, f, f + frame));
            for (size_t f = 0; f + frame <= o.size(); f += frame)
            {
                const double eo = rmsOf(o, f, f + frame), en = rmsOf(n, f, f + frame);
                if (eo > loudest * 0.0316)
                    v.envelope = std::max(v.envelope, std::abs(db(en / eo)));
            }
            float r1 = 0.0f, c1 = 0.0f, r2 = 0.0f, c2 = 0.0f;
            fastmeasure::rmsAndCentroid(o, 4096, SR, r1, c1);
            fastmeasure::rmsAndCentroid(n, 4096, SR, r2, c2);
            const double ratio = c1 > 0.0f ? (double) c2 / (double) c1 : 1.0;
            if (std::abs(ratio - 1.0) > std::abs(v.centroid - 1.0))
                v.centroid = ratio;
            v.spectral = std::max(v.spectral, (double) fastmeasure::difference(o, n, 2048));
        }
        const bool close = std::abs(v.level) <= CLOSE_LEVEL_DB && v.envelope <= CLOSE_ENVELOPE_DB
                           && std::abs(v.centroid - 1.0) <= CLOSE_CENTROID && v.spectral <= CLOSE_SPECTRAL;
        v.word = close ? "CLOSE" : "LOST";
        return v;
    }

    static void writeWav(const juce::String& name, const Stereo& x)
    {
        const char* dir = std::getenv("NEPTUNE_ATLAS_DIR");
        if (dir == nullptr || x.l.empty())
            return;
        juce::File folder(dir);
        folder.createDirectory();
        juce::File file = folder.getChildFile(juce::File::createLegalFileName(name.replaceCharacter(' ', '_')) + ".wav");
        file.deleteFile();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), SR, 2, 24, {}, 0));
        if (writer == nullptr)
            return;
        stream.release();
        const float* channels[2] = { x.l.data(), x.r.data() };
        writer->writeFromFloatArrays(channels, 2, (int) x.l.size());
    }

    static Setup cells(Patch patch)
    {
        return [patch](EngineBase& e) { for (auto& c : patch) e.setParameter(c.first, c.second); };
    }
    static Patch operator_plus(Patch a, const Patch& b)
    {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    struct Row { juce::String name; Verdict verdict; };
    std::vector<Row> rows;

    // Old against new: the old engine set up by `oldSetup`, the new one by `newSetup`, one gesture.
    Verdict pair(const juce::String& name, const Setup& oldSetup, const Setup& newSetup, const Gesture& g, Stereo* newOut = nullptr)
    {
        auto o = std::make_unique<O>();
        auto n = std::make_unique<W>();
        oldSetup(*o);
        newSetup(*n);
        const Stereo a = render(*o, g), b = render(*n, g);
        expect(rmsOf(a.l) > 1.0e-4, name + ": the old engine sounds");
        Verdict v = compare(a, b);
        rows.push_back({ name, v });
        logMessage("[U4-WT] " + name + ": " + v.text());
        // (A pair to listen to for every row that is not a MATCH, except those whose difference
        // lies more than 90 dB under the sound: the last bits of a sine, nothing a file can show.)
        if (! v.match() && v.null > ATLAS_NULL_DB)
        {
            writeWav(name + "_old", a);
            writeWav(name + "_new", b);
        }
        if (newOut != nullptr)
            *newOut = b;
        return v;
    }
    Verdict pair(const juce::String& name, const Patch& patch, const Gesture& g) { return pair(name, cells(patch), cells(patch), g); }

    template <typename Engine = W>
    static Stereo play(const Setup& setup, const Gesture& g)
    {
        auto e = std::make_unique<Engine>();
        setup(*e);
        return render(*e, g);
    }

    // ---- cells by name ------------------------------------------------------------------------
    static int dest(const char* label)
    {
        const int d = W::lfoDests().indexOfLabel(label);
        jassert(d > 0);
        return d;
    }
    static Patch lfo(int which, float rate, float depth, int destination, int shape = 0, int trigger = 1)
    {
        return { { R::lfoId(which, R::Rate), rate }, { R::lfoId(which, R::Depth), depth }, { R::lfoId(which, R::Shape), (float) shape },
                 { R::lfoId(which, R::Trigger), (float) trigger }, { R::lfoId(which, R::DestCell), (float) destination } };
    }
    static Patch slot(int s, int src, int destination, float amount, int via = 0)
    {
        return { { ModSlotParam::id(s, ModSlotParam::SRC), (float) src }, { ModSlotParam::id(s, ModSlotParam::DEST), (float) destination },
                 { ModSlotParam::id(s, ModSlotParam::AMT), amount }, { ModSlotParam::id(s, ModSlotParam::VIA), (float) via } };
    }
    static int source(int src) { return W::sources().indexOf(src); }
    static int published(int k) { return W::sources().placeOfPublished(k); }
    static int dualId(VF::Offset o) { return W::FilterDualBase + (int) o - (int) VF::Count; }

    // A sound in which every destination is heard: the PWM table part of the way in, a slow
    // motion, a scan there and back, two copies, the sub, both filters side by side. (Without the
    // VIBR cell, which has a row of its own: its vibrato is set once a sub-block now, once a
    // block before, and that alone is a null of -28 dB under everything else.)
    static Patch rich()
    {
        return { { W::TableSelect, 4.0f }, { W::Position, 0.3f }, { W::MorphSpeed, 1.5f }, { W::MorphDepth, 0.4f },
                 { W::ScanMode, (float) (int) WavetableScanMode::Ping }, { W::ScanTime, 0.4f },
                 { W::UnisonSpread, 0.3f }, { W::SubLevel, 0.3f }, { W::EnvToPosition, 0.2f }, { W::VelToPosition, 0.2f },
                 { W::AmpAttack, 0.02f }, { W::AmpDecay, 0.3f }, { W::AmpSustain, 0.7f }, { W::AmpRelease, 0.2f },
                 { W::FilterBase + VF::Cutoff, 1500.0f }, { W::FilterBase + VF::Resonance, 0.4f }, { W::FilterBase + VF::EnvAmount, 0.3f },
                 { W::FilterBase + VF::Decay, 0.3f }, { W::FilterBase + VF::Sustain, 0.4f },
                 { dualId(VF::Routing), 2.0f }, { dualId(VF::Balance), 0.5f }, { dualId(VF::OffsetB), 12.0f }, { dualId(VF::FilterFM), 0.2f } };
    }
    // A sound without what is not sample for sample the old arithmetic by design (the sub's sine
    // is PhaseOsc's, the VIBR cell's sine is the router's, the spread copy's phase step is
    // rounded once more in PhaseOsc::advance): what must MATCH is tested on this.
    static Patch plain()
    {
        return { { W::TableSelect, 4.0f }, { W::Position, 0.3f }, { W::AmpAttack, 0.005f }, { W::AmpSustain, 0.8f }, { W::AmpRelease, 0.2f } };
    }
    static Patch with(Patch a, const Patch& b) { return operator_plus(std::move(a), b); }

    static std::function<float(long long)> sine(double hz, float level)
    {
        return [hz, level](long long n) { return level * (float) std::sin(2.0 * juce::MathConstants<double>::pi * hz * (double) n / SR); };
    }
    static std::function<float(double)> rise(double from, double to, float top = 1.0f)
    {
        return [from, to, top](double t) { return top * (float) juce::jlimit(0.0, 1.0, (t - from) / (to - from)); };
    }

    // The strip pushed up over 0.4 s. It leaves 0 where the router's vibrato (which always runs)
    // is at the start of a cycle: the old engine's strip vibrato began its first cycle when the
    // strip first rose, so the two vibratos are then in step and the renders can be compared.
    // (The phase of a vibrato against the note is free in both.)
    static std::function<float(double)> stripUp(float top = 1.0f)
    {
        const double start = (double) (17472 - 1) / SR;     // 2 / 5.5 s, to the block
        return rise(start, start + 0.4, top);
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_WT_BENCH", {}).isNotEmpty())
        {
            bench();
            return;
        }
        tableAndIds();
        factoryPresets();
        oscillatorAndScan();
        lfoDestinations();
        slotsStripPressure();
        trackInput();
        spreadAndEnvelopes();
        whatIsNew();
        review();
        summary();
    }

    // =============================================================================================
    void tableAndIds()
    {
        beginTest("The table: the old destinations at their old numbers, then ETIM FTIM PM; what is per sample");
        {
            const auto& t = W::lfoDests();
            const auto& old = O::lfoDests();
            expectEquals(old.size(), 39);
            for (int i = 0; i < old.size(); ++i)
                expectEquals(juce::String(t.labels()[i]), juce::String(old.labels()[i]), "destination " + juce::String(i));
            expectEquals(t.visible(), 42);
            expectEquals(t.size(), 42);
            expectEquals(juce::String(t.labels()[39]), juce::String("ETIM"));
            expectEquals(juce::String(t.labels()[40]), juce::String("FTIM"));
            expectEquals(juce::String(t.labels()[41]), juce::String("PM"));
            expectEquals(W::dests().envTime, 39);
            expectEquals(W::dests().filterTime, 40);
            expectEquals(W::dests().phase, 41);
            std::set<juce::String> perSample;
            for (int i = 1; i < t.size(); ++i)
                if (t[i].perSample)
                    perSample.insert(t[i].label);
            const std::set<juce::String> expected { "PTCH", "AMP", "PAN", "CUT", "POS", "PM", "BAL", "FFM", "OFSB" };
            expect(perSample == expected, "per sample: " + juce::StringArray(std::vector<juce::String>(perSample.begin(), perSample.end()).data(), (int) perSample.size()).joinIntoString(" "));
            expect(t[t.indexOfLabel("PM")].squared, "PM's amount counts squared, as XDPT did");
            expectEquals(t[t.indexOfLabel("PM")].span, WavetableVoice::PM_CYCLES);
            // The strip's defaults are what they were.
            expectEquals(t.stripDefault(0).dest, (int) WavetableVoice::LfoPosition);
            expectEquals(t.stripDefault(0).amount, 0.6f);
            expectEquals(t.stripVibratoDefaultValue(), 0.25f);
            // A free slot's DEST: the table, then L1RT .. and AMT1 ..; these moved up by three.
            expectEquals(t.slotChoices(), 42 + 4 + 16 + mod::TrackDest::COUNT);     // (and the track's effect cells: E4)
        }

        beginTest("Ids: 158-189 and 700-779 are the router's; the old XIN cells are gone; every cell is set and returned");
        {
            W e;
            const auto pages = e.getParamPages();
            juce::StringArray names;
            for (auto& page : pages)
                names.add(page.name);
            expectEquals(names.joinIntoString(" "),
                         juce::String("WAVE SCAN OSC ANLG AMP KEYS K SMP K POS K PLC VOICE FILTR FLT B F ENV XIN SLT1 LSHP STRIP LFO1 LFO2"));
            for (auto& page : pages)
                for (auto& p : page.params)
                {
                    if (! p.isValid())
                        continue;
                    const bool routers = (p.id >= 158 && p.id <= 189) || (p.id >= 700 && p.id <= 779);
                    expect(routers == R::owns(p.id), juce::String(p.label) + " at " + juce::String(p.id));
                    expect(routers || p.id < 150, juce::String(p.label) + " is in the engine's first block");
                    expect(! (p.id >= 65 && p.id <= 72), "65-72 are free");
                    expectEquals(e.getParameter(p.id), p.defaultVal, juce::String(page.name) + " " + p.label + " is seeded");
                }
            // The XIN page is the router's: two routes, their filters, LPF and HPF.
            const auto& xin = pages[13];     // +1: VOICE page
            static const char* const labels[8] = { "DST1", "AMT1", "FLT1", "LPF", "DST2", "AMT2", "FLT2", "HPF" };
            for (int c = 0; c < 8; ++c)
            {
                expectEquals(juce::String(xin.params[(size_t) c].label), juce::String(labels[c]));
                expectEquals(xin.params[(size_t) c].id, 182 + c);
            }
            expectEquals(juce::String(xin.params[0].choices[41]), juce::String("PM"));
            // The input's filters are open until set, as this engine's XLP and XHP were.
            expectEquals(xin.params[3].defaultVal, 20000.0f);
            expectEquals(xin.params[7].defaultVal, 20.0f);
        }

        beginTest("Sources: the router's list, then SCAN, XRCT and XFLT");
        {
            const auto& s = W::sources();
            const auto& standard = mod::SourceTable::standard(2);
            expectEquals(s.size(), standard.size() + 3);
            for (int i = 0; i < standard.size(); ++i)
                expectEquals(juce::String(s.labels()[i]), juce::String(standard.labels()[i]));
            expectEquals(juce::String(s.labels()[s.size() - 3]), juce::String("SCAN"));
            expectEquals(juce::String(s.labels()[s.size() - 2]), juce::String("XRCT"));
            expectEquals(juce::String(s.labels()[s.size() - 1]), juce::String("XFLT"));
            expectEquals(published(WavetableVoice::PubScan), 37);
            expectEquals(published(WavetableVoice::PubRect), 38);
            expectEquals(published(WavetableVoice::PubFiltered), 39);
            expect(s.audio(WavetableVoice::PubFiltered) && s.bipolar(WavetableVoice::PubFiltered), "XFLT: audio rate, both ways");
            expect(! s.audio(WavetableVoice::PubScan) && ! s.bipolar(WavetableVoice::PubScan), "SCAN: control rate, 0..1");
            expect(s.audio(WavetableVoice::PubRect) && ! s.bipolar(WavetableVoice::PubRect), "XRCT: audio rate, 0..1");
            // The old slots' sources keep their places.
            for (int i = 0; i < oldwt::ModSrc::BASIC_COUNT; ++i)
                expectEquals(juce::String(s.labels()[i]), juce::String(oldwt::ModSrc::labels()[i]));
        }
    }

    // =============================================================================================
    static std::vector<Preset> wavetablePresets()
    {
        std::vector<Preset> out;
        for (auto& p : FactoryPresets::getAll())
            if (p.engineType == EngineType::Wavetable)
                out.push_back(p);
        return out;
    }
    static Setup presetSetup(const Preset& p)
    {
        const ParamMap params = p.soundParams;
        return [params](EngineBase& e)
        {
            for (auto& kv : params)
                if (EngineParamRange::contains(kv.first))
                    e.setParameter(kv.first, kv.second);
        };
    }
    // True when the preset uses nothing that is by design another arithmetic now: the sub (the
    // sine of PhaseOsc), the spread copy (its phase step), the VIBR cell (the router's vibrato
    // sine), an LFO (the router's).
    static bool mustMatch(const Preset& p)
    {
        for (auto& kv : p.soundParams)
            if (((kv.first == W::SubLevel || kv.first == W::Vibrato || kv.first == W::UnisonSpread) && kv.second > 0.0f) || kv.first >= 158)
                return false;
        return true;
    }

    void factoryPresets()
    {
        beginTest("Every wavetable factory preset, old against new: two notes held and let go");
        const auto presets = wavetablePresets();
        expect(presets.size() >= 20, "the wavetable presets: " + juce::String((int) presets.size()));
        int matches = 0;
        for (auto& p : presets)
        {
            Gesture g;
            g.seconds = 1.4;
            g.notes = { { 57, 0.9f, 0.0, 0.9 }, { 64, 0.6f, 0.1, 0.9 } };
            const Verdict v = pair("preset " + p.name, presetSetup(p), presetSetup(p), g);
            expect(! v.lost(), p.name + ": " + v.text());
            if (mustMatch(p))
                expect(v.match(), p.name + " uses nothing whose arithmetic changed: the same samples (" + v.text() + ")");
            matches += v.match() ? 1 : 0;
        }
        logMessage("[U4-WT] presets, plain gesture: " + juce::String(matches) + " of " + juce::String((int) presets.size()) + " MATCH");

        beginTest("Every wavetable factory preset with the mod strip pushed up (its default routes: POS +60 and vibrato)");
        for (auto& p : presets)
        {
            Gesture g;
            g.seconds = 1.4;
            g.notes = { { 57, 0.9f, 0.0, 1.2 }, { 64, 0.6f, 0.1, 1.2 } };
            g.strip = stripUp();
            const Verdict v = pair("preset " + p.name + " + strip", presetSetup(p), presetSetup(p), g);
            expect(! v.lost(), p.name + " with the strip: " + v.text());
        }
    }

    // =============================================================================================
    void oscillatorAndScan()
    {
        beginTest("Each read-out (INTP: LIN SPEC STEP VINT CLSC) through PhaseOsc: the same samples, low and high notes");
        {
            static const char* const names[] = { "LIN", "SPEC", "STEP", "VINT", "CLSC" };
            for (int mode = 0; mode < (int) WavetableInterpMode::Count; ++mode)
                for (int table : { 4, 8, 0 })
                {
                    Gesture g;
                    g.notes = { { 33, 0.9f, 0.0, 0.7 }, { 64, 0.7f, 0.0, 0.7 }, { 96, 0.6f, 0.0, 0.7 }, { 115, 0.5f, 0.1, 0.7 } };
                    const Verdict v = pair(juce::String("INTP ") + names[mode] + " table " + juce::String(table),
                                           with(plain(), { { W::TableSelect, (float) table }, { W::InterpMode, (float) mode }, { W::MorphSpeed, 2.0f },
                                                           { W::Octave, mode == 4 ? 1.0f : 0.0f } }), g);
                    expect(v.match(), juce::String("INTP ") + names[mode] + ": " + v.text());
                }
        }

        beginTest("Tuning, glide, bend, the spread copy, the envelope chain, block sizes: the same samples");
        {
            Gesture legato;
            legato.notes = { { 50, 0.9f, 0.0, 0.3 }, { 62, 0.8f, 0.25, 0.7 }, { 57, 0.7f, 0.5, 0.8 } };
            legato.bend = [](double t) { return (float) std::sin(t * 9.0); };
            expect(pair("glide, tune, fine, bend", with(plain(), { { W::Glide, 0.08f }, { W::TuneParam, 3.0f }, { W::Detune, -30.0f },
                                                                    { W::BendRangeParam, 7.0f } }), legato).match());
            expect(pair("envelope chain SER", with(plain(), { { W::EnvChainStages, 3.0f }, { W::EnvChainMode, 1.0f }, { W::AmpDecay, 0.1f } }), Gesture()).match());
            expect(pair("envelope chain PAR MAX", with(plain(), { { W::EnvChainStages, 2.0f }, { W::EnvChainMode, 2.0f }, { W::EnvChainCombine, 1.0f } }), Gesture()).match());
            expect(pair("ENVP, VELP, filter", with(plain(), { { W::EnvToPosition, 0.5f }, { W::VelToPosition, 0.3f },
                                                              { W::FilterBase + VF::Cutoff, 900.0f }, { W::FilterBase + VF::EnvAmount, 0.5f },
                                                              { W::FilterBase + VF::Resonance, 0.6f }, { W::FilterBase + VF::KeyTrack, 0.5f },
                                                              { W::FilterBase + VF::VelAmount, 0.4f } }), Gesture()).match());
            for (int block : { 16, 100, 512 })
            {
                Gesture g;
                g.block = block;
                g.seconds = 0.8;
                g.notes = { { 57, 0.9f, 0.0, 0.5 }, { 64, 0.6f, 0.2, 0.6 } };
                expect(pair("blocks of " + juce::String(block), with(plain(), { { W::MorphSpeed, 3.0f }, { W::EnvToPosition, 0.3f } }), g).match());
            }
        }

        beginTest("The sub, the spread copy and the VIBR cell: another arithmetic in the last bits, the same sound");
        {
            Gesture wide;
            wide.notes = { { 33, 0.9f, 0.0, 0.7 }, { 64, 0.7f, 0.0, 0.7 }, { 96, 0.6f, 0.0, 0.7 }, { 115, 0.5f, 0.1, 0.7 } };
            for (int mode : { 0, 3 })
            {
                const Verdict copy = pair("SPRD (the copy's phase step), INTP " + juce::String(mode),
                                          with(plain(), { { W::UnisonSpread, 0.4f }, { W::InterpMode, (float) mode }, { W::MorphSpeed, 2.0f } }), wide);
                expect(copy.match() || copy.null < -150.0, "the copy's phase is one rounding off now and then: " + copy.text());
            }
            const Verdict sub = pair("SUB (the sine of PhaseOsc)", with(plain(), { { W::SubLevel, 0.6f } }), Gesture());
            expect(! sub.lost() && sub.null < -100.0, "the two sines differ in their last bits only: " + sub.text());
            const Verdict vib = pair("VIBR cell (the router's vibrato sine)", with(plain(), { { W::Vibrato, 0.6f } }), Gesture());
            expect(! vib.lost(), vib.text());
        }

        beginTest("Each scan mode, old against new");
        {
            struct Case { const char* name; WavetableScanMode mode; Patch more; };
            const Case cases[] = {
                { "RUN", WavetableScanMode::Run, {} },
                { "LOOP", WavetableScanMode::Loop, {} },
                { "PING", WavetableScanMode::Ping, {} },
                { "STEP", WavetableScanMode::Step, { { W::ScanSteps, 1.0f } } },
                { "LOOP REV x2", WavetableScanMode::Loop, { { W::ScanDirection, 1.0f }, { W::ScanSpeed, 2.0f } } },
                { "PING SYNC 1/4", WavetableScanMode::Ping, { { W::ScanSync, 1.0f }, { W::ScanLength, 2.0f } } },
                { "RUN with MSPD and ENVP", WavetableScanMode::Run, { { W::MorphSpeed, 4.0f }, { W::MorphDepth, 0.3f }, { W::EnvToPosition, -0.4f } } } };
            for (auto& c : cases)
            {
                const Verdict v = pair(juce::String("scan ") + c.name,
                                       with(with(plain(), { { W::Position, 0.1f }, { W::ScanMode, (float) (int) c.mode }, { W::ScanTime, 0.3f } }), c.more), Gesture());
                expect(v.match(), juce::String("scan ") + c.name + ": " + v.text());
            }
        }

        beginTest("STAT - the RAND scan: other places than before (another generator, by design), the same kind of sound");
        {
            // Long enough for many draws: 8 steps a pass of 0.2 s, 3 s = 120 places a voice.
            Gesture g;
            g.seconds = 3.0;
            g.notes = { { 57, 0.9f, 0.0, 2.9 }, { 64, 0.6f, 0.0, 2.9 } };
            const Patch patch = with(plain(), { { W::Position, 0.0f }, { W::ScanMode, (float) (int) WavetableScanMode::Rand },
                                                { W::ScanTime, 0.2f }, { W::ScanSteps, 1.0f } });
            const Stereo a = play<O>(cells(patch), g), b = play<W>(cells(patch), g);
            expect(a.l != b.l, "the sequence is another one");
            float r1 = 0.0f, c1 = 0.0f, r2 = 0.0f, c2 = 0.0f;
            fastmeasure::rmsAndCentroid(a.l, 4096, SR, r1, c1);
            fastmeasure::rmsAndCentroid(b.l, 4096, SR, r2, c2);
            const double level = db(r2 / r1), ratio = c2 / c1;
            logMessage("[U4-WT] STAT scan RAND: level " + juce::String(level, 2) + " dB, mean frequency x" + juce::String(ratio, 3));
            expect(std::abs(level) < 1.0, "the same level over 120 places: " + juce::String(level, 2) + " dB");
            expect(std::abs(ratio - 1.0) < 0.10, "the same brightness over 120 places: x" + juce::String(ratio, 3));
            writeWav("STAT scan RAND_old", a);
            writeWav("STAT scan RAND_new", b);
            // The places themselves: whole frames between POS and the end, evenly spread, many of them.
            auto places = [&patch](auto& engine)
            {
                cells(patch)(engine);
                AftertouchRouter off;
                off.clear();
                engine.setAftertouchRouter(&off);
                engine.prepareToPlay(SR, 64);
                engine.noteOn(60, 0.8f, -1);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::vector<float> seen;
                for (int b = 0; b < 15000; ++b)        // 20 s: 800 places
                {
                    buf.clear();
                    engine.processBlock(buf, midi);
                    if (b % 18 == 9)                    // once inside every step of 1200 samples (18.75 blocks)
                        seen.push_back(engine.getVoice(0).getReadPosition());
                }
                engine.setAftertouchRouter(nullptr);
                return seen;
            };
            O oldEngine;
            W newEngine;
            const auto po = places(oldEngine), pn = places(newEngine);
            auto stats = [](const std::vector<float>& p, double& mean, double& sd, int& distinct)
            {
                std::set<int> frames;
                mean = sd = 0.0;
                for (float v : p) { mean += v; frames.insert((int) std::lround(v * 255.0f)); }
                mean /= (double) p.size();
                for (float v : p) sd += (v - mean) * (v - mean);
                sd = std::sqrt(sd / (double) p.size());
                distinct = (int) frames.size();
            };
            double m1, s1, m2, s2;
            int d1, d2;
            stats(po, m1, s1, d1);
            stats(pn, m2, s2, d2);
            logMessage("[U4-WT] STAT scan RAND places: old mean " + juce::String(m1, 3) + " sd " + juce::String(s1, 3) + " distinct " + juce::String(d1)
                       + "; new mean " + juce::String(m2, 3) + " sd " + juce::String(s2, 3) + " distinct " + juce::String(d2));
            expect(std::abs(m2 - 0.5) < 0.05 && std::abs(s2 - 0.2887) < 0.03, "uniform between POS and the end");
            expect(std::abs(m2 - m1) < 0.06 && std::abs(s2 - s1) < 0.04, "as the old places were");
            expect(d2 >= d1 * 3 / 4, "as many different frames");
        }
    }

    // =============================================================================================
    void lfoDestinations()
    {
        beginTest("LFO 1 on every destination the engine had, old against new");
        {
            const auto& old = O::lfoDests();
            for (int d = 1; d < old.size(); ++d)
            {
                const juce::String label = old.labels()[d];
                Patch patch = with(rich(), lfo(0, 3.0f, 0.5f, d));
                if (label == "LRAT" || label == "LDPT")
                    patch = with(patch, lfo(1, 5.0f, 0.5f, WavetableVoice::LfoPosition));       // an LFO for LFO 1 to move
                Gesture g;
                g.seconds = 1.2;
                g.notes = { { 57, 0.9f, 0.0, 0.9 }, { 64, 0.6f, 0.3, 0.9 } };
                const Verdict v = pair("LFO1 > " + label, patch, g);
                expect(! v.lost(), "LFO1 > " + label + ": " + v.text());
            }
        }

        beginTest("LFOs that step, run free, fade in or follow the tempo, old against new");
        {
            Gesture g;
            g.seconds = 1.2;
            g.notes = { { 57, 0.9f, 0.0, 0.9 }, { 64, 0.6f, 0.3, 0.9 } };
            const int square = (int) LFO::Shape::Square;
            struct Case { const char* name; Patch patch; };
            const Case cases[] = {
                { "LFO1 square > PTCH", lfo(0, 6.0f, 0.4f, WavetableVoice::LfoPitch, square) },
                { "LFO1 square > POS", lfo(0, 6.0f, 0.5f, WavetableVoice::LfoPosition, square) },
                { "LFO1 square > AMP", lfo(0, 6.0f, 0.6f, WavetableVoice::LfoAmp, square) },
                { "LFO1 FREE > CUT", lfo(0, 2.0f, 0.5f, WavetableVoice::LfoCutoff, 0, 0) },
                { "LFO1 ONE > POS, LFO2 > PAN", with(lfo(0, 1.5f, 0.6f, WavetableVoice::LfoPosition, 0, 2), lfo(1, 4.0f, 0.8f, WavetableVoice::LfoPan)) },
                { "LFO1 FADE 0.5 SYNC > AMP", with(lfo(0, 6.0f, 0.6f, WavetableVoice::LfoAmp), { { R::lfoId(0, R::Fade), 0.5f }, { R::lfoId(0, R::Sync), 1.0f } }) },
                { "LFO1 negative depth > SUB", lfo(0, 3.0f, -0.6f, WavetableVoice::LfoSub) } };
            for (auto& c : cases)
            {
                const Verdict v = pair(c.name, with(rich(), c.patch), g);
                expect(! v.lost(), juce::String(c.name) + ": " + v.text());
            }
        }
    }

    // =============================================================================================
    void slotsStripPressure()
    {
        beginTest("Mod slots, old against new");
        {
            Gesture g;
            g.seconds = 1.2;
            g.notes = { { 45, 0.9f, 0.0, 0.9 }, { 76, 0.5f, 0.3, 0.9 } };
            Gesture strip = g;
            strip.notes = { { 45, 0.9f, 0.0, 1.0 }, { 76, 0.5f, 0.3, 1.0 } };
            strip.strip = stripUp();
            Gesture pressed = g;
            pressed.pressure = rise(0.2, 0.7, 0.8f);
            Gesture follower = g;
            follower.input = sine(200.0, 0.0f);        // connected and silent: the follower is the rig's 0.5
            namespace S = mod::Src;
            struct Case { const char* name; Patch slots; const Gesture* gesture; };
            const Case cases[] = {
                { "slot ENV > POS", slot(0, S::Env, WavetableVoice::LfoPosition, 0.5f), &g },
                { "slot FENV > SUB", slot(0, S::FilterEnv, WavetableVoice::LfoSub, 0.6f), &g },
                { "slot VEL > CUT", slot(0, S::Velocity, WavetableVoice::LfoCutoff, 0.5f), &g },
                { "slot KEY > PAN, KEY > POS", with(slot(0, S::Key, WavetableVoice::LfoPan, 1.0f), slot(1, S::Key, WavetableVoice::LfoPosition, -0.8f)), &g },
                { "slot PRES > AMP", slot(0, S::Pressure, WavetableVoice::LfoAmp, -0.6f), &pressed },
                { "slot PRES > POS, PRES > PTCH", with(slot(0, S::Pressure, WavetableVoice::LfoPosition, 0.6f), slot(3, S::Pressure, WavetableVoice::LfoPitch, 0.3f)), &pressed },
                { "slot STRP > CUT", slot(1, S::Strip, WavetableVoice::LfoCutoff, 0.6f), &strip },
                { "slot STRP > SUB, no strip vibrato", with(slot(1, S::Strip, WavetableVoice::LfoSub, 0.6f), { { R::STRIP_VIBRATO, 0.0f }, { R::stripAmountId(0), 0.0f } }), &strip },
                { "slot IN > POS", slot(0, S::Input, WavetableVoice::LfoPosition, 0.8f), &follower },
                { "slot LFO1 > PTCH via STRP", with(slot(2, S::Lfo1, WavetableVoice::LfoPitch, 0.4f, S::Strip), { { R::lfoId(0, R::Rate), 6.0f } }), &strip },
                { "slot LFO2 > AMP via ENV", with(slot(0, S::Lfo2, WavetableVoice::LfoAmp, 0.8f, S::Env), { { R::lfoId(1, R::Rate), 8.0f } }), &g },
                { "slot ENV > DEC (a page cell)", slot(0, S::Env, dest("DEC"), 0.5f), &g },
                { "slot VEL > MSPD (a page cell)", slot(0, S::Velocity, dest("MSPD"), 0.5f), &g } };
            for (auto& c : cases)
            {
                const Verdict v = pair(c.name, with(rich(), c.slots), *c.gesture);
                expect(! v.lost(), juce::String(c.name) + ": " + v.text());
            }
            // A slot on another slot's amount, and on an LFO's rate: the cells after the table,
            // which stand three further on now (ETIM, FTIM and PM came before them).
            const Patch lfoOnPos = with(rich(), lfo(0, 5.0f, 0.0f, 0));
            auto amountOf = [&](int tableSize)
            {
                return with(with(lfoOnPos, slot(0, S::Lfo1, WavetableVoice::LfoPosition, 0.1f)), slot(1, S::Env, tableSize + 4 + 0, 0.6f));
            };
            Verdict v = pair("slot ENV > AMT1 (of LFO1 > POS)", cells(amountOf(39)), cells(amountOf(42)), g);
            expect(! v.lost(), "ENV > AMT1: " + v.text());
            auto rateOf = [&](int tableSize)
            {
                return with(with(rich(), lfo(0, 3.0f, 0.5f, WavetableVoice::LfoPosition)), slot(0, S::Env, tableSize + 0, 0.8f));
            };
            v = pair("slot ENV > L1RT", cells(rateOf(39)), cells(rateOf(42)), g);
            expect(! v.lost(), "ENV > L1RT: " + v.text());
        }

        beginTest("STAT - the source RAND: other values than before (the one seeded generator), the same spread");
        {
            // RAND > PAN on eight notes one after the other: where each lands is another place now,
            // how far they spread is the same thing.
            auto pans = [](auto& engine)
            {
                cells(with(plain(), slot(0, mod::Src::Random, WavetableVoice::LfoPan, 1.0f)))(engine);
                AftertouchRouter off;
                off.clear();
                engine.setAftertouchRouter(&off);
                engine.prepareToPlay(SR, 64);
                std::vector<double> out;
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                for (int n = 0; n < 64; ++n)
                {
                    engine.allNotesOff();
                    engine.noteOn(48 + (n % 24), 0.8f, -1);
                    double l = 0.0, r = 0.0;
                    for (int b = 0; b < 40; ++b)
                    {
                        buf.clear();
                        engine.processBlock(buf, midi);
                        for (int i = 0; i < 64; ++i) { l += std::abs(buf.getSample(0, i)); r += std::abs(buf.getSample(1, i)); }
                    }
                    out.push_back((r - l) / std::max(1.0e-9, std::max(l, r)));      // -1 left .. 1 right
                }
                engine.setAftertouchRouter(nullptr);
                return out;
            };
            O oldEngine;
            W newEngine;
            const auto po = pans(oldEngine), pn = pans(newEngine);
            auto spreadOf = [](const std::vector<double>& p)
            {
                double mean = 0.0, sd = 0.0;
                for (double v : p) mean += v;
                mean /= (double) p.size();
                for (double v : p) sd += (v - mean) * (v - mean);
                return std::make_pair(mean, std::sqrt(sd / (double) p.size()));
            };
            const auto so = spreadOf(po), sn = spreadOf(pn);
            logMessage("[U4-WT] STAT source RAND > PAN over 64 notes: old mean " + juce::String(so.first, 3) + " sd " + juce::String(so.second, 3)
                       + "; new mean " + juce::String(sn.first, 3) + " sd " + juce::String(sn.second, 3));
            expect(po != pn, "other values");
            expect(std::abs(sn.first) < 0.25 && sn.second > 0.4, "both ways, widely");
            expect(std::abs(sn.second - so.second) < 0.15, "as widely as before");
        }

        beginTest("The strip's own routes and vibrato, old against new");
        {
            Gesture g;
            g.seconds = 1.4;
            g.notes = { { 57, 0.9f, 0.0, 1.2 }, { 64, 0.6f, 0.0, 1.2 } };
            g.strip = stripUp();
            Verdict v = pair("strip: default routes (POS +60, WVIB 0.25)", plain(), g);
            expect(! v.lost(), v.text());
            v = pair("strip: WVIB 1.0 VRAT 11, nothing else", with(plain(), { { R::stripAmountId(0), 0.0f }, { R::STRIP_VIBRATO, 1.0f }, { R::STRIP_VIBRATO_RATE, 11.0f } }), g);
            expect(! v.lost(), v.text());
            v = pair("strip: three routes (CUT, AMP, SUB)", with(rich(), { { R::stripDestId(0), (float) WavetableVoice::LfoCutoff }, { R::stripAmountId(0), 0.5f },
                                                                            { R::stripDestId(1), (float) WavetableVoice::LfoAmp }, { R::stripAmountId(1), -0.5f },
                                                                            { R::stripDestId(2), (float) WavetableVoice::LfoSub }, { R::stripAmountId(2), 0.7f },
                                                                            { R::STRIP_VIBRATO, 0.0f } }), g);
            expect(! v.lost(), v.text());
            v = pair("strip: the filter's WHL amount", with(rich(), { { W::FilterBase + VF::WheelAmount, 0.5f }, { R::stripAmountId(0), 0.0f }, { R::STRIP_VIBRATO, 0.0f } }), g);
            expect(! v.lost(), v.text());
        }

        beginTest("Pad pressure through the track's routes (AT ROUTE), old against new");
        {
            using D = AftertouchRouter::Destination;
            struct Case { const char* name; D dest; float depth; bool bipolar; };
            const Case cases[] = { { "pressure > VOL", D::Volume, 1.0f, false }, { "pressure > VOL bipolar", D::Volume, 0.8f, true },
                                   { "pressure > CUT", D::FilterCutoff, 0.5f, false }, { "pressure > WAVE", D::WavePosition, 0.6f, false },
                                   { "pressure > TIMB", D::MacroTimbre, -0.3f, false }, { "pressure > VIB", D::Vibrato, 1.0f, false } };
            for (auto& c : cases)
            {
                AftertouchRouter routes;
                routes.clear();
                routes.setRoute(0, { c.dest, c.depth, c.bipolar });
                Gesture g;
                g.seconds = 1.4;
                g.notes = { { 57, 0.9f, 0.0, 1.2 }, { 64, 0.6f, 0.0, 1.2 } };
                g.pressure = rise(0.2, 0.7, 0.8f);
                g.routes = &routes;
                const Verdict v = pair(c.name, with(plain(), { { W::FilterBase + VF::Cutoff, 1200.0f } }), g);
                expect(! v.lost(), juce::String(c.name) + ": " + v.text());
            }
        }
    }

    // =============================================================================================
    void trackInput()
    {
        beginTest("The old XIN page in each of its modes (the old cells against the slots they became)");
        {
            struct Case { const char* name; Patch sound; std::vector<std::pair<int, float>> xin; double inputHz; bool strip; };
            const Patch sinePatch = with(plain(), { { W::TableSelect, 3.0f } });
            const Patch middle = with(plain(), { { W::Position, 0.5f } });
            std::vector<Case> cases = {
                { "XIN PM 50", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.5f } }, 300.0, false },
                { "XIN PM 100, PWM table", plain(), { { X::Mode, 1 }, { X::Depth, 1.0f } }, 300.0, false },
                { "XIN PM 60, spread and sub", with(plain(), { { W::UnisonSpread, 0.4f }, { W::SubLevel, 0.4f } }), { { X::Mode, 1 }, { X::Depth, 0.6f } }, 110.0, false },
                { "XIN POS 50 at POS 50 (both ends)", middle, { { X::Mode, 2 }, { X::Depth, 0.5f } }, 300.0, false },
                { "XIN POS 100 at POS 50 (folds)", middle, { { X::Mode, 2 }, { X::Depth, 1.0f } }, 300.0, false },
                { "XIN POS 30 at POS 0", with(plain(), { { W::Position, 0.0f } }), { { X::Mode, 2 }, { X::Depth, 0.3f } }, 150.0, false },
                { "XIN POS 40 under a scan", with(plain(), { { W::Position, 0.1f }, { W::ScanMode, 3.0f }, { W::ScanTime, 0.3f } }), { { X::Mode, 2 }, { X::Depth, 0.4f } }, 300.0, false },
                { "XIN PM 80 XENV 100", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.8f }, { X::Env, 1.0f } }, 300.0, false },
                { "XIN PM 80 XENV 50", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.8f }, { X::Env, 0.5f } }, 300.0, false },
                { "XIN PM 80 XVEL 70", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.8f }, { X::Vel, 0.7f } }, 300.0, false },
                { "XIN PM 80 XENV 60 XVEL 60", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.8f }, { X::Env, 0.6f }, { X::Vel, 0.6f } }, 300.0, false },
                { "XIN PM 20 XWHL 60, strip up", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.2f }, { X::Strip, 0.6f } }, 300.0, true },
                { "XIN POS 50 XENV 100", middle, { { X::Mode, 2 }, { X::Depth, 0.5f }, { X::Env, 1.0f } }, 300.0, false },
                { "XIN PM 60 XRCT 100", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::Rectify, 1.0f } }, 300.0, false },
                { "XIN PM 60 XRCT 50", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::Rectify, 0.5f } }, 300.0, false },
                { "XIN POS 50 XRCT 100", with(plain(), { { W::Position, 0.2f } }), { { X::Mode, 2 }, { X::Depth, 0.5f }, { X::Rectify, 1.0f } }, 300.0, false },
                { "XIN PM 60 XLP 800 (input at 300 Hz)", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::LowPass, 800.0f } }, 300.0, false },
                { "XIN PM 60 XLP 800 (input at 3 kHz)", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::LowPass, 800.0f } }, 3000.0, false },
                { "XIN PM 60 XHP 1000 (input at 3 kHz)", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::HighPass, 1000.0f } }, 3000.0, false },
                { "XIN PM 60 XHP 1000 (input at 300 Hz)", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::HighPass, 1000.0f } }, 300.0, false } };
            static const char* const interp[] = { "LIN", "SPEC", "STEP", "VINT", "CLSC" };
            for (int mode = 0; mode < 5; ++mode)
                cases.push_back({ interp[mode], with(plain(), { { W::TableSelect, 8.0f }, { W::InterpMode, (float) mode } }),
                                  { { X::Mode, 1 }, { X::Depth, 0.7f } }, 300.0, false });
            for (auto& c : cases)
            {
                Gesture g;
                g.seconds = 1.0;
                g.notes = { { 57, 0.9f, 0.0, 0.7 }, { 64, 0.5f, 0.0, 0.7 } };
                g.input = sine(c.inputHz, 0.7f);
                if (c.strip)
                    g.strip = stripUp();
                const Patch sound = c.sound;
                const auto xin = c.xin;
                const juce::String name = juce::String(c.name).length() <= 4 ? "XIN PM 70 with INTP " + juce::String(c.name) : juce::String(c.name);
                const Verdict v = pair(name,
                    [sound, xin](EngineBase& e) { cells(sound)(e); for (auto& x : xin) e.setParameter(x.first, x.second); },
                    [sound, xin](EngineBase& e) { cells(sound)(e); X cellsOfOld; for (auto& x : xin) cellsOfOld.set(e, x.first, x.second); }, g);
                expect(! v.lost(), name + ": " + v.text());
            }
        }
    }

    // =============================================================================================
    void spreadAndEnvelopes()
    {
        beginTest("The ANLG page (the per-note spread), old against new: the same samples for the same SEED");
        {
            Gesture g;
            g.seconds = 1.6;
            g.notes = { { 57, 0.9f, 0.0, 0.3 }, { 60, 0.8f, 0.2, 0.5 }, { 64, 0.7f, 0.4, 0.7 }, { 67, 0.8f, 0.6, 0.9 }, { 57, 0.6f, 0.8, 1.1 }, { 72, 0.9f, 1.0, 1.3 } };
            const int b = W::SpreadBase;
            struct Case { const char* name; Patch patch; bool spreadCopy = false; };
            const Case cases[] = {
                { "ANLG SLOP 30", { { b + VoiceSpread::Slop, 30.0f } } },
                { "ANLG FVAR 60", { { b + VoiceSpread::Cutoff, 0.6f }, { W::FilterBase + VF::Cutoff, 1000.0f } } },
                { "ANLG EVAR 80", { { b + VoiceSpread::EnvTime, 0.8f }, { W::AmpAttack, 0.05f }, { W::AmpDecay, 0.2f }, { W::FilterBase + VF::EnvAmount, 0.4f },
                                    { W::FilterBase + VF::Cutoff, 800.0f } } },
                { "ANLG EVAR 80 with the envelope chain", { { b + VoiceSpread::EnvTime, 0.8f }, { W::AmpDecay, 0.1f }, { W::EnvChainStages, 3.0f }, { W::EnvChainMode, 1.0f } } },
                { "ANLG WVAR 70", { { b + VoiceSpread::Wave, 0.7f } } },
                { "ANLG PHAS 100", { { b + VoiceSpread::Phase, 1.0f } } },
                { "ANLG PHAS 100 with SPRD", { { b + VoiceSpread::Phase, 1.0f }, { W::UnisonSpread, 0.3f } }, true },
                { "ANLG PVAR 80 LVAR 50", { { b + VoiceSpread::Pan, 0.8f }, { b + VoiceSpread::Level, 0.5f } } },
                { "ANLG everything, SEED 17", { { b + VoiceSpread::Slop, 20.0f }, { b + VoiceSpread::Cutoff, 0.4f }, { b + VoiceSpread::EnvTime, 0.5f },
                                               { b + VoiceSpread::Wave, 0.4f }, { b + VoiceSpread::Phase, 0.6f }, { b + VoiceSpread::Pan, 0.5f },
                                               { b + VoiceSpread::Level, 0.3f }, { b + VoiceSpread::Seed, 17.0f },
                                               { W::FilterBase + VF::Cutoff, 1500.0f }, { W::FilterBase + VF::EnvAmount, 0.3f } } } };
            for (auto& c : cases)
            {
                const Verdict v = pair(c.name, with(plain(), c.patch), g);
                // (With the spread copy: its phase step is rounded once more, see the SPRD rows.)
                expect(c.spreadCopy ? (v.match() || v.null < -150.0) : v.match(), juce::String(c.name) + ": " + v.text());
            }
        }

        beginTest("Stage curves and envelope cells set under a held note, old against new: the same samples");
        {
            auto setup = [](EngineBase& e)
            {
                cells(with(plain(), { { W::AmpAttack, 0.2f }, { W::AmpDecay, 0.3f }, { W::AmpSustain, 0.4f } }))(e);
                e.setParameter(StageCurveParam::id(0, StageCurveParam::Attack), 0.7f);
                e.setParameter(StageCurveParam::id(0, StageCurveParam::Decay), -0.6f);
                e.setParameter(StageCurveParam::id(0, StageCurveParam::Release), 0.5f);
            };
            expect(pair("stage curves", setup, setup, Gesture()).match());
        }
    }

    // =============================================================================================
    static double balance(const Stereo& s, double from, double to)     // -1 left .. 1 right
    {
        const double l = rmsOf(s.l, (size_t) (from * SR), (size_t) (to * SR)), r = rmsOf(s.r, (size_t) (from * SR), (size_t) (to * SR));
        return (r - l) / std::max(1.0e-12, std::max(l, r));
    }
    static double brightness(const std::vector<float>& x, double from, double to)
    {
        const size_t a = (size_t) (from * SR), b = std::min((size_t) (to * SR), x.size());
        double diff = 0.0, sum = 0.0;
        for (size_t i = a + 1; i < b; ++i)
        {
            diff += (double) (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
            sum += (double) x[i] * x[i];
        }
        return std::sqrt(diff / std::max(sum, 1.0e-20));
    }
    // Seconds until the 5 ms level first reaches `share` of the loudest 5 ms.
    static double riseTime(const std::vector<float>& x, double share)
    {
        const size_t frame = (size_t) (0.005 * SR);
        double loudest = 0.0;
        for (size_t f = 0; f + frame <= x.size(); f += frame)
            loudest = std::max(loudest, rmsOf(x, f, f + frame));
        for (size_t f = 0; f + frame <= x.size(); f += frame)
            if (rmsOf(x, f, f + frame) >= share * loudest)
                return (double) f / SR;
        return 1.0e9;
    }

    void whatIsNew()
    {
        beginTest("The source SCAN: any cell can follow the timed scan");
        {
            Gesture g;
            g.seconds = 1.0;
            g.notes = { { 60, 0.9f, 0.0, 0.95 } };
            const Patch run = with(plain(), { { W::Position, 0.0f }, { W::ScanMode, (float) (int) WavetableScanMode::Run }, { W::ScanTime, 0.8f },
                                              { W::AmpSustain, 1.0f } });
            // SCAN > PAN: the voice moves to the right as the scan runs, and stays there at its end.
            const Stereo panned = play(cells(with(run, slot(0, published(WavetableVoice::PubScan), WavetableVoice::LfoPan, 1.0f))), g);
            const double early = balance(panned, 0.02, 0.06), middle = balance(panned, 0.38, 0.42), late = balance(panned, 0.85, 0.95);
            expectWithinAbsoluteError(early, 0.05, 0.05, "at POS: the middle");
            expectWithinAbsoluteError(middle, 0.5, 0.06, "half way: half right");
            expectWithinAbsoluteError(late, 1.0, 0.02, "at the end of the table: right");
            // DIR REV: the source runs from 1 to 0.
            const Stereo reversed = play(cells(with(with(run, { { W::ScanDirection, 1.0f } }), slot(0, published(WavetableVoice::PubScan), WavetableVoice::LfoPan, 1.0f))), g);
            expect(balance(reversed, 0.02, 0.06) > 0.9 && balance(reversed, 0.85, 0.95) < 0.05, "reversed: from the end to POS");
            // With MODE OFF it is 0: the slot does nothing, and the sound is the plain one.
            const Patch still = with(plain(), { { W::AmpSustain, 1.0f } });
            const Stereo off = play(cells(with(still, slot(0, published(WavetableVoice::PubScan), WavetableVoice::LfoPan, 1.0f))), g);
            const Stereo none = play(cells(still), g);
            expect(off.l == none.l && off.r == none.r, "MODE OFF: SCAN is 0");
            // SCAN > CUT: the filter opens as the table is run through (on a sine: only the cutoff changes the level).
            const Patch dark = { { W::TableSelect, 0.0f }, { W::Position, 1.0f }, { W::ScanMode, (float) (int) WavetableScanMode::Run }, { W::ScanTime, 0.8f },
                                 { W::AmpSustain, 1.0f }, { W::FilterBase + VF::Cutoff, 300.0f } };
            const Stereo opened = play(cells(with(dark, slot(0, published(WavetableVoice::PubScan), WavetableVoice::LfoCutoff, 1.0f))), g);
            const Stereo shut = play(cells(dark), g);
            logMessage("[U4-WT] SCAN > CUT: brightness at the start " + juce::String(brightness(shut.l, 0.01, 0.04), 4) + " -> " + juce::String(brightness(opened.l, 0.01, 0.04), 4)
                       + ", at the end " + juce::String(brightness(shut.l, 0.8, 0.95), 4) + " -> " + juce::String(brightness(opened.l, 0.8, 0.95), 4));
            expect(brightness(opened.l, 0.8, 0.95) > 2.0 * brightness(shut.l, 0.8, 0.95), "SCAN > CUT opens the filter by the end of the run");
            expectWithinAbsoluteError(brightness(opened.l, 0.01, 0.04) / brightness(shut.l, 0.01, 0.04), 1.0, 0.35, "... from where it stood");
            // As a VIA: an LFO's depth grows with the scan.
            const Stereo via = play(cells(with(with(run, lfo(0, 8.0f, 0.0f, 0)), slot(0, source(mod::Src::Lfo1), WavetableVoice::LfoPan, 1.0f, published(WavetableVoice::PubScan)))), g);
            auto swing = [&via](double from, double to)
            {
                double lo = 1.0, hi = -1.0;
                for (double t = from; t < to; t += 0.01)
                {
                    const double b = balance(via, t, t + 0.01);
                    lo = std::min(lo, b);
                    hi = std::max(hi, b);
                }
                return hi - lo;
            };
            expect(swing(0.6, 0.9) > 4.0 * swing(0.0, 0.12) && swing(0.6, 0.9) > 1.0, "LFO1 > PAN via SCAN: " + juce::String(swing(0.0, 0.12), 3) + " then " + juce::String(swing(0.6, 0.9), 3));
        }

        beginTest("The RAND scan draws from the voice's generator: SEED repeats it, another SEED is another run, each note its own");
        {
            Gesture g;
            g.seconds = 1.0;
            g.notes = { { 57, 0.9f, 0.0, 0.45 }, { 57, 0.9f, 0.5, 0.95 } };
            auto rand = [&g](float seed)
            {
                return play(cells(with(plain(), { { W::Position, 0.0f }, { W::ScanMode, (float) (int) WavetableScanMode::Rand }, { W::ScanTime, 0.2f },
                                                  { W::ScanSteps, 1.0f }, { W::SpreadBase + VoiceSpread::Seed, seed } })), g);
            };
            const Stereo a = rand(5.0f), b = rand(5.0f), c = rand(6.0f);
            expect(a.l == b.l, "the same SEED: the same samples on a new engine");
            expect(a.l != c.l, "another SEED: other places");
            // The second note of the gesture is the same key on the same voice: its run is another one.
            const size_t half = (size_t) (0.5 * SR), length = (size_t) (0.4 * SR);
            bool sameRun = true;
            for (size_t i = 0; i < length && sameRun; ++i)
                sameRun = std::abs(a.l[i] - a.l[half + i]) < 1.0e-6f;
            expect(! sameRun, "each note draws its own places");
            // Setting SEED under way starts the sequence again.
            W e;
            cells(with(plain(), { { W::Position, 0.0f }, { W::ScanMode, (float) (int) WavetableScanMode::Rand }, { W::ScanTime, 0.2f }, { W::ScanSteps, 1.0f } }))(e);
            Gesture one;
            one.seconds = 0.5;
            one.notes = { { 57, 0.9f, 0.0, 0.4 } };
            e.setParameter(W::SpreadBase + VoiceSpread::Seed, 9.0f);
            const Stereo first = render(e, one);
            e.allNotesOff();
            e.setParameter(W::SpreadBase + VoiceSpread::Seed, 9.0f);
            const Stereo again = render(e, one);
            expect(first.l == again.l, "SEED set again: the run from its start");
        }

        beginTest("The randoms RAND, RND2 .. are draws of the same generator: SEED repeats them with the spread");
        {
            Gesture g;
            g.seconds = 0.8;
            g.notes = { { 57, 0.9f, 0.0, 0.2 }, { 60, 0.9f, 0.2, 0.4 }, { 64, 0.9f, 0.4, 0.6 }, { 67, 0.9f, 0.6, 0.75 } };
            auto randoms = [&g](float seed)
            {
                return play(cells(with(with(with(plain(), slot(0, source(mod::Src::Random), WavetableVoice::LfoPan, 1.0f)),
                                            slot(1, source(mod::Src::Random2), WavetableVoice::LfoPosition, 0.6f)),
                                       { { W::SpreadBase + VoiceSpread::Seed, seed }, { W::SpreadBase + VoiceSpread::Slop, 10.0f } })), g);
            };
            const Stereo a = randoms(3.0f), b = randoms(3.0f), c = randoms(4.0f);
            expect(a.l == b.l && a.r == b.r, "the same SEED: the same samples");
            expect(a.l != c.l, "another SEED: other values");
            expect(std::abs(balance(a, 0.02, 0.18) - balance(a, 0.22, 0.38)) > 0.02, "a value a note");
        }

        beginTest("The sources XRCT and XFLT: the input rectified / through two-pole filters, with no delay");
        {
            Gesture g;
            g.seconds = 0.6;
            g.notes = { { 57, 0.9f, 0.0, 0.5 } };
            g.input = sine(300.0, 0.8f);
            Gesture rectified = g;
            rectified.input = [in = g.input](long long n) { return std::abs(in(n)); };
            const Patch sinePatch = with(plain(), { { W::FilterBase + VF::Cutoff, 800.0f } });      // (the PWM table: POS is heard)
            for (int d : { W::dests().phase, (int) WavetableVoice::LfoPosition, (int) WavetableVoice::LfoCutoff })
            {
                // XFLT with its filters open is the input as it is: the same samples as XIN.
                const Stereo filteredOpen = play(cells(with(sinePatch, slot(0, published(WavetableVoice::PubFiltered), d, 0.6f))), g);
                const Stereo raw = play(cells(with(sinePatch, slot(0, source(mod::Src::Xin), d, 0.6f))), g);
                double apart = 0.0;
                for (size_t i = 0; i < raw.l.size(); ++i)
                    apart = std::max(apart, (double) std::abs(filteredOpen.l[i] - raw.l[i]));
                expect(apart < 1.0e-6, "XFLT, open, on destination " + juce::String(d) + " is XIN: " + juce::String(apart, 9));
                const Stereo own = play(cells(with(sinePatch, slot(0, published(WavetableVoice::PubRect), d, 0.6f))), g);
                const Stereo reference = play(cells(with(sinePatch, slot(0, source(mod::Src::Xin), d, 0.6f))), rectified);
                const Stereo plainSound = play(cells(sinePatch), g);
                expect(own.l != plainSound.l, "XRCT moves destination " + juce::String(d));
                double worst = 0.0;
                for (size_t i = 0; i < own.l.size(); ++i)
                    worst = std::max(worst, (double) std::abs(own.l[i] - reference.l[i]));
                expect(worst < 1.0e-6, "XRCT on destination " + juce::String(d) + " is XIN of |input|, sample for sample: " + juce::String(worst, 9));
            }
            // Nothing connected: XRCT is silence, and the sound the plain one.
            Gesture none = g;
            none.input = nullptr;
            const Stereo quiet = play(cells(with(sinePatch, slot(0, published(WavetableVoice::PubRect), W::dests().phase, 0.6f))), none);
            const Stereo plainSound = play(cells(sinePatch), none);
            expect(quiet.l == plainSound.l, "no input: no modulation");
            // XFLT's low-pass is two poles: a 3 kHz input through LPF 800 moves the phase less than
            // through the router's own one-pole XLP (FLT = LP), and much less than unfiltered.
            Gesture high = g;
            high.input = sine(3000.0, 0.8f);
            const Patch pure = with(plain(), { { W::TableSelect, 3.0f }, { W::AmpSustain, 1.0f } });
            auto sidebands = [&](int src, float lpf)
            {
                const Stereo y = play(cells(with(with(pure, slot(0, src, W::dests().phase, 0.8f)), { { R::xinId(R::XinLowPass), lpf } })), high);
                // What is not the carrier (A3 at 220 Hz), against the carrier.
                const double carrier = std::sqrt(fastmeasure::goertzel(y.l, (size_t) (0.1 * SR), (size_t) (0.3 * SR), 220.0, SR));
                const double all = rmsOf(y.l, (size_t) (0.1 * SR), (size_t) (0.4 * SR));
                return std::make_pair(carrier, all);
            };
            const auto open = sidebands(source(mod::Src::Xin), 800.0f), onePole = sidebands(source(mod::Src::XinLow), 800.0f),
                       twoPole = sidebands(published(WavetableVoice::PubFiltered), 800.0f);
            logMessage("[U4-WT] 3 kHz on PM through LPF 800: carrier (its Goertzel power) against the level, unfiltered " + juce::String(open.first / open.second, 3) + ", XLP (one pole) "
                       + juce::String(onePole.first / onePole.second, 3) + ", XFLT (two poles) " + juce::String(twoPole.first / twoPole.second, 3));
            expect(twoPole.first / twoPole.second > onePole.first / onePole.second && onePole.first / onePole.second > open.first / open.second,
                   "the carrier keeps more of the level the steeper the input is filtered");
        }

        beginTest("ETIM and FTIM: envelope time as a destination");
        {
            Gesture g;
            g.seconds = 1.0;
            g.notes = { { 57, 0.9f, 0.0, 0.5 } };
            const Patch slow = with(plain(), { { W::TableSelect, 3.0f }, { W::AmpAttack, 0.2f }, { W::AmpSustain, 1.0f }, { W::AmpRelease, 0.2f } });
            const int one = source(mod::Src::One);
            const double set = riseTime(play(cells(slow), g).l, 0.9);
            const double shorter = riseTime(play(cells(with(slow, slot(0, one, W::dests().envTime, -1.0f))), g).l, 0.9);
            const double longer = riseTime(play(cells(with(slow, slot(0, one, W::dests().envTime, 1.0f / 3.0f))), g).l, 0.9);
            logMessage("[U4-WT] ETIM: attack to 90 % in " + juce::String(set, 3) + " s as set, " + juce::String(shorter, 3) + " s at -100, "
                       + juce::String(longer, 3) + " s at +33");
            expectWithinAbsoluteError(shorter / set, 0.125, 0.06, "ONE > ETIM at -100: an eighth of the time");
            expectWithinAbsoluteError(longer / set, 2.0, 0.25, "ONE > ETIM at +33: one octave, twice the time");
            // ... and the chain's stages with it.
            const Patch chain = with(slow, { { W::EnvChainStages, 2.0f }, { W::EnvChainMode, 1.0f } });
            const double chainSet = riseTime(play(cells(chain), g).l, 0.9);
            const double chainShort = riseTime(play(cells(with(chain, slot(0, one, W::dests().envTime, -1.0f))), g).l, 0.9);
            expect(chainShort < 0.3 * chainSet, "the chain's stages take ETIM: " + juce::String(chainSet, 3) + " -> " + juce::String(chainShort, 3));
            // With EVAR: the note's factor times the slot's (the product, not one or the other).
            const Patch evar = with(slow, { { W::SpreadBase + VoiceSpread::EnvTime, 1.0f }, { W::SpreadBase + VoiceSpread::Seed, 2.0f } });
            const double varied = riseTime(play(cells(evar), g).l, 0.9);
            const double variedShort = riseTime(play(cells(with(evar, slot(0, one, W::dests().envTime, -1.0f / 3.0f))), g).l, 0.9);
            expect(std::abs(varied / set - 1.0) > 0.05, "EVAR alone moves the time: x" + juce::String(varied / set, 3));
            expectWithinAbsoluteError(variedShort / varied, 0.5, 0.08, "EVAR x ETIM");
            // FTIM: the filter envelope's decay (a saw through a low-pass that the envelope opens).
            const Patch swept = { { W::TableSelect, 0.0f }, { W::Position, 1.0f }, { W::AmpSustain, 1.0f }, { W::FilterBase + VF::Cutoff, 200.0f },
                                  { W::FilterBase + VF::EnvAmount, 0.8f }, { W::FilterBase + VF::Decay, 0.4f }, { W::FilterBase + VF::Sustain, 0.0f } };
            const double brightSet = brightness(play(cells(swept), g).l, 0.1, 0.15);
            const double brightShort = brightness(play(cells(with(swept, slot(0, one, W::dests().filterTime, -1.0f))), g).l, 0.1, 0.15);
            expect(brightShort < 0.5 * brightSet, "ONE > FTIM at -100: the filter has closed by 0.1 s (" + juce::String(brightSet, 4) + " -> " + juce::String(brightShort, 4) + ")");
            // Let go under a held note: the time is the set one again.
            W e;
            cells(with(slow, slot(0, one, W::dests().envTime, -1.0f)))(e);
            Gesture two;
            two.seconds = 0.4;
            two.notes = { { 57, 0.9f, 0.0, 0.3 } };
            render(e, two);
            e.setParameter(ModSlotParam::id(0, ModSlotParam::AMT), 0.0f);
            e.allNotesOff();
            const double back = riseTime(render(e, g).l, 0.9);
            expectWithinAbsoluteError(back / set, 1.0, 0.06, "the slot gone: the attack as set");
        }

        beginTest("POS: an LFO stops at the ends of the table");
        {
            W e;
            cells(with(plain(), with({ { W::Position, 0.9f }, { W::AmpSustain, 1.0f } }, lfo(0, 5.0f, 1.0f, WavetableVoice::LfoPosition))))(e);
            AftertouchRouter off;
            off.clear();
            e.setAftertouchRouter(&off);
            e.prepareToPlay(SR, 64);
            e.noteOn(60, 0.9f, -1);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            float lowest = 1.0f, highest = 0.0f;
            int atTop = 0;
            for (int b = 0; b < 400; ++b)
            {
                buf.clear();
                e.processBlock(buf, midi);
                const float p = e.getVoice(0).getReadPosition();
                lowest = std::min(lowest, p);
                highest = std::max(highest, p);
                atTop += p == 1.0f ? 1 : 0;
            }
            e.setAftertouchRouter(nullptr);
            expect(highest == 1.0f && atTop > 100, "the LFO holds the position at the end for the half it would be beyond it: " + juce::String(atTop) + " of 400 blocks");
            expect(lowest < 0.05f, "and brings it down to " + juce::String(lowest, 3));
        }

        beginTest("A voice nothing moves is the plain loop; a slot that goes leaves the engine as it was");
        {
            // (The MATCH rows above are the first half. Here: every kind of slot set and taken
            // away again under silence, then the same note as an engine that never had them.)
            Gesture g;
            g.seconds = 0.5;
            g.notes = { { 57, 0.9f, 0.0, 0.4 } };
            g.input = sine(300.0, 0.7f);
            W touched, fresh;
            cells(rich())(touched);
            cells(rich())(fresh);
            const Patch on = with(with(with(with(lfo(0, 5.0f, 0.6f, dest("ATK")), lfo(1, 3.0f, 0.5f, dest("BAL"))),
                                            slot(0, source(mod::Src::Xin), W::dests().phase, 0.7f)),
                                       slot(1, source(mod::Src::One), W::dests().envTime, -0.5f)),
                                  slot(2, published(WavetableVoice::PubScan), dest("FFM"), 0.5f));
            cells(on)(touched);
            render(touched, g);
            Patch offPatch = on;
            for (auto& c : offPatch)
                c.second = 0.0f;
            cells(offPatch)(touched);
            touched.setParameter(R::lfoId(0, R::Rate), 2.0f);
            touched.setParameter(R::lfoId(1, R::Rate), 2.0f);
            touched.allNotesOff();
            Gesture quiet;
            quiet.seconds = 0.3;
            quiet.notes = {};
            render(touched, quiet);
            render(fresh, g);
            fresh.allNotesOff();
            render(fresh, quiet);
            // (The scan, the motion and the vibrato have run for as long in both.)
            const Stereo a = render(touched, g), b = render(fresh, g);
            double worst = 0.0;
            for (size_t i = 0; i < a.l.size(); ++i)
                worst = std::max(worst, (double) std::abs(a.l[i] - b.l[i]));
            expect(worst < 1.0e-5, "after the slots have gone the note is the plain one: " + juce::String(worst, 8));
        }
    }

    // =============================================================================================
    // The review's additions (2026-11-22): what the suite did not hold the port to.
    //
    // An engine played a block at a time at any sample rate, with a hook before each block.
    struct Live
    {
        Live(double rate, int blockSize) : sr(rate), block(blockSize), buf(2, blockSize), in((size_t) blockSize, 0.0f)
        {
            routes.clear();
            e.setAftertouchRouter(&routes);
            e.prepareToPlay(sr, block);
        }
        ~Live() { e.setAftertouchRouter(nullptr); }
        void set(const Patch& patch) { cells(patch)(e); }
        // seconds of sound (left channel); each(sampleIndex) runs before every sample's block when block is 1
        std::vector<float> run(double seconds, const std::function<void(std::vector<float>&)>& afterBlock = nullptr)
        {
            std::vector<float> out;
            const long long total = (long long) (seconds * sr);
            for (long long done = 0; done < total; done += block)
            {
                if (input && frames > 0)
                {
                    for (int i = 0; i < block; ++i)
                        in[(size_t) i] = input(clock + i);
                    e.setAudioInput({ in.data(), in.data(), std::min(frames, block), 0.5f });
                }
                else
                    e.setAudioInput({});
                buf.clear();
                e.processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back(buf.getSample(0, i));
                clock += block;
                if (afterBlock)
                    afterBlock(out);
            }
            return out;
        }
        double sr;
        int block;
        W e;
        AftertouchRouter routes;
        juce::AudioBuffer<float> buf;
        juce::MidiBuffer midi;
        std::vector<float> in;
        std::function<float(long long)> input;
        int frames = 1 << 30;       // how much of each block the input covers
        long long clock = 0;
    };

    // The instants (in samples, interpolated) where x rises through zero.
    static std::vector<double> risingCrossings(const std::vector<float>& x, size_t from = 1)
    {
        std::vector<double> out;
        for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                out.push_back((double) (i - 1) + (double) (-x[i - 1]) / (double) (x[i] - x[i - 1]));
        return out;
    }
    static float largestStep(const std::vector<float>& x, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i)
            m = std::max(m, std::abs(x[i] - x[i - 1]));
        return m;
    }

    template <typename Engine>
    static Stereo renderAt(Engine& e, const Gesture& g, double sr)
    {
        AftertouchRouter off;
        off.clear();
        e.setAftertouchRouter(&off);
        e.prepareToPlay(sr, g.block);
        const int total = (int) (g.seconds * sr);
        Stereo out;
        juce::AudioBuffer<float> buf(2, g.block);
        juce::MidiBuffer midi;
        std::vector<float> in((size_t) g.block, 0.0f);
        long long clock = 0;
        for (int b = 0; b * g.block < total; ++b)
        {
            const double t0 = (double) b * g.block / sr, t1 = (double) (b + 1) * g.block / sr;
            for (auto& n : g.notes)
            {
                if (n.off >= t0 && n.off < t1) e.noteOff(n.note, -1);
                if (n.on >= t0 && n.on < t1)   e.noteOn(n.note, n.velocity, -1);
            }
            if (g.strip)    e.setModWheel(g.strip(t0));
            if (g.input)
            {
                for (int i = 0; i < g.block; ++i)
                    in[(size_t) i] = g.input(clock + i);
                e.setAudioInput({ in.data(), in.data(), g.block, 0.5f });
            }
            else
                e.setAudioInput({});
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < g.block; ++i)
            {
                out.l.push_back(buf.getSample(0, i));
                out.r.push_back(buf.getSample(1, i));
            }
            clock += g.block;
        }
        e.setAftertouchRouter(nullptr);
        return out;
    }

    void review()
    {
        beginTest("Review: old against new at 44.1 and 96 kHz (what is the same samples at 48 kHz is the same samples there)");
        {
            struct Case { const char* name; Patch sound; std::vector<std::pair<int, float>> xin; double inputHz; bool same; };
            const Patch sinePatch = with(plain(), { { W::TableSelect, 3.0f } });
            const Patch moving = with(plain(), { { W::InterpMode, 3.0f }, { W::ScanMode, 3.0f }, { W::ScanTime, 0.3f }, { W::MorphSpeed, 1.5f }, { W::EnvToPosition, 0.2f },
                                                 { W::FilterBase + VF::Cutoff, 1500.0f }, { W::FilterBase + VF::EnvAmount, 0.3f }, { dualId(VF::Routing), 2.0f },
                                                 { dualId(VF::FilterFM), 0.2f } });
            const std::vector<Case> cases = {
                { "plain (PWM table)", plain(), {}, 0.0, true },
                { "INTP VINT, scan PING, motion, ENVP, both filters, FFM", moving, {}, 0.0, true },
                { "XIN PM 100, PWM table", plain(), { { X::Mode, 1 }, { X::Depth, 1.0f } }, 300.0, true },
                { "XIN POS 100 at POS 50 (folds)", with(plain(), { { W::Position, 0.5f } }), { { X::Mode, 2 }, { X::Depth, 1.0f } }, 300.0, true },
                { "XIN PM 60 XLP 800 (input at 3 kHz): XFLT", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::LowPass, 800.0f } }, 3000.0, true },
                { "XIN PM 60 XHP 1000 (input at 300 Hz): XFLT", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::HighPass, 1000.0f } }, 300.0, true },
                { "XIN PM 60 XLP 2000 XHP 200 XRCT 100: XRCT after both filters", sinePatch, { { X::Mode, 1 }, { X::Depth, 0.6f }, { X::LowPass, 2000.0f }, { X::HighPass, 200.0f }, { X::Rectify, 1.0f } }, 500.0, true },
                { "LFO1 > POS, LFO2 > CUT", with(with(rich(), lfo(0, 5.0f, 0.5f, WavetableVoice::LfoPosition)), lfo(1, 0.7f, 0.4f, WavetableVoice::LfoCutoff)), {}, 0.0, false } };
            for (double sr : { 44100.0, 96000.0 })
                for (auto& c : cases)
                {
                    Gesture g;
                    g.seconds = 0.8;
                    g.block = sr > 50000.0 ? 100 : 64;      // (96 kHz: a host block that is no whole number of sub-blocks)
                    g.notes = { { 57, 0.9f, 0.0, 0.5 }, { 64, 0.5f, 0.0, 0.5 } };
                    if (c.inputHz > 0.0)
                        g.input = [hz = c.inputHz, sr](long long n) { return 0.7f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * hz * (double) n / sr); };
                    O o;
                    W n;
                    cells(c.sound)(o);
                    cells(c.sound)(n);
                    X cellsOfOld;
                    for (auto& x : c.xin)
                    {
                        o.setParameter(x.first, x.second);
                        cellsOfOld.set(n, x.first, x.second);
                    }
                    const Stereo a = renderAt(o, g, sr), b = renderAt(n, g, sr);
                    const Verdict v = compare(a, b);
                    const juce::String name = juce::String(c.name) + " at " + juce::String(sr / 1000.0, 1) + " kHz";
                    logMessage("[U4-WT-R] " + name + ": " + v.text());
                    expect(rmsOf(a.l) > 1.0e-4, name + ": the old engine sounds");
                    expect(c.same ? v.match() : ! v.lost(), name + ": " + v.text());
                }
        }

        beginTest("Review: the seven presets said to be CLOSE for one cell each are the same samples without that cell");
        {
            const juce::StringArray withSub { "Digital Choir", "Frozen Sweep", "Vowel Lead", "Fold Bass", "Reso Sweep Bass" };
            int seen = 0;
            for (auto& p : wavetablePresets())
            {
                if (p.name != "Drawbar Organ" && p.name != "I You We Yeah" && ! withSub.contains(p.name))
                    continue;
                ++seen;
                Gesture g;
                g.seconds = 1.0;
                g.notes = { { 57, 0.9f, 0.0, 0.7 }, { 64, 0.6f, 0.1, 0.7 } };
                const Patch without = withSub.contains(p.name) ? Patch { { W::SubLevel, 0.0f } }
                                    : (p.name == "Drawbar Organ" ? Patch { { W::Vibrato, 0.0f } }
                                                                 : Patch { { R::lfoId(0, R::Depth), 0.0f }, { R::lfoId(1, R::Depth), 0.0f } });
                O o;
                W n;
                presetSetup(p)(o);
                presetSetup(p)(n);
                cells(without)(o);
                cells(without)(n);
                const Verdict v = compare(render(o, g), render(n, g));
                logMessage("[U4-WT-R] " + p.name + " without " + (withSub.contains(p.name) ? "SUB" : (p.name == "Drawbar Organ" ? "VIBR" : "its two LFOs")) + ": " + v.text());
                expect(v.match(), p.name + ": " + v.text());
            }
            expectEquals(seen, 7, "the seven presets");
        }

        beginTest("Review: the VIBR cell works with no slot reading VIB (strip down, WVIB 0), from the note's start");
        {
            // VIBR at 1: half a semitone each way at the STRIP page's VRAT, with nothing else routed.
            for (int wvib = 0; wvib < 2; ++wvib)
            {
                Live l(SR, 64);
                l.set({ { W::TableSelect, 3.0f }, { W::AmpAttack, 0.001f }, { W::AmpSustain, 1.0f }, { W::Vibrato, 1.0f }, { R::STRIP_VIBRATO_RATE, 8.0f } });
                if (wvib == 0)
                    l.set({ { R::STRIP_VIBRATO, 0.0f } });
                l.e.noteOn(69, 1.0f, -1);
                const auto y = l.run(0.5);
                const auto z = risingCrossings(y, 480);
                double shortest = 1.0e9, longest = 0.0;
                for (size_t i = 1; i < z.size(); ++i)
                {
                    shortest = std::min(shortest, z[i] - z[i - 1]);
                    longest = std::max(longest, z[i] - z[i - 1]);
                }
                const double swing = longest / shortest;        // a semitone between the ends: 1.0595
                expect(z.size() > 150, "the note sounds");
                expect(swing > 1.045 && swing < 1.075, juce::String(wvib == 0 ? "WVIB 0" : "WVIB as it is") + ": the pitch swings a semitone in all, x" + juce::String(swing, 4));
                // ... and at VRAT: the period is longest once every 1 / 8 s.
                std::vector<double> peaks;
                for (size_t i = 2; i + 1 < z.size(); ++i)
                    if (z[i] - z[i - 1] > 0.99 * longest && (peaks.empty() || z[i] - peaks.back() > 0.05 * SR))
                        peaks.push_back(z[i]);
                expect(peaks.size() >= 3 && peaks.size() <= 5, "about four slow moments in 0.5 s at VRAT 8: " + juce::String((int) peaks.size()));
            }
        }

        beginTest("Review: an LFO on PM is a movement of the pitch, two cycles each way at full depth, the amount squared");
        {
            // A triangle at 10 Hz: the phase runs 4 x depth^2 x 2 cycles in 0.1 s up and down again,
            // so the 440 Hz sine sounds at 440 +- 80 x depth^2 Hz.
            for (float depth : { 1.0f, 0.5f })
            {
                Live l(SR, 64);
                l.set(with({ { W::TableSelect, 3.0f }, { W::AmpAttack, 0.001f }, { W::AmpSustain, 1.0f } },
                           lfo(0, 10.0f, depth, W::dests().phase, (int) LFO::Shape::Triangle)));
                l.e.noteOn(69, 1.0f, -1);
                const auto z = risingCrossings(l.run(0.5), 2400);
                // (The mean of four periods: away from the corners of the triangle.)
                double shortest = 1.0e9, longest = 0.0;
                for (size_t i = 4; i < z.size(); ++i)
                {
                    shortest = std::min(shortest, (z[i] - z[i - 4]) / 4.0);
                    longest = std::max(longest, (z[i] - z[i - 4]) / 4.0);
                }
                const double high = SR / shortest, low = SR / longest, swing = 80.0 * depth * depth;
                expectWithinAbsoluteError(high, 440.0 + swing, 0.06 * swing + 1.0, "depth " + juce::String(depth) + ": the pitch at its highest");
                expectWithinAbsoluteError(low, 440.0 - swing, 0.06 * swing + 1.0, "depth " + juce::String(depth) + ": the pitch at its lowest");
            }
        }

        beginTest("Review: an LFO taken off PM under a held note is no blip of the pitch (whole cycles are let go at once, the rest in 4 ms)");
        {
            // A square at 0.02 Hz holds the phase 2 x depth^2 cycles off. Taken away, the router
            // carries that to nothing over 4 ms: at full depth the wave ran 500 Hz faster for 4 ms.
            struct Case { float depth; int how; };     // how: 0 DEST to OFF, 1 DPTH to 0, 2 DEST to another cell
            for (auto c : { Case { 1.0f, 0 }, Case { 1.0f, 1 }, Case { 0.9f, 0 }, Case { 0.6f, 0 }, Case { 0.75f, 2 }, Case { 0.5f, 1 } })
            {
                Live l(SR, 64);
                l.set(with({ { W::TableSelect, 3.0f }, { W::AmpAttack, 0.001f }, { W::AmpSustain, 1.0f } },
                           lfo(0, 0.02f, c.depth, W::dests().phase, (int) LFO::Shape::Square)));
                l.e.noteOn(57, 1.0f, -1);
                auto y = l.run(0.2);
                const size_t at = y.size();
                if (c.how == 0)      l.set({ { R::lfoId(0, R::DestCell), 0.0f } });
                else if (c.how == 1) l.set({ { R::lfoId(0, R::Depth), 0.0f } });
                else                 l.set({ { R::lfoId(0, R::DestCell), (float) dest("MDPT") } });
                const auto after = l.run(0.1);
                y.insert(y.end(), after.begin(), after.end());
                const auto z = risingCrossings(y, (size_t) (0.05 * SR));
                const double period = SR / 220.0, held = 2.0 * c.depth * c.depth;
                // How far the wave ran ahead of (or fell behind) where it was going, in cycles, and
                // the period that differs most from the note's.
                double worst = 0.0, last = 0.0, lastBefore = 0.0;
                size_t countAfter = 0;
                for (size_t i = 1; i < z.size(); ++i)
                {
                    worst = std::max(worst, std::abs((z[i] - z[i - 1]) / period - 1.0));
                    if (z[i] < (double) at) lastBefore = z[i]; else { last = z[i]; ++countAfter; }
                }
                const double cycles = (last - lastBefore) / period;
                const double slip = std::abs(cycles - std::round(cycles));
                const double rest = std::abs(held - std::round(held));
                const juce::String name = "depth " + juce::String(c.depth) + (c.how == 0 ? ", DEST to OFF" : (c.how == 1 ? ", DPTH to 0" : ", DEST to MDPT"));
                logMessage("[U4-WT-R] PM let go, " + name + ": " + juce::String(held, 2) + " cycles held, the period furthest off by "
                           + juce::String(100.0 * worst, 1) + " %, " + juce::String(slip, 3) + " cycles slipped");
                expect(countAfter > 15, name + ": the note goes on");
                expectWithinAbsoluteError(slip, rest > 0.49 ? 0.5 : rest, 0.02, name + ": only the part of a cycle is let go");
                // (Half a cycle in 4 ms is 125 Hz off for 4 ms: the one period it falls in is at most
                // 0.5 of a period longer or shorter. Two cycles in 4 ms was 2 periods in under one.)
                expect(worst <= rest + 0.03, name + ": no period is further off than the part of a cycle let go (" + juce::String(worst, 3) + ")");
                // (The steps of a sine grow with its pitch: 220 Hz + 125 Hz is 1.57 times the note's. Two
                // cycles in 4 ms was 3.3 times.)
                expect(largestStep(y, at - 4, at + 960) <= 1.65f * largestStep(y, at - 4800, at - 4) + 1.0e-4f,
                       name + ": and no step: " + juce::String(largestStep(y, at - 4, at + 960), 4) + " against " + juce::String(largestStep(y, at - 4800, at - 4), 4));
            }
        }

        beginTest("Review: an audio-rate slot that comes to POS or leaves it under an LFO on POS does not step the position");
        {
            // With such a slot the whole of POS's modulation is taken after the glide and mirrored
            // at the ends; without it, inside the sum that stops there. POS 30 + LFO 30: the same
            // place either way, but the glide held the LFO's share once more. POS 90 + LFO 30: the
            // end of the table one way, 80 the other.
            for (float start : { 0.3f, 0.9f })
            {
                Live l(SR, 1);
                l.input = [](long long n) { return 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 300.0 * (double) n / SR); };
                l.set(with({ { W::TableSelect, 4.0f }, { W::Position, start }, { W::AmpSustain, 1.0f } },
                           lfo(0, 0.02f, 0.3f, WavetableVoice::LfoPosition, (int) LFO::Shape::Square)));
                l.e.noteOn(57, 1.0f, -1);
                l.run(0.1);
                const float stopped = juce::jlimit(0.0f, 1.0f, start + 0.3f), mirrored = WavetableVoice::foldPosition(start + 0.3f);
                expectWithinAbsoluteError(l.e.getVoice(0).getReadPosition(), stopped, 1.0e-3f, "the LFO alone: POS + 30, stopped at the end");
                float before = l.e.getVoice(0).getReadPosition(), worst = 0.0f;
                const auto watch = [&](std::vector<float>&)
                {
                    const float now = l.e.getVoice(0).getReadPosition();
                    worst = std::max(worst, std::abs(now - before));
                    before = now;
                };
                // (An amount of 0.2 %: the slot is there, the input moves the position by a thousandth.)
                l.set(slot(0, source(mod::Src::Xin), WavetableVoice::LfoPosition, 0.002f));
                l.run(0.05, watch);
                const juce::String name = "POS " + juce::String(start);
                expect(worst < 0.02f, name + ", the slot comes: the largest move of the position in a sample is " + juce::String(worst, 4) + " (the glide: 0.007)");
                expectWithinAbsoluteError(l.e.getVoice(0).getReadPosition(), mirrored, 3.0e-3f, name + ": with the slot, POS + 30 mirrored at the end");
                worst = 0.0f;
                l.set(slot(0, 0, 0, 0.0f));
                l.run(0.05, watch);
                expect(worst < 0.02f, name + ", the slot goes: " + juce::String(worst, 4));
                expectWithinAbsoluteError(l.e.getVoice(0).getReadPosition(), stopped, 1.0e-3f, name + ": the slot gone, as before it");
            }
        }

        beginTest("Review: MEASURED, not judged - an LFO that pushes POS past the end of the table while the input is on POS too (the LFO's share is mirrored now, it stopped before)");
        {
            // The one case "XIN Wavetable" leaves out of "Off is the same samples". The router hands
            // the voice one sum for POS, so with an audio-rate slot there the LFO's share cannot
            // stop at the end by itself. How far the sound is from what it was:
            // (-0.3: a connected input that is silent, as "left = -right" sums to: the slot moves
            // nothing, yet the LFO's share is taken the other way.)
            for (float xinDepth : { 0.0f, 0.3f, -0.3f })
            {
                Gesture g;
                g.seconds = 1.0;
                g.notes = { { 57, 0.9f, 0.0, 0.8 } };
                g.input = xinDepth < 0.0f ? std::function<float(long long)>([](long long) { return 0.0f; }) : sine(300.0, 0.7f);
                const Patch sound = with(with(plain(), { { W::Position, 0.8f }, { W::AmpSustain, 1.0f } }), lfo(0, 3.0f, 0.5f, WavetableVoice::LfoPosition));
                O o;
                W n;
                cells(sound)(o);
                cells(sound)(n);
                X cellsOfOld;
                for (auto& x : std::vector<std::pair<int, float>> { { X::Mode, 2.0f }, { X::Depth, std::abs(xinDepth) } })
                {
                    o.setParameter(x.first, x.second);
                    cellsOfOld.set(n, x.first, x.second);
                }
                const Verdict v = compare(render(o, g), render(n, g));
                logMessage("[U4-WT-R] POS 80, LFO1 > POS 50, XIN POS " + juce::String((int) (100.0f * std::abs(xinDepth))) + (xinDepth < 0.0f ? " (a silent input)" : "") + ": " + v.text());
                // (U4-1: a slot whose input is silent changes nothing - it was LOST, envelope 6.93 dB.)
                if (xinDepth < 0.0f)
                    expect(! v.lost() && v.envelope < 0.1, "a silent input on POS: as the LFO alone: " + v.text());
            }
        }

        beginTest("Review: the source SCAN is 0 once MODE is turned OFF under a held note");
        {
            Live l(SR, 64);
            l.set(with(with(plain(), { { W::Position, 0.0f }, { W::ScanMode, (float) (int) WavetableScanMode::Run }, { W::ScanTime, 0.2f }, { W::AmpSustain, 1.0f } }),
                       slot(0, published(WavetableVoice::PubScan), WavetableVoice::LfoPan, 1.0f)));
            l.e.noteOn(60, 0.9f, -1);
            l.run(0.4);
            juce::AudioBuffer<float>& b = l.buf;
            expect(b.getRMSLevel(0, 0, 64) < 0.05f * b.getRMSLevel(1, 0, 64), "the scan at its end: the voice is on the right");
            l.set({ { W::ScanMode, 0.0f } });
            l.run(0.2);
            expectWithinAbsoluteError(b.getRMSLevel(0, 0, 64) / b.getRMSLevel(1, 0, 64), 1.0f, 0.02f, "MODE OFF: the voice is back in the middle");
        }

        beginTest("Review: the old XIN cells 65-72 set on the engine (a sound saved before U4) do nothing and break nothing");
        {
            Gesture g;
            g.seconds = 0.4;
            g.input = sine(300.0, 0.7f);
            const Stereo reference = play(cells(rich()), g);
            for (float value : { 1.0f, 2.0f, 0.5f, 20000.0f, -1.0f })
            {
                W e;
                cells(rich())(e);
                for (int id = X::Mode; id <= X::HighPass; ++id)
                {
                    expect(! mod::Router::owns(id) && e.isRebuildParam(id) == false);
                    e.setParameter(id, id == X::Mode ? std::min(value, 2.0f) : value);
                }
                const Stereo y = render(e, g);
                expect(y.l == reference.l && y.r == reference.r, "cells 65-72 at " + juce::String(value) + ": the same samples as without them");
                for (int s = 0; s < mod::Router::NUM_FREE; ++s)
                    expectEquals(e.getParameter(ModSlotParam::id(s, ModSlotParam::SRC)), 0.0f, "no slot was set");
            }
        }

        beginTest("Review: XRCT and XFLT are no sub-block late at host blocks of 100, 37, 1 and 512 (no delay that changes with the block)");
        {
            // The FM port's taps were handed to the router a sub-block late, so their delay moved with
            // the host's block. Here: XFLT with its filters open must be XIN, and XRCT must be XIN of
            // |input|, sample for sample, at every block size; a 300 Hz input on PM, so a delay of even
            // one sample would show as a difference far above 1e-6.
            const Patch pure = with(plain(), { { W::TableSelect, 3.0f }, { W::AmpSustain, 1.0f } });
            for (int block : { 100, 37, 1, 512 })
                for (int d : { W::dests().phase, (int) WavetableVoice::LfoPosition })
                {
                    Gesture g;
                    g.seconds = 0.3;
                    g.block = block;
                    g.notes = { { 57, 0.9f, 0.0, 0.25 } };
                    g.input = sine(300.0, 0.8f);
                    Gesture rectified = g;
                    rectified.input = [in = g.input](long long n) { return std::abs(in(n)); };
                    const Patch sound = d == W::dests().phase ? pure : plain();
                    const Stereo flt = play(cells(with(sound, slot(0, published(WavetableVoice::PubFiltered), d, 0.8f))), g);
                    const Stereo xin = play(cells(with(sound, slot(0, source(mod::Src::Xin), d, 0.8f))), g);
                    const Stereo rct = play(cells(with(sound, slot(0, published(WavetableVoice::PubRect), d, 0.8f))), g);
                    const Stereo xinRect = play(cells(with(sound, slot(0, source(mod::Src::Xin), d, 0.8f))), rectified);
                    double fltApart = 0.0, rctApart = 0.0;
                    for (size_t i = 0; i < xin.l.size(); ++i)
                    {
                        fltApart = std::max(fltApart, (double) std::abs(flt.l[i] - xin.l[i]));
                        rctApart = std::max(rctApart, (double) std::abs(rct.l[i] - xinRect.l[i]));
                    }
                    const juce::String name = "blocks of " + juce::String(block) + ", destination " + juce::String(d);
                    logMessage("[U4-WT-R] " + name + ": XFLT against XIN " + juce::String(fltApart, 9) + ", XRCT against XIN of |input| " + juce::String(rctApart, 9));
                    expect(rmsOf(xin.l) > 1.0e-3, name + ": it sounds");
                    expect(fltApart < 1.0e-6, name + ": XFLT (open) is XIN: " + juce::String(fltApart, 9));
                    expect(rctApart < 1.0e-6, name + ": XRCT is XIN of |input|: " + juce::String(rctApart, 9));
                }
        }

        beginTest("Review: MEASURED, not judged - an LFO aimed at PM under a held note steps the wave (LEFT: the router leads a new destination in over one sub-block; a phase needs what PM LET GO does)");
        {
            // DEST to OFF under a held note, and back to PM one block (1.3 ms) later: the voice's own
            // rest (0.38 of a cycle at depth 0.9) gives way to the router's value; and the same LFO
            // aimed at PM with nothing let go before. Both step: the router leads a destination that
            // comes in from its set value over one sub-block, which for a phase of 1.6 cycles is
            // the wave run through 1.6 cycles in 0.7 ms. Measured 0.0975 / 0.1339 against 0.0161 of
            // the steady note (2026-11-22); a fix is the arrival twin of PM LET GO (render()).
            Live l(SR, 64);
            l.set(with({ { W::TableSelect, 3.0f }, { W::AmpAttack, 0.001f }, { W::AmpSustain, 1.0f } },
                       lfo(0, 0.02f, 0.9f, W::dests().phase, (int) LFO::Shape::Square)));
            l.e.noteOn(57, 1.0f, -1);
            auto y = l.run(0.2);
            const size_t at = y.size();
            l.set({ { R::lfoId(0, R::DestCell), 0.0f } });
            const auto gone = l.run(64.0 / SR);
            y.insert(y.end(), gone.begin(), gone.end());
            l.set({ { R::lfoId(0, R::DestCell), (float) W::dests().phase } });
            const auto back = l.run(0.1);
            y.insert(y.end(), back.begin(), back.end());
            const float steady = largestStep(y, at - 4800, at - 4), then = largestStep(y, at - 4, at + 960);
            // (For comparison: the same LFO aimed at PM under a held note with nothing let go before.)
            Live f(SR, 64);
            f.set(with({ { W::TableSelect, 3.0f }, { W::AmpAttack, 0.001f }, { W::AmpSustain, 1.0f } },
                       lfo(0, 0.02f, 0.9f, 0, (int) LFO::Shape::Square)));
            f.e.noteOn(57, 1.0f, -1);
            auto z = f.run(0.2 + 64.0 / SR);
            const size_t fat = z.size();
            f.set({ { R::lfoId(0, R::DestCell), (float) W::dests().phase } });
            const auto fb = f.run(0.1);
            z.insert(z.end(), fb.begin(), fb.end());
            logMessage("[U4-WT-R] PM back while its rest goes: largest step " + juce::String(then, 4) + "; an LFO newly on PM: " + juce::String(largestStep(z, fat - 4, fat + 960), 4)
                       + "; steady " + juce::String(steady, 4));
            expect(std::isfinite(then) && steady > 0.0f);
            // (U4-1: the router brings a phase in without a step - it was 0.0975 and 0.1339.)
            expect(then <= 1.7f * steady && largestStep(z, fat - 4, fat + 960) <= 1.7f * steady, "no step where PM comes");
        }

        beginTest("Review: XFLT's filters start afresh when a slot reads it again (nothing of what they held long ago)");
        {
            // A slot on XFLT (LPF 800) comes, goes, and comes again 0.2 s later; against an engine
            // where it only came the second time, the sound after that is the same (the voice and
            // the router are in the same place by then; the input's two filters must be too).
            const Patch pure = with(plain(), { { W::TableSelect, 3.0f }, { W::AmpSustain, 1.0f }, { R::xinId(R::XinLowPass), 800.0f } });
            const Patch on = slot(0, published(WavetableVoice::PubFiltered), W::dests().phase, 0.8f), off = slot(0, 0, 0, 0.0f);
            std::vector<float> twice, once;
            for (int which = 0; which < 2; ++which)
            {
                Live l(SR, 64);
                l.input = sine(300.0, 0.8f);
                l.set(with(pure, which == 0 ? on : off));
                l.e.noteOn(57, 1.0f, -1);
                l.run(0.1);
                l.set(off);
                l.run(0.3);
                l.set(on);
                (which == 0 ? twice : once) = l.run(0.05);
            }
            double apart = 0.0;
            for (size_t i = 0; i < once.size(); ++i)
                apart = std::max(apart, (double) std::abs(twice[i] - once[i]));
            expect(rmsOf(once) > 1.0e-3, "it sounds");
            expect(apart < 1.0e-3, "the second time is as the first: " + juce::String(apart, 6));
        }

        beginTest("Review: an input that ends, comes back or is shorter than the block is not jumped to, at any block size and rate");
        {
            // A steady input holds the phase a quarter of a cycle off (PM at 50: 0.5 cycles x 0.5).
            // Dropped, that is a step of the wave; let go in 4 ms it is the pitch 62 Hz off for 4 ms.
            struct Run { double sr; int block; };
            for (auto r : { Run { 48000.0, 1 }, Run { 44100.0, 37 }, Run { 48000.0, 512 }, Run { 96000.0, 100 }, Run { 44100.0, 4096 }, Run { 48000.0, 8192 } })
                for (int what = 0; what < 3; ++what)
                {
                    Live l(r.sr, r.block);
                    l.input = [](long long) { return 0.5f; };
                    l.set(with({ { W::TableSelect, 3.0f }, { W::AmpAttack, 0.001f }, { W::AmpSustain, 1.0f } },
                               slot(0, source(mod::Src::Xin), W::dests().phase, 0.5f)));
                    l.e.noteOn(57, 1.0f, -1);
                    auto y = l.run(0.3);
                    const size_t at = y.size();
                    if (what == 0)      l.frames = 0;                           // taken away
                    else if (what == 1) l.frames = std::max(1, r.block / 2);    // ends half way through every block
                    else                { l.frames = 0; const auto gap = l.run(0.2); y.insert(y.end(), gap.begin(), gap.end()); l.frames = 1 << 30; }   // and back
                    const size_t from = y.size();
                    const auto after = l.run(0.3);
                    y.insert(y.end(), after.begin(), after.end());
                    const float steady = largestStep(y, at / 2, at), then = largestStep(y, from - 1, y.size());
                    bool finite = true;
                    for (float v : y)
                        finite = finite && std::isfinite(v);
                    const juce::String name = juce::String(what == 0 ? "the input goes" : (what == 1 ? "the input covers half of every block" : "the input comes back"))
                                              + ", " + juce::String(r.sr / 1000.0, 1) + " kHz, blocks of " + juce::String(r.block);
                    expect(finite, name);
                    expect(then < 1.6f * steady, name + ": the largest step " + juce::String(then, 4) + " against " + juce::String(steady, 4) + " of the steady note");
                }
        }
    }

    // =============================================================================================
    void summary()
    {
        beginTest("Old against new: the table");
        int match = 0, close = 0, lost = 0;
        for (auto& r : rows)
        {
            match += r.verdict.match() ? 1 : 0;
            close += r.verdict.word == "CLOSE" ? 1 : 0;
            lost += r.verdict.lost() ? 1 : 0;
        }
        logMessage("[U4-WT] " + juce::String((int) rows.size()) + " rows: " + juce::String(match) + " MATCH, " + juce::String(close) + " CLOSE, "
                   + juce::String(lost) + " LOST");
        expect(rows.size() > 150, "the rows: " + juce::String((int) rows.size()));
        expectEquals(lost, 0);
    }

    // =============================================================================================
    // What eight held voices cost, as a share of real time on one core: the best of five runs of
    // ten seconds, old and new turn about.
    template <typename Engine>
    static double cost(const Setup& setup, bool input, float strip, bool pressure)
    {
        constexpr int BLOCK = 512, BLOCKS = 940;
        Engine e(8);
        AftertouchRouter routes;        // (its default: pressure on the volume)
        e.setAftertouchRouter(&routes);
        e.prepareToPlay(SR, BLOCK);
        e.setParameter(W::AmpSustain, 1.0f);
        setup(e);
        e.setModWheel(strip);
        static const int notes[8] = { 48, 52, 55, 59, 62, 65, 69, 72 };
        for (int n : notes)
            e.noteOn(n, 0.8f, -1);
        if (pressure)
            for (int n : notes)
                e.aftertouch(n, 0.6f);
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        std::vector<float> in((size_t) BLOCK);
        double phase = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < BLOCKS; ++b)
        {
            if (input)
            {
                for (auto& s : in)
                {
                    s = 0.7f * (float) std::sin(phase);
                    phase += 2.0 * 3.141592653589793 * 330.0 / SR;
                }
                e.setAudioInput({ in.data(), in.data(), BLOCK, 0.5f });
            }
            buf.clear();
            e.processBlock(buf, midi);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        e.setAftertouchRouter(nullptr);
        return 100.0 * seconds / ((double) BLOCK * BLOCKS / SR);
    }

    void bench()
    {
        beginTest("Cost: eight voices, old against new (% of one core)");
        struct Case { const char* name; Setup oldSetup, newSetup; bool input = false; float strip = 0.0f; bool pressure = false; };
        namespace S = mod::Src;
        const Patch pwm = { { W::TableSelect, 4.0f }, { W::Position, 0.3f } };
        auto same = [](Patch p) { return cells(std::move(p)); };
        auto xinOld = [pwm](int mode) { return [pwm, mode](EngineBase& e) { cells(pwm)(e); e.setParameter(X::Mode, (float) mode); e.setParameter(X::Depth, 0.5f); }; };
        auto xinNew = [pwm](int mode) { return [pwm, mode](EngineBase& e) { cells(pwm)(e); X x; x.set(e, X::Mode, (float) mode); x.set(e, X::Depth, 0.5f); }; };
        std::vector<Case> cases;
        cases.push_back({ "idle: the default sound (saw table)", same({}), same({}) });
        cases.push_back({ "idle: PWM table, spread, sub, motion", same(with(pwm, { { W::UnisonSpread, 0.4f }, { W::SubLevel, 0.4f }, { W::MorphSpeed, 2.0f } })),
                          same(with(pwm, { { W::UnisonSpread, 0.4f }, { W::SubLevel, 0.4f }, { W::MorphSpeed, 2.0f } })) });
        cases.push_back({ "idle: scan PING, filter LP with its envelope", same(with(pwm, { { W::ScanMode, 3.0f }, { W::FilterBase + VF::Cutoff, 1500.0f }, { W::FilterBase + VF::EnvAmount, 0.3f } })),
                          same(with(pwm, { { W::ScanMode, 3.0f }, { W::FilterBase + VF::Cutoff, 1500.0f }, { W::FilterBase + VF::EnvAmount, 0.3f } })) });
        cases.push_back({ "idle: INTP CLSC", same(with(pwm, { { W::InterpMode, 4.0f } })), same(with(pwm, { { W::InterpMode, 4.0f } })) });
        cases.push_back({ "input connected, nothing routed", same(pwm), same(pwm), true });
        cases.push_back({ "pads pressed, default route (VOL)", same(pwm), same(pwm), false, 0.0f, true });
        cases.push_back({ "strip up, default routes (POS, vibrato)", same(pwm), same(pwm), false, 1.0f });
        cases.push_back({ "LFO1 > PTCH", same(with(pwm, lfo(0, 5.0f, 0.3f, WavetableVoice::LfoPitch))), same(with(pwm, lfo(0, 5.0f, 0.3f, WavetableVoice::LfoPitch))) });
        cases.push_back({ "LFO1 > POS, LFO2 > CUT", same(with(with(pwm, lfo(0, 5.0f, 0.3f, WavetableVoice::LfoPosition)), with(lfo(1, 0.7f, 0.4f, WavetableVoice::LfoCutoff), { { W::FilterBase + VF::Cutoff, 2000.0f } }))),
                          same(with(with(pwm, lfo(0, 5.0f, 0.3f, WavetableVoice::LfoPosition)), with(lfo(1, 0.7f, 0.4f, WavetableVoice::LfoCutoff), { { W::FilterBase + VF::Cutoff, 2000.0f } }))) });
        cases.push_back({ "LFO1 > ATK (a page cell)", same(with(pwm, lfo(0, 5.0f, 0.3f, dest("ATK")))), same(with(pwm, lfo(0, 5.0f, 0.3f, dest("ATK")))) });
        const Patch four = with(with(with(with(pwm, slot(0, S::Env, WavetableVoice::LfoAmp, 0.5f)), slot(1, S::Pressure, WavetableVoice::LfoCutoff, 0.4f)),
                                     with(slot(2, S::Lfo1, WavetableVoice::LfoPosition, 0.6f, S::Strip), slot(3, S::Random, WavetableVoice::LfoPan, 0.8f))),
                                { { W::FilterBase + VF::Cutoff, 2000.0f } });
        cases.push_back({ "four slots (ENV, PRES, LFO via STRP, RAND)", same(four), same(four), false, 0.8f, true });
        cases.push_back({ "XIN PM (audio rate)", xinOld(1), xinNew(1), true });
        cases.push_back({ "XIN POS (audio rate)", xinOld(2), xinNew(2), true });
        // NEPTUNE_U4_WT_BENCH_ONLY=<part of a case's name> and NEPTUNE_U4_WT_BENCH_RUNS=<n> (5): one
        // case, more turns - for a difference of a few per cent on a machine that is not quiet.
        const juce::String only = juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_WT_BENCH_ONLY", {});
        const int runs = std::max(1, juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_WT_BENCH_RUNS", "5").getIntValue());
        std::printf("\n%-48s %9s %9s %8s\n", "wavetable engine, 8 voices, % of one core", "old", "new", "new/old");
        for (auto& c : cases)
        {
            if (only.isNotEmpty() && ! juce::String(c.name).contains(only))
                continue;
            double oldBest = 1.0e9, newBest = 1.0e9;
            for (int run = 0; run < runs; ++run)
            {
                oldBest = std::min(oldBest, cost<O>(c.oldSetup, c.input, c.strip, c.pressure));
                newBest = std::min(newBest, cost<W>(c.newSetup, c.input, c.strip, c.pressure));
            }
            std::printf("%-48s %9.3f %9.3f %8.3f\n", c.name, oldBest, newBest, newBest / oldBest);
        }
        // New sources in use (no old to compare with).
        const Case extra[] = {
            { "SCAN > CUT (scan PING)", nullptr, same(with(with(pwm, { { W::ScanMode, 3.0f }, { W::FilterBase + VF::Cutoff, 1500.0f } }), slot(0, published(WavetableVoice::PubScan), WavetableVoice::LfoCutoff, 0.5f))) },
            { "XRCT > PM", nullptr, same(with(pwm, slot(0, published(WavetableVoice::PubRect), W::dests().phase, 0.5f))), true },
            { "ONE > ETIM, RND2 > POS", nullptr, same(with(with(pwm, slot(0, source(S::One), W::dests().envTime, 0.3f)), slot(1, source(S::Random2), WavetableVoice::LfoPosition, 0.3f))) } };
        for (auto& c : extra)
        {
            if (only.isNotEmpty())
                continue;
            double best = 1.0e9;
            for (int run = 0; run < 5; ++run)
                best = std::min(best, cost<W>(c.newSetup, c.input, c.strip, c.pressure));
            std::printf("%-48s %9s %9.3f\n", c.name, "-", best);
        }
        std::fflush(stdout);
        expect(true);
    }
};

static U4WavetableTests u4WavetableTests;
