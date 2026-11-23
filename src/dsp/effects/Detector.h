#pragma once

// The dynamics core (spec-unify.md, phase E3): one level detector, one attack / hold / release
// follower, one sliding minimum (the limiter's lookahead), one gain computer, and the plumbing that
// lets a dynamics effect listen to another sound instead of itself (KEY: SELF / T1..T16 / IN).
//
//   Detector      a stereo key made into one level: optional side-chain high-pass, the two channels
//                 linked, peak or RMS.
//   Follower      attack / hold / release on a value that is driven one way and falls back the
//                 other: an envelope (driven up: `up`) or a gain (driven down: `down`).
//   SlidingMin    the smallest of the last N values at a constant cost per sample.
//   PeakHold      the largest of about the last N values, cheaper and not exact.
//   Onset         when a sound begins: a level out of silence, or a jump over the floor it was at.
//   Trigger       the onsets of a stereo key over a run of samples (Detector + Onset).
//   GainComputer  threshold, ratio and soft knee: the gain a level asks for.
//   Keyed<Fx>     an effect whose detector can be fed from another track or from the track's input.
//   CompressorFx  the one compressor. CMP, DUCK and the master compressor are settings of it.
//
// Who uses what: CMP / DUCK / master compressor = Detector + GainComputer + Follower::down;
// LMT = SlidingMin (the lookahead) + Follower::down (attack 0) on its own ceiling / peak gain;
// TRNS = Detector + PeakHold + four Follower::up; WAH = Detector + Follower::up. CMP, DUCK, TRNS
// and WAH are all Keyed. Every triggered effect LFO (FxLfo.h) and the modulation router = Trigger.
// (TRNS's first stage - the level held over its ripple and let go with a short release - is
// Onset's `fast.up(hold.push(level))`: Onset::level() is that envelope and Onset::floorLevel() the
// floor under it, the two things a transient shaper compares; TRNS itself still keeps its own
// four followers, with its own times.)
// Not on it yet, and why: the vocoder's band followers (Vocoder.h is phase U2's while this is
// written), the vibrato's and the track input's follower in AudioEngine (other owners); all three
// are fxb::EnvelopeFollower, which is Follower::up with other member names.
//
// Two of them sit in the master chain, whose sound is pinned bit for bit (suite "P01 Keyboard"):
// the compressor at 1:1 and the limiter. Follower's recursion `c * y + (1 - c) * target`,
// timeCoeff() and SlidingMin (an exact minimum) are their old arithmetic, moved here unchanged:
// do not reorder those expressions.

#include "CellEffect.h"
#include "FxBlocks.h"
#include "../DSPUtils.h"
#include <array>
#include <cmath>
#include <algorithm>
#include <limits>

namespace dyn
{

// The one-pole coefficient of a time in milliseconds: exp(-1 / (time * rate)). 0 = at once.
inline float timeCoeff(float ms, double sampleRate)
{
    return std::exp(-1.0f / (float) (ms * 0.001 * sampleRate));
}

// ---------------------------------------------------------------------------------------------
struct Follower
{
    float attack = 0.0f, release = 0.0f;    // one-pole coefficients (timeCoeff); 0 = at once
    int hold = 0;                           // samples the value stays after it was last driven
    int left = 0;
    float y = 0.0f;

    void reset(float value) { y = value; left = 0; }

    void setTimes(double sampleRate, float attackMs, float releaseMs)
    {
        attack = attackMs > 0.0f ? timeCoeff(attackMs, sampleRate) : 0.0f;
        release = releaseMs > 0.0f ? timeCoeff(releaseMs, sampleRate) : 0.0f;
    }
    void setHold(double sampleRate, float ms)
    {
        hold = (int) std::clamp((double) ms * 0.001 * sampleRate, 0.0, 1.0e7);
    }

    // An envelope: rises to a higher target with the attack, waits, falls with the release.
    float up(float target)
    {
        if (target >= y)
        {
            left = hold;
            y = attack * y + (1.0f - attack) * target;
        }
        else if (left > 0)
            --left;
        else
            y = fxb::flushDenormal(release * y + (1.0f - release) * target);
        return y;
    }

    // A gain: pulled down to a lower target with the attack, waits, comes back with the release.
    float down(float target)
    {
        if (target < y)
        {
            left = hold;
            y = attack * y + (1.0f - attack) * target;
        }
        else if (left > 0)
            --left;
        else
            y = release * y + (1.0f - release) * target;
        return y;
    }
};

// ---------------------------------------------------------------------------------------------
// The smallest of the last `window` values pushed (the newest included): a monotonic queue
// (Lemire, "Streaming maximum-minimum filter using no more than three comparisons per element",
// 2006). Exact, so it replaces a scan of the window without changing a bit of the result. N is a
// power of two; window <= N - 1. Values that were never pushed count as `fill`.
template <int N>
class SlidingMin
{
public:
    void reset(float fillValue)
    {
        raw.fill(fillValue);
        pos = 0;
        count = 0;
        head = 0;
        windowNow = 0;
    }

    float push(float value, int window)
    {
        window = std::clamp(window, 1, N - 1);
        raw[(size_t) (pos & MASK)] = value;
        if (window != windowNow)
            rebuild(window);                // the window was changed: the queue is made again
        else
            add(value, pos);
        while (pos - index[(size_t) head] >= (unsigned) window)
        {
            head = (head + 1) & MASK;
            --count;
        }
        ++pos;
        return queue[(size_t) head];
    }

private:
    static constexpr unsigned MASK = (unsigned) N - 1u;

    void add(float value, unsigned at)
    {
        while (count > 0 && queue[(size_t) ((head + count - 1) & MASK)] >= value)
            --count;
        const unsigned slot = (head + (unsigned) count) & MASK;
        queue[(size_t) slot] = value;
        index[(size_t) slot] = at;
        ++count;
    }

    void rebuild(int window)
    {
        windowNow = window;
        count = 0;
        head = 0;
        for (int back = window - 1; back >= 0; --back)
            add(raw[(size_t) ((pos - (unsigned) back) & MASK)], pos - (unsigned) back);
    }

    std::array<float, N> raw {};        // the values as pushed (for a change of window)
    std::array<float, N> queue {};      // rising values, the smallest at `head`
    std::array<unsigned, N> index {};   // when each was pushed
    unsigned pos = 0, head = 0;
    int count = 0;
    int windowNow = 0;
};

// The largest of the last `length` to 2 x `length` values, at two comparisons a sample: a level
// held over its own ripple (a rectified wave falls to nothing twice a period). Where the window
// must be exact (the limiter) it is SlidingMin; an envelope does not need that and should not pay
// for it (measured: 5 ns a sample against 0.3).
struct PeakHold
{
    int length = 1, count = 0;
    float now = 0.0f, before = 0.0f;

    void reset() { count = 0; now = before = 0.0f; }

    float push(float value)
    {
        if (--count <= 0)
        {
            count = length;
            before = now;
            now = 0.0f;
        }
        now = std::max(now, value);
        return std::max(now, before);
    }
};

// ---------------------------------------------------------------------------------------------
class Detector
{
public:
    enum Mode { Peak = 0, RMS };

    static constexpr float MAX_LEVEL = 1.0e6f;      // what a key can read at most (+120 dB)
    static constexpr float HP_OFF = 20.0f;          // the side-chain high-pass at or below this: none
    static constexpr float RMS_MS = 10.0f;          // the time the mean square is taken over

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        rmsCoeff = 1.0f - timeCoeff(RMS_MS, sampleRate);
        setHighPass(hpHz);
    }

    void reset()
    {
        hp[0].reset();
        hp[1].reset();
        meanSquare = 0.0f;
    }

    void setMode(Mode m) { mode = m; plainPeak = mode == Peak && ! hpOn; }
    Mode getMode() const { return mode; }

    // A 6 dB / octave high-pass in front of the detector (it never touches the sound): keeps a
    // bass drum or a low note from working the detector. HP_OFF or less = none.
    void setHighPass(float hz)
    {
        hpHz = std::isfinite(hz) ? hz : 0.0f;
        hpOn = hpHz > HP_OFF;
        plainPeak = mode == Peak && ! hpOn;
        if (hpOn)
            for (auto& f : hp) f.setCutoff(sampleRate, hpHz);
    }

    // The level of a stereo key, the two channels linked: the larger of the two (Peak), or the
    // root of their mean square over RMS_MS (RMS). Whatever comes in (not a number, infinite),
    // a level from 0 to MAX_LEVEL comes out.
    float level(float left, float right)
    {
        if (plainPeak)
        {
            const float v = std::max(std::abs(left), std::abs(right));
            return v < MAX_LEVEL ? v : (v >= MAX_LEVEL ? MAX_LEVEL : 0.0f);
        }
        left = sane(left);
        right = sane(right);
        if (hpOn)
        {
            left = hp[0].highPass(left);
            right = hp[1].highPass(right);
        }
        if (mode == Peak)
            return std::max(std::abs(left), std::abs(right));
        meanSquare = fxb::flushDenormal(meanSquare + rmsCoeff * (0.5f * (left * left + right * right) - meanSquare));
        return std::sqrt(meanSquare);
    }

private:
    static float sane(float x)
    {
        if (std::abs(x) < MAX_LEVEL) return x;
        return x != x ? 0.0f : (x < 0.0f ? -MAX_LEVEL : MAX_LEVEL);
    }

    double sampleRate = 44100.0;
    Mode mode = Peak;
    float hpHz = 0.0f;
    bool hpOn = false;
    bool plainPeak = true;          // peak with no high-pass: the short way through level()
    float rmsCoeff = 1.0f;
    float meanSquare = 0.0f;
    std::array<fxb::OnePole, 2> hp;
};

// ---------------------------------------------------------------------------------------------
// The one onset detector: when a sound begins. What an effect's LFO is triggered by (fxm::Onset is
// this, FxLfo.h), what the vibrato's FADE waits for, and what the modulation router hears in the
// track input (mod::Src ONST; an LFO restarted by a hit on another track).
// An onset is a level that comes up out of silence (over -40 dB after
// it was under -50), or jumps by 4 dB over the floor it has just been at: a new note, a drum hit
// (sixteenth notes at 120 BPM are each one, and hits over a held note). The floor is the level
// followed down at once and up slowly. The level is held over its ripple first, so a low note is
// not an onset every period, and notes played legato at one level are one sound.
// Prepared without `jumps`, only a sound out of silence is one: the level is not held and falls
// with `releaseMs` (at 100 ms a loud sound is under -50 dB after half a second of rest: the
// beginning of a phrase, which is what the vibrato's FADE always waited for).
struct Onset
{
    static constexpr float ON = 0.01f, OFF = 0.003f;
    static constexpr float RISE = 1.6f, REARM = 1.2f;

    void prepare(double sr, bool jumps = true, float releaseMs = 30.0f)
    {
        rises = jumps;
        hold.length = jumps ? std::max(1, (int) (0.025 * sr)) : 1;
        fast.setTimes(sr, 1.0f, releaseMs);
        floor.setTimes(sr, 150.0f, 30.0f);
        reset();
    }

    void reset()
    {
        hold.reset();
        fast.reset(0.0f);
        floor.reset(0.0f);
        armed = true;
    }

    // One sample of the key's level (0 or more); true on the sample an onset is heard.
    bool hears(float level)
    {
        if (! rises)
        {
            const float f = fast.up(level);
            if (armed) { if (f > ON) { armed = false; return true; } }
            else if (f < OFF) armed = true;
            return false;
        }
        const float f = fast.up(hold.push(level)), under = floor.up(f);
        if (armed)
        {
            if (f > ON && f > RISE * under)
            {
                armed = false;
                return true;
            }
        }
        else if (f < OFF || f < REARM * under)
            armed = true;
        return false;
    }

    // The envelope it listens to (the level held over its ripple, let go with the release) and
    // the floor that envelope has just been at: an attack is the first standing over the second.
    float level() const { return fast.y; }
    float floorLevel() const { return floor.y; }

    PeakHold hold;
    Follower fast, floor;
    bool armed = true;          // nothing is sounding (or what sounds has settled): the next rise is an onset
    bool rises = true;
};

// The onsets of a key: `n` samples are listened to; true if a sound begins in them. (Who is
// triggered by it is triggered after those samples have been played: an effect's LFO takes it up
// at its next step, whatever the block size; the router's voices in the same sub-block.)
struct Trigger
{
    void prepare(double sr) { detector.prepare(sr); onset.prepare(sr); }
    void reset() { detector.reset(); onset.reset(); }

    bool heard(const float* keyLeft, const float* keyRight, int n)
    {
        bool begun = false;
        for (int i = 0; i < n; ++i)
            begun = onset.hears(detector.level(keyLeft[i], keyRight[i])) || begun;
        return begun;
    }

    Detector detector;
    Onset onset;
};

// ---------------------------------------------------------------------------------------------
// Threshold, ratio and soft knee (a quadratic bend across the knee's width): the gain that a
// level asks for, 0..1. A level below the knee costs one comparison; 1:1 is `idle`.
struct GainComputer
{
    void set(float thresholdDb, float ratioValue, float kneeWidthDb)
    {
        threshold = thresholdDb;
        ratio = ratioValue;
        knee = kneeWidthDb;
        openBelow = dsp::dbToLinear(threshold - knee * 0.5f);
    }

    bool idle() const { return ratio == 1.0f; }

    float reductionDb(float inputDb) const
    {
        const float halfKnee = knee * 0.5f;
        if (knee > 0.01f && inputDb > (threshold - halfKnee) && inputDb < (threshold + halfKnee))
        {
            const float x = inputDb - threshold + halfKnee;
            return (1.0f / ratio - 1.0f) * x * x / (2.0f * knee);
        }
        if (inputDb >= threshold + halfKnee)
            return (inputDb - threshold) * (1.0f / ratio - 1.0f);
        return 0.0f;
    }

    float gain(float level) const
    {
        if (! (level > openBelow)) return 1.0f;
        return dsp::dbToLinear(reductionDb(dsp::linearToDb(level)));
    }

    float threshold = -20.0f, ratio = 4.0f, knee = 3.0f;
    float openBelow = 0.0f;
};

// ---------------------------------------------------------------------------------------------
// The key handed to an effect for one block (EffectBase::setInput).
struct KeyInput
{
    const float* left = nullptr;
    const float* right = nullptr;
    int frames = 0;

    void set(const float* l, const float* r, int n)
    {
        const bool there = l != nullptr && n > 0;
        left = there ? l : nullptr;
        right = there ? (r != nullptr ? r : l) : nullptr;
        frames = there ? n : 0;
    }

    KeyInput take()
    {
        const KeyInput k = *this;
        left = right = nullptr;
        frames = 0;
        return k;
    }
};

// The one choice of what a keyed effect listens to (every KEY cell, and DUCK's SRC, which is the
// same cell under its older name): itself, the sound of track 1..16 (after that track's inserts),
// or IN, the track's input (the source of the track's IN page). The numbers are stored.
static constexpr int KEY_TRACKS = 16;
static constexpr int KEY_CHOICE_IN = KEY_TRACKS + 1;

inline const char* const* keyNames()
{
    static const char* const names[] = { "SELF", "T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8",
                                         "T9", "T10", "T11", "T12", "T13", "T14", "T15", "T16", "IN" };
    return names;
}

inline ParamSpec keySpec(int id, const char* label = "KEY")
{
    return { label, id, 0, (float) KEY_CHOICE_IN, 0, ParamKind::Choice, "", keyNames(), KEY_CHOICE_IN + 1 };
}

// The cell's value as EffectBase::keySource() gives it.
inline int keySourceOf(float value)
{
    const int choice = (int) value;
    if (choice == KEY_CHOICE_IN) return EffectBase::KEY_IN;
    return choice >= 1 && choice <= KEY_TRACKS ? choice - 1 : EffectBase::KEY_SELF;
}

// An effect of cells (CellEffect.h) whose detector listens to the track's own sound (SELF), to
// another track's sound (T1..T16) or to the track's input (IN: the source chosen on the track's IN
// page). The sound that is worked on is always the effect's own. Which of them is the cell made
// by keySpec() (one per effect, found by its list); EffectBase::keySource() reports it, and whoever
// runs the effect hands that sound over before each block through EffectBase::setInput (KeyFeed
// in EffectChain.h: track inserts, the master chain, the sends and the PERF pads alike).
//
// Fx gives `void tick(float& left, float& right, float keyLeft, float keyRight)`: one sample.
// With the key outside and nothing handed over (no source chosen, the track itself, a track that
// is not there) the key is the effect's own signal, as with SELF; keyConnected() says which, for
// a screen's notice. A key shorter than the block keys its own length, the rest keys itself. The
// pointers are used for one call and dropped, so a stale one is never read. Only the detector
// changes its source, and what it drives is smoothed by attack and release: turning KEY under
// sound does not click.
template <class Fx>
class Keyed : public CellEffect
{
public:
    int keySource() const override
    {
        const int c = keyCell();
        return c >= 0 ? keySourceOf(cell(c)) : KEY_SELF;
    }
    void setInput(const float* left, const float* right, int frames) override
    {
        if (keySource() != KEY_SELF) pending.set(left, right, frames); else pending.take();
    }

    // True while the effect is keyed from outside and the last block brought a key.
    bool keyConnected() const { return connected; }

    // One sample, keyed by itself (the way the master section ran before it was a chain; tests).
    void process(float& left, float& right) override
    {
        if (pending.frames != 0 || connected) dropKey();
        if (! enabled) return;
        wet = clock(left, right);
        fx().tick(left, right, left, right);
    }

    void processBlock(float* left, float* right, int numSamples) override
    {
        key = pending.take();
        connected = key.left != nullptr && keySource() != KEY_SELF;
        if (! connected) key = {};
        CellEffect::processBlock(left, right, numSamples);
        key = {};
    }

protected:
    void dropKey() { pending.take(); key = {}; connected = false; }

    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): a tick is one sample)
    void runCore(float* left, float* right, int n, float mixFrom, float mixTo) final
    {
        const float slope = mixTo != mixFrom ? (mixTo - mixFrom) / (float) n : 0.0f;
        wet = mixFrom;
        const int keyed = std::min(n, key.frames);
        for (int i = 0; i < keyed; ++i)
        {
            wet += slope;
            fx().tick(left[i], right[i], key.left[i], key.right[i]);
        }
        if (keyed > 0)
        {
            key.left += keyed;
            key.right += keyed;
            key.frames -= keyed;
        }
        for (int i = std::max(keyed, 0); i < n; ++i)
        {
            wet += slope;
            fx().tick(left[i], right[i], left[i], right[i]);
        }
        wet = mixTo;
    }

    // The gain a tick arrived at, put on the sound. WET below the top leaves the dry sound beside
    // the worked one: dry (1 - mix) + worked * mix.
    void applyGain(float& left, float& right, float gain) const
    {
        if (wet < 1.0f) gain = 1.0f + wet * (gain - 1.0f);
        left *= gain;
        right *= gain;
    }

    float wet = 1.0f;       // the mix for the sample a tick is working on

private:
    Fx& fx() { return static_cast<Fx&> (*this); }
    const Fx& fx() const { return static_cast<const Fx&> (*this); }

    // The cell that holds the key choice: the one made by keySpec(); -1 = the effect has none.
    int keyCell() const
    {
        const ParamSpec* table = cells();
        for (int c = 0; c < CELLS; ++c)
            if (table[c].isValid() && table[c].choices == keyNames())
                return c;
        return -1;
    }

    KeyInput pending, key;
    bool connected = false;
};

// ---------------------------------------------------------------------------------------------
// The one compressor: feed-forward, the gain smoothed by attack / hold / release, soft knee,
// make-up, a side-chain high-pass, peak or RMS detection, an outside key, and WET as parallel
// compression (the dry sound beside the compressed one).
//
// A class over it names its eight cells in order (four on FX, four on FX2) from Setting; the
// cell's label, range and behaviour belong to the setting, the default to the class. The cells
// are CellEffect's: every continuous one moves with modulateCell.
enum class Setting { None = 0, Threshold, Ratio, Attack, Release, Makeup, Knee, SideHp, Hold, Key, Source, Count };

class CompressorFx : public Keyed<CompressorFx>
{
public:
    using DetectionMode = Detector::Mode;

    static constexpr int NUM_CELLS = CELLS;

    // The cell of a setting: label, range, kind. The id and the default are the caller's.
    static ParamSpec settingSpec(Setting s, int id, float defaultValue)
    {
        using K = ParamKind;
        switch (s)
        {
            case Setting::Threshold: return { "THR",  id, -60.0f, 0.0f, defaultValue, K::Linear, "dB" };
            case Setting::Ratio:     return { "RATI", id, 1.0f, 20.0f, defaultValue, K::Log, "x" };
            case Setting::Attack:    return { "ATK",  id, 0.1f, 100.0f, defaultValue, K::Log, "ms" };
            case Setting::Release:   return { "REL",  id, 10.0f, 1000.0f, defaultValue, K::Log, "ms" };
            case Setting::Makeup:    return { "MAKE", id, 0.0f, 30.0f, defaultValue, K::Linear, "dB" };
            case Setting::Knee:      return { "KNEE", id, 0.0f, 12.0f, defaultValue, K::Linear, "dB" };
            case Setting::SideHp:    return { "SCHP", id, Detector::HP_OFF, 2000.0f, defaultValue, K::Log, "Hz" };
            case Setting::Hold:      return { "HOLD", id, 0.0f, 500.0f, defaultValue, K::Linear, "ms" };
            case Setting::Key:       return keySpec(id);
            case Setting::Source:    return keySpec(id, "SRC");         // (DUCK's name for the same cell)
            default:                 return {};
        }
    }

    // ---- by name (the master section, tests) -------------------------------------------------
    void setThreshold(float db)   { set(Setting::Threshold, db); }
    void setRatio(float r)        { set(Setting::Ratio, r); }
    void setAttack(float ms)      { set(Setting::Attack, ms); }
    void setRelease(float ms)     { set(Setting::Release, ms); }
    void setMakeupGain(float db)  { set(Setting::Makeup, db); }
    void setKneeWidth(float db)   { set(Setting::Knee, db); }
    void setSidechainHPF(float hz){ set(Setting::SideHp, hz); }
    void setHold(float ms)        { set(Setting::Hold, ms); }
    float getThreshold() const    { return get(Setting::Threshold); }
    float getRatio() const        { return get(Setting::Ratio); }
    float getAttack() const       { return get(Setting::Attack); }
    float getRelease() const      { return get(Setting::Release); }
    float getMakeupGain() const   { return get(Setting::Makeup); }
    float getKneeWidth() const    { return get(Setting::Knee); }
    float getSidechainHPF() const { return get(Setting::SideHp); }
    float getHold() const         { return get(Setting::Hold); }

    // Peak (the larger channel, sample by sample) or RMS (the level over 10 ms: slower, calmer).
    // No cell of its own yet: the pages are full.
    void setDetectionMode(DetectionMode m) { detector.setMode(m); }
    DetectionMode getDetectionMode() const { return detector.getMode(); }

    // What the compressor takes off right now, in dB (0 or below): a meter.
    float getGainReductionDb() const { return dsp::linearToDb(gain.y); }

    void tick(float& left, float& right, float keyLeft, float keyRight)
    {
        // (1:1 asks for nothing: the detector, the logarithm and the power are not run.)
        const float target = curve.idle() ? 1.0f : curve.gain(detector.level(keyLeft, keyRight));
        applyGain(left, right, gain.down(target) * makeupSmooth.next(makeupLinear));
    }

protected:
    // layout: the settings of cells 0..7 (FX then FX2); defaults: {setting, value} pairs.
    CompressorFx(std::array<Setting, NUM_CELLS> cellSettings, std::initializer_list<std::pair<Setting, float>> defaults)
    {
        rest = { 0.0f, -20.0f, 4.0f, 10.0f, 100.0f, 0.0f, 3.0f, Detector::HP_OFF, 0.0f, 0.0f, 0.0f };
        for (const auto& d : defaults)
            rest[(size_t) d.first] = d.second;
        where.fill(-1);
        for (int c = 0; c < NUM_CELLS; ++c)
        {
            const Setting s = cellSettings[(size_t) c];
            if (s == Setting::None) continue;
            table[(size_t) c] = settingSpec(s, c < 4 ? c : c - 4, rest[(size_t) s]);
            where[(size_t) s] = c;
        }
        gain.reset(1.0f);
        makeupSmooth.snap(1.0f);
        seed();
    }

    const ParamSpec* cells() const override { return table.data(); }

    // A setting by name: its cell where the class has one, otherwise a value beside the cells.
    void set(Setting s, float v)
    {
        if (s == Setting::None || ! std::isfinite(v)) return;
        const int c = where[(size_t) s];
        if (c >= 0) { setCell(c, v); return; }
        const ParamSpec spec = settingSpec(s, 0, 0.0f);
        rest[(size_t) s] = spec.isDiscrete() ? std::round(spec.clamp(v)) : spec.clamp(v);
        touch();
    }
    float get(Setting s) const
    {
        const int c = where[(size_t) s];
        return c >= 0 ? cell(c) : rest[(size_t) s];
    }

    void prepareCore() override
    {
        detector.prepare(sampleRate);
        makeupSmooth.setTime(sampleRate, 20.0f);
    }

    void resetCore() override
    {
        detector.reset();
        gain.reset(1.0f);
        makeupSmooth.snap(makeupLinear);
        dropKey();
    }

    void configure(const float* v) override
    {
        const auto at = [this, v](Setting s) { const int c = where[(size_t) s]; return c >= 0 ? v[c] : rest[(size_t) s]; };
        curve.set(at(Setting::Threshold), at(Setting::Ratio), at(Setting::Knee));
        // (the master compressor's old expressions, through timeCoeff)
        gain.attack = timeCoeff(at(Setting::Attack), sampleRate);
        gain.release = timeCoeff(at(Setting::Release), sampleRate);
        makeupLinear = dsp::dbToLinear(at(Setting::Makeup));
        detector.setHighPass(at(Setting::SideHp));
        gain.setHold(sampleRate, at(Setting::Hold));
    }

private:
    std::array<ParamSpec, NUM_CELLS> table {};                  // the class's cells
    std::array<int, (size_t) Setting::Count> where {};          // a setting's cell, -1 = none
    std::array<float, (size_t) Setting::Count> rest {};         // defaults, then the settings without a cell

    Detector detector;
    GainComputer curve;
    Follower gain;
    fxb::Smoother makeupSmooth;
    float makeupLinear = 1.0f;
};

} // namespace dyn
