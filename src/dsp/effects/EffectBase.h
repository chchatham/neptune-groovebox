#pragma once

#include <cmath>
#include <algorithm>
#include <string>
#include "../../engine/ParamSpec.h"
#include "../TempoDivisions.h"

// Tempo divisions an effect's time or rate can be locked to, shortest first: the one list of
// divisions (dsp/TempoDivisions.h) in its own order. Stored by index.
namespace FxSync
{
    static constexpr int NUM_DIVISIONS = tempo::COUNT;
    static constexpr int DEFAULT_DIVISION = tempo::QUARTER;     // 1/4
    inline const char* const* divisionLabels() { return tempo::labels(); }
    // Length of one division in beats (quarter notes); a bar is four beats.
    inline double divisionBeats(int index) { return tempo::beats(index); }
}

class EffectBase
{
public:
    virtual ~EffectBase() = default;

    void setSampleRate(double sr) { prepare(sr, 0); }

    virtual void prepare(double sr, int /*maxBlockSize*/)
    {
        sampleRate = sr;
    }

    virtual void reset() = 0;

    virtual void process(float& left, float& right) = 0;

    virtual void processBlock(float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
            process(left[i], right[i]);
    }

    void processStereo(float* left, float* right, int numSamples)
    {
        processBlock(left, right, numSamples);
    }

    // Switched on or off. In an EffectChain the effect is faded in or out over a few milliseconds
    // (it goes on running until it has faded: `enabled`, which the effects themselves read, is
    // then the chain's to set, from the audio thread).
    void setEnabled(bool e) { wanted = e; if (! chained) enabled = e; }
    bool isEnabled() const { return wanted; }
    void setChained() { chained = true; enabled = wanted; }
    void hold(bool running) { enabled = running; }      // the chain, audio thread

    // (PERF's throw takes the dry share back out of an effect's output: it relies on the mix being
    // applied as dry (1 - mix) + wet mix. An effect of cells, CellEffect.h, goes to a new mix in a
    // straight line of up to 4 ms: a mix moved slowly is followed within a millisecond, a mix
    // thrown is that long on its way, and until it is there the share taken out is not exact.)
    void setMix(float m) { if (std::isfinite(m)) mix = std::clamp(m, 0.0f, 1.0f); }       // (what is not a number is not taken)
    float getMix() const { return mix; }

    bool isBypassed() const { return !wanted; }

    // The one dry / wet mix of a chain (EffectChain.h). An effect that can give its wet sound alone
    // says so (setWetOnly: an effect of cells does, CellEffect.h); in a chain it is then run at
    // mix 1 and the chain mixes, going to wetWanted() in a straight line of 4 ms at most. An
    // effect that does not say so mixes by itself, as it always did (the pinned DLY, RVB and
    // HALL among them). Outside a chain (a PERF pad, a test) every effect mixes by itself.
    void setWetOnly(bool w) { wetOnly = w; }
    bool givesWetOnly() const { return wetOnly && chained; }
    virtual float wetWanted() const { return mix; }     // the mix asked for, modulation included
    // How many samples the effect's sound is behind what it was given (a limiter's lookahead): the
    // chain mixes it with the dry sound that late, so the two are in step. At most MAX_LATENCY.
    static constexpr int MAX_LATENCY = 1023;
    virtual int latencySamples() const { return 0; }

    // The dry delay the effect asks for, in samples (fractional). The chain delays the dry sound
    // by this much before mixing it with the wet, so a through-zero sweep (the wet tap passing
    // through the dry) cancels at the crossing and the mix is in step. 0 = no delay. The chain
    // glides to the new value when it changes (nothing jumped to). A limiter's lookahead (integer)
    // is returned by latencySamples() and uses the same line.
    virtual float dryDelaySamples() const { return 0.0f; }
    // Whether the effect may ever need the chain to hold a dry delay line for it: an effect whose
    // dryDelaySamples may become non-zero returns true so the chain pre-allocates (no allocation
    // on the audio thread). Effects with latencySamples > 0 also need a line.
    virtual bool needsDryLine() const { return latencySamples() > 0; }

    // How much of the effect is heard, 0..1: an effect switched off or on under sound is faded
    // out or in over a few milliseconds by its chain (EffectChain), never cut. Audio thread.
    float presence = -1.0f;     // below 0: not heard yet (it starts as it is set)

    virtual void setParameter(int /*id*/, float /*value*/) {}
    virtual float getParameter(int /*id*/) const { return 0.0f; }

    // Description of parameter 0..3 as shown on the FX page (label, native range, kind). The spec's
    // id is the index itself; values passed to setParameter / returned by getParameter are in the
    // native range.
    virtual ParamSpec getParamSpec(int /*index*/) const { return {}; }

    // Cells beyond the four: an effect with more to set describes up to MAX_EXTRAS further cells
    // (index 0..3), shown on the second page of its slot (TRK FX "FX2", PERF "MORE"). Same rules as
    // the four: native range, set and read back truly. They are not moved by tempo sync, nor by a
    // PERF pad's press or pressure.
    static constexpr int MAX_EXTRAS = 4;
    virtual ParamSpec getExtraSpec(int /*index*/) const { return {}; }
    virtual void setExtra(int /*index*/, float /*value*/) {}
    virtual float getExtra(int /*index*/) const { return 0.0f; }

    // What a cell shows when its number alone does not say it (TALK's VOWL: the vowel's name
    // before the number): cell 0..3 is one of the four, 4..7 one of the extras, `value` what the
    // cell shows. Empty (every other cell of every effect): the cell's spec formats it.
    virtual juce::String cellText(int /*cell*/, float /*value*/) const { return {}; }

    // Modulation from outside (a router, an LFO): cell 0..3 is one of the four, 4..7 one of the
    // extras, 8 the slot's WET; `offset` is a share of the cell's range, -1..1, added along the
    // cell's own curve to what the cell is set to, until the next call (0 = none). The cell itself
    // reads as it was set. Audio thread, once a block at most; an effect that takes it glides to
    // the moved value. An effect that does not (it is not a CellEffect, CellEffect.h), ignores it.
    static constexpr int WET_CELL = 4 + MAX_EXTRAS;
    virtual void modulateCell(int /*cell*/, float /*offset*/) {}
    // Whether modulateCell moves that cell (a display's question: what a destination is called).
    virtual bool takesModulation(int /*cell*/) const { return false; }

    // The custom LFO shape of the track the effect is on (LFO::USER_POINTS levels; the owner keeps
    // them alive and may change them; null = the rising staircase): what the shapes USR and USRS
    // of an effect's LFO play. An effect without an LFO ignores it.
    virtual void setUserShape(const float* /*levels*/) {}

    // The track's input (TrackInParam: another track's sound). An effect that listens to something
    // other than itself (keySource() != KEY_SELF) is given the input before each process /
    // processBlock call: `frames` samples, valid only for that call; null = the track has no input.
    // `right` may equal `left`.
    virtual void setInput(const float* left, const float* right, int frames) { juce::ignoreUnused(left, right, frames); }

    // What the effect listens to beside its own sound: nothing (KEY_SELF), the track's input
    // (KEY_IN), or the sound of a track, 0-based (a keyed effect's KEY / SRC cell, Detector.h).
    // Whoever runs the effect hands the sound it names through setInput before each block (KeyFeed
    // in EffectChain.h: the one way; null = there is none, and a keyed effect then keys itself).
    static constexpr int KEY_SELF = -1, KEY_IN = -2;
    virtual int keySource() const { return KEY_SELF; }

    // Derived from keySource(): true when the effect listens to the track's input (KEY_IN).
    // A keyed effect set to a track by number returns false here: it wants that track's tap, not
    // the IN sound.
    bool wantsInput() const { return keySource() == KEY_IN; }

    // The pitch of the note the track is playing, in Hz (0 or less: none is known), for an effect
    // that tunes itself to it (VOC's mode TUNE). It stays until the next call. Audio thread.
    // (The AudioEngine hands an insert the last note its track's engine reported: EngineBase::getLastNoteHz.)
    virtual void setPitch(float /*hz*/) {}

    // ---- Tempo sync ---------------------------------------------------------------------------
    // An effect with a cell that is a time or a rate can lock it to a division of the tempo: a cell
    // marked time-like (ParamSpec::timeLike: any of the eight, the first four looked at first), or
    // one of the four labelled TIME or RATE. Its unit says what the division gives it: "Hz" a
    // rate, "ms" milliseconds, anything else seconds. While synced the division sets that cell and
    // the knob's own value is kept for when sync is switched off. A division too long or too short
    // for the cell's range is halved or doubled until it fits, so it stays in time.
    // The index is 0..3 for one of the four, 4..7 for one of the extras (FX2).
    int getSyncParamIndex() const
    {
        for (int i = 0; i < 4 + MAX_EXTRAS; ++i)
            if (isSyncCell(cellSpecAt(i), i < 4)) return i;
        return -1;
    }
    bool canSync() const { return getSyncParamIndex() >= 0; }

    void setSync(bool on)
    {
        const int idx = getSyncParamIndex();
        if (idx < 0 || on == synced) return;
        if (on) freeValue = cellAt(idx);
        synced = on;
        if (on) applySync(); else setCellAt(idx, freeValue);
    }
    bool isSynced() const { return synced; }
    void setSyncDivision(int division)
    {
        syncDivision = std::clamp(division, 0, FxSync::NUM_DIVISIONS - 1);
        applySync();
    }
    int getSyncDivision() const { return syncDivision; }
    void setTempo(float bpm)
    {
        // (A tempo that is not a number, or an infinite one, is not taken: a division of it has no length.)
        if (! (bpm > 0.0f) || ! std::isfinite(bpm) || bpm == tempoBpm) return;
        tempoBpm = bpm;
        applySync();
    }

    // What the knob is set to: while synced, the stored free value rather than the division's.
    void setUserParameter(int index, float value)
    {
        if (synced && index == getSyncParamIndex()) { freeValue = value; return; }
        setParameter(index, value);
    }
    float getUserParameter(int index) const
    {
        return (synced && index == getSyncParamIndex()) ? freeValue : getParameter(index);
    }
    // The same for an extra cell (0..3) that is the synced one.
    void setUserExtra(int index, float value)
    {
        if (synced && 4 + index == getSyncParamIndex()) { freeValue = value; return; }
        setExtra(index, value);
    }
    float getUserExtra(int index) const
    {
        return (synced && 4 + index == getSyncParamIndex()) ? freeValue : getExtra(index);
    }

protected:
    double sampleRate = 44100.0;
    bool enabled = true;        // running (what process() looks at)
    bool wanted = true;         // switched on
    bool chained = false;
    bool wetOnly = false;
    float mix = 1.0f;

    static float lerp(float a, float b, float t) { return a + t * (b - a); }
    float getTempo() const { return tempoBpm; }

private:
    // The eight cells by one index: 0..3 the four, 4..7 the extras.
    ParamSpec cellSpecAt(int i) const { return i < 4 ? getParamSpec(i) : getExtraSpec(i - 4); }
    float cellAt(int i) const { return i < 4 ? getParameter(i) : getExtra(i - 4); }
    void setCellAt(int i, float v) { if (i < 4) setParameter(i, v); else setExtra(i - 4, v); }

    static bool unitIs(const ParamSpec& spec, const char* unit) { return spec.unit != nullptr && std::string(spec.unit) == unit; }
    static bool isSyncCell(const ParamSpec& spec, bool amongTheFour)
    {
        if (! spec.isValid() || spec.label == nullptr || spec.isDiscrete()) return false;
        if (spec.timeLike) return true;
        const std::string label(spec.label);
        return amongTheFour && (label == "TIME" || label == "RATE");
    }
    // A rate (the division gives cycles a second) or a time (it gives the cell's own unit).
    static bool isRateCell(const ParamSpec& spec)
    {
        return unitIs(spec, "Hz") || (! spec.timeLike && std::string(spec.label) == "RATE");
    }

    void applySync()
    {
        const int idx = getSyncParamIndex();
        if (! synced || idx < 0) return;
        const ParamSpec spec = cellSpecAt(idx);
        const double seconds = FxSync::divisionBeats(syncDivision) * 60.0 / (double) tempoBpm;
        const bool isTime = ! isRateCell(spec);
        const double perSecond = unitIs(spec, "ms") ? 1000.0 : 1.0;       // the cell's unit
        double v = isTime ? seconds * perSecond : 1.0 / seconds;
        if (! (v > 0.0) || ! std::isfinite(v)) return;      // (nothing to halve or double into the range)
        const double lo = std::max(1.0e-4 * perSecond, (double) std::max(spec.minVal, isTime ? 0.001f * (float) perSecond : 0.01f)), hi = (double) spec.maxVal;
        while (v > hi && v * 0.5 >= lo) v *= 0.5;
        while (v < lo && v * 2.0 <= hi) v *= 2.0;
        setCellAt(idx, (float) std::clamp(v, (double) spec.minVal, hi));
    }

    bool synced = false;
    int syncDivision = FxSync::DEFAULT_DIVISION;
    float tempoBpm = 120.0f;
    float freeValue = 0.0f;
};
