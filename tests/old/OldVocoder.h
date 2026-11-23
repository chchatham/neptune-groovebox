#pragma once

// THE OLD CLASS, FOR THE TESTS ONLY. The vocoder (Vocoder.h) as it was at ff42d45, before phase U4-0 of
// .ralph/spec-unify.md moved its analysis (the sixteen bands and their followers) out as
// voc::Analysis for the modulation router; only the class name and the include paths were changed.
// The suite "Router Sources" plays it beside the new class and expects the same samples, bit for
// bit. It is not part of the product. Do not edit it: it is the reference.

#include "../../src/dsp/effects/EffectBase.h"
#include "../../src/dsp/effects/Detector.h"
#include "../../src/dsp/effects/CellEffect.h"
#include "../../src/dsp/ModeBank.h"
#include "../../src/dsp/Resonator.h"
#include <array>
#include <cstdint>

// Vocoder, VOC. The track's own sound is the CARRIER; the track's input (TrackInParam: another
// track's sound) is the MODULATOR. The modulator, summed to mono, goes through 16 band-passes from
// 100 Hz to 8 kHz, each followed by an envelope follower (dyn::Follower, the one of the dynamics
// effects): that is the analysis, and it is the same in both modes. What the 16 levels are put on
// is the MODE:
//
//   BAND  a channel vocoder (the structure of Homer Dudley's, 1939, as built in every analogue one
//         since): the carrier goes through 16 band-passes of its own, each scaled by its band's
//         level, and they are summed.
//   TUNE  a spectral resonator: the carrier drives a tuned body (res::Resonator: up to 64 ringing
//         modes whose partial series is the BODY cell's, from a string to a bell, at the pitch of
//         the note), and every partial is driven with the level of the analysis band its frequency
//         falls in (between two band centres: between their levels). A voice or a drum loop speaks
//         through a bell or a plate at the played pitch, and goes on ringing for RING. The body is
//         driven by the carrier's difference (each partial a band-pass, as a band is: nothing of
//         the carrier at 0 Hz or far under the note comes through), made up for partial by partial.
//
// A stereo carrier stays stereo in both.
//
// The bands are modes of dsp/ModeBank.h (a mode driven by a signal is a resonant low-pass; fed the
// signal's difference it is a band-pass). A band is two such band-passes in a row, as it was when
// they were two SVFilters; here they are modes k and 16 + k of one bank of 32, the second driven by
// the first, and the two differences are taken once, of the input, for all sixteen. Equal ratios
// (1.34, about five semitones), each band-pass's -3 dB width the distance to the next band, so that
// a band is 6 dB down and a quarter turn out where it crosses its neighbour; neighbours are summed
// with opposite signs, which puts them back in phase there (the sum is even across the range).
// A bank that has rung out is not run (ModeBank::rest), and neither is a carrier's while every
// band's level is 0: a vocoder nobody speaks into costs next to nothing. A sample under -200 dB is
// none (a tail that never quite ends does end). Switched off, or with its source taken away, it
// keeps nothing of what rang or was heard (a frozen spectrum it keeps).
//
// The AudioEngine hands the input over with setInput before each process / processBlock call; the
// pointers are used for that call only and then dropped, so a stale one is never read. With no
// input (null: the player has not chosen a source) the carrier passes untouched, sample for sample.
// With an input that is silent, MIX at the top is silence: nothing shapes the carrier.
//
// The pitch of TUNE is the track's last note when somebody says it (setPitch, EffectBase's; nobody
// does yet: the AudioEngine does not know an engine's last note, see spec-unify.md U4) and C3
// otherwise; BAND moves it by semitones either way.
//
// Cells: MODE   BAND / TUNE. Turned under sound, the vocoded sound dips for a few milliseconds.
//        BAND   moves the carrier's side against the modulator's, in semitones (-24..24). BAND
//               mode: the carrier's bands, so the voice's formants come out higher or lower than
//               they went in. TUNE mode: the body's pitch.
//        NOIS   white noise added to the carrier: consonants, which a pitched carrier has nothing
//               for, and a silent carrier, still speak
//        MIX    the carrier as it came to the vocoded sound
//   FX2: REL    how fast a band's level falls (it rises in 2 ms)
//        RING   TUNE: the time the body's first partial takes to fall by 60 dB
//        BODY   TUNE: the body, 0..5 = STRG TUBE COMB PLAT BAR BELL and everything between
//        FRZ    the bands hold the levels they have: the vowel stays while the modulator goes on
//               or stops
// NOIS, REL, RING and BODY take modulation (modulateCell) and glide to it.
//
// Level: the band levels are the modulator's as they are (a louder voice is a louder result), times
// a make-up set so that full-scale white noise over white noise comes out at the carrier's level
// (measured at 48 kHz: the "Vocoder" suite; in TUNE for the default body, C3 and RING). What comes
// out is held below 2 whatever goes in (untouched below 1: res::outLimit, the resonator's).
class OldVocoder : public EffectBase
{
public:
    enum Param { Mode = 0, Band, Noise, Mix };
    enum Extra { Release = 0, Ring, BodyAt, Freeze };
    enum ModeChoice { Bands = 0, Tuned };
    // The eight as modulateCell counts them.
    enum Cell { ModeCell = 0, BandCell, NoiseCell, MixCell, ReleaseCell, RingCell, BodyCell, FreezeCell, NumCells };

    static constexpr int NUM_BANDS = 16;
    static constexpr float LOW_HZ = 100.0f, HIGH_HZ = 8000.0f;
    static constexpr float ATTACK_MS = 2.0f;
    static constexpr float MAKEUP = 7.4f;               // BAND
    static constexpr float TUNE_MAKEUP = 32.0f;         // TUNE
    static constexpr float NOISE_LEVEL = 0.5f;          // NOIS at the top: white noise of this peak
    static constexpr float DEFAULT_HZ = 130.8128f;      // C3: TUNE's pitch until a note is given
    static constexpr int MAX_SHIFT = 24;

    // Centre of analysis band b in Hz.
    static float bandHz(int b)
    {
        return LOW_HZ * std::pow(HIGH_HZ / LOW_HZ, (float) std::clamp(b, 0, NUM_BANDS - 1) / (float) (NUM_BANDS - 1));
    }
    // Q of one band-pass: its -3 dB width is the distance to the next band.
    static float bandQ()
    {
        const float r = std::sqrt(std::pow(HIGH_HZ / LOW_HZ, 1.0f / (float) (NUM_BANDS - 1)));
        return 1.0f / (r - 1.0f / r);
    }

    OldVocoder()
    {
        mix = 1.0f;
        prepare(44100.0, 0);
    }

    static const ParamSpec& cellSpec(int cell)
    {
        using K = ParamKind;
        static const char* const modes[] = { "BAND", "TUNE" };
        static const char* const offOn[] = { "OFF", "ON" };
        static const ParamSpec specs[NumCells + 1] = {
            { "MODE", 0, 0, 1, 0, K::Choice, "", modes, 2 },
            { "BAND", 1, (float) -MAX_SHIFT, (float) MAX_SHIFT, 0, K::Int, "st" },
            { "NOIS", 2, 0, 1, 0.0f },
            { "MIX", 3, 0, 1, 1.0f },
            { "REL", 0, 5.0f, 500.0f, 60.0f, K::Log, "ms" },
            { "RING", 1, 0.02f, 10.0f, 0.4f, K::Log, "s" },
            { "BODY", 2, 0.0f, (float) (res::NumBodyPoints - 1), (float) res::Plate },
            { "FRZ", 3, 0, 1, 0, K::Choice, "", offOn, 2 },
            {} };
        return specs[cell >= 0 && cell < NumCells ? cell : NumCells];
    }

    ParamSpec getParamSpec(int index) const override { return index >= 0 && index < 4 ? cellSpec(index) : ParamSpec(); }
    ParamSpec getExtraSpec(int index) const override { return index >= 0 && index < MAX_EXTRAS ? cellSpec(4 + index) : ParamSpec(); }
    void setParameter(int id, float value) override { if (id >= 0 && id < 4) store(id, value); }
    float getParameter(int id) const override { return id >= 0 && id < 4 ? cell(id) : 0.0f; }
    void setExtra(int index, float value) override { if (index >= 0 && index < MAX_EXTRAS) store(4 + index, value); }
    float getExtra(int index) const override { return index >= 0 && index < MAX_EXTRAS ? cell(4 + index) : 0.0f; }

    // NOIS, REL, RING and BODY move (the others are a list, whole steps, a switch, and the mix,
    // which PERF's throw counts on being what it was set to).
    void modulateCell(int cell, float offset) override
    {
        if (cell == NoiseCell || cell == ReleaseCell || cell == RingCell || cell == BodyCell)
            modulation[(size_t) cell] = CellEffect::offsetOf(offset);
    }

    int keySource() const override { return KEY_IN; }
    void setInput(const float* left, const float* right, int frames) override
    {
        const bool there = left != nullptr && frames > 0;
        inputL = there ? left : nullptr;
        inputR = there ? (right != nullptr ? right : left) : nullptr;
        inputFrames = there ? frames : 0;
    }
    bool hasInput() const { return inputL != nullptr && inputFrames > 0; }

    // The pitch of the track's note, for TUNE; it stays until the next. 0 or less (or no number):
    // none is known, and TUNE is at C3.
    void setPitch(float hz) override { playedHz = hz > 0.0f && std::isfinite(hz) ? hz : 0.0f; }

    // The level of band b's envelope now (a meter, and the tests').
    float bandLevel(int b) const { return followers[(size_t) std::clamp(b, 0, NUM_BANDS - 1)].y; }
    // TUNE: the body's pitch now in Hz, and partial n's frequency over it (the tests').
    float tunedHz() const { return pitchOf(shiftNow); }
    float tunedPartial(int n) const { return body[0].partial(n); }
    // How many modes ran in the last sample (bands count two; a meter of what it costs, and the tests').
    int modesAwake() const
    {
        return analysis.bank.awake + (modeNow == Tuned ? body[0].modesAwake() + body[1].modesAwake()
                                                       : synthesis[0].bank.awake + synthesis[1].bank.awake);
    }

    void prepare(double sr, int /*maxBlockSize*/) override
    {
        sampleRate = (std::isfinite(sr) && sr >= 1000.0) ? sr : 44100.0;
        for (auto& b : body)
        {
            b.prepare(sampleRate);
            // Nothing of the exciter but its way in for what comes from outside, as open as it goes.
            b.setExciter(res::ExciteLatent { { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f } });
        }
        period = body[0].controlPeriod();
        connectStep = 1.0f / (float) std::max(1.0, 0.005 * sampleRate);
        fadeStep = 1.0f / (float) std::max(1.0, 0.003 * sampleRate);
        shiftStep = (float) period * 12.0f / (float) std::max(1.0, 0.03 * sampleRate);     // an octave in 30 ms
        positionControls = 2 + (int) (0.04 * sampleRate) / period;                          // a body's glide, and more
        reset();
    }

    void reset() override
    {
        modeNow = modeWanted();
        modeGain = 1.0f;
        shiftNow = value[BandCell];
        noiseNow = moved(NoiseCell);
        analysis.tune(sampleRate, 1.0f);
        for (auto& b : body) b.setSpectrum(nullptr);
        silence(true);
        carriedBefore[0] = carriedBefore[1] = 0.0f;
        releaseSet = -1.0f;
        positionHz = 0.0f;
        inputL = inputR = nullptr;
        inputFrames = 0;
        connected = -1.0f;
        looks = 0;
        seed = 0x2545F491u;
    }

    void process(float& left, float& right) override
    {
        processBlock(&left, &right, 1);
    }

    void processBlock(float* left, float* right, int numSamples) override
    {
        const float* inL = inputL;
        const float* inR = inputR;
        const int inFrames = inputFrames;
        inputL = inputR = nullptr;                      // this call only
        inputFrames = 0;
        if (numSamples <= 0) return;

        const bool there = inL != nullptr && inFrames > 0;
        if (connected < 0.0f) connected = there ? 1.0f : 0.0f;         // the first block: as it is
        if (! enabled || (! there && connected <= 0.0f))                // switched off, or no source: the carrier passes
        {
            // (What rang and what was heard when it stopped is not kept for when it goes on,
            // however much later; a frozen spectrum is.)
            if (holds) silence(! frozen());
            return;
        }
        holds = true;

        const float connectTarget = there ? 1.0f : 0.0f;
        alignas(32) float levels[NUM_BANDS];

        for (int i = 0; i < numSamples; ++i)
        {
            if (--countdown <= 0)
            {
                countdown = period;
                control();
            }
            if (modeGain <= 0.0f && modeNow != modeWanted())
                changeMode();
            modeGain = dsp::towards(modeGain, modeNow == modeWanted() ? 1.0f : 0.0f, fadeStep);
            noiseNow = dsp::towards(noiseNow, noiseTarget, fadeStep);

            float modulator = 0.0f;
            if (i < inFrames)
            {
                const float m = 0.5f * (inL[i] + inR[i]);
                if (std::abs(m) >= SILENT && std::isfinite(m)) modulator = std::clamp(m, -4.0f, 4.0f);
            }
            // The analysis runs while frozen too, so that the bands are where the sound is when
            // they are let go.
            const bool heard = analysis.tick(modulator, nullptr);
            if ((heard || envLive) && ! frozen())
            {
                for (size_t b = 0; b < (size_t) NUM_BANDS; ++b)
                    followers[b].up(std::abs(analysis.bank.y[NUM_BANDS + b]));
                envLive = true;
            }

            const float hiss = noiseNow > 0.0f ? dsp::xorshiftNoise(seed) * noiseNow * NOISE_LEVEL : 0.0f;
            const float dryL = left[i], dryR = right[i];
            const float carL = carried(dryL) + hiss, carR = carried(dryR) + hiss;
            // (Nobody speaks: the carrier is kept out, and its bands or the body ring out and rest.)
            float wetL, wetR;
            if (modeNow == Bands)
            {
                for (size_t b = 0; b < (size_t) NUM_BANDS; ++b)
                    levels[b] = (b & 1) != 0 ? -followers[b].y : followers[b].y;
                synthesis[0].tick(envLive ? carL : 0.0f, levels);
                synthesis[1].tick(envLive ? carR : 0.0f, levels);
                wetL = synthesis[0].sum * MAKEUP;
                wetR = synthesis[1].sum * MAKEUP;
            }
            else
            {
                // (The carrier's difference: see `lift`.)
                wetL = body[0].tick(driven ? carL - carriedBefore[0] : 0.0f, 0.0f);
                wetR = body[1].tick(driven ? carR - carriedBefore[1] : 0.0f, 0.0f);
            }
            carriedBefore[0] = carL;
            carriedBefore[1] = carR;

            connected = dsp::towards(connected, connectTarget, connectStep);
            const float m = mix * connected;
            if (m <= 0.0f) continue;                    // the carrier as it came, whatever it is
            // (A carrier sample that is not a number times a share of 0 is still not a number:
            // it is left out of the mix as it is left out of the bands.)
            const float keptL = std::isfinite(dryL) ? dryL : 0.0f;
            const float keptR = std::isfinite(dryR) ? dryR : 0.0f;
            left[i] = keptL * (1.0f - m) + res::outLimit(wetL * modeGain, OUT_LIMIT) * m;
            right[i] = keptR * (1.0f - m) + res::outLimit(wetR * modeGain, OUT_LIMIT) * m;
        }
    }

private:
    static constexpr float RESTS = 1.0e-20f;            // a block of eight modes that holds less is emptied (-200 dB)
    static constexpr float SILENT = 1.0e-10f;           // a sample under this is none: what dies away ends, and costs nothing
    static constexpr float OUT_LIMIT = 2.0f;
    static constexpr double MAX_OF_RATE = 0.45;         // no band centre above this share of the sample rate

    // Sixteen band-passes of the fourth order as one bank of 32 modes: mode k is band k's first
    // pole pair, mode 16 + k its second, driven by the first. The input's second difference (one
    // zero at 0 Hz for each pole pair) is taken once for all of them.
    struct BandBank
    {
        dsp::ModeBank bank;
        alignas(32) float couple[NUM_BANDS] {};
        float before = 0.0f, twoBefore = 0.0f;
        float sum = 0.0f;                               // the bands times their levels, after tick

        // The bands at bandHz(b) * ratio, held below MAX_OF_RATE of the sample rate.
        void tune(double sampleRate, float ratio)
        {
            alignas(32) float c[dsp::ModeBank::MAX_MODES] {}, s[dsp::ModeBank::MAX_MODES] {}, in[dsp::ModeBank::MAX_MODES] {};
            const float halfQ = 0.5f / bandQ();
            for (int b = 0; b < NUM_BANDS; ++b)
            {
                const float hz = std::clamp(bandHz(b) * ratio, 20.0f, (float) (sampleRate * MAX_OF_RATE));
                const float w = 6.2831855f * hz / (float) sampleRate, radius = std::exp(-w * halfQ);
                const float cw = std::cos(w), sw = std::sin(w);
                c[b] = c[NUM_BANDS + b] = radius * cw;
                s[b] = s[NUM_BANDS + b] = radius * sw;
                // One pole pair and its zero at the band's centre, z = e^jw: |z - 1| r sin w / |z^2 - 2 r cos w z + r^2|.
                const float re = cw * cw - sw * sw - 2.0f * c[b] * cw + radius * radius, im = 2.0f * sw * cw - 2.0f * c[b] * sw;
                const float gain = 2.0f * std::sin(0.5f * w) * s[b] / std::sqrt(re * re + im * im);
                in[b] = couple[b] = 1.0f / gain;
            }
            // (The levels - the second sixteen's `out` - stay as they are while it is retuned.)
            alignas(32) float out[dsp::ModeBank::MAX_MODES];
            std::copy(bank.out, bank.out + dsp::ModeBank::MAX_MODES, out);
            bank.retarget(c, s, in, out, 2 * NUM_BANDS, 0);
        }

        void clear()
        {
            bank.clearState();
            std::fill(bank.out, bank.out + dsp::ModeBank::MAX_MODES, 0.0f);
            before = twoBefore = sum = 0.0f;
        }

        // One sample; levels: what each band is summed with (nullptr: no sum is wanted). False:
        // nothing went in and nothing rings, and nothing was run.
        bool tick(float x, const float* levels) noexcept
        {
            const float force = x - 2.0f * before + twoBefore;
            twoBefore = before;
            before = x;
            if (force == 0.0f && bank.awake == 0)
            {
                sum = 0.0f;
                return false;
            }
            bank.wake();
            for (int b = 0; b < NUM_BANDS; ++b)
                bank.x[NUM_BANDS + b] += couple[b] * bank.y[b];
            if (levels != nullptr)
                std::copy(levels, levels + NUM_BANDS, bank.out + NUM_BANDS);
            sum = bank.tick(force);
            return true;
        }

        void rest() noexcept { bank.rest(RESTS); }
    };

    // The carrier as the bands or the body take it: held, and nothing where it is no number or
    // under SILENT.
    static float carried(float x)
    {
        return std::abs(x) >= SILENT && std::isfinite(x) ? std::clamp(x, -8.0f, 8.0f) : 0.0f;
    }

    // Nothing rings on the analysis' side either, and (levelsToo) nothing has been heard.
    void silence(bool levelsToo)
    {
        analysis.clear();
        envLive = false;
        for (auto& f : followers)
        {
            if (levelsToo) f.reset(0.0f);
            envLive = envLive || f.y != 0.0f;
        }
        rewind();
        holds = false;
        countdown = 0;
    }

    // Nothing rings on the carrier's side, in either mode, and it starts as the cells stand.
    void rewind()
    {
        for (auto& s : synthesis)
        {
            s.tune(sampleRate, shiftRatio());
            s.clear();
        }
        for (auto& b : body) b.reset();
        sent.fill(-1.0f);
        tunedShift = shiftNow;
        bodySet = -1.0f;
        positionsLeft = positionControls;
        driven = anyBefore = false;
    }

    void store(int c, float v)
    {
        const ParamSpec& spec = cellSpec(c);
        if (! std::isfinite(v)) return;
        v = std::clamp(v, spec.minVal, spec.maxVal);
        if (spec.isDiscrete()) v = std::round(v);
        if (c == MixCell) setMix(v); else value[(size_t) c] = v;
    }
    float cell(int c) const { return c == MixCell ? mix : value[(size_t) c]; }
    float moved(int c) const { return CellEffect::moved(cellSpec(c), value[(size_t) c], modulation[(size_t) c]); }
    int modeWanted() const { return value[ModeCell] >= 0.5f ? Tuned : Bands; }
    bool frozen() const { return value[FreezeCell] >= 0.5f; }
    float shiftRatio() const { return std::pow(2.0f, shiftNow / 12.0f); }
    float pitchOf(float shift) const { return (playedHz > 0.0f ? playedHz : DEFAULT_HZ) * std::pow(2.0f, shift / 12.0f); }

    // The vocoded sound has faded out: the other mode takes over from silence.
    void changeMode()
    {
        modeNow = modeWanted();
        rewind();
        countdown = period;         // (the body looks at its cells in its first sample: so does this, with it)
        control();
    }

    // Every `period` samples (the body's own look at its cells falls in the same sample).
    void control()
    {
        const float release = moved(ReleaseCell);
        if (release != releaseSet)
        {
            releaseSet = release;
            for (auto& f : followers) f.setTimes(sampleRate, ATTACK_MS, release);
        }
        noiseTarget = moved(NoiseCell);
        const bool shifting = shiftNow != value[BandCell];
        if (shifting)
            shiftNow = dsp::towards(shiftNow, value[BandCell], shiftStep);

        const bool resting = (++looks & 3) == 0;
        if (resting)
        {
            analysis.rest();
            if (envLive && analysis.bank.awake == 0)
            {
                envLive = false;
                for (const auto& f : followers) envLive = envLive || f.y != 0.0f;
            }
        }

        if (modeNow == Bands)
        {
            if (shifting || tunedShift != shiftNow)
            {
                tunedShift = shiftNow;
                for (auto& s : synthesis) s.tune(sampleRate, shiftRatio());
            }
            if (resting)
                for (auto& s : synthesis) s.rest();
            return;
        }

        const float hz = pitchOf(shiftNow), at = moved(BodyCell);
        if (at != bodySet)
        {
            bodySet = at;
            positionsLeft = positionControls;
        }
        if (hz != positionHz)
            positionsLeft = std::max(positionsLeft, 2);
        for (auto& b : body)
        {
            b.setBody(at);
            b.setDecay(moved(RingCell));
            b.setPitch((double) hz);
        }
        // Where each partial lies among the analysis bands (its place, and how far on to the next):
        // worked out again only while the pitch or the body moves.
        if (positionsLeft > 0)
        {
            --positionsLeft;
            positionHz = hz;
            const float perBand = (float) (NUM_BANDS - 1) / std::log(HIGH_HZ / LOW_HZ);
            const float heldHz = (float) std::clamp((double) hz, res::Resonator::MIN_HZ, MAX_OF_RATE * sampleRate);
            const float top = (float) (MAX_OF_RATE * sampleRate), halfTurn = 3.1415927f / (float) sampleRate;
            for (int n = 0; n < res::MODES; ++n)
            {
                // (A body that has not looked at its cells yet has no series: nothing is driven.)
                const float ratio = body[0].partial(n), partialHz = std::max(heldHz * ratio, 1.0f);
                const float p = std::clamp(std::log(partialHz / LOW_HZ) * perBand, 0.0f, (float) (NUM_BANDS - 1));
                const int below = std::min((int) p, NUM_BANDS - 2);
                place[n] = below;
                onward[n] = p - (float) below;
                lift[n] = ratio > 0.0f ? TUNE_MAKEUP * 0.5f / std::sin(halfTurn * std::min(partialHz, top)) : 0.0f;
            }
        }
        alignas(32) float gains[res::MODES];
        bool any = false, same = true;
        for (int n = 0; n < res::MODES; ++n)
        {
            const float lower = followers[(size_t) place[n]].y, upper = followers[(size_t) place[n] + 1].y;
            gains[n] = lift[n] * (lower + (upper - lower) * onward[n]);
            any = any || gains[n] > 0.0f;
            same = same && gains[n] == sent[(size_t) n];
        }
        if (! same)
        {
            std::copy(gains, gains + res::MODES, sent.begin());
            for (auto& b : body) b.setSpectrum(gains);
        }
        // (Nobody speaks, now or a moment ago: the carrier is kept out of the body, which rings
        // out and rests.)
        driven = any || anyBefore;
        anyBefore = any;
    }

    std::array<float, NumCells> value { { 0.0f, 0.0f, 0.0f, 1.0f, 60.0f, 0.4f, (float) res::Plate, 0.0f } };
    std::array<float, NumCells> modulation {};
    float playedHz = 0.0f;

    const float* inputL = nullptr;      // valid only during the process call that follows setInput
    const float* inputR = nullptr;
    int inputFrames = 0;

    BandBank analysis;
    std::array<BandBank, 2> synthesis;                  // BAND: left, right
    std::array<res::Resonator, 2> body;                 // TUNE: left, right
    std::array<dyn::Follower, NUM_BANDS> followers;
    std::array<float, res::MODES> sent {};              // the spectrum the bodies were last given
    int place[res::MODES] {};
    float onward[res::MODES] {};
    // TUNE: a mode driven by a sound is a low-pass that rings (whatever the carrier holds under
    // the note, down to 0 Hz, would come through as it is); driven by the sound's difference it
    // is a band-pass, as the bands are. `lift` is what the difference takes from a partial at its
    // own frequency, given back (and the make-up): a partial rings as loud either way.
    float lift[res::MODES] {};
    float carriedBefore[2] {};

    int period = 16, countdown = 0, positionControls = 2, positionsLeft = 0;
    unsigned looks = 0;
    int modeNow = Bands;
    float modeGain = 1.0f, fadeStep = 1.0f;
    float releaseSet = -1.0f, bodySet = -1.0f, positionHz = 0.0f;
    float noiseNow = 0.0f, noiseTarget = 0.0f;
    float shiftNow = 0.0f;              // where the carrier's side is, in semitones (it glides to BAND)
    float tunedShift = 0.0f, shiftStep = 1.0f;
    bool envLive = false;               // a band's level is not 0, or the analysis rings
    bool driven = false, anyBefore = false;
    bool holds = false;                 // it has run since it was last silenced
    float connected = -1.0f;            // 0 = no input, 1 = input; below 0 = not known yet
    float connectStep = 1.0f;
    uint32_t seed = 0x2545F491u;
};
