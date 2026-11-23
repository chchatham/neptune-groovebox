#pragma once
#include <juce_core/juce_core.h>
#include <chrono>
#include <complex>
#include <limits>
#include <vector>
#include "../src/engine/AudioEngine.h"
#include "../src/dsp/effects/Vocoder.h"
#include "fast_measure.h"

// The vocoder insert, VOC (spec-track-input.md, 1.1; spec-unify.md U2, fusion 3b): the track's sound
// is the carrier, the track's input the modulator. Mode BAND is the channel vocoder, mode TUNE the
// spectral resonator (the carrier through a tuned body whose partials take the modulator's band
// levels). The tests hand the input over themselves (setInput before each block), as the
// AudioEngine does.
class VocoderTests : public juce::UnitTest
{
public:
    VocoderTests() : UnitTest("Vocoder") {}

    using Signal = std::vector<float>;

    struct Stereo { Signal l, r; };

    static Signal whiteNoise(int n, float peak, uint32_t seed)
    {
        Signal x((size_t) n);
        for (auto& v : x)
        {
            seed = seed * 1664525u + 1013904223u;
            v = peak * (float) (int32_t) seed * (1.0f / 2147483648.0f);
        }
        return x;
    }

    static Signal sine(int n, double hz, double sr, float amp, int soundingSamples = -1)
    {
        Signal x((size_t) n, 0.0f);
        const int until = soundingSamples < 0 ? n : std::min(n, soundingSamples);
        for (int i = 0; i < until; ++i)
            x[(size_t) i] = amp * (float) std::sin(6.283185307179586 * hz * (double) i / sr);
        return x;
    }

    static float rms(const Signal& x, int from, int n)
    {
        double sum = 0.0;
        int count = 0;
        for (int i = std::max(0, from); i < std::min((int) x.size(), from + n); ++i, ++count)
            sum += (double) x[(size_t) i] * x[(size_t) i];
        return count > 0 ? (float) std::sqrt(sum / count) : 0.0f;
    }

    // The carrier through the effect, block by block. `modL` null = no input at all; the input is
    // copied to a scratch buffer that is overwritten after every call (the pointer is only good for
    // the call). `onBlock` is called before each block with the sample position.
    template <typename Fn>
    static Stereo run(Vocoder& fx, const Signal& carL, const Signal& carR, const Signal* modL, const Signal* modR,
                      int block, Fn&& onBlock)
    {
        Stereo out { carL, carR };
        const int n = (int) carL.size();
        Signal scratchL((size_t) block), scratchR((size_t) block);
        for (int pos = 0; pos < n; pos += block)
        {
            const int len = std::min(block, n - pos);
            onBlock(pos);
            if (modL != nullptr)
            {
                for (int i = 0; i < len; ++i)
                {
                    scratchL[(size_t) i] = (*modL)[(size_t) (pos + i)];
                    scratchR[(size_t) i] = (modR != nullptr ? *modR : *modL)[(size_t) (pos + i)];
                }
                fx.setInput(scratchL.data(), scratchR.data(), len);
            }
            else
                fx.setInput(nullptr, nullptr, 0);
            fx.processBlock(out.l.data() + pos, out.r.data() + pos, len);
            std::fill(scratchL.begin(), scratchL.end(), 3.0f);      // stale: must never be heard
            std::fill(scratchR.begin(), scratchR.end(), -3.0f);
        }
        return out;
    }

    static Stereo run(Vocoder& fx, const Signal& car, const Signal* mod, int block = 256)
    {
        return run(fx, car, car, mod, nullptr, block, [] (int) {});
    }

    // Power spectrum of x[from, from + size), Hann window (size a power of two).
    static std::vector<double> spectrum(const Signal& x, int from, int size)
    {
        std::vector<std::complex<double>> a((size_t) size);
        for (int i = 0; i < size; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(6.283185307179586 * i / size);
            const int at = from + i;
            a[(size_t) i] = at >= 0 && at < (int) x.size() ? w * (double) x[(size_t) at] : 0.0;
        }
        fastmeasure::fft(a);
        std::vector<double> p((size_t) size / 2);
        for (size_t k = 0; k < p.size(); ++k) p[k] = std::norm(a[k]);
        return p;
    }

    // The share of the power between lo and hi Hz, 0..1.
    static double share(const std::vector<double>& p, double sr, double lo, double hi)
    {
        double in = 0.0, all = 0.0;
        const double binHz = sr / (double) (2 * p.size());
        for (size_t k = 1; k < p.size(); ++k)
        {
            all += p[k];
            const double hz = (double) k * binHz;
            if (hz >= lo && hz <= hi) in += p[k];
        }
        return all > 0.0 ? in / all : 0.0;
    }

    // The analysis band that holds most of the power (each band: half a band either side).
    static int loudestBand(const std::vector<double>& p, double sr)
    {
        const double half = std::sqrt((double) Vocoder::bandHz(1) / (double) Vocoder::bandHz(0));
        int best = -1;
        double bestShare = 0.0;
        for (int b = 0; b < Vocoder::NUM_BANDS; ++b)
        {
            const double hz = (double) Vocoder::bandHz(b);
            const double s = share(p, sr, hz / half, hz * half);
            if (s > bestShare) { bestShare = s; best = b; }
        }
        return best;
    }

    // The frequency of the strongest bin between lo and hi Hz.
    static double peakHz(const std::vector<double>& p, double sr, double lo, double hi)
    {
        const double binHz = sr / (double) (2 * p.size());
        size_t best = 0;
        for (size_t k = 1; k < p.size(); ++k)
        {
            const double hz = (double) k * binHz;
            if (hz >= lo && hz <= hi && (best == 0 || p[k] > p[best])) best = k;
        }
        return (double) best * binHz;
    }

    // The middle of the power between lo and hi Hz (a partial driven by noise under levels that
    // are noise is a lump some 3 Hz wide, and its strongest bin is anywhere in it).
    static double centreHz(const std::vector<double>& p, double sr, double lo, double hi)
    {
        const double binHz = sr / (double) (2 * p.size());
        double sum = 0.0, weighted = 0.0;
        for (size_t k = 1; k < p.size(); ++k)
        {
            const double hz = (double) k * binHz;
            if (hz >= lo && hz <= hi) { sum += p[k]; weighted += p[k] * hz; }
        }
        return sum > 0.0 ? weighted / sum : 0.0;
    }

    // The largest step from one sample to the next in x[from, from + n).
    static float largestStep(const Signal& x, int from, int n)
    {
        float worst = 0.0f;
        for (int i = std::max(1, from); i < std::min((int) x.size(), from + n); ++i)
            worst = std::max(worst, std::abs(x[(size_t) i] - x[(size_t) i - 1]));
        return worst;
    }

    // A vocoder in the mode TUNE, at a body, a ring time and a played pitch (0: none is given).
    static void tuned(Vocoder& fx, double sr, float bodyAt, float ring, float hz)
    {
        fx.prepare(sr, 256);
        fx.setParameter(Vocoder::Mode, (float) Vocoder::Tuned);
        fx.setExtra(Vocoder::BodyAt, bodyAt);
        fx.setExtra(Vocoder::Ring, ring);
        fx.setPitch(hz);
        fx.reset();
    }

    static float levelDb(float level, float reference)
    {
        return 20.0f * std::log10(std::max(level, 1.0e-15f) / std::max(reference, 1.0e-15f));
    }

    void runTest() override
    {
        const double sr = 48000.0;
        const int second = 48000;

        beginTest("VOC is the last insert type, asks for the track's input and names its eight cells");
        {
            // VOC is no longer the last type (DIRT and TALK inserts were appended after it).
            expectEquals((int) EffectType::TalkInsertEffect, (int) EffectType::NumEffectTypes - 1);
            expectEquals(juce::String(AudioEngine::effectTypeName(EffectType::VocoderEffect)), juce::String("VOC"));
            auto fx = AudioEngine::createEffect(EffectType::VocoderEffect);
            expect(fx != nullptr && fx->wantsInput());
            const char* const labels[8] = { "MODE", "BAND", "NOIS", "MIX", "REL", "RING", "BODY", "FRZ" };
            for (int i = 0; i < 8; ++i)
            {
                const ParamSpec spec = i < 4 ? fx->getParamSpec(i) : fx->getExtraSpec(i - 4);
                expect(spec.isValid());
                expectEquals(spec.id, i % 4);
                expectEquals(juce::String(spec.label), juce::String(labels[i]));
                expectEquals(i < 4 ? fx->getParameter(i) : fx->getExtra(i - 4), spec.defaultVal);
            }
            expect(! fx->getParamSpec(4).isValid() && ! fx->getExtraSpec(4).isValid() && ! fx->getParamSpec(-1).isValid());
            expectEquals(fx->getParameter(Vocoder::Mode), (float) Vocoder::Bands, "a new one is the channel vocoder");
            // No other insert type takes the input.
            for (int t = 0; t < (int) EffectType::VocoderEffect; ++t)
                expect(! AudioEngine::createEffect((EffectType) t)->wantsInput());
            expect(AudioEngine::createPerfEffect(PerfFx::INSERT_BASE + (int) EffectType::VocoderEffect) == nullptr,
                   "VOC has no place on a pad");
            expect(! fx->canSync(), "nothing in it follows the tempo");
        }

        beginTest("Cells: the eight are set and read back, held to their ranges");
        {
            Vocoder fx;
            for (int i = 0; i < 8; ++i)
            {
                const ParamSpec spec = i < 4 ? fx.getParamSpec(i) : fx.getExtraSpec(i - 4);
                auto set = [&] (float v) { if (i < 4) fx.setParameter(i, v); else fx.setExtra(i - 4, v); };
                auto get = [&] { return i < 4 ? fx.getParameter(i) : fx.getExtra(i - 4); };
                set(spec.maxVal);
                expectEquals(get(), spec.maxVal);
                set(spec.minVal);
                expectEquals(get(), spec.minVal);
                set(1.0e9f);
                expectEquals(get(), spec.maxVal);
                set(-1.0e9f);
                expectEquals(get(), spec.minVal);
                set(std::numeric_limits<float>::quiet_NaN());
                expectEquals(get(), spec.minVal, "not a number is not taken");
                set(spec.defaultVal);
            }
            fx.setParameter(Vocoder::Band, 3.4f);
            expectEquals(fx.getParameter(Vocoder::Band), 3.0f, "BAND is whole steps");
            expectEquals(fx.getParamSpec(Vocoder::Band).minVal, -24.0f);
            expectEquals(fx.getParamSpec(Vocoder::Band).maxVal, 24.0f);

            const ParamSpec mode = fx.getParamSpec(Vocoder::Mode);
            expect(mode.kind == ParamKind::Choice && mode.numChoices == 2);
            expectEquals(juce::String(mode.choices[0]), juce::String("BAND"));
            expectEquals(juce::String(mode.choices[1]), juce::String("TUNE"));
            const ParamSpec frz = fx.getExtraSpec(Vocoder::Freeze);
            expect(frz.kind == ParamKind::Choice && frz.numChoices == 2);
            const ParamSpec bodySpec = fx.getExtraSpec(Vocoder::BodyAt);
            expectEquals(bodySpec.minVal, (float) res::String);
            expectEquals(bodySpec.maxVal, (float) (res::NumBodyPoints - 1));        // (BELL until phase U2b's eight more points)
            expect(! bodySpec.isDiscrete(), "BODY is every place between the points");
        }

        const Signal carrier = whiteNoise(second, 0.5f, 11u);
        const Signal silence((size_t) second, 0.0f);
        const int fftSize = 16384;
        const int measureFrom = second - fftSize - 256;

        beginTest("A modulator in one band over white noise comes out in that band (low, middle, high)");
        {
            const auto carrierSpectrum = spectrum(carrier, measureFrom, fftSize);
            for (int b : { 2, 8, 14 })
            {
                Vocoder fx;
                fx.prepare(sr, 256);
                const double hz = Vocoder::bandHz(b);
                const Signal mod = sine(second, hz, sr, 0.5f);
                const Stereo out = run(fx, carrier, &mod);
                const auto p = spectrum(out.l, measureFrom, fftSize);
                const double around = share(p, sr, hz / 1.5, hz * 1.5);
                logMessage("band " + juce::String(b) + " (" + juce::String(hz, 0) + " Hz): "
                           + juce::String(around * 100.0, 1) + " % of the power within a fifth either side, level "
                           + juce::String(rms(out.l, measureFrom, fftSize), 4));
                expectEquals(loudestBand(p, sr), b);
                expect(around > 0.9, "the output is around " + juce::String(hz, 0) + " Hz");
                expect(share(carrierSpectrum, sr, hz / 1.5, hz * 1.5) < 0.3, "and the carrier was not");
                expect(rms(out.l, measureFrom, fftSize) > 0.02f, "and is heard");
                expect(fx.bandLevel(b) > 4.0f * fx.bandLevel((b + 4) % Vocoder::NUM_BANDS));
            }
        }

        beginTest("BAND moves where it comes out: a semitone a step");
        {
            const double hz = Vocoder::bandHz(8);
            const Signal mod = sine(second, hz, sr, 0.5f);
            for (float shift : { -12.0f, 0.0f, 12.0f })
            {
                Vocoder fx;
                fx.prepare(sr, 256);
                fx.setParameter(Vocoder::Band, shift);
                const Stereo out = run(fx, carrier, &mod);
                const auto p = spectrum(out.l, measureFrom, fftSize);
                const double ratio = std::pow(2.0, shift / 12.0);
                const double there = share(p, sr, hz * ratio / 1.5, hz * ratio * 1.5);
                const double stayed = share(p, sr, hz / 1.2, hz * 1.2);
                logMessage("BAND " + juce::String(shift, 0) + ": " + juce::String(there * 100.0, 1) + " % around "
                           + juce::String(hz * ratio, 0) + " Hz, " + juce::String(stayed * 100.0, 1)
                           + " % around " + juce::String(hz, 0) + " Hz");
                expect(there > 0.9);
                if (shift != 0.0f)
                    expect(stayed < 0.1, "nothing left where the modulator is");
            }
            // A turn while it sounds arrives (the bands glide there).
            Vocoder fx;
            fx.prepare(sr, 64);
            const Stereo out = run(fx, carrier, carrier, &mod, nullptr, 64,
                                   [&] (int pos) { if (pos == second / 2) fx.setParameter(Vocoder::Band, 12.0f); });
            const auto late = spectrum(out.l, measureFrom, fftSize);
            expect(share(late, sr, hz * 2.0 / 1.5, hz * 2.0 * 1.5) > 0.9);
            expect(share(spectrum(out.l, second / 2 - fftSize, fftSize), sr, hz / 1.5, hz * 1.5) > 0.9, "and was not there before");
        }

        beginTest("A silent modulator: silence at MIX 100, the carrier as it was at MIX 0");
        {
            Vocoder fx;
            fx.prepare(sr, 256);
            Stereo out = run(fx, carrier, &silence);
            expectEquals(rms(out.l, 0, second), 0.0f);
            expectEquals(rms(out.r, 0, second), 0.0f);

            fx.setParameter(Vocoder::Mix, 0.0f);
            const Signal mod = sine(second, 1000.0, sr, 0.8f);
            out = run(fx, carrier, &mod);
            expect(out.l == carrier && out.r == carrier, "MIX 0 is the carrier, sample for sample");

            fx.setParameter(Vocoder::Mix, 0.5f);
            fx.reset();
            out = run(fx, carrier, &silence);
            expectWithinAbsoluteError(rms(out.l, 0, second), 0.5f * rms(carrier, 0, second), 1.0e-4f);
        }

        beginTest("No input: the carrier passes untouched, at every setting");
        {
            for (int setting = 0; setting < 3; ++setting)
            {
                Vocoder fx;
                fx.prepare(sr, 256);
                if (setting >= 1)
                {
                    fx.setParameter(Vocoder::Band, 12.0f);
                    fx.setParameter(Vocoder::Noise, 1.0f);
                    fx.setExtra(Vocoder::Release, 500.0f);
                }
                if (setting == 2) fx.setExtra(Vocoder::Freeze, 1.0f);
                const Stereo out = run(fx, carrier, nullptr, 100);
                expect(out.l == carrier && out.r == carrier, "bit for bit");
            }
            // The per-sample call too, and an input of no length is no input.
            Vocoder fx;
            fx.prepare(sr, 1);
            float l = 0.25f, r = -0.75f, none = 0.9f;
            fx.setInput(&none, &none, 0);
            fx.process(l, r);
            expect(l == 0.25f && r == -0.75f);
        }

        beginTest("The input is used for one call only");
        {
            Vocoder fx;
            fx.prepare(sr, 64);
            Signal in(64, 0.5f), l(64, 0.1f), r(64, 0.1f);
            fx.setInput(in.data(), in.data(), 64);
            expect(fx.hasInput());
            fx.processBlock(l.data(), r.data(), 64);
            expect(! fx.hasInput(), "dropped after processBlock");
            fx.setInput(in.data(), nullptr, 64);
            expect(fx.hasInput(), "a missing right side is the left");
            float a = 0.1f, b = 0.1f;
            fx.process(a, b);
            expect(! fx.hasInput(), "dropped after process");
            fx.setInput(in.data(), in.data(), 64);
            fx.setEnabled(false);
            fx.processBlock(l.data(), r.data(), 64);
            expect(! fx.hasInput(), "dropped when switched off");
            fx.setEnabled(true);

            // Losing the input under sound fades to the carrier (5 ms), no jump; then it is the carrier.
            fx.reset();
            const Signal mod = sine(second / 4, 1000.0, sr, 0.5f);
            const Signal car(carrier.begin(), carrier.begin() + second / 4);
            run(fx, car, &mod);
            Stereo after { car, car };
            fx.processBlock(after.l.data(), after.r.data(), (int) car.size());
            const int faded = (int) (0.006 * sr);
            bool same = true;
            for (size_t i = (size_t) faded; i < car.size(); ++i) same = same && after.l[i] == car[i];
            expect(same, "6 ms after the input went, the carrier");
            expect(after.l[0] != car[0], "it does not jump there");
        }

        beginTest("REL is how long a band rings on after the modulator stops");
        {
            const int stop = second / 4;
            const Signal mod = sine(second, Vocoder::bandHz(8), sr, 0.5f, stop);
            float tails[2] = {};
            int at = 0;
            for (float rel : { 10.0f, 200.0f })
            {
                Vocoder fx;
                fx.prepare(sr, 256);
                fx.setExtra(Vocoder::Release, rel);
                const Stereo out = run(fx, carrier, &mod);
                const int window = 240;                                  // 5 ms
                const float before = rms(out.l, stop - 4800, 4800);
                int fell = second;
                for (int pos = stop; pos + window <= second; pos += window)
                    if (rms(out.l, pos, window) < 0.1f * before) { fell = pos; break; }
                tails[at] = (float) (fell - stop) / (float) sr * 1000.0f;
                logMessage("REL " + juce::String(rel, 0) + " ms: 20 dB down after " + juce::String(tails[at], 0) + " ms");
                ++at;
            }
            // exp(-t / REL) is a tenth at 2.3 REL.
            expect(tails[0] > 10.0f && tails[0] < 60.0f, "REL 10: " + juce::String(tails[0]));
            expect(tails[1] > 350.0f && tails[1] < 600.0f, "REL 200: " + juce::String(tails[1]));
        }

        beginTest("NOIS makes a silent carrier speak");
        {
            const double hz = Vocoder::bandHz(11);
            const Signal mod = sine(second, hz, sr, 0.5f);
            Vocoder fx;
            fx.prepare(sr, 256);
            Stereo out = run(fx, silence, &mod);
            expect(rms(out.l, 0, second) < 1.0e-9f, "a silent carrier says nothing by itself");

            fx.reset();
            fx.setParameter(Vocoder::Noise, 1.0f);
            out = run(fx, silence, &mod);
            const auto p = spectrum(out.l, measureFrom, fftSize);
            const float full = rms(out.l, measureFrom, fftSize);
            logMessage("NOIS 100 on a silent carrier: level " + juce::String(full, 4) + ", "
                       + juce::String(share(p, sr, hz / 1.5, hz * 1.5) * 100.0, 1) + " % around the modulator");
            expect(full > 0.02f);
            expect(share(p, sr, hz / 1.5, hz * 1.5) > 0.9, "and it says what the modulator says");

            fx.reset();
            fx.setParameter(Vocoder::Noise, 0.5f);
            out = run(fx, silence, &mod);
            expectWithinAbsoluteError(rms(out.l, measureFrom, fftSize) / full, 0.5f, 0.02f);

            // With no modulator the noise is not heard either.
            fx.reset();
            fx.setParameter(Vocoder::Noise, 1.0f);
            out = run(fx, silence, &silence);
            expectEquals(rms(out.l, 0, second), 0.0f);
        }

        beginTest("FRZ holds the spectrum after the modulator stops");
        {
            const int stop = second / 4;
            const double hz = Vocoder::bandHz(5);
            const Signal mod = sine(second, hz, sr, 0.5f, stop);
            Vocoder fx;
            fx.prepare(sr, 256);
            Stereo out = run(fx, carrier, carrier, &mod, nullptr, 256,
                             [&] (int pos) { if (pos + 256 > stop - 512) fx.setExtra(Vocoder::Freeze, 1.0f); });
            const float before = rms(out.l, stop - 9600, 8192);
            const float after = rms(out.l, measureFrom, fftSize);
            const auto p = spectrum(out.l, measureFrom, fftSize);
            logMessage("FRZ: level " + juce::String(before, 4) + " while the modulator sounds, " + juce::String(after, 4)
                       + " 0.4 s after it stopped");
            expect(std::abs(levelDb(after, before)) < 3.0f, "the level stays");
            expectEquals(loudestBand(p, sr), 5);
            expect(share(p, sr, hz / 1.5, hz * 1.5) > 0.9, "and so does the band");

            // Frozen, a new modulator changes nothing; let go, the bands follow it again.
            const Signal other = sine(second, Vocoder::bandHz(12), sr, 0.5f);
            out = run(fx, carrier, &other);
            expectEquals(loudestBand(spectrum(out.l, measureFrom, fftSize), sr), 5);
            fx.setExtra(Vocoder::Freeze, 0.0f);
            out = run(fx, carrier, &other);
            expectEquals(loudestBand(spectrum(out.l, measureFrom, fftSize), sr), 12);

            // Without FRZ the same stop is silence.
            Vocoder plain;
            plain.prepare(sr, 256);
            out = run(plain, carrier, &mod);
            expect(rms(out.l, measureFrom, fftSize) < 1.0e-4f);
        }

        beginTest("A band: its level is the modulator's at its centre, 6 dB down where it meets the next, and nothing at 0 Hz");
        {
            // (The bands are modes of dsp/ModeBank.h fed the input's second difference: measured
            // here as the band-passes they have to be.)
            auto levels = [&] (const Signal& mod, float release = 500.0f)     // (500 ms: the level of a tone's peaks)
            {
                Vocoder fx;
                fx.prepare(sr, 256);
                fx.setExtra(Vocoder::Release, release);
                run(fx, silence, &mod);
                std::array<float, Vocoder::NUM_BANDS> l {};
                for (int b = 0; b < Vocoder::NUM_BANDS; ++b) l[(size_t) b] = fx.bandLevel(b);
                return l;
            };
            for (int b : { 0, 8, 15 })
            {
                const auto centre = levels(sine(second, Vocoder::bandHz(b), sr, 0.5f));
                logMessage("band " + juce::String(b) + ": a tone of 0.5 at its centre reads " + juce::String(centre[(size_t) b], 4)
                           + ", two bands away " + juce::String(levelDb(centre[(size_t) (b < 8 ? b + 2 : b - 2)], centre[(size_t) b]), 1) + " dB");
                expectWithinAbsoluteError(centre[(size_t) b], 0.5f, 0.05f);
                expect(levelDb(centre[(size_t) (b < 8 ? b + 2 : b - 2)], centre[(size_t) b]) < -18.0f, "two bands away it is far down");
            }
            const double between = std::sqrt((double) Vocoder::bandHz(8) * (double) Vocoder::bandHz(9));
            const auto cross = levels(sine(second, between, sr, 0.5f));
            logMessage("between bands 8 and 9: " + juce::String(levelDb(cross[8], 0.5f), 2) + " and " + juce::String(levelDb(cross[9], 0.5f), 2) + " dB");
            expectWithinAbsoluteError(levelDb(cross[8], 0.5f), -6.0f, 1.5f);
            expectWithinAbsoluteError(levelDb(cross[9], 0.5f), -6.0f, 1.5f);
            // A modulator that does not move says nothing, however large (once the step it began
            // with has died away).
            const auto still = levels(Signal((size_t) second, 1.0f), 20.0f);
            for (float l : still) expect(l < 1.0e-6f, "0 Hz: " + juce::String(l, 8));
        }

        beginTest("Level: white noise over white noise comes out near the carrier's level; the sides stay apart");
        {
            Vocoder fx;
            fx.prepare(sr, 256);
            const Signal mod = whiteNoise(second, 1.0f, 77u);
            Stereo out = run(fx, carrier, &mod);
            const float gainDb = levelDb(rms(out.l, second / 2, second / 2), rms(carrier, second / 2, second / 2));
            logMessage("full-scale white noise over white noise: " + juce::String(gainDb, 2) + " dB against the carrier");
            expect(std::abs(gainDb) < 2.0f);

            // The bands add up to an even whole: with every band open alike (a modulator of one tone
            // at each band's centre), a carrier tone where two bands cross comes out about as loud
            // as one at a band's centre. Neighbours are summed with opposite signs; with the same
            // sign they cancel there.
            Signal comb((size_t) second, 0.0f);
            for (int b = 0; b < Vocoder::NUM_BANDS; ++b)
            {
                const Signal one = sine(second, Vocoder::bandHz(b), sr, 0.05f);
                for (size_t i = 0; i < comb.size(); ++i) comb[i] += one[i];
            }
            float tone[2] = {};
            for (int where = 0; where < 2; ++where)
            {
                const double hz = where == 0 ? (double) Vocoder::bandHz(8)
                                             : std::sqrt((double) Vocoder::bandHz(8) * (double) Vocoder::bandHz(9));
                fx.reset();
                const Signal car = sine(second, hz, sr, 0.25f);
                out = run(fx, car, &comb);
                tone[where] = rms(out.l, second / 2, second / 2);
            }
            logMessage("a carrier tone between two bands against one at a band's centre: "
                       + juce::String(levelDb(tone[1], tone[0]), 2) + " dB");
            expect(std::abs(levelDb(tone[1], tone[0])) < 3.0f);

            // A carrier on the left only stays on the left.
            fx.reset();
            out = run(fx, carrier, silence, &mod, nullptr, 256, [] (int) {});
            expect(rms(out.l, 0, second) > 0.05f);
            expect(rms(out.r, 0, second) < 1.0e-9f, "nothing crosses to the silent side");

            // The modulator is the sum of its sides: opposite sides say nothing.
            Signal inverted = mod;
            for (auto& v : inverted) v = -v;
            fx.reset();
            out = run(fx, carrier, carrier, &mod, &inverted, 256, [] (int) {});
            expectEquals(rms(out.l, 0, second), 0.0f);
        }

        beginTest("reset clears the filters and the bands; the sample rate can change");
        {
            Vocoder fx;
            fx.prepare(sr, 256);
            fx.setExtra(Vocoder::Release, 500.0f);
            const Signal mod = whiteNoise(second / 4, 1.0f, 5u);
            const Signal car(carrier.begin(), carrier.begin() + second / 4);
            run(fx, car, &mod);
            expect(fx.bandLevel(8) > 0.01f);
            fx.reset();
            for (int b = 0; b < Vocoder::NUM_BANDS; ++b) expectEquals(fx.bandLevel(b), 0.0f);
            const Signal quiet((size_t) 4800, 0.0f);
            const Stereo out = run(fx, quiet, &quiet);
            expect(rms(out.l, 0, 4800) < 1.0e-9f, "nothing rings on after reset");

            // After reset it is as a new one: the same sound in gives the same sound out, sample
            // for sample (a carrier band or a modulator band still ringing would show).
            {
                run(fx, car, &mod);
                fx.reset();
                Vocoder fresh;
                fresh.prepare(sr, 256);
                fresh.setExtra(Vocoder::Release, 500.0f);
                const Signal tone = sine(4800, Vocoder::bandHz(6), sr, 0.5f);
                const Signal c2(carrier.begin(), carrier.begin() + 4800);
                const Stereo used = run(fx, c2, &tone);
                const Stereo unused = run(fresh, c2, &tone);
                expect(rms(unused.l, 0, 4800) > 0.001f);
                expect(used.l == unused.l && used.r == unused.r, "reset leaves nothing of the sound before");
            }

            // The same band is at the same place at 96 kHz and at 44.1 kHz.
            for (double rate : { 44100.0, 96000.0 })
            {
                const int n = (int) rate;
                const double hz = Vocoder::bandHz(10);
                fx.prepare(rate, 256);
                fx.setExtra(Vocoder::Release, 60.0f);
                const Signal m = sine(n, hz, rate, 0.5f);
                const Signal c = whiteNoise(n, 0.5f, 3u);
                const Stereo o = run(fx, c, &m);
                const auto p = spectrum(o.l, n - fftSize - 256, fftSize);
                expectEquals(loudestBand(p, rate), 10);
                expect(share(p, rate, hz / 1.5, hz * 1.5) > 0.9, juce::String(rate, 0) + " Hz");
            }
        }

        // ---------------------------------------------------------------------------------------
        // The mode TUNE: the spectral resonator.
        const int two = 2 * second;
        const Signal longCarrier = whiteNoise(two, 0.5f, 11u), longWhite = whiteNoise(two, 1.0f, 77u);
        const Signal longSilence((size_t) two, 0.0f);
        const int bigFft = 32768;
        const int bigFrom = two - bigFft - 256;

        beginTest("TUNE: the partials are the body's series at the pitch (string, plate, bell: measured)");
        {
            // What the series are, written down here independently: a string's whole numbers; a
            // plate of 1.37 : 1 held at its edges ((m^2 + n^2 / 1.37^2), lowest first, over the
            // lowest); a church bell's hum, prime, tierce, quint and nominal.
            struct Case { float at; const char* name; std::vector<double> ratios; };
            const Case cases[] = { { (float) res::String, "STRG", { 1.0, 2.0, 3.0, 4.0 } },
                                   { (float) res::Plate, "PLAT", { 1.0, 2.043, 2.957, 3.780 } },
                                   { (float) res::Bell, "BELL", { 0.5, 1.0, 1.183, 1.506, 2.0 } } };
            const double pitch = 220.0;
            for (const auto& c : cases)
            {
                Vocoder fx;
                tuned(fx, sr, c.at, 2.0f, (float) pitch);
                const Stereo out = run(fx, longCarrier, &longWhite);
                const auto p = spectrum(out.l, bigFrom, bigFft);
                juce::String line = juce::String(c.name) + " at 220 Hz:";
                for (size_t k = 0; k < c.ratios.size(); ++k)
                {
                    const double hz = pitch * c.ratios[k];
                    const double found = peakHz(p, sr, hz * 0.97, hz * 1.03);
                    // ... against the same width half way to the next partial.
                    const double next = k + 1 < c.ratios.size() ? c.ratios[k + 1] : c.ratios[k] * c.ratios[k] / c.ratios[k - 1];
                    const double gap = pitch * std::sqrt(c.ratios[k] * next);
                    const double on = share(p, sr, hz * 0.99, hz * 1.01), off = share(p, sr, gap * 0.99, gap * 1.01);
                    line += " " + juce::String(found, 1) + " Hz (" + juce::String(10.0 * std::log10(on / std::max(off, 1.0e-30)), 0) + " dB)";
                    // (A bin of the measurement is 1.5 Hz wide.)
                    expect(std::abs(found - hz) < std::max(0.006 * hz, 1.6), juce::String(c.name) + " partial at " + juce::String(hz, 1) + ": " + juce::String(found, 1));
                    expect(on > 8.0 * off, juce::String(c.name) + ": a peak at " + juce::String(hz, 1));
                }
                logMessage(line);
                expect(rms(out.l, second, second) > 0.02f, "it is heard");
            }

            // The pitch is the note's when one is given (setPitch), C3 otherwise; BAND moves it.
            struct At { float played; float band; double expected; };
            for (const At at : { At { 330.0f, 0.0f, 330.0 }, At { 0.0f, 0.0f, 130.8128 }, At { 0.0f, 12.0f, 261.6256 },
                                 At { 330.0f, -12.0f, 165.0 } })
            {
                Vocoder fx;
                tuned(fx, sr, (float) res::String, 2.0f, at.played);
                fx.setParameter(Vocoder::Band, at.band);
                fx.reset();
                const Stereo out = run(fx, longCarrier, &longWhite);
                const auto p = spectrum(out.l, bigFrom, bigFft);
                const double found = centreHz(p, sr, at.expected * 0.96, at.expected * 1.04);
                logMessage("note " + juce::String(at.played, 0) + " Hz, BAND " + juce::String(at.band, 0) + ": the first partial at "
                           + juce::String(found, 1) + " Hz");
                expect(std::abs(found - at.expected) < 0.012 * at.expected, juce::String(found, 2) + " for " + juce::String(at.expected, 2));
                expect(share(p, sr, at.expected * 0.96, at.expected * 1.04) > 20.0 * share(p, sr, at.expected * 1.3, at.expected * 1.3 * 1.08),
                       "a partial there, and none between it and the next");
                expectWithinAbsoluteError((double) fx.tunedHz(), at.expected, 0.01 * at.expected);
            }
            // A note that comes while it sounds is where the body goes.
            Vocoder fx;
            tuned(fx, sr, (float) res::String, 0.5f, 220.0f);
            const Stereo out = run(fx, longCarrier, longCarrier, &longWhite, nullptr, 256,
                                   [&] (int pos) { if (pos >= second) fx.setPitch(330.0f); });
            const double early = peakHz(spectrum(out.l, second - bigFft / 2 - 256, bigFft / 2), sr, 180.0, 400.0);
            const double late = peakHz(spectrum(out.l, bigFrom, bigFft), sr, 180.0, 400.0);
            expect(std::abs(early / 220.0 - 1.0) < 0.02 && std::abs(late / 330.0 - 1.0) < 0.01,
                   juce::String(early, 1) + " then " + juce::String(late, 1));
        }

        beginTest("TUNE: each partial takes the level of the modulator's band there");
        {
            // A comb at 110 Hz: whole multiples up to 7 kHz, each ringing as long as the others.
            auto through = [&] (const Signal& mod)
            {
                Vocoder fx;
                tuned(fx, sr, (float) res::Comb, 1.0f, 110.0f);
                return spectrum(run(fx, longCarrier, &mod).l, bigFrom, bigFft);
            };
            // A modulator with nothing up high (white noise through four low-passes at 300 Hz).
            Signal dull = longWhite;
            {
                const float k = 1.0f - std::exp(-6.2831853f * 300.0f / (float) sr);
                float z[4] = {};
                for (auto& v : dull)
                {
                    z[0] += k * (v - z[0]);
                    for (int i = 1; i < 4; ++i) z[i] += k * (z[i - 1] - z[i]);
                    v = 4.0f * z[3];
                }
            }
            const auto bright = through(longWhite), low = through(dull);
            const double highBright = share(bright, sr, 2000.0, 20000.0), highLow = share(low, sr, 2000.0, 20000.0);
            logMessage("the power above 2 kHz: " + juce::String(100.0 * highBright, 1) + " % under white noise, "
                       + juce::String(100.0 * highLow, 3) + " % under noise low-passed at 300 Hz");
            expect(highBright > 0.2, "white noise opens the high partials");
            expect(highLow < 0.01 * highBright, "a dull modulator leaves them quiet");
            expect(share(low, sr, 80.0, 700.0) > 0.9, "and what sounds is where the modulator is");

            // A modulator in one band: the partials around it, and they are the comb's (whole
            // multiples of 110 Hz), not the modulator's frequency.
            const double hz = Vocoder::bandHz(12);
            const auto one = through(sine(two, hz, sr, 0.5f));
            const double found = peakHz(one, sr, 20.0, 20000.0), multiple = found / 110.0;
            logMessage("a modulator at " + juce::String(hz, 0) + " Hz: " + juce::String(100.0 * share(one, sr, hz / 1.5, hz * 1.5), 1)
                       + " % within a fifth, the strongest partial at " + juce::String(found, 1) + " Hz (" + juce::String(multiple, 3) + " x 110)");
            expect(share(one, sr, hz / 1.5, hz * 1.5) > 0.8);
            expect(std::abs(multiple - std::round(multiple)) < 0.03 && std::abs(found / hz - 1.0) < 0.2);

            // The levels stay on the partials when the body is told again: frozen (so that nobody
            // sends them a second time), then a new note.
            {
                Vocoder fx;
                tuned(fx, sr, (float) res::Comb, 1.0f, 110.0f);
                const Signal tone = sine(two, hz, sr, 0.5f);
                const Stereo out = run(fx, longCarrier, longCarrier, &tone, nullptr, 256, [&] (int pos)
                {
                    if (pos >= second / 2) fx.setExtra(Vocoder::Freeze, 1.0f);
                    if (pos >= second) fx.setPitch(146.83f);
                });
                const auto p = spectrum(out.l, bigFrom, bigFft);
                const double at = peakHz(p, sr, 20.0, 20000.0) / 146.83;
                logMessage("frozen, then a note of 146.8 Hz: " + juce::String(100.0 * share(p, sr, hz / 1.5, hz * 1.5), 1)
                           + " % within a fifth of the modulator, the strongest partial " + juce::String(at, 3) + " x the note");
                expect(share(p, sr, hz / 1.5, hz * 1.5) > 0.8, "the new note's partials have the levels the old one's had");
                expect(std::abs(at - std::round(at)) < 0.03);
            }
        }

        beginTest("TUNE: a silent modulator is silence; no input passes the carrier; the sides stay apart");
        {
            Vocoder fx;
            tuned(fx, sr, (float) res::Plate, 0.4f, 0.0f);
            fx.setParameter(Vocoder::Noise, 1.0f);
            Stereo out = run(fx, carrier, &silence);
            expectEquals(rms(out.l, 0, second), 0.0f);
            expectEquals(rms(out.r, 0, second), 0.0f);
            expectEquals(fx.modesAwake(), 0, "and nothing runs");

            fx.reset();
            out = run(fx, carrier, nullptr, 100);
            expect(out.l == carrier && out.r == carrier, "no input: bit for bit");

            fx.setParameter(Vocoder::Noise, 0.0f);
            fx.reset();
            const Signal mod = whiteNoise(second, 1.0f, 77u);
            out = run(fx, carrier, &mod);
            const float gainDb = levelDb(rms(out.l, second / 2, second / 2), rms(carrier, second / 2, second / 2));
            logMessage("TUNE, the default body at C3: white noise over white noise is " + juce::String(gainDb, 2) + " dB against the carrier");
            expect(std::abs(gainDb) < 3.0f);
            // (A louder voice is a louder result here too.)
            Signal quiet = mod;
            for (auto& v : quiet) v *= 0.25f;
            fx.reset();
            const Stereo soft = run(fx, carrier, &quiet);
            expectWithinAbsoluteError(levelDb(rms(soft.l, second / 2, second / 2), rms(out.l, second / 2, second / 2)), -12.0f, 1.5f);

            fx.reset();
            out = run(fx, carrier, silence, &mod, nullptr, 256, [] (int) {});
            expect(rms(out.l, 0, second) > 0.05f);
            expect(rms(out.r, 0, second) < 1.0e-9f, "nothing crosses to the silent side");

            // A silent carrier says nothing by itself, and speaks with NOIS.
            fx.reset();
            out = run(fx, silence, &mod);
            expectEquals(rms(out.l, 0, second), 0.0f);
            fx.setParameter(Vocoder::Noise, 1.0f);
            fx.reset();
            out = run(fx, silence, &mod);
            expect(rms(out.l, second / 2, second / 2) > 0.05f, "NOIS: " + juce::String(rms(out.l, second / 2, second / 2), 4));
        }

        beginTest("TUNE: RING is how long the body rings on; FRZ keeps it driven");
        {
            const int stop = second / 4;
            Signal mod = whiteNoise(two, 1.0f, 77u);
            std::fill(mod.begin() + stop, mod.end(), 0.0f);
            float tails[3] = {};
            int at = 0;
            for (float ring : { 0.1f, 1.0f, 4.0f })
            {
                Vocoder fx;
                tuned(fx, sr, (float) res::Comb, ring, 220.0f);         // (a comb: every partial rings as long)
                fx.setExtra(Vocoder::Release, 5.0f);
                const Stereo out = run(fx, longCarrier, &mod);
                const int window = 480;                                  // 10 ms
                const float before = rms(out.l, stop - 4800, 4800);
                int fell = two;
                for (int pos = stop; pos + window <= two; pos += window)
                    if (rms(out.l, pos, window) < 0.1f * before) { fell = pos; break; }
                tails[at] = (float) (fell - stop) / (float) sr;
                logMessage("RING " + juce::String(ring, 1) + " s: 20 dB down " + juce::String(tails[at], 3) + " s after the modulator stopped");
                ++at;
            }
            // 60 dB in RING: 20 dB in a third of it (and the 12 ms the bands take to shut).
            expect(tails[0] > 0.02f && tails[0] < 0.08f, juce::String(tails[0]));
            expect(tails[1] > 0.25f && tails[1] < 0.45f, juce::String(tails[1]));
            expect(tails[2] > 1.0f && tails[2] < 1.7f, juce::String(tails[2]));

            // Frozen before the modulator stops, the body goes on being driven as it was.
            Vocoder fx;
            tuned(fx, sr, (float) res::Plate, 0.1f, 220.0f);
            const Stereo out = run(fx, longCarrier, longCarrier, &mod, nullptr, 256,
                                   [&] (int pos) { if (pos + 256 > stop - 512) fx.setExtra(Vocoder::Freeze, 1.0f); });
            const float before = rms(out.l, stop - 9600, 8192), after = rms(out.l, two - 16384, 16384);
            logMessage("FRZ: level " + juce::String(before, 4) + " while the modulator sounds, " + juce::String(after, 4) + " long after it stopped");
            expect(std::abs(levelDb(after, before)) < 3.0f);
        }

        beginTest("TUNE: BODY moves the partials without a break, and no cell jumps under sound");
        {
            // The second partial from the plate (2.04) by the bar (2.76) to the bell (1.18), in tenths.
            float last = 0.0f, widest = 0.0f, lowest = 10.0f, highest = 0.0f;
            for (int step = 0; step <= 20; ++step)
            {
                Vocoder fx;
                tuned(fx, sr, 3.0f + 0.1f * (float) step, 0.4f, 220.0f);
                const Signal mod = whiteNoise(2400, 1.0f, 77u);
                run(fx, Signal(carrier.begin(), carrier.begin() + 2400), &mod);
                const float second2 = fx.tunedPartial(2);
                if (step == 0) expectWithinAbsoluteError(second2, 2.043f, 0.01f);
                if (step == 10) expectWithinAbsoluteError(second2, 2.757f, 0.02f);
                if (step == 20) expectWithinAbsoluteError(second2, 1.183f, 0.01f);
                if (step > 0) widest = std::max(widest, std::abs(second2 - last));
                lowest = std::min(lowest, second2);
                highest = std::max(highest, second2);
                last = second2;
            }
            logMessage("BODY 3 to 5 in tenths: the second partial between " + juce::String(lowest, 3) + " and " + juce::String(highest, 3)
                       + " times the note, never more than " + juce::String(widest, 3) + " a tenth");
            expect(widest < 0.4f && widest > 0.01f);

            // Half way between the bar and the bell it is where the body says, and it is heard there.
            {
                Vocoder fx;
                tuned(fx, sr, 4.5f, 2.0f, 220.0f);
                const Stereo out = run(fx, longCarrier, &longWhite);
                const double expected = 220.0 * (double) fx.tunedPartial(2);
                const auto p = spectrum(out.l, bigFrom, bigFft);
                const double found = peakHz(p, sr, expected * 0.97, expected * 1.03);
                logMessage("BODY 4.5: the second partial at " + juce::String(found, 1) + " Hz (" + juce::String(found / 220.0, 3) + " times the note)");
                expect(fx.tunedPartial(2) > 1.3f && fx.tunedPartial(2) < 2.6f, "between the bell's and the bar's");
                expect(std::abs(found / expected - 1.0) < 0.006);
                const double gap = expected * 1.06;
                expect(share(p, sr, expected * 0.99, expected * 1.01) > 4.0 * share(p, sr, gap * 0.99, gap * 1.01));
            }

            // Under a steady sound (a carrier of C3's first eight harmonics, every band open alike):
            // a cell thrown from one end to the other steps the output no more than the sound's own
            // steps do. MODE dips to silence and comes back.
            Signal steady((size_t) second, 0.0f), open((size_t) second, 0.0f);
            for (int k = 1; k <= 8; ++k)
            {
                const Signal one = sine(second, 130.8128 * k, sr, 0.04f);
                for (size_t i = 0; i < steady.size(); ++i) steady[i] += one[i];
            }
            for (int b = 0; b < Vocoder::NUM_BANDS; ++b)
            {
                const Signal one = sine(second, Vocoder::bandHz(b), sr, 0.05f);
                for (size_t i = 0; i < open.size(); ++i) open[i] += one[i];
            }
            struct Throw { const char* name; int cell; float from, to; bool startTuned; };
            const Throw throws[] = { { "BODY", 4 + Vocoder::BodyAt, 0.0f, (float) (res::NumBodyPoints - 1), true }, { "RING", 4 + Vocoder::Ring, 0.02f, 10.0f, true },
                                     { "NOIS", Vocoder::Noise, 0.0f, 1.0f, true }, { "BAND (TUNE)", Vocoder::Band, 0.0f, 12.0f, true },
                                     { "BAND (BAND)", Vocoder::Band, 0.0f, 12.0f, false }, { "FRZ", 4 + Vocoder::Freeze, 0.0f, 1.0f, true },
                                     { "MODE to TUNE", Vocoder::Mode, 0.0f, 1.0f, false }, { "MODE to BAND", Vocoder::Mode, 1.0f, 0.0f, true } };
            for (const auto& t : throws)
            {
                Vocoder fx;
                fx.prepare(sr, 64);
                fx.setParameter(Vocoder::Mode, t.startTuned ? 1.0f : 0.0f);
                auto set = [&] (float v) { if (t.cell < 4) fx.setParameter(t.cell, v); else fx.setExtra(t.cell - 4, v); };
                set(t.from);
                fx.reset();
                const int when = second / 2;
                const Stereo out = run(fx, steady, steady, &open, nullptr, 64, [&] (int pos) { if (pos == when) set(t.to); });
                const float own = std::max(largestStep(out.l, when - 9600, 9600), largestStep(out.l, second - 9600, 9600));
                const float at = largestStep(out.l, when, 2400);
                logMessage(juce::String(t.name) + " thrown: the largest step " + juce::String(at, 5) + ", the sound's own " + juce::String(own, 5));
                expect(own > 1.0e-4f, "(it sounds)");
                expect(at < 2.0f * own + 0.002f, juce::String(t.name) + ": " + juce::String(at, 5) + " against " + juce::String(own, 5));
                if (t.cell == Vocoder::Mode)
                {
                    // ... and the other mode is what sounds then: TUNE rings on, BAND does not.
                    Signal gone = open;
                    std::fill(gone.begin() + when + 4800, gone.end(), 0.0f);
                    Vocoder again;
                    again.prepare(sr, 64);
                    again.setParameter(Vocoder::Mode, t.from);
                    again.setExtra(Vocoder::Ring, 4.0f);
                    again.setExtra(Vocoder::Release, 5.0f);
                    const Stereo o = run(again, steady, steady, &gone, nullptr, 64, [&] (int pos) { if (pos == when) again.setParameter(Vocoder::Mode, t.to); });
                    const float tail = rms(o.l, when + 4800 + 4800, 4800), body = rms(o.l, when + 2400, 2400);
                    if (t.to >= 0.5f) expect(tail > 0.1f * body, "TUNE rings on: " + juce::String(tail, 5) + " after " + juce::String(body, 5));
                    else              expect(tail < 0.01f * body, "BAND does not: " + juce::String(tail, 5) + " after " + juce::String(body, 5));
                }
            }
        }

        beginTest("Modulation: NOIS, REL, RING and BODY move with modulateCell and read as they were set");
        {
            // BODY at the plate, moved by two points (2 / 13 of its range since phase U2b's eight
            // more points; it was two fifths), is the bell.
            Vocoder fx;
            tuned(fx, sr, (float) res::Plate, 0.4f, 220.0f);
            fx.modulateCell(Vocoder::BodyCell, 2.0f / (float) (res::NumBodyPoints - 1));
            const Signal mod = whiteNoise(4800, 1.0f, 77u);
            const Signal car(carrier.begin(), carrier.begin() + 4800);
            run(fx, car, &mod);
            expectWithinAbsoluteError(fx.tunedPartial(2), 1.183f, 0.01f);
            expectEquals(fx.getExtra(Vocoder::BodyAt), (float) res::Plate);
            fx.modulateCell(Vocoder::BodyCell, 0.0f);
            run(fx, car, &mod);
            expectWithinAbsoluteError(fx.tunedPartial(2), 2.043f, 0.01f);

            // RING at its shortest, moved to its longest, rings on; REL likewise holds the bands open.
            for (int cell : { (int) Vocoder::RingCell, (int) Vocoder::ReleaseCell })
            {
                float tail[2] = {};
                for (int moved = 0; moved < 2; ++moved)
                {
                    Vocoder v;
                    tuned(v, sr, (float) res::Plate, 0.02f, 220.0f);
                    v.setExtra(Vocoder::Release, 5.0f);
                    v.modulateCell(cell, moved ? 1.0f : 0.0f);
                    Signal burst = whiteNoise(second, 1.0f, 77u);
                    std::fill(burst.begin() + second / 4, burst.end(), 0.0f);
                    const Stereo out = run(v, carrier, &burst);
                    tail[moved] = rms(out.l, second / 4 + 4800, 9600);
                    expectEquals(v.getExtra(Vocoder::Ring), 0.02f);
                    expectEquals(v.getExtra(Vocoder::Release), 5.0f);
                }
                expect(tail[1] > 100.0f * tail[0] && tail[1] > 0.01f,
                       juce::String(cell == Vocoder::RingCell ? "RING" : "REL") + " moved: " + juce::String(tail[1], 5) + " against " + juce::String(tail[0], 7));
            }

            // NOIS moved makes a silent carrier speak (BAND mode).
            Vocoder n;
            n.prepare(sr, 256);
            n.modulateCell(Vocoder::NoiseCell, 1.0f);
            const Signal tone = sine(second, Vocoder::bandHz(8), sr, 0.5f);
            expect(rms(run(n, silence, &tone).l, second / 2, second / 2) > 0.02f);
            expectEquals(n.getParameter(Vocoder::Noise), 0.0f);

            // The list, the whole steps, the switch and the mix are not moved.
            Vocoder a, b;
            a.prepare(sr, 256);
            b.prepare(sr, 256);
            for (int cell : { (int) Vocoder::ModeCell, (int) Vocoder::BandCell, (int) Vocoder::MixCell, (int) Vocoder::FreezeCell, -1, 8 })
                b.modulateCell(cell, 1.0f);
            const Stereo plain = run(a, car, &mod), same = run(b, car, &mod);
            expect(plain.l == same.l && plain.r == same.r);

            // An effect that does not tune itself ignores the pitch.
            auto other = AudioEngine::createEffect(EffectType::RingModEffect);
            other->setPitch(440.0f);
        }

        beginTest("Finite and held at the extremes: 22.05 to 192 kHz, blocks of 1 to 4096, both modes, every cell at both ends");
        {
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();
            float worst = 0.0f;
            int runs = 0;
            for (double rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                const int n = 8192;
                Signal loud = whiteNoise(n, 4.0f, 9u);              // far over full scale
                for (int i = 0; i < n; i += 97) loud[(size_t) i] = (i % 2 == 0) ? 1.0e6f : -1.0e6f;
                loud[50] = 1.0e30f; loud[51] = -1.0e30f;
                Signal bad = loud;
                bad[100] = inf; bad[200] = -inf; bad[300] = nan;
                Signal hot = whiteNoise(n, 1.0f, 21u);
                hot[400] = 1.0e30f; hot[500] = -1.0e30f;
                for (int mode = 0; mode < 2; ++mode)
                    for (int block : { 1, 7, 64, 512, 4096 })
                        for (int ends = 0; ends < 4; ++ends)
                        {
                            Vocoder fx;
                            fx.prepare(rate, block);
                            fx.setParameter(Vocoder::Mode, (float) mode);
                            fx.setParameter(Vocoder::Band, (ends & 1) ? 24.0f : -24.0f);
                            fx.setExtra(Vocoder::Release, (ends & 2) ? 500.0f : 5.0f);
                            fx.setExtra(Vocoder::Ring, (ends & 2) ? 10.0f : 0.02f);
                            fx.setExtra(Vocoder::BodyAt, (ends & 1) ? 5.0f : 0.0f);
                            fx.setParameter(Vocoder::Noise, (ends & 1) ? 1.0f : 0.0f);
                            fx.setParameter(Vocoder::Mix, 1.0f);
                            fx.setPitch(ends == 0 ? 1.0e9f : ends == 1 ? 1.0e-3f : ends == 2 ? nan : 20000.0f);
                            fx.reset();
                            int calls = 0;
                            const Stereo out = run(fx, hot, hot, ends < 2 ? &loud : &bad, nullptr, block, [&] (int)
                            {
                                ++calls;
                                if (ends == 3) fx.setExtra(Vocoder::Freeze, (calls / 3) % 2 == 0 ? 1.0f : 0.0f);
                                if (ends == 2) fx.setParameter(Vocoder::Band, (calls % 2) ? 24.0f : -24.0f);
                                if (ends == 1) fx.setParameter(Vocoder::Mode, (float) ((calls / 2 + mode) % 2));
                                if (ends == 0)
                                {
                                    fx.setExtra(Vocoder::BodyAt, (calls % 2) ? 5.0f : 0.0f);
                                    fx.modulateCell(Vocoder::RingCell, (calls % 3) ? 1.0f : -1.0f);
                                    fx.setPitch((calls % 2) ? inf : 30.0f);
                                }
                            });
                            bool finite = true;
                            for (int i = 0; i < n; ++i)
                            {
                                finite = finite && std::isfinite(out.l[(size_t) i]) && std::isfinite(out.r[(size_t) i]);
                                worst = std::max({ worst, std::abs(out.l[(size_t) i]), std::abs(out.r[(size_t) i]) });
                            }
                            expect(finite, juce::String(rate, 0) + " Hz, mode " + juce::String(mode) + ", block " + juce::String(block)
                                           + ", ends " + juce::String(ends));
                            ++runs;

                            // And it falls silent: nothing rings on by itself (the top band, moved up
                            // two octaves, is held below Nyquist), and nothing is left running.
                            if (block == 512)
                            {
                                fx.setExtra(Vocoder::Freeze, 0.0f);
                                fx.setParameter(Vocoder::Noise, 0.0f);
                                fx.setExtra(Vocoder::Release, 50.0f);
                                fx.setExtra(Vocoder::Ring, 0.02f);
                                fx.modulateCell(Vocoder::RingCell, 0.0f);
                                fx.setParameter(Vocoder::Mode, (float) mode);
                                const int tail = (int) (rate * 0.25);
                                const Signal none((size_t) tail, 0.0f);
                                run(fx, none, &none, 512);
                                run(fx, none, &none, 512);
                                const Stereo end = run(fx, none, &none, 512);
                                expect(rms(end.l, 0, tail) < 1.0e-6f, "silent half a second after: " + juce::String(rate, 0) + " Hz, mode "
                                                                      + juce::String(mode) + ", ends " + juce::String(ends));
                                for (int more = 0; more < 12 && fx.modesAwake() > 0; ++more) run(fx, none, &none, 512);
                                expectEquals(fx.modesAwake(), 0, "at rest: " + juce::String(rate, 0) + " Hz, mode " + juce::String(mode));
                            }
                        }
            }
            logMessage(juce::String(runs) + " runs, largest sample " + juce::String(worst, 3));
            expect(worst <= 2.0f, "never above 2: " + juce::String(worst));

            // A carrier that is not a number does not get into the filters, in either mode.
            for (int mode = 0; mode < 2; ++mode)
            {
                Vocoder fx;
                fx.prepare(48000.0, 64);
                fx.setParameter(Vocoder::Mode, (float) mode);
                fx.reset();
                Signal in(64, 0.5f), l(64, nan), r(64, inf);
                for (int i = 0; i < 64; i += 2) in[(size_t) i] = -0.5f;
                fx.setParameter(Vocoder::Mix, 1.0f);
                fx.setInput(in.data(), in.data(), 64);
                fx.processBlock(l.data(), r.data(), 64);
                Signal l2(64, 0.25f), r2(64, 0.25f);
                fx.setInput(in.data(), in.data(), 64);
                fx.processBlock(l2.data(), r2.data(), 64);
                for (int i = 0; i < 64; ++i) expect(std::isfinite(l2[(size_t) i]) && std::isfinite(r2[(size_t) i]));
                // Nor into what comes out, at any MIX above 0 (not a number times 0 is not a number).
                for (float mixAt : { 1.0f, 0.5f })
                {
                    fx.setParameter(Vocoder::Mix, mixAt);
                    Signal l4(64, 0.25f), r4(64, 0.25f);
                    l4[3] = nan; r4[5] = inf; l4[9] = -inf;
                    fx.setInput(in.data(), in.data(), 64);
                    fx.processBlock(l4.data(), r4.data(), 64);
                    for (int i = 0; i < 64; ++i)
                        expect(std::isfinite(l4[(size_t) i]) && std::isfinite(r4[(size_t) i]), "MIX " + juce::String(mixAt, 1));
                }
            }
            Vocoder fx;
            fx.prepare(48000.0, 64);

            // An input shorter than the block: the rest is silence, nothing is read past it.
            Signal shortIn(16, 0.5f), l3(64, 0.25f), r3(64, 0.25f);
            fx.setInput(shortIn.data(), shortIn.data(), 16);
            fx.processBlock(l3.data(), r3.data(), 64);
            for (int i = 0; i < 64; ++i) expect(std::isfinite(l3[(size_t) i]));
            // The same as an input of the block's length that is silent after those 16: what lies
            // behind the 16 (here a loud signal) is not heard.
            {
                Signal longIn(64, 2.0f), padded(64, 0.0f);
                for (int i = 0; i < 16; ++i) longIn[(size_t) i] = padded[(size_t) i] = (i % 2) ? 0.5f : -0.5f;
                Vocoder a, b;
                a.prepare(48000.0, 64);
                b.prepare(48000.0, 64);
                Signal la = whiteNoise(64, 0.5f, 4u), ra = la, lb = la, rb = la;
                a.setInput(longIn.data(), longIn.data(), 16);
                a.processBlock(la.data(), ra.data(), 64);
                b.setInput(padded.data(), padded.data(), 64);
                b.processBlock(lb.data(), rb.data(), 64);
                expect(rms(la, 0, 64) > 1.0e-4f);
                expect(la == lb && ra == rb, "nothing is read past the frames given");
            }

            // The block's size does not change what comes out, in either mode.
            for (int mode = 0; mode < 2; ++mode)
            {
                Stereo outs[2];
                int k = 0;
                for (int block : { 1, 4096 })
                {
                    Vocoder v;
                    v.prepare(48000.0, block);
                    v.setParameter(Vocoder::Mode, (float) mode);
                    v.reset();
                    const Signal mod = whiteNoise(9600, 1.0f, 77u);
                    outs[k++] = run(v, Signal(carrier.begin(), carrier.begin() + 9600), Signal(carrier.begin(), carrier.begin() + 9600),
                                    &mod, nullptr, block, [] (int) {});
                }
                expect(rms(outs[0].l, 4800, 4800) > 0.01f);
                expect(outs[0].l == outs[1].l && outs[0].r == outs[1].r, "mode " + juce::String(mode) + ": blocks of 1 and of 4096 alike");
            }
        }

        beginTest("What has rung out costs nothing: no mode runs, no level is left, no number is tiny");
        {
            for (int mode = 0; mode < 2; ++mode)
            {
                Vocoder fx;
                fx.prepare(sr, 512);
                fx.setParameter(Vocoder::Mode, (float) mode);
                fx.reset();
                const Signal mod = whiteNoise(second / 2, 1.0f, 77u);
                const Signal car(carrier.begin(), carrier.begin() + second / 2);
                run(fx, car, &mod, 512);
                const int speaking = fx.modesAwake();
                expect(speaking >= (mode == 0 ? 96 : 100), "speaking: " + juce::String(speaking) + " modes run");
                // The modulator stops and the carrier goes on: the carrier's side rests too.
                int tiny = 0;
                Stereo out;
                for (int k = 0; k < 6; ++k)
                {
                    out = run(fx, carrier, &silence, 512);
                    for (float v : out.l) tiny += (v != 0.0f && std::abs(v) < 1.0e-30f) ? 1 : 0;
                }
                expectEquals(tiny, 0, "no sample in the smallest numbers on the way down");
                expectEquals(fx.modesAwake(), 0, "mode " + juce::String(mode) + ": nothing runs under a carrier nobody speaks over");
                for (int b = 0; b < Vocoder::NUM_BANDS; ++b) expectEquals(fx.bandLevel(b), 0.0f);
                expectEquals(rms(out.l, 0, second), 0.0f);
                // ... and it speaks again at once.
                const Stereo again = run(fx, car, &mod, 512);
                expect(rms(again.l, 2400, 9600) > 0.05f);
            }
        }

        beginTest("A modulator or a carrier that dies away without ever being 0 ends: under -200 dB it is none, and nothing runs for it");
        {
            // (A source's tail that halves for ever, or one in the smallest numbers, kept every mode
            // running at the full price: review of U2.)
            for (int mode = 0; mode < 2; ++mode)
                for (float level : { 1.0e-39f, 1.0e-30f, 1.0e-12f })
                {
                    Vocoder fx;
                    fx.prepare(sr, 512);
                    fx.setParameter(Vocoder::Mode, (float) mode);
                    fx.reset();
                    const Signal loud = whiteNoise(second / 2, 1.0f, 77u), faint = whiteNoise(second, level, 78u);
                    const Signal car(carrier.begin(), carrier.begin() + second / 2);
                    const juce::String what = "mode " + juce::String(mode) + ", " + juce::String(level) + ": ";
                    // The modulator: spoken into, then all but silent.
                    run(fx, car, &loud, 512);
                    Stereo out;
                    for (int k = 0; k < 6; ++k) out = run(fx, carrier, &faint, 512);
                    expectEquals(fx.modesAwake(), 0, what + "nothing runs under a modulator that is all but silent");
                    for (int b = 0; b < Vocoder::NUM_BANDS; ++b) expectEquals(fx.bandLevel(b), 0.0f);
                    expectEquals(rms(out.l, 0, second), 0.0f);
                    // The carrier: all but silent under a modulator that speaks; only the analysis runs.
                    const Signal speaks = whiteNoise(second, 1.0f, 79u);
                    for (int k = 0; k < 6; ++k) out = run(fx, faint, &speaks, 512);
                    expectEquals(fx.modesAwake(), 2 * Vocoder::NUM_BANDS, what + "only the analysis runs over a carrier that is all but silent");
                    expectEquals(rms(out.l, 0, second), 0.0f);
                }
            // ... and what is faint but there (-160 dB) is still a modulator.
            Vocoder fx;
            fx.prepare(sr, 512);
            const Signal faint = whiteNoise(second, 1.0e-8f, 78u);
            const Stereo out = run(fx, carrier, &faint, 512);
            expect(rms(out.l, second / 2, second / 2) > 0.0f && fx.modesAwake() == 6 * Vocoder::NUM_BANDS);
        }

        beginTest("The source taken away and given back: nothing of before is heard (no old tail, no old vowel); a frozen spectrum is kept");
        {
            for (int mode = 0; mode < 2; ++mode)
            {
                Vocoder fx;
                tuned(fx, sr, (float) res::Bell, 10.0f, 0.0f);
                fx.setParameter(Vocoder::Mode, (float) mode);
                fx.setExtra(Vocoder::Release, 500.0f);
                fx.reset();
                const Signal mod = whiteNoise(second / 2, 1.0f, 77u);
                const Signal car(carrier.begin(), carrier.begin() + second / 2);
                const juce::String what = "mode " + juce::String(mode) + ": ";
                const Stereo spoke = run(fx, car, &mod, 512);
                expect(rms(spoke.l, second / 4, second / 4) > 0.05f);
                // The source goes (SRC OFF): the carrier passes, and time stands still in here.
                const Stereo off = run(fx, car, nullptr, 512);
                expect(Signal(off.l.begin() + 4800, off.l.end()) == Signal(car.begin() + 4800, car.end()), what + "no source: the carrier");
                expectEquals(fx.modesAwake(), 0, what + "nothing is kept ringing");
                for (int b = 0; b < Vocoder::NUM_BANDS; ++b) expectEquals(fx.bandLevel(b), 0.0f);
                // It comes back, silent: silence once the carrier has faded out (5 ms), not the
                // tail (RING 10 s) or the levels (REL 500 ms) of the moment it went.
                const Stereo back = run(fx, car, &silence, 512);
                expectEquals(rms(back.l, 2400, second), 0.0f, what + "silence after the source came back silent");
                // ... and it is as new: the same again as the first time.
                Vocoder fresh;
                tuned(fresh, sr, (float) res::Bell, 10.0f, 0.0f);
                fresh.setParameter(Vocoder::Mode, (float) mode);
                fresh.setExtra(Vocoder::Release, 500.0f);
                fresh.reset();
                run(fresh, car, &silence, 512);
                run(fx, car, nullptr, 512);
                run(fx, car, &silence, 512);
                expect(run(fx, car, &mod, 512).l == run(fresh, car, &mod, 512).l, what + "as new after the source was away");

                // Switched off and on again (an effect the chain has faded out is not run): the same.
                run(fx, car, &mod, 512);
                fx.setEnabled(false);
                expect(run(fx, car, &mod, 512).l == car, what + "switched off: the carrier");
                expectEquals(fx.modesAwake(), 0, what + "switched off: nothing is kept ringing");
                fx.setEnabled(true);
                const Stereo on = run(fx, car, &silence, 512);
                expectEquals(rms(on.l, 0, second), 0.0f, what + "silence after it was switched on under a silent modulator");
                run(fx, car, &mod, 512);

                // Frozen: the levels are the player's, and stay.
                fx.setExtra(Vocoder::Freeze, 1.0f);
                float levels[Vocoder::NUM_BANDS];
                for (int b = 0; b < Vocoder::NUM_BANDS; ++b) levels[b] = fx.bandLevel(b);
                expect(levels[8] > 0.0f);
                run(fx, car, nullptr, 512);
                for (int b = 0; b < Vocoder::NUM_BANDS; ++b) expectEquals(fx.bandLevel(b), levels[b]);
                const Stereo held = run(fx, car, &silence, 512);
                expect(rms(held.l, second / 4, second / 4) > 0.02f, what + "the frozen spectrum still speaks");
            }
        }

        beginTest("TUNE: what the carrier holds under the note (0 Hz, a bass) does not come through; a partial rings as loud as it did");
        {
            // (The body was driven by the carrier itself, and a mode driven by a sound is a low-pass:
            // 0.1 of 0 Hz came out as 0.085 to 0.3 of 0 Hz, a 40 Hz tone under a body at 440 Hz at
            // -16 dB with nothing of the body in it. Review of U2: the body takes the difference.)
            auto level = [&] (const Signal& car, float hz, float ring, double toneHz, double* mean)
            {
                Vocoder fx;
                tuned(fx, sr, (float) res::String, ring, hz);
                const Stereo out = run(fx, car, &longWhite);
                double re = 0.0, im = 0.0, sum = 0.0;
                for (int i = second; i < two; ++i)
                {
                    const double v = out.l[(size_t) i], a = 6.283185307179586 * toneHz * i / sr;
                    re += v * std::cos(a);
                    im += v * std::sin(a);
                    sum += v;
                }
                if (mean != nullptr) *mean = sum / second;
                return std::make_pair(rms(out.l, second, second), (float) (std::sqrt(2.0) * std::sqrt(re * re + im * im) / second));
            };
            const Signal steady((size_t) two, 0.1f), bass = sine(two, 40.0, sr, 0.5f);
            for (float ring : { 0.02f, 0.4f })
            {
                double mean = 1.0;
                const float out = level(steady, 110.0f, ring, 40.0, &mean).first;
                logMessage("RING " + juce::String(ring, 2) + ", a carrier of 0.1 at 0 Hz: " + juce::String(levelDb(out, 0.1f), 1)
                           + " dB against it, mean " + juce::String(mean, 6));
                expect(out < 1.0e-4f && std::abs(mean) < 1.0e-4, "0 Hz in the carrier is not heard");
            }
            const float under = level(bass, 440.0f, 0.4f, 40.0, nullptr).second;
            logMessage("a 40 Hz carrier under a body at 440 Hz comes out at " + juce::String(levelDb(under, 0.3536f), 1) + " dB against it");
            expect(levelDb(under, 0.3536f) < -20.0f, "a bass under the note is not passed on");
            // ... and a carrier at a partial's own frequency rings it as it did (measured before the
            // change: -1.3 dB against the carrier at 440 Hz, RING 0.4 s, under white noise).
            const float on = level(sine(two, 440.0, sr, 0.1f), 440.0f, 0.4f, 440.0, nullptr).second;
            logMessage("a carrier at the note itself: " + juce::String(levelDb(on, 0.0707f), 1) + " dB against it");
            expect(levelDb(on, 0.0707f) > 10.0f, "the note's own frequency rings");
        }

        beginTest("TUNE: the levels go with the modulator's frequencies, not with the partials' numbers, when the note moves; the outermost bands");
        {
            // A modulator in band 12 only, a comb; the note two octaves up while it sounds: what
            // sounds is still around the modulator (the partials there are others now).
            const double hz = Vocoder::bandHz(12);
            const Signal tone = sine(two, hz, sr, 0.5f);
            Vocoder fx;
            tuned(fx, sr, (float) res::Comb, 0.3f, 110.0f);
            const Stereo out = run(fx, longCarrier, longCarrier, &tone, nullptr, 256, [&] (int pos) { if (pos >= second / 2) fx.setPitch(440.0f); });
            const auto p = spectrum(out.l, bigFrom, bigFft);
            logMessage("the note from 110 to 440 Hz under a modulator at " + juce::String(hz, 0) + " Hz: "
                       + juce::String(100.0 * share(p, sr, hz / 1.5, hz * 1.5), 1) + " % within a fifth of it");
            expect(share(p, sr, hz / 1.5, hz * 1.5) > 0.8);

            // The lowest and the highest band reach the partials there (and only those).
            for (int b : { 0, Vocoder::NUM_BANDS - 1 })
            {
                const double at = Vocoder::bandHz(b);
                Vocoder edge;
                tuned(edge, sr, (float) res::Comb, 0.3f, b == 0 ? 100.0f : 1000.0f);
                const Signal there = sine(two, at, sr, 0.5f);
                const Stereo spoken = run(edge, longCarrier, &there);
                const auto q = spectrum(spoken.l, bigFrom, bigFft);
                // (The partials above the highest band take its level, all of them.)
                const double there2 = share(q, sr, at / 1.6, b == 0 ? at * 1.6 : 0.5 * sr);
                logMessage("a modulator at " + juce::String(at, 0) + " Hz: " + juce::String(100.0 * there2, 1)
                           + " % around it, " + juce::String(levelDb(rms(spoken.l, second, second), 1.0f), 1) + " dB");
                expect(there2 > 0.8 && rms(spoken.l, second, second) > 0.05f);
            }
            // ... louder than a band in the middle does its own, having every partial above it too
            // (a quiet modulator: nothing is held).
            float loud[2] = {};
            for (int k = 0; k < 2; ++k)
            {
                Vocoder edge;
                tuned(edge, sr, (float) res::Comb, 0.3f, 1000.0f);
                const Signal there = sine(two, (double) Vocoder::bandHz(k == 0 ? 12 : Vocoder::NUM_BANDS - 1), sr, 0.01f);
                loud[k] = rms(run(edge, longCarrier, &there).l, second, second);
            }
            logMessage("a quiet modulator in band 12: " + juce::String(levelDb(loud[0], 1.0f), 1) + " dB, in band 15: " + juce::String(levelDb(loud[1], 1.0f), 1) + " dB");
            expect(loud[0] > 1.0e-3f && loud[0] < 0.5f && loud[1] > 1.5f * loud[0] && loud[1] < 6.0f * loud[0]);
        }

        beginTest("BAND glides (an octave in 30 ms) and NOIS fades in (3 ms); MIX 0 is the carrier whatever it holds; "
                  "no band above 0.45 of the sample rate");
        {
            {
                Vocoder fx;
                tuned(fx, sr, (float) res::String, 0.4f, 0.0f);
                const Signal mod = whiteNoise(second / 2, 1.0f, 77u);
                const Signal car(carrier.begin(), carrier.begin() + second / 2);
                run(fx, car, &mod, 64);
                fx.setParameter(Vocoder::Band, 24.0f);
                run(fx, Signal(car.begin(), car.begin() + 960), &mod, 64);       // 20 ms on
                const float between = fx.tunedHz();
                run(fx, car, &mod, 64);
                logMessage("BAND from 0 to 24: " + juce::String(between, 1) + " Hz after 20 ms, " + juce::String(fx.tunedHz(), 1) + " Hz in the end");
                expect(between > 1.3f * Vocoder::DEFAULT_HZ && between < 3.0f * Vocoder::DEFAULT_HZ, "on the way after 20 ms");
                expectWithinAbsoluteError(fx.tunedHz(), 4.0f * Vocoder::DEFAULT_HZ, 0.5f);
            }
            {
                // A silent carrier under a steady modulator in a high band (which answers at once).
                Vocoder fx;
                fx.prepare(sr, 16);
                const Signal mod = sine(second / 2, (double) Vocoder::bandHz(14), sr, 0.5f);
                const Signal none((size_t) second / 2, 0.0f);
                const int at = second / 4;
                const Stereo out = run(fx, none, none, &mod, nullptr, 16, [&] (int pos) { if (pos == at) fx.setParameter(Vocoder::Noise, 1.0f); });
                const float first = rms(out.l, at, 24), later = rms(out.l, at + 2400, 4800);
                logMessage("NOIS thrown to 100: the first half millisecond " + juce::String(levelDb(first, later), 1) + " dB against what follows");
                expect(later > 0.01f && first < 0.3f * later, "it fades in");
                expectEquals(rms(out.l, 0, at), 0.0f);
            }
            {
                Vocoder fx;
                fx.prepare(sr, 256);
                fx.setParameter(Vocoder::Mix, 0.0f);
                Signal car(carrier.begin(), carrier.begin() + 4096);
                car[100] = std::numeric_limits<float>::quiet_NaN();
                car[200] = std::numeric_limits<float>::infinity();
                car[300] = -0.0f;
                const Signal mod = whiteNoise(4096, 1.0f, 77u);
                const Stereo out = run(fx, car, &mod);
                expect(std::memcmp(out.l.data(), car.data(), car.size() * sizeof(float)) == 0, "MIX 0: the carrier bit for bit, also where it is no number");
            }
            {
                // 22.05 kHz, BAND +12: band 15 would be at 16 kHz, above half the rate, and come
                // back down at 6 kHz; it is held at 0.45 of the rate (9.9 kHz).
                const double low = 22050.0;
                Vocoder fx;
                fx.prepare(low, 256);
                fx.setParameter(Vocoder::Band, 12.0f);
                fx.reset();
                const Signal mod = sine(22050, 8000.0, low, 0.5f), car = whiteNoise(22050, 0.5f, 11u);
                const auto p = spectrum(run(fx, car, &mod).l, 22050 - 8192 - 256, 8192);
                logMessage("22.05 kHz, BAND +12, a modulator at 8 kHz: " + juce::String(100.0 * share(p, low, 8000.0, 11025.0), 1) + " % above 8 kHz, "
                           + juce::String(100.0 * share(p, low, 4500.0, 7500.0), 1) + " % around 6 kHz");
                expect(share(p, low, 8000.0, 11025.0) > 0.7 && share(p, low, 4500.0, 7500.0) < 0.15);
            }
        }

        beginTest("res::Resonator::setSpectrum: a body that stands still takes its first spectrum; no number and too much are held; none again is the body as it was");
        {
            auto render = [&] (int spectrumAt, const float* gains, bool takeBack)
            {
                res::Resonator r;
                r.prepare(sr);
                r.setBody((float) res::Comb);
                r.setExciter(res::ExciteLatent { { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f } });
                r.setDecay(0.3f);
                r.setPitch(220.0);
                uint32_t seed = 3u;
                Signal out((size_t) second);
                for (int i = 0; i < second; ++i)
                {
                    if (i == spectrumAt) r.setSpectrum(gains);
                    if (takeBack && i == 3 * second / 4) r.setSpectrum(nullptr);
                    out[(size_t) i] = r.tick(0.2f * dsp::xorshiftNoise(seed), 0.0f);
                }
                return out;
            };
            float none[res::MODES] = {}, wild[res::MODES];
            for (int n = 0; n < res::MODES; ++n)
                wild[n] = n % 3 == 0 ? std::numeric_limits<float>::quiet_NaN() : (n % 3 == 1 ? std::numeric_limits<float>::infinity() : -1.0e30f);
            const Signal plain = render(-1, nullptr, false);
            // The body has long stood still (a quarter of a second) when the spectrum comes.
            const Signal muted = render(second / 4, none, false);
            expect(rms(plain, second / 2, second / 4) > 0.01f);
            expect(Signal(muted.begin(), muted.begin() + second / 4) == Signal(plain.begin(), plain.begin() + second / 4), "none given: the body as it is");
            logMessage("every gain 0, given to a body at rest: " + juce::String(levelDb(rms(muted, 3 * second / 4, second / 4), rms(plain, 3 * second / 4, second / 4)), 1) + " dB");
            expect(rms(muted, 3 * second / 4, second / 4) < 0.01f * rms(plain, 3 * second / 4, second / 4), "the first spectrum is taken by a body that does not move");
            const Signal back = render(second / 4, none, true);
            expect(rms(back, second - 2400, 2400) > 0.5f * rms(plain, second - 2400, 2400), "nullptr: the body again");
            const Signal held = render(second / 4, wild, false);
            float peak = 0.0f;
            bool finite = true;
            for (float v : held) { finite = finite && std::isfinite(v); peak = std::max(peak, std::abs(v)); }
            expect(finite && peak <= res::Resonator::OUT_LIMIT, "gains that are no number, infinite, below 0: finite, peak " + juce::String(peak, 3));
            // (... and held where they come in: the partials given too much go on sounding, the body is not emptied.)
            int sounding = 0;
            for (int k = second / 2; k < second; k += 480) sounding += rms(held, k, 480) > 0.01f ? 1 : 0;
            expectEquals(sounding, 50, "it sounds throughout");
        }

        beginTest("The time one block takes (this build; the optimised numbers are in the commit)");
        {
            const Signal mod = whiteNoise(512, 0.5f, 1u), none(512, 0.0f);
            const double blockUs = 512.0 / sr * 1.0e6;
            for (int mode = 0; mode < 2; ++mode)
                for (int speaking = 1; speaking >= 0; --speaking)
                {
                    Vocoder fx;
                    fx.prepare(sr, 512);
                    fx.setParameter(Vocoder::Mode, (float) mode);
                    fx.reset();
                    Signal l(512), r(512);
                    const Signal car = whiteNoise(512, 0.5f, 2u);
                    const int blocks = 40;
                    double us = 0.0;
                    for (int round = 0; round < 2; ++round)         // (the first round lets a silent one come to rest)
                    {
                        const auto start = std::chrono::steady_clock::now();
                        for (int i = 0; i < blocks; ++i)
                        {
                            l = car; r = car;
                            fx.setInput(speaking ? mod.data() : none.data(), speaking ? mod.data() : none.data(), 512);
                            fx.processBlock(l.data(), r.data(), 512);
                        }
                        us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / blocks;
                    }
                    logMessage(juce::String(mode == 0 ? "BAND" : "TUNE") + (speaking ? ", spoken into" : ", modulator silent")
                               + ": one block of 512 at 48 kHz takes " + juce::String(us, 0) + " us in this build ("
                               + juce::String(100.0 * us / blockUs, 1) + " % of the block's time), " + juce::String(fx.modesAwake()) + " modes run");
                    expect(us < blockUs, "less than the block lasts, even unoptimised");
                }
        }
    }
};

static VocoderTests vocoderTests;
