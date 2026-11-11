#pragma once

#include "../controller/DisplayRenderer.h"
#include "../dsp/Envelope.h"
#include "../engine/ParamSpec.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
// The envelope picture: the shape of an ADSR setting, drawn from the real
// Envelope class (so CURV bends the picture exactly as it bends the sound).
//
// The time axis is not linear: an envelope can run from a millisecond to ten
// seconds per stage, so each stage gets a width that grows with the logarithm of
// its time (1 ms = 1 column, 10 ms = 5, 100 ms = 14, 1 s = 24, 10 s = 34 of 128).
// A longer time is always a wider stage, and short times stay visible. The held
// part (sustain) has a fixed width; the picture ends where the release does.
// Dotted lines mark where attack, decay and the held part end.
// -----------------------------------------------------------------------------
namespace EnvelopeCurve
{
    static constexpr int STAGE_MAX_COLUMNS = 34;
    static constexpr int HOLD_COLUMNS = 22;

    struct Settings
    {
        float attack = 0.01f, decay = 0.1f, sustain = 0.7f, release = 0.3f;   // seconds, 0..1
        float curve = 0.0f;                                                    // -1..1, all stages (CURV)
        // Each stage's own curve (StageCurveParam; Shift + turn on ATK / DEC / REL), added to `curve`.
        float attackCurve = 0.0f, decayCurve = 0.0f, releaseCurve = 0.0f;

        // What a stage's curve comes to, as the engines work it out.
        float stageCurve(float own) const { return std::clamp(curve + own, -1.0f, 1.0f); }

        bool operator== (const Settings& o) const
        {
            return attack == o.attack && decay == o.decay && sustain == o.sustain && release == o.release && curve == o.curve
                && attackCurve == o.attackCurve && decayCurve == o.decayCurve && releaseCurve == o.releaseCurve;
        }
    };

    // Columns a stage of `seconds` takes.
    inline int stageColumns(float seconds)
    {
        const float t = std::clamp(seconds, 0.0f, 10.0f);
        const float n = std::log(1.0f + t / 0.005f) / std::log(1.0f + 10.0f / 0.005f);
        return std::clamp((int) std::lround(n * (float) STAGE_MAX_COLUMNS), 1, STAGE_MAX_COLUMNS);
    }

    struct Shape
    {
        std::vector<float> heights;                 // 0..1 per column, from the left; ends with the release
        int attackEnd = 0, decayEnd = 0, holdEnd = 0;   // column of the last sample of each part
    };

    inline Shape shape(const Settings& s)
    {
        const int nA = stageColumns(s.attack), nD = stageColumns(s.decay), nR = stageColumns(s.release);

        // One column per sample: with a "sample rate" of 1 and stage times given in columns the real
        // envelope walks through each stage in exactly that many steps.
        Envelope env;
        env.setSampleRate(1.0);
        Envelope::Params p;
        p.attack = (float) nA;
        p.decay = (float) nD;
        p.sustain = std::clamp(s.sustain, 0.0f, 1.0f);
        p.release = (float) nR;
        p.attackCurve = s.stageCurve(s.attackCurve);
        p.decayCurve = s.stageCurve(s.decayCurve);
        p.releaseCurve = s.stageCurve(s.releaseCurve);
        env.setParams(p);
        env.reset();

        Shape out;
        out.heights.reserve((size_t) (1 + nA + nD + HOLD_COLUMNS + nR));
        out.heights.push_back(0.0f);
        env.noteOn();
        for (int i = 0; i < nA; ++i) out.heights.push_back(env.process());
        out.attackEnd = (int) out.heights.size() - 1;
        for (int i = 0; i < nD; ++i) out.heights.push_back(env.process());
        out.decayEnd = (int) out.heights.size() - 1;
        for (int i = 0; i < HOLD_COLUMNS; ++i) out.heights.push_back(env.process());
        out.holdEnd = (int) out.heights.size() - 1;
        env.noteOff();
        for (int i = 0; i < nR; ++i) out.heights.push_back(env.process());
        for (auto& h : out.heights)
            h = std::isfinite(h) ? std::clamp(h, 0.0f, 1.0f) : 0.0f;
        return out;
    }

    // ---- Which cells of a page are an envelope -------------------------------------------------
    // By the shared vocabulary: a page with ATK, DEC, SUS and REL (times in seconds) has an envelope;
    // CURV on the same page bends it.
    struct Binding
    {
        bool valid = false;
        ParamSpec atk, dec, sus, rel, curv;

        bool isCell(int id) const
        {
            return valid && (id == atk.id || id == dec.id || id == sus.id || id == rel.id || (curv.isValid() && id == curv.id));
        }
    };

    inline Binding bind(const ParamPage& page)
    {
        Binding b;
        auto take = [](ParamSpec& into, const ParamSpec& p, const char* label)
        {
            if (!into.isValid() && std::strcmp(p.label, label) == 0)
                into = p;
        };
        for (auto& p : page.params)
        {
            if (!p.isValid()) continue;
            take(b.atk, p, "ATK");  take(b.dec, p, "DEC");  take(b.sus, p, "SUS");
            take(b.rel, p, "REL");  take(b.curv, p, "CURV");
        }
        b.valid = b.atk.isValid() && b.dec.isValid() && b.sus.isValid() && b.rel.isValid()
               && std::strcmp(b.atk.unit, "s") == 0;
        return b;
    }

    // get(spec) returns the cell's current value.
    template <typename GetFn>
    inline Settings read(const Binding& b, GetFn get)
    {
        Settings s;
        if (!b.valid) return s;
        s.attack = get(b.atk);
        s.decay = get(b.dec);
        s.sustain = get(b.sus);
        s.release = get(b.rel);
        s.curve = b.curv.isValid() ? get(b.curv) : 0.0f;
        return s;
    }

    // The same with the stages' own curves: stageCurve(spec) returns the curve amount that
    // belongs to the ATK, DEC or REL cell `spec` (0 when it has none).
    template <typename GetFn, typename StageCurveFn>
    inline Settings read(const Binding& b, GetFn get, StageCurveFn stageCurve)
    {
        Settings s = read(b, get);
        if (!b.valid) return s;
        s.attackCurve = stageCurve(b.atk);
        s.decayCurve = stageCurve(b.dec);
        s.releaseCurve = stageCurve(b.rel);
        return s;
    }

    // ---- Drawing -------------------------------------------------------------------------------
    // The shape in the rectangle (x, y, w, h): cleared first, a dotted floor, dotted lines where
    // attack, decay and the held part end, and the envelope joined from column to column.
    inline void draw(DisplayRenderer& display, int x, int y, int w, int h, const Shape& s)
    {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                display.setPixel(xx, yy, false);
        if (s.heights.empty() || h < 3 || w < 2)
            return;

        // One row is kept free at the top so the peak never touches what is above the picture.
        const int top = y + 1, floorY = y + h - 1, span = floorY - top;
        const int n = std::min((int) s.heights.size(), w);

        for (int xx = x; xx < x + w; xx += 4)
            display.setPixel(xx, floorY, true);
        for (int mark : { s.attackEnd, s.decayEnd, s.holdEnd })
            if (mark < n)
                for (int yy = top; yy < floorY; yy += 3)
                    display.setPixel(x + mark, yy, true);

        int prev = -1;
        for (int i = 0; i < n; ++i)
        {
            const int py = floorY - (int) std::lround(s.heights[(size_t) i] * (float) span);
            const int from = prev < 0 ? py : std::min(prev, py), to = prev < 0 ? py : std::max(prev, py);
            for (int yy = from; yy <= to; ++yy)
                display.setPixel(x + i, yy, true);
            prev = py;
        }
    }
}
