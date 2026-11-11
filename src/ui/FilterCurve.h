#pragma once

#include "../controller/DisplayRenderer.h"
#include "../dsp/filters/FilterSlot.h"
#include "../engine/ParamSpec.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
// The filter picture: the magnitude response of a FilterSlot setting over
// 20 Hz - 20 kHz, measured from the real filter classes.
//
// How: a FilterSlot is set up exactly as the voice sets its own (models, modes,
// cutoffs, resonances, morph, routing, balance), fed a small impulse, and the
// response is analysed at one frequency per column (Goertzel). The impulse is
// small so the saturating models are measured in their linear range; drive is
// left at zero (it changes level and tone, not the shape being shown).
//
// One measurement costs 0.1 - 1 ms in an optimised build (the 8192-sample response
// of the real filter is most of it: the ladder, which runs twice per sample, is the
// dearest; the analysis is done sixteen columns at a time). Screens keep a Cache
// and only measure again when a setting changes. Never call this on the audio thread.
// -----------------------------------------------------------------------------
namespace FilterCurve
{
    // What the picture has cost so far (for tests and timing; counted per measurement, not per sample).
    struct Work
    {
        std::atomic<long long> measurements { 0 };      // calls of measureDb
        std::atomic<long long> filterSamples { 0 };     // samples run through a FilterSlot
        std::atomic<long long> analysisSteps { 0 };     // samples x columns analysed
    };
    inline Work& work() { static Work w; return w; }

    static constexpr float MIN_HZ = 20.0f;
    static constexpr float MAX_HZ = 20000.0f;
    static constexpr float TOP_DB = 24.0f;          // height 1
    static constexpr float FLOOR_DB = -48.0f;       // height 0
    static constexpr int MAX_COLUMNS = 128;

    struct Settings
    {
        FilterModel model[2] = { FilterModel::SVF, FilterModel::Bypass };
        int mode[2] = { 0, 0 };
        float cutoffHz[2] = { 1000.0f, 1000.0f };
        float resonance[2] = { 0.0f, 0.0f };
        float morph = 0.0f;                         // SEM: low-pass -> notch -> high-pass
        FilterRouting routing = FilterRouting::Single;
        float balance = 0.5f;

        bool operator== (const Settings& o) const
        {
            return model[0] == o.model[0] && model[1] == o.model[1] && mode[0] == o.mode[0] && mode[1] == o.mode[1]
                && cutoffHz[0] == o.cutoffHz[0] && cutoffHz[1] == o.cutoffHz[1]
                && resonance[0] == o.resonance[0] && resonance[1] == o.resonance[1]
                && morph == o.morph && routing == o.routing && balance == o.balance;
        }
        bool operator!= (const Settings& o) const { return !(*this == o); }
    };

    inline Settings single(FilterModel model, int mode, float cutoffHz, float resonance, float morph = 0.0f)
    {
        Settings s;
        s.model[0] = model;
        s.mode[0] = mode;
        s.cutoffHz[0] = cutoffHz;
        s.resonance[0] = resonance;
        s.morph = morph;
        return s;
    }

    // Column <-> frequency, logarithmic: column 0 = 20 Hz, the last column = 20 kHz.
    inline float columnFrequency(int column, int columns)
    {
        if (columns <= 1) return MIN_HZ;
        return MIN_HZ * std::pow(MAX_HZ / MIN_HZ, (float) column / (float) (columns - 1));
    }

    inline int frequencyColumn(float hz, int columns)
    {
        const float n = std::log(std::clamp(hz, MIN_HZ, MAX_HZ) / MIN_HZ) / std::log(MAX_HZ / MIN_HZ);
        return std::clamp((int) std::lround(n * (float) (columns - 1)), 0, std::max(0, columns - 1));
    }

    inline float heightFromDb(float db)
    {
        return std::clamp((db - FLOOR_DB) / (TOP_DB - FLOOR_DB), 0.0f, 1.0f);
    }

    // Gain in dB at each column's frequency, measured from a FilterSlot set up like `s`.
    inline std::vector<float> measureDb(const Settings& s, int columns)
    {
        columns = std::clamp(columns, 1, MAX_COLUMNS);
        std::vector<float> db((size_t) columns, 0.0f);

        static constexpr double SAMPLE_RATE = 44100.0;
        static constexpr int LENGTH = 8192;
        static constexpr float IMPULSE = 0.05f;

        FilterSlot slot;
        slot.setSampleRate(SAMPLE_RATE);
        for (int i = 0; i < 2; ++i)
        {
            slot.setModel(i, s.model[i]);
            slot.setMode(i, s.mode[i]);
            slot.setCutoff(i, std::clamp(s.cutoffHz[i], MIN_HZ, MAX_HZ));
            slot.setResonance(i, std::clamp(s.resonance[i], 0.0f, 1.0f));
            slot.setDrive(i, 0.0f);
        }
        slot.setMorph(std::clamp(s.morph, 0.0f, 1.0f));
        slot.setRouting(s.routing);
        slot.setBalance(s.balance);
        slot.reset();

        // Impulse response, faded out towards the end so a filter that is still ringing there does
        // not smear across the picture. (The fade is the same for every measurement: made once.)
        static const std::array<float, LENGTH> fadeTable = []
        {
            std::array<float, LENGTH> t {};
            for (int n = 0; n < LENGTH; ++n)
                t[(size_t) n] = 0.5f * (1.0f + std::cos(3.14159265f * (float) n / (float) LENGTH));
            return t;
        }();

        std::vector<float> h((size_t) LENGTH);
        for (int n = 0; n < LENGTH; ++n)
        {
            const float out = slot.process(n == 0 ? IMPULSE : 0.0f);
            h[(size_t) n] = (std::isfinite(out) ? out : 0.0f) * fadeTable[(size_t) n];
        }

        // The response dies away long before the end for most settings: stop where it has.
        int used = LENGTH;
        while (used > 64 && std::abs(h[(size_t) (used - 1)]) < 1.0e-9f)
            --used;

        auto& counted = work();
        counted.measurements.fetch_add(1, std::memory_order_relaxed);
        counted.filterSamples.fetch_add(LENGTH, std::memory_order_relaxed);
        counted.analysisSteps.fetch_add((long long) used * columns, std::memory_order_relaxed);

        // Goertzel at each column's frequency, GROUP columns per pass over the response. One column
        // at a time each step waits for the one before it (128 passes of up to 8192 dependent
        // steps, about 2 ms); side by side the columns are independent, so the processor works on
        // all of a group at once. The arithmetic per column is unchanged.
        static constexpr int GROUP = 16;
        for (int first = 0; first < columns; first += GROUP)
        {
            const int count = std::min(GROUP, columns - first);
            double coeff[GROUP] = {}, s1[GROUP] = {}, s2[GROUP] = {};
            for (int k = 0; k < count; ++k)
                coeff[k] = 2.0 * std::cos(2.0 * 3.14159265358979 * (double) columnFrequency(first + k, columns) / SAMPLE_RATE);

            for (int n = 0; n < used; ++n)
            {
                const double x = (double) h[(size_t) n];
                for (int k = 0; k < GROUP; ++k)
                {
                    const double s0 = x + coeff[k] * s1[k] - s2[k];
                    s2[k] = s1[k];
                    s1[k] = s0;
                }
            }

            for (int k = 0; k < count; ++k)
            {
                const double power = s1[k] * s1[k] + s2[k] * s2[k] - coeff[k] * s1[k] * s2[k];
                const double mag = std::sqrt(std::max(0.0, power)) / (double) IMPULSE;
                db[(size_t) (first + k)] = (float) (20.0 * std::log10(std::max(mag, 1.0e-6)));
            }
        }
        return db;
    }

    // The response as 0..1 heights (FLOOR_DB .. TOP_DB), one per column.
    inline std::vector<float> response(const Settings& s, int columns)
    {
        auto v = measureDb(s, columns);
        for (auto& x : v)
            x = heightFromDb(x);
        return v;
    }

    inline std::vector<float> response(FilterModel model, int mode, float cutoffHz, float resonance, float morph, int columns)
    {
        return response(single(model, mode, cutoffHz, resonance, morph), columns);
    }

    // Keeps the last measurement; measures again only when the settings or the width change.
    class Cache
    {
    public:
        const std::vector<float>& get(const Settings& s, int columns)
        {
            if (!valid || s != settings || columns != (int) heights.size())
            {
                heights = response(s, columns);
                settings = s;
                valid = true;
                ++measurements;
            }
            return heights;
        }
        int getMeasurementCount() const { return measurements; }

    private:
        bool valid = false;
        Settings settings;
        std::vector<float> heights;
        int measurements = 0;
    };

    // ---- Where an engine keeps its filter ------------------------------------------------------
    // Found from the engine's own pages: the cells of its "FLTR" section, by the shared vocabulary
    // (CUT, RESO, TYPE, MODE, MORP; second filter TYPB, MODB, OFSB, RESB, ROUT, BAL). An engine
    // without such a section has no filter to show.
    struct Binding
    {
        bool valid = false;
        ParamSpec cut, reso, type, mode, morph;
        ParamSpec typeB, modeB, offsetB, resoB, rout, bal;
    };

    inline Binding bind(const ParamPages& pages)
    {
        Binding b;
        auto take = [](ParamSpec& into, const ParamSpec& p, const char* label)
        {
            if (!into.isValid() && std::strcmp(p.label, label) == 0)
                into = p;
        };
        for (auto& page : pages)
        {
            if (page.section != "FLTR")
                continue;
            for (auto& p : page.params)
            {
                if (!p.isValid()) continue;
                take(b.cut, p, "CUT");     take(b.reso, p, "RESO");   take(b.type, p, "TYPE");
                take(b.mode, p, "MODE");   take(b.morph, p, "MORP");
                take(b.typeB, p, "TYPB");  take(b.modeB, p, "MODB");  take(b.offsetB, p, "OFSB");
                take(b.resoB, p, "RESB");  take(b.rout, p, "ROUT");   take(b.bal, p, "BAL");
            }
        }
        b.valid = b.cut.isValid() && b.type.isValid();
        return b;
    }

    // A cutoff cell is either in Hz or 0..1 over 20 Hz - 20 kHz (the sampler).
    inline float cutoffHz(const ParamSpec& cut, float value)
    {
        if (juce::String(cut.unit) == "Hz")
            return std::clamp(value, MIN_HZ, MAX_HZ);
        return MIN_HZ * std::pow(MAX_HZ / MIN_HZ, std::clamp(cut.toNorm(value), 0.0f, 1.0f));
    }

    // get(id) returns the parameter's current value.
    template <typename GetFn>
    inline Settings read(const Binding& b, GetFn get)
    {
        Settings s;
        if (!b.valid)
            return s;
        s.model[0] = FilterSlot::modelFromIndex((int) std::lround(get(b.type.id)));
        s.mode[0] = b.mode.isValid() ? (int) std::lround(get(b.mode.id)) : 0;
        s.cutoffHz[0] = cutoffHz(b.cut, get(b.cut.id));
        s.resonance[0] = b.reso.isValid() ? get(b.reso.id) : 0.0f;
        s.morph = b.morph.isValid() ? get(b.morph.id) : 0.0f;

        if (b.typeB.isValid() && b.rout.isValid())
        {
            s.routing = (FilterRouting) std::clamp((int) std::lround(get(b.rout.id)), 0, 2);
            s.model[1] = FilterSlot::modelFromIndex((int) std::lround(get(b.typeB.id)));
            s.mode[1] = b.modeB.isValid() ? (int) std::lround(get(b.modeB.id)) : 0;
            const float semis = b.offsetB.isValid() ? get(b.offsetB.id) : 0.0f;
            s.cutoffHz[1] = std::clamp(s.cutoffHz[0] * std::exp2(semis / 12.0f), MIN_HZ, MAX_HZ);
            s.resonance[1] = b.resoB.isValid() ? get(b.resoB.id) : s.resonance[0];
            s.balance = b.bal.isValid() ? get(b.bal.id) : 0.5f;
        }
        return s;
    }

    // ---- Drawing -------------------------------------------------------------------------------
    // The curve in the rectangle (x, y, w, h): cleared first, one height per pixel column, joined
    // vertically, with a dotted line at the cutoff. heights.size() must be w.
    inline void draw(DisplayRenderer& display, int x, int y, int w, int h, const std::vector<float>& heights,
                     float cutoffMarkHz = 0.0f)
    {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                display.setPixel(xx, yy, false);
        if (heights.empty() || h < 2)
            return;

        if (cutoffMarkHz > 0.0f)
        {
            const int cx = x + frequencyColumn(cutoffMarkHz, w);
            for (int yy = y; yy < y + h; yy += 3)
                display.setPixel(cx, yy, true);
        }

        int prev = -1;
        for (int i = 0; i < w; ++i)
        {
            const float v = heights[(size_t) std::min(i, (int) heights.size() - 1)];
            const int py = y + (h - 1) - (int) std::lround(v * (float) (h - 1));
            const int from = prev < 0 ? py : std::min(prev, py), to = prev < 0 ? py : std::max(prev, py);
            for (int yy = from; yy <= to; ++yy)
                display.setPixel(x + i, yy, true);
            prev = py;
        }
    }
}
