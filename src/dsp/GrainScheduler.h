#pragma once

#include "DSPUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// -----------------------------------------------------------------------------
// One scheduler of grains: short windowed events read from stored sound and summed.
// It keeps the clock (when the next grain is due: a density, scattered by a jitter
// with the mean unchanged), a fixed pool of grains, and each grain's own place,
// length, pitch, level, pan and window. It knows nothing of what is read: the caller
// gives a function from a place (in frames) to a sample, so a sampler voice reads its
// sample with its own interpolation and the resonator's exciter reads a recording of
// the sample bank (dsp/Resonator.h, the GRAN axis).
//
// The scheme is the textbook one (Curtis Roads, "Microsound", MIT Press 2001, ch. 3
// and 5): quasi-synchronous while the jitter is 0, asynchronous as it rises; standard
// windows (Hann, triangle, Roads's "expodec" / "rexpodec"); a sum.
//
// One output sample, for a caller that owns the position:
//     if (cloud.due(density, 1 / sampleRate))                  // the clock
//         cloud.start(spec, position, area, step, sampleRate); // one grain; draws the next interval
//     cloud.sum(read, step, area, left, right);                // or sumMono(read, step, area)
// position: where a grain starts, in frames; area: what grains may read (a grain is
// moved back so that all of it lies inside, and wraps inside when it is longer);
// step: frames per output sample at the note's pitch, which keeps acting on grains
// that already sound (a bend, a pitch LFO); a grain's own pitch is a ratio on it.
//
// Loudness: each grain is scaled by 1 / sqrt(overlap), overlap = size x density held
// to 1 .. the pool: grains that do not line up add in power, so a cloud is as loud
// whatever its size and density (about -4 dB re. the source with Hann grains); sparse
// grains (overlap under 1) play at full level; a grain due while the pool is full is
// skipped (nothing is stolen), and its random numbers are drawn all the same, so a
// full pool does not change what the following grains get.
//
// Everything the sampler's granular mode does is here, in the same arithmetic and the
// same order of random draws (position, pitch, pan, reverse, then the next interval),
// so that SamplerEngine can be moved onto it in phase U4 of .ralph/spec-unify.md
// without a change in its sound:
//     its grainPhase / grainThreshold / grainRng / grains[]      -> one Cloud<48> per voice
//     SIZE, DENS (and their LFOs)      -> Spec::size, Spec::density, due(density, ...)
//     SPRY                             -> Spec::jitter AND Spec::scatter, scatterFrames = 250 ms of frames
//     PJIT (octaves, squared)          -> Spec::pitchJitter;   SPRD -> Spec::spread;   RVRS -> Spec::reverse
//     SHAP                             -> Spec::window;        a note that runs backwards -> Spec::backwards
//     GPOS, the scan position          -> the caller's `position` (it moves and wraps it)
//     the grain area (slice or sample) -> Area;                readSampleStereoInterp -> the `read` function
// A stretch that puts grains at regular places of its own (GRAN / SMER) calls start() when it wants one.
//
// Nothing here allocates or locks; any value may be given (what is no number is taken as 0 or
// held to the area), and a reader is only ever asked for places from the area's start to its end
// (the end itself included: it holds its place to what it has, as readLinear does).
// -----------------------------------------------------------------------------
namespace grn
{
enum Window { Hann = 0, Triangle, Perc, Swell, NumWindows };

static constexpr int WINDOW_SIZE = 1024;

// Window shapes over 0..1 (one guard point so the interpolated lookup needs no wrap).
//   HANN  0.5 - 0.5 cos(2 pi p)
//   TRI   1 - |2p - 1|
//   PERC  "expodec": linear attack over the first 2 %, then exp(-5 q) brought to exactly 0 at the end
//   SWEL  "rexpodec": PERC backwards
inline const std::array<std::array<float, WINDOW_SIZE + 2>, NumWindows>& windows()
{
    static const auto tables = []
    {
        std::array<std::array<float, WINDOW_SIZE + 2>, NumWindows> t {};
        const double attack = 0.02, k = 5.0, floorLevel = std::exp(-k);
        auto perc = [&](double p)
        {
            if (p < attack) return p / attack;
            const double q = (p - attack) / (1.0 - attack);
            return (std::exp(-k * q) - floorLevel) / (1.0 - floorLevel);
        };
        for (int i = 0; i <= WINDOW_SIZE + 1; ++i)
        {
            const double p = std::min(1.0, (double) i / (double) WINDOW_SIZE);
            t[Hann][(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * p));
            t[Triangle][(size_t) i] = (float) (1.0 - std::abs(2.0 * p - 1.0));
            t[Perc][(size_t) i] = (float) perc(p);
            t[Swell][(size_t) i] = (float) perc(1.0 - p);
        }
        return t;
    }();
    return tables;
}

// What grains may read, in frames: start .. end (end is not read).
struct Area
{
    double start = 0.0, end = 1.0;
};

struct Grain
{
    double pos = 0.0;       // frames
    float ratio = 1.0f;     // its own pitch on the caller's step; negative = it plays backwards
    float phase = 0.0f;     // 0..1 through its window
    float phaseInc = 0.0f;
    float gainL = 1.0f, gainR = 1.0f;
    int window = Hann;
};

// What a new grain is made from.
struct Spec
{
    float size = 0.1f;          // seconds
    float density = 40.0f;      // grains a second: with size, the overlap its level is set by
    float jitter = 0.0f;        // 0..1: the next interval is 1 / density x (1 + jitter x u), never under a twentieth
    float scatter = 0.0f;       // 0..1: its place is moved by scatter x u x scatterFrames (reflected at the area's ends)
    float scatterFrames = 0.0f;
    float pitchJitter = 0.0f;   // octaves at u = 1
    float spread = 0.0f;        // 0..1: equal-power place between the channels, spread x u round the centre
    float reverse = 0.0f;       // the chance that it plays backwards
    float level = 1.0f;
    int window = Hann;
    bool backwards = false;     // the note itself runs backwards: a grain ends at the position
};

// A mono buffer read with a straight line between frames, at any place: held to the buffer.
inline float readLinear(const float* data, int frames, double pos) noexcept
{
    const double last = (double) (frames - 1);
    const double p = pos >= 0.0 ? std::min(pos, last) : 0.0;        // (what is no number is 0)
    const int i = (int) p;
    const float t = (float) (p - (double) i);
    const float a = data[i], b = data[std::min(i + 1, frames - 1)];
    return a + (b - a) * t;
}

template <int MaxGrains>
class Cloud
{
public:
    static constexpr int MAX_GRAINS = MaxGrains;
    static constexpr float MIN_GRAIN_SAMPLES = 8.0f;

    Cloud() { windows(); }      // (built here, never on the audio thread)

    // No grain sounds and the first is due with the next sample.
    void reset() noexcept
    {
        count = 0;
        phase = 1.0;
        threshold = 1.0f;
    }
    // The same seed, the same cloud.
    void seed(uint32_t s) noexcept  { rng = s != 0 ? s : 0x1234567u; }
    int sounding() const noexcept   { return count; }
    const Grain& grain(int i) const noexcept { return grains[(size_t) std::clamp(i, 0, MaxGrains - 1)]; }
    void clear() noexcept           { count = 0; }

    // The clock, once an output sample: true when a grain is due.
    bool due(float density, double invSampleRate) noexcept
    {
        phase += (double) (density > 0.0f ? std::min(density, 1.0e5f) : 0.0f) * invSampleRate;
        if (! (phase >= (double) threshold))
            return false;
        phase = std::max(0.0, phase - (double) threshold);
        return true;
    }

    // The random numbers one grain takes, each -1 .. 1 but `reverse`, which is 0 .. 1: for a caller
    // that draws them itself (a seeded sequence of its own: the sampler's spread::Sequence).
    // `interval` is read only while the jitter is above 0.
    struct Draws { float pos = 0.0f, pitch = 0.0f, pan = 0.0f, reverse = 1.0f, interval = 0.0f; };

    // One grain at `position`, and the interval to the next. step: frames an output sample at the
    // note's pitch; sampleRate: the output's. Returns the grain, or nullptr when the pool is full.
    const Grain* start(const Spec& s, double position, const Area& area, double step, double sampleRate) noexcept
    {
        // The random numbers are drawn whether or not the grain is used.
        Draws d;
        d.pos = dsp::xorshiftNoise(rng);
        d.pitch = dsp::xorshiftNoise(rng);
        d.pan = dsp::xorshiftNoise(rng);
        d.reverse = dsp::xorshiftFloat(rng);
        if (s.jitter > 0.0f)
            d.interval = dsp::xorshiftNoise(rng);
        return start(s, position, area, step, sampleRate, d);
    }

    // The same with the caller's random numbers.
    const Grain* start(const Spec& s, double position, const Area& area, double step, double sampleRate, const Draws& d) noexcept
    {
        const float uPos = d.pos, uPitch = d.pitch, uPan = d.pan, uReverse = d.reverse;
        Grain* made = nullptr;
        if (count < MaxGrains)
        {
            auto& g = grains[(size_t) count++];
            made = &g;
            const float lengthSamples = std::max(MIN_GRAIN_SAMPLES, std::min(s.size * (float) sampleRate, 1.0e7f));
            g.phase = 0.0f;
            g.phaseInc = 1.0f / lengthSamples;
            g.window = std::clamp(s.window, 0, (int) NumWindows - 1);

            const float ratio = s.pitchJitter > 0.0f ? std::exp2f(s.pitchJitter * uPitch) : 1.0f;

            const double aStart = area.start, aEnd = std::max(area.end, area.start + 1.0), aLen = aEnd - aStart;
            double p = position;
            if (s.scatter > 0.0f)
            {
                p += (double) (s.scatter * uPos) * std::min((double) s.scatterFrames, aLen);
                if (p < aStart) p = aStart + (aStart - p);
                else if (p > aEnd - 1.0) p = (aEnd - 1.0) - (p - (aEnd - 1.0));
            }
            const double span = (double) lengthSamples * std::abs(held(step)) * (double) ratio;     // frames the grain covers
            if (s.backwards)
                p -= span;      // a note running backwards: the grain ends at the position
            if (span < aLen)
                p = std::min(p, aEnd - span);
            p = p >= aStart ? std::min(p, aEnd - 1.0) : aStart;     // (what is no number is the start)

            const bool reversed = (uReverse < s.reverse) != s.backwards;
            g.pos = reversed ? std::min(p + span, aEnd) : p;
            g.ratio = reversed ? -ratio : ratio;

            const float asked = s.size * s.density;
            const float overlap = asked >= 1.0f ? std::min(asked, (float) MaxGrains) : 1.0f;       // (what is no number is 1)
            const float gain = s.level / std::sqrt(overlap);
            if (s.spread > 0.0f)
            {
                const float angle = (s.spread * uPan + 1.0f) * 0.25f * dsp::PI_F;
                g.gainL = gain * 1.41421356f * std::cos(angle);
                g.gainR = gain * 1.41421356f * std::sin(angle);
            }
            else
            {
                g.gainL = g.gainR = gain;
            }
        }
        // Next interval: 1 / density, scattered by the jitter (mean unchanged).
        threshold = s.jitter > 0.0f ? std::max(0.05f, 1.0f + s.jitter * d.interval) : 1.0f;
        return made;
    }

    // The sum of the grains, which move on by one output sample. read(place, left, right).
    template <typename Read>
    void sum(Read&& read, double step, const Area& area, float& outL, float& outR) noexcept
    {
        const double aStart = area.start, aEnd = std::max(area.end, area.start + 1.0);
        const double inc = held(step);
        const auto& tables = windows();
        float l = 0.0f, r = 0.0f;
        for (int gi = 0; gi < count; )
        {
            auto& g = grains[(size_t) gi];
            float sl = 0.0f, sr = 0.0f;
            read(g.pos, sl, sr);
            const float w = windowAt(tables, g);
            l += sl * w * g.gainL;
            r += sr * w * g.gainR;
            if (advance(g, inc, aStart, aEnd))
                ++gi;
            else
                g = grains[(size_t) --count];       // finished: the last grain takes its place
        }
        outL = l;
        outR = r;
    }

    // The same for one channel. read(place) gives the sample.
    template <typename Read>
    float sumMono(Read&& read, double step, const Area& area) noexcept
    {
        const double aStart = area.start, aEnd = std::max(area.end, area.start + 1.0);
        const double inc = held(step);
        const auto& tables = windows();
        float out = 0.0f;
        for (int gi = 0; gi < count; )
        {
            auto& g = grains[(size_t) gi];
            out += read(g.pos) * windowAt(tables, g) * g.gainL;
            if (advance(g, inc, aStart, aEnd))
                ++gi;
            else
                g = grains[(size_t) --count];
        }
        return out;
    }

private:
    static double held(double step) noexcept { return std::abs(step) < 1.0e6 ? step : 0.0; }

    static float windowAt(const std::array<std::array<float, WINDOW_SIZE + 2>, NumWindows>& tables, const Grain& g) noexcept
    {
        const float x = g.phase * (float) WINDOW_SIZE;
        const int xi = (int) x;
        const float* tbl = tables[(size_t) g.window].data();
        return tbl[xi] + (x - (float) xi) * (tbl[xi + 1] - tbl[xi]);
    }

    // One output sample on; false when the grain is over.
    static bool advance(Grain& g, double inc, double aStart, double aEnd) noexcept
    {
        const double aLen = aEnd - aStart;
        g.pos += inc * (double) g.ratio;
        if (g.pos > aEnd) g.pos -= aLen;
        else if (g.pos < aStart) g.pos += aLen;
        if (g.pos > aEnd || g.pos < aStart)
        {
            // A step longer than the area (a very high note over a short slice): one wrap is
            // not enough, and the grain would run further out with every sample.
            const double off = std::fmod(g.pos - aStart, aLen);
            g.pos = aStart + (off < 0.0 ? off + aLen : off);
        }
        g.phase += g.phaseInc;
        return g.phase < 1.0f;
    }

    std::array<Grain, (size_t) MaxGrains> grains {};
    int count = 0;
    double phase = 1.0;         // the clock: a grain is due when it reaches the threshold
    float threshold = 1.0f;
    uint32_t rng = 0x9E3779B9u;
};
}
