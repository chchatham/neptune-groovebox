#pragma once

#include <algorithm>
#include <cmath>

// -----------------------------------------------------------------------------
// A bank of ringing modes: up to 64 two-pole resonators as rotations (the coupled
// form: a point turned by the mode's frequency and drawn in by its decay), laid
// out as a struct of arrays so the compiler turns the loop over modes into vector
// code (eight modes a step; the two sums are kept in eight lanes for that).
//
// Why rotations: the state's length never grows while c^2 + s^2 <= 1, whatever
// the coefficients do from one sample to the next, so a bank may be retuned under
// a ringing sound - it keeps its energy and its phase, and nothing has to be
// crossfaded or restarted. Coefficients are given as targets at control rate
// (retarget) and moved to in a straight line, sample by sample, only while a
// target differs (the kernel without increments runs otherwise). A straight line
// between two points of a circle passes inside it - a mode retuned far and often
// would be damped by its own retuning - so the kernel lifts the radius back by
// 1 + 2 t (1 - t) sin^2(half the angle moved), which is never more than the dip.
//
// One sample: every mode takes `force` times its input weight, turns, and gives
//   the return value   sum of out[k] * y[k]   (what is heard), and
//   contact            sum of in[k]  * x[k]   (the motion where the force acts:
//                      what a bow or a reed at that place reacts to; force and
//                      contact use the same weights, so a force that opposes the
//                      contact motion can only take energy out).
//
// Modes that have rung out are left out: rest() finds the last block of eight that
// still holds anything and the kernel stops there until wake() (a body struck once
// soon runs only its low partials, a silent one none).
//
// Nothing here allocates; the bank knows nothing of pitch, series or exciters
// (dsp/Resonator.h computes those; the cymbal machine and the vocoder are to
// share this header in phase U2 of .ralph/spec-unify.md). Radii are expected to
// stay a millionth under 1 (the lift is exact to that).
// -----------------------------------------------------------------------------
namespace dsp
{
struct ModeBank
{
    static constexpr int MAX_MODES = 64;
    static constexpr int LANES = 8;
    static constexpr float QUIET = 1.0e-10f;    // rest(): a mode this far under the floor (100 dB) is emptied

    alignas(32) float x[MAX_MODES] {}, y[MAX_MODES] {};
    alignas(32) float c[MAX_MODES] {}, s[MAX_MODES] {}, in[MAX_MODES] {}, out[MAX_MODES] {};
    alignas(32) float dc[MAX_MODES] {}, ds[MAX_MODES] {}, din[MAX_MODES] {}, dout[MAX_MODES] {};
    alignas(32) float bend[MAX_MODES] {};       // sin^2 of half the angle a glide turns a mode by
    int running = 0;        // modes that have coefficients (a multiple of LANES)
    int awake = 0;          // of those, the ones the kernel runs: the rest hold nothing
    int wanted = 0;         // modes that stay once a glide is over
    int glide = 0;          // samples left of a glide
    float glideStep = 1.0f; // 1 / its length

    static int lanes(int modes) { return std::clamp((modes + LANES - 1) / LANES * LANES, 0, MAX_MODES); }

    void reset()
    {
        *this = ModeBank();
    }

    // Silence that keeps the coefficients.
    void clearState()
    {
        std::fill(x, x + MAX_MODES, 0.0f);
        std::fill(y, y + MAX_MODES, 0.0f);
        awake = 0;
    }

    // Every mode runs again (tick does it by itself when the force is not 0; for whoever
    // writes into x or y directly).
    void wake() noexcept { awake = running; }

    // New coefficients for the first `modes` modes (arrays of MAX_MODES; the weights of the
    // rest are taken as 0). steps <= 0: at once. Otherwise the modes that ring reach them in
    // `steps` samples; a mode that leaves fades over that time and is then put away, one that
    // comes starts silent at its own frequency, and one at rest is simply moved.
    void retarget(const float* tc, const float* ts, const float* tin, const float* tout, int modes, int steps)
    {
        const int m = lanes(modes);
        if (steps <= 0)
        {
            for (int i = 0; i < MAX_MODES; ++i)
            {
                const bool on = i < m;
                c[i] = on ? tc[i] : 0.0f;
                s[i] = on ? ts[i] : 0.0f;
                in[i] = on ? tin[i] : 0.0f;
                out[i] = on ? tout[i] : 0.0f;
                if (! on) x[i] = y[i] = 0.0f;
            }
            running = wanted = m;
            awake = std::min(awake, m);
            glide = 0;
            return;
        }
        settle();
        const float inv = 1.0f / (float) steps;
        const int ringing = std::min(awake, m);
        // Modes that hold nothing (those at rest and those that come) are put where they belong
        // at once: there is nothing in them that could step, and the kernel need not run them
        // to move them.
        for (int i = awake; i < std::max(m, running); ++i)
        {
            const bool on = i < m;
            if (on)
            {
                c[i] = tc[i];
                s[i] = ts[i];
            }
            in[i] = on ? tin[i] : 0.0f;
            out[i] = on ? tout[i] : 0.0f;
            x[i] = y[i] = bend[i] = dc[i] = ds[i] = din[i] = dout[i] = 0.0f;
        }
        for (int i = 0; i < ringing; ++i)       // staying
        {
            const float moveC = tc[i] - c[i], moveS = ts[i] - s[i];
            const float from = c[i] * c[i] + s[i] * s[i], radial = from - (tc[i] * tc[i] + ts[i] * ts[i]);
            // The chord's square is (the radii's difference)^2 + 4 r r' sin^2(half the angle); the
            // radii are nearly alike (and nearly 1), so their difference is half that of their squares.
            const float apart = std::max(0.0f, (moveC * moveC + moveS * moveS) * from - 0.25f * radial * radial);
            bend[i] = std::min(1.0f, 0.2499f * apart / std::max(from * from, 1.0e-12f));
            dc[i] = moveC * inv;
            ds[i] = moveS * inv;
            din[i] = (tin[i] - in[i]) * inv;
            dout[i] = (tout[i] - out[i]) * inv;
        }
        for (int i = m; i < awake; ++i)         // leaving
        {
            bend[i] = dc[i] = ds[i] = 0.0f;
            din[i] = -in[i] * inv;
            dout[i] = -out[i] * inv;
        }
        running = std::max(m, running);
        wanted = m;
        glide = steps;
        glideStep = inv;
    }

    // One sample; contact: where the motion at the place of the force is put (nullptr: nobody
    // reacts to it, and it is not summed).
    float tick(float force, float* contact = nullptr) noexcept
    {
        if (force != 0.0f)
            awake = running;
        if (glide > 0)
        {
            // (The modes at rest were moved when the glide began: retarget.)
            const float t = 1.0f - (float) (glide - 1) * glideStep, lift = 2.0f * t * (1.0f - t);
            const float o = contact != nullptr ? kernel<true, true>(awake, force, contact, lift)
                                               : kernel<true, false>(awake, force, nullptr, lift);
            if (--glide == 0)
                settle();
            return o;
        }
        return contact != nullptr ? kernel<false, true>(awake, force, contact) : kernel<false, false>(awake, force, nullptr);
    }

    // The sum of the squared state: what the bank holds.
    float energy() const noexcept
    {
        float total = 0.0f;
        for (int k = 0; k < running; k += LANES)
            total += blockEnergy(k);
        return total;
    }

    // The same, and every block of eight that holds no more than `floor` is emptied; so is, in a
    // block that still holds more, each mode that has fallen QUIET under that floor (a partial
    // that dies long before its neighbours would otherwise dwindle into the smallest numbers,
    // which cost many times a normal one on some processors). The modes above the last block
    // that holds more than `floor` are put to rest: left out until something is put in again.
    float rest(float floor) noexcept
    {
        float total = 0.0f;
        int last = 0;
        for (int k = 0; k < awake; k += LANES)
        {
            float e[LANES];
            for (int j = 0; j < LANES; ++j)
                e[j] = x[k + j] * x[k + j] + y[k + j] * y[k + j];
            const float held = sum(e);
            total += held;
            if (held > floor)
                last = k + LANES;
            const float least = held > floor ? floor * QUIET : held;
            for (int j = 0; j < LANES; ++j)
                if (e[j] <= least)
                    x[k + j] = y[k + j] = 0.0f;
        }
        // (A mode put to rest while coefficients move is not run for the remainder of the
        // glide: it is moved there now.)
        if (glide > 0)
            for (int i = last; i < awake; ++i)
            {
                const float left = (float) glide;
                c[i] += dc[i] * left;
                s[i] += ds[i] * left;
                in[i] += din[i] * left;
                out[i] += dout[i] * left;
                bend[i] = dc[i] = ds[i] = din[i] = dout[i] = 0.0f;
            }
        awake = last;
        return total;
    }

    void scale(float g) noexcept
    {
        for (int i = 0; i < running; ++i)
        {
            x[i] *= g;
            y[i] *= g;
        }
    }

private:
    static float sum(const float* a) noexcept
    {
        return ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
    }

    float blockEnergy(int k) const noexcept
    {
        float e[LANES];
        for (int j = 0; j < LANES; ++j)
            e[j] = x[k + j] * x[k + j] + y[k + j] * y[k + j];
        return sum(e);
    }

    // A glide is over: the modes that left are put away.
    void settle() noexcept
    {
        for (int i = wanted; i < running; ++i)
            x[i] = y[i] = c[i] = s[i] = in[i] = out[i] = 0.0f;
        running = wanted;
        awake = std::min(awake, running);
        glide = 0;
    }

    template <bool Gliding, bool Contact>
    float kernel(int modes, float force, float* contact, float lift = 0.0f) noexcept
    {
        float o[LANES] {}, v[LANES] {};
        for (int k = 0; k < modes; k += LANES)
            for (int j = 0; j < LANES; ++j)
            {
                const int i = k + j;
                float ci, si;
                if (Gliding)
                {
                    c[i] += dc[i];
                    s[i] += ds[i];
                    in[i] += din[i];
                    out[i] += dout[i];
                    const float back = 1.0f + lift * bend[i];
                    ci = c[i] * back;
                    si = s[i] * back;
                }
                else
                {
                    ci = c[i];
                    si = s[i];
                }
                const float pushed = x[i] + in[i] * force;
                const float u = ci * pushed - si * y[i];
                const float w = si * pushed + ci * y[i];
                x[i] = u;
                y[i] = w;
                if (Contact)
                    v[j] += in[i] * u;
                o[j] += out[i] * w;
            }
        if (Contact)
            *contact = sum(v);
        return sum(o);
    }
};
}
