#pragma once

#include "Detector.h"
#include "FxLfo.h"
#include "DelayCore.h"
#include "../filters/FilterSlot.h"
#include "../filters/SVFilter.h"
#include <array>
#include <vector>

// One swept filter for every effect that is a filter moved by something: the wah, the phaser, the
// filter effect and TALK's vowel filter are settings of swp::Core, and so each has what the others
// have.
//
//                    +-- the effects' LFO (FxLfo.h: every shape, a second reader for the right side,
//   where it is  <---+   trigger modes, delay and fade; an onset of the key can trigger it)
//   (a place)        +-- the key's envelope (Detector.h: the sound itself, or the track's input)
//        |
//        v
//   in -> [ the filter, one a side ] -> dry / wet
//
// The filter is one of three kinds:
//   Slot      any type and mode of FilterSlot (state-variable, SEM, ladder, diode, comb);
//   Cascade   two to six first-order all-passes with feedback (the phaser);
//   Formant   three band-passes on the formants of a vowel, A - E - I - O - U (TALK).
// What the two sources move is one number, the place: octaves round a frequency (Octaves), a
// frequency in Hz (Hertz, the phaser's own law), or the vowel itself (Place). Every kind can be
// put at a frequency or at a vowel: a ladder that follows another track's level, a phaser whose
// sweep starts again on every kick, vowels swept by a random LFO.
//
// Nothing here is new low-level DSP: the filters are the project's (FilterSlot.h), the all-pass
// and the formant table are the phaser's and TALK's own, moved here unchanged.
namespace swp
{

// The filters a FILT cell offers in one list (the wah's): every type and mode of FilterSlot, the
// phaser's cascade at two, four and six stages, and the vowels. Stored by index: it only grows at
// the end (the first entry is what a wah was before it had the cell).
namespace filt
{
static constexpr int SLOT_TYPES = FilterSlot::NUM_MODELS - 1;       // every FilterSlot type except OFF
enum Type { Phaser = SLOT_TYPES, Vowel, NumTypes };
struct Entry { const char* name; int type, mode; };
static constexpr int COUNT = 21;
inline const Entry& entry(int index)
{
    static const Entry list[COUNT] = {
        { "S.BP", 0, 1 }, { "S.LP", 0, 0 }, { "S.HP", 0, 2 }, { "S.NT", 0, 3 },
        { "SEM", 1, 0 }, { "SEMB", 1, 1 },
        { "L24", 2, 0 }, { "L18", 2, 1 }, { "L12", 2, 2 }, { "L6", 2, 3 }, { "L.BP", 2, 4 }, { "L.HP", 2, 5 },
        { "D.LP", 3, 0 }, { "D.HP", 3, 1 }, { "D.BP", 3, 2 },
        { "CMB+", 4, 0 }, { "CMB-", 4, 1 },
        { "PH2", Phaser, 2 }, { "PH4", Phaser, 4 }, { "PH6", Phaser, 6 },
        { "VOWL", Vowel, 0 } };
    return list[std::clamp(index, 0, COUNT - 1)];
}
inline const char* const* names()
{
    static const char* names[COUNT] = {};
    static const bool made = [] { for (int i = 0; i < COUNT; ++i) names[i] = entry(i).name; return true; }();
    juce::ignoreUnused(made);
    return names;
}
}

class Core
{
public:
    static constexpr int MAX_STAGES = 6;
    static constexpr float VOWEL_LOW_HZ = 150.0f, VOWEL_OCTAVES = 4.0f;     // a frequency as a vowel: 150 Hz = A, 2.4 kHz = U

    enum Law { Octaves = 0, Hertz, Place };

    // What a setting of the core fixes once (its cells set the rest).
    struct Voicing
    {
        Law law = Octaves;
        bool slots = true;              // room for the FilterSlot kinds (TALK has none: it never leaves its vowels)
        float baseMs = 20.0f;           // how a moved base is reached
        float resoMs = 10.0f, depthMs = 30.0f, amountMs = 10.0f;
        float attackMs = 3.0f, releaseMs = 150.0f;      // the envelope
        float envGain = 2.0f;           // the envelope is the level times this, held to 1
        bool meanLevel = false;         // the level is the mean of the two sides (TALK, as it always was); if not, the detector's (the louder side)
        float lowHz = 60.0f, highHz = 12000.0f;         // where a swept frequency is held
        bool peakLevel = false;         // the state-variable band-pass is brought down by sqrt(Q) (the wah: its peak then gains sqrt(Q))
        bool cascadeSum = false;        // the cascade is heard with the dry sound, half each (a phaser in one piece), or by itself
    };

    void prepare(double sr, const Voicing& v)
    {
        sampleRate = sr;
        voicing = v;
        perRate = 1.0f / (float) sr;
        topHz = std::min(voicing.highHz, (float) sr * 0.45f);
        if (voicing.slots)
        {
            slot.resize(2);
            for (auto& s : slot) s.setSampleRate(sr);
        }
        for (auto& side : formant)
            for (auto& f : side)
            {
                f.setSampleRate(sr);
                f.setMode(SVFilter::Mode::BandPass);
            }
        detector.prepare(sr);
        follower.setTimes(sr, voicing.attackMs, voicing.releaseMs);
        onset.prepare(sr);
        lfo.prepare(sr);
        stageStep = 1.0f / (0.004f * (float) sr);
        duckStep = 1.0f / std::max(8.0f, 0.0015f * (float) sr);
        // (reset() is the caller's next call: everything where it is set.)
    }

    // Everything where it is set, nothing ringing.
    void reset()
    {
        type = typeWanted; mode = modeWanted;
        applyFilter();
        clearFilters();
        setTimes();
        baseSmooth.snap(baseWanted);
        resoSmooth.snap(resoWanted);
        depthSmooth.snap(depthWanted);
        amountSmooth.snap(amountWanted);
        detector.reset();
        follower.reset(0.0f);
        onset.reset();
        lfo.reset();
        stagesNow = (float) stagesWanted();
        duck = duckTarget = 1.0f;
        untilTune = 0;
        quiet = 0;
        cleared = false;
        seenHz[0] = seenHz[1] = seenReso = seenPlace = -1.0f;
        gainNow = outGain = 1.0f;
        gainInc = 0.0f;
        env = 0.0f;
    }

    // ---- what the effect asks for ----------------------------------------------------------------
    // The filter: a FilterSlot type (its display index) and mode, filt::Phaser (mode = stages) or
    // filt::Vowel. Under sound it is exchanged in the dark: the wet sound is taken down for a
    // millisecond and a half, the filter changed, and it comes back.
    void setFilter(int filterType, int filterMode) { typeWanted = std::clamp(filterType, 0, (int) filt::NumTypes - 1); modeWanted = std::max(0, filterMode); }
    // Where the filter rests: Hz (Octaves, Hertz) or the vowel 0..1 (Place).
    void setBase(float v) { baseWanted = v; }
    // Slot: FilterSlot's resonance (0..1; above 1 a state-variable Q). Cascade: its feedback, -0.9..0.9. Formant: 0..1.
    void setResonance(float r) { resoWanted = r; }
    // The LFO: how far it moves the place either way (in the law's own measure), the right side this share of a cycle ahead.
    void setLfo(float rateHz, int shape, float depth, float spread)
    {
        lfo.setRate(rateHz);
        lfo.setShape(shape);
        lfo.setOffset(spread);
        depthWanted = depth;
    }
    // What starts the LFO again (LFO::TriggerMode), and its delay and fade from there: an onset of the key.
    void setTrigger(int triggerMode, float delaySeconds, float fadeSeconds) { lfo.setTrigger(triggerMode, delaySeconds, fadeSeconds); }
    // The envelope: how far a full level moves the place (either way).
    void setEnvelope(float amount) { amountWanted = amount; }
    void setBite(float gain) { biteGain = gain; }          // Formant: the saturation before the bands (1 = just its curve)
    void setShift(float octaves) { shift = octaves; }      // Formant: all three formants up or down
    void setUserShape(const float* levels) { lfo.setUserShape(levels); }

    // Where the filter is now: its frequency (Slot, Cascade), the vowel 0..1 (Formant). For a meter or a test.
    float currentHz() const { return nowHz; }
    float currentPlace() const { return nowPlace; }
    float envelope() const { return env; }
    fxm::Lfo& sweepLfo() { return lfo; }

    // Whether run() may be handed any number of samples: always (the core counts its own steps).
    // `n` samples in place; keyLeft / keyRight is what the envelope and the trigger listen to (it
    // may be the sound itself). In (1 - mix) + wet mix, the mix going from mixFrom to mixTo.
    void run(float* left, float* right, int n, const float* keyLeft, const float* keyRight, float mixFrom, float mixTo)
    {
        const float mixStep = (mixTo - mixFrom) / (float) std::max(1, n);
        float mix = mixFrom;
        // (One loop for each kind of filter, so that none pays for the others; a filter exchanged
        // in the middle of the run ends its loop there.)
        for (int done = 0; done < n;)
        {
            float* l = left + done;
            float* r = right + done;
            const int rest = n - done;
            done += kind == Slot ? runAs<Slot>(l, r, rest, keyLeft + done, keyRight + done, mix, mixStep)
                  : kind == Cascade ? runAs<Cascade>(l, r, rest, keyLeft + done, keyRight + done, mix, mixStep)
                                    : runAs<Formant>(l, r, rest, keyLeft + done, keyRight + done, mix, mixStep);
        }
    }

private:
    enum Kind { Slot = 0, Cascade, Formant };

    template <int K>
    int runAs(float* left, float* right, int n, const float* keyLeft, const float* keyRight, float& mixNow, float mixStep)
    {
        const bool listening = lfo.listens();
        const bool following = amountWanted != 0.0f || amountSmooth.value() != 0.0f;
        const bool keyed = listening || following;
        if (! keyed) env = 0.0f;
        const bool summed = voicing.cascadeSum, mean = voicing.meanLevel;
        const float bite = biteGain;
        // (Local copies for the loop: a write to the output could otherwise be a write to any of them.)
        std::array<float, MAX_STAGES> sL = stateL, sR = stateR;
        float fL = fedL, fR = fedR, count = stagesNow, goal = stagesGoal, cL = coeffL, cR = coeffR, fb = feedback;
        float gNow = gainNow, gInc = gainInc, e = env, dk = duck, mix = mixNow;
        std::array<float, 3> gain = gains;
        dyn::Detector listener = detector;
        dyn::Follower follow = follower;
        int until = untilTune, still = quiet, i = 0;
        bool swapped = false;

        for (; i < n && ! swapped; ++i)
        {
            if (keyed)
            {
                // (The mean: what is not a number is nothing, as the detector has it.)
                const float both = 0.5f * (std::abs(keyLeft[i]) + std::abs(keyRight[i]));
                const float level = ! mean ? listener.level(keyLeft[i], keyRight[i])
                                           : (both < dyn::Detector::MAX_LEVEL ? both : (both >= dyn::Detector::MAX_LEVEL ? dyn::Detector::MAX_LEVEL : 0.0f));
                e = follow.up(level);
                if (listening && onset.hears(level)) lfo.trigger();
            }
            const bool tuned = --until <= 0;
            if (tuned)
            {
                env = e;
                tune();
                until = interval;
                cL = coeffL; cR = coeffR; fb = feedback; goal = stagesGoal; gNow = gainNow; gInc = gainInc; gain = gains;
            }

            const float inL = left[i], inR = right[i];
            float wetL, wetR;
            if (K == Slot)
            {
                // (The level goes to where it is put in a straight line between two steps: a step of it is a step of the sound.)
                gNow += gInc;
                wetL = slot[0].process(inL) * gNow;
                wetR = slot[1].process(inR) * gNow;
            }
            else if (K == Cascade)
            {
                // The stages in use; a new count is crossed to over a few milliseconds, the last stage
                // coming in or going out by degrees.
                if (count != goal)
                    count = dsp::towards(count, goal, stageStep);
                const int whole = (int) count;
                const float part = count - (float) whole;
                wetL = inL + fL * fb;
                wetR = inR + fR * fb;
                for (int s = 0; s < whole; ++s)
                {
                    wetL = allpass(wetL, sL[(size_t) s], cL);
                    wetR = allpass(wetR, sR[(size_t) s], cR);
                }
                if (part > 0.0f)
                {
                    wetL += part * (allpass(wetL, sL[(size_t) whole], cL) - wetL);
                    wetR += part * (allpass(wetR, sR[(size_t) whole], cR) - wetR);
                }
                fL = fxb::flushDenormal(std::clamp(wetL, -4.0f, 4.0f));
                fR = fxb::flushDenormal(std::clamp(wetR, -4.0f, 4.0f));
                if (summed)
                {
                    wetL = 0.5f * (inL + wetL);
                    wetR = 0.5f * (inR + wetR);
                }
            }
            else
            {
                // A little saturation first gives the formants harmonics to pick out.
                const float xL = dsp::fastTanh(inL * bite), xR = dsp::fastTanh(inR * bite);
                wetL = wetR = 0.0f;
                for (int f = 0; f < 3; ++f)
                {
                    wetL += formant[0][(size_t) f].process(xL) * gain[(size_t) f];
                    wetR += formant[1][(size_t) f].process(xR) * gain[(size_t) f];
                }
                wetL *= FORMANT_MAKEUP;
                wetR *= FORMANT_MAKEUP;
            }

            if (dk != 1.0f || duckTarget != 1.0f)
            {
                dk += std::clamp(duckTarget - dk, -duckStep, duckStep);
                wetL *= dk;
                wetR *= dk;
                swapped = dk <= 0.0f && duckTarget == 0.0f;     // (in the dark: the other filter, from the next sample on)
            }

            mix += mixStep;
            left[i] = inL * (1.0f - mix) + wetL * mix;
            right[i] = inR * (1.0f - mix) + wetR * mix;

            // The filters' states are private: once input and output have both been at nothing for
            // a while (looked at where the filter is tuned: QUIET_LOOKS times running), clear them,
            // so a ringing tail cannot linger in the denormal range. (The cascade's stages are put
            // away at the end of the run instead.)
            if (K != Cascade && tuned)
            {
                if (std::abs(inL) + std::abs(inR) + std::abs(wetL) + std::abs(wetR) < 1.0e-12f)
                {
                    if (++still >= QUIET_LOOKS)
                    {
                        still = QUIET_LOOKS;
                        if (! cleared) { clearFilters(); cleared = true; }
                    }
                }
                else
                {
                    still = 0;
                    cleared = false;
                }
            }
        }

        // (A stage left to die away ends at the smallest number there is and stays there for good.)
        if (K == Cascade)
            for (int s = 0; s < MAX_STAGES; ++s)
            {
                stateL[(size_t) s] = fxb::flushDenormal(sL[(size_t) s]);
                stateR[(size_t) s] = fxb::flushDenormal(sR[(size_t) s]);
            }
        detector = listener; follower = follow;
        fedL = fL; fedR = fR; stagesNow = count;
        gainNow = gNow; env = e; duck = dk; mixNow = mix;
        untilTune = until; quiet = still;
        if (swapped) exchange();
        return i;
    }

    static constexpr int QUIET_LOOKS = 16;
    static constexpr float FORMANT_MAKEUP = 2.0f;
    static constexpr float PHASER_MAX_NORM = 0.45f;

    static Kind kindOf(int t) { return t == filt::Phaser ? Cascade : (t == filt::Vowel ? Formant : Slot); }
    // How often each kind is put somewhere new, in samples: the all-pass's coefficient is cheap and
    // was always set for every sample; a FilterSlot's is a tangent or more; the formants are three
    // powers and six tangents.
    static int intervalOf(Kind k) { return k == Cascade ? 1 : (k == Slot ? 4 : 16); }
    int stagesWanted() const { return std::clamp(modeWanted, 2, MAX_STAGES); }

    // First-order allpass filter: y[n] = coeff * (x[n] - y[n-1]) + x[n-1]
    // state holds x[n-1] mixed with y[n-1]
    static float allpass(float input, float& state, float coeff)
    {
        const float output = coeff * input + state;
        state = input - coeff * output;
        return output;
    }

    DLY_INLINE float allpassCoeff(float freq) const
    {
        const float w = dsp::PI_F * std::min(freq * perRate, PHASER_MAX_NORM);
        const float t = dsp::fastTan(w);
        return (t - 1.0f) / (t + 1.0f);
    }

    void setTimes()
    {
        const double ticks = sampleRate / (double) interval;
        auto time = [ticks](fxb::Smoother& s, float ms) { if (ms > 0.0f) s.setTime(ticks, ms); else s.a = 1.0f; };    // (0: at once)
        time(baseSmooth, voicing.baseMs);
        time(resoSmooth, voicing.resoMs);
        time(depthSmooth, voicing.depthMs);
        time(amountSmooth, voicing.amountMs);
    }

    // The filter that was asked for is taken up (its states are whatever they were: see exchange()).
    void applyFilter()
    {
        kind = kindOf(type);
        if (kind == Slot && slot.empty()) { kind = Formant; type = filt::Vowel; }
        interval = intervalOf(kind);
        if (kind == Slot)
            for (auto& s : slot)
            {
                s.setModel(0, FilterSlot::modelFromIndex(type));
                s.setMode(0, mode);
            }
        svfBand = kind == Slot && FilterSlot::modelFromIndex(type) == FilterModel::SVF && mode == 1;
        stagesGoal = (float) std::clamp(mode, 2, MAX_STAGES);
        seenHz[0] = seenHz[1] = seenReso = seenPlace = -1.0f;
    }

    void clearFilters()
    {
        for (auto& s : slot) s.reset();
        for (auto& side : formant)
            for (auto& f : side) f.reset();
        stateL.fill(0.0f);
        stateR.fill(0.0f);
        fedL = fedR = 0.0f;
    }

    // In the dark: the other filter, empty, and what glides counted in its own steps.
    void exchange()
    {
        const bool otherKind = kindOf(typeWanted) != kind;
        const bool onlyStages = ! otherKind && kind == Cascade;
        type = typeWanted; mode = modeWanted;
        const float base = baseSmooth.value(), depth = depthSmooth.value(), amount = amountSmooth.value();
        applyFilter();
        if (! onlyStages) clearFilters();
        setTimes();
        // (The resonance is the new filter's from its first sample: see tune().)
        baseSmooth.snap(base); resoSmooth.snap(resoWanted); depthSmooth.snap(depth); amountSmooth.snap(amount);
        untilTune = 0;
        duckTarget = 1.0f;
    }

    // Every `interval` samples: where the filter is, from the base, the LFO (taken that many
    // samples on) and the envelope.
    DLY_INLINE void tune()
    {
        float a, b;
        lfo.take(interval, a, b);
        gainNow = outGain;      // (arrived)
        gainInc = 0.0f;
        float resoGoal = resoWanted;
        if (typeWanted != type || modeWanted != mode)
        {
            // (Another number of stages is crossed to; anything else is exchanged in the dark.
            // While another filter waits for the dark the resonance stays where it is: what is
            // asked for may be the other filter's measure - a state-variable Q of 12 is no feedback
            // for the cascade - and is taken up with it, in exchange().)
            if (kind == Cascade && typeWanted == type) { mode = modeWanted; stagesGoal = (float) stagesWanted(); }
            else { duckTarget = 0.0f; resoGoal = resoSmooth.value(); }
        }

        const float base = baseSmooth.next(baseWanted), reso = resoSmooth.next(resoGoal);
        const float depth = depthSmooth.next(depthWanted), amount = amountSmooth.next(amountWanted);
        const float pushed = amount != 0.0f ? amount * std::min(1.0f, voicing.envGain * env) : 0.0f;
        const float moveL = depth != 0.0f ? depth * a + pushed : pushed;
        const float moveR = depth != 0.0f && b != a ? depth * b + pushed : moveL;

        if (voicing.law == Place)
        {
            // (TALK: the place is the vowel, whatever the filter.)
            const float p = std::clamp(base + moveL, 0.0f, 1.0f);
            nowPlace = p;
            if (kind == Formant) placeFormants(p, reso);
            else                 placeAt(VOWEL_LOW_HZ * std::exp2(p * VOWEL_OCTAVES), VOWEL_LOW_HZ * std::exp2(std::clamp(base + moveR, 0.0f, 1.0f) * VOWEL_OCTAVES), reso);
            return;
        }
        const float top = topHz;
        // (At rest nothing is raised to a power.)
        const float hzL = std::clamp(voicing.law != Octaves ? base + moveL : (moveL == 0.0f ? base : base * std::exp2(moveL)), voicing.lowHz, top);
        const float hzR = moveR == moveL ? hzL : std::clamp(voicing.law == Octaves ? base * std::exp2(moveR) : base + moveR, voicing.lowHz, top);
        if (kind == Formant)
        {
            nowPlace = std::clamp(std::log2(hzL / VOWEL_LOW_HZ) / VOWEL_OCTAVES, 0.0f, 1.0f);
            placeFormants(nowPlace, reso);
            nowHz = hzL;
        }
        else
            placeAt(hzL, hzR, reso);
    }

    DLY_INLINE void placeAt(float hzL, float hzR, float reso)
    {
        nowHz = hzL;
        if (kind == Cascade)
        {
            coeffL = allpassCoeff(hzL);
            coeffR = hzR != hzL ? allpassCoeff(hzR) : coeffL;
            feedback = reso;
            return;
        }
        if (reso != seenReso)
        {
            seenReso = reso;
            slot[0].setResonance(0, reso);
            slot[1].setResonance(0, reso);
            outGain = voicing.peakLevel && svfBand ? 1.0f / std::sqrt(std::max(1.0f, reso)) : 1.0f;
            gainInc = (outGain - gainNow) / (float) interval;
        }
        if (hzL != seenHz[0]) { seenHz[0] = hzL; slot[0].setCutoff(0, hzL); }
        if (hzR != seenHz[1]) { seenHz[1] = hzR; slot[1].setCutoff(0, hzR); }
    }

    // Three parallel band-pass filters on the first three formants of a vowel; the position moves
    // continuously through A - E - I - O - U. Frequencies and relative levels are standard
    // phonetics values for a male voice (as tabulated e.g. in the Csound manual).
    void placeFormants(float position, float reso)
    {
        if (position == seenPlace && reso == seenReso && shift == seenShift) return;
        seenPlace = position; seenReso = reso; seenShift = shift;
        //                               F1      F2      F3       levels (F2, F3 lifted for clarity)
        static const float table[5][6] = { { 650.0f, 1080.0f, 2650.0f, 1.0f, 0.70f, 0.50f },    // A
                                           { 400.0f, 1700.0f, 2600.0f, 1.0f, 0.45f, 0.40f },    // E
                                           { 290.0f, 1870.0f, 2800.0f, 1.0f, 0.40f, 0.30f },    // I
                                           { 400.0f,  800.0f, 2600.0f, 1.0f, 0.55f, 0.35f },    // O
                                           { 350.0f,  600.0f, 2700.0f, 1.0f, 0.35f, 0.30f } };  // U
        const float p = position * 4.0f;
        const int a = std::clamp((int) p, 0, 3);
        const float t = p - (float) a;
        const float up = std::exp2(shift);
        const float r = std::clamp(reso, 0.0f, 1.0f);
        const float q = 3.0f + r * r * 27.0f;

        for (int f = 0; f < 3; ++f)
        {
            // Frequencies move on a log scale between neighbouring vowels.
            const float hz = table[a][f] * std::pow(table[a + 1][f] / table[a][f], t) * up;
            const float amp = table[a][f + 3] + t * (table[a + 1][f + 3] - table[a][f + 3]);
            for (auto& side : formant)
            {
                side[(size_t) f].setResonance(q);
                side[(size_t) f].setCutoff(std::clamp(hz, 60.0f, (float) (sampleRate * 0.45)));
            }
            // The band output peaks at Q: bring it back to the formant's level, with a little extra
            // loudness as the bands narrow.
            gains[(size_t) f] = amp / q * (1.0f + r);
        }
    }

    double sampleRate = 44100.0;
    Voicing voicing;
    float perRate = 1.0f / 44100.0f, topHz = 12000.0f;

    // the setting
    int typeWanted = 0, modeWanted = 0, type = 0, mode = 0;
    float baseWanted = 1000.0f, resoWanted = 0.0f, depthWanted = 0.0f, amountWanted = 0.0f;
    float biteGain = 1.0f, shift = 0.0f;

    // the sources
    fxm::Lfo lfo;
    dyn::Detector detector;
    dyn::Follower follower;
    fxm::Onset onset;
    float env = 0.0f;
    fxb::Smoother baseSmooth, resoSmooth, depthSmooth, amountSmooth;
    int untilTune = 0, interval = 4;

    // the filter
    Kind kind = Slot;
    std::vector<FilterSlot> slot;           // one a side (none where the voicing has no use for them)
    bool svfBand = false;
    float outGain = 1.0f, gainNow = 1.0f, gainInc = 0.0f;
    float seenHz[2] = { -1.0f, -1.0f }, seenReso = -1.0f, seenPlace = -1.0f, seenShift = 0.0f;
    std::array<float, MAX_STAGES> stateL {}, stateR {};
    float fedL = 0.0f, fedR = 0.0f, coeffL = 0.0f, coeffR = 0.0f, feedback = 0.0f;
    float stagesNow = 4.0f, stagesGoal = 4.0f, stageStep = 0.005f;
    std::array<std::array<SVFilter, 3>, 2> formant;
    std::array<float, 3> gains { 1.0f, 0.5f, 0.3f };

    float duck = 1.0f, duckTarget = 1.0f, duckStep = 0.01f;
    float nowHz = 1000.0f, nowPlace = 0.0f;
    int quiet = 0;
    bool cleared = false;
};

// An effect that is the core and its cells (CellEffect.h, through fxm::KeyedCells: the KEY cell).
class Effect : public fxm::KeyedCells
{
public:
    void setUserShape(const float* levels) override { core.setUserShape(levels); }
    // Where the filter is right now (after envelope and LFO), for meters and tests.
    float getCurrentFrequency() const { return core.currentHz(); }

protected:
    virtual Core::Voicing voicing() const = 0;

    void prepareCore() override { core.prepare(sampleRate, voicing()); }
    void resetCore() override { core.reset(); dropKey(); }
    bool coasting() const override { return true; }     // (nothing here waits for controlCore(): the core counts its own steps)
    void runKeyed(float* left, float* right, int n, const float* keyLeft, const float* keyRight, float mixFrom, float mixTo) override
    {
        core.run(left, right, n, keyLeft, keyRight, mixFrom, mixTo);
    }

    Core core;
};

} // namespace swp
