#pragma once
#include "fast_measure.h"
// (The old class's voices are read by "the grains' draws, read from the engines": every header it
// includes has been included before this point, so the two words below reach that class alone.)
#include "../src/engine/SamplerEngine.h"
#include "../src/engine/EngineLfoSection.h"
#include <atomic>
#include <thread>
#include <array>
#define private public
#include "old/OldSamplerEngine.h"
#undef private
#include "../src/engine/SamplerEngine.h"
#include "../src/project/FactoryPresets.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <array>
#include <vector>

// "U4 Sampler": the sampler on the modulation router (phase U4-SMP of .ralph/spec-unify.md).
//
// The old engine (tests/old/OldSamplerEngine.h, the class as it was at c21530a) is played beside the
// new one with the same gesture, and every sound is given one of three words:
//   MATCH  the two renders are the same samples, bit for bit
//   CLOSE  they differ, within a rule written down here BEFORE the numbers were seen:
//          "same modulation" (an LFO, a slot, the strip: the router leads its sources over 4 ms and
//          works them out once a sub-block where the old section did so once a sample):
//              the level of the whole render within 0.5 dB, every 50 ms of it that is louder than
//              40 dB under its loudest 50 ms within 1.5 dB, the mean frequency within 5 %
//          "another random sequence" (grains, stretch cycles, RAND starts, PLAY = RAND: the draws
//          come from spread::Generator now, not from the engine's xorshift, so no grain is where
//          it was and no two renders can be laid over one another): SIX renders of the old engine
//          and six of the new, each with another seed, and four numbers a render - its level, its
//          mean frequency, its width (side over mid), and how uneven it is (the spread of its
//          250 ms levels). CLOSE when, for each number, the two means are no further apart than
//          three times their standard error plus a floor (0.3 dB, 2 %, 0.02, 0.3 dB): the new
//          engine's clouds cannot be told from the old engine's by any of them.
//          (This rule replaced one written first, a single old render against a single new one
//          with fixed limits - level 1.5 dB, each 500 ms 3 dB, mean frequency 10 %, width 0.15:
//          the old engine against ITSELF with another seed broke those limits on six of thirteen
//          clouds, e.g. SMER 9.5 dB in one 500 ms, so it could not tell a loss from a seed.)
//   LOST   neither
// With NEPTUNE_ATLAS_DIR set, every pair that is not MATCH is written there as two WAV files.
// The numbers are logged whatever the word ("[u4-smp] ..."), so the report is the log.
//
// NEPTUNE_U4_SAMPLER_BENCH=1 runs the cost measurement instead (Release binary).
class U4SamplerTests : public juce::UnitTest
{
public:
    U4SamplerTests() : UnitTest("U4 Sampler") {}

    using S = SamplerEngine;
    using R = mod::Router;
    static constexpr double SR = 48000.0;

    struct Out
    {
        std::vector<float> l, r;
    };

    enum Kind { On, Off, Bend, Strip, Press };
    struct Ev { double at; Kind kind; int note; float value; };
    using Events = std::vector<Ev>;

    template <typename E>
    static void apply(E& e, const ParamMap& sound)
    {
        const int master = e.getMasterParamId();
        if (auto it = sound.find(master); it != sound.end())
            e.setParameter(master, it->second);
        for (auto& [id, value] : sound)
            if (id != master)
                e.setParameter(id, value);
    }

    template <typename E>
    static Out render(E& e, const Events& events, double seconds, int block = 128, const std::vector<float>* input = nullptr,
                      double sr = SR)
    {
        e.prepareToPlay(sr, block);
        const int total = (int) (seconds * sr);
        Out out;
        out.l.assign((size_t) total, 0.0f);
        out.r.assign((size_t) total, 0.0f);
        juce::MidiBuffer midi;
        size_t next = 0;
        for (int at = 0; at < total; at += block)
        {
            const int n = std::min(block, total - at);
            for (; next < events.size() && (int) (events[next].at * sr) <= at; ++next)
            {
                const Ev& ev = events[next];
                switch (ev.kind)
                {
                    case On:    e.noteOn(ev.note, ev.value, 0); break;
                    case Off:   e.noteOff(ev.note, 0); break;
                    case Bend:  e.pitchBend(ev.value); break;
                    case Strip: e.modWheel(ev.value); break;
                    case Press: e.aftertouch(ev.note, ev.value); break;
                }
            }
            juce::AudioBuffer<float> buffer(2, n);
            buffer.clear();
            if (input != nullptr)
            {
                AudioInput in;
                in.left = in.right = input->data() + at;
                in.frames = n;
                in.envelope = 0.5f;
                e.setAudioInput(in);
            }
            e.processBlock(buffer, midi);
            std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + n, out.l.begin() + at);
            std::copy(buffer.getReadPointer(1), buffer.getReadPointer(1) + n, out.r.begin() + at);
        }
        return out;
    }

    // The gesture every comparison uses unless it says otherwise: a note held and let go, a chord
    // with a bend, a note struck twice, the last one left to ring out.
    static Events gesture()
    {
        return { { 0.0, On, 60, 0.9f }, { 0.5, Off, 60, 0.0f },
                 { 0.6, On, 55, 0.7f }, { 0.6, On, 64, 0.5f }, { 0.8, Bend, 0, 0.5f }, { 1.0, Bend, 0, 0.0f },
                 { 1.1, Off, 55, 0.0f }, { 1.1, Off, 64, 0.0f },
                 { 1.2, On, 67, 1.0f }, { 1.35, On, 67, 0.6f }, { 1.7, Off, 67, 0.0f } };
    }
    static constexpr double GESTURE_SECONDS = 2.2;

    // ---- measures ----------------------------------------------------------------------------
    static double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        to = std::min(to, x.size());
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
        return to > from ? std::sqrt(s / (double) (to - from)) : 0.0;
    }
    static double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

    struct Verdict
    {
        juce::String word;
        bool identical = false;
        double level = 0.0;         // dB, new re. old, the whole render
        double window = 0.0;        // dB, the largest difference of a window that is loud enough
        double centroid = 1.0;      // new / old
        double width = 0.0;         // new - old, side over mid
        double residual = 0.0;      // dB, the difference re. the old render
        juce::String text() const
        {
            if (identical)
                return word + "  (bit-identical)";
            return word + "  level " + juce::String(level, 2) + " dB, window " + juce::String(window, 2) + " dB, centroid x"
                 + juce::String(centroid, 3) + ", width " + juce::String(width, 3) + ", residual " + juce::String(residual, 1) + " dB";
        }
    };

    static std::vector<float> mono(const Out& o)
    {
        std::vector<float> m(o.l.size());
        for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (o.l[i] + o.r[i]);
        return m;
    }
    static double widthOf(const Out& o)
    {
        double mid = 0.0, side = 0.0;
        for (size_t i = 0; i < o.l.size(); ++i)
        {
            const double m = 0.5 * (o.l[i] + o.r[i]), s = 0.5 * (o.l[i] - o.r[i]);
            mid += m * m;
            side += s * s;
        }
        return mid > 0.0 ? std::sqrt(side / mid) : 0.0;
    }

    // The rule for "same modulation".
    static Verdict compare(const Out& a, const Out& b)
    {
        Verdict v;
        v.identical = a.l == b.l && a.r == b.r;
        if (v.identical)
        {
            v.word = "MATCH";
            return v;
        }
        // (Both sides count: a sound that moved between the channels is not the same sound.)
        std::vector<float> ea(a.l.size()), eb(b.l.size()), diff(a.l.size());
        for (size_t i = 0; i < ea.size(); ++i)
        {
            ea[i] = std::sqrt(0.5f * (a.l[i] * a.l[i] + a.r[i] * a.r[i]));
            eb[i] = std::sqrt(0.5f * (b.l[i] * b.l[i] + b.r[i] * b.r[i]));
            const float dl = a.l[i] - b.l[i], dr = a.r[i] - b.r[i];
            diff[i] = std::sqrt(0.5f * (dl * dl + dr * dr));
        }
        const double ra = rms(ea, 0, ea.size()), rb = rms(eb, 0, eb.size());
        v.level = db(rb / std::max(ra, 1.0e-12));
        v.residual = db(rms(diff, 0, diff.size()) / std::max(ra, 1.0e-12));
        const size_t hop = (size_t) (0.05 * SR);
        double loudest = 0.0;
        for (size_t at = 0; at + hop <= ea.size(); at += hop)
            loudest = std::max(loudest, std::max(rms(ea, at, at + hop), rms(eb, at, at + hop)));
        for (size_t at = 0; at + hop <= ea.size(); at += hop)
        {
            const double wa = rms(ea, at, at + hop), wb = rms(eb, at, at + hop);
            if (std::max(wa, wb) > loudest * 0.01)
                v.window = std::max(v.window, std::abs(db(wb / std::max(wa, 1.0e-12))));
        }
        float r1 = 0.0f, r2 = 0.0f, c1 = 0.0f, c2 = 0.0f;
        fastmeasure::rmsAndCentroid(mono(a), 2048, SR, r1, c1);
        fastmeasure::rmsAndCentroid(mono(b), 2048, SR, r2, c2);
        v.centroid = c1 > 0.0f ? (double) c2 / (double) c1 : 1.0;
        v.width = widthOf(b) - widthOf(a);
        const bool close = std::abs(v.level) <= 0.5 && v.window <= 1.5 && std::abs(v.centroid - 1.0) <= 0.05 && std::abs(v.width) <= 0.05;
        v.word = close ? "CLOSE" : "LOST";
        return v;
    }

    static void writeWav(const juce::String& name, const Out& o)
    {
        const char* dir = std::getenv("NEPTUNE_ATLAS_DIR");
        if (dir == nullptr || o.l.empty())
            return;
        juce::File folder(dir);
        folder.createDirectory();
        juce::File file = folder.getChildFile(name + ".wav");
        file.deleteFile();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr)
            return;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), SR, 2, 24, {}, 0));
        if (writer == nullptr)
            return;
        stream.release();
        const float* channels[2] = { o.l.data(), o.r.data() };
        writer->writeFromFloatArrays(channels, 2, (int) o.l.size());
    }

    static juce::String fileName(juce::String name)
    {
        return name.replaceCharacters(" :/>,=()", "__-_____").replace("__", "_");
    }

    // Old against new: the same sound, the same gesture. Returns the verdict; logs it; writes the
    // pair when it is not MATCH.
    using Prepare = std::function<void(OldSamplerEngine&, SamplerEngine&)>;
    Verdict pair(const juce::String& name, const ParamMap& sound, const Events& events, double seconds, int block = 128,
                 const Prepare& prepare = nullptr)
    {
        OldSamplerEngine o;
        SamplerEngine n;
        if (prepare)
            prepare(o, n);
        apply(o, sound);
        apply(n, sound);
        const Out a = render(o, events, seconds, block), b = render(n, events, seconds, block);
        const Verdict v = compare(a, b);
        logMessage("[u4-smp] " + name.paddedRight(' ', 34) + v.text());
        expect(rms(a.l, 0, a.l.size()) > 1.0e-4, name + ": the old render is silent - nothing was compared");
        if (! v.identical)
        {
            writeWav(fileName(name) + "_old", a);
            writeWav(fileName(name) + "_new", b);
        }
        return v;
    }
    void expectMatch(const juce::String& name, const ParamMap& sound, const Events& events = gesture(),
                     double seconds = GESTURE_SECONDS, int block = 128)
    {
        const Verdict v = pair(name, sound, events, seconds, block);
        expect(v.identical, name + " is no longer what it was, sample for sample: " + v.text());
    }

    // ---- "another random sequence": six seeds of each engine ------------------------------------------
    struct Features { double level = 0.0, centroid = 0.0, width = 0.0, uneven = 0.0; };
    static Features featuresOf(const Out& o)
    {
        Features f;
        std::vector<float> e(o.l.size());
        for (size_t i = 0; i < e.size(); ++i)
            e[i] = std::sqrt(0.5f * (o.l[i] * o.l[i] + o.r[i] * o.r[i]));
        f.level = db(rms(e, 0, e.size()));
        float r = 0.0f, c = 0.0f;
        fastmeasure::rmsAndCentroid(mono(o), 2048, SR, r, c);
        f.centroid = 12.0 * std::log2(std::max(1.0, (double) c));      // semitones: 2 % is a third of one
        f.width = widthOf(o);
        const size_t hop = (size_t) (0.25 * SR);
        std::vector<double> levels;
        double loudest = 0.0;
        for (size_t at = 0; at + hop <= e.size(); at += hop)
            loudest = std::max(loudest, rms(e, at, at + hop));
        for (size_t at = 0; at + hop <= e.size(); at += hop)
            if (const double w = rms(e, at, at + hop); w > loudest * 0.01)
                levels.push_back(db(w));
        double mean = 0.0;
        for (double x : levels) mean += x;
        mean /= (double) std::max<size_t>(1, levels.size());
        for (double x : levels) f.uneven += (x - mean) * (x - mean);
        f.uneven = std::sqrt(f.uneven / (double) std::max<size_t>(1, levels.size()));
        return f;
    }
    // Member j of an engine's six: its generators seeded with j, and j notes played and cut first
    // (the old engine's RAND start has a generator no seed reaches: the notes move it on).
    template <typename E>
    static Out member(const ParamMap& sound, const Events& events, double seconds, int j)
    {
        E e;
        apply(e, sound);
        e.setGrainSeed(100u + (uint32_t) j);
        e.setSliceSeed(100u + (uint32_t) j);
        for (int k = 0; k < j; ++k)
        {
            e.noteOn(60, 0.5f, 0);
            e.allNotesOff();
        }
        e.setSliceSeed(100u + (uint32_t) j);
        e.takeJump();
        return render(e, events, seconds);
    }
    juce::String cloudPair(const juce::String& name, const ParamMap& sound, const Events& events, double seconds)
    {
        static constexpr int N = 6;
        Features o[N], n[N];
        for (int j = 0; j < N; ++j)
        {
            const Out a = member<OldSamplerEngine>(sound, events, seconds, j), b = member<SamplerEngine>(sound, events, seconds, j);
            if (j == 0)
            {
                expect(rms(a.l, 0, a.l.size()) > 1.0e-4, name + ": the old render is silent - nothing was compared");
                expect(! (a.l == b.l && a.r == b.r), name + ": the same samples - this is no cloud, compare it as a pair");
                writeWav(fileName(name) + "_old", a);
                writeWav(fileName(name) + "_new", b);
            }
            o[j] = featuresOf(a);
            n[j] = featuresOf(b);
        }
        struct Row { const char* what; double Features::* field; double floor; const char* unit; };
        static const Row rows[] = { { "level", &Features::level, 0.3, "dB" }, { "mean frequency", &Features::centroid, 0.34, "st" },
                                    { "width", &Features::width, 0.02, "" }, { "unevenness", &Features::uneven, 0.3, "dB" } };
        bool close = true;
        juce::String text;
        for (auto& row : rows)
        {
            double mo = 0.0, mn = 0.0, vo = 0.0, vn = 0.0;
            for (int j = 0; j < N; ++j) { mo += o[j].*row.field / N; mn += n[j].*row.field / N; }
            for (int j = 0; j < N; ++j) { vo += std::pow(o[j].*row.field - mo, 2.0) / (N - 1); vn += std::pow(n[j].*row.field - mn, 2.0) / (N - 1); }
            const double allowed = 3.0 * std::sqrt((vo + vn) / N) + row.floor;
            const bool ok = std::abs(mn - mo) <= allowed;
            close = close && ok;
            text += juce::String(row.what) + " " + juce::String(mo, 2) + " -> " + juce::String(mn, 2) + " " + row.unit + " (apart "
                    + juce::String(std::abs(mn - mo), 2) + ", allowed " + juce::String(allowed, 2) + (ok ? ")" : ") !") + "; ";
        }
        const juce::String word = close ? "CLOSE" : "LOST";
        logMessage("[u4-smp] " + name.paddedRight(' ', 34) + word + "  six seeds each: " + text);
        return word;
    }
    // drawn: the sound is made of random draws (the six-seed rule); otherwise the same modulation.
    void expectClose(const juce::String& name, const ParamMap& sound, bool drawn, const Events& events = gesture(),
                     double seconds = GESTURE_SECONDS)
    {
        if (drawn)
        {
            expect(cloudPair(name, sound, events, seconds) == "CLOSE", name);
            return;
        }
        const Verdict v = pair(name, sound, events, seconds);
        expect(v.word != "LOST", name + ": " + v.text());
    }

    // ---- test samples (48 kHz, two seconds) ---------------------------------------------------------
    static std::vector<float> tone()
    {
        std::vector<float> x((size_t) (2.0 * SR));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = 0.5f * (float) std::sin(2.0 * 3.141592653589793 * 220.0 * (double) i / SR);
        return x;
    }
    // Noise: loud for the first second, twenty decibels down for the second.
    static std::vector<float> loudThenQuiet()
    {
        std::vector<float> x((size_t) (2.0 * SR));
        uint32_t state = 0x1234567u;
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (i < x.size() / 2 ? 0.5f : 0.05f) * dsp::xorshiftNoise(state);
        return x;
    }
    template <typename E>
    static void load(E& e, const std::vector<float>& x) { e.loadSample(x.data(), (int) x.size(), 1, SR); }

    static int breakSample()
    {
        auto& bank = SampleBank::get();
        return bank.breakCount() > 0 ? bank.breakIndex(0) : bank.defaultIndex();
    }
    static ParamMap lfo(int which, int dest, float depth, float rate = 3.0f, int trigger = 1)
    {
        return { { R::lfoId(which, R::Rate), rate }, { R::lfoId(which, R::Depth), depth },
                 { R::lfoId(which, R::Trigger), (float) trigger }, { R::lfoId(which, R::DestCell), (float) dest } };
    }
    static ParamMap with(ParamMap a, const ParamMap& b)
    {
        for (auto& kv : b) a[kv.first] = kv.second;
        return a;
    }
    static ParamMap slot(int s, int src, int dest, float amount, int via = 0)
    {
        return { { ModSlotParam::id(s, ModSlotParam::SRC), (float) src }, { ModSlotParam::id(s, ModSlotParam::DEST), (float) dest },
                 { ModSlotParam::id(s, ModSlotParam::AMT), amount }, { ModSlotParam::id(s, ModSlotParam::VIA), (float) via } };
    }
    // A base sound in which a destination is heard.
    static ParamMap soundFor(int dest)
    {
        ParamMap p { { S::Mode, (float) S::Loop } };
        if ((dest >= S::LfoGrainPos && dest <= S::LfoGrainDens) || (dest >= S::LfoScan && dest <= S::LfoReverse))
            p = { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.5f } };
        else if (dest == S::LfoSpeed || (dest >= S::LfoStretchSize && dest <= S::LfoStretchSpread))
            p = { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Loop },
                  { S::StretchMode, (float) (dest == S::LfoStretchSpread ? S::StretchGrain : S::StretchCycle) }, { S::StretchSpeed, 0.7f } };
        else if (dest >= S::LfoSlice && dest <= S::LfoSliceLen)
            p = { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Forward }, { S::SliceCount, 8.0f },
                  { S::SliceSelect, 0.4f }, { S::SliceFade, 0.02f }, { S::SliceLen, 0.6f } };
        else if (dest == S::LfoStart || dest == S::LfoEnd || dest == S::LfoStartRandom)
            p = { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Forward }, { S::StartPoint, 0.3f }, { S::EndPoint, 0.7f } };
        else if (dest == S::LfoLoopStart || dest == S::LfoLoopEnd)
            p = { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Loop }, { S::LoopStart, 0.2f }, { S::LoopEnd, 0.6f },
                  { S::EndPoint, 0.5f } };
        if (dest == S::LfoCutoff || dest == S::LfoResonance || (dest >= S::LfoVelFilter && dest <= S::LfoFilterDrive))
            p = with(p, { { S::FilterCutoff, 0.55f }, { S::FilterReso, 0.4f }, { S::FilterEnv, 0.2f }, { S::VelToFilter, 0.2f },
                          { S::FilterKey, 0.3f }, { S::FilterDrive, 0.3f } });
        if (dest == S::LfoDrive)    p[S::DriveAmt] = 0.3f;
        if (dest == S::LfoBits)     p[S::BitReduce] = 0.4f;
        if (dest == S::LfoRate)     p[S::RateReduce] = 0.3f;
        if (dest == S::LfoSustain)  p = with(p, { { S::Sustain, 0.5f }, { S::Decay, 0.1f } });
        if (dest == S::LfoStartRandom) p[S::StartRandom] = 0.0f;
        return p;
    }

    // ------------------------------------------------------------------------------------------
    void runTest() override
    {
        if (std::getenv("NEPTUNE_U4_SAMPLER_BENCH") != nullptr)
        {
            bench();
            return;
        }
        if (std::getenv("NEPTUNE_U4_SAMPLER_HOLD") != nullptr)
        {
            holdOutBench();
            return;
        }
        tableAndPages();
        plainPath();
        factoryPresets();
        modulated();
        randoms();
        readAtTheNote();
        envelopeTime();
        ownSources();
        pressure();
        stripDrive();
        audioRate();
        otherRates();
        grainDrawsCompared();
        reviewCases();
        if (std::getenv("NEPTUNE_U4_REVIEW_PROBE") != nullptr)
            probeNoteStartInput();
    }

    // ---- the table: old stored values keep their meaning -----------------------------------------
    void tableAndPages()
    {
        beginTest("The table: the old list's forty-two entries at their indices, then ETIM");
        {
            const auto& oldList = OldSamplerEngine::lfoDests();
            const auto& table = S::lfoDests();
            expectEquals(oldList.size(), 42);
            expectEquals(table.visible(), 43);
            expectEquals(table.size(), (int) S::NumDests);
            for (int i = 0; i < oldList.size(); ++i)
                expect(juce::String(oldList.labels()[i]) == juce::String(table.labels()[i]),
                       "entry " + juce::String(i) + ": " + juce::String(oldList.labels()[i]) + " is now " + juce::String(table.labels()[i]));
            expect(juce::String(table.labels()[42]) == "ETIM");
            expectEquals(table.ampTime(), (int) S::LfoEnvTime);
            // (What the engine only asserts in a Debug build: the places the voice reads by number.)
            expectEquals(table.indexOfLabel("SPD"), (int) S::LfoSpeed);
            expectEquals(table.indexOfId(S::DriveAmt), (int) S::LfoDrive);
            expectEquals(table.indexOfId(S::StretchSize), (int) S::LfoStretchSize);
            expectEquals(table.filterTime(), -1);
            // Per sample: the five the voice takes sample by sample, and nothing else.
            for (int d = 1; d < table.size(); ++d)
            {
                const bool wanted = d == S::LfoPitch || d == S::LfoAmp || d == S::LfoPan || d == S::LfoCutoff || d == S::LfoGrainPos;
                expect(table[d].perSample == wanted, juce::String(table[d].label) + " per sample");
            }
            // A free slot's DEST: the table, then L1RT .. and AMT1 ..: one later than they were.
            expect(juce::String(table.slotLabels()[43]) == "L1RT" && juce::String(table.slotLabels()[47]) == "AMT1");
            // The strip's defaults, as before the router.
            expectEquals(table.stripDefault(0).dest, (int) S::LfoDrive);
            expectWithinAbsoluteError(table.stripDefault(0).amount, 0.7f, 1.0e-6f);
            expect(juce::String(table[table.stripDefault(1).dest].label) == "RATE");
            expect(juce::String(table[table.stripDefault(2).dest].label) == "SPRY");
            expectWithinAbsoluteError(table.stripVibratoDefaultValue(), 0.0f, 1.0e-6f);
        }

        beginTest("Pages and ids: the engine's seven, then XIN, SLT1, LSHP, STRIP, LFO1, LFO2; nothing of its own in the router's ids");
        {
            SamplerEngine e;
            const auto pages = e.getParamPages();
            juce::StringArray names;
            for (auto& p : pages) names.add(p.name);
            expectEquals(names.joinIntoString(" "), juce::String("SAMPL GRAIN SLICE STRCH TONE FILTR AMP XIN SLT1 LSHP STRIP LFO1 LFO2"));
            for (auto& page : S::pages())
                for (auto& cell : page.params)
                    if (cell.isValid())
                        expect(! R::owns(cell.id) && cell.id < S::NumParams, juce::String(cell.label));
            // The old engine's cells for the LFOs and the strip were at the same ids.
            for (int id = R::LFO_BASE; id < R::XIN_BASE; ++id)
                expect(EngineLfoSection::owns(id));
            // Every cell is its true value both ways.
            for (auto& page : pages)
                for (auto& cell : page.params)
                    if (cell.isValid())
                        expectWithinAbsoluteError(e.getParameter(cell.id), cell.defaultVal, 1.0e-6f,
                                                  juce::String(page.name) + " " + juce::String(cell.label));
            e.setParameter(R::lfoId(1, R::DestCell), (float) S::LfoGrainPos);
            expectEquals((int) e.getParameter(R::lfoId(1, R::DestCell)), (int) S::LfoGrainPos);
            expect(e.getParameterName(R::lfoId(0, R::Rate)) == "LFO1 Rate");
        }

        beginTest("The source list: the router's, then POS GRN SLEV");
        {
            const auto& sources = S::sources();
            expectEquals(sources.published(), 3);
            const int n = sources.size();
            expect(juce::String(sources.labels()[n - 3]) == "POS" && juce::String(sources.labels()[n - 2]) == "GRN"
                   && juce::String(sources.labels()[n - 1]) == "SLEV");
            expect(! sources.audio(0) && ! sources.bipolar(0) && ! sources.bipolar(1) && ! sources.bipolar(2));
            // The places the old slots' SRC cells held are the same sources.
            static const char* const before[] = { "OFF", "LFO1", "LFO2", "ENV", "FENV", "VEL", "PRES", "STRP", "KEY", "RAND", "IN" };
            for (int i = 0; i < 11; ++i)
                expect(juce::String(sources.labels()[i]) == before[i], before[i]);
        }
    }

    // ---- nothing routed: sample for sample what it was ---------------------------------------------
    void plainPath()
    {
        const float brk = (float) breakSample();
        beginTest("Plain path: every play mode, sample for sample");
        static const char* const modes[] = { "FWD", "REV", "1SHT", "GATE", "LOOP", "PING" };
        for (int mode = 0; mode < 6; ++mode)
        {
            expectMatch(juce::String("mode ") + modes[mode], { { S::Mode, (float) mode } });
            expectMatch(juce::String("mode ") + modes[mode] + " break", { { S::SampleSelect, brk }, { S::Mode, (float) mode },
                                                                         { S::StartPoint, 0.1f }, { S::EndPoint, 0.6f } });
        }

        beginTest("Plain path: slices");
        expectMatch("slices GRID ONE", { { S::SampleSelect, brk }, { S::Mode, (float) S::Forward }, { S::SliceCount, 8.0f },
                                         { S::SliceSelect, 0.4f }, { S::SliceFade, 0.02f }, { S::SliceLen, 0.7f } });
        expectMatch("slices GRID KEYS", { { S::SampleSelect, brk }, { S::Mode, (float) S::OneShot }, { S::SliceCount, 16.0f },
                                          { S::SlicePlay, (float) S::SliceKeys }, { S::RootNote, 55.0f } });
        expectMatch("slices TRNS NEXT", { { S::SampleSelect, brk }, { S::Mode, (float) S::OneShot }, { S::SliceMode, (float) S::SliceTransient },
                                          { S::SlicePlay, (float) S::SliceNext } });

        beginTest("Plain path: the stretches that draw nothing at random");
        static const char* const algos[] = { "OFF", "TAPE", "CYCL", "GRAN", "SLCE", "SMER", "WSOL" };
        for (int algo : { (int) S::StretchTape, (int) S::StretchCycle, (int) S::StretchSlice, (int) S::StretchWsol })
            for (float speed : { 0.5f, 1.7f })
                expectMatch(juce::String("stretch ") + algos[algo] + " x" + juce::String(speed, 1),
                            { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::StretchMode, (float) algo },
                              { S::StretchSpeed, speed }, { S::SliceCount, 8.0f } });
        expectMatch("stretch SLCE SYNC", { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::SliceMode, (float) S::SliceTransient },
                                           { S::StretchMode, (float) S::StretchSlice }, { S::StretchSync, 1.0f }, { S::Voices, 1.0f } });

        beginTest("Plain path: grains with nothing drawn at random (the move onto grn::Cloud)");
        for (float scan : { 1.0f, 0.25f, 0.0f, -1.0f })
            expectMatch("GRAN scan " + juce::String(scan, 2), { { S::SampleSelect, brk }, { S::Mode, (float) S::Granular },
                                                                { S::GrainScan, scan }, { S::StartPoint, 0.2f } });
        expectMatch("GRAN PERC dense", { { S::SampleSelect, brk }, { S::Mode, (float) S::Granular }, { S::GrainSize, 0.3f },
                                         { S::GrainDensity, 150.0f }, { S::GrainShape, (float) S::WindowPerc }, { S::SliceCount, 4.0f } });

        beginTest("Plain path: filter, colour, level, voices, block sizes");
        const ParamMap coloured { { S::Mode, (float) S::Loop }, { S::FilterCutoff, 0.5f }, { S::FilterReso, 0.5f }, { S::FilterEnv, 0.4f },
                                  { S::VelToFilter, 0.3f }, { S::FilterKey, 0.5f }, { S::FilterDrive, 0.4f }, { S::BitReduce, 0.3f },
                                  { S::RateReduce, 0.2f }, { S::DriveAmt, 0.4f }, { S::Pan, -0.4f }, { S::Normalize, 1.0f },
                                  { S::Attack, 0.05f }, { S::Decay, 0.2f }, { S::Sustain, 0.6f }, { S::Release, 0.4f } };
        expectMatch("filter and colour", coloured);
        for (int type = 1; type < FilterSlot::NUM_MODELS; ++type)
            expectMatch("filter type " + juce::String(type), with(coloured, { { S::FilterType, (float) type } }));
        expectMatch("two voices, notes give way", with(coloured, { { S::Voices, 2.0f }, { S::Release, 1.5f } }));
        for (int block : { 16, 100, 512 })
            expectMatch("block " + juce::String(block), coloured, gesture(), GESTURE_SECONDS, block);
        // The strip at rest moves nothing; brought up with its routes taken away it moves nothing either.
        expectMatch("strip up, no routes", with(coloured, { { R::stripAmountId(0), 0.0f }, { R::stripAmountId(1), 0.0f }, { R::stripAmountId(2), 0.0f } }),
                    { { 0.0, On, 60, 0.9f }, { 0.3, Strip, 0, 1.0f }, { 1.0, Off, 60, 0.0f } }, 1.5);
    }

    // ---- every factory preset of the engine ---------------------------------------------------------
    void factoryPresets()
    {
        beginTest("Every sampler factory preset, old against new");
        int seen = 0, matched = 0;
        for (auto& preset : FactoryPresets::getAll())
        {
            if (preset.engineType != EngineType::Sampler)
                continue;
            ++seen;
            // Abyss Drone is a cloud whose every grain is drawn at random (SPRY, PJIT, SPRD, RVRS):
            // it is the one factory sound that depends on the grains' random sequence.
            const bool cloud = preset.name == "Abyss Drone";
            const Events events = cloud ? Events { { 0.0, On, 60, 0.9f }, { 0.0, On, 48, 0.7f }, { 5.0, Off, 60, 0.0f }, { 5.0, Off, 48, 0.0f } }
                                        : gesture();
            if (cloud)
            {
                expect(cloudPair("preset " + preset.name, preset.soundParams, events, 7.0) == "CLOSE", preset.name);
                continue;
            }
            const Verdict v = pair("preset " + preset.name, preset.soundParams, events, GESTURE_SECONDS);
            matched += v.identical ? 1 : 0;
            expect(v.identical, preset.name + " is no longer what it was, sample for sample: " + v.text());
        }
        expect(seen >= 5, "sampler presets found: " + juce::String(seen));
        logMessage("[u4-smp] factory presets: " + juce::String(seen) + ", bit-identical " + juce::String(matched));
    }

    // ---- LFOs, slots, the strip: the same modulation through the router -------------------------------
    void modulated()
    {
        const auto& table = S::lfoDests();
        beginTest("LFO 1 to every destination of the old list, old against new");
        for (int dest = 1; dest < 42; ++dest)
        {
            if (table[dest].cross != 0)
                continue;       // (LRAT / LDPT: below, with the other LFO running)
            const float depth = dest == S::LfoPitch ? 0.3f : 0.5f;
            // (What the destination scales is drawn at random: another sequence.)
            const bool drawn = dest == S::LfoSpray || dest == S::LfoPitchJit || dest == S::LfoSpread || dest == S::LfoReverse
                               || dest == S::LfoJitter || dest == S::LfoStretchSpread || dest == S::LfoStartRandom;
            // What is read once at the note is read from a square that starts with the key: its
            // first sample is the whole depth (a sine's would be nothing).
            const bool once = dest == S::LfoStart || dest == S::LfoEnd || dest == S::LfoLoopStart || dest == S::LfoLoopEnd
                              || (dest >= S::LfoSlice && dest <= S::LfoSliceLen) || dest == S::LfoVelFilter || dest == S::LfoFilterKey
                              || (dest >= S::LfoAttack && dest <= S::LfoRelease) || dest == S::LfoStartRandom;
            ParamMap sound = with(soundFor(dest), lfo(0, dest, depth));
            if (once)
            {
                sound[R::lfoId(0, R::Shape)] = (float) (int) LFO::Shape::Square;
                sound[R::lfoId(0, R::Depth)] = dest == S::LfoEnd || dest == S::LfoLoopEnd || dest == S::LfoSliceLen ? -0.2f : 0.2f;
            }
            expectClose(juce::String("LFO1 > ") + table[dest].label, sound, drawn);
        }
        // What is read once is read at the note's FIRST SAMPLE: a fast sine that starts with the
        // key is next to nothing there, and an eighth of its depth sixteen samples later.
        {
            const Verdict v = pair("LFO1 40 Hz sine > STRT", with(with(soundFor(S::LfoStart), lfo(0, S::LfoStart, 1.0f, 40.0f)), { { S::EndPoint, 1.0f } }),
                                   gesture(), GESTURE_SECONDS);
            expect(v.word != "LOST", v.text());
        }
        expectClose("LFO1 > CUT, LFO2 > LRAT", with(with(soundFor(S::LfoCutoff), lfo(0, S::LfoCutoff, 0.5f)), lfo(1, S::LfoCrossRate, 0.8f, 0.7f)), false);
        expectClose("LFO1 > AMP, LFO2 > LDPT", with(with(soundFor(S::LfoAmp), lfo(0, S::LfoAmp, 0.5f)), lfo(1, S::LfoCrossDepth, 0.8f, 0.7f)), false);
        expectClose("LFO1 FREE > PAN", with(soundFor(S::LfoPan), lfo(0, S::LfoPan, 0.8f, 2.0f, 0)), false);
        expectClose("LFO1 synced > CUT", with(with(soundFor(S::LfoCutoff), lfo(0, S::LfoCutoff, 0.5f)), { { R::lfoId(0, R::Sync), 1.0f } }), false);
        expectClose("LFO1 square > AMP", with(with(soundFor(S::LfoAmp), lfo(0, S::LfoAmp, 0.5f)), { { R::lfoId(0, R::Shape), (float) (int) LFO::Shape::Square } }), false);

        beginTest("Mod slots, old against new");
        const ParamMap filtered = soundFor(S::LfoCutoff);
        expectClose("slot VEL > CUT", with(filtered, slot(0, mod::Src::Velocity, S::LfoCutoff, 0.6f)), false);
        expectClose("slot ENV > PTCH", with(filtered, slot(0, mod::Src::Env, S::LfoPitch, 0.4f)), false);
        expectClose("slot KEY > PAN", with(filtered, slot(0, mod::Src::Key, S::LfoPan, 1.0f)), false);
        expectClose("slot LFO1 > AMP via VEL", with(filtered, with(slot(1, mod::Src::Lfo1, S::LfoAmp, 0.8f, mod::Src::Velocity),
                                                                 { { R::lfoId(0, R::Trigger), 1.0f } })), false);
        expectClose("slot ENV > DRIV", with(filtered, slot(2, mod::Src::Env, S::LfoDrive, 0.7f)), false);
        // (The old DEST list had L1RT at 42; it is at 43 now: the same slot, by its new number.)
        {
            OldSamplerEngine o;
            SamplerEngine n;
            const ParamMap sound = with(filtered, lfo(0, S::LfoCutoff, 0.6f));
            apply(o, with(sound, slot(0, mod::Src::Env, 42, 0.8f)));
            apply(n, with(sound, slot(0, mod::Src::Env, 43, 0.8f)));
            const Out a = render(o, gesture(), GESTURE_SECONDS), b = render(n, gesture(), GESTURE_SECONDS);
            const Verdict v = compare(a, b);
            logMessage("[u4-smp] " + juce::String("slot ENV > L1RT (42 -> 43)").paddedRight(' ', 34) + v.text());
            expect(v.word != "LOST", v.text());
            if (! v.identical) { writeWav("slot_ENV-L1RT_old", a); writeWav("slot_ENV-L1RT_new", b); }
        }
        const Events pressed { { 0.0, On, 60, 0.9f }, { 0.3, Press, 60, 0.4f }, { 0.6, Press, 60, 1.0f }, { 1.0, Press, 60, 0.2f },
                               { 1.4, Off, 60, 0.0f } };
        expectClose("slot PRES > CUT", with(filtered, slot(0, mod::Src::Pressure, S::LfoCutoff, 0.7f)), false, pressed, 2.0);
        expectClose("slot PRES > AMP", with(filtered, slot(0, mod::Src::Pressure, S::LfoAmp, -0.8f)), false, pressed, 2.0);

        beginTest("The strip: its default routes in every mode, a route of one's own, its vibrato; old against new");
        const Events stripped { { 0.0, On, 60, 0.9f }, { 0.3, Strip, 0, 0.5f }, { 0.7, Strip, 0, 1.0f }, { 1.2, Strip, 0, 0.0f },
                                { 1.6, Off, 60, 0.0f } };
        static const char* const modes[] = { "FWD", "REV", "1SHT", "GATE", "LOOP", "PING" };
        for (int mode = 0; mode < 6; ++mode)
            expectClose(juce::String("strip defaults ") + modes[mode], { { S::Mode, (float) mode }, { S::DriveAmt, 0.2f } }, false, stripped, 2.0);
        expectClose("strip > PAN, CUT", with(filtered, { { R::stripDestId(0), (float) S::LfoPan }, { R::stripAmountId(0), 1.0f },
                                                         { R::stripDestId(1), (float) S::LfoCutoff }, { R::stripAmountId(1), -0.6f },
                                                         { R::stripAmountId(2), 0.0f } }), false, stripped, 2.0);
        expectClose("strip vibrato", { { S::Mode, (float) S::Loop }, { R::STRIP_VIBRATO, 0.6f }, { R::STRIP_VIBRATO_RATE, 6.0f },
                                       { R::stripAmountId(0), 0.0f }, { R::stripAmountId(1), 0.0f }, { R::stripAmountId(2), 0.0f } },
                    false, stripped, 2.0);
    }

    // ---- what is drawn at random: another sequence, the same cloud -----------------------------------
    void randoms()
    {
        const float brk = (float) breakSample();
        const Events held { { 0.0, On, 60, 0.9f }, { 0.0, On, 67, 0.8f }, { 3.2, Off, 60, 0.0f }, { 3.2, Off, 67, 0.0f } };
        beginTest("Grain clouds and stretch cycles with their randoms up: another sequence, the same cloud");
        const ParamMap cloud { { S::SampleSelect, brk }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.3f }, { S::GrainSize, 0.12f },
                               { S::GrainDensity, 80.0f } };
        expectClose("GRAN SPRY", with(cloud, { { S::GrainSpray, 0.6f } }), true, held, 4.0);
        expectClose("GRAN PJIT", with(cloud, { { S::GrainPitchJit, 0.4f } }), true, held, 4.0);
        expectClose("GRAN SPRD", with(cloud, { { S::GrainSpread, 0.9f } }), true, held, 4.0);
        expectClose("GRAN RVRS", with(cloud, { { S::GrainReverse, 0.5f } }), true, held, 4.0);
        expectClose("GRAN all four", with(cloud, { { S::GrainSpray, 0.5f }, { S::GrainPitchJit, 0.3f }, { S::GrainSpread, 0.8f },
                                                   { S::GrainReverse, 0.4f } }), true, held, 4.0);
        const ParamMap stretched { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::StretchSpeed, 0.6f } };
        expectClose("stretch GRAN", with(stretched, { { S::StretchMode, (float) S::StretchGrain } }), true, held, 4.0);
        expectClose("stretch GRAN JIT SPRD", with(stretched, { { S::StretchMode, (float) S::StretchGrain }, { S::StretchJitter, 0.6f },
                                                               { S::StretchSpread, 0.8f } }), true, held, 4.0);
        expectClose("stretch SMER", with(stretched, { { S::StretchMode, (float) S::StretchSmear } }), true, held, 4.0);
        expectClose("stretch CYCL JIT", with(stretched, { { S::StretchMode, (float) S::StretchCycle }, { S::StretchJitter, 0.6f } }), true, held, 4.0);
        expectClose("strip defaults GRAN", cloud, true, { { 0.0, On, 60, 0.9f }, { 0.5, Strip, 0, 1.0f }, { 3.2, Off, 60, 0.0f } }, 4.0);

        beginTest("A grain's draws are independent: its pitch says nothing of its side");
        {
            // PJIT and SPRD both at the top on a steady tone: were one draw used for both, the
            // high grains would sit on one side and the low ones on the other.
            SamplerEngine e;
            load(e, tone());
            apply(e, { { S::Mode, (float) S::Granular }, { S::GrainScan, 0.2f }, { S::GrainPitchJit, 1.0f }, { S::GrainSpread, 1.0f },
                       { S::GrainDensity, 120.0f }, { S::GrainSize, 0.05f } });
            const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 4.0);
            float rl = 0.0f, cl = 0.0f, rr = 0.0f, cr = 0.0f;
            fastmeasure::rmsAndCentroid(o.l, 2048, SR, rl, cl);
            fastmeasure::rmsAndCentroid(o.r, 2048, SR, rr, cr);
            expect(rl > 0.01f && rr > 0.01f);
            expectWithinAbsoluteError(12.0 * std::log2((double) cr / (double) cl), 0.0, 1.0,
                                      "mean frequency " + juce::String(cl, 0) + " Hz left, " + juce::String(cr, 0) + " Hz right");
        }

        beginTest("A seed repeats a cloud, a RAND start and the router's randoms; another seed is another cloud");
        {
            const ParamMap sound = with(cloud, with({ { S::GrainSpray, 0.5f }, { S::GrainPitchJit, 0.3f }, { S::GrainSpread, 0.8f },
                                                      { S::GrainReverse, 0.4f }, { S::StartRandom, 0.5f } },
                                                    slot(0, S::sources().indexOf(mod::Src::Random2), S::LfoPan, 1.0f)));
            auto play = [&](uint32_t seed)
            {
                SamplerEngine e;
                apply(e, sound);
                e.setGrainSeed(seed);
                return render(e, gesture(), GESTURE_SECONDS);
            };
            const Out a = play(11), b = play(11), c = play(12);
            expect(a.l == b.l && a.r == b.r, "the same seed, the same render");
            expect(a.l != c.l, "another seed, another render");
            // Without a seed set: two new engines play the same (the fixed seed).
            SamplerEngine e1, e2;
            apply(e1, sound);
            apply(e2, sound);
            const Out d1 = render(e1, gesture(), GESTURE_SECONDS), d2 = render(e2, gesture(), GESTURE_SECONDS);
            expect(d1.l == d2.l && d1.r == d2.r, "two new engines, the same render");
            // ... and the second note of a voice is not the first again.
            SamplerEngine e3;
            apply(e3, with(sound, { { S::Voices, 1.0f } }));
            const Out twice = render(e3, { { 0.0, On, 60, 0.9f }, { 1.0, On, 60, 0.9f } }, 2.0);
            const std::vector<float> first(twice.l.begin() + 4800, twice.l.begin() + 24000), second(twice.l.begin() + 52800, twice.l.begin() + 72000);
            expect(first != second, "each note its own draws");
        }

        beginTest("RAND (the start offset) and PLAY = RAND: draws of the one generator");
        {
            // The start offsets of sixty notes cover the region and no two engines' sequences differ.
            auto starts = [](uint32_t seed)
            {
                SamplerEngine e;
                apply(e, { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Forward }, { S::StartRandom, 1.0f }, { S::Voices, 1.0f } });
                e.setGrainSeed(seed);
                e.prepareToPlay(SR, 64);
                std::vector<float> at;
                for (int i = 0; i < 60; ++i)
                {
                    e.noteOn(60, 0.8f, 0);
                    at.push_back(e.getPlayheadNormalized());
                    e.allNotesOff();
                }
                return at;
            };
            const auto a = starts(3), b = starts(3), c = starts(4);
            expect(a == b && a != c);
            float lo = 1.0f, hi = 0.0f;
            for (float x : a) { lo = std::min(lo, x); hi = std::max(hi, x); }
            expect(lo < 0.1f && hi > 0.85f && hi < 1.0f, "offsets from " + juce::String(lo, 3) + " to " + juce::String(hi, 3));

            auto order = [](uint32_t seed)
            {
                SamplerEngine e;
                apply(e, { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::OneShot }, { S::SliceCount, 8.0f },
                           { S::SlicePlay, (float) S::SliceRandom } });
                e.setSliceSeed(seed);
                std::vector<int> played;
                for (int i = 0; i < 64; ++i)
                {
                    e.noteOn(60, 0.8f, 0);
                    played.push_back(e.getPlayingSlice());
                }
                return played;
            };
            const auto p = order(7), q = order(7), r = order(8);
            expect(p == q && p != r);
            int counts[8] = {};
            for (int s : p) { expect(s >= 0 && s < 8); if (s >= 0 && s < 8) ++counts[s]; }
            for (int c8 : counts) expect(c8 >= 2, "every slice comes up");
            // Old against new: both play one slice of the eight a note, at random; which ones differ.
            expectClose("slices PLAY RAND", { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::OneShot }, { S::SliceCount, 4.0f },
                                              { S::SlicePlay, (float) S::SliceRandom } }, true,
                        { { 0.0, On, 60, 0.9f }, { 0.25, On, 60, 0.9f }, { 0.5, On, 60, 0.9f }, { 0.75, On, 60, 0.9f }, { 1.0, On, 60, 0.9f },
                          { 1.25, On, 60, 0.9f }, { 1.5, On, 60, 0.9f }, { 1.75, On, 60, 0.9f }, { 2.0, On, 60, 0.9f }, { 2.25, On, 60, 0.9f },
                          { 2.5, On, 60, 0.9f }, { 2.75, On, 60, 0.9f }, { 3.0, On, 60, 0.9f }, { 3.25, On, 60, 0.9f }, { 3.5, On, 60, 0.9f },
                          { 3.75, On, 60, 0.9f } }, 4.0);
            expectClose("RAND start", { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Loop }, { S::StartRandom, 0.8f } }, true,
                        { { 0.0, On, 60, 0.9f }, { 0.5, On, 62, 0.9f }, { 1.0, On, 64, 0.9f }, { 1.5, On, 65, 0.9f }, { 2.0, On, 67, 0.9f },
                          { 2.5, On, 69, 0.9f }, { 3.0, On, 71, 0.9f }, { 3.5, On, 72, 0.9f } }, 4.0);
        }
    }

    // ---- STRT and the other things read once: at the note's first sub-block ----------------------------
    void readAtTheNote()
    {
        beginTest("STRT, END, SLIC, ATK: read at the note's first sub-block, with the note's own sources");
        const ParamMap base { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Forward }, { S::StartPoint, 0.2f } };
        auto startOf = [&](const ParamMap& sound, float velocity, int note = 60)
        {
            SamplerEngine e;
            apply(e, sound);
            e.prepareToPlay(SR, 64);
            e.noteOn(note, velocity, 0);
            juce::AudioBuffer<float> buffer(2, 16);
            buffer.clear();
            juce::MidiBuffer midi;
            e.processBlock(buffer, midi);
            // (sixteen samples on from where the note began, at the sample's own speed)
            return e.getPlayheadNormalized();
        };
        const float plain = startOf(base, 0.5f);
        const float soft = startOf(with(base, slot(0, mod::Src::Velocity, S::LfoStart, 0.5f)), 0.2f);
        const float hard = startOf(with(base, slot(0, mod::Src::Velocity, S::LfoStart, 0.5f)), 1.0f);
        expectWithinAbsoluteError(plain, 0.2f, 0.01f);
        expectWithinAbsoluteError(soft - plain, 0.1f, 0.01f, "velocity 0.2 x 0.5 of the region");
        expectWithinAbsoluteError(hard - plain, 0.5f, 0.01f, "velocity 1 x 0.5 of the region");
        // KEY (of this note) on STRT: the note's own key, not the last note's.
        const float low = startOf(with(base, slot(0, mod::Src::Key, S::LfoStart, 0.5f)), 0.8f, 36);
        const float high = startOf(with(base, slot(0, mod::Src::Key, S::LfoStart, 0.5f)), 0.8f, 96);
        expectWithinAbsoluteError(low, 0.0f, 0.02f, "KEY -0.4 x 0.5 below STRT 0.2: held at the start");
        expectWithinAbsoluteError(high - plain, 0.3f, 0.02f);
        // An LFO that starts with the key is read at its start: a square's first half.
        const float lfoStart = startOf(with(with(base, lfo(0, S::LfoStart, 0.4f, 1.0f, 1)), { { R::lfoId(0, R::Shape), (float) (int) LFO::Shape::Square } }), 0.8f);
        expectWithinAbsoluteError(std::abs(lfoStart - plain), 0.4f, 0.02f);
        // SLIC by velocity: the slice the SLIC cell chooses, moved.
        {
            SamplerEngine e;
            apply(e, with({ { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::OneShot }, { S::SliceCount, 8.0f } },
                          slot(0, mod::Src::Velocity, S::LfoSlice, 1.0f)));
            e.prepareToPlay(SR, 64);
            juce::AudioBuffer<float> buffer(2, 64);
            juce::MidiBuffer midi;
            e.noteOn(60, 0.55f, 0);
            expectEquals(e.getPlayingSlice(), 0, "as the cell is set, until the note's first sub-block");
            e.processBlock(buffer, midi);
            expectEquals(e.getPlayingSlice(), 4);
        }
    }

    // ---- ETIM ---------------------------------------------------------------------------------------
    // When the level (20 ms at a time) first reaches `level`, or first falls under it, from `from` on.
    static double timeTo(const Out& o, double level, bool rising, size_t from = 0)
    {
        const size_t window = 960, hop = 48;
        for (size_t at = from; at + window <= o.l.size(); at += hop)
            if ((rms(o.l, at, at + window) >= level) == rising)
                return (double) (at + window / 2 - from) / SR;
        return -1.0;
    }
    void envelopeTime()
    {
        beginTest("ETIM: the envelope's times as a destination, under a sounding note too");
        const std::vector<float> sine = tone();
        const ParamMap base { { S::Mode, (float) S::Loop }, { S::Attack, 0.1f }, { S::Release, 0.1f }, { S::VelSens, 0.0f } };
        const double full = 0.5 * 0.70710678 * 0.70710678 * 0.8;      // the sine's level x the centre's pan law x LEVL
        auto attack = [&](const ParamMap& sound)
        {
            SamplerEngine e;
            load(e, sine);
            apply(e, sound);
            const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.0);
            expectWithinAbsoluteError(rms(o.l, 38400, 48000), full, full * 0.02);
            return timeTo(o, full * 0.5, true);
        };
        const double plain = attack(base);
        const double twice = attack(with(base, slot(0, mod::Src::One, S::LfoEnvTime, 1.0f / 3.0f)));     // one of three octaves
        const double half = attack(with(base, slot(0, mod::Src::One, S::LfoEnvTime, -1.0f / 3.0f)));
        expectWithinAbsoluteError(plain, 0.05, 0.004);
        expectWithinAbsoluteError(twice, 0.1, 0.006, "attack " + juce::String(twice, 4) + " s for " + juce::String(plain, 4));
        expectWithinAbsoluteError(half, 0.025, 0.004, "attack " + juce::String(half, 4) + " s for " + juce::String(plain, 4));
        // Velocity shortens the release: the harder note is gone sooner (to a quarter in 0.75 of REL).
        auto release = [&](float velocity)
        {
            SamplerEngine e;
            load(e, sine);
            apply(e, with(base, with(slot(0, mod::Src::Velocity, S::LfoEnvTime, -2.0f / 3.0f), { { S::Release, 0.4f } })));
            const Out o = render(e, { { 0.0, On, 60, velocity }, { 0.5, Off, 60, 0.0f } }, 1.5);
            return timeTo(o, full * 0.25, false, 24000);
        };
        const double softRelease = release(0.25f), hardRelease = release(1.0f);
        expectWithinAbsoluteError(hardRelease, 0.75 * 0.4 * 0.25, 0.012, "velocity 1: two octaves shorter");
        expectWithinAbsoluteError(softRelease, 0.75 * 0.4 * std::pow(2.0, -0.5), 0.02, "velocity 0.25: half an octave shorter");
        // Under a sounding note: the strip brought up during a long attack ends it sooner, and no step.
        {
            SamplerEngine e;
            load(e, sine);
            apply(e, with(base, { { S::Attack, 1.0f }, { R::stripDestId(0), (float) S::LfoEnvTime }, { R::stripAmountId(0), -1.0f },
                                  { R::stripAmountId(1), 0.0f }, { R::stripAmountId(2), 0.0f } }));
            const Out moved = render(e, { { 0.0, On, 60, 1.0f }, { 0.25, Strip, 0, 1.0f } }, 1.5);
            SamplerEngine f;
            load(f, sine);
            apply(f, with(base, { { S::Attack, 1.0f } }));
            const Out still = render(f, { { 0.0, On, 60, 1.0f } }, 1.5);
            const double tMoved = timeTo(moved, full * 0.95, true), tStill = timeTo(still, full * 0.95, true);
            expect(tMoved > 0.3 && tMoved < 0.45, "full level after " + juce::String(tMoved, 3) + " s");
            expectWithinAbsoluteError(tStill, 0.95, 0.02);
            float step = 0.0f;
            for (size_t i = 11000; i < 20000; ++i)
                step = std::max(step, std::abs(moved.l[i] - 2.0f * moved.l[i - 1] + moved.l[i - 2]));
            float usual = 0.0f;
            for (size_t i = 60000; i < 65000; ++i)
                usual = std::max(usual, std::abs(still.l[i] - 2.0f * still.l[i - 1] + still.l[i - 2]));
            expect(step < usual * 1.5f, "no step where the time changes: " + juce::String(step, 6) + " against " + juce::String(usual, 6));
        }
        // A note that is giving way keeps its fast fade whatever ETIM says (an LFO keeps moving
        // it: every sub-block has another factor for the times).
        {
            SamplerEngine e;
            load(e, sine);
            apply(e, with(base, with(lfo(0, S::LfoEnvTime, 1.0f, 7.0f, 0), { { S::Voices, 1.0f }, { S::Attack, 0.001f }, { S::Release, 0.5f } })));
            const Out o = render(e, { { 0.0, On, 60, 1.0f }, { 0.5, On, 84, 1.0f } }, 1.0);
            // (the first note is 220 Hz; 20 ms after the second it must be gone)
            const double left = fastmeasure::goertzel(o.l, 24960, 4800, 220.0, SR), before = fastmeasure::goertzel(o.l, 14400, 4800, 220.0, SR);
            expect(left < before * 1.0e-4, "the note that gave way: " + juce::String(left / before, 6));
        }
    }

    // ---- POS, GRN, SLEV -----------------------------------------------------------------------------
    static double balance(const Out& o, size_t from, size_t to)
    {
        const double l = rms(o.l, from, to), r = rms(o.r, from, to);
        return (r - l) / std::max(l + r, 1.0e-12);      // -1 left .. 1 right
    }
    // The balance a pan gives (the sampler's law).
    static double balanceOfPan(double p)
    {
        const double l = std::cos((p + 1.0) * 0.25 * 3.141592653589793), r = std::sin((p + 1.0) * 0.25 * 3.141592653589793);
        return (r - l) / (l + r);
    }
    void ownSources()
    {
        const int pos = S::sources().placeOfPublished(S::PubPos), grn = S::sources().placeOfPublished(S::PubGrain),
                  slev = S::sources().placeOfPublished(S::PubLevel);
        const std::vector<float> sine = tone();
        beginTest("POS: the pan follows where the sample is");
        {
            // Two seconds of tone once through (1SHT), PAN hard left and POS on it at full: the pan
            // is POS - 1, so the sound travels from the left to the centre.
            SamplerEngine e;
            load(e, sine);
            apply(e, with({ { S::Mode, (float) S::OneShot }, { S::Pan, -1.0f } }, slot(0, pos, S::LfoPan, 1.0f)));
            const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 2.0);
            for (double at : { 0.1, 0.25, 0.5, 0.75, 0.9 })
                expectWithinAbsoluteError(balance(o, (size_t) (at * 96000.0) - 480, (size_t) (at * 96000.0) + 480), balanceOfPan(at - 1.0), 0.02,
                                          "at " + juce::String(at, 2) + " of the sample");
            // An octave up it is through in one second: POS is the place in the sample, not the time.
            SamplerEngine up;
            load(up, sine);
            apply(up, with({ { S::Mode, (float) S::OneShot }, { S::Pan, -1.0f } }, slot(0, pos, S::LfoPan, 1.0f)));
            const Out q = render(up, { { 0.0, On, 72, 1.0f } }, 1.0);
            expectWithinAbsoluteError(balance(q, 24000 - 480, 24000 + 480), balanceOfPan(-0.5), 0.02);
            // Between STRT and END it is the place between them.
            SamplerEngine part;
            load(part, sine);
            apply(part, with({ { S::Mode, (float) S::OneShot }, { S::Pan, -1.0f }, { S::StartPoint, 0.5f }, { S::EndPoint, 0.75f } },
                             slot(0, pos, S::LfoPan, 1.0f)));
            const Out r = render(part, { { 0.0, On, 60, 1.0f } }, 0.5);
            expectWithinAbsoluteError(balance(r, 12000 - 480, 12000 + 480), balanceOfPan(-0.5), 0.02, "half way from STRT to END");
            // ... and in a slice, the place in the slice.
            SamplerEngine sliced;
            load(sliced, sine);
            apply(sliced, with({ { S::Mode, (float) S::OneShot }, { S::Pan, -1.0f }, { S::SliceCount, 4.0f }, { S::SliceSelect, 0.6f } },
                               slot(0, pos, S::LfoPan, 1.0f)));
            const Out t = render(sliced, { { 0.0, On, 60, 1.0f } }, 0.5);
            expectWithinAbsoluteError(balance(t, 12000 - 480, 12000 + 480), balanceOfPan(-0.5), 0.02, "half way through the slice");
        }

        beginTest("POS and GRN in GRAN mode: the scan position, and where the grains are taken from");
        {
            auto pan = [&](int source, const ParamMap& more, double at)
            {
                SamplerEngine e;
                load(e, sine);
                apply(e, with(with({ { S::Mode, (float) S::Granular }, { S::Pan, -1.0f }, { S::GrainDensity, 100.0f }, { S::GrainSize, 0.02f } },
                                   more), slot(0, source, S::LfoPan, 1.0f)));
                const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.5);
                return balance(o, (size_t) (at * SR) - 2400, (size_t) (at * SR) + 2400);
            };
            // The scan runs through the two seconds at the sample's speed: POS after one second is a half.
            expectWithinAbsoluteError(pan(pos, { { S::GrainScan, 1.0f } }, 1.0), balanceOfPan(-0.5), 0.03);
            expectWithinAbsoluteError(pan(grn, { { S::GrainScan, 1.0f } }, 1.0), balanceOfPan(-0.5), 0.03);
            // Frozen at STRT: POS is 0 (the start of STRT..END), GRN is where that is in the sample.
            const ParamMap frozen { { S::GrainScan, 0.0f }, { S::StartPoint, 0.25f } };
            expectWithinAbsoluteError(pan(pos, frozen, 1.0), -1.0, 0.01);
            expectWithinAbsoluteError(pan(grn, frozen, 1.0), balanceOfPan(-0.75), 0.03);
            // GRN follows GPOS: half of STRT..END (0.75 of the sample) further on.
            expectWithinAbsoluteError(pan(grn, with(frozen, slot(1, mod::Src::One, S::LfoGrainPos, 0.5f)), 1.0), balanceOfPan(-0.375), 0.03);
            expectWithinAbsoluteError(pan(pos, with(frozen, slot(1, mod::Src::One, S::LfoGrainPos, 0.5f)), 1.0), -1.0, 0.01, "POS does not");
            // ... and SPRY: the grains' places scatter, GRN with them (the pan is no longer one place).
            SamplerEngine e;
            load(e, sine);
            apply(e, with(with({ { S::Mode, (float) S::Granular }, { S::Pan, -1.0f }, { S::GrainDensity, 100.0f }, { S::GrainSize, 0.02f } },
                               with(frozen, { { S::GrainSpray, 1.0f } })), slot(0, grn, S::LfoPan, 1.0f)));
            const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.5);
            double lo = 1.0, hi = -1.0;
            for (size_t at = 24000; at + 480 <= 72000; at += 480)
            {
                lo = std::min(lo, balance(o, at, at + 480));
                hi = std::max(hi, balance(o, at, at + 480));
            }
            expect(hi - lo > 0.08, "GRN moves with SPRY: balance " + juce::String(lo, 3) + " .. " + juce::String(hi, 3));
        }

        beginTest("SLEV: the sample's own level");
        {
            // Noise, loud for a second and then 20 dB down. PAN hard left and SLEV on it: the loud
            // half comes to the centre (its level is the sample's loudest: 1), the quiet half
            // stays a tenth of the way.
            SamplerEngine e;
            load(e, loudThenQuiet());
            apply(e, with({ { S::Mode, (float) S::OneShot }, { S::Pan, -1.0f } }, slot(0, slev, S::LfoPan, 1.0f)));
            const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 2.0);
            // (Noise: a hop's level is within a decibel or so of the loudest hop's.)
            const double loud = balance(o, 24000, 43200);
            expect(loud > balanceOfPan(-0.2) && loud < 0.01, "the loud half: " + juce::String(loud, 3));
            expectWithinAbsoluteError(balance(o, 57600, 91200), balanceOfPan(-0.9), 0.03, "the quiet half");
            // A file loaded later is analysed too, and one that is silent gives 0 (no division by nothing).
            SamplerEngine silent;
            load(silent, std::vector<float>(48000, 0.0f));
            apply(silent, with({ { S::Mode, (float) S::OneShot } }, slot(0, slev, S::LfoPan, 1.0f)));
            const Out none = render(silent, { { 0.0, On, 60, 1.0f } }, 0.2);
            for (float x : none.l) expect(std::isfinite(x) && x == 0.0f);
        }

        beginTest("A source nobody reads is not worked out; one that is read and aimed nowhere leaves the plain sound");
        {
            SamplerEngine e;
            OldSamplerEngine o;
            apply(e, { { S::Mode, (float) S::Loop } });
            apply(o, { { S::Mode, (float) S::Loop } });
            apply(e, slot(0, pos, 0, 1.0f));
            apply(e, slot(1, slev, 0, 1.0f));
            const Out a = render(o, gesture(), GESTURE_SECONDS), b = render(e, gesture(), GESTURE_SECONDS);
            expect(a.l == b.l && a.r == b.r);
        }
    }

    // ---- pad pressure --------------------------------------------------------------------------------
    void pressure()
    {
        beginTest("The pressure map: VOL, FLT, TBR (drive), WAV (grain position), PAN and VIB; no route, no response");
        const Events pressed { { 0.0, On, 60, 0.9f }, { 0.4, Press, 60, 1.0f }, { 1.4, Off, 60, 0.0f } };
        using D = AftertouchRouter::Destination;
        auto play = [&](const ParamMap& sound, D dest, float depth, bool press = true)
        {
            SamplerEngine e;
            apply(e, sound);
            AftertouchRouter routes;
            routes.clear();
            routes.setRoute(0, { dest, depth, false });
            e.setAftertouchRouter(&routes);
            return render(e, press ? pressed : Events { pressed[0], pressed[2] }, 1.5);
        };
        const ParamMap filtered { { S::Mode, (float) S::Loop }, { S::FilterCutoff, 0.45f } };
        auto centroid = [](const Out& o) { float r = 0.0f, c = 0.0f; fastmeasure::rmsAndCentroid(std::vector<float>(o.l.begin() + 33600, o.l.begin() + 62400), 2048, SR, r, c); return c; };
        const Out rest = play(filtered, D::FilterCutoff, 0.5f, false), open = play(filtered, D::FilterCutoff, 0.5f);
        expect(centroid(open) > centroid(rest) * 1.3f, "FLT: " + juce::String(centroid(rest), 0) + " Hz -> " + juce::String(centroid(open), 0) + " Hz");
        // VOL: the level, twice as loud under a full press at depth 1.
        const Out vol = play(filtered, D::Volume, 1.0f);
        expectWithinAbsoluteError(db(rms(vol.l, 33600, 62400) / rms(rest.l, 33600, 62400)), 6.02, 0.2);
        // No route, no response: an engine that was given no routes (as every sampler track is
        // made), and one whose routes are all off, are sample for sample the unpressed sound.
        {
            SamplerEngine e;
            apply(e, filtered);
            const Out none = render(e, pressed, 1.5);
            expect(none.l == rest.l && none.r == rest.r);
            const Out off = play(filtered, D::Off, 1.0f);
            expect(off.l == rest.l && off.r == rest.r);
            // A destination the sampler has no meaning for moves nothing either.
            const Out fm = play(filtered, D::FMDepth, 1.0f);
            expect(fm.l == rest.l && fm.r == rest.r);
        }
        // WAV: where the grains are taken from. Noise that is loud for its first second and 20 dB
        // down for its second, frozen at the start: the press takes the grains from 0.6 of it.
        {
            auto level = [&](bool press)
            {
                SamplerEngine e;
                load(e, loudThenQuiet());
                apply(e, { { S::Mode, (float) S::Granular }, { S::GrainScan, 0.0f } });
                AftertouchRouter routes;
                routes.clear();
                routes.setRoute(0, { D::WavePosition, 0.6f, false });
                e.setAftertouchRouter(&routes);
                const Out o = render(e, press ? pressed : Events { pressed[0], pressed[2] }, 1.5);
                return rms(o.l, 33600, 62400);
            };
            expectWithinAbsoluteError(db(level(true) / level(false)), -20.0, 1.5, "WAV");
        }
        const Out pan = play(filtered, D::PanSpread, 1.0f);
        expect(balance(pan, 33600, 62400) > 0.8 && std::abs(balance(rest, 33600, 62400)) < 0.01);
        const Out drive = play({ { S::Mode, (float) S::Loop } }, D::MacroTimbre, 1.0f), clean = play({ { S::Mode, (float) S::Loop } }, D::MacroTimbre, 1.0f, false);
        expect(rms(drive.l, 33600, 62400) > rms(clean.l, 33600, 62400) * 1.5);
        const Out vib = play({ { S::Mode, (float) S::Loop } }, D::Vibrato, 1.0f);
        expect(vib.l != clean.l, "VIB");
        // Each voice its own pad: the note that is not pressed stays where it was.
        {
            SamplerEngine e;
            apply(e, { { S::Mode, (float) S::Loop }, { S::Pan, -1.0f } });
            AftertouchRouter routes;
            routes.clear();
            routes.setRoute(0, { D::PanSpread, 1.0f, false });
            e.setAftertouchRouter(&routes);
            const Out one = render(e, { { 0.0, On, 60, 0.9f }, { 0.0, On, 72, 0.9f }, { 0.3, Press, 72, 1.0f } }, 1.2);
            // (Both hard left; the pressed one comes to the centre: the right side holds one note.)
            const double high = fastmeasure::goertzel(one.r, 28800, 19200, 523.25, SR), low = fastmeasure::goertzel(one.r, 28800, 19200, 261.63, SR);
            const double lowLeft = fastmeasure::goertzel(one.l, 28800, 19200, 261.63, SR);
            expect(lowLeft > 0.0, "the test hears the low note on the left");
            expect(low < high * 0.001 || low < lowLeft * 1.0e-6, "only the pressed note moved: " + juce::String(low, 9) + " against " + juce::String(high, 9));
        }
    }

    // ---- the strip's share of DRIV --------------------------------------------------------------------
    void stripDrive()
    {
        beginTest("The strip's drive: the level is taken back for the STRIP page's routes to DRIV, and only for them");
        const Events up { { 0.0, On, 60, 0.9f }, { 0.0, Strip, 0, 1.0f } };
        auto level = [&](const ParamMap& sound, const Events& events)
        {
            SamplerEngine e;
            apply(e, with({ { S::Mode, (float) S::Loop }, { S::DriveAmt, 0.1f } }, sound));
            const Out o = render(e, events, 1.5);
            return rms(o.l, 48000, 72000);
        };
        const ParamMap noOthers { { R::stripAmountId(1), 0.0f }, { R::stripAmountId(2), 0.0f } };
        const double rest = level(noOthers, { up[0] });
        const double page = level(noOthers, up);                                                // DRIV + 0.7 from the page
        const double set = level(with(noOthers, { { S::DriveAmt, 0.8f } }), { up[0] });          // DRIV 0.8 with no strip
        const double free = level(with(with(noOthers, { { R::stripAmountId(0), 0.0f } }), slot(0, mod::Src::Strip, S::LfoDrive, 0.7f)), up);
        expectWithinAbsoluteError(db(free / set), 0.0, 0.1, "a free slot from STRP is drive as asked for");
        const double factor = std::pow((1.0 + 0.1 * 10.0) / (1.0 + 0.8 * 10.0), 0.58);
        expectWithinAbsoluteError(db(page / set), db(factor), 0.1, "the page's route: the level taken back by " + juce::String(db(factor), 2) + " dB");
        // (What the rule is for: without it the page's route would be 5.5 dB louder than this.)
        expect(set > page * 1.5 && rest > 1.0e-3, "levels " + juce::String(rest, 4) + " at rest, " + juce::String(page, 4) + " with the strip, "
                                                  + juce::String(set, 4) + " with DRIV set there");
    }

    // ---- the track input at audio rate ---------------------------------------------------------------
    void audioRate()
    {
        beginTest("XIN at audio rate reaches PTCH, AMP, CUT, PAN and the grains' place");
        std::vector<float> input((size_t) (2.0 * SR));
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = (float) std::sin(2.0 * 3.141592653589793 * 700.0 * (double) i / SR);
        auto play = [&](const ParamMap& sound)
        {
            SamplerEngine e;
            apply(e, sound);
            return render(e, { { 0.0, On, 60, 0.9f } }, 1.0, 128, &input);
        };
        const ParamMap tone { { S::Mode, (float) S::Loop }, { S::FilterCutoff, 0.6f } };
        const Out plain = play(tone);
        const int xin = mod::Src::Xin;
        for (int dest : { (int) S::LfoPitch, (int) S::LfoAmp, (int) S::LfoCutoff, (int) S::LfoPan })
        {
            const Out moved = play(with(tone, slot(0, xin, dest, 0.8f)));
            // Side-bands 700 Hz either side of the tone's partials: energy that was not there.
            std::vector<float> diff(moved.l.size());
            for (size_t i = 0; i < diff.size(); ++i) diff[i] = moved.l[i] - plain.l[i];
            const double changed = rms(diff, 24000, 48000) / rms(plain.l, 24000, 48000);
            expect(changed > 0.05, juce::String(S::lfoDests()[dest].label) + ": changed by " + juce::String(changed, 3));
            float peak = 0.0f;
            for (float x : moved.l) peak = std::max(peak, std::abs(x));
            expect(std::isfinite(peak) && peak < 8.0f);
        }
        // AMP at -100 from ONE and +100 from XIN: ring modulation (the tone itself is gone).
        {
            const Out ring = play(with(tone, with(slot(0, mod::Src::One, S::LfoAmp, -1.0f), slot(1, xin, S::LfoAmp, 1.0f))));
            SamplerEngine probe;
            apply(probe, tone);
            const double hz = 261.63;       // (the tone's pitch at the root key is the bank's; measured, not assumed)
            const double carrier = fastmeasure::goertzel(plain.l, 24000, 19200, hz, SR);
            const double left = fastmeasure::goertzel(ring.l, 24000, 19200, hz, SR);
            if (carrier > 1.0e-6)
                expect(left < carrier * 0.05, "carrier " + juce::String(left / carrier, 4));
            expect(rms(ring.l, 24000, 48000) > 0.01);
        }
        // GPOS per sample: grains are taken from where the input says at the sample they start on.
        {
            const ParamMap cloud { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.0f },
                                   { S::StartPoint, 0.5f }, { S::GrainDensity, 150.0f }, { S::GrainSize, 0.03f } };
            const Out still = play(cloud), moved = play(with(cloud, slot(0, xin, S::LfoGrainPos, 0.3f)));
            expect(still.l != moved.l);
            SamplerEngine e;
            apply(e, with(cloud, slot(0, xin, S::LfoGrainPos, 0.3f)));
            e.prepareToPlay(SR, 128);
            e.noteOn(60, 0.9f, 0);
            float lo = 1.0f, hi = 0.0f;
            juce::MidiBuffer midi;
            for (int at = 0; at + 128 <= 48000; at += 128)
            {
                AudioInput in;
                in.left = in.right = input.data() + at;
                in.frames = 128;
                e.setAudioInput(in);
                juce::AudioBuffer<float> buffer(2, 128);
                buffer.clear();
                e.processBlock(buffer, midi);
                float heads[80];
                bool grain[80];
                const int n = e.getPlayheads(heads, 80, grain);
                for (int i = 0; i < n; ++i)
                    if (grain[i]) { lo = std::min(lo, heads[i]); hi = std::max(hi, heads[i]); }
            }
            // (STRT 0.5: the region is the second half, so +-0.3 of it is +-0.15 of the sample.)
            expect(lo < 0.40f && lo > 0.33f && hi > 0.60f && hi < 0.67f, "grains from " + juce::String(lo, 2) + " to " + juce::String(hi, 2) + " of the sample");
        }
    }


    // =================================================================================================
    // Added by the review of the port.
    // =================================================================================================

    // ---- other sample rates, blocks that are not whole sub-blocks --------------------------------------
    void otherRates()
    {
        beginTest("44.1 and 96 kHz, blocks that are no whole number of sub-blocks: old against new");
        const float brk = (float) breakSample();
        const ParamMap loop { { S::Mode, (float) S::Loop } };
        struct Case { const char* name; ParamMap sound; bool exact; };
        const std::vector<Case> cases {
            { "LOOP tone", loop, true },
            { "FWD break, filter", { { S::SampleSelect, brk }, { S::Mode, (float) S::Forward }, { S::FilterCutoff, 0.5f }, { S::FilterReso, 0.5f },
                                     { S::FilterEnv, 0.3f } }, true },
            { "GRAN, nothing drawn", { { S::SampleSelect, brk }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.5f } }, true },
            { "stretch CYCL", { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::StretchMode, (float) S::StretchCycle }, { S::StretchSpeed, 0.7f } }, true },
            { "stretch WSOL", { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::StretchMode, (float) S::StretchWsol }, { S::StretchSpeed, 0.7f } }, true },
            { "LFO1 > STRT", with(soundFor(S::LfoStart), lfo(0, S::LfoStart, 0.5f)), true },
            { "LFO1 > PTCH", with(loop, lfo(0, S::LfoPitch, 0.3f)), false },
            { "LFO1 > AMP", with(loop, lfo(0, S::LfoAmp, 0.5f)), false },
            { "LFO1 > CUT", with(soundFor(S::LfoCutoff), lfo(0, S::LfoCutoff, 0.5f)), false },
            { "LFO1 > GPOS", with(soundFor(S::LfoGrainPos), lfo(0, S::LfoGrainPos, 0.5f)), false },
            { "strip defaults", loop, false } };
        struct Rate { double sr; int block; };
        for (Rate rate : { Rate { 44100.0, 100 }, Rate { 96000.0, 333 }, Rate { 48000.0, 7 } })
            for (auto& c : cases)
            {
                Events events = gesture();
                if (juce::String(c.name) == "strip defaults")
                    events.insert(events.begin() + 1, { 0.2, Strip, 0, 1.0f });
                OldSamplerEngine o;
                SamplerEngine n;
                apply(o, c.sound);
                apply(n, c.sound);
                const Out a = render(o, events, GESTURE_SECONDS, rate.block, nullptr, rate.sr),
                          b = render(n, events, GESTURE_SECONDS, rate.block, nullptr, rate.sr);
                const Verdict v = compare(a, b);
                const juce::String name = juce::String(c.name) + " at " + juce::String(rate.sr, 0) + " Hz, blocks of " + juce::String(rate.block);
                logMessage("[u4-smp] " + name.paddedRight(' ', 46) + v.text());
                expect(rms(a.l, 0, a.l.size()) > 1.0e-4, name + ": the old render is silent");
                if (c.exact)
                    expect(v.identical, name + " is no longer what it was, sample for sample: " + v.text());
                else
                    expect(v.word != "LOST", name + ": " + v.text());
            }
    }

    // ---- the grains' draws, read from the engines ------------------------------------------------------
    // What every grain of a cloud was given - its place in the sample, its pitch, its side, whether it
    // runs backwards, and how long after the last one it began - read from the engine's own grains on
    // each grain's first sample (blocks of one sample), not guessed from the sound. Four held notes.
    struct GrainDraws
    {
        std::vector<double> jump, pitch, side, gap;     // jump: a grain's place less the place of the one before it (every other pair)
        int reversed = 0, total = 0;
    };
    struct SeenGrain { double pos; float ratio, phase, phaseInc, gainL, gainR; };
    static SeenGrain seenGrain(const OldSamplerEngine& e, int voice, int g)
    {
        const auto& x = e.voices[voice].grains[g];
        return { x.pos, x.ratio, x.phase, x.phaseInc, x.gainL, x.gainR };
    }
    static SeenGrain seenGrain(const SamplerEngine& e, int voice, int g)
    {
        const auto& x = e.getGrainCloud(voice).grain(g);
        return { x.pos, x.ratio, x.phase, x.phaseInc, x.gainL, x.gainR };
    }
    static int grainsOf(const OldSamplerEngine& e, int voice) { return e.voices[voice].numGrains; }
    static int grainsOf(const SamplerEngine& e, int voice)    { return e.getGrainCloud(voice).sounding(); }

    template <typename E>
    static GrainDraws grainDraws(const ParamMap& sound, uint32_t seed, double seconds)
    {
        E e;
        apply(e, sound);
        e.setGrainSeed(seed);
        e.prepareToPlay(SR, 1);
        for (int note : { 48, 55, 60, 67 })
            e.noteOn(note, 0.8f, 0);
        GrainDraws d;
        const double length = (double) std::max(1, e.getSampleLength());
        std::array<long, S::POOL_VOICES> lastAt;
        std::array<double, S::POOL_VOICES> lastPlace;
        std::array<int, S::POOL_VOICES> count {};
        lastAt.fill(-1);
        lastPlace.fill(0.0);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buffer(2, 1);
        const long total = (long) (seconds * SR);
        for (long at = 0; at < total; ++at)
        {
            buffer.clear();
            e.processBlock(buffer, midi);
            for (int vi = 0; vi < S::POOL_VOICES; ++vi)
            {
                if (e.getVoicePosition(vi) < 0.0)
                    continue;
                const auto v = (size_t) vi;
                for (int g = 0, n = grainsOf(e, vi); g < n; ++g)
                {
                    const SeenGrain x = seenGrain(e, vi, g);
                    if (! (x.phase > 0.5f * x.phaseInc && x.phase <= 1.5f * x.phaseInc))
                        continue;       // not on its first sample
                    ++d.total;
                    if (x.ratio < 0.0f) ++d.reversed;
                    d.pitch.push_back(12.0 * std::log2((double) std::max(1.0e-6f, std::abs(x.ratio))));
                    d.side.push_back(std::atan2((double) x.gainR, (double) x.gainL));
                    const double place = x.pos / length;
                    if (lastAt[v] >= 0)
                    {
                        d.gap.push_back((double) (at - lastAt[v]) / SR);
                        if (count[v] % 2 == 1)
                            d.jump.push_back(place - lastPlace[v]);
                    }
                    lastAt[v] = at;
                    lastPlace[v] = place;
                    ++count[v];
                }
            }
        }
        return d;
    }
    // Kolmogorov and Smirnov's distance of two sets of numbers: the largest gap between their
    // cumulative distributions, 0 (the same) .. 1.
    static double ksDistance(std::vector<double> a, std::vector<double> b)
    {
        if (a.empty() || b.empty())
            return a.empty() && b.empty() ? 0.0 : 1.0;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        size_t i = 0, j = 0;
        double d = 0.0;
        while (i < a.size() && j < b.size())
        {
            const double x = std::min(a[i], b[j]);
            while (i < a.size() && a[i] <= x) ++i;
            while (j < b.size() && b[j] <= x) ++j;
            d = std::max(d, std::abs((double) i / (double) a.size() - (double) j / (double) b.size()));
        }
        return d;
    }
    // What that distance exceeds once in a thousand pairs of sets drawn from ONE distribution.
    static double ksLimit(size_t n, size_t m)
    {
        return 1.95 * std::sqrt((double) (n + m) / std::max(1.0, (double) n * (double) m));
    }
    // LOST when the old engine's grains and the new one's cannot be draws of the same cloud.
    juce::String drawsPair(const juce::String& name, const ParamMap& sound, double seconds)
    {
        const GrainDraws o = grainDraws<OldSamplerEngine>(sound, 100u, seconds), n = grainDraws<SamplerEngine>(sound, 100u, seconds);
        struct Row { const char* what; const std::vector<double> GrainDraws::* field; };
        static const Row rows[] = { { "place", &GrainDraws::jump }, { "pitch", &GrainDraws::pitch }, { "side", &GrainDraws::side },
                                    { "interval", &GrainDraws::gap } };
        bool same = true;
        juce::String text = juce::String(o.total) + " -> " + juce::String(n.total) + " grains";
        // (Their number: the clock. The intervals are scattered, so a count wanders by a few roots of itself.)
        const bool countOk = std::abs(o.total - n.total) <= 3.3 * std::sqrt((double) (o.total + n.total));
        same = same && countOk && o.total > 500;
        text += countOk ? "; " : " !; ";
        for (auto& row : rows)
        {
            const auto& a = o.*row.field;
            const auto& b = n.*row.field;
            const double d = ksDistance(a, b), limit = ksLimit(a.size(), b.size());
            same = same && d <= limit;
            text += juce::String(row.what) + " " + juce::String(d, 3) + " (limit " + juce::String(limit, 3) + (d <= limit ? "); " : ") !; ");
        }
        const double po = (double) o.reversed / std::max(1, o.total), pn = (double) n.reversed / std::max(1, n.total);
        const double p = (double) (o.reversed + n.reversed) / std::max(1, o.total + n.total);
        const double allowed = 3.3 * std::sqrt(p * (1.0 - p) * (1.0 / std::max(1, o.total) + 1.0 / std::max(1, n.total)));
        same = same && std::abs(po - pn) <= allowed;
        text += "backwards " + juce::String(po, 3) + " -> " + juce::String(pn, 3) + " (allowed " + juce::String(allowed, 3)
                + (std::abs(po - pn) <= allowed ? ")" : ") !");
        const juce::String word = same ? "SAME" : "LOST";
        logMessage("[u4-smp] draws: " + name.paddedRight(' ', 22) + word + "  " + text);
        return word;
    }
    void grainDrawsCompared()
    {
        beginTest("The grains' draws, read from the engines: place, pitch, side, direction and interval are the old cloud's");
        // (The six-seed rule above hears a cloud's level, its mean frequency, its width and its
        // unevenness: it cannot tell a pitch scatter of half the size, and its mean-frequency limit
        // is semitones wide. This reads what each grain was given.)
        const ParamMap cloud { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.3f },
                               { S::GrainSize, 0.12f }, { S::GrainDensity, 80.0f }, { S::GrainSpray, 0.5f }, { S::GrainPitchJit, 0.3f },
                               { S::GrainSpread, 0.8f }, { S::GrainReverse, 0.4f } };
        expect(drawsPair("GRAN all four", cloud, 8.0) == "SAME");
        for (auto& preset : FactoryPresets::getAll())
            if (preset.name == "Abyss Drone")
                expect(drawsPair("preset Abyss Drone", preset.soundParams, 16.0) == "SAME");
        const ParamMap stretched { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Loop }, { S::StretchSpeed, 0.6f },
                                   { S::StretchMode, (float) S::StretchGrain }, { S::StretchJitter, 0.6f }, { S::StretchSpread, 0.8f } };
        expect(drawsPair("stretch GRAN JIT SPRD", stretched, 8.0) == "SAME");
        expect(drawsPair("stretch SMER", with(stretched, { { S::StretchMode, (float) S::StretchSmear } }), 8.0) == "SAME");
    }

    // ---- single cases the review's mutations of the engine came through --------------------------------
    void reviewCases()
    {
        const int slev = S::sources().placeOfPublished(S::PubLevel);
        beginTest("SLEV on a low note is its level, not its waveform");
        {
            // A steady 55 Hz tone; PAN hard left and SLEV on it at full: a steady level is a steady
            // place (the centre). One hop of 2.9 ms is a sixth of this tone's period.
            std::vector<float> low((size_t) (2.0 * SR));
            for (size_t i = 0; i < low.size(); ++i)
                low[i] = 0.5f * (float) std::sin(2.0 * 3.141592653589793 * 55.0 * (double) i / SR);
            SamplerEngine e;
            load(e, low);
            apply(e, with({ { S::Mode, (float) S::OneShot }, { S::Pan, -1.0f } }, slot(0, slev, S::LfoPan, 1.0f)));
            const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.6);
            double lo = 1.0, hi = -1.0;
            for (size_t at = 24000; at + 96 <= 72000; at += 96)
            {
                lo = std::min(lo, balance(o, at, at + 96));
                hi = std::max(hi, balance(o, at, at + 96));
            }
            logMessage("[u4-smp] SLEV on 55 Hz: balance " + juce::String(lo, 3) + " .. " + juce::String(hi, 3));
            expect(lo > balanceOfPan(-0.2) && hi < 0.01, "balance " + juce::String(lo, 3) + " .. " + juce::String(hi, 3));
            // A sample shorter than one hop has a level too.
            SamplerEngine tiny;
            load(tiny, std::vector<float>(100, 0.5f));
            apply(tiny, with({ { S::Mode, (float) S::Loop }, { S::Pan, -1.0f } }, slot(0, slev, S::LfoPan, 1.0f)));
            const Out t = render(tiny, { { 0.0, On, 60, 1.0f } }, 0.2);
            expectWithinAbsoluteError(balance(t, 4800, 9600), 0.0, 0.01, "a hundred frames of sound");
        }

        const float brk = (float) breakSample();
        const ParamMap loop { { S::Mode, (float) S::Loop } };
        auto levelOf = [&](const ParamMap& sound)
        {
            SamplerEngine e;
            apply(e, sound);
            const Out o = render(e, { { 0.0, On, 60, 0.9f } }, 1.0);
            return rms(o.l, 24000, 48000);
        };

        beginTest("AMP is held to 0..4, not to 2: two sources that sum above 2 are heard");
        {
            const double plain = levelOf(loop);
            const double three = levelOf(with(loop, with(slot(0, mod::Src::One, S::LfoAmp, 1.0f), slot(1, mod::Src::One, S::LfoAmp, 1.0f))));
            expectWithinAbsoluteError(db(three / plain), 9.54, 0.2, "a gain of 1 + 1 + 1");
            const double held = levelOf(with(loop, with(with(slot(0, mod::Src::One, S::LfoAmp, 1.0f), slot(1, mod::Src::One, S::LfoAmp, 1.0f)),
                                                        with(slot(2, mod::Src::One, S::LfoAmp, 1.0f), slot(3, mod::Src::One, S::LfoAmp, 1.0f)))));
            expectWithinAbsoluteError(db(held / plain), 12.04, 0.2, "1 + 4 is held to 4");
            // ... and to 0 below: two slots at -1 (1 - 1 - 1) are silence, not the note upside down.
            const double under = levelOf(with(loop, with(slot(0, mod::Src::One, S::LfoAmp, -1.0f), slot(1, mod::Src::One, S::LfoAmp, -1.0f))));
            expect(under < plain * 1.0e-3, "1 - 2 is held to 0: " + juce::String(db(under / plain), 1) + " dB");
        }

        beginTest("A slot on CUT is heard with the CUT cell fully open; NORM is kept under a moved voice");
        {
            // (With CUT at the top and no envelope the filter is bypassed - unless something moves it.)
            expectClose("LFO1 > CUT, CUT open", with(loop, lfo(0, S::LfoCutoff, -0.9f)), false);
            // (On the break, not the default tone: 4 octaves under 20 kHz is 1.25 kHz, above most of
            // the tone, which loses only 0.4 dB there. The old engine is the reference for how much.)
            const ParamMap bright { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop } };
            const ParamMap shutSound = with(bright, slot(0, mod::Src::One, S::LfoCutoff, -1.0f));
            // The old engine switched the filter on for an LFO's route only, so it did not hear a
            // slot on CUT with the cell open at all: the new one does (a change in kind, wanted).
            auto darker = [&](auto& plainEngine, auto& shutEngine)
            {
                apply(plainEngine, bright);
                apply(shutEngine, shutSound);
                return compare(render(plainEngine, gesture(), GESTURE_SECONDS), render(shutEngine, gesture(), GESTURE_SECONDS)).centroid;
            };
            OldSamplerEngine oldPlain, oldShut;
            SamplerEngine newPlain, newShut;
            const double oldRatio = darker(oldPlain, oldShut), newRatio = darker(newPlain, newShut);
            logMessage("[u4-smp] slot ONE > CUT -1, CUT open: centroid x" + juce::String(oldRatio, 3) + " (old), x" + juce::String(newRatio, 3) + " (new)");
            expect(newRatio < 0.7, "four octaves down from the top: centroid x" + juce::String(newRatio, 3));
            // NORM: the level's factor is the same whether or not a slot moves the voice.
            auto quietLevel = [&](const ParamMap& more)
            {
                SamplerEngine e;
                std::vector<float> quiet = tone();
                for (float& x : quiet) x *= 0.25f;      // (peak 0.125: NORM is a factor of 8)
                load(e, quiet);
                apply(e, with({ { S::Mode, (float) S::Loop }, { S::Normalize, 1.0f } }, more));
                const Out o = render(e, { { 0.0, On, 60, 0.9f } }, 1.0);
                return rms(mono(o), 24000, 48000);
            };
            expectWithinAbsoluteError(db(quietLevel(slot(0, mod::Src::Key, S::LfoDrive, 0.5f)) / quietLevel({})), 0.0, 0.1,
                                      "NORM with a slot that moves nothing (KEY at the root key)");
            expect(db(quietLevel({}) / quietLevel({ { S::Normalize, 0.0f } })) > 17.0, "NORM itself");
        }

        beginTest("Grain and stretch cells under an LFO in the modes the first list left out; a moved voice at its first samples");
        {
            // SPD in GRAN mode is the scan's speed.
            expectClose("GRAN, LFO1 > SPD", with({ { S::SampleSelect, brk }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.5f } },
                                                 lfo(0, S::LfoSpeed, 0.8f)), false);
            // FADE and SLEN in the SLCE stretch follow while the note sounds.
            const ParamMap sliced { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::StretchMode, (float) S::StretchSlice },
                                    { S::StretchSpeed, 0.6f }, { S::SliceCount, 8.0f }, { S::SliceLen, 0.9f } };
            {
                const ParamMap moved = with(sliced, with(lfo(0, S::LfoSliceLen, -0.8f, 1.0f), { { R::lfoId(0, R::Shape), (float) (int) LFO::Shape::Square } }));
                expectClose("SLCE stretch, LFO1 > SLEN", moved, false);
                SamplerEngine still, cut;
                apply(still, sliced);
                apply(cut, moved);
                const Out a = render(still, gesture(), GESTURE_SECONDS), b = render(cut, gesture(), GESTURE_SECONDS);
                expect(rms(b.l, 0, b.l.size()) < rms(a.l, 0, a.l.size()) * 0.9, "shorter slices are less sound");
            }
            // PJIT under an LFO: each voice's own scatter, read from the grains.
            expect(drawsPair("GRAN, LFO1 > PJIT", with({ { S::SampleSelect, brk }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.3f },
                                                         { S::GrainSize, 0.12f }, { S::GrainDensity, 80.0f } },
                                                       with(lfo(0, S::LfoPitchJit, 1.0f, 0.5f), { { R::lfoId(0, R::Shape), (float) (int) LFO::Shape::Square } })), 6.0) == "SAME");
            // A voice that a slot moves counts RATE's held samples from the block, at its first samples too.
            expectMatch("slot KEY > PAN with RATE", with(loop, with({ { S::RateReduce, 0.3f } }, slot(0, mod::Src::Key, S::LfoPan, 1.0f))));
        }

        beginTest("ETIM is the decay's time too; a new note in a voice does not inherit the last note's pressure");
        {
            const std::vector<float> sine = tone();
            const double full = 0.5 * 0.70710678 * 0.70710678 * 0.8;
            auto decay = [&](const ParamMap& more)
            {
                SamplerEngine e;
                load(e, sine);
                apply(e, with({ { S::Mode, (float) S::Loop }, { S::Attack, 0.001f }, { S::Decay, 0.4f }, { S::Sustain, 0.0f }, { S::VelSens, 0.0f } }, more));
                const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.2);
                return timeTo(o, full * 0.5, false, 960);
            };
            const double plain = decay({}), twice = decay(slot(0, mod::Src::One, S::LfoEnvTime, 1.0f / 3.0f)),
                         half = decay(slot(0, mod::Src::One, S::LfoEnvTime, -1.0f / 3.0f));
            expectWithinAbsoluteError(plain, 0.18, 0.02, "the decay as set: half way after " + juce::String(plain, 3) + " s");
            expectWithinAbsoluteError(twice / plain, 2.0, 0.15, "decay " + juce::String(twice, 3) + " s for " + juce::String(plain, 3));
            expectWithinAbsoluteError(half / plain, 0.5, 0.06, "decay " + juce::String(half, 3) + " s for " + juce::String(plain, 3));

            // A pressed note ends; the next note takes the same voice and is not pressed: it stays left.
            SamplerEngine e;
            apply(e, with({ { S::Mode, (float) S::Loop }, { S::Pan, -1.0f }, { S::Release, 0.02f } }, slot(0, mod::Src::Pressure, S::LfoPan, 1.0f)));
            const Out o = render(e, { { 0.0, On, 60, 0.9f }, { 0.1, Press, 60, 1.0f }, { 0.4, Off, 60, 0.0f }, { 0.6, On, 62, 0.9f } }, 1.0);
            expect(balance(o, 9600, 19200) > -0.1, "the pressed note came to the centre: " + juce::String(balance(o, 9600, 19200), 3));
            expect(balance(o, 33600, 48000) < -0.99, "the next note is where PAN puts it: " + juce::String(balance(o, 33600, 48000), 3));
        }

        beginTest("GRN at a note's first samples is where the note begins, not the area's start");
        {
            // Frozen at three quarters of a steady tone, PAN hard left and GRN on it at full: the note
            // is a quarter left of the centre from its first millisecond on (the attack is 1 ms).
            SamplerEngine e;
            load(e, tone());
            apply(e, with({ { S::Mode, (float) S::Granular }, { S::Pan, -1.0f }, { S::GrainScan, 0.0f }, { S::StartPoint, 0.75f },
                            { S::GrainDensity, 100.0f }, { S::GrainSize, 0.02f } }, slot(0, S::sources().placeOfPublished(S::PubGrain), S::LfoPan, 1.0f)));
            const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 0.5);
            expectWithinAbsoluteError(balance(o, 24, 120), balanceOfPan(-0.25), 0.03, "the note's first 2.5 ms");
            expectWithinAbsoluteError(balance(o, 9600, 19200), balanceOfPan(-0.25), 0.03, "later");
        }

        beginTest("A sample change keeps the track silent for the copy, not for the analysis of a long sample");
        {
            // (The onset and level analysis ran inside the handshake: a 60 s stereo sample kept the
            // track silent 7 ms in Release, a 300 s one 36 ms - all of loadSample's time. The copy
            // alone is a fraction of it, as before the port.)
            const HoldOut h = holdOut<SamplerEngine>("new", 60.0);
            expect(h.silent < 0.5 * h.load, "silent " + juce::String(h.silent * 1000.0, 1) + " ms of a "
                                               + juce::String(h.load * 1000.0, 1) + " ms sample change");
        }

        beginTest("A slot taken off AMP under a sounding note that something else still moves: the level comes back");
        {
            // (KEY > PAN keeps the voice on the moved path; the AMP slot doubles the level, then is
            // aimed at nothing. The voice must not keep the last gain the AMP buffer gave it.)
            auto play = [&](bool ampFirst)
            {
                SamplerEngine e;
                apply(e, with(loop, with(slot(0, mod::Src::One, ampFirst ? S::LfoAmp : 0, 1.0f), slot(1, mod::Src::Key, S::LfoPan, 0.5f))));
                e.prepareToPlay(SR, 128);
                e.noteOn(62, 0.9f, 0);
                juce::AudioBuffer<float> buffer(2, 128);
                juce::MidiBuffer midi;
                std::vector<float> l;
                for (int k = 0; k < 750; ++k)       // 2 s
                {
                    if (k == 250)
                        e.setParameter(ModSlotParam::id(0, ModSlotParam::DEST), 0.0f);
                    buffer.clear();
                    e.processBlock(buffer, midi);
                    for (int i = 0; i < 128; ++i)
                        l.push_back(buffer.getSample(0, i));
                }
                return rms(l, 48000, 96000);
            };
            const double after = play(true), plain = play(false);
            expectWithinAbsoluteError(db(after / plain), 0.0, 0.1, "the level a second after AMP's slot was aimed away");
        }

        beginTest("POS, GRN and SLEV land at the same time whatever the host's block size");
        {
            // (An engine source handed over as "the last sub-block" moves with the host's block size.
            // The sampler's three are values published before each run, from where the voice is.)
            // A 220 Hz tone that falls 20 dB at 1 s; PAN hard left, SLEV on it at full: the note
            // moves left when the level falls. When, at blocks of 16, 7, 100, 128 and 333 samples.
            std::vector<float> step((size_t) (2.0 * SR));
            for (size_t i = 0; i < step.size(); ++i)
                step[i] = (i < (size_t) SR ? 0.5f : 0.05f) * (float) std::sin(2.0 * 3.141592653589793 * 220.0 * (double) i / SR);
            auto crossing = [&](int source, int block)
            {
                SamplerEngine e;
                load(e, step);
                apply(e, with({ { S::Mode, (float) S::Forward }, { S::Pan, -1.0f }, { S::Release, 0.5f } }, slot(0, source, S::LfoPan, 1.0f)));
                const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.6, block);
                const double mid = 0.5 * (balance(o, 24000, 43200) + balance(o, 57600, 72000));
                for (size_t at = 43200; at + 96 <= 72000; at += 12)
                    if (balance(o, at, at + 96) < mid)
                        return (double) at / SR;
                return -1.0;
            };
            const int slev = S::sources().placeOfPublished(S::PubLevel);
            const double at16 = crossing(slev, 16);
            juce::String text;
            double worst = 0.0;
            for (int block : { 7, 100, 128, 333 })
            {
                const double t = crossing(slev, block);
                text += juce::String(block) + ": " + juce::String((t - at16) * 1000.0, 2) + " ms  ";
                worst = std::max(worst, std::abs(t - at16));
            }
            logMessage("[u4-smp] SLEV's step re. blocks of 16 (at " + juce::String(at16, 4) + " s): " + text);
            // (SLEV is the level of the 23 ms ROUND the playhead: half way down 4.0 ms after the step. Its
            // half that only looks back would be half way down 8.0 ms after it.)
            expect(at16 > 1.0 && at16 < 1.006, "the step heard at " + juce::String(at16, 4) + " s");
            expect(worst <= 16.0 / SR + 1.0e-9, "no later or earlier than a sub-block: " + text);

            // POS and GRN: the balance over the note at blocks of 7 and 333 against 16.
            const ParamMap gran { { S::SampleSelect, brk }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.5f }, { S::Pan, -1.0f } };
            for (int published : { (int) S::PubPos, (int) S::PubGrain })
            {
                const int source = S::sources().placeOfPublished(published);
                auto trace = [&](int block)
                {
                    SamplerEngine e;
                    apply(e, with(gran, slot(0, source, S::LfoPan, 1.0f)));
                    const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.0, block);
                    std::vector<double> t;
                    for (size_t at = 2400; at + 480 <= o.l.size(); at += 480)
                        t.push_back(balance(o, at, at + 480));
                    return t;
                };
                const std::vector<double> a = trace(16);
                double gap = 0.0;
                for (int block : { 7, 333 })
                {
                    const std::vector<double> t = trace(block);
                    for (size_t k = 0; k < std::min(a.size(), t.size()); ++k)
                        gap = std::max(gap, std::abs(t[k] - a[k]));
                }
                logMessage("[u4-smp] " + juce::String(published == S::PubPos ? "POS" : "GRN") + " > PAN, blocks of 7 and 333 re. 16: largest balance gap " + juce::String(gap, 4));
                expect(gap < 0.02, "balance gap " + juce::String(gap, 4));
            }
            // GRN in the GRAN stretch: the grains walk through the break with the time position.
            {
                SamplerEngine e;
                apply(e, with({ { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::StretchMode, (float) S::StretchGrain },
                                { S::StretchSpeed, 1.0f }, { S::Pan, -1.0f } }, slot(0, S::sources().placeOfPublished(S::PubGrain), S::LfoPan, 1.0f)));
                const Out o = render(e, { { 0.0, On, 60, 1.0f } }, 1.0);
                const double early = balance(o, 2400, 7200), late = balance(o, 40800, 45600);
                expect(late - early > 0.05, "GRN in the GRAN stretch: balance " + juce::String(early, 3) + " then " + juce::String(late, 3));
            }
        }

        beginTest("The tempo reaches the router: a synced LFO at another tempo, old against new");
        {
            const ParamMap sound = with(with(loop, lfo(0, S::LfoAmp, 0.6f)), { { R::lfoId(0, R::Sync), 1.0f } });
            const Verdict v = pair("LFO1 synced > AMP at 77 bpm", sound, gesture(), GESTURE_SECONDS, 128,
                                   [](OldSamplerEngine& o, SamplerEngine& n) { o.setTempo(77.0f); n.setTempo(77.0f); });
            expect(v.word != "LOST", v.text());
        }

        beginTest("What the seven unpinned hashes of \"Sampler Stretch\" rendered, old against new");
        {
            // Its "variant 3": a stereo file at 44.1 kHz in a 48 kHz engine, STRT / END / loop points,
            // slices, the filter, BITS and RATE, an LFO on the pitch and one on the pan, in each play
            // mode - without the three things drawn at random (RAND, SPRY, SPRD), which the six-seed
            // rule and the grains' draws compare.
            std::vector<float> file(2 * 30000);
            uint32_t state = 0x13579bdu;
            for (int i = 0; i < 30000; ++i)
            {
                const float fall = std::exp(-(float) (i % 7500) / 3000.0f);
                file[(size_t) i] = 0.4f * fall * (float) std::sin(2.0 * 3.141592653589793 * 180.0 * i / 44100.0) + 0.1f * fall * dsp::xorshiftNoise(state);
                file[(size_t) (30000 + i)] = 0.4f * fall * (float) std::sin(2.0 * 3.141592653589793 * 271.0 * i / 44100.0) + 0.1f * fall * dsp::xorshiftNoise(state);
            }
            for (int mode = 0; mode < S::NumPlayModes; ++mode)
            {
                const ParamMap sound = with({ { S::Mode, (float) mode }, { S::StartPoint, 0.1f }, { S::EndPoint, 0.9f }, { S::LoopStart, 0.2f },
                                              { S::LoopEnd, 0.7f }, { S::Release, 0.05f }, { S::SliceCount, 4.0f }, { S::SliceSelect, 0.5f },
                                              { S::FilterCutoff, 0.6f }, { S::FilterEnv, 0.2f }, { S::PitchSemi, 3.0f }, { S::FineTune, -0.3f },
                                              { S::DriveAmt, 0.3f }, { S::Attack, 0.01f }, { S::BitReduce, 0.3f }, { S::RateReduce, 0.1f } },
                                            with(lfo(0, S::LfoPitch, 0.4f, 1.0f, 0), lfo(1, S::LfoPan, -0.5f, 1.0f, 0)));
                const Verdict v = pair("stereo 44.1 kHz file, two LFOs, mode " + juce::String(mode), sound, gesture(), GESTURE_SECONDS, 64,
                                       [&](OldSamplerEngine& o, SamplerEngine& n)
                                       {
                                           o.loadSample(file.data(), 30000, 2, 44100.0);
                                           n.loadSample(file.data(), 30000, 2, 44100.0);
                                       });
                expect(v.word != "LOST", "mode " + juce::String(mode) + ": " + v.text());
            }
        }
    }

    // ---- probes (NEPTUNE_U4_REVIEW_PROBE): numbers for the report, nothing expected ---------------------
    void probeNoteStartInput()
    {
        beginTest("probe: which sample of the track input each of a note's first samples hears");
        std::vector<float> input(4800);
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = 0.001f * (float) (i + 1);
        auto play = [&](bool moved)
        {
            SamplerEngine e;
            load(e, std::vector<float>(48000, 0.5f));
            apply(e, { { S::Mode, (float) S::Loop } });
            if (moved)
                apply(e, slot(0, mod::Src::Xin, S::LfoAmp, 1.0f));
            return render(e, { { 0.0, On, 60, 1.0f } }, 0.05, 128, &input);
        };
        const Out plain = play(false), moved = play(true);
        juce::String text;
        for (int i = 0; i < 40; ++i)
            text += juce::String((moved.l[(size_t) i] / plain.l[(size_t) i] - 1.0f) * 1000.0f - 1.0f, 1) + " ";
        logMessage("[u4-smp-probe] input sample heard by output samples 0..39: " + text);

        // The grain counts over eight seeds: is the new cloud's clock systematically slower?
        {
            const ParamMap stretched { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Loop }, { S::StretchSpeed, 0.6f },
                                       { S::StretchMode, (float) S::StretchGrain }, { S::StretchJitter, 0.6f }, { S::StretchSpread, 0.8f } };
            const ParamMap cloud { { S::SampleSelect, (float) breakSample() }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.3f },
                                   { S::GrainSize, 0.12f }, { S::GrainDensity, 80.0f }, { S::GrainSpray, 0.5f }, { S::GrainPitchJit, 0.3f },
                                   { S::GrainSpread, 0.8f }, { S::GrainReverse, 0.4f } };
            ParamMap abyss;
            for (auto& preset : FactoryPresets::getAll())
                if (preset.name == "Abyss Drone") abyss = preset.soundParams;
            struct C { const char* name; ParamMap sound; double seconds; };
            for (const C& c : { C { "stretch SMER", with(stretched, { { S::StretchMode, (float) S::StretchSmear } }), 8.0 },
                                C { "GRAN all four", cloud, 8.0 }, C { "Abyss Drone", abyss, 16.0 } })
            {
                juce::String a, b;
                double so = 0.0, sn = 0.0, po = 0.0, pn = 0.0, ko = 0.0, kn = 0.0;
                for (uint32_t seed = 1; seed <= 8; ++seed)
                {
                    const GrainDraws o = grainDraws<OldSamplerEngine>(c.sound, seed * 7919u, c.seconds),
                                     n = grainDraws<SamplerEngine>(c.sound, seed * 7919u, c.seconds);
                    a += juce::String(o.total) + " ";
                    b += juce::String(n.total) + " ";
                    so += o.total; sn += n.total;
                    po += (double) o.reversed / std::max(1, o.total); pn += (double) n.reversed / std::max(1, n.total);
                    ko += ksDistance(o.pitch, n.pitch);
                    (void) kn;
                }
                logMessage("[u4-smp-probe] grains, " + juce::String(c.name) + ": old " + a + "(sum " + juce::String(so, 0) + "); new " + b + "(sum "
                           + juce::String(sn, 0) + "); backwards old " + juce::String(po / 8.0, 3) + " new " + juce::String(pn / 8.0, 3)
                           + "; mean KS pitch old-new " + juce::String(ko / 8.0, 3));
            }
        }

        // Abyss Drone, the number "Strip Routes" pins: the level with the strip up re. at rest, sixteen seeds.
        for (auto& preset : FactoryPresets::getAll())
            if (preset.name == "Abyss Drone")
            {
                auto change = [&](auto make, uint32_t seed)
                {
                    double level[2] = {};
                    for (int up = 0; up < 2; ++up)
                    {
                        auto e = make();
                        apply(*e, preset.soundParams);
                        e->setGrainSeed(seed);
                        const Out o = render(*e, { { 0.0, Strip, 0, (float) up }, { 0.0, On, 48, 0.8f }, { 0.0, On, 60, 0.8f }, { 2.25, Off, 48, 0.0f },
                                                   { 2.25, Off, 60, 0.0f } }, 3.0);
                        level[up] = rms(mono(o), 0, o.l.size());
                    }
                    return db(level[1] / level[0]);
                };
                juce::String a, b;
                double mo = 0.0, mn = 0.0, vo = 0.0, vn = 0.0;
                std::vector<double> xo, xn;
                for (uint32_t seed = 0; seed < 16; ++seed)
                {
                    xo.push_back(change([] { return std::make_unique<OldSamplerEngine>(); }, seed + 1));
                    xn.push_back(change([] { return std::make_unique<SamplerEngine>(); }, seed));
                    a += juce::String(xo.back(), 1) + " ";
                    b += juce::String(xn.back(), 1) + " ";
                    mo += xo.back() / 16.0;
                    mn += xn.back() / 16.0;
                }
                for (int k = 0; k < 16; ++k) { vo += std::pow(xo[(size_t) k] - mo, 2.0) / 15.0; vn += std::pow(xn[(size_t) k] - mn, 2.0) / 15.0; }
                logMessage("[u4-smp-probe] Abyss Drone, strip up re. at rest, dB: old " + a + "(mean " + juce::String(mo, 2) + ", sd " + juce::String(std::sqrt(vo), 2)
                           + "); new " + b + "(mean " + juce::String(mn, 2) + ", sd " + juce::String(std::sqrt(vn), 2) + ")");
            }
        expect(true);
    }

    // ---- NEPTUNE_U4_SAMPLER_HOLD: how long a sample change keeps the audio thread out (Release binary) ----
    struct HoldOut { double load = 0.0, silent = 0.0; };      // seconds
    template <typename E>
    HoldOut holdOut(const char* name, double sampleSeconds)
    {
        const int frames = (int) (sampleSeconds * SR);
        std::vector<float> big((size_t) frames * 2);
        uint32_t state = 0x2468aceu;
        for (float& x : big) x = 0.5f * dsp::xorshiftNoise(state);
        E e;
        e.prepareToPlay(SR, 64);
        e.loadSample(big.data(), 4800, 1, SR);
        e.setParameter(S::Mode, (float) S::Forward);
        std::atomic<bool> stop { false };
        std::atomic<double> longest { 0.0 };
        std::atomic<int> blocks { 0 };
        std::thread audio([&]
        {
            juce::AudioBuffer<float> buffer(2, 64);
            juce::MidiBuffer midi;
            double silentSince = -1.0;
            const auto t0 = std::chrono::steady_clock::now();
            while (! stop.load())
            {
                e.noteOn(60, 1.0f, 0);
                buffer.clear();
                e.processBlock(buffer, midi);
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const bool sound = buffer.getMagnitude(0, 64) > 0.0f;
                if (! sound && silentSince < 0.0) silentSince = now;
                if (sound && silentSince >= 0.0)
                {
                    longest.store(std::max(longest.load(), now - silentSince));
                    silentSince = -1.0;
                }
                ++blocks;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        longest.store(0.0);
        const auto t0 = std::chrono::steady_clock::now();
        e.loadSample(big.data(), frames, 2, SR);
        const double load = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true);
        audio.join();
        logMessage("[u4-smp-hold] " + juce::String(name) + ", " + juce::String(sampleSeconds, 0) + " s of stereo sample: loadSample "
                   + juce::String(load * 1000.0, 1) + " ms, the track silent for " + juce::String(longest.load() * 1000.0, 1) + " ms ("
                   + juce::String(blocks.load()) + " blocks run)");
        return { load, longest.load() };
    }
    void holdOutBench()
    {
        beginTest("A sample change: how long the audio thread is kept out of the track");
        for (int pass = 0; pass < 3; ++pass)
            for (double seconds : { 2.0, 60.0, 300.0 })
            {
                holdOut<OldSamplerEngine>("old", seconds);
                holdOut<SamplerEngine>("new", seconds);
            }
        expect(true);
    }

    // ---- cost ----------------------------------------------------------------------------------------
    template <typename E>
    static double cost(const ParamMap& sound, bool stripUp, int voices = 8)
    {
        double best = 1.0e9;
        for (int run = 0; run < 5; ++run)
        {
            E e;
            apply(e, sound);
            e.prepareToPlay(SR, 128);
            for (int v = 0; v < voices; ++v)
                e.noteOn(48 + v * 3, 0.8f, 0);
            if (stripUp)
                e.modWheel(1.0f);
            juce::AudioBuffer<float> buffer(2, 128);
            juce::MidiBuffer midi;
            for (int i = 0; i < 40; ++i) { buffer.clear(); e.processBlock(buffer, midi); }
            const int blocks = 3000;        // 8 s of sound
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < blocks; ++i)
            {
                buffer.clear();
                e.processBlock(buffer, midi);
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            best = std::min(best, 100.0 * seconds / ((double) blocks * 128.0 / SR));
        }
        return best;
    }
    void bench()
    {
        beginTest("Cost: 8 voices, old against new (% of one core, best of five)");
        const float brk = (float) breakSample();
        struct Case { const char* name; ParamMap sound; bool strip; bool newOnly; };
        const ParamMap loop { { S::Mode, (float) S::Loop } };
        const ParamMap filtered { { S::Mode, (float) S::Loop }, { S::FilterCutoff, 0.5f }, { S::FilterEnv, 0.3f } };
        const ParamMap grains { { S::SampleSelect, brk }, { S::Mode, (float) S::Granular }, { S::GrainScan, 0.3f } };
        const ParamMap cloud = with(grains, { { S::GrainSpray, 0.5f }, { S::GrainPitchJit, 0.3f }, { S::GrainSpread, 0.8f }, { S::GrainReverse, 0.4f } });
        const ParamMap wsol { { S::SampleSelect, brk }, { S::Mode, (float) S::Loop }, { S::StretchMode, (float) S::StretchWsol }, { S::StretchSpeed, 0.7f } };
        const int pos = S::sources().placeOfPublished(S::PubPos), slev = S::sources().placeOfPublished(S::PubLevel);
        const std::vector<Case> cases {
            { "idle: LOOP", loop, false, false },
            { "idle: LOOP + filter", filtered, false, false },
            { "idle: GRAN", grains, false, false },
            { "idle: GRAN randoms", cloud, false, false },
            { "idle: WSOL stretch", wsol, false, false },
            { "LFO1 > PTCH", with(loop, lfo(0, S::LfoPitch, 0.3f)), false, false },
            { "LFO1 > AMP", with(loop, lfo(0, S::LfoAmp, 0.5f)), false, false },
            { "LFO1 > PAN", with(loop, lfo(0, S::LfoPan, 0.5f)), false, false },
            { "LFO1 > CUT", with(filtered, lfo(0, S::LfoCutoff, 0.5f)), false, false },
            { "LFO1 > RESO", with(filtered, lfo(0, S::LfoResonance, 0.5f)), false, false },
            { "LFO1 > LEVL", with(loop, lfo(0, S::LfoLevel, 0.5f)), false, false },
            { "LFO1 > GPOS, LFO2 > SPRY", with(with(cloud, lfo(0, S::LfoGrainPos, 0.5f)), lfo(1, S::LfoSpray, 0.5f)), false, false },
            { "strip up, defaults", loop, true, false },
            { "2 LFOs + 4 slots", with(with(with(with(with(with(filtered, lfo(0, S::LfoCutoff, 0.5f)), lfo(1, S::LfoPan, 0.5f)),
                                  slot(0, mod::Src::Velocity, S::LfoCutoff, 0.4f)), slot(1, mod::Src::Env, S::LfoPitch, 0.2f)),
                                  slot(2, mod::Src::Key, S::LfoPan, 0.5f)), slot(3, mod::Src::Lfo1, S::LfoDrive, 0.5f)), false, false },
            { "POS > PAN (new)", with(loop, slot(0, pos, S::LfoPan, 1.0f)), false, true },
            { "SLEV > CUT (new)", with(filtered, slot(0, slev, S::LfoCutoff, 0.5f)), false, true },
            { "ONE > ETIM (new)", with(loop, slot(0, mod::Src::One, S::LfoEnvTime, 0.3f)), false, true },
        };
        for (auto& c : cases)
        {
            // (alternately, twice each way: the machine is shared)
            double o = 0.0, n = 1.0e9;
            if (! c.newOnly)
            {
                o = 1.0e9;
                for (int pass = 0; pass < 2; ++pass)
                {
                    o = std::min(o, cost<OldSamplerEngine>(c.sound, c.strip));
                    n = std::min(n, cost<SamplerEngine>(c.sound, c.strip));
                }
            }
            else
                n = cost<SamplerEngine>(c.sound, c.strip);
            logMessage("[u4-smp-bench] " + juce::String(c.name).paddedRight(' ', 28) + (c.newOnly ? juce::String("     -") : juce::String(o, 3).paddedLeft(' ', 6))
                       + " -> " + juce::String(n, 3).paddedLeft(' ', 6) + " %"
                       + (c.newOnly ? juce::String() : "   (" + juce::String(100.0 * (n / o - 1.0), 1) + " %)"));
        }
        expect(true);
    }
};

static U4SamplerTests u4SamplerTests;
