#pragma once

#include "SVFilter.h"
#include "SEMFilter.h"
#include "LadderFilter.h"
#include "DiodeFilter.h"
#include "CombFilter.h"

// The filter families. The first four keep their original values (stored sounds and presets use
// them); the display order used on the pages is separate (see displayOrder).
enum class FilterModel { SVF, Ladder, Diode, Bypass, SEM, Comb };
enum class FilterRouting { Single, Series, Parallel };

// -----------------------------------------------------------------------------
// Two filters (A and B), each any model in any of that model's modes, routed
// single (A only), in series (A into B) or in parallel (mixed by the balance).
//
//   SVF    clean 12 dB state-variable       LP / BP / HP / notch
//   SEM    expander-style 12 dB             LP-notch-HP morph / BP
//   Ladder transistor ladder                LP 24 / 18 / 12 / 6, BP, HP
//   Diode  two-pole with diode feedback     LP / HP / BP   (aggressive when driven)
//   Comb   tuned feedback comb              + / -
// -----------------------------------------------------------------------------
class FilterSlot
{
public:
    static constexpr int NUM_MODELS = 6;

    // Order in which the models are offered to the user, and the conversions.
    static FilterModel modelFromIndex(int index)
    {
        static const FilterModel order[NUM_MODELS] = { FilterModel::SVF, FilterModel::SEM, FilterModel::Ladder,
                                                       FilterModel::Diode, FilterModel::Comb, FilterModel::Bypass };
        return order[std::clamp(index, 0, NUM_MODELS - 1)];
    }

    static int indexFromModel(FilterModel m)
    {
        for (int i = 0; i < NUM_MODELS; ++i)
            if (modelFromIndex(i) == m)
                return i;
        return 0;
    }

    static const char* const* modelNames()
    {
        static const char* const names[NUM_MODELS] = { "SVF", "SEM", "LADR", "DIOD", "COMB", "OFF" };
        return names;
    }

    static int modeCount(FilterModel m)
    {
        switch (m)
        {
            case FilterModel::SVF:    return 4;
            case FilterModel::SEM:    return 2;
            case FilterModel::Ladder: return 6;
            case FilterModel::Diode:  return 3;
            case FilterModel::Comb:   return 2;
            case FilterModel::Bypass: return 1;
        }
        return 1;
    }

    static const char* const* modeNames(FilterModel m)
    {
        static const char* const svf[]    = { "LP", "BP", "HP", "NTCH" };
        static const char* const sem[]    = { "MRPH", "BP" };
        static const char* const ladder[] = { "LP24", "LP18", "LP12", "LP6", "BP", "HP" };
        static const char* const diode[]  = { "LP", "HP", "BP" };
        static const char* const comb[]   = { "POS", "NEG" };
        static const char* const none[]   = { "---" };
        switch (m)
        {
            case FilterModel::SVF:    return svf;
            case FilterModel::SEM:    return sem;
            case FilterModel::Ladder: return ladder;
            case FilterModel::Diode:  return diode;
            case FilterModel::Comb:   return comb;
            case FilterModel::Bypass: return none;
        }
        return none;
    }

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        for (int i = 0; i < 2; ++i)
        {
            svf[i].setSampleRate(sr);
            sem[i].setSampleRate(sr);
            ladder[i].setSampleRate(sr);
            diode[i].setSampleRate(sr);
            comb[i].setSampleRate(sr);
        }
    }

    void setModel(int slot, FilterModel m)
    {
        if (slot < 0 || slot >= 2)
            return;
        model[slot] = m;
        // The newly chosen model picks up the slot's current settings.
        setMode(slot, mode[slot]);
        setCutoffForModel(slot, lastCutoff[slot]);
        setResonanceForModel(slot, resonance[slot]);
        setDriveForModel(slot, drive[slot]);
    }

    FilterModel getModel(int slot) const
    {
        return (slot >= 0 && slot < 2) ? model[slot] : FilterModel::Bypass;
    }

    // Mode within the slot's model (index into modeNames); out-of-range values use the last mode.
    void setMode(int slot, int m)
    {
        if (slot < 0 || slot >= 2)
            return;
        mode[slot] = std::max(0, m);
        const int clamped = std::min(mode[slot], modeCount(model[slot]) - 1);
        switch (model[slot])
        {
            case FilterModel::SVF:
            {
                static const SVFilter::Mode map[] = { SVFilter::Mode::LowPass, SVFilter::Mode::BandPass,
                                                      SVFilter::Mode::HighPass, SVFilter::Mode::Notch };
                svf[slot].setMode(map[clamped]);
                break;
            }
            case FilterModel::SEM:    sem[slot].setBandPass(clamped == 1); break;
            case FilterModel::Ladder: ladder[slot].setMode(static_cast<LadderFilter::Mode>(clamped)); break;
            case FilterModel::Diode:  diode[slot].setMode(static_cast<DiodeFilter::Mode>(clamped)); break;
            case FilterModel::Comb:   comb[slot].setNegative(clamped == 1); break;
            case FilterModel::Bypass: break;
        }
    }

    int getMode(int slot) const
    {
        if (slot < 0 || slot >= 2) return 0;
        return std::min(mode[slot], modeCount(model[slot]) - 1);
    }

    // SEM: low-pass (0) -> notch (0.5) -> high-pass (1).
    void setMorph(float m)
    {
        morph = m;
        for (int i = 0; i < 2; ++i)
            sem[i].setMorph(m);
    }
    float getMorph() const { return morph; }

    FilterModel getModelA() const { return model[0]; }
    FilterModel getModelB() const { return model[1]; }

    void setRouting(FilterRouting r) { routing = r; }
    FilterRouting getRouting() const { return routing; }

    // Parallel routing: 0 = filter A only, 1 = filter B only.
    void setBalance(float b) { balance = std::clamp(b, 0.0f, 1.0f); }
    float getBalance() const { return balance; }

    void setCutoff(float hz)
    {
        for (int i = 0; i < 2; ++i)
            setCutoffForModel(i, hz);
    }

    void setCutoff(int slot, float hz)
    {
        if (slot < 0 || slot >= 2) return;
        setCutoffForModel(slot, hz);
    }

    // Resonance is 0..1 for every model (each maps it to its own useful range). For backward
    // compatibility a value above 1 is taken as a literal Q by the SVF.
    void setResonance(float q)
    {
        for (int i = 0; i < 2; ++i)
            setResonanceForModel(i, q);
    }

    void setResonance(int slot, float q)
    {
        if (slot < 0 || slot >= 2) return;
        setResonanceForModel(slot, q);
    }

    void setDrive(float d)
    {
        for (int i = 0; i < 2; ++i)
            setDriveForModel(i, d);
    }

    void setDrive(int slot, float d)
    {
        if (slot < 0 || slot >= 2) return;
        setDriveForModel(slot, d);
    }

    float process(float sample)
    {
        switch (routing)
        {
            case FilterRouting::Single:
                return processSlot(0, sample);

            case FilterRouting::Series:
                return processSlot(1, processSlot(0, sample));

            case FilterRouting::Parallel:
            {
                float a = processSlot(0, sample);
                float b = processSlot(1, sample);
                return a * (1.0f - balance) + b * balance;
            }
        }
        return sample;
    }

    void reset()
    {
        for (int i = 0; i < 2; ++i)
        {
            svf[i].reset();
            sem[i].reset();
            ladder[i].reset();
            diode[i].reset();
            comb[i].reset();
        }
    }

private:
    float processSlot(int slot, float sample)
    {
        switch (model[slot])
        {
            case FilterModel::SVF:    return svf[slot].process(drive[slot] > 0.0f ? driven(sample, drive[slot]) : sample);
            case FilterModel::SEM:    return sem[slot].process(sample);
            case FilterModel::Ladder: return ladder[slot].process(sample);
            case FilterModel::Diode:  return diode[slot].process(sample);
            case FilterModel::Comb:   return comb[slot].process(sample);
            case FilterModel::Bypass: return sample;
        }
        return sample;
    }

    // DRIV on the SVF, which has no non-linearity of its own: the input is pushed into a soft
    // clipper (gain 1..10; the limiter is flat from +-3 on, where the tanh approximation reaches
    // exactly 1), scaled so that a full-level signal keeps its peak. At 0 the filter is untouched.
    static float driven(float x, float amount)
    {
        const float gain = 1.0f + amount * 9.0f;
        return dsp::fastTanh(std::clamp(x * gain, -3.0f, 3.0f)) / dsp::fastTanh(std::min(gain, 3.0f));
    }

    void setCutoffForModel(int slot, float hz)
    {
        lastCutoff[slot] = hz;
        switch (model[slot])
        {
            case FilterModel::SVF:    svf[slot].setCutoff(hz); break;
            case FilterModel::SEM:    sem[slot].setCutoff(hz); break;
            case FilterModel::Ladder: ladder[slot].setCutoff(hz); break;
            case FilterModel::Diode:  diode[slot].setCutoff(hz); break;
            case FilterModel::Comb:   comb[slot].setCutoff(hz); break;
            case FilterModel::Bypass: break;
        }
    }

    void setResonanceForModel(int slot, float q)
    {
        resonance[slot] = q;
        switch (model[slot])
        {
            // The SVF wants a Q: 0..1 becomes 0.5 .. 15, larger values are a literal Q.
            case FilterModel::SVF:    svf[slot].setResonance(q <= 1.0f ? 0.5f + q * q * 14.5f : q); break;
            case FilterModel::SEM:    sem[slot].setResonance(std::min(q, 1.0f)); break;
            case FilterModel::Ladder: ladder[slot].setResonance(q); break;
            case FilterModel::Diode:  diode[slot].setResonance(q); break;
            case FilterModel::Comb:   comb[slot].setResonance(std::min(q, 1.0f)); break;
            case FilterModel::Bypass: break;
        }
    }

    void setDriveForModel(int slot, float d)
    {
        drive[slot] = d;
        switch (model[slot])
        {
            case FilterModel::SEM:    sem[slot].setDrive(d); break;
            case FilterModel::Ladder: ladder[slot].setSaturation(d); break;
            case FilterModel::Diode:  diode[slot].setDrive(d); break;
            case FilterModel::Comb:   comb[slot].setDamping(d); break;   // drive darkens the comb's feedback
            default: break;                                              // the SVF: processSlot drives its input
        }
    }

    double sampleRate = 44100.0;
    float morph = 0.0f;
    float balance = 0.5f;
    FilterModel model[2] = { FilterModel::SVF, FilterModel::Bypass };
    int mode[2] = { 0, 0 };
    float lastCutoff[2] = { 1000.0f, 1000.0f };
    float resonance[2] = { 0.0f, 0.0f };
    float drive[2] = { 0.0f, 0.0f };
    FilterRouting routing = FilterRouting::Single;

    SVFilter svf[2];
    SEMFilter sem[2];
    LadderFilter ladder[2];
    DiodeFilter diode[2];
    CombFilter comb[2];
};
