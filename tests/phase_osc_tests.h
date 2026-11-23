#pragma once
#include <juce_core/juce_core.h>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "fast_measure.h"
#include "phase_osc_bench.h"
#include "../src/dsp/PhaseOsc.h"
#include "../src/engine/FMEngine.h"

// -----------------------------------------------------------------------------
// The shared oscillator (src/dsp/PhaseOsc.h) and the FM engine on it.
//
// The core: the sine against a copy of the operator as it was before the
// oscillator was shared (bit for bit), a table read against the table read-out
// (bit for bit), the position, folding under deep phase modulation against a
// read that takes no notice of the modulation, sync inside the sample, the
// wave sequence, the cycles of a sample, and anything at all as input.
// The FM engine: a table as carrier and as modulator against the spectra the
// table's own harmonics predict, the WAVE and WPOS pages and their cells.
//
// Spectra of the core are whole-number bins of one 32768-point transform
// (nothing leaks); the engine's are projections over a whole number of periods
// (note 45 = 110 Hz, one second at 48 kHz). Levels and differences are RMS.
//
// NEPTUNE_PHASEOSC_BENCH=1 prints what the engine and one operator cost and
// runs no test (tests/phase_osc_bench.h).
// -----------------------------------------------------------------------------

// The FM operator as it was at commit 720d2b7, before its phase and its sine moved into
// PhaseOsc: the reference the sine path is held to. Do not "tidy" it.
class LegacyFMOperator
{
public:
    void setSampleRate(double sr) { sampleRate = sr; env.setSampleRate(sr); levelStep = (float) (1.0 / (0.004 * sr)); }
    void setRatio(float r) { ratio = r; }
    void setLevel(float l) { level = l; }
    void setDetune(float cents) { detuneCents = cents; detuneMul = std::exp2(cents / 1200.0f); }
    void setPitchMul(float m) { pitchMul = m; }
    void setModSensitivity(float s) { modSensitivity = s; }
    void setFeedback(float fb) { feedback = fb; }
    void setFixed(bool f) { fixedFreq = f; }
    void setFixedHz(float hz) { fixedHz = hz; }
    void setLfo(float pitchRatio, float indexFactor, float feedbackOffset)
    {
        lfoPitchMul = pitchRatio;
        lfoIndexMul = indexFactor;
        lfoFeedback = feedbackOffset;
    }
    void clearLfo() { setLfo(1.0f, 1.0f, 0.0f); }
    void setExternal(float radians) { external = radians; }
    Envelope& getEnvelope() { return env; }

    void noteOn(int note, float velocity)
    {
        currentNote = note;
        currentVelocity = velocity;
        baseFrequency = dsp::midiNoteToHz(note);
        phase = 0.0;
        prevOutput = 0.0f;
        env.noteOn();
        active = true;
        struck = true;
    }

    void noteOff() { env.noteOff(); }

    float process(float modulation = 0.0f)
    {
        if (!active)
            return 0.0f;

        double freq = fixedFreq ? (double) fixedHz
                                : baseFrequency * (double) ratio * (double) detuneMul * (double) pitchMul;
        if (lfoPitchMul != 1.0f)
            freq *= (double) lfoPitchMul;
        double phaseInc = freq / sampleRate;

        if (feedbackNow != feedback)
            feedbackNow = struck ? feedback : dsp::towards(feedbackNow, feedback, levelStep);
        float fbMod = prevOutput * (lfoFeedback != 0.0f ? std::clamp(feedbackNow + lfoFeedback, 0.0f, 1.0f) : feedbackNow);
        if (external != 0.0f)
            modulation += external;
        float totalMod = modulation * modSensitivity * lfoIndexMul + fbMod;

        float sample = std::sin((float)(phase * dsp::TWO_PI_D) + totalMod);
        phase = dsp::wrapPhase(phase + phaseInc);

        float envVal = env.process();
        if (!env.isActive())
            active = false;

        if (levelNow != level)
        {
            const float d = level - levelNow;
            levelNow = struck || std::abs(d) <= levelStep ? level : levelNow + (d > 0.0f ? levelStep : -levelStep);
        }
        struck = false;

        float output = sample * envVal * levelNow * currentVelocity;
        prevOutput = output;
        return output;
    }

    void reset()
    {
        phase = 0.0;
        prevOutput = 0.0f;
        env.reset();
        active = false;
    }

private:
    double sampleRate = 44100.0;
    double baseFrequency = 440.0;
    double phase = 0.0;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool active = false;
    float ratio = 1.0f;
    float level = 1.0f;
    float levelNow = 1.0f, levelStep = 0.005f;
    bool struck = true;
    float detuneCents = 0.0f;
    float detuneMul = 1.0f;
    float pitchMul = 1.0f;
    float modSensitivity = 1.0f;
    float feedback = 0.0f, feedbackNow = 0.0f;
    bool fixedFreq = false;
    float fixedHz = 440.0f;
    float prevOutput = 0.0f;
    float lfoPitchMul = 1.0f;
    float lfoIndexMul = 1.0f;
    float lfoFeedback = 0.0f;
    float external = 0.0f;
    Envelope env;
};

class PhaseOscTests : public juce::UnitTest
{
public:
    PhaseOscTests() : UnitTest("Phase Osc") {}

    using F = FMEngine;
    using Wave = phaseosc::Wave;
    using Osc = phaseosc::PhaseOsc;

    static constexpr int N = 32768;                 // one transform; a pitch of k / N cycles a sample is bin k
    static constexpr double SR = 48000.0;
    static constexpr int SAW = 0, SQR = 1, TRI = 2, SIN = 3, VOWL = 9, BELL = 14;   // WavetableFactory::builtInTables()

    static const Wavetable& table(int index) { return *WavetableFactory::builtInTables()[(size_t) index]; }
    static float waveIndex(int tableIndex) { return (float) (phaseosc::FIRST_TABLE_WAVE + tableIndex); }

    // ---- measuring ----------------------------------------------------------------------------------

    static double rms(const std::vector<float>& x)
    {
        double sum = 0.0;
        for (float v : x) sum += (double) v * v;
        return x.empty() ? 0.0 : std::sqrt(sum / (double) x.size());
    }

    // RMS of the difference over the RMS of the two (0 = the same samples, about 1.4 = unrelated).
    static double apart(const std::vector<float>& a, const std::vector<float>& b)
    {
        double diff = 0.0, sum = 0.0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            diff += (double) (a[i] - b[i]) * (a[i] - b[i]);
            sum += 0.5 * ((double) a[i] * a[i] + (double) b[i] * b[i]);
        }
        return sum > 0.0 ? std::sqrt(diff / sum) : 0.0;
    }

    static bool same(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }

    // Power in each bin of the last N samples.
    static std::vector<double> spectrum(const std::vector<float>& x)
    {
        std::vector<std::complex<double>> a((size_t) N);
        for (int i = 0; i < N; ++i)
            a[(size_t) i] = { (double) x[x.size() - (size_t) N + (size_t) i], 0.0 };
        fastmeasure::fft(a);
        std::vector<double> power((size_t) N / 2);
        for (int b = 0; b < N / 2; ++b)
            power[(size_t) b] = std::norm(a[(size_t) b]);
        return power;
    }

    // A sound made only of whole multiples of bin g has everything else from folding: the share
    // of the power that is not on a multiple of g, in dB (g odd, so nothing folds onto the grid
    // before it has folded many times).
    static double foldedDb(const std::vector<float>& x, int g)
    {
        const auto p = spectrum(x);
        double on = 0.0, off = 0.0;
        for (int b = 1; b < N / 2; ++b)
            (b % g == 0 ? on : off) += p[(size_t) b];
        return 10.0 * std::log10(std::max(off / (on + off), 1.0e-12));
    }

    // Level of harmonic k of a frame (1 = a sine of peak 1).
    static double frameHarmonic(const std::array<float, Wavetable::kFrameSize>& frame, int k)
    {
        double c = 0.0, s = 0.0;
        for (int n = 0; n < Wavetable::kFrameSize; ++n)
        {
            const double a = dsp::TWO_PI_D * (double) k * (double) n / (double) Wavetable::kFrameSize;
            c += frame[(size_t) n] * std::cos(a);
            s += frame[(size_t) n] * std::sin(a);
        }
        return 2.0 * std::sqrt(c * c + s * s) / (double) Wavetable::kFrameSize;
    }

    // ---- the core, rendered -------------------------------------------------------------------------

    // `samples` of one oscillator: a carrier reading `wave` at `step`, phase-modulated by a sine
    // at `modStep` with an index of `index` radians.
    static std::vector<float> renderOsc(const Wave& wave, double step, float position, int samples,
                                        double modStep = 0.0, double index = 0.0)
    {
        Osc osc;
        osc.setSampleRate(SR);
        osc.reset();
        std::vector<float> out((size_t) samples);
        double mod = 0.0;
        for (auto& s : out)
        {
            s = osc.next(wave, step, (float) (index * std::sin(mod * dsp::TWO_PI_D)), position);
            mod = dsp::wrapPhase(mod + modStep);
        }
        return out;
    }

    // The same carrier read with no notice taken of the modulation: the copy for the pitch alone.
    static std::vector<float> renderNaive(const Wavetable& t, double step, int samples, double modStep, double index)
    {
        std::vector<float> out((size_t) samples);
        double phase = 0.0, mod = 0.0;
        const int level = WavetableRead::mipLevel(t, step);
        for (auto& s : out)
        {
            const float radians = (float) (index * std::sin(mod * dsp::TWO_PI_D));
            s = WavetableRead::sample(t, level, 1.0f, dsp::wrapPhase(phase + (double) radians * phaseosc::CYCLES_PER_RADIAN));
            phase = dsp::wrapPhase(phase + step);
            mod = dsp::wrapPhase(mod + modStep);
        }
        return out;
    }

    // ---- the engine, rendered -------------------------------------------------------------------------

    // Six carriers, all silent, envelopes that open at once and hold: a test raises what it wants heard.
    static void steady(F& e, double sr = SR, int block = 64)
    {
        e.prepareToPlay(sr, block);
        e.setParameter(F::AlgorithmParam, 5.0f);
        for (int op = 0; op < 6; ++op)
        {
            e.setParameter(op * 10 + 1, 0.0f);
            e.setParameter(op * 10 + 4, 0.001f);
            e.setParameter(op * 10 + 6, 1.0f);
        }
    }

    static void run(F& e, std::vector<float>& out, int samples, int block = 64)
    {
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        for (int done = 0; done < samples; done += block)
        {
            buf.clear();
            e.processBlock(buf, midi);
            for (int i = 0; i < block && done + i < samples; ++i)
                out.push_back(buf.getSample(0, i));
        }
    }

    // One second of note 45 (110 Hz: 110 whole periods) after a tenth of a second to settle.
    static std::vector<float> second(F& e, int note = 45, float velocity = 1.0f)
    {
        e.noteOn(note, velocity, -1);
        std::vector<float> lead, out;
        run(e, lead, 4800);
        run(e, out, 48000);
        return out;
    }

    static double harmonic(const std::vector<float>& x, int h) { return fastmeasure::harmonic(x, 110.0, h, SR); }

    static int destIndex(F& e, const char* label)
    {
        for (auto& page : e.getParamPages())
            if (const auto* dest = page.find(EngineLfoSection::id(0, EngineLfoSection::Dest)))
                for (int i = 0; i < dest->numChoices; ++i)
                    if (juce::String(dest->choices[i]) == label)
                        return i;
        return -1;
    }

    static bool finiteAndUnder(const std::vector<float>& x, float limit)
    {
        for (float v : x)
            if (! std::isfinite(v) || std::abs(v) > limit)
                return false;
        return true;
    }

    // ---- the old operator against the new, one script for both ------------------------------------

    template <typename Op>
    static std::vector<float> operatorScript(double sr, int variant)
    {
        Op a, b;                // b modulates a
        for (Op* op : { &a, &b })
        {
            op->setSampleRate(sr);
            Envelope::Params p = op->getEnvelope().getParams();
            p.attack = 0.002f; p.decay = 0.05f; p.sustain = 0.6f; p.release = 0.03f;
            op->getEnvelope().setParams(p);
        }
        a.setRatio(1.0f);
        b.setRatio(variant == 0 ? 2.0f : 3.51f);
        b.setLevel(0.9f);
        b.setFeedback(variant == 2 ? 0.8f : 0.0f);
        a.setFeedback(variant == 1 ? 0.35f : 0.0f);
        if (variant == 3) { a.setFixed(true); a.setFixedHz(1234.5f); }
        a.noteOn(variant == 4 ? 127 : 57, 0.83f);
        b.noteOn(variant == 4 ? 127 : 57, 0.83f);

        std::vector<float> out;
        const int total = 9000;
        for (int n = 0; n < total; ++n)
        {
            // Everything an engine does to an operator while it sounds.
            if (n == 700)  { a.setDetune(13.0f); b.setDetune(-7.5f); }
            if (n == 1100) { a.setLevel(0.4f); b.setLevel(1.0f); }
            if (n == 1500) { a.setPitchMul(1.0594631f); b.setPitchMul(1.0594631f); }
            if (n == 1900) a.setModSensitivity(2.5f);
            if (n == 2300) { a.setFeedback(0.6f); b.setFeedback(0.2f); }
            if (n >= 2700 && n < 3500)
            {
                const float w = std::sin((float) n * 0.01f);
                a.setLfo(std::exp2(w / 12.0f), 1.0f + 0.5f * w, 0.3f * w);
                b.setLfo(std::exp2(-w / 12.0f), 1.0f, 0.0f);
            }
            if (n == 3500) { a.clearLfo(); b.clearLfo(); }
            if (n >= 3900 && n < 4700) a.setExternal(0.7f * std::sin((float) n * 0.21f));
            if (n == 4700) a.setExternal(0.0f);
            if (n == 5200) { a.noteOff(); b.noteOff(); }
            if (n == 5600) { a.noteOn(81, 1.0f); b.noteOn(81, 1.0f); }          // struck again in the release
            if (n == 6400) a.setRatio(0.5f);
            if (n == 7000) { a.setLevel(1.0f); a.setRatio(16.0f); }
            if (n == 8000) { a.noteOff(); b.noteOff(); }
            out.push_back(a.process(b.process()));
        }
        return out;
    }

    // ---- the tools ------------------------------------------------------------------------------------

    template <typename Op>
    static double timeOperator()
    {
        double best = 1.0e30;
        float keep = 0.0f;
        for (int run = 0; run < 7; ++run)
        {
            Op a, b;
            for (Op* op : { &a, &b })
            {
                op->setSampleRate(SR);
                Envelope::Params p = op->getEnvelope().getParams();
                p.sustain = 1.0f;
                op->getEnvelope().setParams(p);
                op->noteOn(57, 0.8f);
            }
            b.setRatio(2.0f);
            b.setFeedback(0.3f);
            const int samples = 4000000;
            const auto t0 = std::chrono::steady_clock::now();
            for (int n = 0; n < samples; ++n)
                keep += a.process(b.process());
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / (2.0 * samples));
        }
        return keep == keep ? best * 1.0e9 : -1.0;
    }

    static void bench()
    {
       #if JUCE_DEBUG
        std::printf("(a Debug build: these numbers say little; use a Release build)\n");
       #endif
        std::printf("One operator, sine, modulated, with feedback (best of 7 runs of 8 million operator-samples):\n");
        const double before = timeOperator<LegacyFMOperator>(), after = timeOperator<FMOperator>();
        const double before2 = timeOperator<LegacyFMOperator>(), after2 = timeOperator<FMOperator>();
        std::printf("  the operator as it was          %6.2f ns/sample   (again: %6.2f)\n", before, before2);
        std::printf("  the operator on PhaseOsc        %6.2f ns/sample   (again: %6.2f)\n", after, after2);
        std::fflush(stdout);
        phaseOscBench::run();
    }

    // ---------------------------------------------------------------------------------------------

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_PHASEOSC_BENCH", {}).isNotEmpty())
        {
            beginTest("Timed");
            bench();
            expect(true);
            return;
        }

        beginTest("The sine is the operator as it was: the same samples, bit for bit");
        {
            for (double sr : { 22050.0, 48000.0, 192000.0 })
                for (int variant = 0; variant < 5; ++variant)
                {
                    const auto before = operatorScript<LegacyFMOperator>(sr, variant);
                    const auto after = operatorScript<FMOperator>(sr, variant);
                    expect(rms(before) > 0.05, "the script sounds");
                    expect(same(before, after), "variant " + juce::String(variant) + " at " + juce::String(sr) + " Hz");
                }

            // The core by itself: std::sin((float) (phase * 2 pi) + radians), then the advance.
            Osc osc;
            osc.reset();
            double phase = 0.0;
            const double step = 440.0 / SR;
            bool equal = true;
            for (int n = 0; n < 20000; ++n)
            {
                const float radians = 3.0f * std::sin((float) n * 0.05f);
                const float expected = std::sin((float) (phase * dsp::TWO_PI_D) + radians);
                phase = dsp::wrapPhase(phase + step);
                const float got = osc.next(Wave::sine(), step, radians, 0.3f);
                equal = equal && std::memcmp(&expected, &got, sizeof(float)) == 0;
            }
            expect(equal, "PhaseOsc::next on the sine");
        }

        beginTest("A table without modulation is the table read-out's own sample, bit for bit");
        {
            bool equal = true;
            int reads = 0;
            for (int t : { SAW, SQR, TRI, SIN, VOWL, BELL })
                for (double hz : { 27.5, 110.0, 439.0, 1760.0, 7040.0, 19000.0 })
                    for (float position : { 0.0f, 0.37f, 1.0f })
                        for (auto mode : { WavetableInterpMode::Linear, WavetableInterpMode::Stepped, WavetableInterpMode::Vintage,
                                           WavetableInterpMode::Classic })
                        {
                            const double step = hz / SR;
                            const Wave wave = Wave::of(&table(t), mode);
                            Osc osc;
                            osc.reset();
                            double phase = 0.0;
                            for (int n = 0; n < 300; ++n, ++reads)
                            {
                                const float expected = WavetableRead::sample(table(t), WavetableRead::mipLevel(table(t), step), position, phase, mode);
                                const float byFunction = phaseosc::read(wave, phase, step, position);
                                const float byOsc = osc.next(wave, step, 0.0f, position);
                                phase = dsp::wrapPhase(phase + step);
                                equal = equal && std::memcmp(&expected, &byFunction, sizeof(float)) == 0
                                              && std::memcmp(&expected, &byOsc, sizeof(float)) == 0;
                            }
                        }
            expect(equal, juce::String(reads) + " reads");
        }

        beginTest("A table as the carrier: the harmonics of the table's own wave, at the level of a sine operator");
        {
            F sine, square;
            for (F* e : { &sine, &square })
            {
                steady(*e);
                e->setParameter(F::Op1Level, 1.0f);
            }
            square.setParameter(F::Op1Ratio + F::OP_WAVE, waveIndex(SQR));
            square.setParameter(F::Op1Ratio + F::OP_POSITION, 1.0f);
            const auto s = second(sine), q = second(square);
            const double unit = harmonic(s, 1);
            expect(unit > 0.1, "a sine operator sounds");
            expect(harmonic(s, 2) < 0.001 * unit && harmonic(s, 3) < 0.001 * unit, "and is a sine");

            // 110 Hz reads the copy with 64 harmonics; POS 1 is the last frame.
            const auto& frame = table(SQR).frames[(size_t) WavetableRead::mipLevel(table(SQR), 110.0 / SR)].back();
            int checked = 0;
            for (int k = 1; k <= 9; ++k)
            {
                const double expected = frameHarmonic(frame, k), got = harmonic(q, k) / unit;
                if (expected > 0.05)
                {
                    ++checked;
                    expectWithinAbsoluteError(got, expected, 0.03 * expected, "harmonic " + juce::String(k));
                }
                else
                    expect(got < 0.02, "harmonic " + juce::String(k) + " is not in the wave: " + juce::String(got, 4));
            }
            expect(checked >= 4, "a square has its odd harmonics: " + juce::String(checked));
        }

        beginTest("A table as the modulator: a pair of sidebands for each of its harmonics, half the index times that harmonic");
        {
            // 2 > 1 (algorithm 2+4): operator 1 a sine at 16 x 110 Hz, operator 2 a sawtooth
            // table at 110 Hz with an index of 0.03. To first order the carrier gets sidebands at
            // (16 +- k) x 110 Hz of index x a(k) / 2, a(k) = harmonic k of the modulating wave.
            const float index = 0.03f;
            auto make = [&](F& e, float wave)
            {
                steady(e);
                e.setParameter(F::AlgorithmParam, 6.0f);
                e.setParameter(F::Op1Ratio, 16.0f);
                e.setParameter(F::Op1Level, 1.0f);
                e.setParameter(F::Op2Level, index);
                e.setParameter(F::Op2Ratio + F::OP_WAVE, wave);
                e.setParameter(F::Op2Ratio + F::OP_POSITION, 1.0f);
            };
            F saw, sine;
            make(saw, waveIndex(SAW));
            make(sine, 0.0f);
            const auto x = second(saw), y = second(sine);
            const double carrier = harmonic(x, 16);
            const auto& frame = table(SAW).frames[(size_t) WavetableRead::mipLevel(table(SAW), 110.0 / SR)].back();
            int checked = 0;
            for (int k = 1; k <= 6; ++k)
            {
                const double a = frameHarmonic(frame, k), expected = 0.5 * index * a;
                if (a < 0.25 * frameHarmonic(frame, 1))
                    continue;
                ++checked;
                // (What the harmonics do to each other, second order, pushes one of a pair up and
                // the other down: the pair's mean is held closely, each of the two loosely.)
                const double upper = harmonic(x, 16 + k) / carrier, lower = harmonic(x, 16 - k) / carrier;
                expectWithinAbsoluteError(0.5 * (upper + lower), expected, 0.05 * expected, "sideband pair " + juce::String(k));
                expectWithinAbsoluteError(upper, expected, 0.2 * expected, "upper sideband " + juce::String(k));
                expectWithinAbsoluteError(lower, expected, 0.2 * expected, "lower sideband " + juce::String(k));
            }
            expect(checked >= 3, "a sawtooth has its first harmonics: " + juce::String(checked));
            // A sine modulator has the first pair only.
            expectWithinAbsoluteError(harmonic(y, 17) / harmonic(y, 16), 0.5 * (double) index, 0.05 * 0.5 * index);
            expect(harmonic(y, 18) < 0.02 * harmonic(y, 17), "a sine modulator: no second pair");
            expect(harmonic(x, 18) > 0.2 * harmonic(x, 17), "a sawtooth modulator: a second pair");
        }

        beginTest("Position: the spectrum moves with it, by small steps everywhere, and a sweep has no click");
        {
            const double step = 110.0 / SR;
            const Wave vowels = Wave::of(&table(VOWL));
            expect(vowels.hasPosition() && ! Wave::sine().hasPosition() && ! Wave::of(&table(SIN)).hasPosition());
            const auto first = renderOsc(vowels, step, 0.0f, 9600), last = renderOsc(vowels, step, 1.0f, 9600);
            expect(apart(first, last) > 0.5, "the two ends of the table are two sounds: " + juce::String(apart(first, last), 3));

            // A thousandth of the table further on is nearly the same sound, wherever on the table.
            double largest = 0.0;
            for (int i = 0; i < 1000; i += 7)
                largest = std::max(largest, apart(renderOsc(vowels, step, (float) i / 1000.0f, 2400),
                                                  renderOsc(vowels, step, (float) (i + 1) / 1000.0f, 2400)));
            expect(largest < 0.03, "largest change for 1/1000 of the table: " + juce::String(largest, 4));

            // Swept through the whole table in a second, sample by sample: never a larger step
            // from one sample to the next than the steepest of the waves it passes has by itself.
            Osc osc;
            osc.reset();
            float before = 0.0f, steepestSweep = 0.0f, steepestStill = 0.0f;
            for (int n = 0; n < 48000; ++n)
            {
                const float s = osc.next(vowels, step, 0.0f, (float) n / 48000.0f);
                if (n > 0)
                    steepestSweep = std::max(steepestSweep, std::abs(s - before));
                before = s;
            }
            for (int f = 0; f < 64; ++f)        // (each of the table's 64 waves)
            {
                const auto still = renderOsc(vowels, step, (float) f / 63.0f, 4800);
                for (size_t n = 1; n < still.size(); ++n)
                    steepestStill = std::max(steepestStill, std::abs(still[n] - still[n - 1]));
            }
            expect(steepestSweep <= 1.05f * steepestStill,
                   "sweep " + juce::String(steepestSweep, 4) + " against " + juce::String(steepestStill, 4));

            // Out of range, or not a number: the nearest end.
            expect(same(renderOsc(vowels, step, -3.0f, 500), renderOsc(vowels, step, 0.0f, 500)));
            expect(same(renderOsc(vowels, step, 7.0f, 500), renderOsc(vowels, step, 1.0f, 500)));
            expect(same(renderOsc(vowels, step, std::numeric_limits<float>::quiet_NaN(), 500), renderOsc(vowels, step, 0.0f, 500)));
        }

        beginTest("Deep phase modulation: what folds back, against a read that takes no notice of the modulation");
        {
            // Carrier and modulator on whole multiples of bin 137 (about 200 Hz): everything the
            // pair should make is on that grid, and what is not has folded.
            const int g = 137;
            const double modStep = (double) g / N;
            struct Case { int table; double index; int multiple; };
            double worst = -200.0;
            for (int t : { SAW, SQR, BELL })
                for (double index : { 2.0, 8.0, 20.0, 60.0 })
                    for (int multiple : { 5, 21 })
                    {
                        const double step = (double) (g * multiple) / N;
                        const double naive = foldedDb(renderNaive(table(t), step, 2 * N, modStep, index), g);
                        const double ours = foldedDb(renderOsc(Wave::of(&table(t)), step, 1.0f, 2 * N, modStep, index), g);
                        const juce::String what = juce::String(table(t).name) + ", index " + juce::String(index) + ", carrier x "
                                                  + juce::String(multiple) + ": " + juce::String(ours, 1) + " dB against " + juce::String(naive, 1);
                        worst = std::max(worst, ours);
                        expect(ours < -65.0, what);
                        expect(ours < naive + 3.0, "never worse than taking no notice. " + what);
                        if (naive > -30.0)
                            expect(ours < naive - 50.0, "where it folds badly, 50 dB less. " + what);
                    }
            logMessage("  the most that folds back in any case: " + juce::String(worst, 1) + " dB");

            // It is not done by making everything dull: under a light modulation (index 1 at
            // the carrier's own pitch, so the phase moves at up to twice the pitch) the spectrum
            // from 4 kHz up keeps its power.
            {
                const double step = (double) (g * 5) / N;
                auto upper = [&](const std::vector<float>& x)
                {
                    const auto p = spectrum(x);
                    double sum = 0.0;
                    for (int b = g * 20; b < N / 2; ++b)        // from 4 kHz up
                        sum += p[(size_t) b];
                    return sum;
                };
                const double kept = upper(renderOsc(Wave::of(&table(SAW)), step, 1.0f, 2 * N, step, 1.0))
                                  / upper(renderNaive(table(SAW), step, 2 * N, step, 1.0));
                expect(kept > 0.8 && kept <= 1.001, "above 4 kHz it keeps " + juce::String(kept, 3) + " of the power");
            }

            // Nor by believing one sample: a sawtooth modulator moves the phase by its whole
            // index at each of its edges (here 2 radians, a third of a cycle, in a sample or two,
            // 200 times a second). A carrier that took that for its swing would be read as a
            // sine; it keeps its harmonics.
            {
                const double step = (double) (g * 5) / N;
                const Wave saw = Wave::of(&table(SAW));
                Osc carrier, modulator;
                carrier.reset();
                modulator.reset();
                std::vector<float> ours, naive;
                double phase = 0.0;
                const int level = WavetableRead::mipLevel(table(SAW), step);
                for (int n = 0; n < 2 * N; ++n)
                {
                    const float radians = modulator.next(saw, modStep, 0.0f, 1.0f);
                    ours.push_back(carrier.next(saw, step, radians, 1.0f));
                    naive.push_back(WavetableRead::sample(table(SAW), level, 1.0f, dsp::wrapPhase(phase + (double) radians * phaseosc::CYCLES_PER_RADIAN)));
                    phase = dsp::wrapPhase(phase + step);
                }
                auto upper = [&](const std::vector<float>& x)
                {
                    const auto p = spectrum(x);
                    double sum = 0.0;
                    for (int b = g * 20; b < N / 2; ++b)
                        sum += p[(size_t) b];
                    return sum;
                };
                const double kept = upper(ours) / upper(naive);
                expect(kept > 0.7, "under a sawtooth modulator it keeps " + juce::String(kept, 3) + " of the power above 4 kHz");
                expect(carrier.getSwing() < 0.75f * (float) step, "the swing: " + juce::String(carrier.getSwing(), 5) + " at a pitch of " + juce::String(step, 5));
            }

            // The copy never steps: a little more swing is a little less of the top, at any
            // swing, and none at all is exactly the plain read.
            {
                const Wave saw = Wave::of(&table(SAW));
                const double step = 0.004;
                float largest = 0.0f;
                bool plain = true;
                for (int n = 0; n < 997; ++n)
                {
                    const double phase = (double) n / 997.0;
                    for (float swing = 1.0e-6f; swing < 0.6f; swing *= 1.31f)
                        largest = std::max(largest, std::abs(phaseosc::read(saw, phase, step, 1.0f, swing)
                                                             - phaseosc::read(saw, phase, step, 1.0f, swing * 1.002f)));
                    const float a = phaseosc::read(saw, phase, step, 1.0f, 0.0f), b = phaseosc::read(saw, phase, step, 1.0f),
                                c = phaseosc::read(saw, phase, step, 1.0f, 1.0e-6f);
                    plain = plain && std::memcmp(&a, &b, sizeof(float)) == 0 && std::memcmp(&a, &c, sizeof(float)) == 0;
                }
                expect(largest < 0.004f, "largest change for 0.2 % more swing: " + juce::String(largest, 5));
                // The copy chosen once and read at several phases (a stack, a spread copy) is the
                // same read; without swing it is the table read-out's own copy and nothing else.
                {
                    const Wavetable& t = table(SAW);
                    const int top = t.getMipLevelCount() - 1;
                    bool sameRead = true, sound = true, mixes = false;
                    for (double pitch : { 1.0e-4, 0.004, 0.03, 0.26, 3.0 })
                        for (float swing : { 0.0f, 1.0e-5f, 0.002f, 0.011f, 0.07f, 0.4f, 50.0f })
                        {
                            const phaseosc::Copy c = phaseosc::copyFor(t, -pitch, swing);
                            sound = sound && c.level >= 0 && c.level <= top && c.next >= 0.0f && c.next <= 1.0f
                                          && (c.level < top || c.next == 0.0f) && c.level >= WavetableRead::mipLevel(t, pitch);
                            if (swing == 0.0f)
                                sound = sound && c.level == WavetableRead::mipLevel(t, pitch) && c.next == 0.0f;
                            mixes = mixes || c.next > 0.0f;
                            for (const Wave& w : { saw, Wave::sequence(&table(VOWL), 0.4f, 5) })
                                for (int n = 0; n < 50; ++n)
                                {
                                    const double phase = (double) n / 50.0;
                                    const phaseosc::Copy cw = phaseosc::copyFor(*w.table, pitch, swing);
                                    const float a = phaseosc::read(w, phase, pitch, 0.37f, swing), b = phaseosc::readAt(w, cw, phase, 0.37f);
                                    sameRead = sameRead && std::memcmp(&a, &b, sizeof(float)) == 0;
                                }
                        }
                    expect(sound, "a copy the table has, never a brighter one than the pitch alone asks for");
                    expect(mixes, "some of these mix two copies");
                    expect(sameRead, "copyFor + readAt is read");
                }
                expect(plain, "no swing, or one too small to matter: the plain read");
                std::vector<float> swung;
                for (int n = 0; n < 4000; ++n)
                    swung.push_back(phaseosc::read(saw, dsp::wrapPhase(n * step), step, 1.0f, 0.05f));
                expect(apart(renderOsc(saw, step, 1.0f, 4000), swung) > 0.05, "a large swing is a duller copy");
                // In numbers: the copy for a speed, and the room a steady pitch has.
                expectEquals(phaseosc::copyForSpeed(0.0019), 0.0f);
                expectWithinAbsoluteError(phaseosc::copyForSpeed(4.0 / phaseosc::SWING_BAND), 2.0f, 1.0e-5f);
                expectWithinAbsoluteError(phaseosc::copyForSpeed(6.0 / phaseosc::SWING_BAND), 2.5f, 1.0e-5f);
                for (double pitch = 1.0e-4; pitch < 0.2; pitch *= 1.07)
                    expect(phaseosc::copyForSpeed(pitch) <= (float) WavetableRead::mipLevel(table(SAW), pitch),
                           "the steady rule is the stricter one at " + juce::String(pitch, 5));
            }

            // The swing: at once up to the modulation's movement, back to nothing after it stops
            // (and then the read is the plain one again).
            {
                Osc osc;
                osc.setSampleRate(SR);
                osc.reset();
                const Wave saw = Wave::of(&table(SAW));
                const double step = 0.004;
                // 8 radians at 375 Hz (a cycle of 128 samples): 0.39 radians = 0.0625 cycles a sample
                // at the steepest. Stopped where the sine is at 0.
                for (int n = 0; n < 4096; ++n)
                    osc.next(saw, step, 8.0f * std::sin(dsp::TWO_PI_F * (float) (n % 128) / 128.0f), 1.0f);
                expect(osc.getSwing() > 0.95f * 0.0625f && osc.getSwing() < 1.05f * 0.0625f,
                       "the swing of a sine: " + juce::String(osc.getSwing(), 4) + " for 0.0625");
                const float atStop = osc.getSwing();
                for (int n = 0; n < (int) (Osc::SWING_SECONDS * SR); ++n)
                    osc.next(saw, step, 0.0f, 1.0f);
                expectWithinAbsoluteError(osc.getSwing(), atStop / 2.7183f, 0.002f, "a fiftieth of a second later: 1 / e of it");
                for (int n = 0; n < (int) (3.0 * SR); ++n)
                    osc.next(saw, step, 0.0f, 1.0f);
                expectEquals(osc.getSwing(), 0.0f);
                // The first read of a note measures nothing (there is no "before" to move from).
                osc.reset();
                osc.next(saw, step, 5.0f, 1.0f);
                expectEquals(osc.getSwing(), 0.0f);
            }
        }

        beginTest("Deep phase modulation from a note's first sample: the start folds back no more than the middle");
        {
            // The first 43 ms of a note (2048 samples under a window whose side lobes are 92 dB
            // down), carrier and modulator on whole multiples of bin 17: what lies more than four
            // bins from that grid has folded. The modulation is there from the first sample, or
            // comes up over 2 ms (a modulator's attack), or begins a tenth of a second into the note.
            const int M = 2048, g = 17;
            auto folded = [&](const std::vector<float>& x, int from)
            {
                std::vector<std::complex<double>> a((size_t) M);
                for (int i = 0; i < M; ++i)
                {
                    const double t = dsp::TWO_PI_D * i / (M - 1);
                    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
                    a[(size_t) i] = { w * x[(size_t) (from + i)], 0.0 };
                }
                fastmeasure::fft(a);
                double on = 0.0, off = 0.0;
                for (int b = 1; b < M / 2; ++b)
                    (std::min(b % g, g - b % g) <= 4 ? on : off) += std::norm(a[(size_t) b]);
                return 10.0 * std::log10(std::max(off / (on + off), 1.0e-14));
            };
            const double step = 3.0 * g / M, modStep = (double) g / M;
            for (int t : { SAW, SQR })
                for (double index : { 8.0, 20.0 })
                    for (int shape = 0; shape < 3; ++shape)
                    {
                        const int begins = shape == 2 ? 4800 : 0, rise = shape == 1 ? 96 : 0;
                        Osc osc;
                        osc.setSampleRate(SR);
                        osc.reset();
                        std::vector<float> ours, naive;
                        double phase = 0.0, mod = 0.0;
                        const int level = WavetableRead::mipLevel(table(t), step);
                        const Wave wave = Wave::of(&table(t));
                        for (int n = 0; n < begins + 12 * M; ++n)
                        {
                            const double up = n < begins ? 0.0 : (rise > 0 ? std::min(1.0, (double) (n - begins) / rise) : 1.0);
                            const float radians = (float) (up * index * std::sin(mod * dsp::TWO_PI_D));
                            ours.push_back(osc.next(wave, step, radians, 1.0f));
                            naive.push_back(WavetableRead::sample(table(t), level, 1.0f, dsp::wrapPhase(phase + (double) radians * phaseosc::CYCLES_PER_RADIAN)));
                            phase = dsp::wrapPhase(phase + step);
                            if (n >= begins)
                                mod = dsp::wrapPhase(mod + modStep);
                        }
                        const double start = folded(ours, begins), middle = folded(ours, begins + 10 * M), untouched = folded(naive, begins);
                        const juce::String what = juce::String(table(t).name) + ", index " + juce::String(index) + ", shape " + juce::String(shape)
                                                  + ": start " + juce::String(start, 1) + " dB, middle " + juce::String(middle, 1)
                                                  + ", taking no notice " + juce::String(untouched, 1);
                        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_PO_EXP", {}).isNotEmpty())
                            logMessage("  " + what);
                        expect(untouched > -45.0, "the case folds when nothing is done. " + what);
                        expect(start < -65.0 && start < untouched - 45.0, what);
                        // (A modulation that comes up is not a steady tone: some of what is off
                        // the grid there is the rise itself.)
                        if (shape != 1)
                            expect(start < middle + 10.0, what);
                    }
        }

        beginTest("Sync: the cycle starts again inside the sample, where the master's did");
        {
            // A slave at 2.5 times a master, restarted at each of the master's cycles: its phase is
            // 2.5 x the master's phase at every sample, to the fraction of the sample.
            const double masterStep = 311.0 / SR, slaveStep = 2.5 * masterStep;
            for (int which = 0; which < 2; ++which)
            {
                const Wave wave = which == 0 ? Wave::sine() : Wave::of(&table(TRI));
                Osc osc;
                osc.reset();
                double master = 0.0, worstPhase = 0.0, worstSample = 0.0;
                int restarts = 0;
                for (int n = 0; n < 20000; ++n)
                {
                    const double expectedPhase = dsp::wrapPhase(2.5 * master);
                    double d = std::abs(osc.getPhase() - expectedPhase);
                    d = std::min(d, 1.0 - d);
                    worstPhase = std::max(worstPhase, d);
                    const float got = osc.read(wave, slaveStep, 0.0f, 0.0f);
                    const float expected = phaseosc::read(wave, expectedPhase, slaveStep, 0.0f);
                    worstSample = std::max(worstSample, (double) std::abs(got - expected));

                    const double next = master + masterStep;
                    osc.advance(slaveStep);
                    if (next >= 1.0)
                    {
                        master = next - 1.0;
                        osc.sync(slaveStep, master / masterStep);       // that much of this sample had passed
                        ++restarts;
                    }
                    else
                        master = next;
                }
                expect(restarts > 100);
                expect(worstPhase < 1.0e-9, "phase off by at most " + juce::String(worstPhase, 12));
                expect(worstSample < 1.0e-4, "sample off by at most " + juce::String(worstSample, 7));
            }
            // The arguments: a start elsewhere than 0, and anything at all.
            Osc osc;
            osc.reset(0.3);
            expectWithinAbsoluteError(osc.getPhase(), 0.3, 1.0e-12);
            osc.sync(0.01, 0.25);
            expectWithinAbsoluteError(osc.getPhase(), 0.0025, 1.0e-12);
            osc.sync(0.01, 0.5, 0.75);
            expectWithinAbsoluteError(osc.getPhase(), 0.755, 1.0e-12);
            osc.sync(std::numeric_limits<double>::infinity(), 0.5);
            expectEquals(osc.getPhase(), 0.0);
            osc.sync(0.01, std::numeric_limits<double>::quiet_NaN());
            expectEquals(osc.getPhase(), 0.0);
        }

        beginTest("Wave sequence: each wave held, then crossfaded into the next");
        {
            // A list of four waves picked from four tables.
            const Wavetable list = phaseosc::makeSequence({ { &table(SIN), 0.0f }, { &table(SAW), 1.0f }, { &table(SQR), 1.0f }, { &table(VOWL), 0.5f } });
            expectEquals(list.numFrames, 4);
            const double step = 110.0 / SR;
            const std::vector<Wave> picks = { Wave::of(&table(SIN)), Wave::of(&table(SAW)), Wave::of(&table(SQR)), Wave::of(&table(VOWL)) };
            const float pickPosition[4] = { 0.0f, 1.0f, 1.0f, 0.5f };
            const Wave quarter = Wave::sequence(&list, 0.25f);
            expectEquals(quarter.steps, 4);
            std::vector<std::vector<float>> waves;
            for (int k = 0; k < 4; ++k)
            {
                waves.push_back(renderOsc(quarter, step, (float) k / 3.0f, 4800));
                // Wave k of the list is the wave that was picked (the same harmonics; the copies
                // are made by another transform, so not the same bits).
                expect(apart(waves.back(), renderOsc(picks[(size_t) k], step, pickPosition[k], 4800)) < 0.02,
                       "wave " + juce::String(k) + ": " + juce::String(apart(waves.back(), renderOsc(picks[(size_t) k], step, pickPosition[k], 4800)), 4));
            }
            expect(apart(waves[0], waves[1]) > 0.3 && apart(waves[1], waves[2]) > 0.3 && apart(waves[2], waves[3]) > 0.3, "four different waves");

            // XFAD 25 %: through the first three quarters of a step the wave is held (the same
            // samples); half way through the last quarter it is half the next wave.
            for (int k = 0; k < 3; ++k)
            {
                const float at = (float) k / 3.0f;
                expect(same(renderOsc(quarter, step, at + 0.74f / 3.0f, 4800), renderOsc(quarter, step, at + 0.001f, 4800)), "held, step " + juce::String(k));
                auto half = renderOsc(quarter, step, at + 0.875f / 3.0f, 4800);
                std::vector<float> mean(half.size());
                for (size_t n = 0; n < mean.size(); ++n)
                    mean[n] = 0.5f * (waves[(size_t) k][n] + waves[(size_t) k + 1][n]);
                expect(apart(half, mean) < 0.01, "half way through the crossfade, step " + juce::String(k) + ": " + juce::String(apart(half, mean), 4));
            }
            // XFAD 100 %: the whole step is the crossfade.
            {
                auto third = renderOsc(Wave::sequence(&list, 1.0f), step, (1.0f + 1.0f / 3.0f) / 3.0f, 4800);
                std::vector<float> mix(third.size());
                for (size_t n = 0; n < mix.size(); ++n)
                    mix[n] = waves[1][n] + (waves[2][n] - waves[1][n]) / 3.0f;
                expect(apart(third, mix) < 0.01, juce::String(apart(third, mix), 4));
            }
            // The place in a sequence, in numbers.
            {
                auto p = phaseosc::sequencePlace(0.5f, 5, 0.5f);        // 2.0 of 4 steps: wave 2, held
                expect(p.wave == 2 && p.into == 0.0f);
                p = phaseosc::sequencePlace(0.6875f, 5, 0.5f);          // 2.75: half way through the fade
                expect(p.wave == 2);
                expectWithinAbsoluteError(p.into, 0.5f, 1.0e-4f);
                p = phaseosc::sequencePlace(1.0f, 5, 0.5f);             // the end: all of the last wave
                expect(p.wave == 3 && p.into == 1.0f);
                p = phaseosc::sequencePlace(0.2f, 2, 0.0f);             // no crossfade asked for: the shortest
                expect(p.wave == 0 && p.into == 0.0f);
                p = phaseosc::sequencePlace(1.0f - 0.5f * phaseosc::MIN_FADE, 2, 0.0f);
                expectWithinAbsoluteError(p.into, 0.5f, 1.0e-3f);
            }
            // Any table read as a sequence: five of the vowel table's waves, each one of its frames.
            {
                const Wave five = Wave::sequence(&table(VOWL), 0.1f, 5);
                for (int k = 0; k < 5; ++k)
                    expect(apart(renderOsc(five, step, (float) k / 4.0f + (k < 4 ? 0.1f : 0.0f), 2400),
                                 renderOsc(Wave::of(&table(VOWL)), step, (float) k / 4.0f, 2400)) < 1.0e-3, "vowel " + juce::String(k));
            }
            // Even the shortest crossfade is one: a position moving through a boundary in a
            // tenth of a second never steps further in a sample than the waves themselves do.
            {
                Osc osc;
                osc.reset();
                const Wave hard = Wave::sequence(&list, 0.0f);
                float before = 0.0f, steepest = 0.0f, steepestStill = 0.0f;
                for (int n = 0; n < 4800; ++n)
                {
                    const float s = osc.next(hard, step, 0.0f, (1.0f + 0.9f + 0.2f * (float) n / 4800.0f) / 3.0f);
                    if (n > 0) steepest = std::max(steepest, std::abs(s - before));
                    before = s;
                }
                for (auto& w : waves)
                    for (size_t n = 1; n < w.size(); ++n)
                        steepestStill = std::max(steepestStill, std::abs(w[n] - w[n - 1]));
                expect(steepest <= 1.05f * steepestStill, juce::String(steepest, 4) + " against " + juce::String(steepestStill, 4));
            }
            // Nothing picked: a table that can still be read.
            expectEquals(phaseosc::makeSequence({}).numFrames, 1);
            expect(finiteAndUnder(renderOsc(Wave::sequence(nullptr, 0.5f), step, 0.5f, 100), 1.0f));
        }

        beginTest("The cycles of a sample: the place in the recording is the position, the pitch is the oscillator's");
        {
            // Two seconds: a 200 Hz tone with a second and a third harmonic, then a 300 Hz sine.
            const double fileRate = 44100.0;
            std::vector<float> recording((size_t) (2.0 * fileRate));
            for (size_t n = 0; n < recording.size(); ++n)
            {
                const double t = (double) n / fileRate;
                recording[n] = n < recording.size() / 2
                    ? (float) (0.4 * (std::sin(dsp::TWO_PI_D * 200.0 * t) + 0.5 * std::sin(dsp::TWO_PI_D * 400.0 * t) + 0.25 * std::sin(dsp::TWO_PI_D * 600.0 * t)))
                    : (float) (0.5 * std::sin(dsp::TWO_PI_D * 300.0 * t));
            }
            const Wavetable cycles = phaseosc::cyclesFromSample(recording.data(), nullptr, (int) recording.size(), fileRate);
            expectEquals(cycles.numFrames, phaseosc::CYCLE_FRAMES);
            const Wave wave = Wave::of(&cycles);
            // Played at 110 Hz, whatever the recording's pitch was: one period a cycle.
            const auto early = renderOsc(wave, 110.0 / SR, 0.2f, 48000), late = renderOsc(wave, 110.0 / SR, 0.8f, 48000);
            expect(harmonic(early, 1) > 0.2, "the fundamental is at the oscillator's pitch: " + juce::String(harmonic(early, 1), 3));
            expectWithinAbsoluteError(harmonic(early, 2) / harmonic(early, 1), 0.5, 0.03);
            expectWithinAbsoluteError(harmonic(early, 3) / harmonic(early, 1), 0.25, 0.03);
            expect(harmonic(early, 4) < 0.03 * harmonic(early, 1));
            expect(harmonic(late, 1) > 0.2);
            expect(harmonic(late, 2) < 0.03 * harmonic(late, 1) && harmonic(late, 3) < 0.03 * harmonic(late, 1), "the second half is a sine");
            // Two channels are read as their mean; nothing to read gives a table all the same.
            std::vector<float> silent(recording.size(), 0.0f);
            const Wavetable half = phaseosc::cyclesFromSample(recording.data(), silent.data(), (int) recording.size(), fileRate);
            expect(apart(renderOsc(Wave::of(&half), 110.0 / SR, 0.2f, 4800), early) < 0.02, "left and a silent right: the same wave (each frame is brought to full level)");
            const Wavetable fromNothing = phaseosc::cyclesFromSample(nullptr, nullptr, 0, fileRate);
            expect(finiteAndUnder(renderOsc(Wave::of(&fromNothing), 0.01, 0.5f, 100), 1.01f));
            std::vector<float> bad(4000, std::numeric_limits<float>::quiet_NaN());
            const Wavetable fromBad = phaseosc::cyclesFromSample(bad.data(), nullptr, (int) bad.size(), fileRate);
            expect(finiteAndUnder(renderOsc(Wave::of(&fromBad), 0.01, 0.5f, 1000), 1.01f), "a recording that is not a number");

            // The bank: made once, the same table for everyone, nothing for a sample that is not there.
            auto& bank = SampleBank::get();
            const int tone = bank.indexOf("TONE");
            expect(tone >= 0);
            const Wavetable* a = phaseosc::sampleCycles(tone);
            expect(a != nullptr && a == phaseosc::sampleCycles(tone));
            if (a != nullptr)
            {
                expectEquals(a->numFrames, phaseosc::CYCLE_FRAMES);
                // The bank's tone is twelve harmonics falling away: as an oscillator it has them too.
                const auto x = renderOsc(Wave::of(a), 110.0 / SR, 0.5f, 48000);
                expect(harmonic(x, 1) > 0.2 && harmonic(x, 2) > 0.1 * harmonic(x, 1) && harmonic(x, 2) < harmonic(x, 1),
                       juce::String(harmonic(x, 1), 3) + " " + juce::String(harmonic(x, 2), 3));
                expect(harmonic(x, 20) < 0.02 * harmonic(x, 1), "and nothing above the twelfth");
            }
            expect(phaseosc::sampleCycles(-1) == nullptr && phaseosc::sampleCycles(100000) == nullptr);
            // The WAVE list: the sine, the cycles, then the built-in tables by name.
            expectEquals(juce::String(phaseosc::waveLabels()[0]), juce::String("SINE"));
            expectEquals(juce::String(phaseosc::waveLabels()[1]), juce::String("CYCL"));
            expectEquals(juce::String(phaseosc::waveLabels()[2]), juce::String("SAW"));
            expectEquals(juce::String(phaseosc::waveLabels()[phaseosc::waveCount() - 1]), juce::String("COMB"));
            expectEquals(phaseosc::waveCount(), 2 + WavetableFactory::builtInCount());
            expect(phaseosc::waveTable(0, a) == nullptr && phaseosc::waveTable(1, a) == a && phaseosc::waveTable(2, a) == &table(SAW)
                   && phaseosc::waveTable(-5, a) == nullptr && phaseosc::waveTable(9999, a) == nullptr);
        }

        beginTest("Anything at all as input: finite, and no louder than the wave");
        {
            const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
            const std::vector<double> nasty = { nan, inf, -inf, 1.0e30, -1.0e30, 1.0e-30, 0.0, -1.0, 1.0, 0.5, 3.0e9, 0.01 };
            const Wavetable list = phaseosc::makeSequence({ { &table(SAW), 1.0f }, { &table(SQR), 1.0f }, { &table(BELL), 1.0f } });
            std::vector<Wave> waves = { Wave::sine(), Wave::of(&table(SAW)), Wave::of(&table(BELL), WavetableInterpMode::Classic),
                                        Wave::of(&table(VOWL), WavetableInterpMode::Vintage), Wave::of(&table(SQR), WavetableInterpMode::Stepped),
                                        Wave::of(&table(SIN)), Wave::sequence(&list, 0.3f), Wave::sequence(&table(VOWL), (float) nan, 7) };
            { Wave odd = Wave::sequence(&table(VOWL), -5.0f, 1000000); waves.push_back(odd); }
            { Wave odd = Wave::of(&table(VOWL)); odd.steps = -3; odd.fade = (float) inf; waves.push_back(odd); }
            bool ok = true;
            long reads = 0;
            float loudest = 0.0f;
            for (auto& w : waves)
                for (double step : nasty)
                    for (double radians : nasty)
                        for (double position : nasty)
                        {
                            Osc osc;
                            osc.setSampleRate(step);                // (and a sample rate that makes no sense)
                            osc.reset(radians);
                            for (int n = 0; n < 6; ++n, ++reads)
                            {
                                const float a = osc.next(w, step, (float) radians, (float) position);
                                const float b = osc.readCycles(w, step, radians, (float) position);
                                const float c = phaseosc::read(w, dsp::wrapPhase(step * n), radians, (float) position, (float) step);
                                osc.sync(step, position, radians);
                                const float d = osc.read(w, position, (float) step, (float) radians);
                                for (float v : { a, b, c, d })
                                {
                                    ok = ok && std::isfinite(v);
                                    loudest = std::max(loudest, std::abs(v));
                                }
                                ok = ok && osc.getPhase() >= 0.0 && osc.getPhase() < 1.0 && std::isfinite(osc.getSwing());
                            }
                        }
            expect(ok, juce::String((juce::int64) reads) + " reads");
            expect(loudest <= 1.3f, "loudest " + juce::String(loudest, 3));       // (a band-limited square overshoots its corners)

            // The FM engine with tables in every operator, at the ends of everything.
            for (double sr : { 22050.0, 96000.0, 192000.0 })
                for (int block : { 1, 4096 })
                {
                    F e;
                    e.prepareToPlay(sr, block);
                    for (int op = 0; op < 6; ++op)
                    {
                        e.setParameter(op * 10 + 0, op % 2 == 0 ? 16.0f : 0.5f);
                        e.setParameter(op * 10 + 1, 1.0f);
                        e.setParameter(op * 10 + 3, 1.0f);
                        e.setParameter(op * 10 + F::OP_WAVE, waveIndex(op == 0 ? SAW : (op == 1 ? BELL : VOWL)));
                        e.setParameter(op * 10 + F::OP_POSITION, 1.0f);
                    }
                    e.setParameter(50 + F::OP_WAVE, (float) phaseosc::WAVE_CYCLES);
                    e.setParameter(F::SeqSteps, 15.0f);
                    e.setParameter(F::SeqFade, 0.0f);
                    e.setParameter(F::PositionEnv, -1.0f);
                    e.setParameter(F::TuneParam, 24.0f);
                    e.setParameter(F::BendRangeParam, 24.0f);
                    e.pitchBend(1.0f);
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 40.0f);
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 1.0f);
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) destIndex(e, "POS1"));
                    e.setModWheel(1.0f);
                    std::vector<float> out;
                    for (int note : { 0, 127, 60 })
                    {
                        e.noteOn(note, 1.0f, -1);
                        e.aftertouch(note, 1.0f);
                        run(e, out, block == 1 ? 600 : 8192, block);
                    }
                    expect(finiteAndUnder(out, 12.0f), juce::String(sr) + " Hz, blocks of " + juce::String(block));
                    expect(rms(out) > 1.0e-4, "and it sounds");
                }

            // A file may hold anything in the new cells.
            {
                F e;
                steady(e);
                e.setParameter(F::Op1Level, 1.0f);
                for (float value : { (float) nan, (float) inf, (float) -inf, 1.0e30f, -1.0e30f, 1.0e9f, -7.0f })
                {
                    for (int id : { (int) F::Op1Ratio + F::OP_WAVE, (int) F::Op1Ratio + F::OP_POSITION, (int) F::CycleSample,
                                    (int) F::SeqSteps, (int) F::SeqFade, (int) F::PositionEnv })
                    {
                        e.setParameter(id, value);
                        const float held = e.getParameter(id);
                        const bool wave = id == F::Op1Ratio + F::OP_WAVE;
                        const float top = wave ? (float) (phaseosc::waveCount() - 1)
                                               : (id == F::CycleSample ? (float) (SampleBank::get().size() - 1) : (id == F::SeqSteps ? 15.0f : 1.0f));
                        expect(std::isfinite(held) && held >= (id == F::PositionEnv ? -1.0f : 0.0f) && held <= top,
                               "id " + juce::String(id) + " given " + juce::String(value) + " holds " + juce::String(held));
                    }
                    std::vector<float> out;
                    e.noteOn(60, 0.9f, -1);
                    run(e, out, 512);
                    expect(finiteAndUnder(out, 4.0f));
                }
            }
        }

        beginTest("FM engine: the WAVE and WPOS pages, eight cells each, set and read back");
        {
            F e;
            const auto pages = e.getParamPages();
            const ParamPage* wavePage = nullptr;
            const ParamPage* placePage = nullptr;
            int waveAt = -1;
            for (size_t i = 0; i < pages.size(); ++i)
            {
                if (pages[i].name == "WAVE") { wavePage = &pages[i]; waveAt = (int) i; }
                if (pages[i].name == "WPOS") placePage = &pages[i];
            }
            expect(wavePage != nullptr && placePage != nullptr);
            if (wavePage == nullptr || placePage == nullptr)
                return;
            expectEquals(waveAt, 2, "after FMMIX and RATIO");
            expectEquals(pages[3].name, juce::String("WPOS"));
            expectEquals(pages[4].name, juce::String("OP1"));
            static const char* const waveLabels[8] = { "WAV1", "WAV2", "WAV3", "WAV4", "WAV5", "WAV6", "SMPL", "WSEQ" };
            static const char* const placeLabels[8] = { "POS1", "POS2", "POS3", "POS4", "POS5", "POS6", "XFAD", "PENV" };
            static const int waveIds[8] = { 8, 18, 28, 38, 48, 58, 66, 67 };
            static const int placeIds[8] = { 9, 19, 29, 39, 49, 59, 68, 69 };
            for (int c = 0; c < 8; ++c)
            {
                const auto& w = wavePage->params[(size_t) c];
                const auto& p = placePage->params[(size_t) c];
                expect(w.isValid() && p.isValid(), "eight working cells");
                expectEquals(juce::String(w.label), juce::String(waveLabels[c]));
                expectEquals(juce::String(p.label), juce::String(placeLabels[c]));
                expectEquals(w.id, waveIds[c]);
                expectEquals(p.id, placeIds[c]);
                expect(w.kind == ParamKind::Choice && w.choices != nullptr && w.numChoices == (int) w.maxVal + 1);
                expect(p.kind == ParamKind::Linear && p.maxVal == 1.0f && p.minVal == (c == 7 ? -1.0f : 0.0f));
                // What a new engine holds is what the cell says a new sound has.
                expectEquals(e.getParameter(w.id), w.defaultVal, w.label);
                expectEquals(e.getParameter(p.id), p.defaultVal, p.label);
            }
            expectEquals(wavePage->params[0].numChoices, phaseosc::waveCount());
            expectEquals(juce::String(wavePage->params[0].choices[0]), juce::String("SINE"));
            expectEquals(wavePage->params[0].defaultVal, 0.0f, "the sine is the default");
            expectEquals(wavePage->params[6].numChoices, SampleBank::get().size());
            expectEquals(juce::String(wavePage->params[6].choices[(size_t) wavePage->params[6].defaultVal]), juce::String("TONE"));
            expectEquals(wavePage->params[7].numChoices, 16);
            expectEquals(juce::String(wavePage->params[7].choices[0]), juce::String("SCAN"));
            expect(e.isRebuildParam(F::CycleSample) && ! e.isRebuildParam(F::SeqFade) && ! e.isRebuildParam(9));
            expectEquals(e.getParameterName(18), juce::String("Op2 Wave"));
            expectEquals(e.getParameterName(59), juce::String("Op6 Position"));
            for (int id : { 8, 58, 66, 67 }) { e.setParameter(id, 3.0f); expectEquals(e.getParameter(id), 3.0f); }
            for (int id : { 9, 59, 68, 69 }) { e.setParameter(id, 0.25f); expectEquals(e.getParameter(id), 0.25f); }
            // The positions are LFO and mod-slot destinations: the last group of the list.
            const int first = destIndex(e, "POS1");
            expect(first > 0);
            expectEquals(destIndex(e, "POS6"), first + 5);
            expectEquals(first, (int) FMVoice::LfoPosition);     // (the envelope times and the phases follow them: U4-FM)
            expectEquals(F::lfoDests()[first + 2].id, 29);
        }

        beginTest("FM engine: POS under a note is moved to; an LFO, the envelope (PENV) and WSEQ / XFAD act on a table and leave a sine alone");
        {
            auto vowelCarrier = [&](F& e)
            {
                steady(e);
                e.setParameter(F::Op1Level, 1.0f);
                e.setParameter(F::Op1Ratio + F::OP_WAVE, waveIndex(VOWL));
            };
            auto note = [&](F& e, int samples = 9600)
            {
                std::vector<float> out;
                e.noteOn(45, 1.0f, -1);
                run(e, out, samples);
                return out;
            };
            // The same note with POS1 at 0, at 1, and turned from 0 to 1 while it sounds.
            F low, high, turned;
            for (F* e : { &low, &high, &turned })
                vowelCarrier(*e);
            high.setParameter(F::Op1Ratio + F::OP_POSITION, 1.0f);
            auto a = note(low), b = note(high), c = note(turned);
            expect(apart(a, b) > 0.5, "POS1 is heard: " + juce::String(apart(a, b), 3));
            expect(same(a, c));
            turned.setParameter(F::Op1Ratio + F::OP_POSITION, 1.0f);
            for (F* e : { &low, &high, &turned })
            {
                (e == &low ? a : (e == &high ? b : c)).clear();
                run(*e, e == &low ? a : (e == &high ? b : c), 2400);
            }
            // In the first eight samples after the turn the position has come 8 / 144 of the way:
            // where the two held sounds are furthest apart the turned one is still at the old one.
            size_t at = 0;
            for (size_t n = 0; n < 8; ++n)
                if (std::abs(a[n] - b[n]) > std::abs(a[at] - b[at]))
                    at = n;
            expect(std::abs(a[at] - b[at]) > 0.02f, "the two held sounds differ there: " + juce::String(std::abs(a[at] - b[at]), 4));
            expect(std::abs(c[at] - a[at]) < 0.25f * std::abs(b[at] - a[at]),
                   "moved to, not jumped to: " + juce::String(std::abs(c[at] - a[at]), 4) + " of " + juce::String(std::abs(b[at] - a[at]), 4));
            // ... and after 3 ms it has arrived: the last tenth of the render is the high one's.
            expect(apart(std::vector<float>(c.end() - 240, c.end()), std::vector<float>(b.end() - 240, b.end())) < 1.0e-3);

            // An LFO on POS1.
            {
                F plain, moved;
                for (F* e : { &plain, &moved })
                {
                    vowelCarrier(*e);
                    e->setParameter(F::Op1Ratio + F::OP_POSITION, 0.5f);
                }
                moved.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 6.0f);
                moved.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.8f);
                moved.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) destIndex(moved, "POS1"));
                const auto x = note(plain, 24000), y = note(moved, 24000);
                expect(apart(x, y) > 0.2, "the LFO moves the position: " + juce::String(apart(x, y), 3));
                // The level stays (a position is not a volume): RMS within 3 dB.
                expect(rms(y) > 0.7 * rms(x) && rms(y) < 1.4 * rms(x), juce::String(rms(x), 3) + " " + juce::String(rms(y), 3));
            }
            // PENV: the operator's envelope carries the position. A slow attack sweeps the table.
            {
                F plain, swept;
                for (F* e : { &plain, &swept })
                {
                    vowelCarrier(*e);
                    e->setParameter(F::Op1Attack, 0.15f);
                }
                swept.setParameter(F::PositionEnv, 1.0f);
                F top;
                vowelCarrier(top);
                top.setParameter(F::Op1Attack, 0.15f);
                top.setParameter(F::Op1Ratio + F::OP_POSITION, 1.0f);
                const auto x = note(plain, 14400), y = note(swept, 14400), z = note(top, 14400);
                auto part = [](const std::vector<float>& v, int from, int n) { return std::vector<float>(v.begin() + from, v.begin() + from + n); };
                // In the first millisecond (the envelope near 0) the two are the same wave; once
                // the envelope is up the swept one is at the other end of the table.
                expect(apart(part(x, 0, 48), part(y, 0, 48)) < 0.1, juce::String(apart(part(x, 0, 48), part(y, 0, 48)), 3));
                expect(apart(part(x, 12000, 2400), part(y, 12000, 2400)) > 0.5);
                expect(apart(part(y, 12000, 2400), part(z, 12000, 2400)) < 0.01,
                       "PENV + 100 with the envelope up is POS at its top: " + juce::String(apart(part(y, 12000, 2400), part(z, 12000, 2400)), 4));
            }
            // WSEQ and XFAD: four of the table's waves. Inside a step's hold the position does
            // nothing; with SCAN it does; XFAD decides where the hold ends.
            {
                auto make = [&](F& e, float steps, float fade, float position)
                {
                    vowelCarrier(e);
                    e.setParameter(F::SeqSteps, steps);
                    e.setParameter(F::SeqFade, fade);
                    e.setParameter(F::Op1Ratio + F::OP_POSITION, position);
                };
                F scan1, scan2, held1, held2, faded;
                make(scan1, 0.0f, 0.2f, 0.05f);
                make(scan2, 0.0f, 0.2f, 0.2f);
                make(held1, 3.0f, 0.2f, 0.05f);         // 4 waves: a step is a third of the table
                make(held2, 3.0f, 0.2f, 0.2f);
                make(faded, 3.0f, 1.0f, 0.2f);
                const auto s1 = note(scan1), s2 = note(scan2), h1 = note(held1), h2 = note(held2), f = note(faded);
                expect(apart(s1, s2) > 0.05, "SCAN: the position is heard: " + juce::String(apart(s1, s2), 3));
                expect(same(h1, h2), "a sequence: the same wave through the hold");
                expect(apart(h2, f) > 0.05, "XFAD at 100 has begun to cross where XFAD at 20 still holds: " + juce::String(apart(h2, f), 3));
            }
            // WAVE turned while the note sounds: the operator reads the new wave from there on
            // (no new note), and the sine again when it is turned back.
            {
                F e;
                steady(e);
                e.setParameter(F::Op1Level, 1.0f);
                e.noteOn(45, 1.0f, -1);
                std::vector<float> lead, asSine, asSaw, back;
                run(e, lead, 4800);
                run(e, asSine, 48000);
                e.setParameter(F::Op1Ratio + F::OP_WAVE, waveIndex(SAW));
                e.setParameter(F::Op1Ratio + F::OP_POSITION, 1.0f);
                run(e, lead, 4800);
                run(e, asSaw, 48000);
                e.setParameter(F::Op1Ratio + F::OP_WAVE, 0.0f);
                run(e, lead, 4800);
                run(e, back, 48000);
                expect(harmonic(asSine, 1) > 0.05 && harmonic(asSine, 2) < 0.001 * harmonic(asSine, 1), "a sine first");
                expect(harmonic(asSaw, 2) > 0.3 * harmonic(asSaw, 1) && harmonic(asSaw, 3) > 0.2 * harmonic(asSaw, 1),
                       "then the saw, under the same note: 2nd " + juce::String(harmonic(asSaw, 2) / harmonic(asSaw, 1), 3));
                expect(harmonic(back, 1) > 0.05 && harmonic(back, 2) < 0.001 * harmonic(back, 1), "and the sine again");
            }
            // All of it on a sine operator: the sine has no position. The same samples.
            {
                F plain, busy;
                for (F* e : { &plain, &busy })
                {
                    steady(*e);
                    e->setParameter(F::Op1Level, 1.0f);
                    e->setParameter(F::Op2Level, 0.7f);
                    e->setParameter(F::AlgorithmParam, 6.0f);
                }
                busy.setParameter(F::Op1Ratio + F::OP_POSITION, 0.8f);
                busy.setParameter(F::Op2Ratio + F::OP_POSITION, 0.3f);
                busy.setParameter(F::SeqSteps, 5.0f);
                busy.setParameter(F::SeqFade, 0.1f);
                busy.setParameter(F::PositionEnv, 0.9f);
                busy.setParameter(F::CycleSample, 0.0f);
                expect(same(note(plain), note(busy)));
            }
        }

        beginTest("The cycles of a bank sample are made when the sample is chosen, never when an operator asks for them");
        {
            // A WAVE cell can be locked to CYCL on a step, and a lock is applied on the audio
            // thread: by then the table has to be there. A sample nobody has cut in this run:
            auto& bank = SampleBank::get();
            int fresh = -1;
            for (int i = bank.size() - 1; i >= 0 && fresh < 0; --i)
                if (i != bank.defaultIndex() && phaseosc::builtSampleCycles(i) == nullptr
                    && phaseosc::builtSampleCycles(i, 128, false) == nullptr)
                    fresh = i;
            expect(fresh >= 0, "the bank has a sample not yet cut");
            if (fresh < 0)
                return;
            F e;
            steady(e);
            e.setParameter(F::Op1Level, 1.0f);
            expect(phaseosc::builtSampleCycles(bank.defaultIndex()) != nullptr, "a new engine's SMPL (TONE) is cut already");
            e.setParameter(F::CycleSample, (float) fresh);          // (no operator reads CYCL)
            const Wavetable* made = phaseosc::builtSampleCycles(fresh);
            expect(made != nullptr, "SMPL makes the cycles");
            if (made == nullptr)
                return;
            expect(phaseosc::sampleCycles(fresh) == made, "once, and the same table for everyone after");
            expectEquals(made->numFrames, phaseosc::CYCLE_FRAMES);
            e.setParameter(F::Op1Ratio + F::OP_WAVE, (float) phaseosc::WAVE_CYCLES);
            std::vector<float> out;
            e.noteOn(45, 1.0f, -1);
            run(e, out, 4800);
            expect(rms(out) > 1.0e-3 && finiteAndUnder(out, 4.0f), "and the operator reads them: " + juce::String(rms(out), 4));

            // The other ways of cutting a recording (the wavetable engine's FRMS) are tables of
            // their own, made when asked for.
            expect(phaseosc::builtSampleCycles(fresh, 128, false) == nullptr);
            const Wavetable* pieces = phaseosc::sampleCycles(fresh, 128, false);
            expect(pieces != nullptr && pieces != made && phaseosc::builtSampleCycles(fresh, 128, false) == pieces);
            if (pieces != nullptr)
                expectEquals(pieces->numFrames, 128);
            expect(phaseosc::builtSampleCycles(fresh) == made, "the first is still there");
            const Wavetable* wide = phaseosc::sampleCycles(bank.defaultIndex(), 256, true);
            expect(wide != nullptr && wide->numFrames == 256 && wide != phaseosc::sampleCycles(bank.defaultIndex(), 256, false));
            for (int index : { -1, -1000000, SampleBank::MAX_ENTRIES, 1000000, bank.size() })
                expect(phaseosc::sampleCycles(index) == nullptr && phaseosc::builtSampleCycles(index) == nullptr, juce::String(index));
        }

        beginTest("FM engine: the cycles of a bank sample as an operator's wave");
        {
            auto& bank = SampleBank::get();
            auto make = [&](F& e, float wave, const char* sample, float position)
            {
                steady(e);
                e.setParameter(F::Op1Level, 1.0f);
                e.setParameter(F::CycleSample, (float) bank.indexOf(sample));
                e.setParameter(F::Op1Ratio + F::OP_WAVE, wave);
                e.setParameter(F::Op1Ratio + F::OP_POSITION, position);
            };
            F sine, tone, toneLate, beat, late;
            make(sine, 0.0f, "TONE", 0.3f);
            make(tone, (float) phaseosc::WAVE_CYCLES, "TONE", 0.3f);
            make(toneLate, (float) phaseosc::WAVE_CYCLES, "TONE", 0.9f);
            make(beat, (float) phaseosc::WAVE_CYCLES, "BEAT", 0.3f);
            // (the sample chosen after the wave: the operator follows)
            make(late, (float) phaseosc::WAVE_CYCLES, "KICK", 0.3f);
            late.setParameter(F::CycleSample, (float) bank.indexOf("BEAT"));
            const auto s = second(sine), t = second(tone), u = second(toneLate), b = second(beat), l = second(late);
            expect(rms(t) > 0.05 && rms(b) > 0.02, "they sound: " + juce::String(rms(t), 3) + " " + juce::String(rms(b), 3));
            expect(harmonic(t, 1) > 0.05 && harmonic(t, 2) > 0.1 * harmonic(t, 1), "the tone's cycle: a fundamental at the key and its harmonics");
            expect(harmonic(s, 2) < 0.001 * harmonic(s, 1));
            expect(apart(t, b) > 0.3, "another sample, another wave: " + juce::String(apart(t, b), 3));
            expect(apart(t, u) > 0.02, "the position is the place in the sample: " + juce::String(apart(t, u), 3));
            expect(same(b, l), "SMPL set after WAVE");
        }
    }
};

static PhaseOscTests phaseOscTests;
