#pragma once
#include <juce_core/juce_core.h>
#include <chrono>
#include <cstring>
#include "../src/engine/AudioEngine.h"
#include "../src/engine/ResonatorEngine.h"
#include "../src/project/presets/ResonatorPresets.h"
#include "fast_measure.h"

// The resonator and its engine (dsp/Resonator.h, dsp/ModeBank.h, ResonatorEngine.h): one body and
// one exciter computed from latent parameters, six and four named points, positions between them.
// The first half measures the class by itself, the second the engine around it.
class ResonatorTests : public juce::UnitTest
{
public:
    ResonatorTests() : UnitTest("Resonator") {}

    using E = ResonatorEngine;
    using V = ResonatorVoice;
    using R = res::Resonator;

    static const char* modelName(int m) { return E::bodyName(m); }
    static const char* exciterName(int x) { return E::exciterName(x); }
    static float dB(double v) { return (float) (20.0 * std::log10(std::max(v, 1.0e-12))); }
    static double noteHz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }

    static double rms(const std::vector<float>& x, size_t from = 0, size_t n = (size_t) -1)
    {
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = from; i < x.size() && count < n; ++i, ++count)
            sum += (double) x[i] * x[i];
        return count > 0 ? std::sqrt(sum / (double) count) : 0.0;
    }
    static double peak(const std::vector<float>& x, size_t from = 0, size_t n = (size_t) -1)
    {
        double most = 0.0;
        size_t count = 0;
        for (size_t i = from; i < x.size() && count < n; ++i, ++count)
            most = std::max(most, (double) std::abs(x[i]));
        return most;
    }
    static bool finite(const std::vector<float>& x)
    {
        for (float v : x)
            if (! std::isfinite(v)) return false;
        return true;
    }

    // How far, in cents, the strongest frequency near `hz` is from it (searched +-span cents).
    static double centsOff(const std::vector<float>& x, size_t from, size_t n, double hz, double sr, double span = 70.0)
    {
        auto power = [&](double cents) { return fastmeasure::goertzel(x, from, n, hz * std::exp2(cents / 1200.0), sr); };
        double best = 0.0, bestPower = -1.0;
        for (double c = -span; c <= span; c += 2.0)
        {
            const double p = power(c);
            if (p > bestPower) { bestPower = p; best = c; }
        }
        double fine = best;
        for (double c = best - 2.0; c <= best + 2.0; c += 0.125)
        {
            const double p = power(c);
            if (p > bestPower) { bestPower = p; fine = c; }
        }
        return fine;
    }
    // The level of what sounds at hz (the root of its Goertzel power).
    static double levelAt(const std::vector<float>& x, size_t from, size_t n, double hz, double sr)
    {
        return std::sqrt(fastmeasure::goertzel(x, from, n, hz, sr));
    }

    // ---------------------------------------------------------------------------------------
    // The class by itself: one resonator, an energy that is held and then let go.
    struct Solo
    {
        R r;
        double sr;
        explicit Solo(double rate = 48000.0) : sr(rate) { r.prepare(rate); }

        Solo& sound(float body, float exciter, double hz = 220.0, float decay = 2.0f)
        {
            r.setBody(body);
            r.setExciter(exciter);
            r.setPitch(hz);
            r.setDecay(decay);
            return *this;
        }
        std::vector<float> run(double seconds, float energy = 0.0f)
        {
            std::vector<float> out((size_t) std::lround(seconds * sr));
            for (auto& v : out)
                v = r.tick(0.0f, energy);
            return out;
        }
        std::vector<float> play(double seconds, float strike = 0.5f, float energy = 0.5f)
        {
            r.strike(strike);
            return run(seconds, energy);
        }
    };

    static res::BodyLatent bodyOffset(int which, float value)
    {
        res::BodyLatent o {};
        o[(size_t) which] = value;
        return o;
    }
    static res::ExciteLatent exciteOffset(int which, float value)
    {
        res::ExciteLatent o {};
        o[(size_t) which] = value;
        return o;
    }

    // ---------------------------------------------------------------------------------------
    void latentSpace()
    {
        beginTest("the latent space: fourteen bodies and five exciters as points; a position is the point between two neighbours, "
                  "latent by latent, plus the offsets, held to 0..1; values no position can have give a point");
        {
            const auto& bodies = res::bodyPoints();
            const auto& exciters = res::excitePoints();
            // (U2b: HEAD and the seven bodies with partials placed by hand after BELL, BRSH after SCRP.)
            expectEquals((int) bodies.size(), 14);
            expectEquals((int) exciters.size(), 5);
            for (int p = 0; p < (int) bodies.size(); ++p)
                expect(res::latentAt(bodies, (float) p, {}) == bodies[(size_t) p], juce::String(modelName(p)) + " is its point");
            for (int p = 0; p < (int) exciters.size(); ++p)
                expect(res::latentAt(exciters, (float) p, {}) == exciters[(size_t) p], juce::String(exciterName(p)) + " is its point");
            // Half-way from the string to the tube: half the string's stiffness, half the tube's odd.
            const auto half = res::latentAt(bodies, 0.5f, {});
            expectWithinAbsoluteError(half[res::Stiff], 0.5f * bodies[res::String][res::Stiff], 1.0e-6f);
            expectWithinAbsoluteError(half[res::Odd], 0.5f, 1.0e-6f);
            // The points differ: no two bodies and no two exciters are the same place.
            // (The bodies placed by hand differ in their tables, and all in the second eight latents.)
            for (int a = 0; a < (int) bodies.size(); ++a)
                for (int b = a + 1; b < (int) bodies.size(); ++b)
                    expect(bodies[(size_t) a] != bodies[(size_t) b] || res::morePoints()[(size_t) a] != res::morePoints()[(size_t) b]
                           || res::handOf(a) != res::handOf(b), juce::String(modelName(a)) + " and " + modelName(b));
            for (int a = 0; a < (int) exciters.size(); ++a)
                for (int b = a + 1; b < (int) exciters.size(); ++b)
                    expect(exciters[(size_t) a] != exciters[(size_t) b]);
            // What makes each what it is.
            expect(bodies[res::Tube][res::Odd] == 1.0f && bodies[res::Plate][res::Sheet] == 1.0f && bodies[res::Bar][res::Stiff] == 1.0f
                   && bodies[res::Bell][res::Shell] == 1.0f && bodies[res::Bell][res::Hum] > 0.0f && bodies[res::Comb][res::Damp] == 0.0f);
            expect(exciters[res::Strike][res::Cont] == 0.0f && exciters[res::Strike][res::Couple] == 0.0f
                   && exciters[res::Scrape][res::Couple] == 0.0f && exciters[res::Bow][res::Couple] > 0.5f
                   && exciters[res::Breath][res::Couple] > 0.5f && exciters[res::Bow][res::Shape] == 0.0f
                   && exciters[res::Breath][res::Shape] == 1.0f && exciters[res::Breath][res::Grain] == 1.0f);
            // Offsets add and are held.
            expectWithinAbsoluteError(res::latentAt(bodies, 0.0f, bodyOffset(res::Hum, 0.3f))[res::Hum], 0.3f, 1.0e-6f);
            expectWithinAbsoluteError(res::latentAt(bodies, 1.0f, bodyOffset(res::Odd, 0.7f))[res::Odd], 1.0f, 1.0e-6f);
            expectWithinAbsoluteError(res::latentAt(bodies, 1.0f, bodyOffset(res::Odd, -3.0f))[res::Odd], 0.0f, 1.0e-6f);
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            expect(res::latentAt(bodies, nan, {}) == bodies[0] && res::latentAt(bodies, -inf, {}) == bodies[0]
                   && res::latentAt(bodies, inf, {}) == bodies.back() && res::latentAt(bodies, 1.0e30f, {}) == bodies.back());
            expect(res::latentAt(bodies, 2.0f, bodyOffset(res::Tilt, nan)) == bodies[2], "an offset that is no number is no offset");
        }

        beginTest("the mode bank: a mode rings at its frequency and falls by its decay; retuned while it rings it keeps what it "
                  "holds; a mode that leaves fades and is put away; nothing in it grows by itself");
        {
            dsp::ModeBank bank;
            float c[64] {}, s[64] {}, in[64] {}, out[64] {};
            const float w = 6.2831855f * 1000.0f / 48000.0f, radius = 0.9999f;
            for (int i = 0; i < 64; ++i) { c[i] = radius * std::cos(w * (float) (i + 1) * 0.3f); s[i] = radius * std::sin(w * (float) (i + 1) * 0.3f); }
            c[0] = radius * std::cos(w);
            s[0] = radius * std::sin(w);
            in[0] = out[0] = 1.0f;
            bank.retarget(c, s, in, out, 1, 0);
            expectEquals(bank.running, 8, "modes run eight at a time");
            float contact = 0.0f;
            std::vector<float> x(48000);
            x[0] = bank.tick(1.0f, &contact);
            for (size_t i = 1; i < x.size(); ++i) x[i] = bank.tick(0.0f, &contact);
            expect(std::abs(centsOff(x, 0, 24000, 1000.0, 48000.0)) < 0.5, "1000 Hz");
            const double fall = rms(x, 24000, 4800) / rms(x, 0, 4800);
            expectWithinAbsoluteError(dB(fall), dB(std::pow((double) radius, 24000.0)), 0.3f, "falls by its radius a sample");

            // Retuned over 480 samples to twice the frequency: what it holds is still there.
            const float before = bank.energy();
            c[0] = radius * std::cos(2.0f * w);
            s[0] = radius * std::sin(2.0f * w);
            bank.retarget(c, s, in, out, 1, 480);
            float most = 0.0f;
            for (int i = 0; i < 480; ++i) { bank.tick(0.0f, &contact); most = std::max(most, bank.energy()); }
            expect(most <= before, "it never holds more than before the move");
            expect(bank.energy() > 0.85f * before, "and has lost only what 480 samples of ringing cost: " + juce::String(bank.energy() / before, 3));
            for (auto& v : x) v = bank.tick(0.0f, &contact);
            expect(std::abs(centsOff(x, 0, 24000, 2000.0, 48000.0)) < 0.5, "2000 Hz after the move");

            // Sixteen modes, then two: the fourteen that leave fade over the glide and are put away.
            for (int i = 0; i < 16; ++i) in[i] = out[i] = 1.0f;
            bank.retarget(c, s, in, out, 16, 0);
            bank.tick(1.0f, &contact);
            expectEquals(bank.running, 16);
            bank.retarget(c, s, in, out, 2, 64);
            expectEquals(bank.running, 16, "they run on while they fade");
            for (int i = 0; i < 64; ++i) bank.tick(0.0f, &contact);
            expectEquals(bank.running, 8);
            for (int i = 8; i < 64; ++i)
                expect(bank.x[i] == 0.0f && bank.y[i] == 0.0f && bank.in[i] == 0.0f);

            // Coefficients thrown about every few samples, radius 1 at most: what it holds never grows.
            juce::Random rng(5);
            for (int i = 0; i < 64; ++i) in[i] = out[i] = 1.0f;
            bank.retarget(c, s, in, out, 64, 0);
            for (int i = 0; i < 64; ++i) bank.x[i] = 1.0f;
            bank.wake();
            float held = bank.energy();
            bool grew = false;
            for (int n = 0; n < 4000; ++n)
            {
                for (int i = 0; i < 64; ++i)
                {
                    const float a = rng.nextFloat() * 3.1f;
                    c[i] = 0.99999f * std::cos(a);
                    s[i] = 0.99999f * std::sin(a);
                }
                bank.retarget(c, s, in, out, 64, 1 + n % 3);
                for (int k = 0; k < 1 + n % 3; ++k) bank.tick(0.0f, &contact);
                if (held > 1.0e-20f && bank.energy() > held * 1.00001f) grew = true;
                held = bank.energy();
            }
            expect(! grew, "retuned at random every few samples: no growth");

            // Rest: 64 modes struck, the upper 48 dying a thousand times as fast - soon only the 16 that
            // still ring are run; struck again, all 64 are; left alone it ends in true silence, not in
            // numbers too small to stop.
            for (int i = 0; i < 64; ++i)
            {
                const float r = i < 16 ? 0.99999f : 0.99f;
                c[i] = r * std::cos(0.02f * (float) (i + 1));
                s[i] = r * std::sin(0.02f * (float) (i + 1));
            }
            bank.reset();
            bank.retarget(c, s, in, out, 64, 0);
            expectEquals(bank.awake, 0, "silent: nothing runs");
            bank.tick(1.0f);
            expectEquals(bank.awake, 64, "struck: all of it");
            for (int i = 0; i < 4800; ++i) bank.tick(0.0f);
            const float all = bank.energy();
            expectWithinAbsoluteError(bank.rest(1.0e-14f), all, 1.0e-6f, "rest gives what the bank holds");
            expectEquals(bank.awake, 16, "after a tenth of a second the upper 48 have rung out and rest");
            for (int i = 16; i < 64; ++i)
                expect(bank.x[i] == 0.0f && bank.y[i] == 0.0f);
            bank.tick(0.5f);
            expectEquals(bank.awake, 64, "struck again");
            for (int i = 0; i < 3000000 && bank.awake > 0; ++i) { bank.tick(0.0f); if (i % 64 == 0) bank.rest(1.0e-14f); }
            expectEquals(bank.awake, 0, "left alone: silence");
            expect(bank.energy() == 0.0f && bank.tick(0.0f) == 0.0f, "and nothing at all in it");
        }
    }

    // ---------------------------------------------------------------------------------------
    // The frequency of the partial expected near `ratio` times the note, as a ratio, and its level.
    struct Partial { double ratio = 0.0, level = 0.0; };
    static Partial partialNear(const std::vector<float>& x, double hz, double ratio, double sr, double span = 40.0)
    {
        const size_t from = (size_t) (0.02 * sr), n = (size_t) (0.5 * sr);
        const double off = centsOff(x, from, n, hz * ratio, sr, span);
        Partial p;
        p.ratio = ratio * std::exp2(off / 1200.0);
        p.level = levelAt(x, from, n, hz * p.ratio, sr);
        return p;
    }

    void character()
    {
        const double hz = 220.0, sr = 48000.0;
        auto struck = [&](float body, const res::BodyLatent& offset = {})
        {
            Solo s;
            s.r.setBody(body, offset);
            s.r.setExciter((float) res::Strike, exciteOffset(res::Hard, 1.0f));     // a bright strike: every partial is reached
            s.r.setPitch(hz);
            s.r.setDecay(4.0f);
            return s.play(0.6, 1.0f, 0.0f);
        };

        beginTest("STRG: every harmonic, a little stretched as a stiff string's are (n sqrt(1 + B n^2)), the high ones weaker; "
                  "the place it is touched takes its own partials out");
        {
            const auto x = struck((float) res::String);
            juce::String found;
            double last = 1.0e9;
            for (int n = 1; n <= 8; ++n)
            {
                if (n == 5) continue;       // (where it is touched: below)
                const auto p = partialNear(x, hz, (double) n, sr);
                found << " " << juce::String(p.ratio, 4) << " (" << juce::String(dB(p.level), 0) << " dB)";
                expect(std::abs(p.ratio / n - 1.0) < 0.004, "partial " + juce::String(n) + " at " + juce::String(p.ratio, 4));
                expect(p.ratio >= n * 0.9999, "partial " + juce::String(n) + " is not flat: " + juce::String(p.ratio, 4));
                if (n <= 4)
                {
                    expect(p.level < last, "partial " + juce::String(n) + " is weaker than the one below");
                    last = p.level;
                }
            }
            logMessage("STRG partials:" + found);
            // POS 0.25 on the string: the fifth partial is where it is touched (cos(4 x 0.25 x pi / 2) = 0).
            expect(levelAt(x, 960, 24000, hz * 5.0, sr) < 0.01 * levelAt(x, 960, 24000, hz * 3.0, sr), "the fifth is not reached");
            const auto open = struck((float) res::String, bodyOffset(res::Pos, -1.0f));
            expect(partialNear(open, hz, 5.0, sr).level > 0.05 * partialNear(open, hz, 3.0, sr).level, "touched at the end, it is");
        }

        beginTest("TUBE: the odd harmonics only (a tube closed at one end); ODD taken away, the even ones are back");
        {
            const auto x = struck((float) res::Tube);
            for (int n : { 2, 4, 6 })
            {
                const double even = levelAt(x, 960, 24000, hz * n, sr), odd = levelAt(x, 960, 24000, hz * (n + 1), sr);
                expect(even < 0.003 * odd, "harmonic " + juce::String(n) + " is " + juce::String(dB(even / odd), 0) + " dB under harmonic "
                       + juce::String(n + 1));
            }
            for (int n : { 3, 5, 7 })
                expect(std::abs(partialNear(x, hz, (double) n, sr).ratio / n - 1.0) < 0.001, "harmonic " + juce::String(n) + " in place");
            const auto open = struck((float) res::Tube, bodyOffset(res::Odd, -1.0f));
            expect(partialNear(open, hz, 2.0, sr).level > 0.3 * partialNear(open, hz, 3.0, sr).level, "ODD off: the second harmonic sounds");
        }

        beginTest("COMB: every harmonic, exactly in place, as loud as the first and ringing as long (nothing damped)");
        {
            Solo s;
            s.r.setBody((float) res::Comb);
            s.r.setExciter((float) res::Strike, exciteOffset(res::Hard, 1.0f));
            s.r.setPitch(hz);
            s.r.setDecay(1.0f);
            const auto x = s.play(1.0, 1.0f, 0.0f);
            const double first = partialNear(x, hz, 1.0, sr).level;
            for (int n = 2; n <= 12; ++n)
            {
                const auto p = partialNear(x, hz, (double) n, sr);
                expect(std::abs(p.ratio / n - 1.0) < 0.0005, "harmonic " + juce::String(n) + " at " + juce::String(p.ratio, 4));
                expect(p.level > 0.5 * first && p.level < 2.0 * first, "harmonic " + juce::String(n) + ": " + juce::String(dB(p.level / first), 1) + " dB");
            }
            // The twelfth falls as the first does: by 30 dB in half of DCAY.
            for (double ratio : { 1.0, 12.0 })
            {
                const double early = levelAt(x, 2400, 9600, hz * ratio, sr), late = levelAt(x, 26400, 9600, hz * ratio, sr);
                expectWithinAbsoluteError(dB(late / early), -30.0f, 1.5f, "harmonic " + juce::String(ratio, 0) + " over half a second of DCAY 1");
            }
        }

        beginTest("PLAT, BAR and BELL are three different series: a plate's (m^2 + (n / 1.37)^2), a free bar's (1, 2.757, 5.404, 8.933) "
                  "and a bell's (hum 0.5, prime 1, tierce 1.183, quint 1.506, nominal 2); each has its own partials and not the others'");
        {
            const auto plate = struck((float) res::Plate), bar = struck((float) res::Bar), bell = struck((float) res::Bell);
            const double side = 1.0 / (1.37 * 1.37), base = 1.0 + side;
            const double plateSeries[] = { (1.0 + 4.0 * side) / base, (4.0 + side) / base, (1.0 + 9.0 * side) / base, (4.0 + 4.0 * side) / base };
            const double barSeries[] = { 2.757, 5.404, 8.933 };       // ((2n + 1) / 3.0112)^2
            const double bellSeries[] = { 0.5, 1.183, 1.506, 2.0, 2.514 };
            juce::String found = "PLAT";
            for (double r : plateSeries)
            {
                const auto p = partialNear(plate, hz, r, sr, 25.0);
                found << " " << juce::String(p.ratio, 3);
                expect(std::abs(p.ratio / r - 1.0) < 0.002 && p.level > 0.05 * partialNear(plate, hz, 1.0, sr).level, "plate partial " + juce::String(r, 3)
                       + ": " + juce::String(p.ratio, 3));
            }
            found << "  BAR";
            for (double r : barSeries)
            {
                const auto p = partialNear(bar, hz, r, sr, 25.0);
                found << " " << juce::String(p.ratio, 3);
                expect(std::abs(p.ratio / r - 1.0) < 0.01, "bar partial " + juce::String(r, 3) + ": " + juce::String(p.ratio, 3) + " (within 1 %)");
            }
            found << "  BELL";
            for (double r : bellSeries)
            {
                const auto p = partialNear(bell, hz, r, sr, 25.0);
                found << " " << juce::String(p.ratio, 3);
                expect(std::abs(p.ratio / r - 1.0) < 0.002 && p.level > 0.05 * partialNear(bell, hz, 1.0, sr).level, "bell partial " + juce::String(r, 3)
                       + ": " + juce::String(p.ratio, 3));
            }
            logMessage(found);
            // Told apart: the bar's second partial is in neither of the others, the bell's tierce and hum in neither, the
            // plate's second in neither.
            auto at = [&](const std::vector<float>& x, double r) { return levelAt(x, 960, 24000, hz * r, sr) / levelAt(x, 960, 24000, hz, sr); };
            expect(at(bar, 2.757) > 0.1 && at(plate, 2.757) < 0.01 && at(bell, 2.757) < 0.02, "2.757 is the bar's");
            expect(at(bell, 1.183) > 0.1 && at(plate, 1.183) < 0.01 && at(bar, 1.183) < 0.01, "1.183 is the bell's");
            expect(at(bell, 0.5) > 0.1 && at(plate, 0.5) < 0.01 && at(bar, 0.5) < 0.01 && at(struck((float) res::String), 0.5) < 0.01, "the hum is the bell's");
            expect(at(plate, plateSeries[0]) > 0.1 && at(bar, plateSeries[0]) < 0.01 && at(bell, plateSeries[0]) < 0.03, "2.043 is the plate's");
            // HUM is a cell of every body: a sub-octave under a string.
            expect(at(struck((float) res::String, bodyOffset(res::Hum, 1.0f)), 0.5) > 0.1, "HUM up on the string: its sub-octave sounds");
        }

        beginTest("DCAY is the ring time of the note itself (60 dB), at every point; DAMP makes the high partials die sooner, TILT "
                  "makes them weaker, STIF moves them up");
        {
            for (int b = 0; b < 6; ++b)
            {
                Solo s;
                s.sound((float) b, (float) res::Strike, hz, 1.0f);
                const auto x = s.play(1.0, 1.0f, 0.0f);
                const double early = levelAt(x, 2400, 9600, hz, sr), late = levelAt(x, 26400, 9600, hz, sr);
                expectWithinAbsoluteError(dB(late / early), -30.0f, 1.5f, juce::String(modelName(b)) + ": 30 dB in half of DCAY");
            }
            const auto flat = struck((float) res::Comb);
            const auto damped = struck((float) res::Comb, bodyOffset(res::Damp, 0.6f));
            auto fall = [&](const std::vector<float>& x, double r) { return dB(levelAt(x, 14400, 9600, hz * r, sr) / levelAt(x, 2400, 9600, hz * r, sr)); };
            expect(fall(damped, 8.0) < fall(flat, 8.0) - 6.0f, "DAMP: the eighth harmonic falls " + juce::String(fall(damped, 8.0), 1) + " dB in a "
                   "quarter second, " + juce::String(fall(flat, 8.0), 1) + " without");
            expectWithinAbsoluteError(fall(damped, 1.0), fall(flat, 1.0), 0.5f, "the note itself rings as long");
            const auto tilted = struck((float) res::Comb, bodyOffset(res::Tilt, 0.5f));
            const double ratio = partialNear(tilted, hz, 8.0, sr).level / partialNear(tilted, hz, 1.0, sr).level;
            expectWithinAbsoluteError(dB(ratio), dB(1.0 / 8.0), 1.0f, "TILT 0.5: the eighth harmonic at an eighth (1 / r)");
            const auto stiff = struck((float) res::Comb, bodyOffset(res::Stiff, 0.25f));
            expect(partialNear(stiff, hz, 4.0, sr, 100.0).ratio > 4.03, "STIF: the fourth partial is sharp");
        }
    }

    // ---------------------------------------------------------------------------------------
    void tuning()
    {
        beginTest("tuning, a strike: within 3 cents of the note from note 24 to 96 at 44.1, 48 and 96 kHz at the six points, and at "
                  "48 kHz at the five places half-way between them as well (the partials move, the pitch does not)");
        {
            double worst = 0.0;
            juce::String where;
            for (double sr : { 44100.0, 48000.0, 96000.0 })
                for (int note : { 24, 31, 36, 48, 60, 67, 72, 84, 96 })
                    for (float body = 0.0f; body <= 5.0f; body += 0.5f)
                    {
                        // (The whole grid at 48 kHz; at the other rates the points, at five notes.)
                        if (sr != 48000.0 && (body != std::floor(body) || note == 31 || note == 48 || note == 67 || note == 84)) continue;
                        const double hz = noteHz(note);
                        Solo s(sr);
                        s.sound(body, (float) res::Strike, hz, 4.0f);
                        const double seconds = std::max(0.4, 60.0 / hz);        // sixty periods or more
                        const auto x = s.play(seconds + 0.05, 1.0f, 0.0f);
                        const double off = centsOff(x, (size_t) (0.02 * sr), (size_t) (seconds * sr), hz, sr, 40.0);
                        if (std::abs(off) > std::abs(worst)) { worst = off; where = juce::String(body, 1) + " note " + juce::String(note) + " at " + juce::String(sr, 0); }
                        expect(std::abs(off) < 3.0, "body " + juce::String(body, 1) + " note " + juce::String(note) + " at " + juce::String(sr, 0) + " Hz: "
                               + juce::String(off, 2) + " cents");
                    }
            logMessage("worst: " + juce::String(worst, 2) + " cents (" + where + ")");
        }

        beginTest("tuning with every latent cell thrown about (a strike): the note stays within 3 cents");
        {
            juce::Random rng(11);
            double worst = 0.0;
            for (int trial = 0; trial < 60; ++trial)
            {
                const double sr = trial % 3 == 0 ? 44100.0 : 48000.0, hz = noteHz(36 + rng.nextInt(49));
                Solo s(sr);
                res::BodyLatent offset;
                for (auto& o : offset) o = rng.nextFloat() * 2.0f - 1.0f;
                offset[res::Hum] = std::min(offset[res::Hum], 0.0f);      // (the hum sounds an octave under the note: not looked for here)
                s.r.setBody(rng.nextFloat() * 5.0f, offset);
                s.r.setExciter((float) res::Strike, exciteOffset(res::Hard, 0.3f));
                s.r.setPitch(hz);
                s.r.setDecay(4.0f);
                const auto x = s.play(0.65, 1.0f, 0.0f);
                const double off = centsOff(x, (size_t) (0.02 * sr), (size_t) (0.6 * sr), hz, sr, 40.0);
                worst = std::max(worst, std::abs(off));
                expect(std::abs(off) < 3.0, "trial " + juce::String(trial) + ": " + juce::String(off, 2) + " cents");
            }
            logMessage("worst of 60 random bodies: " + juce::String(worst, 2) + " cents");
        }

        beginTest("tuning under a bow and a breath (the junction pulls the pitch: stated as measured): within 6 cents from note 36 to "
                  "96 at the six points; note 24 is logged, not promised");
        {
            juce::String table = "cents off under BOW / BRTH, notes 24 36 48 60 72 96:\n";
            for (int x : { (int) res::Bow, (int) res::Breath })
                for (int b = 0; b < 6; ++b)
                {
                    table << juce::String(exciterName(x)).paddedRight(' ', 5) << juce::String(modelName(b)).paddedRight(' ', 5);
                    for (int note : { 24, 36, 48, 60, 72, 96 })
                    {
                        const double hz = noteHz(note), sr = 48000.0;
                        Solo s(sr);
                        s.sound((float) b, (float) x, hz, 2.0f);
                        const auto out = s.play(2.2, 0.7f, 0.7f);
                        const double off = centsOff(out, 48000, 57600, hz, sr, 100.0);
                        table << " " << juce::String(off, 1);
                        expect(rms(out, 86400) > 1.0e-3, juce::String(exciterName(x)) + " " + modelName(b) + " note " + juce::String(note) + " sounds");
                        if (note >= 36)
                            expect(std::abs(off) < 6.0, juce::String(exciterName(x)) + " " + modelName(b) + " note " + juce::String(note) + ": "
                                   + juce::String(off, 2) + " cents");
                    }
                    table << "\n";
                }
            logMessage(table);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Partial n at a body position (the class's own answer).
    static float partialOf(float body, int n)
    {
        R r;
        r.prepare(48000.0);
        r.setBody(std::min(body, 5.0f));
        r.setPitch(220.0);
        r.strike(0.0f);
        r.tick(0.0f, 0.0f);
        return r.partial(n);
    }

    void morph()
    {
        const double hz = 220.0, sr = 48000.0;

        beginTest("a morph moves the partials: from point to point in steps of an eighth each partial is where the latents put "
                  "it and nowhere outside the two points' own (within 2 %), no step takes one further than 0.45 of the whole way, "
                  "and the level of a strike never jumps by more than 3 dB from one step to the next");
        {
            juce::String levels = "strike, dB at MRPH 0 .. 5 in eighths:";
            double lastLevel = 0.0, mostStep = 0.0;
            std::array<double, 4> lastRatio {};
            for (int step = 0; step <= 40; ++step)
            {
                const float body = (float) step / 8.0f;
                Solo s;
                s.r.setBody(body);
                s.r.setExciter((float) res::Strike, exciteOffset(res::Hard, 1.0f));
                s.r.setPitch(hz);
                s.r.setDecay(4.0f);
                const auto x = s.play(0.6, 1.0f, 0.0f);
                const double level = rms(x, 0, 14400);
                levels << " " << juce::String(dB(level), 1);
                if (step > 0)
                {
                    mostStep = std::max(mostStep, (double) std::abs(dB(level / lastLevel)));
                    expect(std::abs(dB(level / lastLevel)) < 3.0f, "MRPH " + juce::String(body, 3) + ": " + juce::String(dB(level / lastLevel), 2) + " dB from the step before");
                }
                lastLevel = level;
                for (int n = 2; n <= 5; ++n)
                {
                    const double said = s.r.partial(n);
                    const float from = std::floor((float) std::max(0, step - 1) / 8.0f);
                    const double a = partialOf(from, n), b = partialOf(from + 1.0f, n), way = std::abs(b - a);
                    expect(said >= std::min(a, b) * 0.98 && said <= std::max(a, b) * 1.02, "MRPH " + juce::String(body, 3) + " partial " + juce::String(n)
                           + " at " + juce::String(said, 3) + " is between the points' " + juce::String(a, 3) + " and " + juce::String(b, 3));
                    if (step > 0)
                        expect(std::abs(said - lastRatio[(size_t) (n - 2)]) <= way * 0.45 + 1.0e-3,
                               "MRPH " + juce::String(body, 3) + " partial " + juce::String(n) + " moved from " + juce::String(lastRatio[(size_t) (n - 2)], 3)
                               + " to " + juce::String(said, 3) + " (the whole way is " + juce::String(way, 3) + ")");
                    lastRatio[(size_t) (n - 2)] = said;
                    const auto found = partialNear(x, hz, said, sr, 20.0);
                    if (found.level > 0.05 * levelAt(x, 960, 24000, hz, sr))     // (a partial the body barely sounds is not looked for)
                        expect(std::abs(1200.0 * std::log2(found.ratio / said)) < 6.0, "MRPH " + juce::String(body, 3) + " partial " + juce::String(n)
                               + ": said " + juce::String(said, 4) + ", heard " + juce::String(found.ratio, 4));
                }
            }
            logMessage(levels + "\nthe largest step: " + juce::String(mostStep, 2) + " dB");
        }

        beginTest("a morph within a ringing note keeps what rings: struck once, moved from COMB to PLAT and back while it rings, "
                  "the body never holds more than before, loses nothing but its own decay, and is not struck again");
        {
            Solo moved, still;
            res::BodyLatent hold {};        // (DAMP and TILT held at the comb's: every partial rings DCAY all the way)
            hold[res::Damp] = -1.0f;
            hold[res::Tilt] = -1.0f;
            for (Solo* s : { &moved, &still })
            {
                s->r.setBody((float) res::Comb, hold);
                s->r.setExciter((float) res::Strike);
                s->r.setPitch(hz);
                s->r.setDecay(4.0f);
                s->play(0.05, 1.0f, 0.0f);
            }
            float last = moved.r.held(), mostRise = 0.0f;
            const float atStart = last;
            std::vector<float> out;
            for (int i = 0; i < 48000; ++i)
            {
                const float way = 0.5f - 0.5f * std::cos(6.2831855f * (float) i / 48000.0f);
                moved.r.setBody(2.0f + way, hold);
                out.push_back(moved.r.tick(0.0f, 0.0f));
                still.r.tick(0.0f, 0.0f);
                if (i % 16 == 15)
                {
                    const float held = moved.r.held();
                    mostRise = std::max(mostRise, held - last);
                    last = held;
                }
            }
            expect(mostRise <= 0.0f, "what the body holds never rises during the morph (" + juce::String(mostRise, 9) + ")");
            const float kept = moved.r.held() / atStart, plain = still.r.held() / atStart;
            expect(kept > 0.7f * plain, "after a second of morphing it holds " + juce::String(dB(std::sqrt(kept)), 1) + " dB of what it had; left alone "
                   + juce::String(dB(std::sqrt(plain)), 1) + " dB");
            double mostJump = 0.0;
            for (size_t i = 1; i < out.size(); ++i)
                mostJump = std::max(mostJump, (double) std::abs(out[i] - out[i - 1]));
            expect(mostJump < 2.0 * peak(out), "no step larger than the sound itself");
            // Back at COMB: the harmonics are where they were.
            std::vector<float> after(24000);
            for (auto& v : after) v = moved.r.tick(0.0f, 0.0f);
            for (int n : { 1, 2, 3, 5 })
                expect(std::abs(centsOff(after, 0, 24000, hz * n, sr, 30.0)) < 1.0, "harmonic " + juce::String(n) + " back in place");
        }

        beginTest("no click in a morph under a bow: the body moved from point to point at once, in 5 ms and in 20 ms, makes no "
                  "step from one sample to the next larger than 1.5 times the largest that the bodies on its way make by themselves");
        {
            // What each bowed body's own largest step is (a comb's is by far the largest: every harmonic).
            auto own = [&](float body)
            {
                Solo s;
                s.sound(body, (float) res::Bow, 110.0, 2.0f);
                const auto x = s.play(1.5, 0.5f, 0.5f);
                double most = 0.0;
                for (size_t i = 48000; i < x.size(); ++i)
                    most = std::max(most, (double) std::abs(x[i] - x[i - 1]));
                return most;
            };
            std::array<double, 11> steady {};
            for (size_t i = 0; i < steady.size(); ++i) steady[i] = own(0.5f * (float) i);
            for (float from : { 0.0f, 1.0f, 2.0f, 4.0f })
                for (float to : { 0.0f, 2.0f, 3.0f, 5.0f })
                    for (int ms : { 0, 5, 20 })
                    {
                        if (from == to) continue;
                        Solo s;
                        s.sound(from, (float) res::Bow, 110.0, 2.0f);
                        auto x = s.play(1.0, 0.5f, 0.5f);
                        for (int i = 0; i < 9600; ++i)
                        {
                            const float t = ms == 0 ? 1.0f : std::min(1.0f, (float) i / ((float) ms * 48.0f));
                            s.r.setBody(from + (to - from) * t);
                            x.push_back(s.r.tick(0.0f, 0.5f));
                        }
                        double most = 0.0, allowed = 0.0;
                        for (size_t i = 48000; i < x.size(); ++i)
                            most = std::max(most, (double) std::abs(x[i] - x[i - 1]));
                        for (size_t i = 0; i < steady.size(); ++i)
                            if (0.5f * (float) i >= std::min(from, to) && 0.5f * (float) i <= std::max(from, to))
                                allowed = std::max(allowed, steady[i]);
                        expect(most <= 1.5 * allowed, "MRPH " + juce::String(from, 0) + " -> " + juce::String(to, 0) + " in " + juce::String(ms) + " ms: a step of "
                               + juce::String(most, 4) + "; the bodies on the way make " + juce::String(allowed, 4));
                    }
        }

        beginTest("both positions jumped about at random, every sample, every 7th and every 300th sample, under a bow and a strike: "
                  "finite, within the limit, and the body holds no more than its ceiling");
        {
            for (int every : { 1, 7, 300 })
            {
                juce::Random rng(every);
                Solo s;
                s.sound(0.0f, 1.0f, 110.0, 8.0f);
                s.r.strike(1.0f);
                bool ok = true;
                float most = 0.0f, mostHeld = 0.0f;
                for (int i = 0; i < 96000; ++i)
                {
                    if (i % every == 0)
                    {
                        s.r.setBody(rng.nextFloat() * 5.0f);
                        s.r.setExciter(rng.nextFloat() * 3.0f);
                    }
                    if (i % 9000 == 0) s.r.strike(2.0f);
                    const float y = s.r.tick(0.0f, 1.0f + rng.nextFloat());
                    ok = ok && std::isfinite(y);
                    most = std::max(most, std::abs(y));
                    if (i % 64 == 0) mostHeld = std::max(mostHeld, s.r.held());
                }
                expect(ok && most <= R::OUT_LIMIT, "every " + juce::String(every) + ": peak " + juce::String(most, 2));
                expect(mostHeld <= R::HELD * 1.5f, "every " + juce::String(every) + ": holds " + juce::String(mostHeld, 1));
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    void exciters()
    {
        const double hz = 220.0, sr = 48000.0;

        beginTest("the four exciters, told apart by what they do to a string: STRK is one burst (gone when the string has rung "
                  "out, however long the energy lasts), BOW BRTH and SCRP are fed for as long as there is energy and stop with it");
        {
            juce::String table = "dB, energy held 2 s then none (0-0.1 s / 1.5-2 s / 2.6-3 s):";
            for (int x = 0; x < 4; ++x)
            {
                Solo s;
                s.sound((float) res::String, (float) x, hz, 0.5f);
                auto out = s.play(2.0, 0.5f, 0.5f);
                const auto tail = s.run(1.0, 0.0f);
                out.insert(out.end(), tail.begin(), tail.end());
                const double first = rms(out, 0, 4800), held = rms(out, 72000, 24000), after = rms(out, 124800, 19200);
                table << "  " << exciterName(x) << " " << juce::String(dB(first), 1) << " / " << juce::String(dB(held), 1) << " / " << juce::String(dB(after), 1);
                if (x == res::Strike)
                    expect(first > 1.0e-3 && held < 1.0e-3 * first, "STRK: one burst, " + juce::String(dB(held / first), 1) + " dB after 1.5 s of energy");
                else
                {
                    expect(held > 0.01, juce::String(exciterName(x)) + " is fed: " + juce::String(dB(held), 1) + " dB while the energy lasts");
                    expect(after < 0.03 * held, juce::String(exciterName(x)) + " stops with the energy: " + juce::String(dB(after / held), 1) + " dB");
                }
            }
            logMessage(table);
        }

        beginTest("grain, on a body that barely rings: a strike is one impulse, a scrape sparse irregular impulses (a high crest, "
                  "uneven from one stretch to the next), a breath dense noise (a low crest, even); GRAN moves a scrape from the one "
                  "to the other");
        {
            auto crest = [&](float exciter, const res::ExciteLatent& offset, double& uneven)
            {
                Solo s;
                s.r.setBody((float) res::Comb);
                s.r.setExciter(exciter, offset);
                s.r.setPitch(hz);
                s.r.setDecay(0.01f);
                const auto x = s.play(2.0, 0.5f, 0.5f);
                // How uneven: the spread of the level over 38 stretches of 50 ms, against their mean.
                double mean = 0.0, spread = 0.0;
                std::array<double, 38> levels {};
                for (size_t i = 0; i < levels.size(); ++i) { levels[i] = rms(x, 4800 + i * 2400, 2400); mean += levels[i] / (double) levels.size(); }
                for (double l : levels) spread += (l - mean) * (l - mean) / (double) levels.size();
                uneven = std::sqrt(spread) / std::max(mean, 1.0e-12);
                return peak(x, 4800) / std::max(rms(x, 4800), 1.0e-12);
            };
            res::ExciteLatent alone {};
            alone[res::Couple] = -1.0f;         // the grains by themselves: no junction
            double scrapeUneven = 0.0, breathUneven = 0.0, denseUneven = 0.0, sparseUneven = 0.0;
            const double scrape = crest((float) res::Scrape, alone, scrapeUneven), breath = crest((float) res::Breath, alone, breathUneven);
            res::ExciteLatent dense = alone, sparse = alone;
            dense[res::Grain] = 1.0f;
            sparse[res::Grain] = -0.15f;
            const double scrapeDense = crest((float) res::Scrape, dense, denseUneven), scrapeSparse = crest((float) res::Scrape, sparse, sparseUneven);
            logMessage("crest / unevenness: SCRP " + juce::String(scrape, 1) + " / " + juce::String(scrapeUneven, 2) + ", BRTH " + juce::String(breath, 1)
                       + " / " + juce::String(breathUneven, 2) + ", SCRP with GRAN up " + juce::String(scrapeDense, 1) + " / " + juce::String(denseUneven, 2)
                       + ", with GRAN down " + juce::String(scrapeSparse, 1) + " / " + juce::String(sparseUneven, 2));
            expect(scrape > 2.0 * breath, "a scrape has the higher crest");
            expect(scrapeUneven > 3.0 * breathUneven, "and is uneven");
            expect(scrapeDense < 1.3 * breath && denseUneven < 2.0 * breathUneven, "GRAN up: a scrape is noise");
            expect(scrapeSparse > scrape, "GRAN down: sparser still");

            // One strike on that body: all of it within the first 5 ms.
            Solo s;
            s.r.setBody((float) res::Comb);
            s.r.setExciter((float) res::Strike);
            s.r.setPitch(hz);
            s.r.setDecay(0.01f);
            const auto x = s.play(0.5, 1.0f, 1.0f);
            expect(rms(x, 0, 240) > 1.0e-3 && rms(x, 2400) < 1.0e-4 * rms(x, 0, 240), "a strike is over at once: " + juce::String(dB(rms(x, 2400) / rms(x, 0, 240)), 0) + " dB after 50 ms");
        }

        beginTest("coupling: with no grain at all only BOW and BRTH sound (they sustain the body by themselves, at its note, for "
                  "as long as there is energy); STRK rings out and SCRP is silent");
        {
            for (int b : { (int) res::String, (int) res::Tube, (int) res::Bar, (int) res::Bell })
                for (int x = 0; x < 4; ++x)
                {
                    Solo s;
                    s.r.setBody((float) b);
                    s.r.setExciter((float) x, exciteOffset(res::Grain, -1.0f));
                    s.r.setPitch(hz);
                    s.r.setDecay(0.5f);
                    const auto out = s.play(3.0, 0.7f, 0.7f);
                    const double late = rms(out, 96000, 24000), later = rms(out, 120000, 24000);
                    const juce::String name = juce::String(exciterName(x)) + " on " + modelName(b);
                    if (x == res::Bow || x == res::Breath)
                    {
                        expect(late > 0.01 && later > 0.8 * late && later < 1.25 * late, name + " holds its level: " + juce::String(dB(late), 1) + " then "
                               + juce::String(dB(later), 1) + " dB");
                        const double at = levelAt(out, 96000, 48000, hz * std::exp2(centsOff(out, 96000, 48000, hz, sr) / 1200.0), sr);
                        expect(at > 0.3 * late, name + ": it is the note that sounds (" + juce::String(dB(at / late), 1) + " dB of the level)");
                    }
                    else
                        expect(late < 1.0e-5, name + " does not sustain: " + juce::String(dB(late), 1) + " dB after 2 s");
                }
        }

        beginTest("the junction starts and stops: no energy, no sound; it speaks without a spike when the energy comes, follows it, "
                  "and when it goes the body rings out by its own decay (a breath has a threshold: under it only air)");
        {
            for (int x : { (int) res::Bow, (int) res::Breath })
                for (int b : { (int) res::String, (int) res::Tube, (int) res::Bell })
                {
                    const juce::String name = juce::String(exciterName(x)) + " on " + modelName(b);
                    Solo s;
                    s.sound((float) b, (float) x, hz, 1.0f);
                    s.r.strike(0.0f);
                    expect(rms(s.run(0.3, 0.0f)) < 1.0e-9, name + ": silent without energy");
                    const auto rise = s.run(3.0, 0.6f);
                    const double steady = rms(rise, 120000);
                    expect(steady > 0.01, name + " speaks: " + juce::String(dB(steady), 1) + " dB");
                    expect(peak(rise) < 2.5 * peak(rise, 120000), name + ": no spike on the way (" + juce::String(peak(rise) / peak(rise, 120000), 2) + " x the steady peak)");
                    const auto more = s.run(2.0, 1.2f);
                    expect(rms(more, 72000) > 1.1 * steady, name + ": more energy, louder (" + juce::String(dB(rms(more, 72000) / steady), 1) + " dB)");
                    // Let go: what the body holds only falls, and the sound falls by DCAY (60 dB a second here).
                    float last = s.r.held();
                    bool rose = false;
                    std::vector<float> tail;
                    for (int i = 0; i < 48000; ++i)
                    {
                        tail.push_back(s.r.tick(0.0f, 0.0f));
                        if (i > 4800 && i % 16 == 0) { rose = rose || s.r.held() > last * 1.0001f; last = s.r.held(); }
                    }
                    expect(! rose, name + ": let go, it only dies away");
                    const float fall = dB(rms(tail, 33600, 4800) / rms(tail, 9600, 4800));
                    expect(fall < -20.0f && fall > -45.0f, name + ": by its own decay, " + juce::String(fall, 1) + " dB in half a second");
                }
            // The reed's threshold on the tube: the note is not there at a small energy and is there at a larger one.
            double tone[2] = {};
            int i = 0;
            for (float energy : { 0.12f, 0.6f })
            {
                Solo s;
                s.sound((float) res::Tube, (float) res::Breath, hz, 1.0f);
                const auto out = s.play(2.0, energy, energy);
                tone[i++] = levelAt(out, 48000, 48000, hz, sr) / std::max(rms(out, 48000), 1.0e-12);
            }
            expect(tone[1] > 0.5 && tone[0] < 0.5 * tone[1], "the tube under a breath: the note is " + juce::String(dB(tone[0]), 1) + " dB of the sound at energy 0.12, "
                   + juce::String(dB(tone[1]), 1) + " dB at 0.6");
        }

        beginTest("REED runs from the bow's curve to the reed's and COUP from no hold to a firm one: both are heard under a junction; "
                  "HARD is heard under every exciter; CONT makes a strike last");
        {
            auto render = [&](float exciter, const res::ExciteLatent& offset, float body = (float) res::String)
            {
                Solo s;
                s.r.setBody(body);
                s.r.setExciter(exciter, offset);
                s.r.setPitch(hz);
                s.r.setDecay(1.0f);
                return s.play(1.5, 0.6f, 0.6f);
            };
            auto differ = [&](const std::vector<float>& a, const std::vector<float>& b)
            {
                return (double) fastmeasure::difference(std::vector<float>(a.begin() + 24000, a.end()), std::vector<float>(b.begin() + 24000, b.end()), 2048);
            };
            const auto bow = render((float) res::Bow, {});
            expect(differ(bow, render((float) res::Bow, exciteOffset(res::Shape, 0.5f))) > 0.02, "REED on a bow");
            expect(differ(render((float) res::Breath, {}), render((float) res::Breath, exciteOffset(res::Shape, -0.5f))) > 0.02, "REED on a breath");
            expect(differ(bow, render((float) res::Bow, exciteOffset(res::Couple, -0.3f))) > 0.02, "COUP down on a bow");
            expect(rms(render((float) res::Bow, exciteOffset(res::Couple, -1.0f)), 24000) < 1.0e-6, "COUP off: a bow with nothing to hold is silent");
            juce::String hardness = "the twelfth harmonic against the first, dB, HARD up 0.4 / down 0.4:";
            for (int x = 0; x < 4; ++x)
            {
                const auto bright = render((float) x, exciteOffset(res::Hard, 0.4f), (float) res::Comb), dull = render((float) x, exciteOffset(res::Hard, -0.4f), (float) res::Comb);
                const double top = levelAt(bright, 0, 24000, hz * 12.0, sr) / std::max(levelAt(bright, 0, 24000, hz, sr), 1.0e-12);
                const double low = levelAt(dull, 0, 24000, hz * 12.0, sr) / std::max(levelAt(dull, 0, 24000, hz, sr), 1.0e-12);
                hardness << "  " << exciterName(x) << " " << juce::String(dB(top), 1) << " / " << juce::String(dB(low), 1);
                expect(top > 2.0 * low, juce::String(exciterName(x)) + ": HARD is heard (the twelfth harmonic " + juce::String(dB(top), 1) + " dB hard, "
                       + juce::String(dB(low), 1) + " dB soft)");
            }
            logMessage(hardness);
            auto noisy = exciteOffset(res::Grain, 1.0f);
            const auto strike = render((float) res::Strike, noisy);
            noisy[res::Cont] = 1.0f;
            const auto lasting = render((float) res::Strike, noisy);
            expect(rms(lasting, 48000) > 30.0 * rms(strike, 48000), "CONT up: a strike of noise goes on for as long as the energy");
        }

        beginTest("fusion 4: every curve of dsp/Saturation.h as the junction's curve, at both ends of REED and between: bounded, "
                  "finite, silent without energy; the curves are heard (not one sound under 19 names)");
        {
            expectEquals(E::NUM_CURVES, (int) sat::Count + 1);
            juce::String table = "dB under a bow / between / a reed, on the string:";
            std::vector<std::vector<float>> bowed;
            for (int curve = 0; curve <= (int) sat::Count; ++curve)
            {
                table << "\n  " << juce::String(curve == 0 ? "PHYS" : sat::names()[curve - 1]).paddedRight(' ', 5);
                for (float shape : { -1.0f, 0.5f, 1.0f })
                    for (int b : { (int) res::String, (int) res::Bell })
                    {
                        if (b == res::Bell && shape != (curve % 2 == 0 ? -1.0f : 1.0f)) continue;      // (the heavy body at one end of REED a curve)
                        Solo s;
                        s.r.setBody((float) b);
                        s.r.setExciter((float) res::Bow, exciteOffset(res::Shape, shape));
                        s.r.setCurve(curve);
                        s.r.setPitch(hz);
                        s.r.setDecay(8.0f);
                        expect(rms(s.run(0.1, 0.0f)) < 1.0e-9, "silent without energy");
                        auto out = s.play(1.25, 2.0f, 2.0f);
                        float mostHeld = 0.0f;
                        for (int i = 0; i < 24000; ++i)
                        {
                            out.push_back(s.r.tick(0.0f, i < 12000 ? 2.0f : 0.3f));
                            if (i % 32 == 0) mostHeld = std::max(mostHeld, s.r.held());
                        }
                        const juce::String name = juce::String(curve == 0 ? "PHYS" : sat::names()[curve - 1]) + " REED " + juce::String(shape, 1) + " " + modelName(b);
                        expect(finite(out) && peak(out) <= R::OUT_LIMIT, name + ": peak " + juce::String(peak(out), 2));
                        expect(mostHeld <= R::HELD * 1.5f, name + ": holds " + juce::String(mostHeld, 1));
                        if (b == res::String)
                        {
                            table << " " << juce::String(dB(rms(out, 48000, 24000)), 1);
                            if (shape == -1.0f) bowed.push_back(std::vector<float>(out.begin() + 24000, out.begin() + 72000));
                        }
                    }
            }
            logMessage(table);
            int distinct = 0;
            for (size_t c = 1; c < bowed.size(); ++c)
                if (fastmeasure::difference(bowed[0], bowed[c], 2048) > 0.02f)
                    ++distinct;
            expect(distinct >= 12, juce::String(distinct) + " of the 18 curves sound different from PHYS under a bow");
        }
    }

    // ---------------------------------------------------------------------------------------
    void extremes()
    {
        beginTest("the class at its edges: what comes from outside as not-a-number, infinity or 1e30; energies and pitches that "
                  "are no numbers; 22.05 to 192 kHz; every latent at both ends: finite, within the limit, and silent again after");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            for (double sr : { 22050.0, 44100.0, 96000.0, 192000.0 })
                for (int x = 0; x < 4; ++x)
                {
                    Solo s(sr);
                    s.sound((float) (x + 1), (float) x, 110.0, 20.0f);
                    s.r.strike(inf);
                    bool ok = true;
                    float most = 0.0f;
                    const float outside[] = { 0.0f, nan, inf, -inf, 1.0e30f, -1.0e30f, 1.0f, -1.0f, 1.0e-30f };
                    const float energies[] = { 0.5f, nan, inf, -inf, 1.0e30f, -1.0f, 2.0f };
                    const double pitches[] = { 110.0, 0.0, -5.0, 1.0e9, (double) nan, (double) inf, 20000.0, 8.0 };
                    for (int i = 0; i < 40000; ++i)
                    {
                        if (i % 500 == 0) s.r.setPitch(pitches[(i / 500) % 8]);
                        if (i % 2700 == 0) s.r.setDecay(i % 5400 == 0 ? nan : (i % 8100 == 0 ? 1.0e30f : -1.0f));
                        const float y = s.r.tick(outside[i % 9] * (i % 2000 < 1000 ? 1.0f : 0.0f), energies[(i / 300) % 7]);
                        ok = ok && std::isfinite(y);
                        most = std::max(most, std::abs(y));
                    }
                    expect(ok && most <= R::OUT_LIMIT, juce::String(sr, 0) + " Hz " + exciterName(x) + ": peak " + juce::String(most, 2));
                    expect(std::isfinite(s.r.held()) && s.r.held() <= R::HELD * 1.5f, "what it holds: " + juce::String(s.r.held(), 2));
                    // Left alone at a short decay it is silent again.
                    s.r.setPitch(220.0);
                    s.r.setDecay(0.05f);
                    s.run(1.0, 0.0f);
                    expect(rms(s.run(0.1, 0.0f)) < 1.0e-6, juce::String(sr, 0) + " Hz " + exciterName(x) + ": silent again");
                }

            // Every latent at both ends, the others at random; a loud noise outside as well.
            juce::Random rng(3);
            for (int trial = 0; trial < 13 * 2 * 4; ++trial)
            {
                const int which = trial % 13;
                const float end = (trial / 13) % 2 == 0 ? 0.0f : 1.0f;
                res::BodyLatent body;
                res::ExciteLatent excite;
                for (auto& v : body) v = rng.nextFloat();
                for (auto& v : excite) v = rng.nextFloat();
                if (which < 8) body[(size_t) which] = end; else excite[(size_t) (which - 8)] = end;
                Solo s(trial % 3 == 0 ? 192000.0 : 48000.0);
                s.r.setBody(body);
                s.r.setExciter(excite);
                s.r.setCurve(trial % 19);
                s.r.setPitch(noteHz(trial % 2 == 0 ? 0 : 127));
                s.r.setDecay(trial % 4 == 0 ? 60.0f : 0.5f);
                s.r.strike(2.0f);
                bool ok = true;
                float most = 0.0f;
                for (int i = 0; i < 24000; ++i)
                {
                    const float y = s.r.tick((rng.nextFloat() * 2.0f - 1.0f) * 4.0f, 2.0f);
                    ok = ok && std::isfinite(y);
                    most = std::max(most, std::abs(y));
                }
                expect(ok && most <= R::OUT_LIMIT && s.r.held() <= R::HELD * 1.5f, "latent " + juce::String(which) + " at " + juce::String(end, 0) + ": peak "
                       + juce::String(most, 2) + ", holds " + juce::String(s.r.held(), 1));
            }
        }

        beginTest("a sine from outside right on a partial of a body that rings for 60 s: held under the ceiling");
        {
            Solo s;
            s.sound((float) res::Comb, (float) res::Strike, 220.0, 60.0f);
            std::vector<float> out;
            float mostHeld = 0.0f;
            for (int i = 0; i < 96000; ++i)
            {
                out.push_back(s.r.tick(std::sin(6.2831855f * 220.0f * (float) i / 48000.0f), 0.0f));
                if (i % 16 == 0) mostHeld = std::max(mostHeld, s.r.held());
            }
            expect(finite(out) && mostHeld <= R::HELD * 1.5f, "holds " + juce::String(mostHeld, 1) + " at most (ceiling " + juce::String(R::HELD, 0) + ")");
            expect(peak(out, 48000) <= R::OUT_LIMIT, "peak " + juce::String(peak(out, 48000), 2));
        }
    }

    // =======================================================================================
    // The engine. An engine with its own pressure router (no route until a test sets one) and,
    // when asked, something on the track's input.
    struct Rig
    {
        E engine;
        double sr;
        int block;
        AftertouchRouter router;
        enum Feed { None, Silence, Bursts, Noise };
        Feed feed = None;
        float feedLevel = 0.5f;
        uint32_t seed = 22222;
        long long clock = 0;
        bool finite = true;
        float peak = 0.0f;
        std::vector<float> in;

        explicit Rig(double sampleRate = 48000.0, int blockSize = 64, int voices = 8) : engine(voices), sr(sampleRate), block(blockSize)
        {
            router.clear();
            engine.setAftertouchRouter(&router);
            engine.prepareToPlay(sr, block);
        }

        void sound(float body, float exciter)
        {
            engine.setParameter(E::MorphParam, body);
            engine.setParameter(E::ExciterMorphParam, exciter);
        }
        void excite(float depth = 1.0f) { router.setRoute(0, { AftertouchRouter::Destination::Excite, depth, false }); }

        // The input at sample n: 10 ms of noise every 125 ms, or noise without end.
        float inputAt(long long n)
        {
            const float noise = (float) ((seed = seed * 1664525u + 1013904223u) >> 8) / 8388608.0f - 1.0f;
            if (feed == Noise) return noise * feedLevel;
            if (feed == Bursts) return (n % (long long) (sr * 0.125)) < (long long) (sr * 0.01) ? noise * feedLevel : 0.0f;
            return 0.0f;
        }

        // Push the local AftertouchRouter's routes to the engine's pressure slots (the old
        // per-block pointer read is now a one-shot translation in setAftertouchRouter).
        void pushRoutes() { engine.setAftertouchRouter(&router); }

        std::vector<float> render(double seconds, std::vector<float>* rightOut = nullptr)
        {
            pushRoutes();      // sync any route changes made since the last render
            std::vector<float> out;
            int left = (int) std::round(seconds * sr);
            out.reserve((size_t) left);
            while (left > 0)
            {
                const int n = std::min(left, block);
                juce::AudioBuffer<float> buffer(2, n);
                buffer.clear();
                juce::MidiBuffer midi;
                if (feed != None)
                {
                    in.resize((size_t) n);
                    float level = 0.0f;
                    for (int i = 0; i < n; ++i)
                    {
                        in[(size_t) i] = inputAt(clock + i);
                        level = std::max(level, std::abs(in[(size_t) i]));
                    }
                    engine.setAudioInput({ in.data(), in.data(), n, level });
                }
                else
                    engine.setAudioInput({});
                engine.processBlock(buffer, midi);
                for (int i = 0; i < n; ++i)
                {
                    const float l = buffer.getSample(0, i), r = buffer.getSample(1, i);
                    if (! std::isfinite(l) || ! std::isfinite(r)) finite = false;
                    peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
                    out.push_back(l);
                    if (rightOut) rightOut->push_back(r);
                }
                clock += n;
                left -= n;
            }
            return out;
        }
    };

    // The id of the cell `label` on page `page`; -1 if there is none.
    static int cellId(const E& e, const char* page, const char* label)
    {
        for (auto& p : e.getParamPages())
            if (p.name == page)
                for (auto& c : p.params)
                    if (juce::String(c.label) == label)
                        return c.id;
        return -1;
    }
    // The place of `label` in the LFOs' and the slots' destination list.
    static int destIndex(const char* label)
    {
        const auto& dests = E::lfoDests();
        for (int i = 0; i < dests.size(); ++i)
            if (juce::String(dests[i].label) == label)
                return i;
        return -1;
    }
    // A depth or an amount on MRPH or XMPH is a share of the cell's whole range. The tests of this
    // file were written when there were six bodies and four exciters (ranges of 5 and 3; fourteen
    // and five since phase U2b): what they give is brought to the same number of points.
    static float inPoints(const char* dest, float share)
    {
        if (juce::String(dest) == "MRPH") return share * 5.0f / (float) (E::NUM_BODIES - 1);
        if (juce::String(dest) == "XMPH") return share * 3.0f / (float) (E::NUM_EXCITERS - 1);
        return share;
    }
    static void setLfo(E& e, int lfo, const char* dest, float depth, float rate, int shape = 0)
    {
        depth = inPoints(dest, depth);
        const char* page = lfo == 0 ? "LFO1" : "LFO2";
        e.setParameter(cellId(e, page, "DEST"), (float) destIndex(dest));
        e.setParameter(cellId(e, page, "DPTH"), depth);
        e.setParameter(cellId(e, page, "RATE"), rate);
        e.setParameter(cellId(e, page, "SHPE"), (float) shape);
    }
    static void setSlot(E& e, int slot, int source, const char* dest, float amount, int via = ModSrc::Off)
    {
        // (A slot's DEST is the LFOs' list, then LRAT / LDPT and the other slots' amounts.)
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::SRC), (float) source);
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::DEST), (float) destIndex(dest));
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::AMT), inPoints(dest, amount));
        e.setParameter(ModSlotParam::id(slot, ModSlotParam::VIA), (float) via);
    }

    void identity()
    {
        beginTest("RSN is the ninth engine type, made by createEngine, eight voices, with pressure on the exciter by default");
        {
            expectEquals((int) EngineType::Resonator, ENGINE_TYPE_COUNT - 1);
            auto e = AudioEngine::createEngine(EngineType::Resonator);
            expect(e != nullptr && e->getType() == EngineType::Resonator);
            expectEquals(e->getName(), juce::String("Resonator"));
            expectEquals(E().getNumVoices(), 8);

            using D = AftertouchRouter::Destination;
            expect(AudioEngine::pressureDestinationValid(EngineType::Resonator, D::Excite));
            const auto router = AudioEngine::defaultPressureRouter(EngineType::Resonator);
            expect(router.effectiveRoute(0).dest == D::Excite && router.effectiveRoute(0).depth == 1.0f);
            expect(router.hasRoute(D::Excite) && ! router.hasRoute(D::Volume));
        }

        beginTest("pages: RSN, BODY, BOD2, EXC, GRAIN, AMP, FILTR, F ENV, the mod slots' SLT1 and LSHP, then STRIP, LFO1, LFO2; eight working "
                  "cells each, names of four characters at most and none twice among the destinations, every cell read back as set");
        {
            E e;
            const auto pages = e.getParamPages();
            juce::StringArray names;
            for (auto& p : pages) names.add(p.name);
            // [U4-RSN] The router's pages since the port: XIN before the slots (the old prefix kept).
            expect(names.joinIntoString(" ").startsWith("RSN BODY BOD2 EXC GRAIN AMP FILTR F ENV"));
            expectEquals(names.joinIntoString(" "), juce::String("RSN BODY BOD2 EXC GRAIN AMP FILTR F ENV XIN SLT1 LSHP STRIP LFO1 LFO2"));
            juce::StringArray cells;
            for (int p = 0; p < 4; ++p)
                for (auto& c : pages[(size_t) p].params) cells.add(c.label);
            expectEquals(cells.joinIntoString(" "), juce::String("MRPH XMPH DCAY ENRG TUNE FINE VSEN LEVL STIF SHT SHEL ODD DAMP TILT HUM POS "
                                                                 "MEMB DROP WIRE MEET SPRD PAIR HAND DIR CONT GRAN COUP REED HARD CURV IN LAG"));
            std::set<int> ids;
            for (auto& page : pages)
                for (auto& p : page.params)
                {
                    expect(p.isValid(), "an empty cell on " + page.name);
                    expect(ids.insert(p.id).second, juce::String(p.label) + ": its id twice");
                    expect(std::strlen(p.label) >= 1 && std::strlen(p.label) <= 4, juce::String("label ") + p.label);
                    expectWithinAbsoluteError(e.getParameter(p.id), p.defaultVal, 1.0e-6f, juce::String(p.label) + " default");
                    if (p.kind != ParamKind::Choice && page.name != "STRIP" && ! page.name.startsWith("LFO"))
                    {
                        e.setParameter(p.id, p.maxVal);
                        expectWithinAbsoluteError(e.getParameter(p.id), p.maxVal, 1.0e-6f, juce::String(p.label) + " max");
                        e.setParameter(p.id, p.minVal);
                        expectWithinAbsoluteError(e.getParameter(p.id), p.minVal, 1.0e-6f, juce::String(p.label) + " min");
                        e.setParameter(p.id, p.defaultVal);
                    }
                }
            const auto& dests = E::lfoDests();
            juce::StringArray destNames;
            for (int i = 0; i < dests.size(); ++i)
            {
                expect(! destNames.contains(dests[i].label) && std::strlen(dests[i].label) <= 4, juce::String("DEST ") + dests[i].label);
                destNames.add(dests[i].label);
            }
            // Both positions and all thirteen latents can be modulated.
            for (const char* label : { "MRPH", "XMPH", "STIF", "SHT", "SHEL", "ODD", "DAMP", "TILT", "HUM", "POS", "CONT", "GRAN", "COUP", "REED", "HARD",
                                       "DCAY", "ENRG", "IN", "LEVL", "MEMB", "DROP", "WIRE", "MEET", "SPRD", "PAIR", "HAND", "DIR" })
                expect(destNames.contains(label), juce::String(label) + " is a destination");
            for (const char* label : { "CURV", "TUNE", "FINE", "VSEN", "LAG", "BEND" })
                expect(! destNames.contains(label), juce::String(label) + " is not");
            // MRPH and XMPH: continuous, the points at the whole numbers, 0.05 a click.
            expect(pages[0].params[0].maxVal == 13.0f && pages[0].params[1].maxVal == 4.0f && pages[0].params[0].kind == ParamKind::Linear);
            expectWithinAbsoluteError(pages[0].params[0].turned(0.95f, 1, TurnStep::Plain), 1.0f, 1.0e-5f);
            expectEquals(pages[3].params[5].numChoices, 19, "CURV: PHYS and the 18 curves");
            expectEquals(juce::String(pages[3].params[5].choices[0]), juce::String("PHYS"));
            expectEquals(juce::String(pages[3].params[5].choices[18]), juce::String(sat::names()[17]));

            // Values no cell can have are refused or held to the cell.
            e.setParameter(E::DecayParam, std::numeric_limits<float>::quiet_NaN());
            expectWithinAbsoluteError(e.getParameter(E::DecayParam), 2.0f, 1.0e-6f);
            e.setParameter(E::MorphParam, 1.0e30f);
            expectWithinAbsoluteError(e.getParameter(E::MorphParam), (float) (E::NUM_BODIES - 1), 1.0e-6f);
            e.setParameter(E::CurveParam, 99.0f);
            expectEquals((int) e.getParameter(E::CurveParam), E::NUM_CURVES - 1);
        }

        beginTest("a new track is a struck string that sounds from the keys; every point x point sounds at a level a mix can use; "
                  "the same notes render the same, sample for sample");
        {
            juce::String table = "dB (peak) of a second, note 52 at velocity 0.8:";
            for (int x = 0; x < 4; ++x)
            {
                table << "\n  " << juce::String(exciterName(x)).paddedRight(' ', 5);
                for (int b = 0; b < 6; ++b)
                {
                    Rig rig;
                    rig.sound((float) b, (float) x);
                    rig.engine.noteOn(52, 0.8f, -1);
                    const auto out = rig.render(1.0);
                    const double level = x == 0 ? rms(out, 0, 9600) : rms(out, 24000);
                    table << " " << modelName(b) << " " << juce::String(dB(level), 1) << " (" << juce::String(dB(rig.peak), 1) << ")";
                    expect(rig.finite && level > 0.003 && level < 0.35 && rig.peak < 1.0f, juce::String(exciterName(x)) + " " + modelName(b) + ": "
                           + juce::String(dB(level), 1) + " dB, peak " + juce::String(dB(rig.peak), 1));
                    Rig again;
                    again.sound((float) b, (float) x);
                    again.engine.noteOn(52, 0.8f, -1);
                    expect(again.render(1.0) == out, juce::String(exciterName(x)) + " " + modelName(b) + ": the same again");
                }
            }
            logMessage(table);
        }

        beginTest("TUNE, FINE and the pitch strip move the note: an octave, a quarter-tone, BEND semitones");
        {
            auto offBy = [&](const std::function<void(E&)>& set, double expected)
            {
                Rig rig;
                set(rig.engine);
                rig.engine.noteOn(57, 1.0f, -1);
                rig.render(0.05);
                const auto x = rig.render(0.5);
                return centsOff(x, 0, x.size(), 220.0 * expected, 48000.0);
            };
            expect(std::abs(offBy([](E& e) { e.setParameter(E::TuneParam, 12.0f); }, 2.0)) < 3.0, "TUNE +12");
            expect(std::abs(offBy([](E& e) { e.setParameter(E::FineParam, 50.0f); }, std::exp2(0.5 / 12.0))) < 3.0, "FINE +50");
            expect(std::abs(offBy([](E& e) { e.setParameter(E::BendRangeParam, 7.0f); e.pitchBend(1.0f); }, std::exp2(7.0 / 12.0))) < 3.0, "the strip up, BEND 7");
        }
    }

    // ---------------------------------------------------------------------------------------
    // What a voice's body and exciter are at this moment.
    static float bodyNow(Rig& rig, int voice, int latent) { return rig.engine.getVoice(voice).getResonator().body()[(size_t) latent]; }
    static float exciteNow(Rig& rig, int voice, int latent) { return rig.engine.getVoice(voice).getResonator().exciter()[(size_t) latent]; }

    void modulation()
    {
        beginTest("an LFO on MRPH morphs the body within a held note: the latents swing with it (a sine: from COMB to PLAT and to "
                  "TUBE, smoothly); with a square the body is the plate for half a cycle and the tube for the other, cycle after cycle, "
                  "and is heard to be (the plate's 2.04 x the note against the tube's fifth harmonic); the sound goes on");
        {
            Rig rig(48000.0, 32);
            rig.sound((float) res::Comb, (float) res::Scrape);          // fed all the time: every partial is always there to be seen
            rig.engine.setParameter(E::ExciteBase + res::Grain, 1.0f);
            rig.engine.setParameter(E::AmpSustain, 1.0f);
            setLfo(rig.engine, 0, "MRPH", 0.2f, 2.0f);                  // 0.2 of the range of 5: COMB +- one point
            rig.engine.noteOn(57, 1.0f, -1);
            float low = 1.0f, high = 0.0f, mostStep = 0.0f, last = 0.0f, mostOdd = 0.0f;
            for (int i = 0; i < 1500 * 2; ++i)
            {
                rig.render(32.0 / 48000.0);
                const float sheet = bodyNow(rig, 0, res::Sheet);
                if (i > 100) { low = std::min(low, sheet); high = std::max(high, sheet); mostStep = std::max(mostStep, std::abs(sheet - last)); }
                mostOdd = std::max(mostOdd, bodyNow(rig, 0, res::Odd));
                last = sheet;
            }
            expect(low < 0.02f && high > 0.95f && mostOdd > 0.95f, "SHT swings from " + juce::String(low, 2) + " to " + juce::String(high, 2) + ", ODD up to " + juce::String(mostOdd, 2));
            expect(mostStep < 0.02f, "by " + juce::String(mostStep, 4) + " a block of 32 samples at most (no jump)");

            Rig square(48000.0, 32);
            square.sound((float) res::Comb, (float) res::Scrape);
            square.engine.setParameter(E::ExciteBase + res::Grain, 1.0f);
            square.engine.setParameter(E::AmpSustain, 1.0f);
            square.engine.setParameter(E::DecayParam, 0.3f);
            setLfo(square.engine, 0, "MRPH", 0.2f, 1.0f, (int) LFO::Shape::Square);
            square.engine.noteOn(57, 1.0f, -1);
            std::vector<float> out, sheetAt;
            for (int i = 0; i < 1500 * 4; ++i)
            {
                const auto x = square.render(32.0 / 48000.0);
                out.insert(out.end(), x.begin(), x.end());
                sheetAt.push_back(bodyNow(square, 0, res::Sheet));
            }
            // Each stretch of 0.3 s in which the body stood still: what it sounded there.
            int plates = 0, tubes = 0;
            for (int start = 300; start + 450 < (int) sheetAt.size(); start += 150)
            {
                bool plate = true, tube = true;
                // ([U4-RSN] 10 blocks less at each end, was start - 150 .. start + 450: the router gives a
                // cell from where its LFO will be at the end of the sub-block, so the body leaves a half
                // up to 16 samples sooner, and the old window fitted the half to the block.)
                for (int i = start - 140; i < start + 440; ++i) { plate = plate && sheetAt[(size_t) i] > 0.99f; tube = tube && sheetAt[(size_t) i] < 0.01f; }
                if (! plate && ! tube) continue;
                const size_t from = (size_t) start * 32;
                const double platePartial = levelAt(out, from, 14400, 220.0 * 2.0431, 48000.0), tubePartial = levelAt(out, from, 14400, 220.0 * 5.0, 48000.0);
                if (plate) { ++plates; expect(platePartial > 3.0 * tubePartial, "the plate's half: 2.04 x is " + juce::String(dB(platePartial / tubePartial), 1) + " dB over 5 x"); }
                if (tube)  { ++tubes;  expect(tubePartial > 3.0 * platePartial, "the tube's half: 5 x is " + juce::String(dB(tubePartial / platePartial), 1) + " dB over 2.04 x"); }
            }
            expect(plates >= 3 && tubes >= 3, juce::String(plates) + " plate halves and " + juce::String(tubes) + " tube halves looked at");
            expect(square.finite && rms(out, 96000) > 0.5 * rms(out, 0, 96000), "it sounds on");
        }

        beginTest("the envelope morphs within each note through a mod slot (ENV on MRPH: a string that becomes a bell as the "
                  "envelope rises and comes back as it falls), FENV and an LFO through a slot do too; the exciter's position as well");
        {
            Rig rig(48000.0, 32);
            rig.engine.setParameter(E::AmpAttack, 0.5f);
            rig.engine.setParameter(E::AmpSustain, 1.0f);
            rig.engine.setParameter(E::AmpRelease, 0.5f);
            setSlot(rig.engine, 0, ModSrc::Env, "MRPH", 1.0f);
            rig.engine.noteOn(57, 1.0f, -1);
            rig.render(0.01);
            const float atOnset = bodyNow(rig, 0, res::Shell);
            rig.render(0.25);
            const float midway = rig.engine.getVoice(0).getCell(V::Morph);
            rig.render(0.5);
            const float atTop = bodyNow(rig, 0, res::Shell);
            rig.engine.noteOff(57, -1);
            rig.render(0.45);
            const float released = bodyNow(rig, 0, res::Shell);
            expect(atOnset < 0.01f && atTop > 0.99f, "from the string (SHEL " + juce::String(atOnset, 2) + ") to the bell (" + juce::String(atTop, 2) + ") with the attack");
            expect(midway > 1.0f && midway < 4.0f, "half-way up it is on the way (MRPH " + juce::String(midway, 2) + ")");
            expect(released < 0.5f, "and back with the release (" + juce::String(released, 2) + ")");

            Rig filterEnv(48000.0, 32);
            filterEnv.engine.setParameter(E::AmpSustain, 1.0f);
            setSlot(filterEnv.engine, 0, ModSrc::FilterEnv, "XMPH", 1.0f / 3.0f);
            filterEnv.engine.noteOn(57, 1.0f, -1);
            float early = 0.0f;
            for (int i = 0; i < 150; ++i)
            {
                filterEnv.render(32.0 / 48000.0);
                early = std::max(early, exciteNow(filterEnv, 0, res::Cont));
            }
            filterEnv.render(3.0);
            const float late = exciteNow(filterEnv, 0, res::Cont);
            expect(early > late + 0.3f, "FENV on XMPH: towards the bow while the filter envelope is up (CONT " + juce::String(early, 2) + "), a strike again when it "
                   "has fallen (" + juce::String(late, 2) + ")");

            Rig viaLfo(48000.0, 32);
            viaLfo.engine.setParameter(E::AmpSustain, 1.0f);
            viaLfo.engine.setParameter(cellId(viaLfo.engine, "LFO1", "RATE"), 4.0f);
            setSlot(viaLfo.engine, 1, ModSrc::Lfo1, "XMPH", 1.0f);
            viaLfo.engine.noteOn(57, 1.0f, -1);
            float low = 1.0f, high = 0.0f;
            for (int i = 0; i < 1500; ++i)
            {
                viaLfo.render(32.0 / 48000.0);
                low = std::min(low, exciteNow(viaLfo, 0, res::Couple));
                high = std::max(high, exciteNow(viaLfo, 0, res::Couple));
            }
            expect(low < 0.05f && high > 0.7f, "LFO1 through a slot on XMPH: COUP from " + juce::String(low, 2) + " to " + juce::String(high, 2));
        }

        beginTest("from note to note: VEL, KEY and RAND through a slot put each note's body somewhere else (each voice its own), and "
                  "a soft and a hard note of the same key sound different bodies");
        {
            // VEL: a soft note stays near the string, a hard one goes to the bell.
            Rig rig;
            setSlot(rig.engine, 0, ModSrc::Velocity, "MRPH", 1.0f);
            rig.engine.noteOn(50, 0.05f, -1);
            rig.engine.noteOn(62, 1.0f, -1);
            rig.render(0.05);
            expect(bodyNow(rig, 0, res::Shell) < 0.01f && bodyNow(rig, 1, res::Shell) > 0.99f, "VEL: SHEL " + juce::String(bodyNow(rig, 0, res::Shell), 2)
                   + " soft, " + juce::String(bodyNow(rig, 1, res::Shell), 2) + " hard, at once in two voices");

            // KEY: low notes one body, high notes another.
            Rig keyed;
            keyed.engine.setParameter(E::MorphParam, 2.5f);
            setSlot(keyed.engine, 0, ModSrc::Key, "MRPH", 1.0f);
            keyed.engine.noteOn(36, 1.0f, -1);
            keyed.engine.noteOn(84, 1.0f, -1);
            keyed.render(0.05);
            const float lowKey = keyed.engine.getVoice(0).getCell(V::Morph), highKey = keyed.engine.getVoice(1).getCell(V::Morph);
            expect(highKey > lowKey + 1.0f, "KEY: MRPH " + juce::String(lowKey, 2) + " at note 36, " + juce::String(highKey, 2) + " at note 84");

            // RAND: eight notes, eight places.
            Rig random;
            random.engine.setParameter(E::MorphParam, 2.5f);
            setSlot(random.engine, 0, ModSrc::Random, "MRPH", 0.4f);
            std::set<int> places;
            float least = 5.0f, most = 0.0f;
            for (int n = 0; n < 8; ++n)
            {
                random.engine.noteOn(48 + n, 1.0f, -1);
                random.render(0.02);
                const float at = random.engine.getVoice(n).getCell(V::Morph);
                places.insert((int) std::lround(at * 100.0f));
                least = std::min(least, at);
                most = std::max(most, at);
            }
            expect(places.size() >= 7 && most - least > 1.0f, "RAND: " + juce::String((int) places.size()) + " different places for 8 notes, from "
                   + juce::String(least, 2) + " to " + juce::String(most, 2));

            // Heard: the hard note has the bell's tierce, the soft one has not.
            double tierce[2] = {};
            int i = 0;
            for (float velocity : { 0.05f, 1.0f })
            {
                Rig heard;
                heard.engine.setParameter(E::VelSenseParam, 0.0f);
                setSlot(heard.engine, 0, ModSrc::Velocity, "MRPH", 1.0f);
                heard.engine.noteOn(57, velocity, -1);
                const auto x = heard.render(0.6);
                tierce[i++] = levelAt(x, 960, 24000, 220.0 * 1.183, 48000.0) / levelAt(x, 960, 24000, 220.0, 48000.0);
            }
            expect(tierce[1] > 0.1 && tierce[0] < 0.2 * tierce[1], "the tierce: " + juce::String(dB(tierce[0]), 1) + " dB soft, " + juce::String(dB(tierce[1]), 1) + " dB hard");
        }

        beginTest("both LFOs at their fastest (40 Hz), full depth, on MRPH and XMPH under held notes, at 44.1 and 192 kHz and in "
                  "blocks of 1 to 4096: finite, bounded, and no step in the sound larger than the sound");
        {
            for (double sr : { 44100.0, 192000.0 })
                for (int block : { 1, 7, 64, 4096 })
                {
                    if (block == 1 && sr > 48000.0) continue;       // (slow in the Debug build, and nothing new)
                    Rig rig(sr, block);
                    rig.engine.setParameter(E::AmpSustain, 1.0f);
                    rig.engine.setParameter(E::DecayParam, 8.0f);
                    setLfo(rig.engine, 0, "MRPH", 1.0f, 40.0f);
                    setLfo(rig.engine, 1, "XMPH", 1.0f, 40.0f, 2);
                    for (int note : { 40, 52, 59, 64 })
                        rig.engine.noteOn(note, 1.0f, -1);
                    const auto out = rig.render(block == 1 ? 0.3 : 1.0);
                    double mostJump = 0.0;
                    for (size_t i = 1; i < out.size(); ++i)
                        mostJump = std::max(mostJump, (double) std::abs(out[i] - out[i - 1]));
                    expect(rig.finite && rig.peak < 4.0f, juce::String(sr, 0) + " Hz, block " + juce::String(block) + ": peak " + juce::String(rig.peak, 2));
                    expect(mostJump < 1.2 * rig.peak, "the largest step " + juce::String(mostJump, 3) + " against a peak of " + juce::String(rig.peak, 3));
                }
        }
    }

    // ---------------------------------------------------------------------------------------
    void energy()
    {
        beginTest("pressure is the energy at every exciter position that is fed: with an EXC route BOW, BRTH and SCRP get louder as "
                  "the pad is pressed harder, come back, and stop when it is let up; the places between them answer the pad too "
                  "(between a bow and a breath not always by getting louder: the reed half closes as it is blown harder)");
        {
            juce::String table = "Level in dB at pressure 0.3 / 1 / 0.3 / 0:\n";
            for (float x : { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f })
                for (int b : { (int) res::String, (int) res::Tube, (int) res::Bar, (int) res::Bell })
                {
                    Rig rig;
                    rig.sound((float) b, x);
                    rig.excite();
                    rig.engine.setParameter(E::DecayParam, 0.4f);
                    rig.engine.noteOn(52, 0.5f, -1);
                    double level[4];
                    int i = 0;
                    for (float pressure : { 0.3f, 1.0f, 0.3f, 0.0f })
                    {
                        // (A pad that is let up all the way sends 0: the voice has had pressure, so 0 is its energy.)
                        rig.engine.aftertouch(52, pressure);
                        rig.render(0.8);
                        level[i++] = rms(rig.render(0.25));
                    }
                    const juce::String name = juce::String(modelName(b)) + " XMPH " + juce::String(x, 1);
                    table << name.paddedRight(' ', 15) << juce::String(dB(level[0]), 1) << " / " << juce::String(dB(level[1]), 1) << " / "
                          << juce::String(dB(level[2]), 1) << " / " << juce::String(dB(level[3]), 1) << "\n";
                    expect(rig.finite && level[0] > 1.0e-5, name + " sounds when pressed lightly");
                    if (x == std::floor(x))
                    {
                        expect(level[1] > 1.4 * level[0], name + ": louder pressed hard, " + juce::String(dB(level[1] / level[0]), 1) + " dB");
                        expect(level[2] < 0.75 * level[1], name + ": comes back, " + juce::String(dB(level[2] / level[1]), 1) + " dB");
                    }
                    else
                        expect(std::abs(dB(level[1] / level[0])) > 1.0f && std::abs(dB(level[2] / level[0])) < 0.6f * std::abs(dB(level[1] / level[0])) + 1.5f, name + ": answers the pad ("
                               + juce::String(dB(level[1] / level[0]), 1) + " dB pressed hard) and comes back (" + juce::String(dB(level[2] / level[0]), 1) + " dB)");
                    expect(level[3] < 0.1 * level[0], name + ": let up, it stops (" + juce::String(dB(level[3] / level[0]), 1) + " dB)");
                }
            logMessage(table);
        }

        beginTest("the energy itself: pressure x the route's depth, smoothed over LAG, from the strike velocity; without an EXC "
                  "route, or without pressure, velocity x the amp envelope; ENRG doubles it at the top; VSEN takes velocity out");
        {
            Rig rig(48000.0, 16);
            rig.sound((float) res::String, (float) res::Bow);
            rig.excite(0.5f);
            rig.engine.setParameter(E::AmpAttack, 2.0f);
            rig.engine.noteOn(60, 0.8f, -1);
            rig.render(0.001);
            expectWithinAbsoluteError(rig.engine.getVoice(0).getEnergy(), 0.8f, 0.06f, "a pad's note starts from its strike velocity");
            rig.engine.aftertouch(60, 1.0f);
            rig.render(0.5);
            expectWithinAbsoluteError(rig.engine.getVoice(0).getEnergy(), 0.5f, 0.01f, "pressure 1 x depth 0.5");
            // LAG: 63 % of the way after LAG seconds.
            rig.engine.setParameter(E::LagParam, 0.1f);
            rig.engine.aftertouch(60, 0.0f);
            rig.render(0.1);
            expectWithinAbsoluteError(rig.engine.getVoice(0).getEnergy(), 0.5f * 0.368f, 0.03f, "LAG 0.1 s");
            rig.engine.setParameter(E::EnergyParam, 1.0f);
            rig.engine.aftertouch(60, 1.0f);
            rig.render(1.0);
            expectWithinAbsoluteError(rig.engine.getVoice(0).getEnergy(), 1.0f, 0.02f, "ENRG 100: twice");

            Rig keys(48000.0, 16);
            keys.sound((float) res::String, (float) res::Bow);
            keys.engine.setParameter(E::AmpAttack, 0.001f);
            keys.engine.setParameter(E::AmpSustain, 0.5f);
            keys.engine.setParameter(E::AmpDecay, 0.01f);
            keys.engine.noteOn(60, 0.6f, -1);
            keys.render(0.5);
            expectWithinAbsoluteError(keys.engine.getVoice(0).getEnergy(), 0.6f * 0.5f, 0.02f, "from the keys: velocity x envelope");
            keys.excite();
            keys.render(0.3);
            expectWithinAbsoluteError(keys.engine.getVoice(0).getEnergy(), 0.3f, 0.02f, "a route but no pressure yet: still the envelope");
            keys.engine.setParameter(E::VelSenseParam, 0.0f);
            keys.render(0.3);
            expectWithinAbsoluteError(keys.engine.getVoice(0).getEnergy(), 0.5f, 0.02f, "VSEN 0: the envelope alone");
        }

        beginTest("a strike that becomes a bow under pressure (PRES on XMPH through a slot): the note is struck, rings out when "
                  "the pad rests, and is bowed when it is pressed; the other pressure destinations act too (level, vibrato)");
        {
            Rig rig;
            rig.excite();
            rig.engine.setParameter(E::DecayParam, 0.5f);
            setSlot(rig.engine, 0, ModSrc::Pressure, "XMPH", 1.0f / 3.0f);
            rig.engine.noteOn(52, 0.8f, -1);
            const auto struck = rig.render(0.2);
            rig.render(1.3);
            const double rungOut = rms(rig.render(0.3));
            rig.engine.aftertouch(52, 0.9f);
            rig.render(1.2);
            const double bowed = rms(rig.render(0.3));
            expectWithinAbsoluteError(exciteNow(rig, 0, res::Couple), 0.9f * res::excitePoints()[res::Bow][res::Couple], 0.03f, "pressed: nine tenths of the way to the bow");
            expect(rms(struck) > 0.01 && rungOut < 0.01 * rms(struck), "struck, and rung out with the pad at rest");
            expect(bowed > 30.0 * rungOut && bowed > 0.01, "pressed: bowed, " + juce::String(dB(bowed), 1) + " dB");

            using D = AftertouchRouter::Destination;
            Rig level;
            level.sound((float) res::Comb, (float) res::Scrape);
            level.router.setRoute(0, { D::Volume, 1.0f, false });
            level.engine.setParameter(E::AmpSustain, 1.0f);
            level.engine.noteOn(52, 1.0f, -1);
            level.render(0.5);
            const double quiet = rms(level.render(0.5));
            level.engine.aftertouch(52, 1.0f);
            level.render(0.3);
            expect(rms(level.render(0.5)) > 1.5 * quiet, "pressure on LEVL");
            Rig vibrato;
            vibrato.sound((float) res::Tube, (float) res::Bow);
            vibrato.router.setRoute(0, { D::Vibrato, 1.0f, false });
            vibrato.engine.setParameter(E::AmpSustain, 1.0f);
            vibrato.engine.noteOn(57, 1.0f, -1);
            vibrato.render(1.0);
            const auto plain = vibrato.render(1.0);
            vibrato.engine.aftertouch(57, 1.0f);
            vibrato.render(1.0);
            const auto x = vibrato.render(1.0);
            const double steady = levelAt(plain, 0, 48000, 220.0, 48000.0) / rms(plain), wandering = levelAt(x, 0, 48000, 220.0, 48000.0) / rms(x);
            expect(wandering < 0.8 * steady, "pressure on VIB: the note wanders (its line is " + juce::String(dB(wandering), 1) + " dB of the level, "
                   + juce::String(dB(steady), 1) + " without)");
        }
    }

    // ---------------------------------------------------------------------------------------
    void input()
    {
        beginTest("IN at the top: the input alone drives the body - noise bursts come out pitched at the note at every point; "
                  "silence in, or nothing connected, gives silence; no note, no sound; after the release it is shut out again");
        {
            for (int b = 0; b < 6; ++b)
            {
                const juce::String name = modelName(b);
                Rig rig;
                rig.sound((float) b, 0.0f);
                rig.engine.setParameter(E::InputParam, 1.0f);
                rig.feed = Rig::Bursts;
                expect(rms(rig.render(0.3)) == 0.0, name + ": no note, no sound");
                rig.engine.noteOn(57, 1.0f, -1);
                const auto x = rig.render(1.0);
                const size_t from = 4800, n = 43200;
                const double at = fastmeasure::goertzel(x, from, n, 220.0, 48000.0);
                double beside = 0.0;
                for (double ratio : { 0.84, 0.92, 1.08 })
                    beside = std::max(beside, fastmeasure::goertzel(x, from, n, 220.0 * ratio, 48000.0));
                expect(rms(x) > 1.0e-3, name + " rings to the input: " + juce::String(dB(rms(x)), 1) + " dB");
                expect(at > 30.0 * beside, name + ": the note stands " + juce::String(dB(std::sqrt(at / std::max(beside, 1.0e-30))), 1) + " dB over what is beside it");
                expect(std::abs(centsOff(x, from, n, 220.0, 48000.0)) < 3.0, name + " in tune");
                rig.engine.setParameter(E::AmpRelease, 0.02f);
                rig.engine.setParameter(E::DecayParam, 0.05f);
                rig.engine.noteOff(57, -1);
                rig.render(0.5);
                expect(rms(rig.render(0.3)) < 1.0e-6, name + ": released, the input is shut out");

                Rig quiet;
                quiet.sound((float) b, 0.0f);
                quiet.engine.setParameter(E::InputParam, 1.0f);
                quiet.feed = Rig::Silence;
                quiet.engine.noteOn(57, 1.0f, -1);
                expect(rms(quiet.render(0.3)) < 1.0e-9, name + ": silence in, silence out");
                Rig open;
                open.sound((float) b, 0.0f);
                open.engine.setParameter(E::InputParam, 1.0f);
                open.engine.noteOn(57, 1.0f, -1);
                expect(rms(open.render(0.3)) < 1.0e-9, name + ": nothing connected, silence");
            }
        }

        beginTest("IN is a level under any exciter, not an exciter: at 0 the input is not heard, at the middle it is added to the "
                  "exciter at full, at the top the exciter is gone; under a bow, a breath and a scrape as under a strike");
        {
            for (float x : { 0.0f, 1.0f, 2.0f, 3.0f })
            {
                auto level = [&](float in, Rig::Feed feed, double from, double seconds)
                {
                    Rig rig;
                    rig.sound((float) res::Comb, x);
                    rig.engine.setParameter(E::InputParam, in);
                    rig.engine.setParameter(E::AmpSustain, 1.0f);
                    rig.engine.setParameter(E::DecayParam, 0.3f);
                    rig.feed = feed;
                    rig.engine.noteOn(57, 1.0f, -1);
                    rig.render(from);
                    return rms(rig.render(seconds));
                };
                const juce::String name = exciterName((int) x);
                const double from = x == 0.0f ? 0.0 : 1.5, seconds = x == 0.0f ? 0.1 : 0.5;
                const double alone = level(0.0f, Rig::None, from, seconds), deaf = level(0.0f, Rig::Noise, from, seconds);
                const double exciterAtHalf = level(0.5f, Rig::None, from, seconds), exciterAtTop = level(1.0f, Rig::None, from, seconds);
                const double inputOnly = level(1.0f, Rig::Noise, 1.5, 0.5), inputAtHalf = level(0.5f, Rig::Noise, 1.5, 0.5);
                expectWithinAbsoluteError(dB(deaf / std::max(alone, 1.0e-9)), 0.0f, 0.01f, name + ": IN 0, the input is not heard");
                expect(inputOnly > 1.0e-3 && exciterAtTop < 1.0e-6, name + ": IN at the top, the input alone (" + juce::String(dB(inputOnly), 1) + " dB)");
                expectWithinAbsoluteError(dB(exciterAtHalf / std::max(alone, 1.0e-9)), 0.0f, 0.01f, name + ": at the middle the exciter is still at full");
                expect(inputAtHalf > 0.5 * inputOnly, name + ": and the input is there in full as well (" + juce::String(dB(inputAtHalf), 1) + " dB with the exciter, "
                       + juce::String(dB(inputOnly), 1) + " alone)");
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // What a review found (each of these passed, or was not asked, before): the top of the range,
    // the smallest numbers, and behaviours no test would have missed.
    void review()
    {
        beginTest("a note that leaves the range fades and is not made up for: a breath or a scrape whose pitch slides to the top "
                  "(0.45 x the sample rate) and back stays at its level, and what the body holds stays a number");
        {
            for (double sr : { 22050.0, 48000.0 })
                for (int x : { (int) res::Breath, (int) res::Scrape })
                    for (int b : { (int) res::String, (int) res::Tube, (int) res::Comb })
                    {
                        Solo s(sr);
                        s.sound((float) b, (float) x, 1000.0, 2.0f);
                        s.r.strike(1.0f);
                        const int n = (int) sr * 3;
                        float most = 0.0f, mostHeld = 0.0f;
                        bool ok = true;
                        for (int i = 0; i < n; ++i)
                        {
                            const double t = (double) i / n;
                            s.r.setPitch(1000.0 * std::exp2(6.0 * (t < 0.5 ? 2.0 * t : 2.0 - 2.0 * t)));
                            const float y = s.r.tick(0.0f, 1.0f);
                            ok = ok && std::isfinite(y);
                            most = std::max(most, std::abs(y));
                            if (i % 16 == 0)
                            {
                                const float h = s.r.held();
                                ok = ok && std::isfinite(h);
                                mostHeld = std::max(mostHeld, h);
                            }
                        }
                        expect(ok && most <= 2.0f && mostHeld <= R::HELD, juce::String(sr, 0) + " Hz " + exciterName(x) + " " + modelName(b) + ": peak "
                               + juce::String(most, 2) + ", holds " + juce::String(mostHeld, 2) + " at most");
                    }
            const float nan = std::numeric_limits<float>::quiet_NaN();
            expect(res::outLimit(nan, R::OUT_LIMIT) == 0.0f, "what is no number is silence at the output, never a level");
            expect(res::outLimit(0.5f, R::OUT_LIMIT) == 0.5f && res::outLimit(-100.0f, R::OUT_LIMIT) > -R::OUT_LIMIT);

            // The engine: a breath on a comb, the pitch strip sweeping the note through the top and back.
            Rig rig(48000.0, 64);
            rig.sound((float) res::Comb, (float) res::Breath);
            rig.engine.setParameter(E::AmpSustain, 1.0f);
            rig.engine.setParameter(E::BendRangeParam, 24.0f);
            rig.engine.noteOn(115, 1.0f, -1);
            for (int i = 0; i < 1500; ++i)
            {
                rig.engine.pitchBend(std::sin(6.2831853f * (float) i / 750.0f));
                rig.render(64.0 / 48000.0);
            }
            expect(rig.finite && rig.peak < 0.5f, "the engine, bent through the top: peak " + juce::String(rig.peak, 3));
        }

        beginTest("a partial that dies long before its neighbours is emptied (no number dwindles to the smallest, which cost many "
                  "times a normal one on some processors) while the others ring on");
        {
            dsp::ModeBank bank;
            float c[dsp::ModeBank::MAX_MODES] {}, sn[dsp::ModeBank::MAX_MODES] {}, in[dsp::ModeBank::MAX_MODES] {}, out[dsp::ModeBank::MAX_MODES] {};
            for (int i = 1; i <= 2; ++i)
            {
                const float radius = i == 1 ? 0.99999f : 0.9f, w = 0.1f * (float) i;
                c[i] = radius * std::cos(w);
                sn[i] = radius * std::sin(w);
                in[i] = out[i] = 1.0f;
            }
            bank.retarget(c, sn, in, out, 8, 0);
            bank.tick(1.0f);
            bool tiny = false;
            for (int i = 1; i <= 2000; ++i)
            {
                bank.tick(0.0f);
                if (i % 64 == 0)
                    bank.rest(R::RESTS);
                for (int m = 0; m < 8; ++m)
                    tiny = tiny || std::fpclassify(bank.x[m]) == FP_SUBNORMAL || std::fpclassify(bank.y[m]) == FP_SUBNORMAL;
            }
            expect(! tiny, "no state was ever a subnormal number");
            expect(bank.x[2] == 0.0f && bank.y[2] == 0.0f, "the short partial is emptied");
            expect(bank.x[1] * bank.x[1] + bank.y[1] * bank.y[1] > 0.9f && bank.awake == 8, "the long one rings on");
        }

        beginTest("modes at rest are not run while coefficients move, and still arrive: one at rest when a glide begins is "
                  "moved at once, one put to rest during a glide is moved then; woken, each rings where it belongs");
        {
            const int n = dsp::ModeBank::MAX_MODES;
            float c[dsp::ModeBank::MAX_MODES] {}, sn[dsp::ModeBank::MAX_MODES] {}, in[dsp::ModeBank::MAX_MODES] {}, out[dsp::ModeBank::MAX_MODES] {};
            auto tune = [&](float scale)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float w = scale * 0.01f * (float) (i + 1);
                    c[i] = 0.999f * std::cos(w);
                    sn[i] = 0.999f * std::sin(w);
                    in[i] = 1.0f;
                    out[i] = scale;
                }
            };
            auto arrived = [&](const dsp::ModeBank& bank)
            {
                float off = 0.0f;
                for (int i = 0; i < n; ++i)
                    off = std::max({ off, std::abs(bank.c[i] - c[i]), std::abs(bank.s[i] - sn[i]), std::abs(bank.in[i] - in[i]), std::abs(bank.out[i] - out[i]) });
                return off;
            };
            for (int restAt : { -1, 5 })       // at rest before the glide begins; put to rest 5 samples into it
            {
                dsp::ModeBank bank;
                tune(1.0f);
                bank.retarget(c, sn, in, out, n, 0);
                bank.tick(1.0f);
                // The first sixteen modes ring on; the others are emptied and put to rest.
                auto restUpper = [&]
                {
                    for (int i = 16; i < n; ++i) bank.x[i] = bank.y[i] = 0.0f;
                    bank.rest(1.0e-14f);
                };
                if (restAt < 0) restUpper();
                tune(1.5f);
                bank.retarget(c, sn, in, out, n, 16);
                for (int i = 0; i < 16; ++i)
                {
                    if (i == restAt) restUpper();
                    bank.tick(0.0f);
                }
                expectEquals(bank.awake, 16);
                expect(bank.glide == 0 && arrived(bank) < 1.0e-5f, juce::String(restAt < 0 ? "at rest before" : "put to rest during") + " the glide: every mode is "
                       "where it was sent (off by " + juce::String(arrived(bank), 7) + " at most)");
                bank.tick(1.0f);
                expectEquals(bank.awake, n, "a force wakes them all");
            }
        }

        beginTest("a position that jumps is glided to (about 1.5 ms) and the coefficients move sample by sample: a ringing body "
                  "whose position jumps, or is swept, has no step in its sound");
        {
            res::BodyLatent dull {};
            dull[res::Tilt] = 1.0f;
            dull[res::Damp] = 0.6f;
            // The largest third difference: a step in the sound or in its slope stands far out of a smooth one's.
            auto rough = [](const std::vector<float>& x, size_t from, size_t to)
            {
                double most = 0.0;
                for (size_t i = from + 3; i < to; ++i)
                    most = std::max(most, (double) std::abs(x[i] - 3.0f * x[i - 1] + 3.0f * x[i - 2] - x[i - 3]));
                return most;
            };
            for (int sweep = 0; sweep < 2; ++sweep)
            {
                Solo s;
                s.r.setBody((float) res::Comb, dull);
                s.r.setExciter((float) res::Strike);
                s.r.setPitch(110.0);
                s.r.setDecay(4.0f);
                s.r.strike(1.0f);
                std::vector<float> x;
                for (int i = 0; i < 9600; ++i) x.push_back(s.r.tick(0.0f, 0.0f));
                float humSoon = 0.0f;
                for (int i = 0; i < 4800; ++i)
                {
                    s.r.setBody(sweep ? 2.0f + 3.0f * (float) i / 4800.0f : (float) res::Bell, dull);
                    x.push_back(s.r.tick(0.0f, 0.0f));
                    if (i == s.r.controlPeriod()) humSoon = s.r.body()[res::Hum];
                }
                const double before = rough(x, 4800, 9600), during = rough(x, 9600, 14400);
                expect(rms(x, 4800, 4800) > 0.05, "it rings");
                expect(during < 5.0 * before, juce::String(sweep ? "swept" : "jumped") + ": the roughest place is " + juce::String(during / before, 1) + " x the ringing's own");
                if (! sweep)
                {
                    expect(humSoon > 0.02f && humSoon < 0.4f, "one look after the jump the hum is on its way (" + juce::String(humSoon, 3) + " of 0.5)");
                    expectWithinAbsoluteError(s.r.body()[res::Hum], res::bodyPoints()[res::Bell][res::Hum], 1.0e-3f, "and there after 100 ms");
                }
            }
        }

        beginTest("CONT between its ends: the burst lasts longer (2 ms at 0, a quarter of a second half-way); GRAN under a junction "
                  "is turbulence in the drive (a breath is never twice the same); STIF stretches a sheet's and a shell's partials too");
        {
            auto burst = [&](float cont, double& first, double& later)
            {
                Solo s;
                s.r.setBody((float) res::Comb);
                s.r.setExciter(res::ExciteLatent { { cont, 1.0f, 0.0f, 0.0f, 1.0f } });
                s.r.setPitch(220.0);
                s.r.setDecay(0.01f);
                const auto x = s.play(0.1, 1.0f, 0.0f);
                first = rms(x, 0, 240);
                later = rms(x, 960, 1920);
            };
            double first = 0.0, later = 0.0;
            burst(0.0f, first, later);
            expect(first > 0.05 && later < 1.0e-3 * first, "CONT 0: gone after 20 ms (" + juce::String(dB(later / first), 1) + " dB)");
            burst(0.5f, first, later);
            expect(first > 0.05 && later > 0.3 * first, "CONT half-way: still there after 20 ms (" + juce::String(dB(later / first), 1) + " dB)");

            for (float grain : { 0.0f, 1.0f })
            {
                std::vector<float> a, b;
                for (int pass = 0; pass < 2; ++pass)
                {
                    Solo s;
                    s.r.setBody((float) res::Tube);
                    s.r.setExciter(res::ExciteLatent { { 1.0f, grain, 1.0f, 1.0f, 0.45f } });     // (COUP 1: no grain is fed to the body itself)
                    s.r.setPitch(220.0);
                    s.r.seedNoise(pass == 0 ? 12345u : 777u);
                    (pass == 0 ? a : b) = s.play(1.0, 1.0f, 1.0f);
                }
                double apart = 0.0;
                for (size_t i = 24000; i < 48000; ++i)
                    apart += (double) (a[i] - b[i]) * (a[i] - b[i]);
                apart = std::sqrt(apart / 24000.0);
                expect(rms(a, 24000) > 0.05, "the reed sounds");
                if (grain == 0.0f) expect(apart == 0.0, "no grain: the same sound whatever the noise");
                else               expect(apart > 0.003, "grain: the noise is in the drive (" + juce::String(dB(apart / rms(a, 24000)), 1) + " dB of the sound)");
            }

            for (int b : { (int) res::Plate, (int) res::Bell })
            {
                Solo plain, stiff;
                plain.sound((float) b, 0.0f, 110.0).play(0.01);
                stiff.r.setBody((float) b, bodyOffset(res::Stiff, 1.0f));
                stiff.r.setPitch(110.0);
                stiff.play(0.01);
                expect(stiff.r.partial(1) == 1.0f && stiff.r.partial(8) > 1.5f * plain.r.partial(8) && stiff.r.partial(2) < 2.0f * plain.r.partial(2),
                       juce::String(modelName(b)) + " with STIF: partial 8 from " + juce::String(plain.r.partial(8), 2) + " to " + juce::String(stiff.r.partial(8), 2));
            }
        }

        beginTest("the input is let in by velocity x envelope and opened further by pressure (up to twice); a voice that was "
                  "pressed is played from the keys again by velocity x envelope; a voice struck again starts from silence");
        {
            auto let = [&](float velocity, float pressure)
            {
                Rig rig;
                rig.sound((float) res::Comb, (float) res::Strike);
                rig.excite();
                rig.engine.setParameter(E::InputParam, 1.0f);
                rig.engine.setParameter(E::AmpSustain, 1.0f);
                rig.engine.setParameter(E::DecayParam, 0.3f);
                rig.feed = Rig::Noise;
                rig.engine.noteOn(57, velocity, -1);
                if (pressure > 0.0f) rig.engine.aftertouch(57, pressure);
                rig.render(1.0);
                return rms(rig.render(0.5));
            };
            const double soft = let(0.25f, 0.0f), hard = let(1.0f, 0.0f), pressed = let(0.25f, 1.0f);
            expectWithinAbsoluteError(dB(hard / soft), 12.0f, 2.0f, "velocity 1 against 0.25");
            expectWithinAbsoluteError(dB(pressed / soft), 6.0f, 1.5f, "pressed all the way: twice");

            Rig one(48000.0, 64, 1);
            one.sound((float) res::String, (float) res::Bow);
            one.excite();
            one.engine.setParameter(E::AmpRelease, 0.01f);
            one.engine.setParameter(E::DecayParam, 0.2f);
            one.engine.noteOn(52, 0.8f, -1);
            one.engine.aftertouch(52, 1.0f);
            one.render(0.5);
            one.engine.aftertouch(52, 0.0f);
            one.engine.noteOff(52, -1);
            one.render(1.0);
            one.engine.noteOn(55, 0.8f, -1);        // (the same voice, from the keys: no pressure comes)
            one.render(0.7);
            const double fromKeys = rms(one.render(0.3));
            expect(fromKeys > 0.01, "after a pressed note the voice is bowed from the keys: " + juce::String(dB(fromKeys), 1) + " dB");

            Rig again(48000.0, 64, 1);
            again.sound((float) res::Comb, (float) res::Strike);
            again.engine.setParameter(E::DecayParam, 20.0f);
            again.engine.setParameter(E::AmpSustain, 1.0f);
            again.engine.noteOn(45, 1.0f, -1);
            again.render(0.3);
            const double ringing = rms(again.render(0.1));
            again.engine.noteOn(81, 0.02f, -1);
            again.render(0.05);
            const double after = rms(again.render(0.2));
            expect(ringing > 0.01 && after < 0.1 * ringing, "struck again softly, the loud note's ringing is not carried over (" + juce::String(dB(after / ringing), 1) + " dB)");
        }
    }

    // ---------------------------------------------------------------------------------------
    void robustness()
    {
        beginTest("the engine at its edges: every point x point, notes 0 and 127, 22.05 to 192 kHz, blocks of 1 to 4096, every cell "
                  "at both ends in turn, the strip, bend and pressure at their ends, a loud input with not-a-number in it");
        {
            int runs = 0;
            for (double sr : { 22050.0, 48000.0, 192000.0 })
                for (int block : { 1, 64, 4096 })
                    for (int x = 0; x < 4; ++x)
                    {
                        if (block == 1 && sr != 48000.0) continue;
                        const int b = (x + (int) (sr / 1000.0) + block) % 6;
                        Rig rig(sr, block);
                        rig.sound((float) b, (float) x);
                        rig.excite();
                        rig.feed = Rig::Noise;
                        rig.feedLevel = 1.0f;
                        rig.engine.setParameter(E::InputParam, 0.5f);
                        for (int note : { 0, 127, 60, 36 })
                            rig.engine.noteOn(note, 1.0f, -1);
                        rig.engine.aftertouch(60, 1.0f);
                        rig.engine.setModWheel(1.0f);
                        rig.engine.pitchBend(-1.0f);
                        const double slice = (block == 1 ? 144.0 : 576.0) / sr;      // (in samples: the same work at every rate)
                        for (auto& page : rig.engine.getParamPages())
                            for (auto& p : page.params)
                            {
                                if (page.name == "STRIP" || page.name.startsWith("LFO") || page.name.startsWith("SLT") || page.name == "LSHP") continue;
                                for (float v : { p.maxVal, p.minVal, p.defaultVal })
                                {
                                    rig.engine.setParameter(p.id, v);
                                    rig.render(slice);
                                }
                            }
                        std::vector<float> bad((size_t) block, std::numeric_limits<float>::quiet_NaN());
                        rig.engine.setAudioInput({ bad.data(), bad.data(), block, std::numeric_limits<float>::infinity() });
                        juce::AudioBuffer<float> buffer(2, block);
                        buffer.clear();
                        juce::MidiBuffer midi;
                        rig.engine.processBlock(buffer, midi);
                        for (int i = 0; i < buffer.getNumSamples(); ++i)
                            rig.finite = rig.finite && std::isfinite(buffer.getSample(0, i));
                        rig.render(0.05);
                        ++runs;
                        expect(rig.finite && rig.peak <= 8.0f * 0.5f * 4.0f, juce::String(sr, 0) + " Hz, block " + juce::String(block) + ", " + exciterName(x) + " "
                               + modelName(b) + ": peak " + juce::String(rig.peak, 2));
                    }
            expect(runs >= 28);
        }

        beginTest("left alone it dies away: every point, the longest DCAY, nothing damped - 15 dB down after a quarter of DCAY; "
                  "allNotesOff is silence at once");
        {
            for (int b = 0; b < 6; ++b)
            {
                Rig rig(48000.0, 512);
                rig.sound((float) b, 0.0f);
                rig.engine.setParameter(E::DecayParam, 20.0f);
                rig.engine.setParameter(E::BodyBase + res::Damp, -1.0f);
                rig.engine.setParameter(E::AmpSustain, 1.0f);
                rig.engine.noteOn(45, 1.0f, -1);
                const double early = rms(rig.render(0.5));
                rig.render(4.5);
                const double late = rms(rig.render(0.5));
                expectWithinAbsoluteError(dB(late / early), -15.0f, 2.0f, juce::String(modelName(b)) + " after 5 s of DCAY 20");
                rig.engine.allNotesOff();
                expect(rms(rig.render(0.1)) == 0.0, "allNotesOff");
            }
        }

        beginTest("the mod strip at the defaults of the STRIP page: more energy under a fed exciter");
        {
            for (float x : { 1.0f, 3.0f })
            {
                double level[2] = {};
                for (int up = 0; up < 2; ++up)
                {
                    Rig rig;
                    rig.sound((float) res::String, x);
                    rig.engine.setParameter(E::AmpSustain, 1.0f);
                    rig.engine.setModWheel((float) up);
                    rig.engine.noteOn(52, 0.8f, -1);
                    rig.render(1.0);
                    level[up] = rms(rig.render(0.5));
                }
                expect(level[1] > 1.3 * level[0], juce::String(exciterName((int) x)) + ": the strip up, " + juce::String(dB(level[1] / level[0]), 1) + " dB louder");
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    void presets()
    {
        beginTest("the three morph presets (project/presets/ResonatorPresets.h; registered nowhere yet): each is an RSN sound "
                  "whose every value is a cell of the engine within its range, loads, sounds, and does what its name says");
        {
            // (After the three: every ready sound of the resonator drum as a preset on the keys, and
            // three more that use the slots and LFOs. All are loaded and played below; the "Resonator
            // Drum" suite has what they are.)
            const auto all = resonatorPresets();
            expectEquals((int) all.size(), 3 + (int) resonatorSounds().size() + 3);
            auto load = [&](Rig& rig, const Preset& preset)
            {
                expect(preset.engineType == EngineType::Resonator && preset.name.isNotEmpty() && preset.name.length() <= 16, preset.name);
                expect(preset.category.isNotEmpty() && preset.isFactory);
                std::map<int, ParamSpec> cells;
                for (auto& page : rig.engine.getParamPages())
                    for (auto& p : page.params) cells[p.id] = p;
                for (auto& [id, value] : preset.soundParams)
                {
                    // (An envelope stage's own curve is no cell of a page: Shift + its knob.)
                    expect(cells.count(id) > 0 || StageCurveParam::owns(id), preset.name + ": id " + juce::String(id) + " is a cell");
                    if (cells.count(id) > 0)
                        expect(value >= cells[id].minVal && value <= cells[id].maxVal, preset.name + ": " + cells[id].label + " = " + juce::String(value));
                    rig.engine.setParameter(id, value);
                }
            };

            // 1. The envelope morphs string to bell within each note.
            {
                Rig rig(48000.0, 32);
                load(rig, all[0]);
                rig.engine.noteOn(57, 0.9f, -1);
                rig.render(0.003);
                const float first = bodyNow(rig, 0, res::Shell);
                float most = first, least = first;
                std::vector<float> out;
                for (int i = 0; i < 3000; ++i)
                {
                    const auto x = rig.render(32.0 / 48000.0);
                    out.insert(out.end(), x.begin(), x.end());
                    most = std::max(most, bodyNow(rig, 0, res::Shell));
                    least = std::min(least, bodyNow(rig, 0, res::Shell));
                }
                expect(most - least > 0.9f, all[0].name + ": SHEL goes from " + juce::String(least, 2) + " to " + juce::String(most, 2) + " within the note");
                expect(rig.finite && rms(out, 0, 24000) > 0.01 && rig.peak < 1.0f, all[0].name + " sounds: " + juce::String(dB(rms(out, 0, 24000)), 1) + " dB");
                expect(std::abs(centsOff(out, 2400, 24000, 220.0, 48000.0)) < 3.0, all[0].name + " stays on its note while it morphs");
            }
            // 2. Another body every note.
            {
                Rig rig;
                load(rig, all[1]);
                std::set<int> places;
                double quietest = 1.0;
                for (int n = 0; n < 8; ++n)
                {
                    rig.engine.noteOn(50 + n, 0.9f, -1);
                    quietest = std::min(quietest, rms(rig.render(0.25)));
                    places.insert((int) std::lround(rig.engine.getVoice(n).getCell(V::Morph) * 50.0f));
                    rig.engine.noteOff(50 + n, -1);
                }
                expect(places.size() >= 6, all[1].name + ": " + juce::String((int) places.size()) + " bodies for 8 notes");
                expect(rig.finite && quietest > 0.003 && rig.peak < 1.0f, all[1].name + " sounds on every note");
            }
            // 3. Struck, then bowed under pressure.
            {
                Rig rig;
                rig.excite();
                load(rig, all[2]);
                rig.engine.noteOn(52, 0.8f, -1);
                const double struck = rms(rig.render(0.2));
                rig.render(2.5);
                const double resting = rms(rig.render(0.3));
                rig.engine.aftertouch(52, 1.0f);
                rig.render(1.5);
                const double pressed = rms(rig.render(0.3));
                expect(struck > 0.01 && resting < 0.1 * struck, all[2].name + ": struck (" + juce::String(dB(struck), 1) + " dB), then ringing out");
                expect(pressed > 5.0 * resting && pressed > 0.01, all[2].name + ": bowed under pressure (" + juce::String(dB(pressed), 1) + " dB)");
                expect(exciteNow(rig, 0, res::Couple) > 0.6f, "the exciter has gone to the bow");
                expect(rig.finite && rig.peak < 1.0f);
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // NEPTUNE_RSN_BENCH=1: the time a block takes (build-release for numbers that mean something).
    // The median of the blocks, so that whatever else the machine does is not counted.
    template <class Engine>
    static double benchRun(const char* what, int base, const std::function<void(Engine&)>& setup, int* modes = nullptr)
    {
        Engine engine(8);
        engine.prepareToPlay(48000.0, 512);
        setup(engine);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        static const int steps[8] = { 0, 4, 7, 11, 14, 17, 21, 24 };
        const int blocks = 48000 * 10 / 512, period = 47;
        std::vector<double> times;
        for (int b = 0; b < blocks; ++b)
        {
            if (b % period == 0)  for (int s : steps) engine.noteOn(base + s, 0.8f, 0);
            if (b % period == 35) for (int s : steps) engine.noteOff(base + s, 0);
            buffer.clear();
            const auto t0 = std::chrono::steady_clock::now();
            engine.processBlock(buffer, midi);
            times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(times.begin(), times.end());
        if (modes != nullptr) *modes = benchModes(engine);
        juce::ignoreUnused(what);
        return times[times.size() / 2];
    }
    static int benchModes(E& e) { return e.getVoice(0).getResonator().modesRunning(); }
    template <class Other> static int benchModes(Other&) { return 24; }

    static void bench()
    {
        std::printf("microseconds a 512-sample block (the median of 937), 8 notes held and struck again every half second, 48 kHz\n");
        for (int base : { 36, 60, 84 })
        {
            auto row = [base](const juce::String& what, float body, float exciter, int lfos)
            {
                int modes = 0;
                double best = 1.0e9;
                for (int pass = 0; pass < 3; ++pass)
                    best = std::min(best, benchRun<E>(what.toRawUTF8(), base, [=](E& e)
                    {
                        e.setParameter(E::MorphParam, body);
                        e.setParameter(E::ExciterMorphParam, exciter);
                        e.setParameter(E::DecayParam, 8.0f);
                        e.setParameter(E::AmpRelease, 3.0f);
                        if (lfos == 1) { setLfo(e, 0, "LEVL", 1.0f, 5.0f); setLfo(e, 1, "PAN", 1.0f, 3.0f); }
                        if (lfos == 2) { setLfo(e, 0, "MRPH", 1.0f, 5.0f); setLfo(e, 1, "PAN", 1.0f, 3.0f); }
                        if (lfos == 3) { setLfo(e, 0, "MRPH", 1.0f, 5.0f); setLfo(e, 1, "XMPH", 1.0f, 3.0f); }
                    }, &modes));
                std::printf("  note %2d  %-24s %7.1f us   (%d modes in the first voice)\n", base, what.toRawUTF8(), best, modes);
            };
            for (int b = 0; b < 6; ++b)
                row(juce::String(modelName(b)) + " STRK", (float) b, 0.0f, 0);
            row("STRG BOW", 0.0f, 1.0f, 0);
            row("TUBE BRTH", 1.0f, 2.0f, 0);
            row("PLAT BOW", 3.0f, 1.0f, 0);
            row("BELL SCRP", 5.0f, 3.0f, 0);
            row("LFOs on LEVL and PAN", 2.5f, 0.0f, 1);
            row("LFOs on MRPH and PAN", 2.5f, 0.0f, 2);
            row("LFOs on MRPH and XMPH", 2.5f, 1.5f, 3);
        }
    }

    void cost()
    {
        beginTest("the time one block of 8 morphing voices takes (this is the Debug build: the app's is many times faster)");
        {
            E engine(8);
            engine.prepareToPlay(48000.0, 512);
            setLfo(engine, 0, "MRPH", 1.0f, 5.0f);
            for (int n = 0; n < 8; ++n)
                engine.noteOn(36 + 3 * n, 1.0f, -1);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < 40; ++b)
            {
                buffer.clear();
                engine.processBlock(buffer, midi);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 40.0;
            logMessage("8 voices, 64 modes each, the body morphing: " + juce::String(ms, 3) + " ms a block of 512 (10.67 ms of sound)");
            expect(ms < 60.0, "nothing pathological (the Debug build runs it in about half of real time; a sanitizer build is slower)");
        }
    }

    void runTest() override
    {
        if (juce::SystemStats::getEnvironmentVariable("NEPTUNE_RSN_BENCH", {}).isNotEmpty())
        {
            beginTest("bench");
            bench();
            return;
        }
        latentSpace();
        character();
        tuning();
        morph();
        exciters();
        extremes();
        identity();
        modulation();
        energy();
        input();
        robustness();
        review();
        presets();
        cost();
    }
};

static ResonatorTests resonatorTests;
