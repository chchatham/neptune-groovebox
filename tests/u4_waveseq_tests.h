#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <vector>
#include "fast_measure.h"
#include "mod_router_tests.h"
#include "old/OldWaveSeqEngine.h"
#include "../src/engine/WaveSeqEngine.h"
#include "../src/engine/ModRouter.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/PresetManager.h"

// -----------------------------------------------------------------------------
// "U4 Wave Sequencer": the wave sequencer on the modulation router (src/engine/ModRouter.h) and
// on the shared oscillator's phase (src/dsp/PhaseOsc.h), against the engine as it was
// (tests/old/OldWaveSeqEngine.h: the header of commit c21530a under another name, on
// EngineLfoSection / EngineModSection).
//
//   the table       86 destinations; the 83 that were there are where they were; ETIM FTIM PM
//   the sources     PLAN SLAN LLAN TLAN at the end of the SRC / VIA list; what each one gives
//   pages and ids   the router's pages and ids; every cell set and read back
//   old against new every factory preset and the plain paths bit for bit; each folded feature
//                   (LFO destinations, strip, pressure, slots) measured: MATCH / CLOSE / LOST
//   PRNG            a stored value beyond the cell's range stays unless a slot is aimed at it
//   the lanes       as sources: on the pan, on the cutoff, a lane of its own length
//   the randoms     a random loop from the one generator: repeats, differs by voice and by note
//   ETIM / FTIM     the envelopes' times as destinations
//   PM              the track input on the oscillator's phase, at audio rate
//   real time       no allocation; finite at the ends
//
// NEPTUNE_U4_PRINT=1 prints the old-against-new table; NEPTUNE_U4_ATLAS=<folder> writes the pairs
// that are not bit-identical as WAV; NEPTUNE_U4_WSEQ_BENCH=1 (Release binary) times old and new.
//
// The verdicts. MATCH: the same samples. CLOSE: the loudness of the whole render within 1 dB,
// the loudness over time (100 ms frames, where either is above -50 dB of the peak frame) within
// 2 dB, the brightness (energy-weighted mean frequency) within 10 %. LOST: anything else.
// -----------------------------------------------------------------------------
class U4WaveSeqTests : public juce::UnitTest
{
public:
    U4WaveSeqTests() : UnitTest("U4 Wave Sequencer") {}

    using W = WaveSeqEngine;
    using V = WaveSeqVoice;
    using O = OldWaveSeqEngine;
    using R = mod::Router;
    static constexpr double SR = 48000.0;
    static constexpr int FB = W::FilterBase;

    struct Stereo { std::vector<float> l, r; };
    enum Kind { On, Off, Pressure, Strip, Bend };
    struct Event { double at; Kind kind; int note; float value; };
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
        }
    }

    // Events are applied at the first block that starts at or after their time. each(block start).
    static Stereo render(EngineBase& e, const Events& events, double seconds, int block = 256, double sr = SR,
                         const Input* input = nullptr, const std::function<void(long)>& each = nullptr)
    {
        e.prepareToPlay(sr, block);
        const long total = (long) (seconds * sr);
        Stereo out;
        out.l.reserve((size_t) total);
        out.r.reserve((size_t) total);
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
            if (each)
                each(pos);
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
                 { 2.7, On, 55, 0.8f }, { 3.3, Off, 55, 0.0f } };
    }
    static Events oneNote(double length = 1.5, int note = 57) { return { { 0.0, On, note, 0.8f }, { length, Off, note, 0.0f } }; }

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
        double level = 0.0;         // dB, new against old, the whole render
        double curve = 0.0;         // dB, the largest difference of a 100 ms frame
        double bright = 0.0;        // new against old - 1
        double width = 0.0;         // the difference of (L - R) / (L + R) energy shares
        double spectrum = 0.0;      // fastmeasure::difference: 0 the same sound, 1 nothing in common
        double residual = 0.0;      // dB: the difference signal (new - old, both sides) against the old one
                                    // (the review's: not part of the verdict; -200 = the same samples)
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
        auto side = [](const Stereo& s)
        {
            double d = 0.0, m = 0.0;
            for (size_t i = 0; i < s.l.size(); ++i)
            {
                d += (double) (s.l[i] - s.r[i]) * (double) (s.l[i] - s.r[i]);
                m += (double) (s.l[i] + s.r[i]) * (double) (s.l[i] + s.r[i]);
            }
            return m > 0.0 ? d / m : 0.0;
        };
        c.width = side(newer) - side(older);
        c.spectrum = fastmeasure::difference(a, b, 2048);
        return c;
    }
    static const char* verdict(const Compared& c)
    {
        if (c.identical) return "MATCH";
        if (std::abs(c.level) <= 1.0 && c.curve <= 2.0 && std::abs(c.bright) <= 0.10) return "CLOSE";
        return "LOST";
    }

    static bool printing() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_PRINT", "").isNotEmpty(); }
    static juce::String atlas() { return juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_ATLAS", ""); }

    static void writeWav(const juce::File& file, const Stereo& s)
    {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (! out.openedOk()) return;
        const int frames = (int) s.l.size(), bytes = frames * 4;
        out.write("RIFF", 4); out.writeInt(36 + bytes); out.write("WAVEfmt ", 8);
        out.writeInt(16); out.writeShort(1); out.writeShort(2); out.writeInt((int) SR); out.writeInt((int) SR * 4);
        out.writeShort(4); out.writeShort(16); out.write("data", 4); out.writeInt(bytes);
        for (int i = 0; i < frames; ++i)
            for (float v : { s.l[(size_t) i], s.r[(size_t) i] })
                out.writeShort((short) std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    // The same setup and the same gesture on the old engine and on the new one.
    Compared against(const juce::String& name, const Setup& setUp, const Events& events, double seconds,
                     int block = 256, const std::function<void(O&)>& oldOnly = nullptr, const std::function<void(W&)>& newOnly = nullptr,
                     double sr = SR)
    {
        auto older = std::make_unique<O>();
        auto newer = std::make_unique<W>();
        if (setUp) { setUp(*older); setUp(*newer); }
        if (oldOnly) oldOnly(*older);
        if (newOnly) newOnly(*newer);
        const Stereo a = render(*older, events, seconds, block, sr), b = render(*newer, events, seconds, block, sr);
        const Compared c = compare(a, b, sr);
        if (printing())
            logMessage(juce::String::formatted("U4-WSEQ | %-34s | %-5s | level %+6.2f dB | over time %5.2f dB | bright %+6.1f %% | width %+6.3f | spectrum %5.3f | residual %6.1f dB",
                                               name.toRawUTF8(), verdict(c), c.level, c.curve, 100.0 * c.bright, c.width, c.spectrum, c.residual));
        if (atlas().isNotEmpty() && ! c.identical)
        {
            const juce::File folder(atlas());
            folder.createDirectory();
            const juce::String file = name.replaceCharacters(" >/*,", "_____").replace("__", "_");
            writeWav(folder.getChildFile(file + "_old.wav"), a);
            writeWav(folder.getChildFile(file + "_new.wav"), b);
        }
        expect(finite(b), name + ": finite and bounded");
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
    static int place(const char* source) { return W::sources().indexOfLabel(source); }
    static int dest(const char* label) { return W::lfoDests().indexOfLabel(label); }

    // A sequence of `steps` steps of `seconds` each, one shape, no crossfade, not tempo-synced.
    static void sequence(EngineBase& e, int steps, float seconds, int shape = 0)
    {
        e.setParameter(W::SeqLength, (float) steps);
        for (int id : { (int) W::TimingLaneLen, (int) W::PitchLaneLen, (int) W::ShapeLaneLen, (int) W::LevelLaneLen })
            e.setParameter(id, (float) steps);
        e.setParameter(W::TempoSync, 0.0f);
        e.setParameter(W::Crossfade, 0.0f);
        e.setParameter(W::AmpAttack, 0.001f);
        e.setParameter(W::AmpSustain, 1.0f);
        for (int s = 0; s < 16; ++s)
        {
            e.setParameter(W::StepPitch + s, 0.0f);
            e.setParameter(W::StepShape + s, (float) shape);
            e.setParameter(W::StepLevel + s, 1.0f);
            e.setParameter(W::StepTime + s, seconds);
        }
    }
    static void openFilter(EngineBase& e, float hz = 900.0f)
    {
        e.setParameter(FB + VoiceFilterSection::Cutoff, hz);
        e.setParameter(FB + VoiceFilterSection::Resonance, 0.3f);
    }
    static V* sounding(W& e)
    {
        for (int v = 0; v < 8; ++v)
            if (e.getVoice(v).isActive())
                return &e.getVoice(v);
        return nullptr;
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_WSEQ_BENCH", "").isNotEmpty())
        {
            bench();
            return;
        }
        testTable();
        testSources();
        testPagesAndIds();
        testFactoryPresets();
        testPlainPaths();
        testFoldedFeatures();
        testPitchRangeBeyondItsCell();
        testLanesAsSources();
        testRandomLoop();
        testRate();
        testEnvelopeTimes();
        testPhaseModulation();
        testPressurePerVoice();
        testControlCellsGoBack();
        testBodyRingsOut();
        testRealTime();
        testReviewSampleRates();
        testReviewChainEnvelopeAtTheNote();
        testReviewLostRows();
        testReviewRandomWalk();
        testReviewDepths();
        testReviewLanes();
        testReviewMonoAndLetGo();
        testReviewLaneTimingByBlock();
        testReviewInputsGivenBack();
        testReviewRandomLoopApartFromRand();
    }

    // ---- the random loop and the source RAND are not the same draws (the takeover review) --------
    // Both come from the one per-note generator; the loop's steps on a stream of their own
    // (spread::Lanes). On the stream of RAND (Free) the loop's first draw WOULD BE RAND: the first
    // random step of a note would be RAND's value read as a step. Over 64 notes, the step each lane
    // stands at after its first random move against floor((RAND + 1) / 2 x 16): chance is 1 in 16.
    void testReviewRandomLoopApartFromRand()
    {
        beginTest("Review: the random loop's steps are not the source RAND's value");
        auto e = std::make_unique<W>();
        randomPatch(*e);                                     // 16 steps of 15 ms, LOOP RAND
        slot(*e, 0, mod::Src::ofRandom(0), V::LfoPan, 0.01f);  // (RAND is read)
        std::array<int, 4> matches {};
        const int notes = 64;
        for (int k = 0; k < notes; ++k)
        {
            float rand = 0.0f;
            std::array<int, 4> step {};
            render(*e, { { 0.0, On, 57, 0.8f } }, 0.025, 16, SR, nullptr, [&](long)
            {
                if (auto* v = sounding(*e))
                {
                    rand = v->getMod().source(mod::Src::ofRandom(0));
                    for (int l = 0; l < 4; ++l)
                        step[(size_t) l] = v->getLaneConfig(l).currentStep;
                }
            });
            const int fromRand = std::min(15, (int) ((rand + 1.0f) * 0.5f * 16.0f));
            for (int l = 0; l < 4; ++l)
                matches[(size_t) l] += step[(size_t) l] == fromRand;
            e->allNotesOff();
        }
        for (int l = 0; l < 4; ++l)
            expect(matches[(size_t) l] < notes / 4, "lane " + juce::String(l) + ": its first random step is RAND's value in "
                                                     + juce::String(matches[(size_t) l]) + " of " + juce::String(notes) + " notes");
    }

    // ---- RATE, XFAD and PRNG are moved for one sub-block at a time (the takeover review) ---------
    // The voice sets them from the router for the samples of a sub-block and gives the set values
    // back after it: between sub-blocks, and once every route to them is gone, the voice holds
    // what the cells say (the same as on a voice nothing was ever routed to).
    void testReviewInputsGivenBack()
    {
        beginTest("Review: RATE, XFAD and PRNG moved by slots are given back after every sub-block and when the routes go");
        auto routed = std::make_unique<W>(), plain = std::make_unique<W>();
        for (auto* e : { routed.get(), plain.get() })
        {
            sequence(*e, 4, 0.05f);
            e->setParameter(W::Crossfade, 0.3f);
            e->setParameter(W::PitchRange, 0.5f);
            e->setParameter(W::Rate, 1.5f);
        }
        slot(*routed, 0, place("ONE"), V::LfoCrossfade, 0.5f);
        slot(*routed, 1, place("ONE"), V::LfoRate, 0.4f);
        slot(*routed, 2, place("ONE"), V::LfoPitchRange, 0.6f);
        const Events note = { { 0.0, On, 57, 0.8f } };
        bool alike = true;
        int blocks = 0;
        {
            // (the routes are heard)
            auto heard = std::make_unique<W>(), unheard = std::make_unique<W>();
            for (auto* e : { heard.get(), unheard.get() })
            {
                sequence(*e, 4, 0.05f);
                e->setParameter(W::StepPitch + 1, 7.0f);
            }
            slot(*heard, 0, place("ONE"), V::LfoRate, 0.4f);
            expect(! same(render(*heard, note, 0.3), render(*unheard, note, 0.3)), "(ONE > RATE is heard)");
        }
        auto check = [&](long)
        {
            const V& a = routed->getVoice(0);
            const V& b = plain->getVoice(0);
            alike = alike && a.getCrossfadeAmount() == b.getCrossfadeAmount() && a.getSpeedMultiplier() == b.getSpeedMultiplier()
                    && a.getPitchRange() == b.getPitchRange();
            ++blocks;
        };
        render(*plain, note, 0.3, 100);
        render(*routed, note, 0.3, 100, SR, nullptr, check);
        expect(blocks > 100 && alike, "between sub-blocks the voice holds the cells' values (" + juce::String(blocks) + " blocks)");
        for (int k = 0; k < 3; ++k)
            routed->setParameter(ModSlotParam::id(k, ModSlotParam::SRC), 0.0f);
        render(*routed, {}, 0.1, 100);
        const V& a = routed->getVoice(0);
        const V& b = plain->getVoice(0);
        expectEquals(a.getCrossfadeAmount(), b.getCrossfadeAmount(), "the routes gone: XFAD as set");
        expectEquals(a.getSpeedMultiplier(), b.getSpeedMultiplier(), "the routes gone: RATE as set");
        expectEquals(a.getPitchRange(), b.getPitchRange(), "the routes gone: PRNG as set");
    }

    // ---- a lane source and the host's block size (the takeover review) ---------------------------
    // The FM review found an AUDIO-rate source handed over "the last sub-block", which moved with
    // the host's block size. A lane is a CONTROL-rate source: published at the start of each
    // sub-block from where the lane stands then. The sub-blocks start again at each host block, so
    // where a step change is picked up moves with the host's block size, but never by more than
    // one sub-block (16 samples at 48 kHz, 32 at 96): the lane is never handed over a sub-block
    // stale. PLAN > PAN 100, the pitch lane 0 then 24 (heard only through the pan: PRNG 0), a
    // square: the sample where the left side first falls below half the right, against the step
    // change (block 1: every sample is a sub-block). Measured: 7 samples later at blocks of 512, 256,
    // 100 and 16, 12 at 37 (48 kHz); 15 .. 16, and 31 at 37 (96 kHz).
    void testReviewLaneTimingByBlock()
    {
        beginTest("Review: a lane source picks up a step change within one sub-block, whatever the host's block size");
        for (double sr : { 48000.0, 96000.0 })
        {
            auto crossing = [sr](int block)
            {
                auto e = std::make_unique<W>();
                sequence(*e, 2, 0.0505f, 2);
                e->setParameter(W::StepPitch + 1, 24.0f);
                e->setParameter(W::PitchRange, 0.0f);
                slot(*e, 0, place("PLAN"), V::LfoPan, 1.0f);
                const Stereo out = render(*e, { { 0.0, On, 57, 0.8f } }, 0.08, block, sr);
                for (size_t i = (size_t) (0.02 * sr); i < out.l.size(); ++i)
                    if (std::abs(out.r[i]) > 0.05f && std::abs(out.l[i]) < 0.5f * std::abs(out.r[i]))
                        return (long) i;
                return -1L;
            };
            const long chunk = sr > 50000.0 ? 32 : 16;
            const long reference = crossing(1);
            expect(reference > 0, "block 1: the pan moves at the step change");
            juce::String seen;
            for (int block : { 512, 256, 100, 37, 16, 5 })
            {
                const long at = crossing(block);
                seen << block << ": " << (at - reference) << "  ";
                expect(at >= reference - 2 && at - reference <= chunk + 2,     // (+-2: where the pan's ramp crosses one half)
                       juce::String::formatted("%.0f kHz, block %d: the step change heard %ld samples after block 1's (at most one sub-block, %ld)",
                                               sr / 1000.0, block, at - reference, chunk));
            }
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: PLAN > PAN, %.0f kHz: the step change heard later than at block 1 by (block: samples) ", sr / 1000.0) + seen);
        }
    }

    // ---- the table -----------------------------------------------------------------------------
    void testTable()
    {
        beginTest("The table: 94 destinations (91 + ETIM FTIM PM), the 91 that were there where they were");
        const auto& t = W::lfoDests();
        const auto& old = O::lfoDests();
        expectEquals(old.size(), 91);    // +8 BDY2 cells (VoiceBody::NumCells grew from 8 to 16)
        expectEquals(t.size(), 94);
        expectEquals(t.visible(), 94, "nothing hidden");
        expect(t.size() <= mod::DestTable::MAX);
        for (int i = 0; i < old.size(); ++i)
            expectEquals(juce::String(t[i].label), juce::String(old[i].label), "destination " + juce::String(i));
        // (The old list's fixed entries have no cell; of the new table's, XFAD is the cell.)
        for (int i = V::NumLfoDests; i < old.size(); ++i)
            if (old[i].key >= 0)
                expectEquals(t[i].id, old[i].key, juce::String(t[i].label) + " is the same cell");
        expectEquals(juce::String(t[91].label), juce::String("ETIM"));   // +8: BDY2's eight cells before ETIM
        expectEquals(juce::String(t[92].label), juce::String("FTIM"));
        expectEquals(juce::String(t[93].label), juce::String("PM"));
        // The three the voice reads by number are read from the table (they were typed constants,
        // and were dead for a day when BODY grew: a jassert does not fail this binary). Found, and
        // the entries they say; that each ACTS is measured below (testEnvelopeTimes, testPhaseModulation).
        const auto& found = W::dests();
        expect(found.envTime > 0 && found.filterTime > 0 && found.phase > 0, "ETIM, FTIM and PM are found in the table");
        expectEquals(found.envTime, t.ampTime());
        expectEquals(found.filterTime, t.filterTime());
        expectEquals(juce::String(t[std::max(0, found.envTime)].label), juce::String("ETIM"));
        expectEquals(juce::String(t[std::max(0, found.filterTime)].label), juce::String("FTIM"));
        expectEquals(juce::String(t[std::max(0, found.phase)].label), juce::String("PM"));
        expectEquals(t[V::LfoCrossfade].id, (int) W::Crossfade);
        expectEquals(t[V::LfoRate].id, -1, "RATE is a factor, not the cell: it is not held to the cell's range");
        expectEquals(t[V::LfoPitchRange].id, -1, "PRNG is an offset, not the cell: a stored value beyond the range stays");
        for (int i = 1; i < t.size(); ++i)
        {
            const bool perSample = i == V::LfoPitch || i == V::LfoAmp || i == V::LfoPan || i == V::LfoCutoff || i == WaveSeqEngine::dests().phase;
            expect(t[i].perSample == perSample, juce::String(t[i].label) + (perSample ? " is taken per sample" : " is taken at control rate"));
        }
        expectEquals(t.stripDefault(0).dest, (int) V::LfoRate);
        expectEquals(t.stripDefault(0).amount, 0.5f);
        expectEquals(t.stripDefault(1).dest, 0);
        expectEquals(t.stripVibratoDefaultValue(), 0.0f, "no vibrato by default (the engine had none)");
        // Every cell of the pages that is in the table is a continuous one, and every lane step
        // but the shape lane's is there.
        int steps = 0;
        for (int i = 1; i < t.size(); ++i)
            if (t[i].id >= W::StepPitch && t[i].id < W::StepTime + 16)
            {
                ++steps;
                expect(t[i].id < W::StepShape || t[i].id >= W::StepLevel, "no shape step is a destination");
            }
        expectEquals(steps, 48);
    }

    // ---- the sources ---------------------------------------------------------------------------
    void testSources()
    {
        beginTest("The sources: the four lanes at the end of the SRC / VIA list");
        const auto& s = W::sources();
        const auto& plain = mod::SourceTable::standard(2);
        expectEquals(s.published(), 4);
        expectEquals(s.size(), plain.size() + 4);
        for (int i = 0; i < plain.size(); ++i)
            expectEquals(juce::String(s.labels()[i]), juce::String(plain.labels()[i]), "the list before them is every engine's");
        static const char* const names[4] = { "PLAN", "SLAN", "LLAN", "TLAN" };
        for (int k = 0; k < 4; ++k)
        {
            expectEquals(juce::String(s.labels()[plain.size() + k]), juce::String(names[k]));
            expectEquals(s.placeOfPublished(k), plain.size() + k);
            expect(! s.audio(k), "control rate");
            expect(std::strlen(names[k]) == 4);
        }
        expect(s.bipolar(V::PubPitch), "the pitch lane swings both ways");
        expect(! s.bipolar(V::PubShape) && ! s.bipolar(V::PubLevel) && ! s.bipolar(V::PubTime), "the others go one way");
        // The old slots' list (OFF LFO1 LFO2 ENV FENV VEL PRES STRP KEY RAND IN) keeps its places.
        static const char* const was[11] = { "OFF", "LFO1", "LFO2", "ENV", "FENV", "VEL", "PRES", "STRP", "KEY", "RAND", "IN" };
        for (int i = 0; i < 11; ++i)
            expectEquals(juce::String(s.labels()[i]), juce::String(was[i]));
        // A free slot's SRC and VIA cells offer them.
        W e;
        for (auto& page : e.getParamPages())
            for (auto& p : page.params)
                if (p.id == ModSlotParam::id(0, ModSlotParam::SRC) || p.id == ModSlotParam::id(0, ModSlotParam::VIA))
                {
                    expectEquals(p.numChoices, s.size());
                    expectEquals(juce::String(p.choices[p.numChoices - 1]), juce::String("TLAN"));
                }
        expectEquals(V::timeAsUnit(0.03f), 0.0f);
        expectEquals(V::timeAsUnit(2.0f), 1.0f);
        expectWithinAbsoluteError(V::timeAsUnit(0.245f), 0.5f, 0.002f);
        expectEquals(V::timeAsUnit(0.0f), 0.0f);
        expectEquals(V::timeAsUnit(100.0f), 1.0f);
    }

    // ---- pages and ids -------------------------------------------------------------------------
    void testPagesAndIds()
    {
        beginTest("Pages and ids: the engine's own pages, then the router's");
        W e;
        O old;
        juce::StringArray names, oldNames;
        for (auto& p : e.getParamPages()) names.add(p.name);
        for (auto& p : old.getParamPages()) oldNames.add(p.name);
        if (printing())
        {
            logMessage("U4-WSEQ | pages old: " + oldNames.joinIntoString(" "));
            logMessage("U4-WSEQ | pages new: " + names.joinIntoString(" "));
        }
        // The same pages in the same order, with VOICE (VOX cell) before the filter and the router's XIN page in front of the slots.
        juce::StringArray expected = oldNames;
        expected.insert(expected.indexOf("FILTR"), "VOICE");
        expected.insert(expected.indexOf("SLT1"), "XIN");
        expect(names == expected, "pages: " + names.joinIntoString(" "));
        expectEquals(names[names.size() - 1], juce::String("LFO2"));
        expectEquals(names[names.size() - 3], juce::String("STRIP"));

        // Every cell of every page is set and read back, and has a name; the router's ids are the router's.
        for (auto& page : e.getParamPages())
            for (auto& p : page.params)
            {
                if (! p.isValid())
                    continue;       // an empty cell is legal on any page (the owner: empty knobs are fine); every other check is for every page
                const float v = p.kind == ParamKind::Choice || p.kind == ParamKind::Int ? p.maxVal : 0.5f * (p.minVal + p.maxVal);
                e.setParameter(p.id, v);
                if (! (page.name == "FILTR" && juce::String(p.label) == "MODE"))       // (its choices follow TYPE)
                    expectWithinAbsoluteError(e.getParameter(p.id), v, 1.0e-4f * std::max(1.0f, std::abs(v)), page.name + " " + p.label);
                expect(e.getParameterName(p.id).isNotEmpty());
                const bool routers = page.name == "XIN" || page.name.startsWith("SLT") || page.name == "LSHP" || page.name == "STRIP" || page.name.startsWith("LFO");
                expect(R::owns(p.id) == routers, page.name + " " + p.label + ": 158-189 and 700-779 are the router's, and nothing else is");
            }
        // The DEST cells show the table.
        W fresh;
        for (auto& page : fresh.getParamPages())
            for (auto& p : page.params)
            {
                if (p.id == R::lfoId(0, R::DestCell) || p.id == R::stripDestId(0) || p.id == R::xinId(0))
                    expectEquals(p.numChoices, W::lfoDests().visible() + mod::TrackDest::COUNT, page.name + " DEST");     // (and the track's effect cells: E4)
                if (p.id == ModSlotParam::id(0, ModSlotParam::DEST))
                    expectEquals(p.numChoices, W::lfoDests().slotChoices());
            }
        expectEquals(fresh.getParameter(R::stripDestId(0)), (float) V::LfoRate, "the strip's default: the sequence faster");
        expectEquals(fresh.getParameter(R::stripAmountId(0)), 0.5f);
    }

    // ---- old against new: the factory presets --------------------------------------------------
    static std::vector<Preset> factoryPresets()
    {
        std::vector<Preset> out;
        for (auto& p : FactoryPresets::getAll())
            if (p.engineType == EngineType::WaveSeq)
                out.push_back(p);
        Preset init;                    // (PresetManager's "Init WS": the engine as it is made)
        init.name = "Init WS";
        init.engineType = EngineType::WaveSeq;
        out.push_back(init);
        return out;
    }

    void testFactoryPresets()
    {
        beginTest("Old against new: every factory preset, bit for bit");
        const auto presets = factoryPresets();
        expectEquals((int) presets.size(), 3, "Sequenced Bells, Morphing Texture, Init WS");
        for (auto& preset : presets)
        {
            const Setup setUp = [&preset](EngineBase& e)
            {
                for (auto& [id, value] : PresetManager::soundOf(preset))
                    e.setParameter(id, value);
            };
            for (int block : { 256, 100, 16 })
            {
                const Compared c = against(preset.name + " (blocks of " + juce::String(block) + ")", setUp, phrase(), 4.0, block);
                expect(c.identical, preset.name + " at blocks of " + juce::String(block) + ": " + verdict(c));
            }
            // ... and not because nothing sounded.
            auto e = std::make_unique<W>();
            setUp(*e);
            const auto m = mono(render(*e, phrase(), 4.0));
            expect(rms(m, 0, m.size()) > 0.01, preset.name + " sounds");
        }
        // A preset is the same sound at any block size (the sub-blocks change nothing at rest).
        // (One note at the start: an event lands on a block's first sample whatever the block is.)
        auto a = std::make_unique<W>(), b = std::make_unique<W>(), c = std::make_unique<W>();
        const Events start = { { 0.0, On, 52, 0.8f } };
        const Stereo big = render(*a, start, 1.024, 512), small = render(*b, start, 1.024, 48), odd = render(*c, start, 1.024, 7);
        expect(big.l == std::vector<float>(small.l.begin(), small.l.begin() + (long) big.l.size())
               && big.l == std::vector<float>(odd.l.begin(), odd.l.begin() + (long) big.l.size()), "the same samples at blocks of 512, 48 and 7");
    }

    // ---- old against new: the paths without modulation -----------------------------------------
    void testPlainPaths()
    {
        beginTest("Old against new: every loop mode but RAND, SLID, the chain, BODY, noise, the strip on the cutoff: bit for bit");
        struct Case { const char* name; Setup setUp; Events events; };
        const Events withStrip = { { 0.0, Strip, 0, 0.0f }, { 0.0, On, 52, 0.8f }, { 0.5, Strip, 0, 1.0f }, { 1.2, Strip, 0, 0.3f }, { 1.6, Off, 52, 0.0f } };
        const Events withBend = { { 0.0, On, 52, 0.8f }, { 0.3, Bend, 0, 0.7f }, { 0.8, Bend, 0, -1.0f }, { 1.6, Off, 52, 0.0f } };
        const std::vector<Case> cases = {
            { "loop REV", [](EngineBase& e) { e.setParameter(W::LoopModeParam, 1.0f); }, phrase() },
            { "loop PING", [](EngineBase& e) { e.setParameter(W::LoopModeParam, 2.0f); }, phrase() },
            { "loop ONCE", [](EngineBase& e) { e.setParameter(W::LoopModeParam, 4.0f); e.setParameter(W::Rate, 4.0f); }, phrase() },
            { "SLID on, XFAD 0", [](EngineBase& e) { e.setParameter(W::LaneInterp, 1.0f); e.setParameter(W::Crossfade, 0.0f); }, phrase() },
            { "lanes of 3, 5, 7 and 8, RATE 3", [](EngineBase& e)
              {
                  e.setParameter(W::PitchLaneLen, 3.0f); e.setParameter(W::ShapeLaneLen, 5.0f);
                  e.setParameter(W::LevelLaneLen, 7.0f); e.setParameter(W::Rate, 3.0f);
              }, phrase() },
            { "noise steps (SHP* NOIS, S1 SAW)", [](EngineBase& e) { e.setParameter(W::ShapeMix, 5.0f); e.setParameter(W::StepShape, 1.0f); }, phrase() },
            { "envelope chain SER", [](EngineBase& e) { e.setParameter(W::EnvChainMode, 1.0f); e.setParameter(W::EnvChainStages, 3.0f); }, phrase() },
            { "envelope chain PAR MAX", [](EngineBase& e) { e.setParameter(W::EnvChainMode, 2.0f); e.setParameter(W::EnvChainCombine, 1.0f); }, phrase() },
            { "BODY (MIX 70, plate)", [](EngineBase& e) { e.setParameter(W::BodyBase + VoiceBody::Mix, 0.7f); e.setParameter(W::BodyBase + VoiceBody::Morph, 3.0f); }, phrase() },
            { "filter LP 700 Hz with its envelope", [](EngineBase& e)
              {
                  openFilter(e, 700.0f);
                  e.setParameter(FB + VoiceFilterSection::EnvAmount, 0.6f);
              }, phrase() },
            { "WHL 80: the strip on the cutoff (routes off)", [](EngineBase& e)
              {
                  openFilter(e, 500.0f);
                  e.setParameter(FB + VoiceFilterSection::WheelAmount, 0.8f);
                  e.setParameter(R::stripAmountId(0), 0.0f);
              }, withStrip },
            { "TUNE, FINE and the bend", [](EngineBase& e) { e.setParameter(W::TuneParam, -5.0f); e.setParameter(W::FineParam, 30.0f); e.setParameter(W::BendRangeParam, 12.0f); }, withBend },
            { "tempo 87 with SYNC on", [](EngineBase& e) { e.setParameter(W::TempoSync, 1.0f); e.setTempo(87.0f); }, phrase() },
        };
        for (auto& c : cases)
        {
            const Compared r = against(c.name, c.setUp, c.events, 4.0);
            expect(r.identical, juce::String(c.name) + ": " + verdict(r));
        }
        // The WHL case is one in which the strip does something.
        auto with = std::make_unique<W>(), without = std::make_unique<W>();
        for (auto* e : { with.get(), without.get() })
        {
            cases[10].setUp(*e);
        }
        without->setParameter(FB + VoiceFilterSection::WheelAmount, 0.0f);
        expect(! same(render(*with, withStrip, 2.0), render(*without, withStrip, 2.0)), "WHL is heard");
    }

    // ---- old against new: what was folded into the router --------------------------------------
    void testFoldedFeatures()
    {
        beginTest("Old against new: the LFO destinations, the strip, the pads and the slots, measured");
        struct Case { const char* name; Setup setUp; Events events; bool mayBeLost; };
        const Events held = oneNote(2.4);
        const Events chord = { { 0.0, On, 50, 0.8f }, { 0.0, On, 57, 0.8f }, { 2.4, Off, 50, 0.0f }, { 2.4, Off, 57, 0.0f } };
        const Events stripUp = { { 0.0, Strip, 0, 1.0f }, { 0.3, On, 57, 0.8f }, { 2.4, Off, 57, 0.0f } };
        const Events pressed = { { 0.0, On, 57, 0.8f }, { 0.5, Pressure, 57, 0.8f }, { 1.5, Pressure, 57, 0.2f }, { 2.4, Off, 57, 0.0f } };
        const Events soft = { { 0.0, On, 57, 0.3f }, { 2.4, Off, 57, 0.0f } };
        const int P2 = dest("P2"), T1 = dest("T1"), L3 = dest("L3"), BMIX = dest("BMIX"), LEVL = dest("LEVL"), ATK = dest("ATK"),
                  LRAT = dest("LRAT"), FATK = dest("FATK"), ENVc = dest("ENV");
        const std::vector<Case> cases = {
            { "LFO1 > PTCH 30", [](EngineBase& e) { lfo(e, 0, V::LfoPitch, 0.3f); }, held, false },
            { "LFO1 > AMP 60", [](EngineBase& e) { lfo(e, 0, V::LfoAmp, 0.6f); }, held, false },
            { "LFO1 > PAN 80", [](EngineBase& e) { lfo(e, 0, V::LfoPan, 0.8f, 2.0f); }, held, false },
            { "LFO1 > CUT 50", [](EngineBase& e) { openFilter(e); lfo(e, 0, V::LfoCutoff, 0.5f, 3.0f); }, held, false },
            { "LFO1 > RESO 80", [](EngineBase& e) { openFilter(e); lfo(e, 0, V::LfoResonance, 0.8f, 3.0f); }, held, false },
            { "LFO1 > RATE 50", [](EngineBase& e) { lfo(e, 0, V::LfoRate, 0.5f, 1.0f); }, held, false },
            { "LFO1 > XFAD 100", [](EngineBase& e) { lfo(e, 0, V::LfoCrossfade, 1.0f, 1.0f); }, held, false },
            { "LFO1 > PRNG 60", [](EngineBase& e) { lfo(e, 0, V::LfoPitchRange, 0.6f, 1.0f); }, held, false },
            { "LFO1 > LEVL 50", [=](EngineBase& e) { lfo(e, 0, LEVL, 0.5f); }, held, false },
            { "LFO1 > ATK 40", [=](EngineBase& e) { lfo(e, 0, ATK, 0.4f, 1.0f); }, phrase(), false },
            { "LFO1 > P2 30", [=](EngineBase& e) { lfo(e, 0, P2, 0.3f, 2.0f); }, held, false },
            { "LFO1 > L3 -80", [=](EngineBase& e) { lfo(e, 0, L3, -0.8f, 2.0f); }, held, false },
            { "LFO1 > T1 60", [=](EngineBase& e) { lfo(e, 0, T1, 0.6f, 1.0f); }, held, false },
            { "LFO1 > BMIX 60 (body on)", [=](EngineBase& e) { e.setParameter(W::BodyBase + VoiceBody::Mix, 0.3f); lfo(e, 0, BMIX, 0.6f, 2.0f); }, held, false },
            { "LFO1 > FATK 50", [=](EngineBase& e) { openFilter(e, 400.0f); e.setParameter(FB + VoiceFilterSection::EnvAmount, 0.7f); lfo(e, 0, FATK, 0.5f, 1.0f); }, phrase(), false },
            { "LFO2 > LRAT 80, LFO1 > PTCH 40", [=](EngineBase& e) { lfo(e, 0, V::LfoPitch, 0.4f, 4.0f); lfo(e, 1, LRAT, 0.8f, 0.5f); }, held, false },
            { "LFO1 square > CUT 40, two notes", [](EngineBase& e) { openFilter(e); lfo(e, 0, V::LfoCutoff, 0.4f, 2.0f, 3); }, chord, false },
            { "strip up, default route (RATE +50)", nullptr, stripUp, false },
            { "strip moved under the note (RATE +50)", nullptr, { { 0.0, On, 57, 0.8f }, { 0.4, Strip, 0, 1.0f }, { 1.2, Strip, 0, 0.35f }, { 1.8, Strip, 0, 0.0f }, { 2.4, Off, 57, 0.0f } }, false },
            { "strip moved, WHL 60 and DST2 CUT -50", [](EngineBase& e)
              {
                  openFilter(e, 1500.0f);
                  e.setParameter(FB + VoiceFilterSection::WheelAmount, 0.6f);
                  e.setParameter(R::stripDestId(1), (float) V::LfoCutoff);
                  e.setParameter(R::stripAmountId(1), -0.5f);
              }, { { 0.0, On, 57, 0.8f }, { 0.4, Strip, 0, 1.0f }, { 1.2, Strip, 0, 0.35f }, { 2.4, Off, 57, 0.0f } }, false },
            { "strip up, STRIP DST2 CUT +60", [](EngineBase& e)
              {
                  openFilter(e, 400.0f);
                  e.setParameter(R::stripDestId(1), (float) V::LfoCutoff);
                  e.setParameter(R::stripAmountId(1), 0.6f);
              }, stripUp, false },
            { "pad pressed, default route (VOL)", nullptr, pressed, false },
            // (The pad's gain and the LFO's were multiplied; in the router they are one sum.)
            { "LFO1 > AMP 60 with the pad pressed", [](EngineBase& e) { lfo(e, 0, V::LfoAmp, 0.6f); }, pressed, true },
            { "slot ENV > CUT 50", [](EngineBase& e) { openFilter(e, 300.0f); slot(e, 0, 3, V::LfoCutoff, 0.5f); }, phrase(), false },
            { "slot VEL > AMP -80 (soft note)", [](EngineBase& e) { slot(e, 0, 5, V::LfoAmp, -0.8f); }, soft, false },
            { "slot KEY > filter ENV 60", [=](EngineBase& e) { openFilter(e, 300.0f); slot(e, 0, 8, ENVc, 0.6f); }, phrase(), false },
            { "slot LFO1 > PTCH 50 via STRP", [](EngineBase& e) { lfo(e, 0, 0, 0.0f); slot(e, 0, 1, V::LfoPitch, 0.5f, 7); e.setParameter(R::stripAmountId(0), 0.0f); }, stripUp, false },
            { "slot PRES > PAN 100", [](EngineBase& e) { slot(e, 0, 6, V::LfoPan, 1.0f); }, pressed, false },
            // One value a note from another generator: a different pan is the feature.
            { "slot RAND > PAN 100", [](EngineBase& e) { slot(e, 0, 9, V::LfoPan, 1.0f); }, held, true },
        };
        int close = 0, lost = 0, match = 0;
        for (auto& c : cases)
        {
            const Compared r = against(c.name, c.setUp, c.events, 3.0);
            const juce::String v = verdict(r);
            match += v == "MATCH";
            close += v == "CLOSE";
            lost += v == "LOST";
            if (! c.mayBeLost)
                expect(v != "LOST", juce::String(c.name) + juce::String::formatted(": LOST (level %+.2f dB, over time %.2f dB, bright %+.1f %%)", r.level, r.curve, 100.0 * r.bright));
            // The feature is heard at all in the new engine: the same gesture without the setup differs.
            if (c.setUp)
            {
                auto with = std::make_unique<W>(), without = std::make_unique<W>();
                c.setUp(*with);
                expect(! same(render(*with, c.events, 1.0), render(*without, c.events, 1.0)), juce::String(c.name) + " does something");
            }
        }
        if (printing())
            logMessage(juce::String::formatted("U4-WSEQ | folded features: %d MATCH, %d CLOSE, %d LOST", match, close, lost));

        // The one that is another sound by the measure: an LFO on the level under a pressed pad.
        // The old engine multiplied the pad's gain and the LFO's ((1 + p) x (1 + d x lfo)); the
        // router adds them (1 + p + d x lfo), so the tremolo no longer deepens with the pad. The
        // old product is the sum plus d x lfo x p, which is one more slot: LFO1 > AMP at the same
        // depth VIA PRES. With it the old sound is there again.
        const Compared rebuilt = against("that, with a slot LFO1 > AMP 60 via PRES", [](EngineBase& e) { lfo(e, 0, V::LfoAmp, 0.6f); }, pressed, 3.0, 256,
                                         nullptr, [](W& e) { slot(e, 0, 1, V::LfoAmp, 0.6f, 6); });
        expect(juce::String(verdict(rebuilt)) == "CLOSE" && rebuilt.curve < 0.3,
               juce::String::formatted("the old product of pad and LFO, rebuilt with a slot: level %+.2f dB, over time %.2f dB", rebuilt.level, rebuilt.curve));
    }

    // ---- PRNG beyond its cell ------------------------------------------------------------------
    void testPitchRangeBeyondItsCell()
    {
        beginTest("A preset's PRNG beyond the cell's range stays unless a slot is aimed at it");
        Preset bells;
        for (auto& p : factoryPresets())
            if (p.name == "Sequenced Bells")
                bells = p;
        expectEquals(bells.soundParams[W::PitchRange], 12.0f, "the preset holds PRNG 12 (the cell goes to 2)");
        const Setup preset = [&bells](EngineBase& e)
        {
            for (auto& [id, value] : PresetManager::soundOf(bells))
                e.setParameter(id, value);
        };
        const Events note = oneNote(1.2, 45);
        auto plain = std::make_unique<W>();
        preset(*plain);
        expectEquals(plain->getParameter(W::PitchRange), 12.0f, "stored as it is");
        const Stereo dry = render(*plain, note, 1.5);

        // A route elsewhere: ENV > PAN only ever takes from the left, so the right side is the
        // voice as it is. It must be the samples of the sound with nothing routed.
        auto elsewhere = std::make_unique<W>();
        preset(*elsewhere);
        slot(*elsewhere, 0, 3, V::LfoPan, 0.7f);
        const Stereo panned = render(*elsewhere, note, 1.5);
        expect(panned.r == dry.r, "a slot aimed elsewhere leaves PRNG 12 alone");
        expect(panned.l != dry.l, "(and that slot does act)");
        for (int v = 0; v < 8; ++v)
            expectEquals(elsewhere->getVoice(v).getPitchRange(), 12.0f);

        // A slot aimed at PRNG: the sum is held to the cell's 0..2. ONE at +100 adds 1: 13 held
        // to 2 is the preset with PRNG at 2.
        auto routed = std::make_unique<W>(), atTwo = std::make_unique<W>();
        preset(*routed);
        preset(*atTwo);
        slot(*routed, 0, place("ONE"), V::LfoPitchRange, 1.0f);
        atTwo->setParameter(W::PitchRange, 2.0f);
        const Stereo held = render(*routed, note, 1.5), two = render(*atTwo, note, 1.5);
        expect(held.r == two.r && held.l == two.l, "routed, it is held to the cell's top");
        expect(held.r != dry.r, "(which is another sound than 12)");
        expectEquals(routed->getParameter(W::PitchRange), 12.0f, "the stored value is not touched");
        for (int v = 0; v < 8; ++v)
            expectEquals(routed->getVoice(v).getPitchRange(), 12.0f, "nor the voices' set value");

        // The slot taken away under the note: 12 again (a new note is the unrouted sound).
        slot(*routed, 0, 0, 0, 0.0f);
        routed->allNotesOff();
        auto again = std::make_unique<W>();
        preset(*again);
        const Events later = { { 0.0, On, 45, 0.8f }, { 1.2, Off, 45, 0.0f } };
        const Stereo back = render(*routed, later, 1.5), fresh = render(*again, later, 1.5);
        expect(back.r == fresh.r, "the slot gone: PRNG 12 again");

        // The old engine did the same (its LFO path's clamp applied to routed cells only).
        const Compared c = against("Sequenced Bells, LFO1 > CUT 30", [&](EngineBase& e) { preset(e); openFilter(e, 2000.0f); lfo(e, 0, V::LfoCutoff, 0.3f, 2.0f); }, note, 1.5);
        expect(juce::String(verdict(c)) != "LOST", "PRNG 12 under a route elsewhere, old against new");
        const Compared d = against("Sequenced Bells, LFO1 > PRNG 50", [&](EngineBase& e) { preset(e); lfo(e, 0, V::LfoPitchRange, 0.5f, 2.0f); }, note, 1.5);
        expect(juce::String(verdict(d)) != "LOST", "PRNG 12 with an LFO on it, old against new");
    }

    // ---- the lanes as sources ------------------------------------------------------------------
    void testLanesAsSources()
    {
        beginTest("The lanes as sources: each lane's step, on the pan, on the cutoff, a lane of its own length");
        const float stepSeconds = 0.1f;
        const int block = 64;
        // What a source stands at in the middle of each step (the voice that sounds).
        auto readAtSteps = [&](W& e, int published, int steps)
        {
            std::vector<float> seen;
            render(e, { { 0.0, On, 60, 0.8f } }, stepSeconds * (float) steps, block, SR, nullptr, [&](long pos)
            {
                const double inStep = std::fmod((double) pos / SR, (double) stepSeconds) / stepSeconds;
                if (inStep >= 0.5 && inStep < 0.5 + (double) block / (SR * stepSeconds))
                    if (auto* v = sounding(e))
                        seen.push_back(v->getMod().source(mod::Src::Published1 + published));
            });
            return seen;
        };
        static const float pitches[4] = { 0.0f, 24.0f, -24.0f, 12.0f };
        static const float levels[4] = { 1.0f, 0.5f, 0.25f, 0.0f };
        static const float shapes[4] = { 0.0f, 1.0f, 3.0f, 5.0f };
        auto make = [&]()
        {
            auto e = std::make_unique<W>();
            sequence(*e, 4, stepSeconds);
            for (int s = 0; s < 4; ++s)
            {
                e->setParameter(W::StepPitch + s, pitches[s]);
                e->setParameter(W::StepLevel + s, levels[s]);
                e->setParameter(W::StepShape + s, shapes[s]);
            }
            return e;
        };
        {
            auto e = make();
            slot(*e, 0, place("PLAN"), V::LfoPan, 0.5f);
            const auto seen = readAtSteps(*e, V::PubPitch, 8);
            expectEquals((int) seen.size(), 8);
            for (size_t i = 0; i < seen.size(); ++i)
                expectWithinAbsoluteError(seen[i], pitches[i % 4] / 24.0f, 1.0e-3f, "PLAN at step " + juce::String((int) i));
        }
        {
            auto e = make();
            slot(*e, 0, place("LLAN"), V::LfoPan, 0.5f);
            const auto seen = readAtSteps(*e, V::PubLevel, 8);
            for (size_t i = 0; i < seen.size(); ++i)
                expectWithinAbsoluteError(seen[i], levels[i % 4], 1.0e-3f, "LLAN at step " + juce::String((int) i));
        }
        {
            auto e = make();
            slot(*e, 0, place("SLAN"), V::LfoPan, 0.5f);
            const auto seen = readAtSteps(*e, V::PubShape, 8);
            for (size_t i = 0; i < seen.size(); ++i)
                expectWithinAbsoluteError(seen[i], shapes[i % 4] / 5.0f, 1.0e-3f, "SLAN at step " + juce::String((int) i));
        }
        {
            // The time lane: steps of 0.1 s and 0.2 s.
            auto e = make();
            e->setParameter(W::TimingLaneLen, 2.0f);
            e->setParameter(W::StepTime + 1, 0.2f);
            slot(*e, 0, place("TLAN"), V::LfoPan, 0.5f);
            std::vector<float> seen;
            render(*e, { { 0.0, On, 60, 0.8f } }, 0.6, block, SR, nullptr, [&](long pos)
            {
                if (pos == 2368 || pos == 9600 || pos == 16768)       // 0.05 s, 0.2 s, 0.35 s
                    if (auto* v = sounding(*e))
                        seen.push_back(v->getMod().source(mod::Src::Published1 + V::PubTime));
            });
            expectEquals((int) seen.size(), 3);
            if (seen.size() == 3)
            {
                expectWithinAbsoluteError(seen[0], V::timeAsUnit(0.1f), 1.0e-3f);
                expectWithinAbsoluteError(seen[1], V::timeAsUnit(0.2f), 1.0e-3f);
                expectWithinAbsoluteError(seen[2], V::timeAsUnit(0.1f), 1.0e-3f);
            }
            expectWithinAbsoluteError(V::timeAsUnit(0.1f), 0.2867f, 0.001f);
        }
        {
            // A source nobody reads is not worked out.
            auto e = make();
            const auto seen = readAtSteps(*e, V::PubPitch, 4);
            for (float v : seen)
                expectEquals(v, 0.0f, "no slot reads PLAN: nothing is published");
        }
        {
            // A pitch lane of three steps against a time lane of four: a pattern of its own length.
            auto e = make();
            e->setParameter(W::PitchLaneLen, 3.0f);
            slot(*e, 0, place("PLAN"), V::LfoPan, 0.5f);
            const auto seen = readAtSteps(*e, V::PubPitch, 9);
            for (size_t i = 0; i < seen.size(); ++i)
                expectWithinAbsoluteError(seen[i], pitches[i % 3] / 24.0f, 1.0e-3f, "PLEN 3: step " + juce::String((int) i));
        }
        {
            // SLID: the pitch lane glides to the next step, and so does the source.
            auto e = make();
            e->setParameter(W::LaneInterp, 1.0f);
            slot(*e, 0, place("PLAN"), V::LfoPan, 0.5f);
            const auto seen = readAtSteps(*e, V::PubPitch, 2);
            if (seen.size() == 2)
                expectWithinAbsoluteError(seen[0], 0.5f * (pitches[0] + pitches[1]) / 24.0f, 0.03f, "half way from step 1 to step 2");
        }
        {
            // In the sound: PLAN > PAN at 100. The pan is the step / 24: 0, right, left, half right.
            auto e = make();
            for (int s = 0; s < 4; ++s)
                e->setParameter(W::StepLevel + s, 1.0f);
            slot(*e, 0, place("PLAN"), V::LfoPan, 1.0f);
            const Stereo out = render(*e, { { 0.0, On, 60, 0.8f } }, 0.4, block);
            auto side = [&](const std::vector<float>& x, int step) { return rms(x, (size_t) ((step + 0.4) * 0.1 * SR), (size_t) ((step + 0.9) * 0.1 * SR)); };
            expectWithinAbsoluteError(side(out.l, 0) / side(out.r, 0), 1.0, 0.01, "step 1 (0 st): centre");
            expect(side(out.l, 1) < 0.01 * side(out.r, 1), "step 2 (+24 st): all the way right");
            expect(side(out.r, 2) < 0.01 * side(out.l, 2), "step 3 (-24 st): all the way left");
            expectWithinAbsoluteError(side(out.l, 3) / side(out.r, 3), 0.5, 0.01, "step 4 (+12 st): half right");
        }
        {
            // On the filter: LLAN > CUT at 50 opens it by two octaves for a step of level 1.
            auto e = make();
            openFilter(*e, 500.0f);
            slot(*e, 0, place("LLAN"), V::LfoCutoff, 0.5f);
            std::vector<float> cut;
            render(*e, { { 0.0, On, 60, 0.8f } }, 0.4, block, SR, nullptr, [&](long pos)
            {
                const double inStep = std::fmod((double) pos / SR, 0.1) / 0.1;
                if (inStep >= 0.7 && inStep < 0.7 + (double) block / (SR * 0.1))
                    if (auto* v = sounding(*e))
                        cut.push_back(v->getFilter().getCurrentCutoff());
            });
            expectEquals((int) cut.size(), 4);
            if (cut.size() == 4)
                for (int s = 0; s < 4; ++s)
                    expectWithinAbsoluteError(std::log2(cut[(size_t) s] / 500.0f), 2.0f * levels[s], 0.05f, "the cutoff in octaves at step " + juce::String(s + 1));
        }
        {
            // A lane steps; what it moves does not jump. LLAN > AMP at 100 on a steady sine whose
            // level lane goes 1, 0.3: the gain goes from 2 to 1.3 and back, led as every source is
            // (no faster than 2 in 4 ms: 1 / 96 a sample at 48 kHz; unled, the sub-block's ramp
            // alone would take the 0.7 in 16 samples, four times as fast).
            auto routed = make(), plain = make();
            for (auto* e : { routed.get(), plain.get() })
            {
                for (int s = 0; s < 4; ++s)
                {
                    e->setParameter(W::StepPitch + s, 0.0f);
                    e->setParameter(W::StepLevel + s, s % 2 == 0 ? 1.0f : 0.3f);
                    e->setParameter(W::StepShape + s, 0.0f);
                }
            }
            slot(*routed, 0, place("LLAN"), V::LfoAmp, 1.0f);
            const Stereo a = render(*routed, { { 0.0, On, 60, 0.8f } }, 0.8, block), b = render(*plain, { { 0.0, On, 60, 0.8f } }, 0.8, block);
            float fastest = 0.0f, lowest = 10.0f, highest = 0.0f, last = -1.0f;
            for (size_t i = 4800; i < a.l.size(); ++i)
            {
                if (std::abs(b.l[i]) < 0.03f) { last = -1.0f; continue; }
                const float gain = a.l[i] / b.l[i];
                if (last > 0.0f) fastest = std::max(fastest, std::abs(gain - last));
                last = gain;
                lowest = std::min(lowest, gain);
                highest = std::max(highest, gain);
            }
            expectWithinAbsoluteError(lowest, 1.3f, 0.01f, "the gain at the quiet step");
            expectWithinAbsoluteError(highest, 2.0f, 0.01f, "the gain at the loud step");
            expect(fastest < 0.0125f, "a lane's step is led, not jumped (" + juce::String(fastest, 5) + " a sample at most)");
            expect(fastest > 0.008f, "(and it is a step, not a slow glide)");
        }
        {
            // On another lane's steps, on the body and on an LFO's rate: any cell (finite, and heard).
            auto e = make(), plain = make();
            for (auto* x : { e.get(), plain.get() })
            {
                x->setParameter(W::BodyBase + VoiceBody::Mix, 0.5f);
                lfo(*x, 0, V::LfoPitch, 0.3f, 4.0f);
            }
            slot(*e, 0, place("PLAN"), dest("T1"), 0.5f);
            slot(*e, 1, place("SLAN"), dest("BMRP"), 0.8f);
            slot(*e, 2, place("LLAN"), W::lfoDests().visible(), 0.9f);      // L1RT: the first entry after the table
            slot(*e, 3, place("TLAN"), dest("L2"), -0.5f, place("PLAN"));
            const Stereo a = render(*e, oneNote(1.0), 1.5), b = render(*plain, oneNote(1.0), 1.5);
            expect(finite(a));
            expect(! same(a, b), "the lanes on a step, the body and an LFO's rate are heard");
        }
    }

    // ---- the random loop -----------------------------------------------------------------------
    // The steps the pitch lane of the first sounding voice stands at, block by block.
    template <typename Engine>
    static std::vector<int> laneWalk(Engine& e, const Events& events, double seconds, int voice = -1)
    {
        std::vector<int> walk;
        render(e, events, seconds, 64, SR, nullptr, [&](long)
        {
            for (int v = 0; v < 8; ++v)
                if ((voice < 0 || v == voice) && e.getVoice(v).isActive())
                {
                    walk.push_back(e.getVoice(v).getLaneConfig(1).currentStep);
                    return;
                }
        });
        return walk;
    }
    static void randomPatch(EngineBase& e)
    {
        sequence(e, 16, 0.03f);
        e.setParameter(W::Rate, 2.0f);
        e.setParameter(W::LoopModeParam, 3.0f);
        for (int s = 0; s < 16; ++s)
            e.setParameter(W::StepPitch + s, (float) (s - 8));
    }
    // How unevenly the sixteen steps were visited: the largest share against 1 / 16, and the smallest.
    static std::pair<double, double> spread(const std::vector<int>& walk)
    {
        std::array<int, 16> count {};
        for (int s : walk)
            ++count[(size_t) std::clamp(s, 0, 15)];
        const double even = (double) walk.size() / 16.0;
        return { *std::max_element(count.begin(), count.end()) / even, *std::min_element(count.begin(), count.end()) / even };
    }

    void testRandomLoop()
    {
        beginTest("The random loop: from the one generator; repeats, differs by voice and by note; as even as the old one");
        const Events note = { { 0.0, On, 57, 0.8f } };
        auto a = std::make_unique<W>(), b = std::make_unique<W>();
        randomPatch(*a);
        randomPatch(*b);
        const auto walkA = laneWalk(*a, note, 20.0), walkB = laneWalk(*b, note, 20.0);
        expect(walkA == walkB, "the same notes on a new engine: the same steps");
        const auto [most, least] = spread(walkA);
        auto old = std::make_unique<O>();
        randomPatch(*old);
        const auto [oldMost, oldLeast] = spread(laneWalk(*old, note, 20.0));
        if (printing())
            logMessage(juce::String::formatted("U4-WSEQ | random loop, 1333 steps over 16: most / least visited step against even: old %.2f / %.2f, new %.2f / %.2f",
                                               oldMost, oldLeast, most, least));
        expect(most < 1.5 && least > 0.5, juce::String::formatted("every step is as likely as the other (%.2f / %.2f)", most, least));
        int moves = 0;
        for (size_t i = 1; i < walkA.size(); ++i)
            moves += walkA[i] != walkA[i - 1];
        expect(moves > 1000, "it steps (" + juce::String(moves) + " changes)");

        // Two voices of a chord walk different ways (the old engine's walked the same way: every
        // voice began from the same number).
        const Events chord = { { 0.0, On, 57, 0.8f }, { 0.0, On, 64, 0.8f } };
        auto c = std::make_unique<W>(), d = std::make_unique<W>();
        randomPatch(*c);
        randomPatch(*d);
        expect(laneWalk(*c, chord, 2.0, 0) != laneWalk(*d, chord, 2.0, 1), "voice 1 and voice 2 draw their own steps");
        auto oc = std::make_unique<O>(), od = std::make_unique<O>();
        randomPatch(*oc);
        randomPatch(*od);
        if (printing())
            logMessage(juce::String("U4-WSEQ | random loop, a chord's two voices walk the same steps: old ")
                       + (laneWalk(*oc, chord, 2.0, 0) == laneWalk(*od, chord, 2.0, 1) ? "yes" : "no") + ", new no");

        // The second note of a voice is another walk than its first.
        auto f = std::make_unique<W>();
        randomPatch(*f);
        const auto first = laneWalk(*f, note, 1.0, 0);
        f->allNotesOff();
        const auto second = laneWalk(*f, note, 1.0, 0);
        expect(first.size() == second.size() && first != second, "note by note another walk");

        // The noise step's generator is its own: a random loop no longer moves it. And RAND of the
        // router comes from the same generator as the walk (a new engine: the same pan).
        auto g = std::make_unique<W>(), h = std::make_unique<W>();
        for (auto* e : { g.get(), h.get() })
        {
            randomPatch(*e);
            slot(*e, 0, place("RAND"), V::LfoPan, 1.0f);
            slot(*e, 1, place("RND3"), V::LfoCutoff, 0.5f);
        }
        expect(same(render(*g, phrase(), 3.0), render(*h, phrase(), 3.0)), "RAND, RND3 and the walk repeat together");

        // Old against new. The old loop was no random walk where the lanes are 2, 4, 8 or 16 long:
        // its LCG's low bits, shared out over four lanes, send each lane round 4 steps of 16 (or
        // between 2 of 8). The new one reaches every step. So RAND is another sound than it was:
        // never MATCH, and LOST by the measure wherever the steps the old cycle left out sound
        // different from those it kept (noise on the even steps here: the old cycle never met
        // them). The old loop is therefore kept as LOOP's sixth choice, ORND: that one is the old
        // samples.
        const Events longNote = { { 0.0, On, 57, 0.8f }, { 7.5, Off, 57, 0.0f } };
        const Setup pure = [](EngineBase& e) { randomPatch(e); };
        const Setup noisy = [](EngineBase& e) { randomPatch(e); for (int s = 0; s < 16; s += 2) e.setParameter(W::StepShape + s, 5.0f); };
        const Setup sevens = [](EngineBase& e)
        {
            randomPatch(e);
            for (int id : { (int) W::TimingLaneLen, (int) W::PitchLaneLen, (int) W::ShapeLaneLen, (int) W::LevelLaneLen })
                e.setParameter(id, 7.0f);
        };
        const Setup defaults = [](EngineBase& e) { e.setParameter(W::LoopModeParam, 3.0f); };
        const Compared r = against("loop RAND, 16 steps", pure, longNote, 8.0);
        const Compared n = against("loop RAND, 16 steps, noise on the even ones", noisy, longNote, 8.0);
        const Compared t = against("loop RAND, lanes of 7", sevens, longNote, 8.0);
        const Compared q = against("loop RAND, the default sequence (8)", defaults, phrase(), 4.0);
        for (auto* c : { &r, &n, &t, &q })
            expect(! c->identical, "the walk is not the old one: never MATCH");
        // The measure does tell the two apart where they are apart.
        expect(juce::String(verdict(n)) == "LOST", "the old cycle left the noise steps out: by the measure the old sound is not the new one");
        // ORND against the old engine's RAND: the same samples, a chord and the noise included.
        const std::vector<std::pair<const char*, Setup>> walks = { { "16 steps", pure }, { "noise on the even steps", noisy },
                                                                   { "lanes of 7", sevens }, { "the default sequence", defaults } };
        for (auto& walk : walks)
        {
            const Compared c = against(juce::String("loop ORND against the old RAND, ") + walk.first, walk.second, phrase(), 4.0, 256, nullptr,
                                       [](W& e) { e.setParameter(W::LoopModeParam, 5.0f); });
            expect(c.identical, juce::String("ORND is the old random loop, ") + walk.first + ": " + verdict(c));
        }
        W limits;
        expectEquals(limits.getParameter(W::LoopModeParam), 0.0f);
        limits.setParameter(W::LoopModeParam, 5.0f);
        expect(limits.getVoice(0).getLoopMode() == V::LoopMode::OldRandom);
        limits.setParameter(W::LoopModeParam, 3.0f);
        expect(limits.getVoice(0).getLoopMode() == V::LoopMode::Random, "a stored RAND is the new walk");
    }

    // ---- RATE ----------------------------------------------------------------------------------
    template <typename Engine>
    static int stepsTaken(Engine& e, const Events& events, double seconds)
    {
        const auto walk = laneWalk(e, events, seconds);
        int changes = 0;
        for (size_t i = 1; i < walk.size(); ++i)
            changes += walk[i] != walk[i - 1];
        return changes;
    }

    void testRate()
    {
        beginTest("RATE: a factor on the sequence's speed, two octaves at full depth, not held to the cell's range");
        const Events note = { { 0.0, On, 57, 0.8f } };
        const Events stripUp = { { 0.0, Strip, 0, 1.0f }, { 0.0, On, 57, 0.8f } };
        auto make = [](float rate = 1.0f)
        {
            auto e = std::make_unique<W>();
            sequence(*e, 16, 0.1f);
            e->setParameter(W::Rate, rate);
            return e;
        };
        auto plain = make();
        expectWithinAbsoluteError(stepsTaken(*plain, note, 2.0), 19, 1);
        auto strip = make();
        expectWithinAbsoluteError(stepsTaken(*strip, stripUp, 2.0), 39, 2, "the strip's default route: up to twice as fast");
        auto oldStrip = std::make_unique<O>();
        sequence(*oldStrip, 16, 0.1f);
        expectWithinAbsoluteError(stepsTaken(*oldStrip, stripUp, 2.0), 39, 2, "(as the old engine)");
        auto faster = make(), slower = make(), beyond = make(4.0f);
        slot(*faster, 0, place("ONE"), V::LfoRate, 0.5f);
        slot(*slower, 0, place("ONE"), V::LfoRate, -0.5f);
        slot(*beyond, 0, place("ONE"), V::LfoRate, 0.5f);
        expectWithinAbsoluteError(stepsTaken(*faster, note, 2.0), 39, 1, "+50: an octave faster");
        expectWithinAbsoluteError(stepsTaken(*slower, note, 2.0), 9, 1, "-50: an octave slower");
        expectWithinAbsoluteError(stepsTaken(*beyond, note, 2.0), 159, 3, "RATE at its top and +50: eight times, beyond the cell's 4");
        expectEquals(beyond->getParameter(W::Rate), 4.0f);
        // A lane on the rate: the level lane makes the loud steps short (LLAN > RATE).
        auto laned = make();
        for (int s = 0; s < 16; ++s)
            laned->setParameter(W::StepLevel + s, s % 2 == 0 ? 1.0f : 0.0f);
        slot(*laned, 0, place("LLAN"), V::LfoRate, 0.5f);
        const int steps = stepsTaken(*laned, note, 2.0);
        expect(steps >= 25 && steps <= 28, "every other step twice as fast: 26 or 27 steps in 2 s (" + juce::String(steps) + ")");
    }

    // ---- ETIM / FTIM ---------------------------------------------------------------------------
    void testEnvelopeTimes()
    {
        beginTest("ETIM and FTIM: the envelopes' times as destinations");
        const int ONE = place("ONE");
        const Events note = { { 0.0, On, 57, 0.8f }, { 0.8, Off, 57, 0.0f } };
        auto setUp = [](W& e, float attack, float decay, float release)
        {
            sequence(e, 1, 0.25f);
            e.setParameter(W::AmpAttack, attack);
            e.setParameter(W::AmpDecay, decay);
            e.setParameter(W::AmpSustain, 0.4f);
            e.setParameter(W::AmpRelease, release);
        };
        // ONE > ETIM at -33.3: one octave down of three, every stage half as long.
        auto routed = std::make_unique<W>(), halved = std::make_unique<W>(), asSet = std::make_unique<W>();
        setUp(*routed, 0.2f, 0.3f, 0.6f);
        setUp(*asSet, 0.2f, 0.3f, 0.6f);
        setUp(*halved, 0.1f, 0.15f, 0.3f);
        slot(*routed, 0, ONE, WaveSeqEngine::dests().envTime, -1.0f / 3.0f);
        const auto a = mono(render(*routed, note, 1.6)), b = mono(render(*halved, note, 1.6)), c = mono(render(*asSet, note, 1.6));
        double worst = 0.0, apart = 0.0;
        const size_t frame = 480;
        for (size_t f = 0; f + frame <= a.size(); f += frame)
        {
            const double x = rms(a, f, f + frame), y = rms(b, f, f + frame), z = rms(c, f, f + frame);
            if (std::max(x, y) > 1.0e-3)
            {
                worst = std::max(worst, std::abs(db(x / std::max(y, 1.0e-9))));
                apart = std::max(apart, std::abs(db(z / std::max(y, 1.0e-9))));
            }
        }
        expect(worst < 0.5, juce::String::formatted("ETIM at half is the envelope with its times halved (%.2f dB apart at most)", worst));
        expect(apart > 6.0, juce::String::formatted("(the envelope as set is another one: %.1f dB)", apart));
        expectEquals(routed->getParameter(W::AmpRelease), 0.6f, "the cells stay as set");

        // The chain's stages follow ETIM as well.
        auto chainRouted = std::make_unique<W>(), chainSet = std::make_unique<W>();
        for (auto* e : { chainRouted.get(), chainSet.get() })
        {
            setUp(*e, 0.2f, 0.3f, 0.6f);
            e->setParameter(W::EnvChainMode, 1.0f);
        }
        slot(*chainRouted, 0, ONE, WaveSeqEngine::dests().envTime, -2.0f / 3.0f);
        const auto ca = mono(render(*chainRouted, note, 0.3)), cb = mono(render(*chainSet, note, 0.3));
        expect(rms(ca, 0, 2400) > 1.5 * rms(cb, 0, 2400), "the chain's attack is shorter under ETIM");

        // The source ENV is the chain while CHAN is on (the old engine read the amp envelope,
        // which stands still then: ENV gave nothing).
        auto chained = std::make_unique<W>();
        setUp(*chained, 0.01f, 0.3f, 0.6f);
        chained->setParameter(W::EnvChainMode, 1.0f);
        slot(*chained, 0, place("ENV"), V::LfoPan, 1.0f);
        const Stereo led = render(*chained, note, 0.6);
        expect(rms(led.l, 4800, 24000) < 0.8 * rms(led.r, 4800, 24000), "ENV > PAN under the chain: the envelope pans the voice");

        // FTIM: the filter's envelope, read at the filter.
        auto filterSetUp = [&](W& e, float attack, float decay)
        {
            setUp(e, 0.005f, 0.3f, 0.3f);
            openFilter(e, 300.0f);
            e.setParameter(FB + VoiceFilterSection::EnvAmount, 0.8f);
            e.setParameter(FB + VoiceFilterSection::Attack, attack);
            e.setParameter(FB + VoiceFilterSection::Decay, decay);
            e.setParameter(FB + VoiceFilterSection::Sustain, 0.2f);
        };
        auto trace = [&](W& e)
        {
            std::vector<float> levels;
            render(e, note, 0.7, 64, SR, nullptr, [&](long) { if (auto* v = sounding(e)) levels.push_back(v->getFilter().getEnvelope().getCurrentLevel()); });
            return levels;
        };
        auto fr = std::make_unique<W>(), fh = std::make_unique<W>(), fs = std::make_unique<W>();
        filterSetUp(*fr, 0.2f, 0.4f);
        filterSetUp(*fs, 0.2f, 0.4f);
        filterSetUp(*fh, 0.1f, 0.2f);
        slot(*fr, 0, ONE, WaveSeqEngine::dests().filterTime, -1.0f / 3.0f);
        const auto tr = trace(*fr), th = trace(*fh), ts = trace(*fs);
        float off = 0.0f, other = 0.0f;
        for (size_t i = 0; i < std::min({ tr.size(), th.size(), ts.size() }); ++i)
        {
            off = std::max(off, std::abs(tr[i] - th[i]));
            other = std::max(other, std::abs(ts[i] - th[i]));
        }
        expect(tr.size() > 400);
        expect(off < 0.02f, "FTIM at half is the filter envelope with its times halved (" + juce::String(off, 4) + ")");
        expect(other > 0.2f, "(the envelope as set is another one)");
        // ... and it leaves the amp envelope alone.
        auto ampOnly = std::make_unique<W>();
        filterSetUp(*ampOnly, 0.2f, 0.4f);
        ampOnly->setParameter(FB + VoiceFilterSection::EnvAmount, 0.0f);
        auto ampRouted = std::make_unique<W>();
        filterSetUp(*ampRouted, 0.2f, 0.4f);
        ampRouted->setParameter(FB + VoiceFilterSection::EnvAmount, 0.0f);
        slot(*ampRouted, 0, ONE, WaveSeqEngine::dests().filterTime, -1.0f / 3.0f);
        expect(same(render(*ampOnly, note, 1.2), render(*ampRouted, note, 1.2)), "FTIM does not move the amp envelope");
    }

    // ---- PM --------------------------------------------------------------------------------------
    void testPhaseModulation()
    {
        beginTest("PM: the track input on the oscillator's phase, at audio rate");
        const double carrier = dsp::midiNoteToHz(69), modulator = 110.0;
        const Input tone = [modulator](long n) { return 0.5f * (float) std::sin(2.0 * 3.141592653589793 * modulator * (double) n / SR); };
        auto make = [](float amount)
        {
            auto e = std::make_unique<W>();
            sequence(*e, 1, 0.25f);
            if (amount != 0.0f)
                slot(*e, 0, place("XIN"), WaveSeqEngine::dests().phase, amount);
            return e;
        };
        // amount 0.05 x an input of 0.5 = 0.025 cycles = an index of 0.157 radians: the first
        // side bands are J1 / J0 = 0.0787 of the carrier.
        auto e = make(0.05f);
        const auto out = mono(render(*e, { { 0.0, On, 69, 0.8f } }, 1.0, 256, SR, &tone));
        const size_t from = 9600, n = 32768;
        const double c = std::sqrt(fastmeasure::goertzel(out, from, n, carrier, SR));
        const double upper = std::sqrt(fastmeasure::goertzel(out, from, n, carrier + modulator, SR));
        const double lower = std::sqrt(fastmeasure::goertzel(out, from, n, carrier - modulator, SR));
        expectWithinAbsoluteError(upper / c, 0.0787, 0.006, "the upper side band");
        expectWithinAbsoluteError(lower / c, 0.0787, 0.006, "the lower side band");
        // Without the slot: no side bands, input or not (and the samples of the engine with no input).
        auto plain = make(0.0f), alone = make(0.0f);
        const Stereo withInput = render(*plain, { { 0.0, On, 69, 0.8f } }, 1.0, 256, SR, &tone);
        const auto dry = mono(withInput);
        expect(std::sqrt(fastmeasure::goertzel(dry, from, n, carrier + modulator, SR)) < 0.002 * c, "nothing routed: no side band");
        expect(same(withInput, render(*alone, { { 0.0, On, 69, 0.8f } }, 1.0)), "an input nobody reads changes no sample");
        // Deeper: more of it.
        auto deep = make(0.2f);
        const auto loud = mono(render(*deep, { { 0.0, On, 69, 0.8f } }, 1.0, 256, SR, &tone));
        expect(std::sqrt(fastmeasure::goertzel(loud, from, n, carrier + modulator, SR)) > 3.0 * upper, "four times the amount: about four times the side band");
        // An LFO on PM is a vibrato of its own kind: heard, finite.
        auto slow = make(0.0f), still = make(0.0f);
        lfo(*slow, 0, WaveSeqEngine::dests().phase, 1.0f, 6.0f);
        const Stereo wob = render(*slow, oneNote(0.8), 1.0);
        expect(finite(wob) && ! same(wob, render(*still, oneNote(0.8), 1.0)));
    }

    // ---- pressure per voice ----------------------------------------------------------------------
    void testPressurePerVoice()
    {
        beginTest("Pad pressure reaches its own voice only; the routes are slots (VOL, CUT, PAN, vibrato)");
        const double low = dsp::midiNoteToHz(48), high = dsp::midiNoteToHz(67);
        auto level = [&](const Stereo& s, double hz, double at) { return std::sqrt(fastmeasure::goertzel(mono(s), (size_t) (at * SR), 16384, hz, SR)); };
        auto make = []
        {
            auto e = std::make_unique<W>();
            sequence(*e, 1, 0.25f);
            return e;
        };
        const Events plain = { { 0.0, On, 48, 0.8f }, { 0.0, On, 67, 0.8f } };
        const Events pressed = { { 0.0, On, 48, 0.8f }, { 0.0, On, 67, 0.8f }, { 0.1, Pressure, 67, 0.5f } };
        auto a = make(), b = make();
        const Stereo dry = render(*a, plain, 1.0), wet = render(*b, pressed, 1.0);
        expectWithinAbsoluteError(level(wet, high, 0.4) / level(dry, high, 0.4), 1.5, 0.02, "the default route: pressure 50 = half as loud again");
        expectWithinAbsoluteError(level(wet, low, 0.4) / level(dry, low, 0.4), 1.0, 0.01, "the other voice is not touched");

        // The track's own routes: the cutoff, and the pan (which the engine could not do before).
        AftertouchRouter routes;
        routes.setRoute(0, { AftertouchRouter::Destination::PanSpread, 1.0f, false });
        auto c = make();
        c->setAftertouchRouter(&routes);
        const Stereo panned = render(*c, pressed, 1.0);
        auto side = [&](const std::vector<float>& x, double hz) { return std::sqrt(fastmeasure::goertzel(x, (size_t) (0.4 * SR), 16384, hz, SR)); };
        expectWithinAbsoluteError(side(panned.l, high) / side(panned.r, high), 0.5, 0.02, "PAN route: pressure 50 = half right");
        expectWithinAbsoluteError(side(panned.l, low) / side(panned.r, low), 1.0, 0.01);

        routes.setRoute(0, { AftertouchRouter::Destination::FilterCutoff, 0.5f, false });
        auto d = make();
        openFilter(*d, 400.0f);
        d->setAftertouchRouter(&routes);
        float cutoff = 0.0f;
        render(*d, { { 0.0, On, 67, 0.8f }, { 0.1, Pressure, 67, 1.0f } }, 0.5, 64, SR, nullptr, [&](long) { if (auto* v = sounding(*d)) cutoff = v->getFilter().getCurrentCutoff(); });
        expectWithinAbsoluteError(std::log2(cutoff / 400.0f), 2.0f, 0.05f, "CUT route at 50: two octaves at full pressure (as before: 4 x depth)");
    }

    // ---- a cell a slot held goes back ------------------------------------------------------------
    void testControlCellsGoBack()
    {
        beginTest("A page cell a slot moved goes back to its set value; a silent voice is given it back too");
        auto e = std::make_unique<W>();
        sequence(*e, 2, 0.1f);
        e->setParameter(W::StepLevel, 0.5f);
        slot(*e, 0, place("ONE"), dest("L1"), 0.4f);
        slot(*e, 1, place("ONE"), dest("FATK"), 0.5f);
        render(*e, { { 0.0, On, 60, 0.8f } }, 0.2);
        auto* v = sounding(*e);
        expect(v != nullptr);
        if (v == nullptr) return;
        expectWithinAbsoluteError(v->getStep(0).level, 0.9f, 1.0e-3f, "L1 0.5 moved by +0.4");
        expectEquals(e->getParameter(W::StepLevel), 0.5f, "the cell's own value stays");
        // Set under the slot: the slot moves the new value.
        e->setParameter(W::StepLevel, 0.3f);
        render(*e, {}, 0.1);
        expectWithinAbsoluteError(v->getStep(0).level, 0.7f, 1.0e-3f);
        // The slot goes: the set value comes back (carried off over a few milliseconds).
        slot(*e, 0, 0, 0, 0.0f);
        render(*e, {}, 0.1);
        expectWithinAbsoluteError(v->getStep(0).level, 0.3f, 1.0e-4f, "the slot gone: the set value");
        // A voice that fell silent holding a cell is given it back.
        slot(*e, 0, place("ONE"), dest("L2"), 0.3f);
        e->setParameter(W::StepLevel + 1, 0.2f);
        render(*e, {}, 0.1);
        expectWithinAbsoluteError(v->getStep(1).level, 0.5f, 1.0e-3f);
        e->allNotesOff();
        slot(*e, 0, 0, 0, 0.0f);
        render(*e, {}, 0.1);
        for (int i = 0; i < 8; ++i)
            expectWithinAbsoluteError(e->getVoice(i).getStep(1).level, 0.2f, 1.0e-4f, "voice " + juce::String(i));
    }

    // ---- a voice with BODY ends -------------------------------------------------------------------
    // The start of the last block in which a voice of the engine sounded (-1: none ever did).
    template <typename Engine>
    static long lastSounding(Engine& e, const Events& events, double seconds, int block)
    {
        long last = -1;
        render(e, events, seconds, block, SR, nullptr, [&](long pos)
        {
            for (int v = 0; v < 8; ++v)
                if (e.getVoice(v).isActive())
                    last = pos;
        });
        return last;
    }

    void testBodyRingsOut()
    {
        beginTest("A voice with BODY is given back once its body has rung out: in the very block the old engine gave it back");
        const Events note = { { 0.0, On, 60, 0.8f }, { 0.1, Off, 60, 0.0f } };
        const Setup body = [](EngineBase& e)
        {
            sequence(e, 1, 0.25f);
            e.setParameter(W::BodyBase + VoiceBody::Mix, 0.6f);
            e.setParameter(W::BodyBase + VoiceBody::Decay, 0.05f);
            e.setParameter(W::AmpRelease, 0.01f);
        };
        for (int block : { 64, 100, 7, 1 })
        {
            auto older = std::make_unique<O>();
            auto newer = std::make_unique<W>();
            body(*older);
            body(*newer);
            const long was = lastSounding(*older, note, 3.0, block), is = lastSounding(*newer, note, 3.0, block);
            expect(was > (long) (0.12 * SR) && was < (long) (2.5 * SR), "the old voice rang out and ended");
            // (A body that was put away in the middle of a sub-block must not be started again by
            // the samples left of it: the voice would sound, silent, for some sub-blocks more.)
            expectEquals((int) is, (int) was, "blocks of " + juce::String(block) + ": the voice is free in the same block");
        }
        // ... and with a slot in use (the other render loop): free, and not much later than without.
        auto routed = std::make_unique<W>(), plain = std::make_unique<W>();
        body(*routed);
        body(*plain);
        lfo(*routed, 0, V::LfoPan, 0.5f);
        const long with = lastSounding(*routed, note, 3.0, 7), without = lastSounding(*plain, note, 3.0, 7);
        expect(with > 0 && std::abs(with - without) <= 16, juce::String::formatted("routed: free within a sub-block of the plain voice (%ld against %ld)", with, without));
    }

    // ---- real time -------------------------------------------------------------------------------
    static void everything(W& e)
    {
        e.setParameter(W::BodyBase + VoiceBody::Mix, 0.5f);
        openFilter(e, 800.0f);
        lfo(e, 0, V::LfoPitch, 0.4f, 6.0f);
        lfo(e, 1, V::LfoRate, 0.7f, 0.8f);
        slot(e, 0, place("XIN"), WaveSeqEngine::dests().phase, 0.5f);
        slot(e, 1, place("PLAN"), V::LfoCutoff, 0.6f, place("LLAN"));
        slot(e, 2, place("SLAN"), WaveSeqEngine::dests().envTime, -0.4f);
        slot(e, 3, place("TLAN"), dest("BMRP"), 0.7f);
        slot(e, 4, place("ENV"), V::LfoPan, 0.5f);
        slot(e, 5, place("RND2"), dest("P3"), 0.3f);
        slot(e, 6, place("XLP"), V::LfoAmp, 0.6f);
        slot(e, 7, place("PRES"), WaveSeqEngine::dests().filterTime, 0.5f);
        slot(e, 8, place("ONST"), V::LfoCrossfade, 0.8f);
        slot(e, 9, place("STRP"), V::LfoPitchRange, 0.8f);
        e.setParameter(W::LoopModeParam, 3.0f);
    }

    void testRealTime()
    {
        beginTest("Real time: no allocation in the block; finite at the ends of pitch, rate and block size");
        {
            auto e = std::make_unique<W>();
            everything(*e);
            e->prepareToPlay(SR, 256);
            juce::AudioBuffer<float> buf(2, 256);
            juce::MidiBuffer midi;
            std::vector<float> in(256);
            for (int i = 0; i < 256; ++i)
                in[(size_t) i] = 0.6f * std::sin(0.05f * (float) i);
            AudioInput ai;
            ai.left = ai.right = in.data();
            ai.frames = 256;
            ai.envelope = 0.4f;
            e->setAudioInput(ai);
            e->setModWheel(0.7f);
            for (int n : { 40, 52, 59, 64 })
                e->noteOn(n, 0.8f, -1);
            e->aftertouch(52, 0.6f);
            for (int b = 0; b < 20; ++b) { buf.clear(); e->processBlock(buf, midi); }      // (the slots are resolved)
            auto* before = malloc_logger;
            malloc_logger = ModRouterAllocations::count;
            ModRouterAllocations::events = 0;
            ModRouterAllocations::counting = true;
            for (int b = 0; b < 200; ++b)
            {
                if (b == 60) { e->noteOff(52, -1); e->noteOn(71, 0.5f, -1); }
                if (b == 120) e->setParameter(ModSlotParam::id(1, ModSlotParam::AMT), -0.3f);
                e->processBlock(buf, midi);
            }
            ModRouterAllocations::counting = false;
            malloc_logger = before;
            expectEquals(ModRouterAllocations::events, 0, "no allocation while playing with every kind of slot in use");
        }
        const Input bad = [](long n) { return n % 97 == 0 ? std::numeric_limits<float>::quiet_NaN() : (n % 2 ? 4.0f : -4.0f); };
        for (double sr : { 44100.0, 96000.0, 192000.0 })
            for (int block : { 1, 7, 64, 500 })
            {
                auto e = std::make_unique<W>();
                everything(*e);
                e->setParameter(W::TuneParam, 24.0f);
                e->setParameter(W::BendRangeParam, 24.0f);
                const Events ends = { { 0.0, Strip, 0, 1.0f }, { 0.0, Bend, 0, 1.0f }, { 0.0, On, 127, 1.0f }, { 0.0, On, 0, 1.0f }, { 0.0, On, 60, 1.0f },
                                      { 0.02, Pressure, 60, 1.0f }, { 0.05, Bend, 0, -1.0f }, { 0.08, Off, 127, 0.0f } };
                const Stereo out = render(*e, ends, block == 1 ? 0.03 : 0.12, block, sr, &bad);
                expect(finite(out), juce::String::formatted("finite and bounded at %.0f Hz, blocks of %d", sr, block));
            }
    }

    // =============================================================================================
    // THE REVIEW'S ADDITIONS (2026-11-22)
    // =============================================================================================

    // ---- old against new at the other sample rates ----------------------------------------------
    void testReviewSampleRates()
    {
        beginTest("Review: old against new at 44.1 and 96 kHz, and with blocks that are no whole sub-blocks");
        const Events held = oneNote(2.0);
        const Events pressed = { { 0.0, On, 57, 0.8f }, { 0.5, Pressure, 57, 0.8f }, { 1.5, Pressure, 57, 0.2f }, { 2.0, Off, 57, 0.0f } };
        const Events moved = { { 0.0, On, 57, 0.8f }, { 0.4, Strip, 0, 1.0f }, { 1.2, Strip, 0, 0.35f }, { 2.0, Off, 57, 0.0f } };
        const int P2 = dest("P2"), T1 = dest("T1"), BMIX = dest("BMIX");
        struct Case { const char* name; Setup setUp; Events events; bool exact; };
        std::vector<Case> cases;
        for (auto& preset : factoryPresets())
            cases.push_back({ "preset", [preset](EngineBase& e) { for (auto& [id, value] : PresetManager::soundOf(preset)) e.setParameter(id, value); }, phrase(), true });
        cases.push_back({ "SLID, lanes of 3 5 7, noise step", [](EngineBase& e)
                          {
                              e.setParameter(W::LaneInterp, 1.0f); e.setParameter(W::PitchLaneLen, 3.0f); e.setParameter(W::ShapeLaneLen, 5.0f);
                              e.setParameter(W::LevelLaneLen, 7.0f); e.setParameter(W::StepShape + 2, 5.0f);
                          }, phrase(), true });
        cases.push_back({ "chain SER, BODY, filter with its envelope", [](EngineBase& e)
                          {
                              e.setParameter(W::EnvChainMode, 1.0f); e.setParameter(W::EnvChainStages, 3.0f);
                              e.setParameter(W::BodyBase + VoiceBody::Mix, 0.7f);
                              openFilter(e, 700.0f); e.setParameter(FB + VoiceFilterSection::EnvAmount, 0.6f);
                          }, phrase(), true });
        cases.push_back({ "loop ORND against the old RAND", [](EngineBase& e) { e.setParameter(W::LoopModeParam, 3.0f); }, phrase(), true });
        // (A voice struck again while it sounds: the restart and the new velocity are carried off as they were.)
        cases.push_back({ "a note struck again while it sounds", nullptr,
                          { { 0.0, On, 57, 0.9f }, { 0.31, On, 57, 0.3f }, { 0.52, On, 57, 1.0f }, { 1.5, Off, 57, 0.0f } }, true });
        cases.push_back({ "LFO1 > PTCH 30", [](EngineBase& e) { lfo(e, 0, V::LfoPitch, 0.3f); }, held, false });
        cases.push_back({ "LFO1 > AMP 60", [](EngineBase& e) { lfo(e, 0, V::LfoAmp, 0.6f); }, held, false });
        cases.push_back({ "LFO1 > PAN 80", [](EngineBase& e) { lfo(e, 0, V::LfoPan, 0.8f, 2.0f); }, held, false });
        cases.push_back({ "LFO1 > CUT 50", [](EngineBase& e) { openFilter(e); lfo(e, 0, V::LfoCutoff, 0.5f, 3.0f); }, held, false });
        cases.push_back({ "LFO1 > RATE 50", [](EngineBase& e) { lfo(e, 0, V::LfoRate, 0.5f, 1.0f); }, held, false });
        cases.push_back({ "LFO1 > P2 30", [=](EngineBase& e) { lfo(e, 0, P2, 0.3f, 2.0f); }, held, false });
        cases.push_back({ "LFO1 > T1 60", [=](EngineBase& e) { lfo(e, 0, T1, 0.6f, 1.0f); }, held, false });
        cases.push_back({ "LFO1 > BMIX 60 (body on)", [=](EngineBase& e) { e.setParameter(W::BodyBase + VoiceBody::Mix, 0.3f); lfo(e, 0, BMIX, 0.6f, 2.0f); }, held, false });
        cases.push_back({ "pad pressed (VOL)", nullptr, pressed, false });
        cases.push_back({ "strip moved (RATE +50)", nullptr, moved, false });
        cases.push_back({ "slot ENV > CUT 50", [](EngineBase& e) { openFilter(e, 300.0f); slot(e, 0, 3, V::LfoCutoff, 0.5f); }, phrase(), false });
        struct Run { double sr; int block; };
        for (const Run run : { Run { 44100.0, 256 }, Run { 96000.0, 256 }, Run { 48000.0, 37 }, Run { 44100.0, 1 } })
        {
            for (auto& c : cases)
            {
                if (run.block == 1 && c.exact)
                    continue;       // (the presets at odd blocks: testFactoryPresets; one sample a block is slow)
                const double seconds = run.block == 1 ? 1.0 : (c.events.size() > 4 && c.events.back().at > 3.0 ? 4.0 : 2.5);
                const juce::String name = juce::String::formatted("%.1f kHz / %d: ", run.sr / 1000.0, run.block) + c.name;
                const Compared r = against(name, c.setUp, c.events, seconds, run.block, nullptr,
                                           juce::String(c.name).startsWith("loop ORND") ? std::function<void(W&)>([](W& e) { e.setParameter(W::LoopModeParam, 5.0f); }) : nullptr,
                                           run.sr);
                if (c.exact)
                    expect(r.identical, name + ": " + verdict(r));
                else
                    expect(juce::String(verdict(r)) != "LOST", name + juce::String::formatted(": LOST (level %+.2f dB, over time %.2f dB, bright %+.1f %%)", r.level, r.curve, 100.0 * r.bright));
            }
        }
    }

    // ---- the source ENV at a note's first sample, CHAN on ---------------------------------------
    void testReviewChainEnvelopeAtTheNote()
    {
        beginTest("Review: with CHAN on, the source ENV starts a struck-again voice from nothing, as the chain does");
        // The chain begins every note at 0 (EnvelopeChain::noteOn resets its stages). A voice
        // struck again while it sounds must not read the level of the note before: ENV > PAN at
        // 100 would start the new note panned (the last note's sustain) for a sub-block.
        auto e = std::make_unique<W>();
        sequence(*e, 1, 0.25f);
        e->setParameter(W::AmpAttack, 0.05f);
        e->setParameter(W::EnvChainMode, 2.0f);         // PAR: both stages at once, held at the sustain
        slot(*e, 0, place("ENV"), V::LfoPan, 1.0f);
        std::vector<float> seen;
        const Events twice = { { 0.0, On, 60, 0.8f }, { 0.5, On, 60, 0.8f } };
        long struckAt = -1;
        const Stereo out = render(*e, twice, 0.6, 64, SR, nullptr, [&](long pos)
        {
            if (auto* v = sounding(*e))
                seen.push_back(v->getMod().source(mod::Src::Env));
            if (struckAt < 0 && (double) pos >= 0.5 * SR) struckAt = pos;
        });
        const size_t block = (size_t) (struckAt / 64);
        expect(seen[block - 1] > 0.9f, "the first note is held (" + juce::String(seen[block - 1], 3) + ")");
        // (seen[block + 1]: the source as the first block of the new note left it: 64 samples of a 50 ms attack.)
        expect(seen[block + 1] < 0.1f, "ENV 64 samples into the second note: " + juce::String(seen[block + 1], 3));
        // In the sound: the first sub-block of the second note is as far left as right (ENV 0 = centre).
        double l = 0.0, r = 0.0;
        for (long i = struckAt; i < struckAt + 16; ++i)
        {
            l += std::abs(out.l[(size_t) i]);
            r += std::abs(out.r[(size_t) i]);
        }
        if (printing())
            logMessage(juce::String::formatted("U4-WSEQ | review: chain, struck again: ENV %.3f before, %.3f after; first 16 samples left / right %.4f / %.4f",
                                               seen[block - 1], seen[block + 1], l, r));
        expect(l > 0.6 * r, juce::String::formatted("the new note begins in the centre (left %.4f, right %.4f over its first 16 samples)", l, r));
        // allNotesOff in mid-note: the next note's ENV starts from nothing too.
        e->allNotesOff();
        seen.clear();
        render(*e, { { 0.0, On, 62, 0.8f } }, 0.01, 16, SR, nullptr, [&](long) { if (auto* v = sounding(*e)) seen.push_back(v->getMod().source(mod::Src::Env)); });
        expect(seen.size() > 2 && seen[1] < 0.05f, "after allNotesOff: ENV one sub-block into the note " + juce::String(seen.size() > 1 ? seen[1] : -1.0f, 3));
    }

    // ---- the rows the report calls LOST: its explanations, measured ------------------------------
    // The steps a lane of the first voice stands at, sub-block by sub-block.
    template <typename Engine>
    static std::vector<int> laneSteps(Engine& e, int lane, const Events& events, double seconds)
    {
        std::vector<int> walk;
        render(e, events, seconds, 16, SR, nullptr, [&](long) { if (e.getVoice(0).isActive()) walk.push_back(e.getVoice(0).getLaneConfig(lane).currentStep); });
        return walk;
    }
    static std::vector<int> visited(const std::vector<int>& walk)
    {
        std::vector<int> v(walk);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
    static juce::String list(const std::vector<int>& v)
    {
        juce::String s;
        for (int x : v) s << x << " ";
        return s.trim();
    }

    void testReviewLostRows()
    {
        beginTest("Review: the LOST rows are what the report says they are");
        // 1. LFO1 > AMP 60 under a pad at 0.8 (VOL route: gain 1 + p). Old: (1 + p) x (1 + d x lfo)
        //    = 1.8 x (0.4 .. 1.6) = 0.72 .. 2.88, a swing of 4. New: 1 + p + d x lfo = 1.2 .. 2.4, a
        //    swing of 2. With the slot LFO1 > AMP 60 VIA PRES added: + 0.6 x 0.8 x lfo = 0.72 .. 2.88.
        const Events pressed = { { 0.0, On, 57, 0.8f }, { 0.2, Pressure, 57, 0.8f } };
        auto swing = [](const Stereo& s)
        {
            const auto m = mono(s);
            double low = 1.0e9, high = 0.0;
            for (size_t f = (size_t) (0.6 * SR); f + 480 <= (size_t) (1.6 * SR); f += 240)      // 5 Hz: five cycles
            {
                const double x = rms(m, f, f + 480);
                low = std::min(low, x);
                high = std::max(high, x);
            }
            return std::pair<double, double> { low, high };
        };
        auto setUp = [](EngineBase& e) { sequence(e, 1, 0.25f); lfo(e, 0, V::LfoAmp, 0.6f); };
        auto older = std::make_unique<O>();
        auto newer = std::make_unique<W>(), rebuilt = std::make_unique<W>(), bare = std::make_unique<W>();
        setUp(*older); setUp(*newer); setUp(*rebuilt);
        sequence(*bare, 1, 0.25f);
        slot(*rebuilt, 0, 1, V::LfoAmp, 0.6f, 6);
        const double unit = swing(render(*bare, { { 0.0, On, 57, 0.8f } }, 1.7)).second;
        const auto o = swing(render(*older, pressed, 1.7)), n = swing(render(*newer, pressed, 1.7)), b = swing(render(*rebuilt, pressed, 1.7));
        if (printing())
            logMessage(juce::String::formatted("U4-WSEQ | review: LFO > AMP 60, pad 0.8: gain low .. high: old %.2f .. %.2f, new %.2f .. %.2f, with the VIA PRES slot %.2f .. %.2f",
                                               o.first / unit, o.second / unit, n.first / unit, n.second / unit, b.first / unit, b.second / unit));
        // (g: the pad's gain, read off the new engine's sum as the middle of its swing.)
        const double g = 0.5 * (n.first + n.second) / unit;
        expect(g > 1.6 && g < 1.9, "the pad's gain at 0.8: " + juce::String(g, 3));
        expectWithinAbsoluteError((n.second - n.first) / unit, 1.2, 0.08, "new: a sum, the LFO's swing is +-0.6 whatever the pad does");
        expectWithinAbsoluteError(o.first / unit, g * 0.4, 0.05, "old: a product, the trough is the pad's gain x 0.4");
        expectWithinAbsoluteError(o.second / unit, g * 1.6, 0.08, "old: a product, the crest is the pad's gain x 1.6");
        expectWithinAbsoluteError(b.first / unit, o.first / unit, 0.05, "the VIA PRES slot gives the old trough");
        expectWithinAbsoluteError(b.second / unit, o.second / unit, 0.08, "the VIA PRES slot gives the old crest");

        // 2. The old random loop on 16 steps: each lane goes round four steps (after the step the
        //    note starts on). Its state steps once a lane, four times a step, and its lowest bit
        //    alternates: so a lane keeps the parity of its steps for as long as nothing else turns
        //    the state. Without noise the shape lane's four are S3 S7 S11 S15. A noise step does
        //    turn the state (the same number is the noise's): with noise on S1 S3 .. S15 the lane
        //    is thrown about while it stands on one of them, and once it lands on a step without
        //    noise it stays on those for good: the old patch plays noise for a moment, then never.
        const Events note = { { 0.0, On, 57, 0.8f } };
        auto old16 = std::make_unique<O>();
        randomPatch(*old16);
        const auto oldShape = visited(laneSteps(*old16, 2, note, 6.0)), oldPitch = visited(laneSteps(*old16, 1, note, 6.0 ));
        auto new16 = std::make_unique<W>();
        randomPatch(*new16);
        const auto newShape = visited(laneSteps(*new16, 2, note, 6.0));
        if (printing())
            logMessage("U4-WSEQ | review: old RAND, 16 steps: the shape lane visits " + list(oldShape) + "; the pitch lane " + list(oldPitch)
                       + "; new RAND's shape lane visits " + juce::String((int) newShape.size()) + " steps");
        expectEquals((int) oldShape.size(), 5, "old: the shape lane goes round four steps of sixteen (and the one it starts on)");
        expectEquals((int) oldPitch.size(), 5, "old: the pitch lane too");
        expectEquals((int) newShape.size(), 16, "new: every step is met");
        {
            auto oldNoisy = std::make_unique<O>();
            auto newNoisy = std::make_unique<W>();
            for (EngineBase* e : { (EngineBase*) oldNoisy.get(), (EngineBase*) newNoisy.get() })
            {
                randomPatch(*e);
                for (int s = 0; s < 16; s += 2)
                    e->setParameter(W::StepShape + s, 5.0f);
            }
            const auto was = laneSteps(*oldNoisy, 2, note, 6.0), is = laneSteps(*newNoisy, 2, note, 6.0);
            auto onNoise = [](const std::vector<int>& walk, size_t from)
            {
                size_t count = 0;
                for (size_t i = from; i < walk.size(); ++i)
                    count += walk[i] % 2 == 0;
                return (double) count / (double) std::max<size_t>(1, walk.size() - from);
            };
            size_t lastNoise = 0;
            for (size_t i = 0; i < was.size(); ++i)
                if (was[i] % 2 == 0) lastNoise = i;
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: noise on the even steps, the share of the time on a noise step: old %.3f (the last one %.3f s into the note), new %.3f",
                                                   onNoise(was, 0), (double) lastNoise * 16.0 / SR, onNoise(is, 0)));
            expect((double) lastNoise * 16.0 / SR < 1.0, "old: no noise step after the first second of the note");
            expectWithinAbsoluteError(onNoise(is, 0), 0.5, 0.06, "new: half the time on a noise step");
        }
        // 3. On the default sequence (8 steps): two steps a lane.
        auto old8 = std::make_unique<O>();
        old8->setParameter(O::LoopModeParam, 3.0f);
        old8->setParameter(O::TempoSync, 0.0f);
        std::vector<int> each;
        for (int lane = 0; lane < 4; ++lane)
        {
            auto walk = laneSteps(*old8, lane, note, 8.0);
            walk.erase(walk.begin(), walk.begin() + (long) (walk.size() / 8));     // (without the step the note starts on)
            each.push_back((int) visited(walk).size());
        }
        if (printing())
            logMessage("U4-WSEQ | review: old RAND, the default 8 steps: steps visited by the time, pitch, shape, level lane: " + list(each));
        for (int lane = 0; lane < 4; ++lane)
            expect(each[(size_t) lane] <= 2, "old: lane " + juce::String(lane) + " moves between at most two of its eight steps");
    }

    // ---- the new random loop, step by step -------------------------------------------------------
    void testReviewRandomWalk()
    {
        beginTest("Review: the new random loop counted step by step: even, no step favoured after another, each lane its own");
        auto e = std::make_unique<W>();
        randomPatch(*e);                // 16 steps of 15 ms
        const Events note = { { 0.0, On, 57, 0.8f } };
        std::array<std::vector<int>, 4> lane;
        // One reading a step: sub-blocks of 16 samples, the time lane's position cannot be read
        // (it may repeat), so a step is counted every 720 samples (15 ms), in its middle.
        render(*e, note, 40.0, 16, SR, nullptr, [&](long pos)
        {
            if (pos % 720 == 352)
                for (int l = 0; l < 4; ++l)
                    lane[(size_t) l].push_back(e->getVoice(0).getLaneConfig(l).currentStep);
        });
        const int steps = (int) lane[1].size();
        expect(steps > 2600, juce::String(steps) + " steps");
        for (int l = 0; l < 4; ++l)
        {
            std::array<int, 16> count {};
            int repeats = 0;
            for (size_t i = 0; i < lane[(size_t) l].size(); ++i)
            {
                ++count[(size_t) lane[(size_t) l][i]];
                repeats += i > 0 && lane[(size_t) l][i] == lane[(size_t) l][i - 1];
            }
            // Chi-square against even, 15 degrees of freedom: 37.7 is the 0.1 % point.
            double chi = 0.0;
            const double even = (double) steps / 16.0;
            for (int c : count)
                chi += ((double) c - even) * ((double) c - even) / even;
            const double repeatShare = (double) repeats / (double) (steps - 1);
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: new RAND, lane %d over %d steps: chi-square %.1f (15 d.f.), most / least %.2f / %.2f of even, the same step twice running %.3f (even: 0.0625)",
                                                   l, steps, chi, *std::max_element(count.begin(), count.end()) / even, *std::min_element(count.begin(), count.end()) / even, repeatShare));
            expect(chi < 37.7, "lane " + juce::String(l) + ": the sixteen steps are met evenly (chi-square " + juce::String(chi, 1) + ")");
            expect(repeatShare > 0.04 && repeatShare < 0.09, "lane " + juce::String(l) + ": a step follows itself as often as chance has it (" + juce::String(repeatShare, 3) + ")");
        }
        int together = 0;
        for (int i = 0; i < steps; ++i)
            together += lane[1][(size_t) i] == lane[2][(size_t) i];
        expect(together < steps / 8, "the pitch and the shape lane draw their own steps (" + juce::String(together) + " of " + juce::String(steps) + " alike)");
    }

    // ---- depths the CLOSE measure cannot see ------------------------------------------------------
    // (Level, loudness over time and brightness are the same for a vibrato half as deep, a pan half
    // as wide or a resonance moved half as far: the review's mutations R1 and R7 passed every
    // old-against-new row. These measure the thing itself, old against new.)
    // The lowest and the highest frequency of a steady tone, from its upward zero crossings.
    static std::pair<double, double> pitchSwing(const Stereo& s, double from, double to)
    {
        const auto m = mono(s);
        double low = 1.0e9, high = 0.0, last = -1.0;
        for (size_t i = (size_t) (from * SR); i + 1 < std::min(m.size(), (size_t) (to * SR)); ++i)
            if (m[i] <= 0.0f && m[i + 1] > 0.0f)
            {
                const double at = (double) i + (double) m[i] / ((double) m[i] - (double) m[i + 1]);
                if (last >= 0.0)
                {
                    const double hz = SR / (at - last);
                    low = std::min(low, hz);
                    high = std::max(high, hz);
                }
                last = at;
            }
        return { low, high };
    }

    void testReviewDepths()
    {
        beginTest("Review: how far an LFO moves the pitch, the pan and the resonance, old against new");
        const Events note = { { 0.0, On, 57, 0.8f } };      // 220 Hz
        for (float depth : { 0.3f, 1.0f, -0.6f })
        {
            auto older = std::make_unique<O>();
            auto newer = std::make_unique<W>();
            for (EngineBase* e : { (EngineBase*) older.get(), (EngineBase*) newer.get() })
            {
                sequence(*e, 1, 0.25f);
                lfo(*e, 0, V::LfoPitch, depth, 3.0f);
            }
            const auto was = pitchSwing(render(*older, note, 1.5), 0.3, 1.4), is = pitchSwing(render(*newer, note, 1.5), 0.3, 1.4);
            const double semisWas = 12.0 * std::log2(was.second / was.first), semisIs = 12.0 * std::log2(is.second / is.first);
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: LFO1 > PTCH %+.0f: the pitch swings over %.2f semitones (old %.2f): %.1f .. %.1f Hz (old %.1f .. %.1f)",
                                                   100.0 * depth, semisIs, semisWas, is.first, is.second, was.first, was.second));
            expectWithinAbsoluteError(semisWas, 24.0 * (double) depth * (double) depth, 0.25, "old: +-12 semitones x the depth squared");
            expectWithinAbsoluteError(semisIs, semisWas, 0.02 * semisWas + 0.02, "the vibrato is as deep as it was");
            expectWithinAbsoluteError(is.first, was.first, 0.005 * was.first, "its lowest pitch");
            expectWithinAbsoluteError(is.second, was.second, 0.005 * was.second, "its highest pitch");
        }
        // A lane's step moved by an LFO (the rows whose difference signal is as loud as the sound,
        // because the oscillator's phase runs apart: P2 -0.2 dB, T1 -6.8 dB): the step's own value
        // in the voice, old against new, at its two ends and on the way.
        {
            struct Cell { const char* label; int id; float depth; float range; };
            for (const Cell cell : { Cell { "P2", W::StepPitch + 1, 0.3f, 48.0f }, Cell { "L3", W::StepLevel + 2, -0.8f, 1.0f }, Cell { "T1", W::StepTime, 0.6f, 1.0f } })
            {
                auto older = std::make_unique<O>();
                auto newer = std::make_unique<W>();
                for (EngineBase* e : { (EngineBase*) older.get(), (EngineBase*) newer.get() })
                {
                    e->setParameter(W::TempoSync, 0.0f);
                    if (cell.id == W::StepLevel + 2) e->setParameter(cell.id, 0.5f);
                    lfo(*e, 0, dest(cell.label), cell.depth, 2.0f);
                }
                auto value = [&](const WaveSeqVoice::SeqStep& st) { return cell.id < W::StepShape ? st.pitchOffset : (cell.id < W::StepTime ? st.level : st.duration); };
                auto oldValue = [&](const OldWaveSeqVoice::SeqStep& st) { return cell.id < W::StepShape ? st.pitchOffset : (cell.id < W::StepTime ? st.level : st.duration); };
                std::vector<float> was, is;
                const int step = (cell.id - W::StepPitch) % 16;
                render(*older, note, 1.2, 16, SR, nullptr, [&](long) { was.push_back(oldValue(older->getVoice(0).getStep(step))); });
                render(*newer, note, 1.2, 16, SR, nullptr, [&](long) { is.push_back(value(newer->getVoice(0).getStep(step))); });
                float apart = 0.0f;
                for (size_t i = 600; i < std::min(was.size(), is.size()); ++i)        // (from 0.2 s)
                    apart = std::max(apart, std::abs(is[i] - was[i]));
                const auto [wasLow, wasHigh] = std::minmax_element(was.begin() + 600, was.end());
                const auto [isLow, isHigh] = std::minmax_element(is.begin() + 600, is.end());
                if (printing())
                    logMessage(juce::String::formatted("U4-WSEQ | review: LFO1 > %s %+.0f: the step goes %.4f .. %.4f (old %.4f .. %.4f), apart by %.4f at most",
                                                       cell.label, 100.0 * cell.depth, *isLow, *isHigh, *wasLow, *wasHigh, apart));
                expect(*wasHigh - *wasLow > 0.05f * cell.range, juce::String(cell.label) + ": the old LFO moves the step");
                expectWithinAbsoluteError(*isLow, *wasLow, 0.01f * (*wasHigh - *wasLow), juce::String(cell.label) + ": the step's lowest value");
                expectWithinAbsoluteError(*isHigh, *wasHigh, 0.01f * (*wasHigh - *wasLow), juce::String(cell.label) + ": the step's highest value");
                expect(apart < 0.03f * (*wasHigh - *wasLow), juce::String(cell.label) + ": the same value at the same time (" + juce::String(apart, 4) + ")");
            }
        }
        // The pan: the share of the left side at the LFO's two ends (a square LFO, 2 Hz).
        {
            auto older = std::make_unique<O>();
            auto newer = std::make_unique<W>();
            for (EngineBase* e : { (EngineBase*) older.get(), (EngineBase*) newer.get() })
            {
                sequence(*e, 1, 0.25f);
                lfo(*e, 0, V::LfoPan, 0.6f, 2.0f, 3);
            }
            const Stereo a = render(*older, note, 1.2), b = render(*newer, note, 1.2);
            for (double at : { 0.60, 0.85 })        // (the two halves of the LFO's second cycle)
            {
                const size_t from = (size_t) (at * SR), to = from + 4800;
                const double was = rms(a.l, from, to) / std::max(rms(a.r, from, to), 1.0e-9), is = rms(b.l, from, to) / std::max(rms(b.r, from, to), 1.0e-9);
                if (printing())
                    logMessage(juce::String::formatted("U4-WSEQ | review: LFO1 > PAN 60 (square) at %.2f s: left / right %.3f (old %.3f)", at, is, was));
                expectWithinAbsoluteError(is, was, 0.01 * std::max(1.0, was), "the pan is as wide as it was at " + juce::String(at, 2) + " s");
                expect(std::abs(was - 1.0) > 0.3, "(and it is off centre there)");
            }
        }
        // The resonance: what the filter is told at the LFO's two ends (square, 2 Hz, depth 80).
        {
            auto older = std::make_unique<O>();
            auto newer = std::make_unique<W>();
            for (EngineBase* e : { (EngineBase*) older.get(), (EngineBase*) newer.get() })
            {
                sequence(*e, 1, 0.25f, 1);
                openFilter(*e, 900.0f);
                lfo(*e, 0, V::LfoResonance, 0.8f, 2.0f, 3);
            }
            // (A saw through the low pass at 900 Hz: the resonance is the level of the band around the cutoff.)
            const auto a = mono(render(*older, note, 1.2)), b = mono(render(*newer, note, 1.2));
            double ends[2][2];
            int k = 0;
            for (double at : { 0.60, 0.85 })
            {
                ends[0][k] = std::sqrt(fastmeasure::goertzel(a, (size_t) (at * SR), 4800, 880.0, SR));
                ends[1][k] = std::sqrt(fastmeasure::goertzel(b, (size_t) (at * SR), 4800, 880.0, SR));
                ++k;
            }
            const double was = db(ends[0][0] / ends[0][1]), is = db(ends[1][0] / ends[1][1]);
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: LFO1 > RESO 80 (square): the fourth harmonic at the cutoff, one end against the other: %.2f dB (old %.2f dB)", is, was));
            expect(std::abs(was) > 3.0, "old: the resonance moves the band at the cutoff (" + juce::String(was, 2) + " dB)");
            expectWithinAbsoluteError(is, was, 0.3, "the resonance is moved as far as it was");
        }
    }

    // ---- the lanes as sources: what the first tests left open --------------------------------------
    void testReviewLanes()
    {
        beginTest("Review: LLAN glides with SLID; TLAN is the time lane's own step; a struck-again voice reads its lanes from the note's first sample");
        auto reading = [&](W& e, int published, double seconds, const std::vector<long>& at)
        {
            std::vector<float> seen;
            render(e, { { 0.0, On, 60, 0.8f } }, seconds, 64, SR, nullptr, [&](long pos)
            {
                if (std::find(at.begin(), at.end(), pos) != at.end())
                    if (auto* v = sounding(e))
                        seen.push_back(v->getMod().source(mod::Src::Published1 + published));
            });
            return seen;
        };
        {
            // SLID: the level lane is half way from 1 to 0.2 in the middle of the first step.
            auto e = std::make_unique<W>();
            sequence(*e, 2, 0.1f);
            e->setParameter(W::StepLevel + 1, 0.2f);
            e->setParameter(W::LaneInterp, 1.0f);
            slot(*e, 0, place("LLAN"), V::LfoPan, 0.5f);
            const auto seen = reading(*e, V::PubLevel, 0.1, { 2432 });      // 50.7 ms
            expectEquals((int) seen.size(), 1);
            if (seen.size() == 1)
                expectWithinAbsoluteError(seen[0], 0.6f, 0.03f, "LLAN half way through a step that glides from 1 to 0.2");
        }
        {
            // A time lane of two steps (0.1 s, 0.2 s) against a pitch lane of three: at 0.45 s the
            // time lane is on its second step (0.4 .. 0.6 s), the pitch lane on its first.
            auto e = std::make_unique<W>();
            sequence(*e, 4, 0.1f);
            e->setParameter(W::TimingLaneLen, 2.0f);
            e->setParameter(W::PitchLaneLen, 3.0f);
            e->setParameter(W::StepTime + 1, 0.2f);
            slot(*e, 0, place("TLAN"), V::LfoPan, 0.5f);
            const auto seen = reading(*e, V::PubTime, 0.5, { 21632 });      // 0.4507 s
            expectEquals((int) seen.size(), 1);
            if (seen.size() == 1)
                expectWithinAbsoluteError(seen[0], V::timeAsUnit(0.2f), 1.0e-3f, "TLAN follows the time lane, not the pitch lane");
        }
        {
            // A voice struck again in mid-sequence: the new note's first sample is on the lane's
            // first step, not on the step the last note stood at. LLAN > PAN at 100, levels 0, 1:
            // the first note is on step 2 (all the way right) when it is struck again; the new
            // note begins on step 1 (level 0: the centre).
            auto e = std::make_unique<W>();
            sequence(*e, 2, 0.1f);
            for (int s = 0; s < 2; ++s)
                e->setParameter(W::StepLevel + s, 1.0f);
            auto plain = std::make_unique<W>();
            sequence(*plain, 2, 0.1f);
            // (The pan reads the lane; the sound keeps its level: the lane's steps are moved by a
            // second slot's source instead of by the level itself.)
            e->setParameter(W::StepPitch + 0, 0.0f);
            e->setParameter(W::StepPitch + 1, 24.0f);
            e->setParameter(W::PitchRange, 0.0f);       // (the pitch lane is heard only as a source)
            slot(*e, 0, place("PLAN"), V::LfoPan, 1.0f);
            const Events twice = { { 0.0, On, 60, 0.8f }, { 0.15, On, 60, 0.8f } };
            const Stereo out = render(*e, twice, 0.3, 16);
            const size_t struck = (size_t) (0.15 * SR);
            double beforeL = 0.0, beforeR = 0.0, afterL = 0.0, afterR = 0.0;
            for (size_t i = 0; i < 16; ++i)
            {
                beforeL += std::abs(out.l[struck - 16 + i]); beforeR += std::abs(out.r[struck - 16 + i]);
                afterL += std::abs(out.l[struck + i]);       afterR += std::abs(out.r[struck + i]);
            }
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: struck again on step 2: left / right before %.4f / %.4f, the new note's first 16 samples %.4f / %.4f", beforeL, beforeR, afterL, afterR));
            expect(beforeL < 0.01 * beforeR, "the first note stands on step 2: all the way right");
            expect(afterL > 0.9 * afterR, "the new note's first sub-block is on step 1: the centre");
        }
    }

    // ---- a one-channel buffer; a cutoff route that goes ---------------------------------------------
    void testReviewMonoAndLetGo()
    {
        beginTest("Review: a one-channel buffer is the mean of the two sides, sub-block by sub-block; the cutoff is let go of when its route goes");
        {
            auto stereo = std::make_unique<W>(), single = std::make_unique<W>();
            for (auto* e : { stereo.get(), single.get() })
                lfo(*e, 0, V::LfoPan, 0.7f, 3.0f);
            const int block = 100;                  // (six sub-blocks and a rest of four)
            const Stereo both = render(*stereo, { { 0.0, On, 52, 0.8f } }, 0.5, block);
            single->prepareToPlay(SR, block);
            single->noteOn(52, 0.8f, -1);
            juce::AudioBuffer<float> buf(1, block);
            juce::MidiBuffer midi;
            bool alike = true;
            double energy = 0.0;
            for (size_t pos = 0; pos + (size_t) block <= both.l.size(); pos += (size_t) block)
            {
                buf.clear();
                single->processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                {
                    const float expected = 0.5f * (both.l[pos + (size_t) i] + both.r[pos + (size_t) i]);
                    alike = alike && buf.getSample(0, i) == expected;
                    energy += (double) expected * (double) expected;
                }
            }
            expect(energy > 1.0, "(it sounds)");
            expect(alike, "one channel: every sample is half the sum of the two sides");
        }
        {
            // The pad's CUT route under an LFO on the pan (so the voice stays on the routed path):
            // pressed, the filter opens; let go, it is where it was set, exactly.
            AftertouchRouter routes;
            routes.setRoute(0, { AftertouchRouter::Destination::FilterCutoff, 0.5f, false });
            auto e = std::make_unique<W>();
            sequence(*e, 1, 0.25f);
            openFilter(*e, 400.0f);
            lfo(*e, 0, V::LfoPan, 0.5f);
            e->setAftertouchRouter(&routes);
            std::vector<float> cut;
            render(*e, { { 0.0, On, 67, 0.8f }, { 0.1, Pressure, 67, 1.0f }, { 0.4, Pressure, 67, 0.0f } }, 1.0, 64, SR, nullptr,
                   [&](long) { if (auto* v = sounding(*e)) cut.push_back(v->getFilter().getCurrentCutoff()); });
            const float open = cut[(size_t) (0.35 * SR / 64)], after = cut.back();
            expectWithinAbsoluteError(std::log2(open / 400.0f), 2.0f, 0.05f, "pressed: two octaves up");
            expectEquals(after, 400.0f, "let go: the cutoff as set");
            // An LFO taken off the cutoff while another route stays: the same.
            auto f = std::make_unique<W>();
            sequence(*f, 1, 0.25f);
            openFilter(*f, 400.0f);
            lfo(*f, 0, V::LfoCutoff, 0.5f, 0.3f, 3);        // (a square: the cutoff stands away from 400 Hz)
            slot(*f, 0, place("ONE"), V::LfoPan, 0.3f);
            render(*f, { { 0.0, On, 67, 0.8f } }, 0.3, 64);
            if (auto* v = sounding(*f))
                expect(std::abs(std::log2(v->getFilter().getCurrentCutoff() / 400.0f)) > 0.5f, "the LFO holds the cutoff away");
            f->setParameter(R::lfoId(0, R::DestCell), 0.0f);
            render(*f, {}, 0.3, 64);
            if (auto* v = sounding(*f))
                expectEquals(v->getFilter().getCurrentCutoff(), 400.0f, "the LFO's DEST set to OFF: the cutoff as set");
            else
                expect(false, "the voice still sounds");
        }
        {
            // The strip reaches the router as smooth as it reached the old routes (the engine's own
            // smoothing first): the strip thrown up under a note with STRIP DST2 = AMP +100, old
            // against new, sample by sample around the throw.
            const Setup setUp = [](EngineBase& e)
            {
                sequence(e, 1, 0.25f);
                e.setParameter(R::stripAmountId(0), 0.0f);
                e.setParameter(R::stripDestId(1), (float) V::LfoAmp);
                e.setParameter(R::stripAmountId(1), 1.0f);
            };
            auto older = std::make_unique<O>();
            auto newer = std::make_unique<W>();
            setUp(*older);
            setUp(*newer);
            const Events thrown = { { 0.0, Strip, 0, 0.0f }, { 0.0, On, 57, 0.8f }, { 0.4, Strip, 0, 1.0f } };
            const Stereo a = render(*older, thrown, 0.8, 64), b = render(*newer, thrown, 0.8, 64);
            double apart = 0.0, peak = 0.0;
            for (size_t i = (size_t) (0.39 * SR); i < (size_t) (0.6 * SR); ++i)
            {
                apart = std::max(apart, (double) std::abs(b.l[i] - a.l[i]));
                peak = std::max(peak, (double) std::abs(a.l[i]));
            }
            const double before = rms(a.l, (size_t) (0.3 * SR), (size_t) (0.39 * SR)), after = rms(a.l, (size_t) (0.7 * SR), (size_t) (0.79 * SR));
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: the strip thrown up on AMP: old and new apart by %.4f of the peak at most; the level goes x %.2f", apart / peak, after / before));
            expect(after > 1.8 * before, "the strip doubles the level");
            expect(apart < 0.02 * peak, juce::String::formatted("the throw arrives as it did (apart by %.4f of the peak)", apart / peak));
        }
        {
            // A slot moves a filter cell from where the cell is SET, not from its default: ONE >
            // FATK +30 on an attack of 0.5 s and on one of 0.05 s is the same factor on each.
            float routed[2] = { 0.0f, 0.0f };
            const float set[2] = { 0.5f, 0.05f };
            for (int k = 0; k < 2; ++k)
            {
                auto e = std::make_unique<W>();
                sequence(*e, 1, 0.25f);
                openFilter(*e, 400.0f);
                e->setParameter(FB + VoiceFilterSection::Attack, set[k]);
                slot(*e, 0, place("ONE"), dest("FATK"), 0.3f);
                render(*e, { { 0.0, On, 60, 0.8f } }, 0.1, 64, SR, nullptr,
                       [&](long) { if (auto* v = sounding(*e)) routed[k] = v->getFilter().getEnvelope().getParams().attack; });
                expect(routed[k] > 1.5f * set[k], "ONE > FATK +30 lengthens the attack (" + juce::String(routed[k], 4) + " s from " + juce::String(set[k], 3) + " s)");
            }
            expectWithinAbsoluteError(routed[0] / set[0], routed[1] / set[1], 0.01f, "the same factor from either set value");
        }
        {
            // AMP below nothing: two audio-rate slots (XIN > AMP +100 twice) on an input of +-1 take
            // the gain from 3 to -1: the voice is turned over (ring modulation), not held at 0.
            const Input tone = [](long n) { return (float) std::sin(2.0 * 3.141592653589793 * 50.0 * (double) n / SR); };
            auto ring = std::make_unique<W>(), plain = std::make_unique<W>();
            for (auto* e : { ring.get(), plain.get() })
                sequence(*e, 1, 0.25f);
            slot(*ring, 0, place("XIN"), V::LfoAmp, 1.0f);
            slot(*ring, 1, place("XIN"), V::LfoAmp, 1.0f);
            const Stereo a = render(*ring, { { 0.0, On, 81, 0.8f } }, 0.5, 256, SR, &tone), b = render(*plain, { { 0.0, On, 81, 0.8f } }, 0.5, 256, SR, &tone);
            float lowest = 10.0f, highest = -10.0f;
            for (size_t i = 9600; i < a.l.size(); ++i)
                if (std::abs(b.l[i]) > 0.1f)
                {
                    lowest = std::min(lowest, a.l[i] / b.l[i]);
                    highest = std::max(highest, a.l[i] / b.l[i]);
                }
            if (printing())
                logMessage(juce::String::formatted("U4-WSEQ | review: XIN > AMP +100 twice, an input of +-1: the gain goes %.2f .. %.2f", lowest, highest));
            expectWithinAbsoluteError(highest, 3.0f, 0.15f, "the gain's top");
            expectWithinAbsoluteError(lowest, -1.0f, 0.15f, "the gain's bottom: below nothing");
        }
        // A voice that ended with the LFO's octaves on its filter, the route taken away between
        // the notes: the next note's cutoff is the cell's (nothing routed at all, and with
        // another route in use: the two render loops).
        for (bool otherRoute : { false, true })
        {
            auto e = std::make_unique<W>();
            sequence(*e, 1, 0.25f);
            openFilter(*e, 400.0f);
            lfo(*e, 0, V::LfoCutoff, 0.5f, 0.3f, 3);        // (a square: the cutoff stands away from 400 Hz)
            if (otherRoute)
                slot(*e, 0, place("ONE"), V::LfoPan, 0.3f);
            float held = 0.0f;
            render(*e, { { 0.0, On, 67, 0.8f } }, 0.2, 64, SR, nullptr, [&](long) { if (auto* v = sounding(*e)) held = v->getFilter().getCurrentCutoff(); });
            expect(std::abs(std::log2(held / 400.0f)) > 0.5f, "the LFO holds the cutoff away from the cell");
            e->allNotesOff();
            e->setParameter(R::lfoId(0, R::DestCell), 0.0f);
            float next = 0.0f;
            render(*e, { { 0.0, On, 67, 0.8f } }, 0.1, 64, SR, nullptr, [&](long) { if (auto* v = sounding(*e)) next = v->getFilter().getCurrentCutoff(); });
            expectEquals(next, 400.0f, otherRoute ? "the next note, another route in use: the cutoff as set" : "the next note, nothing routed: the cutoff as set");
        }
        {
            // PM reaches the step that is fading out as well: two steps alike, XFAD 100 (one sine
            // fading into its copy, all the time). The side bands are those of one sine: J1 / J0
            // = 0.0787 for an index of 0.157 radians, as in testPhaseModulation.
            const double carrier = dsp::midiNoteToHz(69), modulator = 110.0;
            const Input tone = [modulator](long n) { return 0.5f * (float) std::sin(2.0 * 3.141592653589793 * modulator * (double) n / SR); };
            auto e = std::make_unique<W>();
            sequence(*e, 2, 0.1f);
            e->setParameter(W::Crossfade, 1.0f);
            slot(*e, 0, place("XIN"), WaveSeqEngine::dests().phase, 0.05f);
            const auto out = mono(render(*e, { { 0.0, On, 69, 0.8f } }, 1.0, 256, SR, &tone));
            const double c = std::sqrt(fastmeasure::goertzel(out, 9600, 32768, carrier, SR));
            const double upper = std::sqrt(fastmeasure::goertzel(out, 9600, 32768, carrier + modulator, SR));
            expectWithinAbsoluteError(upper / c, 0.0787, 0.006, "PM under a crossfade: the side band of one sine");
        }
        {
            // A synced LFO follows the tempo as it did: LFO1 > AMP with SYNC on at 87 and at 140 BPM,
            // old against new by the difference signal (unsynced it is below -100 dB).
            for (float bpm : { 87.0f, 140.0f })
            {
                const Compared c = against("LFO1 > AMP 60, SYNC on, " + juce::String((int) bpm) + " BPM", [bpm](EngineBase& e)
                                           {
                                               sequence(e, 1, 0.25f);
                                               lfo(e, 0, V::LfoAmp, 0.6f);
                                               e.setParameter(R::lfoId(0, R::Sync), 1.0f);
                                               e.setTempo(bpm);
                                           }, oneNote(1.6), 2.0);
                expect(c.residual < -60.0, juce::String::formatted("the synced LFO at %.0f BPM is the old one (difference %.1f dB)", bpm, c.residual));
            }
            auto slow = std::make_unique<W>(), fast = std::make_unique<W>();
            for (auto* e : { slow.get(), fast.get() })
            {
                sequence(*e, 1, 0.25f);
                lfo(*e, 0, V::LfoAmp, 0.6f);
                e->setParameter(R::lfoId(0, R::Sync), 1.0f);
            }
            slow->setTempo(87.0f);
            fast->setTempo(140.0f);
            expect(! same(render(*slow, oneNote(1.6), 2.0), render(*fast, oneNote(1.6), 2.0)), "(and the tempo is heard)");
        }
        {
            // XFAD moved by a slot is the cell at that value: ONE > XFAD +50 on XFAD 30 is XFAD 80,
            // the same samples; and at +100 it is held to the cell's top.
            auto routed = std::make_unique<W>(), set = std::make_unique<W>(), over = std::make_unique<W>(), top = std::make_unique<W>();
            for (auto* e : { routed.get(), set.get(), over.get(), top.get() })
                e->setParameter(W::TempoSync, 0.0f);
            slot(*routed, 0, place("ONE"), V::LfoCrossfade, 0.5f);
            set->setParameter(W::Crossfade, 0.8f);
            slot(*over, 0, place("ONE"), V::LfoCrossfade, 1.0f);
            top->setParameter(W::Crossfade, 1.0f);
            const Events note = { { 0.0, On, 52, 0.8f } };
            const Stereo a = render(*routed, note, 1.5), b = render(*set, note, 1.5);
            // (The slot's value is there from the note's first sample; the level's lead of the
            // other path is not in it: compare after the first step.)
            auto alike = [](const Stereo& x, const Stereo& y)
            {
                double worst = 0.0;
                for (size_t i = 0; i < x.l.size(); ++i)
                    worst = std::max(worst, (double) std::abs(x.l[i] - y.l[i]));
                return worst;
            };
            expect(alike(a, b) < 1.0e-5, "ONE > XFAD +50 on 30 is XFAD 80 (apart by " + juce::String(alike(a, b), 7) + ")");
            const Stereo c = render(*over, note, 1.5), d = render(*top, note, 1.5);
            expect(alike(c, d) < 1.0e-5, "ONE > XFAD +100 is held to XFAD 100 (apart by " + juce::String(alike(c, d), 7) + ")");
            expect(alike(a, d) > 1.0e-3, "(and 80 is another sound than 100)");
        }
        // What a free slot's DEST lists after the table (an index stored in projects).
        if (printing())
        {
            O old;
            W now;
            juce::String was, is;
            for (auto& page : old.getParamPages())
                for (auto& p : page.params)
                    if (p.id == ModSlotParam::id(0, ModSlotParam::DEST))
                        for (int i = 83; i < p.numChoices; ++i) was << i << " " << p.choices[i] << "  ";
            for (auto& page : now.getParamPages())
                for (auto& p : page.params)
                    if (p.id == ModSlotParam::id(0, ModSlotParam::DEST))
                        for (int i = 83; i < p.numChoices; ++i) is << i << " " << p.choices[i] << "  ";
            logMessage("U4-WSEQ | review: a free slot's DEST from 83 on, old: " + was);
            logMessage("U4-WSEQ | review: a free slot's DEST from 83 on, new: " + is);
        }
    }

    // ---- cost --------------------------------------------------------------------------------------
    template <typename Engine>
    static double measure(const std::function<void(EngineBase&)>& setUp, bool input, float strip, bool pressure)
    {
        constexpr int BLOCK = 512, BLOCKS = 940;        // 10 s
        Engine e(8);
        e.prepareToPlay(SR, BLOCK);
        if (setUp) setUp(e);
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
                AudioInput ai;
                ai.left = ai.right = in.data();
                ai.frames = BLOCK;
                ai.envelope = 0.5f;
                e.setAudioInput(ai);
            }
            buf.clear();
            e.processBlock(buf, midi);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return 100.0 * seconds / (BLOCKS * BLOCK / SR);
    }

    // Old and new in one binary, run alternately, the best of five each.
    void bench()
    {
        beginTest("Cost (NEPTUNE_U4_WSEQ_BENCH)");
        struct Case { const char* name; Setup setUp; bool input; float strip; bool pressure; bool oldToo; };
        const int P2 = dest("P2"), PLAN = place("PLAN"), LLAN = place("LLAN"), XIN = place("XIN");
        const std::vector<Case> cases = {
            { "idle (nothing routed)", nullptr, false, 0.0f, false, true },
            { "idle, SLID on (the pitch moves every sample)", [](EngineBase& e) { e.setParameter(W::LaneInterp, 1.0f); }, false, 0.0f, false, true },
            { "idle, filter LP 900", [](EngineBase& e) { openFilter(e); }, false, 0.0f, false, true },
            { "idle, BODY on", [](EngineBase& e) { e.setParameter(W::BodyBase + VoiceBody::Mix, 0.5f); }, false, 0.0f, false, true },
            { "input connected, nothing routed", nullptr, true, 0.0f, false, true },
            { "pads pressed, default route (VOL)", nullptr, false, 0.0f, true, true },
            { "LFO1 > PTCH", [](EngineBase& e) { lfo(e, 0, V::LfoPitch, 0.3f); }, false, 0.0f, false, true },
            { "LFO1 > CUT, LFO2 > P2 (a page cell)", [=](EngineBase& e) { openFilter(e); lfo(e, 0, V::LfoCutoff, 0.4f, 0.7f); lfo(e, 1, P2, 0.3f); }, false, 0.0f, false, true },
            { "strip up, default route (RATE)", nullptr, false, 1.0f, false, true },
            { "four slots (ENV, PRES, LFO via STRP, RAND)", [](EngineBase& e)
              {
                  openFilter(e);
                  slot(e, 0, 3, V::LfoAmp, 0.5f);
                  slot(e, 1, 6, V::LfoCutoff, 0.4f);
                  slot(e, 2, 1, V::LfoCrossfade, 0.6f, 7);
                  slot(e, 3, 9, V::LfoPan, 0.8f);
              }, false, 0.8f, true, true },
            { "new: PLAN > CUT, LLAN > PAN (the lanes)", [=](EngineBase& e) { openFilter(e); slot(e, 0, PLAN, V::LfoCutoff, 0.5f); slot(e, 1, LLAN, V::LfoPan, 0.5f); }, false, 0.0f, false, false },
            { "new: XIN > PM (audio rate)", [=](EngineBase& e) { slot(e, 0, XIN, WaveSeqEngine::dests().phase, 0.3f); }, true, 0.0f, false, false },
            { "new: ONE > ETIM", [](EngineBase& e) { slot(e, 0, 16, WaveSeqEngine::dests().envTime, 0.3f); }, false, 0.0f, false, false },
        };
        std::printf("Wave sequencer, 8 voices, 512-sample blocks, 48 kHz: %% of one core (best of 5, old and new alternately)\n");
        // (NEPTUNE_U4_WSEQ_BENCH=<part of a case's name> times only those.)
        const juce::String only = juce::SystemStats::getEnvironmentVariable("NEPTUNE_U4_WSEQ_BENCH", "");
        for (auto& c : cases)
        {
            if (only != "1" && ! juce::String(c.name).contains(only))
                continue;
            double older = 1.0e9, newer = 1.0e9;
            for (int run = 0; run < 5; ++run)
            {
                if (c.oldToo)
                    older = std::min(older, measure<O>(c.setUp, c.input, c.strip, c.pressure));
                newer = std::min(newer, measure<W>(c.setUp, c.input, c.strip, c.pressure));
            }
            if (c.oldToo)
                std::printf("  %-46s old %6.3f %%   new %6.3f %%   (%+5.1f %%)\n", c.name, older, newer, 100.0 * (newer / older - 1.0));
            else
                std::printf("  %-46s old    -     new %6.3f %%\n", c.name, newer);
        }
        std::fflush(stdout);
        expect(true);
    }
};

static U4WaveSeqTests u4WaveSeqTests;
