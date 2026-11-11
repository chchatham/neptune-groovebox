#pragma once

#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>
#include <algorithm>

// Resampler: records the instrument's own sound (one track after its effects, or the master
// output) into memory so it can become a sample or a wavetable.
//
//   message thread:  prepare(), settings, arm(), stop(), cancel(), then when isFinished():
//                    read left()/right()/length(), and release() to go back to idle
//   audio thread:    process() once per block with the chosen source
//
// Start: at once; or when the signal first exceeds the threshold (a short pre-roll keeps the attack);
// or, with bar start on and the sequencer running, exactly on the next bar line. End: stop(), the
// fixed length in beats (exact to the sample, so a bar-started take loops), or the maximum length.
// Nothing is allocated or locked on the audio thread.
class Resampler
{
public:
    enum class State { Idle, Armed, Recording, Finished };

    static constexpr int MASTER = -1;
    static constexpr float MAX_SECONDS = 60.0f;
    static constexpr int PREROLL_MS = 20;

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        const size_t cap = (size_t) (sampleRate * MAX_SECONDS);
        bufL.assign(cap, 0.0f);
        bufR.assign(cap, 0.0f);
        preL.assign((size_t) (sampleRate * PREROLL_MS / 1000.0) + 1, 0.0f);
        preR.assign(preL.size(), 0.0f);
        state.store(State::Idle);
        count.store(0);
    }

    // ---- Settings (message thread; read by the audio thread) ----
    void setSource(int trackOrMaster) { source.store(trackOrMaster); }
    int getSource() const { return source.load(); }
    void setThreshold(float t) { threshold.store(std::clamp(t, 0.0f, 1.0f)); }      // 0 = start at once
    float getThreshold() const { return threshold.load(); }
    void setLengthBeats(float beats) { lengthBeats.store(std::max(0.0f, beats)); }   // 0 = until stopped
    float getLengthBeats() const { return lengthBeats.load(); }
    void setBarStart(bool on) { barStart.store(on); }
    bool isBarStart() const { return barStart.load(); }

    // ---- Control (message thread) ----
    void arm()
    {
        if (bufL.empty() || state.load() != State::Idle) return;
        count.store(0);
        prePos = 0;
        preFilled = 0;
        targetSamples = 0;
        peak.store(0.0f);
        state.store(State::Armed);
    }
    void stop()         // keep what was recorded so far; from Armed this just cancels
    {
        State s = State::Recording;
        if (state.compare_exchange_strong(s, State::Finished)) return;
        s = State::Armed;
        state.compare_exchange_strong(s, State::Idle);
    }
    void release() { state.store(State::Idle); count.store(0); }     // after the take has been used
    void cancel() { stop(); release(); }

    State getState() const { return state.load(); }
    bool isFinished() const { return state.load() == State::Finished; }
    bool isActive() const { auto s = state.load(); return s == State::Armed || s == State::Recording; }
    int length() const { return count.load(); }
    float seconds() const { return (float) ((double) count.load() / sampleRate); }
    const float* left() const { return bufL.data(); }
    const float* right() const { return bufR.data(); }
    double getSampleRate() const { return sampleRate; }
    float getPeak() const { return peak.load(); }           // input level of the latest block (meter)

    // ---- Audio thread ----
    // `beatAtStart` is the sequencer's position in beats at the first sample of this block,
    // `beatsPerSample` its speed; `playing` says whether it is running.
    void process(const float* l, const float* r, int n, double beatAtStart, double beatsPerSample, bool playing)
    {
        const State s = state.load(std::memory_order_relaxed);
        if (s != State::Armed && s != State::Recording)
            return;
        if (r == nullptr) r = l;

        float blockPeak = 0.0f;
        for (int i = 0; i < n; ++i)
            blockPeak = std::max(blockPeak, std::max(std::abs(l[i]), std::abs(r[i])));
        peak.store(blockPeak, std::memory_order_relaxed);

        int from = 0;
        if (s == State::Armed)
        {
            from = -1;
            const float thr = threshold.load(std::memory_order_relaxed);
            if (barStart.load(std::memory_order_relaxed) && playing && beatsPerSample > 0.0)
            {
                // The first sample at or after the next bar line (a bar is four beats).
                const double nextBar = std::ceil(beatAtStart / 4.0 - 1.0e-9) * 4.0;
                const double offset = (nextBar - beatAtStart) / beatsPerSample;
                if (offset < (double) n)
                    from = std::max(0, (int) std::ceil(offset - 1.0e-6));
            }
            else if (thr > 0.0f)
            {
                for (int i = 0; i < n && from < 0; ++i)
                    if (std::max(std::abs(l[i]), std::abs(r[i])) >= thr)
                        from = i;
                if (from >= 0)
                    flushPreroll(l, r, from);
            }
            else
                from = 0;

            if (from < 0)
            {
                if (thr > 0.0f) writePreroll(l, r, n);
                return;
            }
            const float beats = lengthBeats.load(std::memory_order_relaxed);
            targetSamples = (beats > 0.0f && beatsPerSample > 0.0)
                                ? (int) std::lround((double) beats / beatsPerSample) + count.load(std::memory_order_relaxed)
                                : 0;
            state.store(State::Recording, std::memory_order_relaxed);
        }

        int have = count.load(std::memory_order_relaxed);
        const int cap = (int) bufL.size();
        const int limit = targetSamples > 0 ? std::min(targetSamples, cap) : cap;
        const int take = std::min(n - from, limit - have);
        if (take > 0)
        {
            std::copy(l + from, l + from + take, bufL.begin() + have);
            std::copy(r + from, r + from + take, bufR.begin() + have);
            have += take;
            count.store(have, std::memory_order_relaxed);
        }
        if (have >= limit)
        {
            State rec = State::Recording;
            state.compare_exchange_strong(rec, State::Finished);
        }
    }

private:
    void writePreroll(const float* l, const float* r, int n)
    {
        const int size = (int) preL.size();
        for (int i = 0; i < n; ++i)
        {
            preL[(size_t) prePos] = l[i];
            preR[(size_t) prePos] = r[i];
            prePos = (prePos + 1) % size;
        }
        preFilled = std::min(size, preFilled + n);
    }

    // The pre-roll kept so far, then this block's samples before the trigger point, oldest first.
    void flushPreroll(const float* l, const float* r, int upTo)
    {
        writePreroll(l, r, upTo);
        const int size = (int) preL.size();
        int have = 0;
        const int start = (prePos - preFilled + size) % size;
        for (int i = 0; i < preFilled && have < (int) bufL.size(); ++i, ++have)
        {
            bufL[(size_t) have] = preL[(size_t) ((start + i) % size)];
            bufR[(size_t) have] = preR[(size_t) ((start + i) % size)];
        }
        count.store(have, std::memory_order_relaxed);
    }

    double sampleRate = 44100.0;
    std::vector<float> bufL, bufR, preL, preR;
    int prePos = 0, preFilled = 0, targetSamples = 0;
    std::atomic<State> state { State::Idle };
    std::atomic<int> count { 0 };
    std::atomic<int> source { MASTER };
    std::atomic<float> threshold { 0.0f };
    std::atomic<float> lengthBeats { 0.0f };
    std::atomic<bool> barStart { false };
    std::atomic<float> peak { 0.0f };
};
