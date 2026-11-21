#pragma once

#include "DSPUtils.h"
#include "effects/FxBlocks.h"
#include <algorithm>
#include <cmath>

// The saturation curves: one list of memoryless shapes, shared by every effect that lets the
// player choose one (waveshaper CURV, amp SAT, dub echo SAT). All are textbook waveshapers (Zoelzer,
// "DAFX", nonlinear processing; Pakarinen & Yeh 2009 for the tube and diode shapes).
//
// Every curve: gives 0 for 0 (silence stays silence), is finite for any finite input, and is held
// within about +-2 for any input except SOFT and ASYM (kept as the waveshaper always had them: above
// 3 they go on rising slowly, at a ninth of the input). Most have a slope of 1 at 0, so a quiet sound passes
// unchanged; the exceptions say so.
//
// The index is stored in projects and presets: the list only grows at the end, and the first four
// are the waveshaper's original CURV values.
namespace sat
{
enum Curve
{
    Soft = 0,   // rational tanh left open: soft knee, then a slow rise instead of a ceiling
    Hard,       // hard clip at +-1
    Fold,       // sine wavefolder: past +-1 the wave turns back on itself
    Asym,       // two different knees: the upper half bends early (x 1.5), the lower late (x 0.8)
    Tanh,       // the true hyperbolic tangent
    Fast,       // rational tanh with a ceiling: almost Tanh, a little squarer at the top
    Atan,       // arctangent: a rounder knee that never quite flattens
    Algebraic,  // x / sqrt(1 + x^2): between Tanh and Atan
    Slow,       // x / (1 + |x|): the gentlest knee, compresses long before it clips
    Cubic,      // x - 4x^3/27 up to 1.5, flat beyond: clean until late, then a firm ceiling
    Diode,      // 1 - e^-|x|: an early, rounded knee (a diode pair)
    Tube,       // lopsided ceilings (+1.3, -0.7): even harmonics without any bias (a triode stage)
    TriFold,    // triangle wavefolder: straight segments, a harder and brighter fold than Fold
    Rectify,    // full-wave rectifier: both halves go up - the octave above (silent when squared off)
    HalfWave,   // half-wave rectifier: the lower half is cut away
    Crossover,  // dead zone round zero (a class-B stage): quiet sound vanishes, loud sound rasps
    Even,       // adds the square: the second harmonic rises with the level
    Cheby3,     // third Chebyshev polynomial: a full-level sine comes out as its third harmonic;
                // a quiet sound passes three times louder and upside down
    Count
};

inline const char* const* names()
{
    static const char* const n[Count] = { "SOFT", "HARD", "FOLD", "ASYM", "TANH", "FAST", "ATAN", "ALGB", "SLOW",
                                          "CUBE", "DIOD", "TUBE", "TRI", "RECT", "HALF", "XOVR", "EVEN", "CHB3" };
    return n;
}

static constexpr float OPEN_LIMIT = 1.0e6f;    // SOFT and ASYM have no ceiling: their input is held here (far beyond any sound)
static constexpr float DEAD_ZONE = 0.12f;     // Crossover: this much either side of zero gives nothing

inline int valid(int curve) { return std::clamp(curve, 0, (int) Count - 1); }

inline float shape(int curve, float x)
{
    switch (curve)
    {
        case Soft:      return dsp::fastTanh(std::clamp(x, -OPEN_LIMIT, OPEN_LIMIT));
        case Hard:      return std::clamp(x, -1.0f, 1.0f);
        case Fold:      return std::sin(std::clamp(x, -5.0f, 5.0f) * dsp::PI_F * 0.5f);
        case Asym:      return dsp::fastTanh(std::clamp(x, -OPEN_LIMIT, OPEN_LIMIT) * (x > 0.0f ? 1.5f : 0.8f));
        case Tanh:      return std::tanh(x);
        case Fast:      return fxb::softSat(x);
        case Atan:      return (2.0f / dsp::PI_F) * std::atan(x * dsp::PI_F * 0.5f);
        case Algebraic: { const float c = std::clamp(x, -OPEN_LIMIT, OPEN_LIMIT); return c / std::sqrt(1.0f + c * c); }
        case Slow:      return x / (1.0f + std::abs(x));
        case Cubic:
        {
            const float c = std::clamp(x, -1.5f, 1.5f);
            return c - (4.0f / 27.0f) * c * c * c;
        }
        case Diode:     return std::copysign(1.0f - std::exp(-std::min(std::abs(x), 30.0f)), x);
        case Tube:      return x > 0.0f ? 1.3f * (x / (1.3f + x)) : 0.7f * fxb::softSat(x / 0.7f);
        case TriFold:
        {
            const float p = (std::clamp(x, -1.0e4f, 1.0e4f) - 1.0f) * 0.25f;
            return 4.0f * std::abs(p - std::floor(p) - 0.5f) - 1.0f;
        }
        case Rectify:   return std::abs(fxb::softSat(x));
        case HalfWave:  return std::max(fxb::softSat(x), 0.0f);
        case Crossover: return fxb::softSat(std::copysign(std::max(std::abs(x) - DEAD_ZONE, 0.0f), x));
        case Even:
        {
            const float c = fxb::softSat(x);
            return c + c * c;
        }
        case Cheby3:
        {
            const float c = fxb::softSat(x);
            return (4.0f * c * c - 3.0f) * c;
        }
        default:        return fxb::softSat(x);
    }
}

// A curve worked off-centre: the two halves of the wave meet different parts of it, which adds even
// harmonics to any curve. Still 0 for 0; the steady offset a loud sound is left with is the
// caller's to take out (a high-pass after it).
struct Biased
{
    void set(int curveIndex, float offset)
    {
        curve = valid(curveIndex);
        bias = offset;
        out = shape(curve, bias);
        // The curve's slope where it is worked (never taken as less than a fifth: Crossover and
        // Rectify have flat or turning places).
        const float h = 0.01f;
        slope = std::max(std::abs(shape(curve, bias + h) - shape(curve, bias - h)) / (2.0f * h), 0.2f);
    }

    float operator()(float x) const { return shape(curve, x + bias) - out; }

    int curve = Fast;
    float bias = 0.0f, out = 0.0f, slope = 1.0f;
};
}
