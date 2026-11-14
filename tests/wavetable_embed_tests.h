#pragma once
#include <numeric>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../src/dsp/WaveTables.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/UnifiedVoiceEngine.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/Preset.h"
#include "../src/project/ProjectBridge.h"
#include "../src/sequencer/SequencerCore.h"
#include "mod_wheel_tests.h"
#include <cmath>
#include <map>
#include <set>
#include <vector>

// -----------------------------------------------------------------------------
// The wavetable engine embedded in the unified voice's oscillator slots, and the
// factory presets for the newer tables.
//
// A slot set to WT reads the wavetable engine's built-in tables (WavetableFactory::
// builtInTables(): the nine it always had, then the newer fourteen) through the
// wavetable engine's read-out (WavetableRead), which the wavetable engine's own
// voices call too. Measured here:
//   - the slot and the wavetable engine give the same waveform for the same table
//     and position (harmonic by harmonic), and point at the same table objects;
//   - table numbers 0-8 in a slot sound as they did before the newer tables were
//     offered (PINNED: measured on the code before the change, and the old read-out
//     kept here as a reference the shared one must equal sample for sample);
//   - every newer table in a slot is audible, finite, free of DC and its own sound;
//   - stack, detune and cross-mod act on a newer table; a slot using one is saved
//     with a project and a preset;
//   - an LFO sent to a slot's table cell still moves it by eight tables at full depth.
// Renders are the note A3 (220 Hz) at 48 kHz, whose cycles fit the 4,800-sample
// window exactly, so a harmonic's level is read without leakage.
// -----------------------------------------------------------------------------
class WavetableEmbedTests : public juce::UnitTest
{
public:
    WavetableEmbedTests() : UnitTest("Wavetable Embed") {}

    using U = UnifiedVoiceEngine;
    using WE = WavetableEngine;
    using MW = ModWheelTests;

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;
    static constexpr int WINDOW = 4800;
    static constexpr int WT = (int) UnifiedVoice::OscWaveform::Wavetable;
    static constexpr int OLD = WavetableFactory::NUM_FIRST_TABLES;      // 9
    static constexpr int HARMONICS = 24;                                // compared: up to 5.3 kHz at A3

    static int total() { return WavetableFactory::builtInCount(); }
    static constexpr int FILTER_OFF = FilterSlot::NUM_MODELS - 1;      // TYPE = OFF, on both engines

    // ---- rendering ----------------------------------------------------------------------------

    static void quiet(U& e)
    {
        e.setParameter(UnifiedParam::DRIFT, 0.0f);
        for (int v = 0; v < U::MAX_VOICES; ++v)
            e.getVoice(v).getAnalogCharacter().enabled = false;
        e.setParameter(UnifiedParam::CUTOFF, 20000.0f);
        e.setParameter(UnifiedParam::MIX, 0.0f);
        e.setParameter(UnifiedParam::SUB, 0.0f);
    }

    static std::vector<float> play(EngineBase& e, int note, int from, int length = WINDOW)
    {
        e.noteOn(note, 1.0f, -1);
        juce::MidiBuffer midi;
        std::vector<float> out;
        juce::AudioBuffer<float> buf(2, BLOCK);
        while ((int) out.size() < from + length)
        {
            buf.clear();
            e.processBlock(buf, midi);
            out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + BLOCK);
        }
        return std::vector<float>(out.begin() + from, out.begin() + from + length);
    }

    // Oscillator 1 alone playing built-in table `table` at `position`.
    static std::vector<float> renderSlot(int table, float position, int note, int from = 9600,
                                         const std::function<void (U&)>& more = {})
    {
        U e(1);
        e.prepareToPlay(SR, BLOCK);
        quiet(e);
        e.setParameter(UnifiedParam::WAVE1, (float) WT);
        e.setParameter(UnifiedParam::SRC_A1, position);
        e.setParameter(UnifiedParam::SRC_B1, (float) table);
        if (more)
            more(e);
        return play(e, note, from);
    }

    // The wavetable engine playing the same built-in table (TABL numbers USER and KEYS in between).
    static int engineTable(int builtIn) { return builtIn < OLD ? builtIn : builtIn + 2; }
    static std::vector<float> renderEngine(int table, float position, int note, int from = 9600, bool filtered = true)
    {
        WE e(1);
        e.prepareToPlay(SR, BLOCK);
        e.setParameter(WE::TableSelect, (float) engineTable(table));
        e.setParameter(WE::Position, position);
        e.setParameter(WE::AmpSustain, 1.0f);
        e.setParameter(EngineLfoSection::stripAmountId(0), 0.0f);
        if (! filtered)
            e.setParameter(WE::FilterBase + VoiceFilterSection::Type, (float) FILTER_OFF);
        return play(e, note, from);
    }

    // ---- measuring ----------------------------------------------------------------------------

    static double harmonic(const std::vector<float>& x, double hz, int h)
    {
        double c = 0.0, s = 0.0;
        for (size_t n = 0; n < x.size(); ++n)
        {
            const double a = 2.0 * 3.14159265358979323846 * hz * (double) h * (double) n / SR;
            c += x[n] * std::cos(a);
            s += x[n] * std::sin(a);
        }
        return 2.0 * std::sqrt(c * c + s * s) / (double) x.size();
    }
    static double rms(const std::vector<float>& x)
    {
        double sq = 0.0;
        for (float v : x) sq += (double) v * v;
        return std::sqrt(sq / (double) std::max<size_t>(1, x.size()));
    }
    static double mean(const std::vector<float>& x)
    {
        double sum = 0.0;
        for (float v : x) sum += v;
        return sum / (double) std::max<size_t>(1, x.size());
    }
    static bool finite(const std::vector<float>& x)
    {
        for (float v : x)
            if (! std::isfinite(v))
                return false;
        return true;
    }
    // The levels of harmonics 1 .. HARMONICS of A3, as shares of their sum of squares: the shape
    // of the wave whatever its level.
    static std::vector<double> shape(const std::vector<float>& x, double hz = 220.0)
    {
        std::vector<double> h((size_t) HARMONICS);
        double sq = 0.0;
        for (int k = 0; k < HARMONICS; ++k)
        {
            h[(size_t) k] = harmonic(x, hz, k + 1);
            sq += h[(size_t) k] * h[(size_t) k];
        }
        const double norm = std::sqrt(std::max(sq, 1.0e-30));
        for (auto& v : h)
            v /= norm;
        return h;
    }
    static double shapeDistance(const std::vector<double>& a, const std::vector<double>& b)
    {
        double d = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            d = std::max(d, std::abs(a[i] - b[i]));
        return d;
    }
    // The part of x that is not y, as a share of x's power.
    static double changed(const std::vector<float>& x, const std::vector<float>& y)
    {
        double diff = 0.0, power = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            diff += ((double) x[i] - y[i]) * ((double) x[i] - y[i]);
            power += (double) x[i] * x[i];
        }
        return diff / std::max(power, 1.0e-30);
    }

    // The slot's read-out as it was before it was shared (UnifiedVoice::generateOsc, v0.9), word for word.
    static float readAsBefore(const Wavetable& table, float a, double phase, double dt)
    {
        const float p = (float) phase;
        int level = 0;
        for (double ratio = dt * 2.0 * Wavetable::kFrameSize; ratio > 1.0 && level < table.getMipLevelCount() - 1; ratio *= 0.5)
            ++level;
        const auto& frames = table.frames[(size_t) level];
        const int count = (int) frames.size();
        if (count == 0) return 0.0f;
        const float framePos = std::clamp(a, 0.0f, 1.0f) * (float) (count - 1);
        const int frameA = std::min(count - 1, (int) framePos), frameB = std::min(count - 1, frameA + 1);
        const float samplePos = p * (float) Wavetable::kFrameSize;
        const int sA = ((int) samplePos) % Wavetable::kFrameSize, sB = (sA + 1) % Wavetable::kFrameSize;
        const float sFrac = samplePos - std::floor(samplePos);
        const float vA = dsp::lerp(frames[(size_t) frameA][(size_t) sA], frames[(size_t) frameA][(size_t) sB], sFrac);
        const float vB = dsp::lerp(frames[(size_t) frameB][(size_t) sA], frames[(size_t) frameB][(size_t) sB], sFrac);
        return dsp::lerp(vA, vB, framePos - (float) frameA);
    }

    // ---- the factory presets for the newer tables ----------------------------------------------
    struct Expected { const char* name; const char* table; const char* category; bool classic; };
    static const std::vector<Expected>& expectedPresets()
    {
        static const std::vector<Expected> list = {
            { "Vowel Lead", "VOWL", "LEAD", false },      { "Vowel Choir", "VOWL", "PAD", false },
            { "Sync Sweep Lead", "SYNC", "LEAD", false }, { "Fold Bass", "FOLD", "BASS", false },
            { "FM Table Keys", "FM1", "KEYS", false },    { "Glass Pluck", "FM3", "KEYS", false },
            { "Struck Bar", "BELL", "KEYS", false },      { "Drawbar Organ", "ORGN", "KEYS", false },
            { "Noise To Tone", "NOIS", "FX", false },     { "Reso Sweep Bass", "RESO", "BASS", false },
            { "Reso Sweep Pad", "RESO", "PAD", false },   { "Walsh Stepper", "WLSH", "LEAD", false },
            { "Prime Shimmer", "PRIM", "PAD", false },    { "Octave Climb", "OCTV", "PAD", false },
            { "Phase Bass", "PDST", "BASS", false },      { "Comb Phase Pad", "COMB", "PAD", false },
            { "Eight Bit Vowels", "VOWL", "LEAD", true }, { "Glassy Bell", "BELL", "KEYS", true },
            { "Raw Sync Lead", "SYNC", "LEAD", true } };
        return list;
    }
    // How loud a preset is: the loudest 100 ms of the "Mod Wheel" render (two held notes, strip at rest), dB.
    static float loudest(const std::vector<float>& x)
    {
        const size_t win = (size_t) (0.1 * SR);
        double best = 0.0;
        for (size_t from = 0; from + win <= x.size(); from += win / 2)
        {
            double sq = 0.0;
            for (size_t i = from; i < from + win; ++i)
                sq += (double) x[i] * x[i];
            best = std::max(best, sq / (double) win);
        }
        return MW::dB((float) std::sqrt(best));
    }

    void runTest() override
    {
        beginTest("One set of tables: the slots, the wavetable engine and the built-in set are the same objects");
        {
            const auto& builtIn = WavetableFactory::builtInTables();
            const auto& slots = UnifiedVoice::sharedTables();
            expectEquals((int) builtIn.size(), OLD + (int) wavetables::definitions().size());
            expectEquals((int) builtIn.size(), 23, "nine and fourteen");
            expectEquals(total(), (int) builtIn.size());
            expect(&slots == &builtIn, "the slots read the built-in set itself");
            WE engine(1);
            for (int i = 0; i < (int) builtIn.size(); ++i)
            {
                expect(&engine.getTable(engineTable(i)) == builtIn[(size_t) i], "table " + juce::String(i) + " is the wavetable engine's");
                expectEquals(juce::String(WE::tableLabels()[engineTable(i)]), juce::String(WavetableFactory::builtInLabels()[i]));
            }
            for (int i = 0; i < OLD; ++i)
                expect(builtIn[(size_t) i] == &WavetableFactory::sharedFactoryTables()[(size_t) i]);
            for (int i = OLD; i < (int) builtIn.size(); ++i)
                expect(builtIn[(size_t) i] == &WavetableFactory::sharedMoreTables()[(size_t) (i - OLD)]);
        }

        beginTest("The table cell of each slot: the nine old names where they were, then the newer fourteen");
        {
            static const char* const old[] = { "SAW", "SQR", "TRI", "SIN", "PWM", "FORM", "HARM", "SPEC", "DIGI" };
            static const char* const newer[] = { "VOWL", "SYNC", "FOLD", "FM1", "FM3", "BELL", "ORGN", "NOIS", "RESO",
                                                 "WLSH", "PRIM", "OCTV", "PDST", "COMB" };
            for (int osc = 0; osc < 3; ++osc)
            {
                const ParamSpec spec = U::srcSpec(osc, WT, true);
                expect(spec.isValid() && spec.kind == ParamKind::Choice);
                expectEquals(spec.id, UnifiedParam::srcB(osc), "the cell that was there: no new id");
                expectEquals(spec.numChoices, 23);
                expectEquals(spec.minVal, 0.0f);
                expectEquals(spec.maxVal, 22.0f);
                expectEquals(spec.defaultVal, 0.0f, "a slot turned to WT still starts on SAW");
                for (int i = 0; i < 9; ++i)
                    expectEquals(juce::String(spec.format((float) i)), juce::String(old[i]), "stored index " + juce::String(i));
                for (int i = 0; i < 14; ++i)
                    expectEquals(juce::String(spec.format((float) (9 + i))), juce::String(newer[i]));
                expectEquals(spec.turned(8.0f, 1, TurnStep::Plain), 9.0f, "one click past DIGI is VOWL");
                expectEquals(spec.turned(22.0f, 1, TurnStep::Plain), 22.0f, "the list ends at COMB");
            }
            // The cell is applied and read back, on the page the screen shows for a WT slot.
            U e(1);
            e.setParameter(UnifiedParam::WAVE2, (float) WT);
            e.setParameter(UnifiedParam::SRC_B2, 14.0f);
            expectEquals(e.getParameter(UnifiedParam::SRC_B2), 14.0f);
            bool onPage = false;
            for (auto& page : e.getParamPages())
                for (auto& p : page.params)
                    if (p.isValid() && p.id == UnifiedParam::SRC_B2)
                    {
                        onPage = true;
                        expectEquals(juce::String(p.format(e.getParameter(p.id))), juce::String("BELL"));
                        expectEquals(p.numChoices, 23);
                    }
            expect(onPage, "2TBL is on a page");
        }

        beginTest("Old table numbers 0-8 in a slot sound as they did (pinned before the change, v0.9 code path)");
        {
            // Measured with the slot's own read-out and its own nine tables, before the newer tables
            // were offered (2026-11-11, Debug, 48 kHz): position 0.37; RMS at A3 and at A6, then
            // the levels of harmonics 1-8 of A3.
            struct Pin { float rmsA3, rmsA6; float h[8]; };
            static const Pin pins[9] = {
                { 0.30491f, 0.28701f, { 0.33942f, 0.16969f, 0.11310f, 0.08480f, 0.06782f, 0.05649f, 0.04839f, 0.04232f } },
                { 0.52037f, 0.49773f, { 0.66664f, 0.00002f, 0.22214f, 0.00002f, 0.13319f, 0.00002f, 0.09504f, 0.00002f } },
                { 0.35427f, 0.35408f, { 0.49703f, 0.00037f, 0.05521f, 0.00000f, 0.01986f, 0.00004f, 0.01012f, 0.00000f } },
                { 0.43022f, 0.43045f, { 0.60798f, 0.00039f, 0.00014f, 0.00008f, 0.00005f, 0.00003f, 0.00002f, 0.00002f } },
                { 0.58485f, 0.56642f, { 0.72320f, 0.25792f, 0.11750f, 0.19088f, 0.03881f, 0.10294f, 0.09006f, 0.02030f } },
                { 0.13752f, 0.00103f, { 0.00012f, 0.00021f, 0.00053f, 0.00134f, 0.00315f, 0.00671f, 0.01294f, 0.02258f } },
                { 0.11685f, 0.04822f, { 0.03393f, 0.03392f, 0.03392f, 0.03391f, 0.03389f, 0.03388f, 0.03386f, 0.03384f } },
                { 0.10453f, 0.00007f, { 0.00005f, 0.00006f, 0.00007f, 0.00011f, 0.00018f, 0.00032f, 0.00056f, 0.00095f } },
                { 0.43055f, 0.43078f, { 0.60845f, 0.00039f, 0.00070f, 0.00008f, 0.00046f, 0.00003f, 0.00070f, 0.00002f } } };
            constexpr float TOLERANCE = 2.0e-5f;        // the pins are printed to five places
            double worst = 0.0;
            for (int t = 0; t < 9; ++t)
            {
                const auto x = renderSlot(t, 0.37f, 57);
                const juce::String name = "table " + juce::String(t);
                worst = std::max(worst, std::abs(rms(x) - (double) pins[t].rmsA3));
                expectWithinAbsoluteError((float) rms(x), pins[t].rmsA3, TOLERANCE, name + ": level at A3");
                const float high = (float) rms(renderSlot(t, 0.37f, 93));
                worst = std::max(worst, (double) std::abs(high - pins[t].rmsA6));
                expectWithinAbsoluteError(high, pins[t].rmsA6, TOLERANCE, name + ": level at A6 (a band-limited copy)");
                for (int h = 1; h <= 8; ++h)
                {
                    const float level = (float) harmonic(x, 220.0, h);
                    worst = std::max(worst, (double) std::abs(level - pins[t].h[h - 1]));
                    expectWithinAbsoluteError(level, pins[t].h[h - 1], TOLERANCE, name + ": harmonic " + juce::String(h));
                }
            }
            logMessage("tables 0-8 in a slot v. the pins from before: largest difference " + juce::String(worst, 7)
                       + " (tolerance " + juce::String(TOLERANCE, 5) + ")");
        }

        beginTest("The shared read-out gives the old slot read-out's samples exactly, on every old table");
        {
            const auto& builtIn = WavetableFactory::builtInTables();
            int compared = 0, different = 0;
            for (int t = 0; t < OLD; ++t)
                for (double hz : { 27.5, 110.0, 261.6256, 880.0, 3520.0, 9000.0 })
                    for (float position : { 0.0f, 0.004f, 0.37f, 0.5f, 0.999f, 1.0f })
                        for (int n = 0; n < 257; ++n)
                        {
                            const double phase = std::fmod((double) n * 0.00389105058 + 0.0001, 1.0), dt = hz / SR;
                            const Wavetable& table = *builtIn[(size_t) t];
                            const float now = WavetableRead::sample(table, WavetableRead::mipLevel(table, dt), position, phase);
                            ++compared;
                            if (now != readAsBefore(table, position, phase, dt))
                                ++different;
                        }
            expectEquals(different, 0, "of " + juce::String(compared) + " samples");
            logMessage(juce::String(compared) + " samples of tables 0-8 compared with the read-out as it was: " + juce::String(different) + " differ");
        }

        beginTest("Every newer table in a slot: audible, finite, no DC, and a sound of its own");
        {
            std::vector<std::vector<double>> shapes;
            for (int t = 0; t < total(); ++t)
            {
                const juce::String name = WavetableFactory::builtInLabels()[t];
                const auto x = renderSlot(t, 0.5f, 57);
                shapes.push_back(shape(x));
                if (t < OLD)
                    continue;
                expect(finite(x), name);
                expect(rms(x) > 0.05, name + ": level " + juce::String(rms(x), 4));
                expect(std::abs(mean(x)) < 0.002, name + ": DC " + juce::String(mean(x), 5));
                // And along the table: both ends sound, and are not the same wave.
                const auto first = renderSlot(t, 0.0f, 57), last = renderSlot(t, 1.0f, 57);
                expect(finite(first) && finite(last) && rms(first) > 0.05 && rms(last) > 0.05, name + ": the ends of the table");
                expect(shapeDistance(shape(first), shape(last)) > 0.05, name + ": POS does nothing");
            }
            double nearest = 1.0;
            juce::String pair;
            for (int a = OLD; a < total(); ++a)
                for (int b = 0; b < a; ++b)
                {
                    const double d = shapeDistance(shapes[(size_t) a], shapes[(size_t) b]);
                    if (d < nearest)
                    {
                        nearest = d;
                        pair = juce::String(WavetableFactory::builtInLabels()[a]) + " / " + WavetableFactory::builtInLabels()[b];
                    }
                    expect(d > 0.03, juce::String(WavetableFactory::builtInLabels()[a]) + " sounds like " + WavetableFactory::builtInLabels()[b]);
                }
            logMessage("the two nearest tables at POS 50: " + pair + ", largest harmonic difference " + juce::String(nearest, 3));
        }

        beginTest("One read-out: a slot and the wavetable engine give the same wave for the same table and position");
        {
            // Harmonics 1-24 as shares of the wave (its level left out: the two engines have their
            // own gain stages), every built-in table at three positions; at A3, and at A5 where a
            // band-limited copy is read (harmonics up to 21 kHz). Both engines' filters are set
            // to OFF: what is compared is the oscillator.
            constexpr double TOLERANCE = 0.001;
            expectEquals(juce::String(FilterSlot::modelNames()[FILTER_OFF]), juce::String("OFF"));
            double worst = 0.0;
            juce::String where;
            for (int t = 0; t < total(); ++t)
                for (float position : { 0.0f, 0.37f, 1.0f })
                    for (int note : { 57, 81 })
                    {
                        const double hz = note == 57 ? 220.0 : 880.0;
                        const auto slot = renderSlot(t, position, note, 72000, [](U& e) { e.setParameter(UnifiedParam::MODEL_A, (float) FILTER_OFF); });
                        const auto engine = renderEngine(t, position, note, 72000, false);
                        const juce::String name = juce::String(WavetableFactory::builtInLabels()[t]) + " at " + juce::String(position, 2)
                                                  + (note == 57 ? ", A3" : ", A5");
                        if (rms(slot) < 1.0e-3 && rms(engine) < 1.0e-3)
                            continue;       // nothing left of the frame at this pitch, in either
                        const double d = shapeDistance(shape(slot, hz), shape(engine, hz));
                        if (d > worst)
                        {
                            worst = d;
                            where = name;
                        }
                        expect(d < TOLERANCE, name + ": harmonics differ by " + juce::String(d, 5));
                    }
            logMessage("slot v. wavetable engine, 23 tables x 3 positions x 2 pitches: largest harmonic difference "
                       + juce::String(worst, 6) + " (" + where + "), tolerance " + juce::String(TOLERANCE, 3));
        }

        beginTest("Stack, detune and cross-mod act on a newer table as on any oscillator kind");
        {
            const int bell = OLD + 5, vowel = OLD;
            const auto plain = renderSlot(bell, 0.6f, 57);

            const auto stacked = renderSlot(bell, 0.6f, 57, 9600, [](U& e)
            {
                e.setParameter(UnifiedParam::STACK, 3.0f);
                e.setParameter(UnifiedParam::SPREAD, 0.6f);
            });
            expect(finite(stacked) && rms(stacked) > 0.02, "the stack of eight");
            expect(changed(plain, stacked) > 0.05, "STCK changes nothing: " + juce::String(changed(plain, stacked), 4));

            // Two slots on two newer tables, the second 25 cents away: they beat, so the level of
            // the first harmonic differs between two halves of a beat.
            auto two = [&](float cents)
            {
                return renderSlot(vowel, 0.2f, 57, 9600, [&](U& e)
                {
                    e.setParameter(UnifiedParam::WAVE2, (float) WT);
                    e.setParameter(UnifiedParam::SRC_B2, (float) vowel);
                    e.setParameter(UnifiedParam::SRC_A2, 0.2f);
                    e.setParameter(UnifiedParam::MIX, 0.5f);
                    e.setParameter(UnifiedParam::DETUNE2, cents);
                });
            };
            const auto unison = two(0.0f), detuned = two(25.0f);
            expect(finite(detuned) && rms(detuned) > 0.02);
            expect(changed(unison, detuned) > 0.05, "DET2 changes nothing: " + juce::String(changed(unison, detuned), 4));

            auto crossed = [&](float amount)
            {
                return renderSlot(bell, 0.6f, 57, 9600, [&](U& e)
                {
                    e.setParameter(UnifiedParam::WAVE2, (float) WT);
                    e.setParameter(UnifiedParam::SRC_B2, (float) (OLD + 3));       // FM1
                    e.setParameter(UnifiedParam::SRC_A2, 0.5f);
                    e.setParameter(UnifiedParam::CROSS_MOD, amount);
                });
            };
            const auto none = crossed(0.0f), some = crossed(0.6f);
            expect(finite(some) && rms(some) > 0.02);
            expect(changed(none, some) > 0.05, "XMOD changes nothing: " + juce::String(changed(none, some), 4));
            logMessage("on BELL / VOWL: stack " + juce::String(changed(plain, stacked), 3) + ", detune "
                       + juce::String(changed(unison, detuned), 3) + ", cross-mod " + juce::String(changed(none, some), 3)
                       + " of the sound's power changed");

            // The third slot too.
            U e(1);
            e.prepareToPlay(SR, BLOCK);
            quiet(e);
            e.setParameter(UnifiedParam::WAVE1, 0.0f);
            e.setParameter(UnifiedParam::WAVE3, (float) WT);
            e.setParameter(UnifiedParam::SRC_B3, (float) (OLD + 13));
            e.setParameter(UnifiedParam::SRC_A3, 0.5f);
            e.setParameter(UnifiedParam::SUB, 1.0f);
            const auto withThird = play(e, 57, 9600);
            expect(finite(withThird) && rms(withThird) > 0.02, "slot 3 on COMB");
        }

        beginTest("An LFO on a slot's table cell still moves it eight tables at full depth, whatever the list's length");
        {
            int dest = -1;
            for (int d = 0; d < 200 && dest < 0; ++d)
                if (UnifiedVoice::paramOfDest(d) == UnifiedParam::SRC_B1)
                    dest = d;
            expect(dest > 0, "1B is a destination");
            // A square LFO, 20 s per cycle, started by the key: high for the first ten seconds.
            // Depth 50 from SAW (0): half of eight tables = PWM (4), as in v0.9; a span of the
            // whole list would arrive at table 11.
            const auto moved = renderSlot(0, 0.37f, 57, 9600, [&](U& e)
            {
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_RATE), 0.05f);
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_SHAPE), (float) (int) LFO::Shape::Square);
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_TRIG), (float) (int) LFO::TriggerMode::KeySync);
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEPTH), 0.5f);
                e.setParameter(ModParam::lfoParamId(0, ModParam::LFO_DEST), (float) dest);
            });
            const double toPwm = shapeDistance(shape(moved), shape(renderSlot(4, 0.37f, 57)));
            const double toSaw = shapeDistance(shape(moved), shape(renderSlot(0, 0.37f, 57)));
            const double toWhole = shapeDistance(shape(moved), shape(renderSlot(11, 0.37f, 57)));
            logMessage("LFO at depth 50 on 1TBL from SAW: distance to PWM " + juce::String(toPwm, 4) + ", to SAW " + juce::String(toSaw, 4)
                       + ", to table 11 " + juce::String(toWhole, 4));
            expect(toPwm < 0.005, "not at PWM");
            expect(toSaw > 0.05 && toWhole > 0.05);
        }

        beginTest("A slot on a newer table is saved with a project and with a preset");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            const std::vector<std::pair<int, float>> values = {
                { UnifiedParam::WAVE1, (float) WT }, { UnifiedParam::SRC_B1, 14.0f }, { UnifiedParam::SRC_A1, 0.4f },      // BELL
                { UnifiedParam::WAVE2, (float) WT }, { UnifiedParam::SRC_B2, 22.0f }, { UnifiedParam::SRC_A2, 0.8f },      // COMB
                { UnifiedParam::WAVE3, (float) WT }, { UnifiedParam::SRC_B3, 9.0f },  { UnifiedParam::SRC_A3, 0.1f } };    // VOWL
            for (auto& [id, value] : values)
            {
                audio.setTrackParameter(0, id, value);
                seq.getPattern().getTrack(0).soundParams[id] = value;
            }
            const auto data = ProjectData::fromJSON(ProjectBridge::capture({ seq, audio }, "EMBED").toJSON());
            SequencerCore seq2;
            AudioEngine audio2;
            audio2.setSequencer(&seq2);
            ProjectBridge::apply(data, { seq2, audio2 });
            expect(audio2.getEngine(0) != nullptr && audio2.getEngine(0)->getType() == EngineType::UnifiedVoice);
            for (auto& [id, value] : values)
            {
                expectWithinAbsoluteError(audio2.getTrackParameter(0, id), value, 1.0e-5f, "project: id " + juce::String(id));
                if (auto* e = audio2.getEngine(0))
                    expectWithinAbsoluteError(e->getParameter(id), value, 1.0e-5f, "project, engine: id " + juce::String(id));
            }
            // And it is the same sound: the loaded engine's first slot plays BELL at 0.4.
            if (auto* loaded = dynamic_cast<U*>(audio2.getEngine(0)))
            {
                loaded->prepareToPlay(SR, BLOCK);
                quiet(*loaded);
                const auto x = play(*loaded, 57, 9600);
                const double d = shapeDistance(shape(x), shape(renderSlot(14, 0.4f, 57)));
                expect(d < 0.001, "the loaded project's slot is not BELL at 40: " + juce::String(d, 5));
            }

            Preset preset;
            preset.name = "EMBED";
            preset.engineType = EngineType::UnifiedVoice;
            for (auto& [id, value] : values)
                preset.soundParams[id] = value;
            const Preset back = Preset::fromJSON(preset.toJSON());
            auto fresh = MW::makeEngine(back.engineType, &back.soundParams);
            for (auto& [id, value] : values)
                expectWithinAbsoluteError(fresh->getParameter(id), value, 1.0e-5f, "preset: id " + juce::String(id));
        }

        beginTest("Factory presets: every newer table has one, three play through CLSC, each names its table");
        {
            const auto all = FactoryPresets::getAll();
            std::map<juce::String, const Preset*> byName;
            for (auto& p : all)
                byName[p.name] = &p;
            std::set<juce::String> tablesUsed;
            int classic = 0;
            for (auto& x : expectedPresets())
            {
                const auto it = byName.find(x.name);
                expect(it != byName.end(), juce::String(x.name) + ": missing");
                if (it == byName.end())
                    continue;
                const Preset& p = *it->second;
                expect(p.isFactory, p.name + ": factory");
                expect(p.engineType == EngineType::Wavetable, p.name + ": a wavetable-engine preset");
                expectEquals(p.category, juce::String(x.category), p.name);
                const auto table = p.soundParams.find(WE::TableSelect);
                expect(table != p.soundParams.end(), p.name + ": sets TABL");
                if (table == p.soundParams.end())
                    continue;
                const int index = (int) std::lround(table->second);
                expect(index >= WE::FIRST_MORE_TABLE && index < WE::NUM_TABLES, p.name + ": a newer built-in table (not USER, not KEYS)");
                expectEquals(juce::String(WE::tableLabels()[juce::jlimit(0, WE::NUM_TABLES - 1, index)]), juce::String(x.table), p.name);
                tablesUsed.insert(x.table);
                const auto interp = p.soundParams.find(WE::InterpMode);
                const bool isClassic = interp != p.soundParams.end()
                                       && (int) std::lround(interp->second) == (int) WavetableInterpMode::Classic;
                expect(isClassic == x.classic, p.name + ": INTP");
                classic += isClassic ? 1 : 0;
                for (auto& [id, value] : p.soundParams)
                {
                    expect(EngineParamRange::contains(id) && ! MixParam::isMixParam(id), p.name + ": id " + juce::String(id) + " is not an engine id");
                    expect(! EngineLfoSection::ownsStrip(id), p.name + ": sets a STRIP cell");
                    expect(! (id >= WE::KeyAlgo && id < WE::KeyPlaceBase + WavetableKeys::MAX_KEYS), p.name + ": sets a key-frame cell");
                    juce::ignoreUnused(value);
                }
                // What it sets is what a fresh engine then holds (every id is a real cell).
                auto e = MW::makeEngine(EngineType::Wavetable, &p.soundParams);
                for (auto& [id, value] : p.soundParams)
                    expectWithinAbsoluteError(e->getParameter(id), value, 1.0e-4f, p.name + ": id " + juce::String(id) + " is not held");
            }
            for (auto& def : wavetables::definitions())
                expect(tablesUsed.count(def.label) > 0, juce::String(def.label) + " has no preset");
            expectEquals((int) tablesUsed.size(), 14);
            expect(classic >= 3, "presets through CLSC");
            expectEquals(all.back().name, juce::String("Narptune"), "the saved sound stays the last factory preset");
        }

        beginTest("Factory presets for the newer tables: audible, as loud as the older wavetable presets, and the strip moves them");
        {
            const auto all = FactoryPresets::getAll();
            auto level = [&](const Preset& p) { return loudest(MW::render(*MW::makeEngine(p.engineType, &p.soundParams), 0.0f)); };
            float older = 0.0f;
            int olderCount = 0;
            for (auto& p : all)
                for (const char* name : { "Digital Choir", "Frozen Sweep", "Glitch Table" })
                    if (p.name == name)
                    {
                        const float dB = level(p);
                        logMessage(p.name.paddedRight(' ', 18) + " (older)  loudest 100 ms " + juce::String(dB, 1) + " dB");
                        older += dB;
                        ++olderCount;
                    }
            expectEquals(olderCount, 3);
            older /= (float) std::max(1, olderCount);
            logMessage("the three older wavetable presets: " + juce::String(older, 1) + " dB on average");
            constexpr float LEVEL_WINDOW_DB = 4.0f;
            for (auto& x : expectedPresets())
                for (auto& p : all)
                    if (p.name == x.name)
                    {
                        const auto rest = MW::render(*MW::makeEngine(p.engineType, &p.soundParams), 0.0f);
                        const auto again = MW::render(*MW::makeEngine(p.engineType, &p.soundParams), 0.0f);
                        const auto up = MW::render(*MW::makeEngine(p.engineType, &p.soundParams), 1.0f);
                        const float dB = loudest(rest), strip = MW::difference(rest, up);
                        logMessage(p.name.paddedRight(' ', 18) + " " + juce::String(x.table).paddedRight(' ', 5) + juce::String(x.category).paddedRight(' ', 5)
                                   + " loudest 100 ms " + juce::String(dB, 1) + " dB (" + juce::String(dB - older, 1) + " from the older ones)"
                                   + "  strip " + juce::String(strip, 3));
                        expect(finite(rest) && finite(up), p.name);
                        expect(MW::measure(rest).rms > 0.003f, p.name + ": hardly audible");
                        expectWithinAbsoluteError(dB, older, LEVEL_WINDOW_DB, p.name + ": level");
                        expect(MW::difference(rest, again) <= MW::MAX_REST_DIFFERENCE, p.name + ": two renders at rest differ");
                        expect(strip >= MW::MIN_DIFFERENCE, p.name + ": the strip changes the sound by only " + juce::String(strip, 3));
                    }
        }

        beginTest("XMOD is through-zero FM: in tune and harmonic with any modulator, no jumps from a bright one");
        {
            // Oscillator 1 a sine, heard alone; oscillator 2 the modulator, at the same pitch.
            auto fm = [&](float amount, int wave2, float pw, float tune2 = 0.0f, int note = 57)
            {
                U e(1);
                e.prepareToPlay(SR, BLOCK);
                quiet(e);
                e.setParameter(UnifiedParam::WAVE1, 0.0f);              // SIN
                e.setParameter(UnifiedParam::WAVE2, (float) wave2);
                e.setParameter(UnifiedParam::PULSE_WIDTH, pw);
                e.setParameter(UnifiedParam::TUNE2, tune2);
                e.setParameter(UnifiedParam::DETUNE2, 0.0f);
                e.setParameter(UnifiedParam::AMP_SUSTAIN, 1.0f);
                e.setParameter(UnifiedParam::CROSS_MOD, amount);
                return play(e, note, 24000);                            // A3 = 220 Hz, well settled
            };
            // The share of the sound's power that lies on whole multiples of the played pitch.
            auto harmonicShare = [&](const std::vector<float>& x, double hz = 220.0)
            {
                double onPitch = 0.0;
                for (int h = 1; (double) h * hz < 18000.0; ++h)
                {
                    const double a = harmonic(x, hz, h);
                    onPitch += 0.5 * a * a;
                }
                const double mean = std::accumulate(x.begin(), x.end(), 0.0) / (double) x.size();
                return onPitch / std::max(1.0e-12, rms(x) * rms(x) - mean * mean);
            };
            auto biggestStep = [](const std::vector<float>& x)
            {
                float step = 0.0f, peak = 0.0f;
                for (size_t n = 1; n < x.size(); ++n)
                {
                    step = std::max(step, std::abs(x[n] - x[n - 1]));
                    peak = std::max(peak, std::abs(x[n]));
                }
                return step / std::max(1.0e-6f, peak);
            };
            const int SAW = 1, SQR = 2;
            const auto plain = fm(0.0f, SAW, 0.5f);
            expect(harmonicShare(plain) > 0.98, "the measure itself: a plain sine is on pitch");

            for (float amount : { 0.3f, 0.7f, 1.0f })
            {
                const juce::String at = " at XMOD " + juce::String(amount, 1);
                // A saw: the phase must not jump with it (plain phase modulation would follow the
                // saw's edge), and the tone stays a harmonic one on the played pitch.
                const auto saw = fm(amount, SAW, 0.5f);
                expect(finite(saw) && rms(saw) > 0.02);
                expect(changed(plain, saw) > 0.05, "XMOD is heard" + at);
                expect(harmonicShare(saw) > 0.97, "saw modulator: harmonic" + at + ", " + juce::String(harmonicShare(saw), 3));
                expect(biggestStep(saw) < 0.3f, "saw modulator: no jump in the wave" + at + ", " + juce::String(biggestStep(saw), 3));

                // A lopsided pulse has an offset: with linear FM that is not AC-coupled the note
                // would go out of tune. It must stay on pitch.
                // (PW also folds the sine carrier, which adds high harmonics: an octave lower,
                //  measured against the same patch without XMOD.)
                const auto pulse = fm(amount, SQR, 0.15f, 0.0f, 45);
                const auto pulseOff = fm(0.0f, SQR, 0.15f, 0.0f, 45);
                expect(harmonicShare(pulse, 110.0) > harmonicShare(pulseOff, 110.0) - 0.03,
                       "offset modulator: still in tune" + at + ", " + juce::String(harmonicShare(pulse, 110.0), 3)
                       + " against " + juce::String(harmonicShare(pulseOff, 110.0), 3));

                // An octave and a fifth up (ratio 3): whole-number tunings stay harmonic.
                const auto third = fm(amount, SAW, 0.5f, 19.0f);
                expect(harmonicShare(third) > 0.9, "modulator a twelfth up: harmonic" + at + ", " + juce::String(harmonicShare(third), 3));
            }

            // Deep enough, the frequency really passes through zero: the sine runs backwards for
            // part of each cycle, so it turns round more often than a sine or a one-way FM would.
            auto turns = [](const std::vector<float>& x)
            {
                int n = 0;
                for (size_t i = 2; i < x.size(); ++i)
                    if ((x[i] - x[i - 1]) * (x[i - 1] - x[i - 2]) < 0.0f) ++n;
                return n;
            };
            expect(turns(fm(1.0f, 0, 0.5f)) > turns(plain) + 20, "full XMOD from a sine: the phase reverses");
        }
    }
};

static WavetableEmbedTests wavetableEmbedTests;
