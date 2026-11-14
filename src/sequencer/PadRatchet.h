#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

// -----------------------------------------------------------------------------
// Hold-to-ratchet pads (spec 6.8, "Pressure ratchets").
//
// While a pad is held its note is retriggered; how hard the pad is pressed sets
// how fast. Up to 16 pads are tracked independently.
//
// THREADING
//   * padDown / setPressure / padUp / allOff are the control side. They only
//     write scalars: pad events go into a fixed lock-free single-producer queue
//     and pressure into one atomic per pad. They must all come from ONE thread
//     at a time (the MIDI/message thread, or the audio thread itself); two
//     threads calling them concurrently is not supported.
//   * processBlock is the audio side. EVERY callback (onNoteOn / onNoteOff) is
//     fired from inside processBlock, never from padDown / padUp, also when the
//     ratchet is disabled. A pad press therefore sounds at the start of the next
//     processBlock call. processBlock must be called for every audio block,
//     whether or not the ratchet is enabled.
//   * Settings are atomics and may be changed from any thread at any time.
//   * Assign onNoteOn / onNoteOff before the audio thread starts calling
//     processBlock; std::function assignment is not thread-safe.
//   * No allocation, lock, I/O or logging after construction.
//
// TIMING
//   Each pad carries the time to its next hit in (fractional) samples from one
//   block to the next, so the long-run rate is exact for any block size.
//   Callbacks carry no sample offset: a hit that falls inside a block is fired
//   in that block.
//
// ROLL EXTRAS (all neutral by default)
//   swing, snap glide, pressure threshold, latch, humanize and accent: see the
//   setters. Order of the velocity stages for a retrigger: pressure blend and
//   decay -> accent -> humanize -> clamp to kMinVelocity..1. The pad press
//   itself always plays at its own velocity, on time.
// -----------------------------------------------------------------------------
class PadRatchet
{
public:
    static constexpr int kNumPads = 16;

    enum class Mode
    {
        Quantized = 0,   // pressure steps through note divisions, tempo-synced
        Free             // pressure sweeps the rate continuously in Hz
    };
    static constexpr int kNumModes = 2;

    // Note divisions, slowest first. The index is what the settings store.
    enum Division
    {
        Div1_2 = 0,
        Div1_4,
        Div1_4T,
        Div1_8,
        Div1_8T,
        Div1_16,
        Div1_16T,
        Div1_32,
        Div1_32T,
        Div1_64,
        Div1_64T,
        Div1_128,
        kNumDivisions
    };

    // Setting ranges (for the parameter pages).
    static constexpr float kMinGate = 0.05f;
    static constexpr float kMaxGate = 1.0f;
    static constexpr float kMinRateHz = 0.5f;
    static constexpr float kMaxRateHz = 60.0f;
    static constexpr float kMinVelocity = 0.02f;            // retriggers never go below this
    static constexpr double kPressureSmoothingSeconds = 0.006;
    static constexpr float kHysteresis = 0.15f;             // fraction of one division band
    static constexpr float kMaxSwingShift = 0.5f;           // swing 1 delays the off-beat by half an interval (75/25)
    static constexpr float kMaxGlideSeconds = 2.0f;
    static constexpr float kThresholdHysteresis = 0.05f;    // pressure below the threshold before repeats stop
    static constexpr float kHumanizeTime = 0.2f;            // humanize 1 = up to +-20 % of the gap
    static constexpr float kHumanizeVelocity = 0.25f;       // humanize 1 = up to +-25 % velocity
    static constexpr float kAccentOffScale = 0.6f;          // velocity factor of the unaccented repeats
    static constexpr int kMaxAccent = 8;
    static constexpr int kMaxRepeatLimit = 16;

    // ---- display helpers ----------------------------------------------------

    static int getNumDivisions() { return (int) kNumDivisions; }

    // Choice list usable directly as ParamSpec::choices (kNumDivisions entries,
    // each at most 5 characters).
    static const char* const* getDivisionNames()
    {
        static const char* const names[kNumDivisions] = {
            "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64", "1/64T", "1/128"
        };
        return names;
    }

    static const char* getDivisionName(int division)
    {
        return getDivisionNames()[juce::jlimit(0, (int) kNumDivisions - 1, division)];
    }

    // Length of a division in quarter notes (1/16 = 0.25, triplets are 2/3).
    static double getDivisionBeats(int division)
    {
        static const double beats[kNumDivisions] = {
            2.0, 1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 0.0625, 1.0 / 24.0, 0.03125
        };
        return beats[juce::jlimit(0, (int) kNumDivisions - 1, division)];
    }

    static const char* const* getModeNames()
    {
        static const char* const names[kNumModes] = { "QUANT", "FREE" };
        return names;
    }

    static const char* getModeName(Mode m) { return getModeNames()[m == Mode::Free ? 1 : 0]; }

    // ---- construction -------------------------------------------------------

    PadRatchet()
    {
        for (auto& e : eventQueue)
            e.store(0u, std::memory_order_relaxed);
        for (auto& p : rawPressure)
            p.store(0.0f, std::memory_order_relaxed);
        for (auto& r : rollingFlag)
            r.store(false, std::memory_order_relaxed);
        for (auto& l : latchedFlag)
            l.store(false, std::memory_order_relaxed);
    }

    // ---- callbacks (audio thread, from processBlock only) -------------------

    std::function<void(int pad, int note, float velocity)> onNoteOn;
    std::function<void(int pad, int note)> onNoteOff;

    // ---- control side -------------------------------------------------------

    // Pad pressed. velocity 0..1. Plays the note at the start of the next block
    // (the first hit is the press itself) and, when enabled, starts the roll.
    // Resets the pad's pressure to 0 until the next setPressure.
    void padDown(int pad, int note, float velocity)
    {
        if (! isValidPad(pad))
            return;
        rawPressure[(size_t) pad].store(0.0f, std::memory_order_relaxed);
        pushEvent(EventDown, pad, juce::jlimit(0, 127, note), juce::jlimit(0.0f, 1.0f, velocity));
    }

    // Polyphonic aftertouch for a held pad, 0..1. Latest value wins.
    void setPressure(int pad, float pressure)
    {
        if (! isValidPad(pad))
            return;
        rawPressure[(size_t) pad].store(juce::jlimit(0.0f, 1.0f, pressure), std::memory_order_relaxed);
    }

    // The pad's pressure as last set (0 after padDown / padUp). Any thread.
    float getPressure(int pad) const
    {
        return isValidPad(pad) ? rawPressure[(size_t) pad].load(std::memory_order_relaxed) : 0.0f;
    }

    // Pad released: stops the retriggers and ends the note (unless latch keeps
    // the roll going, see setLatch).
    void padUp(int pad)
    {
        if (! isValidPad(pad))
            return;
        rawPressure[(size_t) pad].store(0.0f, std::memory_order_relaxed);
        pushEvent(EventUp, pad, 0, 0.0f);
    }

    // Ends every note and stops every roll.
    void allOff()
    {
        pushEvent(EventAllOff, 0, 0, 0.0f);
    }

    // ---- settings (any thread) ----------------------------------------------

    // Enabled: pads roll while held. Disabled: padDown / padUp pass one note
    // on / off through. Disabling mid-roll ends the rolling notes at once.
    // Default: off.
    void setEnabled(bool e) { enabled.store(e, std::memory_order_relaxed); }
    bool isEnabled() const { return enabled.load(std::memory_order_relaxed); }

    // Default: Quantized.
    void setMode(Mode m) { mode.store(m == Mode::Free ? 1 : 0, std::memory_order_relaxed); }
    Mode getMode() const { return mode.load(std::memory_order_relaxed) == 1 ? Mode::Free : Mode::Quantized; }

    // Quantized mode: division at zero pressure. 0..kNumDivisions-1, default 1/8.
    void setBaseDivision(int d) { baseDivision.store(clampDivision(d), std::memory_order_relaxed); }
    int getBaseDivision() const { return baseDivision.load(std::memory_order_relaxed); }

    // Quantized mode: division at full pressure. 0..kNumDivisions-1, default 1/32.
    // May be slower than the base division (pressure then slows the roll).
    void setMaxDivision(int d) { maxDivision.store(clampDivision(d), std::memory_order_relaxed); }
    int getMaxDivision() const { return maxDivision.load(std::memory_order_relaxed); }

    // Free mode: rate at zero pressure, kMinRateHz..kMaxRateHz, default 2 Hz.
    void setBaseRateHz(float hz) { baseRateHz.store(juce::jlimit(kMinRateHz, kMaxRateHz, hz), std::memory_order_relaxed); }
    float getBaseRateHz() const { return baseRateHz.load(std::memory_order_relaxed); }

    // Free mode: rate at full pressure, kMinRateHz..kMaxRateHz, default 40 Hz.
    // The sweep between the two is exponential.
    void setMaxRateHz(float hz) { maxRateHz.store(juce::jlimit(kMinRateHz, kMaxRateHz, hz), std::memory_order_relaxed); }
    float getMaxRateHz() const { return maxRateHz.load(std::memory_order_relaxed); }

    // How far pressure moves the rate, 0..1. 0 = pressure ignored (always the
    // base rate), 1 = full pressure reaches the max rate. Default 1.
    void setPressureDepth(float d) { pressureDepth.store(juce::jlimit(0.0f, 1.0f, d), std::memory_order_relaxed); }
    float getPressureDepth() const { return pressureDepth.load(std::memory_order_relaxed); }

    // Note length as a fraction of the hit interval, kMinGate..kMaxGate.
    // 1 = each note lasts until the next hit. Default 0.5.
    void setGate(float g) { gate.store(juce::jlimit(kMinGate, kMaxGate, g), std::memory_order_relaxed); }
    float getGate() const { return gate.load(std::memory_order_relaxed); }

    // Velocity of the retriggers: 0 = the velocity of the pad press,
    // 1 = the pad's current pressure, in between a blend. Default 0.5.
    // The pad press itself always plays at its own velocity.
    void setPressureToVelocity(float a) { pressureToVelocity.store(juce::jlimit(0.0f, 1.0f, a), std::memory_order_relaxed); }
    float getPressureToVelocity() const { return pressureToVelocity.load(std::memory_order_relaxed); }

    // Per-hit velocity decay 0..1: every retrigger is (1 - decay) times the one
    // before, down to kMinVelocity. Default 0 (no fade).
    void setVelocityDecay(float d) { velocityDecay.store(juce::jlimit(0.0f, 1.0f, d), std::memory_order_relaxed); }
    float getVelocityDecay() const { return velocityDecay.load(std::memory_order_relaxed); }

    // Swing 0..1, Quantized mode only (ignored in Free mode). Hits are paired
    // from the start of the hold (press = first of a pair); the second of each
    // pair is delayed by swing * kMaxSwingShift intervals: 0 = 50/50 (straight),
    // 1 = 75/25. The first of every pair stays on the unswung grid. Default 0.
    void setSwing(float s) { swing.store(juce::jlimit(0.0f, 1.0f, s), std::memory_order_relaxed); }
    float getSwing() const { return swing.load(std::memory_order_relaxed); }

    // Snap glide in SECONDS, 0..kMaxGlideSeconds, Quantized mode only. When
    // pressure selects another division, the hit interval moves from the old
    // division's length to the new one's over this time (exponentially, i.e.
    // evenly in pitch-of-rate) instead of jumping at the next hit. Once the time
    // has passed the intervals are exactly the new division's. A glide shorter
    // than the faster of the two intervals is a jump. Default 0 (jump).
    void setGlide(float seconds) { glide.store(juce::jlimit(0.0f, kMaxGlideSeconds, seconds), std::memory_order_relaxed); }
    float getGlide() const { return glide.load(std::memory_order_relaxed); }

    // Pressure threshold 0..1 (raw pad pressure, before pressure depth).
    // 0 = always repeat. Above 0 the pad only repeats while its pressure is at
    // or above the threshold: below it the press sounds once and is held like a
    // plain note until the pad is released. Rising to the threshold retriggers
    // at once and starts a fresh roll (swing, accent and decay count from that
    // hit). Repeats stop when pressure falls below
    // max(threshold - kThresholdHysteresis, threshold / 2): the current note
    // finishes its gate and nothing more is played. Default 0.
    void setThreshold(float t) { threshold.store(juce::jlimit(0.0f, 1.0f, t), std::memory_order_relaxed); }
    float getThreshold() const { return threshold.load(std::memory_order_relaxed); }

    // Latch: a pad that is repeating when it is released keeps rolling, at the
    // pressure (rate) and velocity of its last hit, until the same pad is
    // pressed again (that press only stops the roll, it plays nothing) or
    // allOff(). Switching latch off, or disabling the ratchet, also ends
    // latched rolls. Default off.
    void setLatch(bool l) { latch.store(l, std::memory_order_relaxed); }
    bool isLatch() const { return latch.load(std::memory_order_relaxed); }

    // Humanize 0..1: every retrigger is moved by a random amount up to
    // +-humanize * kHumanizeTime of its gap and its velocity is scaled by a
    // random factor within +-humanize * kHumanizeVelocity. The offsets do not
    // accumulate (the underlying grid is kept), hits never swap order and a
    // hit never lands before the previous note's note-off. Default 0.
    void setHumanize(float h) { humanize.store(juce::jlimit(0.0f, 1.0f, h), std::memory_order_relaxed); }
    float getHumanize() const { return humanize.load(std::memory_order_relaxed); }

    // Restarts the humanize random sequence (xorshift32) at the start of the
    // next processBlock. For repeatable tests.
    void setHumanizeSeed(uint32_t seed) { pendingSeed.store(seed != 0u ? seed : kDefaultSeed, std::memory_order_relaxed); }

    // Accent N: 0 = off, 2..kMaxAccent (1 is stored as 0). Hits 1, N+1, 2N+1...
    // of a roll (the press is hit 1) play at their normal velocity; all others
    // are scaled by kAccentOffScale. Default 0.
    void setAccent(int n) { accent.store(n < 2 ? 0 : std::min(n, kMaxAccent), std::memory_order_relaxed); }
    int getAccent() const { return accent.load(std::memory_order_relaxed); }

    // Probability 0..1 that a retrigger sounds. The first hit of a roll (the
    // press, or the hit that starts a roll at the threshold) always sounds. A
    // skipped retrigger keeps its place: timing, swing and accent counting go
    // on as if it had played, and a note still sounding ends at the skipped
    // hit's time at the latest. Uses the humanize random sequence. Default 1.
    void setProbability(float pr) { probability.store(juce::jlimit(0.0f, 1.0f, pr), std::memory_order_relaxed); }
    float getProbability() const { return probability.load(std::memory_order_relaxed); }

    // Maximum hits of one roll, the press included: 0 = unlimited,
    // 1..kMaxRepeatLimit. Skipped hits (probability) count. After the last hit
    // the roll stops although the pad is still held: the last note ends at its
    // gate (at full gate: where the next hit would have been) and isRolling
    // becomes false. A latched roll that reaches the limit ends and clears its
    // latch. A new press, or pressure dropping below the threshold and rising
    // to it again, starts a fresh count. Default 0.
    void setMaxRepeats(int n) { maxRepeats.store(juce::jlimit(0, kMaxRepeatLimit, n), std::memory_order_relaxed); }
    int getMaxRepeats() const { return maxRepeats.load(std::memory_order_relaxed); }

    // ---- state for displays / pad LEDs (any thread) --------------------------

    // True while the pad is latched (rolling after its release).
    bool isLatched(int pad) const
    {
        return isValidPad(pad) && latchedFlag[(size_t) pad].load(std::memory_order_relaxed);
    }

    // True while the pad is retriggering (held at or above the threshold, or latched).
    bool isRolling(int pad) const
    {
        return isValidPad(pad) && rollingFlag[(size_t) pad].load(std::memory_order_relaxed);
    }

    // ---- audio side -----------------------------------------------------------

    void processBlock(int numSamples, double sampleRate, float bpm)
    {
        if (numSamples < 0)
            numSamples = 0;
        if (sampleRate <= 0.0)
            sampleRate = 44100.0;

        const bool en = enabled.load(std::memory_order_relaxed);
        const bool freeMode = mode.load(std::memory_order_relaxed) == 1;

        blockSampleRate = sampleRate;
        blockBpm = (double) juce::jlimit(20.0f, 999.0f, bpm);

        // Disabled mid-roll: end the rolling notes now.
        if (wasEnabled && ! en)
            for (int p = 0; p < kNumPads; ++p)
                if (pads[(size_t) p].rolling)
                    stopPad(p);
        wasEnabled = en;

        const uint32_t seed = pendingSeed.exchange(0u, std::memory_order_relaxed);
        if (seed != 0u)
            rngState = seed;

        const bool latchOn = latch.load(std::memory_order_relaxed);
        drainEvents(en, freeMode, latchOn);

        if (! latchOn)
            for (int p = 0; p < kNumPads; ++p)
                if (pads[(size_t) p].latched)
                    stopPad(p);

        if (! en)
            return;

        const double n = (double) numSamples;
        const float smoothCoeff = numSamples > 0
            ? (float) (1.0 - std::exp(-n / (kPressureSmoothingSeconds * sampleRate)))
            : 0.0f;

        for (int p = 0; p < kNumPads; ++p)
        {
            auto& pad = pads[(size_t) p];
            if (! pad.rolling)
                continue;

            // A latched pad keeps the pressure of its last hit.
            if (! pad.latched)
            {
                // One-pole smoothing of the (noisy, ~75 Hz) aftertouch.
                const float target = rawPressure[(size_t) p].load(std::memory_order_relaxed);
                pad.pressure += (target - pad.pressure) * smoothCoeff;
                if (std::abs(target - pad.pressure) < 1.0e-5f)
                    pad.pressure = target;

                // Pressure threshold, with hysteresis.
                const bool want = aboveThreshold(pad);
                if (pad.finished)
                {
                    // The roll used up its repeats: wait for pressure to drop
                    // below the threshold before another one may start.
                    if (! want)
                        pad.finished = false;
                }
                else if (want != pad.repeating)
                {
                    pad.repeating = want;
                    rollingFlag[(size_t) p].store(want, std::memory_order_relaxed);
                    if (want)
                    {
                        // A fresh roll (and repeat count) starting with a hit now.
                        pad.toNext = 0.0;
                        pad.hitIndex = -1;
                        pad.step = -1;
                        pad.glideTo = 0.0;
                        pad.jitter = 0.0;
                    }
                    else
                    {
                        pad.toNext = kNever;   // the current note still ends at its gate
                    }
                }
            }

            // Free mode: the rate follows pressure continuously, so the running
            // interval is rescaled in place. Quantized mode only changes at a hit.
            if (freeMode && pad.repeating)
            {
                const double newInterval = freeInterval(pad);
                if (newInterval != pad.interval && pad.interval > 0.0)
                {
                    const double ratio = newInterval / pad.interval;
                    if (pad.toNext < kNever)
                        pad.toNext *= ratio;
                    if (pad.toOff < kNever)
                        pad.toOff *= ratio;
                    pad.jitter *= ratio;
                    pad.interval = newInterval;
                }
            }

            for (;;)
            {
                const bool offDue = pad.sounding && pad.toOff < n;
                const bool hitDue = pad.toNext < n;

                if (offDue && (! hitDue || pad.toOff <= pad.toNext))
                {
                    sendOff(p);
                    continue;
                }

                if (! hitDue)
                    break;

                const double t = pad.toNext;
                if (pad.sounding)
                    sendOff(p);

                ++pad.hitIndex;
                pad.interval = freeMode ? freeInterval(pad) : quantizedInterval(pad);
                const bool fire = pad.hitIndex == 0 || passesProbability();
                if (fire)
                    sendOn(p, hitVelocity(pad));
                scheduleNext(pad, t, freeMode);
                if (! fire)
                    pad.toOff = kNever;   // nothing is sounding
                finishAtLimit(p);
            }

            // A latched roll that used up its repeats ends itself.
            if (pad.finished && pad.latched && ! pad.sounding)
                stopPad(p);

            if (pad.toNext < kNever)
                pad.toNext -= n;
            if (pad.toOff < kNever)
                pad.toOff -= n;
        }
    }

private:
    static constexpr double kNever = std::numeric_limits<double>::max();
    static constexpr double kMinIntervalSamples = 8.0;
    static constexpr uint32_t kDefaultSeed = 0x9E3779B9u;

    enum EventType : uint32_t { EventDown = 1u, EventUp = 2u, EventAllOff = 3u };

    struct PadState
    {
        bool rolling = false;     // held (or latched) with the ratchet enabled
        bool repeating = false;   // rolling and retriggering (at/above the threshold, or latched)
        bool latched = false;     // released, kept rolling by latch
        bool finished = false;    // the roll reached the repeat limit
        bool sounding = false;    // a note-on is waiting for its note-off
        int note = 60;
        float velocity = 1.0f;    // velocity of the pad press
        float pressure = 0.0f;    // smoothed
        int step = 0;             // quantized: current step between base and max division
        int hitIndex = 0;         // 0 = the pad press (or first hit of a roll), 1.. = retriggers
        float hitPressure = 0.0f; // pressure at the last hit (what latch keeps)
        float hitVelocity = 1.0f; // velocity of the last hit before accent / humanize (what latch keeps)
        double glideFrom = 0.0;   // quantized: snap glide start, beats
        double glideTo = 0.0;     // quantized: division the glide ends on, beats (0 = none yet)
        double glideBeats = 0.0;  // quantized: current (gliding) interval, beats
        double glideElapsed = 0.0;// samples since the glide started
        double jitter = 0.0;      // humanize offset included in toNext, samples
        double interval = 0.0;    // samples between hits, as of the last hit
        double toNext = 0.0;      // samples from the block start to the next hit
        double toOff = kNever;    // samples from the block start to the gate end
    };

    static bool isValidPad(int pad) { return pad >= 0 && pad < kNumPads; }
    static int clampDivision(int d) { return juce::jlimit(0, (int) kNumDivisions - 1, d); }

    // ---- event queue (single producer, single consumer) ----------------------

    static constexpr uint32_t kQueueSize = 256u;   // power of two

    void pushEvent(EventType type, int pad, int note, float velocity)
    {
        const uint32_t w = queueWrite.load(std::memory_order_relaxed);
        const uint32_t r = queueRead.load(std::memory_order_acquire);
        if (w - r >= kQueueSize)
        {
            // Full (the audio thread is not running): never lose a note-off,
            // so ask for everything to be ended instead.
            queueOverflow.store(true, std::memory_order_release);
            return;
        }
        const uint32_t vel = (uint32_t) std::lround(velocity * 65535.0f);
        const uint32_t packed = ((uint32_t) type << 29) | ((uint32_t) pad << 24) | ((uint32_t) note << 16) | vel;
        eventQueue[w & (kQueueSize - 1u)].store(packed, std::memory_order_relaxed);
        queueWrite.store(w + 1u, std::memory_order_release);
    }

    void drainEvents(bool en, bool freeMode, bool latchOn)
    {
        uint32_t r = queueRead.load(std::memory_order_relaxed);
        const uint32_t w = queueWrite.load(std::memory_order_acquire);

        while (r != w)
        {
            const uint32_t e = eventQueue[r & (kQueueSize - 1u)].load(std::memory_order_relaxed);
            ++r;

            const uint32_t type = e >> 29;
            const int pad = (int) ((e >> 24) & 0x1Fu);
            if (type == EventAllOff)
            {
                stopAll();
            }
            else if (isValidPad(pad))
            {
                if (type == EventDown)
                    startPad(pad, (int) ((e >> 16) & 0xFFu), (float) (e & 0xFFFFu) / 65535.0f, en, freeMode);
                else if (type == EventUp)
                    releasePad(pad, latchOn);
            }
        }
        queueRead.store(r, std::memory_order_release);

        if (queueOverflow.exchange(false, std::memory_order_acq_rel))
            stopAll();
    }

    // ---- pad handling (audio thread) -----------------------------------------

    void startPad(int p, int note, float velocity, bool en, bool freeMode)
    {
        auto& pad = pads[(size_t) p];

        // Pressing a latched pad only stops its roll.
        if (pad.latched)
        {
            stopPad(p);
            return;
        }

        if (pad.sounding)
            sendOff(p);

        pad.note = note;
        pad.velocity = velocity;
        pad.hitVelocity = velocity;
        pad.hitIndex = 0;
        pad.rolling = en;
        pad.repeating = false;
        pad.latched = false;
        pad.finished = false;
        pad.jitter = 0.0;
        pad.toNext = kNever;
        pad.toOff = kNever;   // plain note, ended by padUp
        sendOn(p, velocity);

        if (en)
        {
            // Start from the pressure as it is now (no smoothing from a stale value).
            pad.pressure = rawPressure[(size_t) p].load(std::memory_order_relaxed);
            pad.hitPressure = pad.pressure;
            pad.step = -1;
            pad.glideTo = 0.0;
            pad.interval = freeMode ? freeInterval(pad) : quantizedInterval(pad);
            pad.repeating = aboveThreshold(pad);
            if (pad.repeating)
            {
                scheduleNext(pad, 0.0, freeMode);
                finishAtLimit(p);
            }
        }
        rollingFlag[(size_t) p].store(pad.repeating, std::memory_order_relaxed);
    }

    // padUp: ends the pad, or latches it if it is repeating and latch is on.
    void releasePad(int p, bool latchOn)
    {
        auto& pad = pads[(size_t) p];
        if (pad.latched)
            return;   // already released; only a new press or allOff ends it
        if (latchOn && pad.rolling && pad.repeating)
        {
            pad.latched = true;
            pad.pressure = pad.hitPressure;
            latchedFlag[(size_t) p].store(true, std::memory_order_relaxed);
            return;
        }
        stopPad(p);
    }

    void stopPad(int p)
    {
        auto& pad = pads[(size_t) p];
        if (pad.sounding)
            sendOff(p);
        pad.rolling = false;
        pad.repeating = false;
        pad.latched = false;
        pad.finished = false;
        pad.toNext = kNever;
        pad.toOff = kNever;
        rollingFlag[(size_t) p].store(false, std::memory_order_relaxed);
        latchedFlag[(size_t) p].store(false, std::memory_order_relaxed);
    }

    void stopAll()
    {
        for (int p = 0; p < kNumPads; ++p)
            stopPad(p);
    }

    void sendOn(int p, float velocity)
    {
        auto& pad = pads[(size_t) p];
        pad.sounding = true;
        if (onNoteOn)
            onNoteOn(p, pad.note, velocity);
    }

    void sendOff(int p)
    {
        auto& pad = pads[(size_t) p];
        pad.sounding = false;
        if (onNoteOff)
            onNoteOff(p, pad.note);
    }

    // Threshold state for the pad's current pressure, with hysteresis.
    bool aboveThreshold(const PadState& pad) const
    {
        const float thr = threshold.load(std::memory_order_relaxed);
        if (thr <= 0.0f)
            return true;
        if (pad.repeating)
            return pad.pressure >= std::max(thr - kThresholdHysteresis, thr * 0.5f);
        return pad.pressure >= thr;
    }

    // Uniform in [-1, 1), xorshift32.
    float nextRandom()
    {
        uint32_t x = rngState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rngState = x;
        return (float) (x >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }

    bool passesProbability()
    {
        const float pr = probability.load(std::memory_order_relaxed);
        if (pr >= 1.0f)
            return true;
        return (nextRandom() + 1.0f) * 0.5f < pr;
    }

    // Called after a hit has been scheduled: if it was the roll's last allowed
    // hit, no further hit follows and its note ends at the gate (full gate:
    // where the next hit would have been).
    void finishAtLimit(int p)
    {
        auto& pad = pads[(size_t) p];
        const int limit = maxRepeats.load(std::memory_order_relaxed);
        if (limit <= 0 || pad.hitIndex + 1 < limit)
            return;
        if (pad.sounding && pad.toOff >= kNever)
            pad.toOff = pad.toNext;
        pad.toNext = kNever;
        pad.repeating = false;
        pad.finished = true;
        rollingFlag[(size_t) p].store(false, std::memory_order_relaxed);
    }

    // Called at a hit at time t (pad.interval and pad.hitIndex are that hit's):
    // sets the time of the next hit (swing, humanize) and of this note's end.
    void scheduleNext(PadState& pad, double t, bool freeMode)
    {
        double gap = pad.interval;
        if (! freeMode)
        {
            const double s = (double) (swing.load(std::memory_order_relaxed) * kMaxSwingShift);
            if (s > 0.0)
                gap *= (pad.hitIndex & 1) ? 1.0 - s : 1.0 + s;
        }

        // pad.jitter is how late this hit was against its grid time; the next
        // one is placed against the grid again, so offsets do not accumulate.
        const float h = humanize.load(std::memory_order_relaxed);
        double newJitter = h > 0.0f ? (double) (nextRandom() * h * kHumanizeTime) * gap : 0.0;
        double actual = gap;
        if (newJitter != 0.0 || pad.jitter != 0.0)
        {
            actual = gap + newJitter - pad.jitter;
            const double minGap = std::max(kMinIntervalSamples, 0.1 * gap);
            if (actual < minGap)
            {
                actual = minGap;
                newJitter = actual - gap + pad.jitter;
            }
        }
        pad.jitter = newJitter;
        pad.toNext = t + actual;

        // The gate is a fraction of the real distance to the next hit, so the
        // note-off can never come after it.
        const float g = gate.load(std::memory_order_relaxed);
        pad.toOff = g >= 0.999f ? kNever   // legato: the next hit (or padUp) ends the note
                                : t + (double) g * actual;
    }

    // Velocity of the retrigger being played (pad.hitIndex already counts it).
    float hitVelocity(PadState& pad)
    {
        float v;
        if (pad.latched)
        {
            v = pad.hitVelocity;
        }
        else
        {
            v = retriggerVelocity(pad);
            pad.hitVelocity = v;
            pad.hitPressure = pad.pressure;
        }

        const int n = accent.load(std::memory_order_relaxed);
        if (n >= 2 && (pad.hitIndex % n) != 0)
            v *= kAccentOffScale;

        const float h = humanize.load(std::memory_order_relaxed);
        if (h > 0.0f)
            v *= 1.0f + nextRandom() * h * kHumanizeVelocity;

        return juce::jlimit(kMinVelocity, 1.0f, v);
    }

    float retriggerVelocity(const PadState& pad) const
    {
        const float amount = pressureToVelocity.load(std::memory_order_relaxed);
        const float decay = velocityDecay.load(std::memory_order_relaxed);
        float v = pad.velocity + (pad.pressure - pad.velocity) * amount;
        if (decay > 0.0f)
            v *= std::pow(1.0f - decay, (float) pad.hitIndex);
        return juce::jlimit(kMinVelocity, 1.0f, v);
    }

    // Free mode: samples per hit for the pad's current pressure.
    double freeInterval(const PadState& pad) const
    {
        const double base = (double) baseRateHz.load(std::memory_order_relaxed);
        const double top = (double) maxRateHz.load(std::memory_order_relaxed);
        const double amount = (double) (pad.pressure * pressureDepth.load(std::memory_order_relaxed));
        const double hz = base * std::pow(top / base, amount);
        return std::max(kMinIntervalSamples, blockSampleRate / hz);
    }

    // Quantized mode: picks the division for the pad's current pressure (with
    // hysteresis against the step it is on) and returns its length in samples
    // at the current tempo. Called at a hit only, so changes never land
    // mid-interval.
    double quantizedInterval(PadState& pad) const
    {
        const int base = baseDivision.load(std::memory_order_relaxed);
        const int top = maxDivision.load(std::memory_order_relaxed);
        const int span = std::abs(top - base);
        const float amount = pad.pressure * pressureDepth.load(std::memory_order_relaxed);

        // span + 1 equal pressure bands, one per division.
        const float x = amount * (float) (span + 1);
        int step = pad.step;
        if (step < 0 || step > span)
            step = juce::jlimit(0, span, (int) std::floor(x));
        else if (x >= (float) (step + 1) + kHysteresis || x < (float) step - kHysteresis)
            step = juce::jlimit(0, span, (int) std::floor(x));
        pad.step = step;

        const int division = base + (top >= base ? step : -step);
        const double targetBeats = getDivisionBeats(division);
        const double samplesPerBeat = 60.0 / blockBpm * blockSampleRate;
        const double glideSamples = (double) glide.load(std::memory_order_relaxed) * blockSampleRate;

        if (pad.glideTo <= 0.0 || glideSamples <= 0.0)
        {
            // First hit of a roll, or no glide: jump.
            pad.glideFrom = pad.glideTo = pad.glideBeats = targetBeats;
        }
        else
        {
            if (targetBeats != pad.glideTo)
            {
                pad.glideFrom = pad.glideBeats;
                pad.glideTo = targetBeats;
                pad.glideElapsed = 0.0;
            }
            if (pad.glideBeats != pad.glideTo)
            {
                // Position on the glide at the end of the coming interval,
                // estimated with the shorter of the current and target lengths.
                const double ahead = std::min(pad.glideBeats, pad.glideTo) * samplesPerBeat;
                const double frac = (pad.glideElapsed + ahead) / glideSamples;
                pad.glideBeats = frac >= 1.0 ? pad.glideTo
                                             : pad.glideFrom * std::pow(pad.glideTo / pad.glideFrom, frac);
                pad.glideElapsed += pad.glideBeats * samplesPerBeat;
            }
        }

        const double samples = pad.glideBeats * samplesPerBeat;
        return std::max(kMinIntervalSamples, samples);
    }

    // ---- settings -------------------------------------------------------------

    std::atomic<bool> enabled { false };
    std::atomic<int> mode { 0 };
    std::atomic<int> baseDivision { (int) Div1_8 };
    std::atomic<int> maxDivision { (int) Div1_128 };
    std::atomic<float> baseRateHz { 2.0f };
    std::atomic<float> maxRateHz { 40.0f };
    std::atomic<float> pressureDepth { 1.0f };
    std::atomic<float> gate { 0.5f };
    std::atomic<float> pressureToVelocity { 0.5f };
    std::atomic<float> velocityDecay { 0.0f };
    std::atomic<float> swing { 0.0f };
    std::atomic<float> glide { 0.0f };
    std::atomic<float> threshold { 0.0f };
    std::atomic<bool> latch { false };
    std::atomic<float> humanize { 0.0f };
    std::atomic<int> accent { 0 };
    std::atomic<float> probability { 1.0f };
    std::atomic<int> maxRepeats { 0 };
    std::atomic<uint32_t> pendingSeed { 0u };

    // ---- shared between the control side and the audio side -------------------

    std::array<std::atomic<uint32_t>, kQueueSize> eventQueue;
    std::atomic<uint32_t> queueWrite { 0u };
    std::atomic<uint32_t> queueRead { 0u };
    std::atomic<bool> queueOverflow { false };
    std::array<std::atomic<float>, kNumPads> rawPressure;
    std::array<std::atomic<bool>, kNumPads> rollingFlag;
    std::array<std::atomic<bool>, kNumPads> latchedFlag;

    // ---- audio thread only ------------------------------------------------------

    std::array<PadState, kNumPads> pads {};
    bool wasEnabled = false;
    double blockSampleRate = 44100.0;
    double blockBpm = 120.0;
    uint32_t rngState = kDefaultSeed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PadRatchet)
};
