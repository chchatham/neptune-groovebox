#pragma once

#include "ParamSpec.h"
#include "EngineLfoSection.h"
#include "ModRouter.h"
#include "VoiceSpread.h"
#include "../dsp/Envelope.h"
#include "../dsp/filters/FilterSlot.h"
#include <algorithm>
#include <array>
#include <cmath>

// -----------------------------------------------------------------------------
// The per-voice filter of every engine: two filters (A and B, each any FilterSlot type and
// mode), routed single, in series or in parallel with a balance, a drive per filter, filter FM
// from an audio-rate signal the voice hands in, and the filter's own ADSR. One class per voice
// (VoiceFilter) and one per engine (VoiceFilterSection: ids, pages, true values).
//
//   FILTR   CUT  RESO ENV  DRIV | TYPE MODE MORP KTRK
//   FLT B   OFSB RESB FFM  DRVB | TYPB MODB ROUT BAL        (an engine with the dual block)
//   F ENV   ATK  DEC  SUS  REL  | CURV EVEL VEL  WHL
//
//     TYPB MODB  filter B's type and mode (a second SVF until chosen; it is in the path once ROUT
//                leaves SNGL: until then the section is filter A alone)
//     OFSB       filter B's cutoff, in semitones from filter A's (it follows everything that
//                moves A's: envelope, key, velocity, strip, LFOs, FM)
//     RESB DRVB  filter B's own resonance and drive
//     ROUT       SNGL: A only (the default).  SER: A into B.  PAR: both side by side, mixed by BAL.
//     BAL        PAR: 0 = A only, 100 = B only. (With TYPB at OFF, PAR mixes the filtered sound
//                with the unfiltered one.)
//     FFM        the voice's own oscillator moves the cutoff at audio rate, up to 2 octaves
//                either way (which signal: the engine's choice, see process())
//
// The cutoff is moved in octaves. Who adds the octaves up is the one thing that differs
// between engines today, so the voice class has two doors onto the same filter:
//   process(in, fm)                 the section adds them: its own cells ENV / EVEL / KTRK / VEL
//                                   (and WHL through setExternalOctaves), the LFOs, the note's
//                                   spread. For an engine that is not on the modulation router.
//   processOctaves(in, oct, fm)     the caller adds them and hands the sum in: the router's CUT
//                                   buffer (key, velocity, envelope and strip are then default
//                                   slots), or the unified voice's own sum, in its own order.
//                                   tickEnvelope() advances the filter envelope and returns it.
// Everything after the sum is the same arithmetic, and it is the unified voice's: cutoff A =
// base x 2^oct, cutoff B = base x 2^(oct x TRKB) x 2^(OFSB / 12), both held to 20 Hz .. 20 kHz,
// then FilterSlot::process. With filter FM at 0 the sum handed in is used untouched, so a caller
// that adds in the unified voice's order gets the unified voice's samples ("Voice Filter Spread"
// pins that bit for bit for SVF LP + SVF LP in PAR and for the ladder LP24, at 44.1 and 48 kHz).
// It was also run beside the REAL UnifiedVoice playing the P01 fixture (every section of the pin,
// both sounds, 44.1 and 48 kHz, plain and seeded: 5.4 million voice samples, the voice's
// FilterSlot recorded and this class fed the same input and the voice's own sum): the same
// samples, provided the voice starts its notes with noteOnKeepingState() (see there). The two
// sounds use: SVF LP + SVF LP in PAR at BAL 0.5 with DRIV / DRVB, and the ladder LP24 alone with
// drive; no filter FM, no resonance modulation, a cutoff that is never turned under a note, no
// Hz offset from the old mod matrix and no envelope chain on the cutoff.
// Where it is NOT that voice's arithmetic, and why:
//   - with FFM above 0 the FM term is added to the caller's sum as one number ((fm) + (rest));
//     the unified voice adds it first, term by term. The last bit can differ. (The pinned sounds
//     have no filter FM: the voice hands its oscillator 2 in as `fm` and nothing moves.)
//   - the highest cutoff is min(20 kHz, 0.49 x sample rate); the unified voice hands 20 kHz to
//     its filters at any rate. The same from 40.8 kHz up; below, this is the safer one.
//   - a CUT that is turned under a note is moved to along a one-pole (5 ms), the unified
//     voice's along a straight line in octaves. At rest both are the cell's value exactly.
//   - a note on a voice that was silent clears the filters and the envelope (noteOn(.., false));
//     the unified voice never clears them at a note: noteOnKeepingState() is that.
//   - what the unified voice adds to the cutoff in Hz (the old mod matrix's offset, the envelope
//     chain aimed at the cutoff) has no door here: both become octaves when that voice moves (U4).
//     Its LFO on filter B's cutoff alone is OFSB's per-sample value (setOffsetBNow), the same
//     pitch but not the same last bit.
//
// With ROUT at SNGL and FFM at 0 (the defaults) the section is the single filter it was, sample
// for sample, at the same cost: one flag per sample (`plain`) says that nothing new is in use.
// A value that is not a number never reaches a filter: a cell does not take one (set()), and a
// cutoff, resonance or balance worked out from one is held to the low end of its range.
//
// Nothing is jumped to: a cell that is set under a sounding note (drive, resonance, balance,
// FM depth, filter B's offset and tracking) is moved to over about 4 ms; TYPE / MODE / ROUT
// are swaps (EngineBase::soundSwapped, see VoiceFilterSection::swaps). A note takes
// everything as it stands. No allocation; everything in VoiceFilter is safe on the audio thread.
//
// HOW TO PORT AN ENGINE (the wavetable engine is the model: WavetableEngine.h)
//  1. Ids. The single filter's sixteen ids stay where they are (the engine's FilterBase). The
//     dual block needs nine more: VoiceFilterSection filterSection { FilterBase, DualBase };
//     with DualBase = FilterBase + 16 an id minus FilterBase is the section's offset (applyTo).
//     Check the engine's block for those nine ids first (factory presets may not use ids above
//     115: tests/factory_preset_coverage_tests.h; the dual block is not needed by a preset that
//     leaves filter B off).
//  2. Pages: appendPages(pages, FilterBase, DualBase) in place of appendPages(pages, FilterBase);
//     updateModeCell() already does both MODE cells. A page-count pin of the engine moves by one.
//  3. Destinations: addDualDests(list, pages, DualBase) after addLfoDests (BAL, FFM, OFSB, RESB,
//     DRVB; on a mod::DestTable the first three are PerSample). On the router, addRouterInputs(
//     table) declares CUT (octaves, PerSample) and RESO (an offset for both filters).
//  4. The voice: filter.process(in, fm) where it called filter.process(in); `fm` is the signal
//     the engine offers as the modulator, -1 .. 1 (its main oscillator before the filter; one
//     the filter does not also filter gives the cleaner sidebands). Not read while FFM is 0.
//     A voice on the router: per sample, where it has a buffer, setLfoOctaves(cut[i]),
//     setBalanceNow(bal[i]), setFmNow(ffm[i]), setOffsetBNow(ofsb[i]); the control-rate ones
//     through applyTo() from forEachControl, as the macro oscillator does for the single filter.
//  5. setParameter: `if (filterSection.swaps(id, value)) soundSwapped();` covers TYPB, MODB and
//     ROUT. Nothing else changes: set / get / name / applyAll / each are as they were.
//  6. When the engine moves onto the router (U4): the four amounts KTRK, VEL, EVEL and WHL become
//     default slots (KEY > CUT, VEL > CUT, FENV via VEL > CUT, STRP > CUT; ENV stays a cell, the
//     amount of FENV > CUT), the voice calls tickEnvelope() and processOctaves() with the
//     router's CUT buffer, and the pages are makePages(base, dualBase, Routed): TRKB takes a
//     freed cell and F ENV has three cells left for the engine (the unified voice: PMOD).
//     When the last engine has moved, process(in, fm), the four amounts, setExternalOctaves and
//     the Cells layout are deleted.
//  What is special per engine: FM - `fm` is the sum of the carriers; macro oscillator - its own
//  MORP is a macro, so the filter's stays "FMOR" in the table; wave sequencer - `fm` is the wave
//  mix before the filter; sampler - it has its own filter (CUT 0..1, not this class): moving it
//  here is its U4 port, and `fm` is the sample read; RSN - `fm` is the exciter, so the body is
//  not what moves its own filter; drum kit - no voice filter of this kind; unified voice -
//  processOctaves with its own sum under the pin and its notes through noteOnKeepingState(),
//  OFSB / TRKB / BAL / FFM map one to one (ids FILTER_B_OFFSET, B_TRACK, BALANCE, FILTER_FM), its
//  oscillator 2 is `fm`, its Hz offset from the old mod matrix (modCutoffOffset) and its envelope
//  chain on the cutoff have no place here and become octaves.
// -----------------------------------------------------------------------------

// One voice's filter.
class VoiceFilter
{
public:
    static constexpr float ENV_OCTAVES = 7.0f;       // envelope amount of 1 = this many octaves
    static constexpr float VEL_OCTAVES = 4.0f;
    static constexpr float WHEEL_OCTAVES = 5.0f;
    static constexpr float PRESSURE_OCTAVES = 4.0f;
    static constexpr float FM_OCTAVES = 2.0f;        // FFM of 1 with a full-level modulator
    static constexpr float MAX_OCTAVES = 32.0f;      // what a sum of octaves is held to
    static constexpr float MAX_HZ = 20000.0f;
    static constexpr float MIN_HZ = 20.0f;

    VoiceFilter()
    {
        slot.setCutoff(0, baseTarget);
        slot.setResonance(0, 0.0f);
    }

    void setSampleRate(double sr)
    {
        sampleRate = (float) sr;
        maxHz = std::min(MAX_HZ, sampleRate * 0.49f);
        smoothCoeff = 1.0f - std::exp(-1.0f / (0.005f * sampleRate));   // 5 ms
        octaveStep = 8.0f / (0.004f * sampleRate);                      // 8 octaves in 4 ms
        driveStep = 1.0f / (0.004f * sampleRate);
        env.setSampleRate(sr);
        slot.setSampleRate(sr);
        lastHz = -1.0f;
        lastHzB = -1.0f;
    }

    // voiceWasSounding: a retriggered voice keeps its filter state (no click); an idle one starts clean.
    // spreadOctaves: this note's own cutoff error (VoiceSpread, FVAR); 0 = none.
    void noteOn(int note, float velocity, bool voiceWasSounding, float spreadOctaves = 0.0f)
    {
        if (! voiceWasSounding)
        {
            slot.reset();
            env.reset();
            baseSmoothed = baseTarget;
        }
        currentNote = note;
        currentVelocity = velocity;
        noteSpread = std::isfinite(spreadOctaves) ? spreadOctaves : 0.0f;
        updateNoteOctaves();
        // The new note takes its own values at once, as they stand at its first sample (a voice
        // struck again is declicked by its owner).
        struck = true;
        fresh = true;
        plain = false;
        env.noteOn();
    }

    // A note on a voice that never clears its filter between notes (the unified voice, under the
    // P01 pin): the filters' state and the envelope's level go on from where they are, and every
    // value, the cutoff too, is taken as it stands. (noteOn(.., true) alone lets a cutoff that was
    // set between two notes glide into the new one, as the single filter always did.)
    void noteOnKeepingState(int note, float velocity, float spreadOctaves = 0.0f)
    {
        noteOn(note, velocity, true, spreadOctaves);
        baseSmoothed = baseTarget;
    }

    void noteOff() { env.noteOff(); }

    void reset()
    {
        slot.reset();
        env.reset();
        baseSmoothed = baseTarget;
        struck = false;
        envOctavesNow = envOctaves;
        noteOctavesNow = noteOctaves;
        externalNow = externalOctaves;
        driveNow = driveTarget;
        slot.setDrive(0, driveNow);
        arrive();
    }

    // One sample, the section adding the octaves up from its own cells. `fm`: the audio-rate
    // signal that moves the cutoff by FFM (-1 .. 1; not read while FFM is 0).
    float process(float in, float fm = 0.0f)
    {
        if (! plain)
            return processFull(in, fm);

        // Filter A alone, nothing led, no FM, not a note's first sample: the single filter, at
        // (a little under) the single filter's cost. One flag says so.
        const float e = env.process();
        glideSet();
        const float oct = e * envOctavesNow + noteOctavesNow + externalNow + lfoOctaves;
        float hz = baseSmoothed;
        if (oct != 0.0f)
            hz *= std::exp2(oct);
        hz = held(hz, MIN_HZ, maxHz);
        if (hz != lastHz)
        {
            slot.setCutoff(0, hz);
            lastHz = hz;
        }
        return slot.process(in);
    }

    // The same with the octaves added up by the caller (the router's CUT buffer, or a voice's
    // own sum): tickEnvelope() first where the caller wants the filter envelope in its sum.
    float tickEnvelope() { return env.process(); }
    float processOctaves(float in, float octaves, float fm = 0.0f)
    {
        if (bypassed)
            return in;
        glide();
        return run(in, octaves, fm);
    }

    // --- settings (the same values go to every voice of an engine) ---
    void setModel(FilterModel m)
    {
        slot.setModel(0, m);
        lastHz = -1.0f;
        topology();
    }
    void setMode(int m)             { slot.setMode(0, m); }
    void setModelB(FilterModel m)
    {
        slot.setModel(1, m);
        topology();
    }
    void setModeB(int m)            { slot.setMode(1, m); }
    void setRouting(FilterRouting r)
    {
        slot.setRouting(r);
        topology();
    }
    void setCutoff(float hz)        { baseTarget = held(hz, MIN_HZ, MAX_HZ); }
    void setResonance(float r)      { if (lead(resonance, resonanceNow, held(r, 0.0f, 1.0f))) applyResonance(); }
    void setResonanceB(float r)     { if (lead(resonanceB, resonanceBNow, held(r, 0.0f, 1.0f))) applyResonance(); }
    void setDrive(float d)
    {
        driveTarget = held(d, 0.0f, 1.0f);
        if (! struck)       // nothing sounding yet: as set (process() glides to it under a note)
        {
            driveNow = driveTarget;
            slot.setDrive(0, driveNow);
        }
    }
    void setDriveB(float d)         { if (lead(driveB, driveBNow, held(d, 0.0f, 1.0f))) slot.setDrive(1, driveBNow); }
    // Parallel routing: 0 = filter A only, 1 = filter B only.
    void setBalance(float b)        { if (lead(balance, balanceNow, held(b, 0.0f, 1.0f))) slot.setBalance(balanceNow); }
    // How far the signal handed to process() moves the cutoff: 0 .. 1 (x FM_OCTAVES).
    void setFmDepth(float d)        { lead(fmDepth, fmNow, held(d, 0.0f, 1.0f)); }
    // Filter B's cutoff in semitones from filter A's, and how much of A's modulation it follows.
    void setOffsetB(float semitones)
    {
        if (lead(offsetB, offsetBNow, held(semitones, -96.0f, 96.0f)))
            ratioB = std::exp2(offsetBNow / 12.0f);
    }
    void setTrackB(float t)         { lead(trackB, trackBNow, held(t, 0.0f, 1.0f)); }
    void setMorph(float m)          { slot.setMorph(held(m, 0.0f, 1.0f)); }
    void setEnvAmount(float a)      { envAmount = held(a, -1.0f, 1.0f); updateNoteOctaves(); }
    void setEnvVelocity(float a)    { envVelocity = held(a, 0.0f, 1.0f); updateNoteOctaves(); }
    void setKeyTrack(float a)       { keyTrack = held(a, -1.0f, 1.0f); updateNoteOctaves(); }
    void setVelAmount(float a)      { velAmount = held(a, 0.0f, 1.0f); updateNoteOctaves(); }

    // The filter envelope as set. The note's own time scale (VoiceSpread, EVAR) is applied to
    // what is set here, never to the envelope in place: 1 leaves the envelope exactly as set.
    void setEnvParams(const Envelope::Params& p)
    {
        envBase = p;
        env.setParams(timeScale == 1.0f ? p : spread::scaleTimes(p, timeScale));
    }
    void setEnvTimeScale(float scale)
    {
        scale = std::isfinite(scale) ? held(scale, 0.125f, 8.0f) : 1.0f;
        if (scale == timeScale)
            return;
        timeScale = scale;
        env.setParams(spread::scaleTimes(envBase, timeScale));
    }
    // One stage of it under a running note (an LFO or a slot on FATK ...).
    void setEnvAttack(float v)      { envBase.attack = v; env.setAttack(v * timeScale); }
    void setEnvDecay(float v)       { envBase.decay = v; env.setDecay(v * timeScale); }
    void setEnvSustain(float v)     { envBase.sustain = v; env.setSustain(v); }
    void setEnvRelease(float v)     { envBase.release = v; env.setRelease(v * timeScale); }

    // Mod strip and pad pressure, already in octaves; set once per block by the engine.
    void setExternalOctaves(float oct) { externalOctaves = oct; }

    // Modulation from the voice's LFOs or the router, set per sample by the voice while
    // something is routed here and back to zero when nothing is: the cutoff in octaves, the
    // resonance (of both filters) as an offset from its set value. Zero leaves the filter exactly
    // as it is without them.
    void setLfoOctaves(float oct) { lfoOctaves = oct; }
    void setResonanceMod(float offset)
    {
        if (offset == resonanceMod)
            return;
        resonanceMod = offset;
        applyResonance();
    }
    // The router's per-sample values of the three cells it can ramp or move at audio rate: final
    // values (the cell with its modulation), taken as they are.
    void setBalanceNow(float b)
    {
        balance = balanceNow = held(b, 0.0f, 1.0f);
        slot.setBalance(balanceNow);
    }
    void setFmNow(float d)          { fmDepth = fmNow = held(d, 0.0f, 1.0f); rate(); }
    void setOffsetBNow(float semitones)
    {
        semitones = held(semitones, -96.0f, 96.0f);
        if (semitones == offsetBNow && semitones == offsetB)
            return;
        offsetB = offsetBNow = semitones;
        ratioB = std::exp2(offsetBNow / 12.0f);
    }

    FilterSlot& getSlot() { return slot; }
    Envelope& getEnvelope() { return env; }
    float getCurrentCutoff() const { return lastHz; }
    float getCurrentCutoffB() const { return lastHzB; }
    // True while filter B is in the path (ROUT is not SNGL and its type is not OFF).
    bool isDual() const { return dual; }

private:
    // std::clamp for a value that may be anything: what is not a number becomes `lo` (std::clamp
    // hands it on, and one such cutoff is a filter that never speaks again).
    static float held(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

    // A cell set from outside: taken at once while nothing sounds, moved to under a note.
    // True when it is there already (the caller then hands it to the filters).
    bool lead(float& target, float& now, float value)
    {
        target = value;
        if (! struck)
            now = value;
        else if (now != value)
            moving = true;
        rate();
        return now == value;
    }

    // plain: this sample is filter A alone with nothing to lead, no FM and no note starting
    // (see process()). Everything else goes through processFull().
    void rate() { plain = ! (moving || dual || fmNow != 0.0f || bypassed || fresh); }

    void applyResonance()
    {
        slot.setResonance(0, held(resonanceNow + resonanceMod, 0.0f, 1.0f));
        slot.setResonance(1, held(resonanceBNow + resonanceMod, 0.0f, 1.0f));
    }

    // Everything led is where it was set (a note's first sample, a reset).
    __attribute__ ((noinline)) void arrive()
    {
        moving = false;
        if (resonanceNow != resonance || resonanceBNow != resonanceB)
        {
            resonanceNow = resonance;
            resonanceBNow = resonanceB;
            applyResonance();
        }
        if (driveBNow != driveB)        { driveBNow = driveB; slot.setDrive(1, driveBNow); }
        if (balanceNow != balance)      { balanceNow = balance; slot.setBalance(balanceNow); }
        if (offsetBNow != offsetB)      { offsetBNow = offsetB; ratioB = std::exp2(offsetBNow / 12.0f); }
        fmNow = fmDepth;
        trackBNow = trackB;
        rate();
    }

    // One sample on the way there.
    void follow()
    {
        if (resonanceNow != resonance || resonanceBNow != resonanceB)
        {
            resonanceNow = dsp::towards(resonanceNow, resonance, driveStep);
            resonanceBNow = dsp::towards(resonanceBNow, resonanceB, driveStep);
            applyResonance();
        }
        if (driveBNow != driveB)        { driveBNow = dsp::towards(driveBNow, driveB, driveStep); slot.setDrive(1, driveBNow); }
        if (balanceNow != balance)      { balanceNow = dsp::towards(balanceNow, balance, driveStep); slot.setBalance(balanceNow); }
        if (offsetBNow != offsetB)
        {
            offsetBNow = dsp::towards(offsetBNow, offsetB, 3.0f * octaveStep);     // two octaves in 4 ms
            ratioB = std::exp2(offsetBNow / 12.0f);
        }
        if (fmNow != fmDepth)           fmNow = dsp::towards(fmNow, fmDepth, driveStep);
        if (trackBNow != trackB)        trackBNow = dsp::towards(trackBNow, trackB, driveStep);
        moving = resonanceNow != resonance || resonanceBNow != resonanceB || driveBNow != driveB || balanceNow != balance
                 || offsetBNow != offsetB || fmNow != fmDepth || trackBNow != trackB;
        rate();
    }

    void topology()
    {
        const bool was = dual;
        dual = slot.getModelB() != FilterModel::Bypass && slot.getRouting() != FilterRouting::Single;
        // (Filter A off and B out of the path: PAR still mixes by BAL, the unfiltered sound with itself.)
        bypassed = slot.getModelA() == FilterModel::Bypass && ! dual;
        if (dual && ! was)
            lastHzB = -1.0f;
        rate();
    }

    // A note's first sample (after the cutoff's own glide, as the single filter always had it):
    // everything is where it was set.
    void glide()
    {
        glideCutoff();
        if (fresh)
        {
            fresh = false;
            envOctavesNow = envOctaves;
            noteOctavesNow = noteOctaves;
            externalNow = externalOctaves;
            if (driveNow != driveTarget)
            {
                driveNow = driveTarget;
                slot.setDrive(0, driveNow);
            }
            arrive();
        }
        glideAmounts();
    }
    forcedinline void glideSet()
    {
        glideCutoff();
        glideAmounts();
    }
    forcedinline void glideCutoff()
    {
        if (baseSmoothed != baseTarget)
        {
            const float d = baseTarget - baseSmoothed;
            baseSmoothed += d * smoothCoeff;
            if (std::abs(d) < baseTarget * 1.0e-4f)
                baseSmoothed = baseTarget;
        }
    }
    // What is set from outside under a sounding note (the envelope amount by a step lock,
    // strip and pressure once per block, the drive) is moved to over a few milliseconds:
    // opening or closing a filter in one sample is a click. The envelope itself is not held.
    forcedinline void glideAmounts()
    {
        if (envOctavesNow != envOctaves)    envOctavesNow = dsp::towards(envOctavesNow, envOctaves, octaveStep);
        if (noteOctavesNow != noteOctaves)  noteOctavesNow = dsp::towards(noteOctavesNow, noteOctaves, octaveStep);
        if (externalNow != externalOctaves) externalNow = dsp::towards(externalNow, externalOctaves, octaveStep);
        if (driveNow != driveTarget)
        {
            driveNow = dsp::towards(driveNow, driveTarget, driveStep);
            slot.setDrive(0, driveNow);
        }
    }

    // process() when the sample is not a plain one. (Functions of their own: inside process()
    // they cost the plain path what the compiler keeps in memory around them.)
    __attribute__ ((noinline)) float processFull(float in, float fm)
    {
        if (bypassed)
            return in;
        const float e = env.process();
        glide();
        return run(in, e * envOctavesNow + noteOctavesNow + externalNow + lfoOctaves, fm);
    }

    // From the sum of octaves on, with everything the section can do: what is led moves on, the
    // FM, the two cutoffs, then the filters. With nothing led, no FM and filter A alone it is
    // process()'s plain path, sample for sample.
    __attribute__ ((noinline)) float run(float in, float oct, float fm)
    {
        if (moving)
            follow();
        if (fmNow != 0.0f)
            oct = fmNow * (std::isfinite(fm) ? std::clamp(fm, -4.0f, 4.0f) : 0.0f) * FM_OCTAVES + oct;
        if (! (std::abs(oct) <= MAX_OCTAVES))       // (also what is not a number)
            oct = oct > 0.0f ? MAX_OCTAVES : (oct < 0.0f ? -MAX_OCTAVES : 0.0f);

        const float ratio = oct != 0.0f ? std::exp2(oct) : 1.0f;
        const float hz = held(baseSmoothed * ratio, MIN_HZ, maxHz);
        if (hz != lastHz)
        {
            slot.setCutoff(0, hz);
            lastHz = hz;
        }
        if (dual)
        {
            // Filter B follows that modulation fully, partly or not at all, from its own offset.
            const float ratioOfB = trackBNow == 1.0f || oct == 0.0f ? ratio : std::exp2(oct * trackBNow);
            const float hzB = held(baseSmoothed * ratioOfB * ratioB, MIN_HZ, maxHz);
            if (hzB != lastHzB)
            {
                slot.setCutoff(1, hzB);
                lastHzB = hzB;
            }
        }
        return slot.process(in);
    }

    void updateNoteOctaves()
    {
        noteOctaves = keyTrack * (float) (currentNote - 60) / 12.0f + velAmount * currentVelocity * VEL_OCTAVES;
        if (noteSpread != 0.0f)
            noteOctaves += noteSpread;
        envOctaves = envAmount * (1.0f - envVelocity + envVelocity * currentVelocity) * ENV_OCTAVES;
    }

    FilterSlot slot;
    Envelope env;
    Envelope::Params envBase;       // the envelope as set (env holds it with the note's time scale)
    float timeScale = 1.0f;
    float envOctavesNow = 0.0f, noteOctavesNow = 0.0f, externalNow = 0.0f, octaveStep = 0.04f;
    float driveTarget = 0.0f, driveNow = 0.0f, driveStep = 0.005f;
    bool struck = false, fresh = true;
    float sampleRate = 44100.0f;
    float maxHz = MAX_HZ;
    float smoothCoeff = 0.0045f;
    float baseTarget = MAX_HZ;
    float baseSmoothed = MAX_HZ;
    float lastHz = -1.0f, lastHzB = -1.0f;
    float envAmount = 0.0f, envVelocity = 0.0f, keyTrack = 0.0f, velAmount = 0.0f, noteSpread = 0.0f;
    float noteOctaves = 0.0f, envOctaves = 0.0f, externalOctaves = 0.0f, lfoOctaves = 0.0f;
    float resonance = 0.0f, resonanceNow = 0.0f, resonanceMod = 0.0f;
    // The second filter and what joins the two: as set, and as heard
    float resonanceB = 0.0f, resonanceBNow = 0.0f, driveB = 0.0f, driveBNow = 0.0f;
    float balance = 0.5f, balanceNow = 0.5f, fmDepth = 0.0f, fmNow = 0.0f;
    float offsetB = 0.0f, offsetBNow = 0.0f, ratioB = 1.0f, trackB = 1.0f, trackBNow = 1.0f;
    bool moving = false, dual = false, plain = false;
    int currentNote = 60;
    float currentVelocity = 1.0f;
    bool bypassed = false;
};

// The engine side: parameter ids (base + offset), page descriptions, true values, and applying a
// change to every voice's filter.
class VoiceFilterSection
{
public:
    enum Offset
    {
        Type = 0,       // index into FilterSlot's display order
        Mode,           // mode within the type
        Cutoff,         // Hz
        Resonance,      // 0..1
        EnvAmount,      // -1..1 (x 7 octaves)
        KeyTrack,       // -1..1 (1 = cutoff follows the keyboard exactly, -1 = falls as the notes rise)
        Drive,          // 0..1
        VelAmount,      // velocity -> cutoff, 0..1 (x 4 octaves)
        Attack,         // filter envelope, seconds
        Decay,
        Sustain,        // 0..1
        Release,
        Curve,          // -1..1, all three envelope stages
        Morph,          // SEM: low-pass -> notch -> high-pass
        EnvVelocity,    // how much velocity scales the envelope amount, 0..1
        WheelAmount,    // mod strip -> cutoff, -1..1 (x 5 octaves)
        Count,          // the single filter: sixteen ids from the base
        // The dual block: nine ids from the dual base (an engine without one has filter A only).
        TypeB = Count,  // index into FilterSlot's display order
        ModeB,
        OffsetB,        // semitones from filter A's cutoff, -48..48
        ResonanceB,     // 0..1
        DriveB,         // 0..1
        Routing,        // FilterRouting: SNGL / SER / PAR
        Balance,        // PAR: 0 = A, 1 = B
        FilterFM,       // the voice's oscillator -> cutoff, 0..1 (x 2 octaves)
        TrackB,         // how much of A's cutoff modulation B follows, 0..1 (a cell in the Routed layout)
        DualCount
    };
    static constexpr int NUM_DUAL = (int) DualCount - (int) Count;

    // Which cells the pages show. Cells: the four fixed amounts (KTRK, EVEL, VEL, WHL) are cells
    // (an engine that is not on the modulation router). Routed: they are the router's default
    // slots and their places go to FFM and TRKB; F ENV keeps three cells free for the engine.
    enum Layout { Cells, Routed };

    explicit VoiceFilterSection(int baseParamId, int dualBaseParamId = -1) : base(baseParamId), dualBase(dualBaseParamId)
    {
        for (auto& page : makePages(base))
            for (auto& p : page.params)
                values[(size_t) (p.id - base)] = p.defaultVal;
        values[Balance] = 0.5f;
        values[TrackB] = 1.0f;
    }

    bool hasDual() const { return dualBase >= 0; }
    int id(Offset o) const { return (int) o < (int) Count ? base + (int) o : dualBase + (int) o - (int) Count; }
    // The offset of a cell id; -1 for an id that is not the section's.
    int offsetOf(int paramId) const
    {
        if (paramId >= base && paramId < base + (int) Count)
            return paramId - base;
        if (hasDual() && paramId >= dualBase && paramId < dualBase + NUM_DUAL)
            return (int) Count + paramId - dualBase;
        return -1;
    }
    // True when setting paramId to value changes a filter's type or mode or the routing (a jump
    // in the sound of a note that is held: EngineBase::soundSwapped).
    bool swaps(int paramId, float value) const
    {
        const int o = offsetOf(paramId);
        return (o == (int) Type || o == (int) Mode || o == (int) TypeB || o == (int) ModeB || o == (int) Routing)
               && (int) std::lround(value) != (int) std::lround(values[(size_t) o]);
    }
    bool owns(int paramId) const { return offsetOf(paramId) >= 0 || isStageCurve(paramId); }
    float get(int paramId) const
    {
        if (isStageCurve(paramId))
            return stageCurves.get(paramId);
        const int o = offsetOf(paramId);
        return o >= 0 ? values[(size_t) o] : 0.0f;
    }

    // The filter envelope's own curve per stage (StageCurveParam slot FILTER_ENV; Shift + turn on
    // ATK / DEC / REL). They add to CURV.
    static bool isStageCurve(int paramId)
    {
        return StageCurveParam::owns(paramId) && StageCurveParam::envelopeOf(paramId) == StageCurveParam::FILTER_ENV;
    }
    // The stage-curve id behind the F ENV page's ATK, DEC or REL cell; -1 for any other id.
    static int stageCurveParamFor(int baseId, int cellId)
    {
        if (cellId == baseId + (int) Attack)  return StageCurveParam::id(StageCurveParam::FILTER_ENV, StageCurveParam::Attack);
        if (cellId == baseId + (int) Decay)   return StageCurveParam::id(StageCurveParam::FILTER_ENV, StageCurveParam::Decay);
        if (cellId == baseId + (int) Release) return StageCurveParam::id(StageCurveParam::FILTER_ENV, StageCurveParam::Release);
        return -1;
    }
    int stageCurveParamFor(int cellId) const { return stageCurveParamFor(base, cellId); }
    float wheelAmount() const { return values[WheelAmount]; }

    // Cutoff offset from the mod strip (0..1) and the pressure routing's cutoff amount, in octaves.
    float externalOctaves(float wheel, float pressureCutoffMod) const
    {
        return wheel * values[WheelAmount] * VoiceFilter::WHEEL_OCTAVES
             + pressureCutoffMod * VoiceFilter::PRESSURE_OCTAVES;
    }

    // The FILTR and F ENV pages for an engine whose filter ids start at baseId (filter A only).
    static std::array<ParamPage, 2> makePages(int baseId)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto C = ParamKind::Choice;
        const int b = baseId;
        std::array<ParamPage, 2> out;
        out[0].name = "FILTR"; out[0].section = "FLTR";
        out[0].params = {{
            { "CUT",  b + Cutoff, 20, 20000, 20000, G, "Hz" },
            { "RESO", b + Resonance, 0, 1, 0, L },
            { "ENV",  b + EnvAmount, -1, 1, 0, L },
            { "DRIV", b + Drive, 0, 1, 0, L },
            { "TYPE", b + Type, 0, FilterSlot::NUM_MODELS - 1, 0, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
            { "MODE", b + Mode, 0, 3, 0, C, "", FilterSlot::modeNames(FilterModel::SVF), 4 },
            { "MORP", b + Morph, 0, 1, 0, L },
            { "KTRK", b + KeyTrack, -1, 1, 0, L } }};
        out[1].name = "F ENV"; out[1].section = "FLTR";
        out[1].params = {{
            { "ATK",  b + Attack, 0.001f, 10, 0.01f, G, "s" },
            { "DEC",  b + Decay, 0.001f, 10, 0.5f, G, "s" },
            { "SUS",  b + Sustain, 0, 1, 0, L },
            { "REL",  b + Release, 0.001f, 10, 0.3f, G, "s" },
            { "CURV", b + Curve, -1, 1, 0, L },
            { "EVEL", b + EnvVelocity, 0, 1, 0, L },
            { "VEL",  b + VelAmount, 0, 1, 0, L },
            { "WHL",  b + WheelAmount, -1, 1, 0, L } }};
        return out;
    }

    // FILTR, FLT B and F ENV for an engine with the dual block.
    static std::array<ParamPage, 3> makePages(int baseId, int dualBaseId, Layout layout = Cells)
    {
        auto L = ParamKind::Linear; auto C = ParamKind::Choice;
        static const char* const routes[] = { "SNGL", "SER", "PAR" };
        const int off = FilterSlot::NUM_MODELS - 1;
        const auto single = makePages(baseId);
        const auto cell = [dualBaseId](Offset o) { return dualBaseId + (int) o - (int) Count; };
        const ParamSpec ffm  { "FFM",  cell(FilterFM), 0, 1, 0, L };
        const ParamSpec trkb { "TRKB", cell(TrackB), 0, 1, 1, L };

        std::array<ParamPage, 3> out;
        out[0] = single[0];
        out[1].name = "FLT B"; out[1].section = "FLTR";
        out[1].params = {{
            { "OFSB", cell(OffsetB), -48, 48, 0, L, "st" },
            { "RESB", cell(ResonanceB), 0, 1, 0, L },
            layout == Routed ? trkb : ffm,
            { "DRVB", cell(DriveB), 0, 1, 0, L },
            { "TYPB", cell(TypeB), 0, (float) off, 0, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
            { "MODB", cell(ModeB), 0, 3, 0, C, "", FilterSlot::modeNames(FilterModel::SVF), 4 },
            { "ROUT", cell(Routing), 0, 2, 0, C, "", routes, 3 },
            { "BAL",  cell(Balance), 0, 1, 0.5f, L } }};
        out[2] = single[1];
        if (layout == Routed)
        {
            out[0].params[7] = ffm;                 // KTRK is a default slot (KEY > CUT)
            for (size_t i = 5; i < 8; ++i)          // EVEL, VEL and WHL too
                out[2].params[i] = {};
        }
        return out;
    }

    static void appendPages(ParamPages& pages, int baseId)
    {
        for (auto& p : makePages(baseId))
            pages.push_back(p);
    }
    static void appendPages(ParamPages& pages, int baseId, int dualBaseId, Layout layout = Cells)
    {
        for (auto& p : makePages(baseId, dualBaseId, layout))
            pages.push_back(p);
    }

    // ---- LFO destinations (EngineLfoSection) ------------------------------------------------
    // CUT and RESO are in every engine's original list. These are the filter's other continuous
    // cells; the names say "filter" where the cell's own name is used elsewhere in an engine.
    // (Not offered: TYPE and MODE are choices, CURV would re-bend the envelope every sample, WHL
    // is the mod strip's own amount.)
    // morphLabel: for an engine whose own list already has a MORP (the macro oscillator).
    // (List: LfoDestList, or the modulation router's mod::DestTable.)
    template <typename List>
    static void addLfoDests(List& l, const ParamPages& pages, int baseId, const char* morphLabel = nullptr)
    {
        l.group();
        l.add(pages, baseId + EnvAmount);
        l.add(pages, baseId + Drive);
        l.add(pages, baseId + Morph, morphLabel);
        l.add(pages, baseId + KeyTrack);
        l.group();
        l.add(pages, baseId + Attack, "FATK");
        l.add(pages, baseId + Decay, "FDEC");
        l.add(pages, baseId + Sustain, "FSUS");
        l.add(pages, baseId + Release, "FREL");
        l.add(pages, baseId + EnvVelocity);
        l.add(pages, baseId + VelAmount, "FVEL");
    }

    // The dual block's continuous cells, as a group of their own: BAL, FFM and OFSB (taken per
    // sample on the router: a ramp, or the track input at audio rate), RESB and DRVB. A cell the
    // pages do not show (TRKB in the Cells layout) is left out.
    template <typename List>
    static void addDualDests(List& l, const ParamPages& pages, int dualBaseId)
    {
        l.group();
        for (Offset o : { Balance, FilterFM, OffsetB })
            addDest(l, pages, dualBaseId + (int) o - (int) Count, true);
        for (Offset o : { ResonanceB, DriveB, TrackB })
            addDest(l, pages, dualBaseId + (int) o - (int) Count, false);
    }

    // The two a voice takes as pure modulation, for an engine's router table: CUT in octaves
    // (4 at full depth, per sample: the track input on it is filter FM) and RESO as an offset
    // (+-0.5) for both filters. VoiceFilter::setLfoOctaves / setResonanceMod take them.
    static void addRouterInputs(mod::DestTable& t)
    {
        t.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, mod::PerSample);
        t.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
    }

    // One voice's filter takes `value` for the cell at `offset` (the per-voice half of set(), and
    // what an LFO or a slot on a cell of addLfoDests / addDualDests calls; range-checked by the
    // caller or by the filter's own setters).
    static void applyTo(VoiceFilter& f, int offset, float value)
    {
        switch (offset)
        {
            case Cutoff:      f.setCutoff(value); break;
            case Resonance:   f.setResonance(value); break;
            case EnvAmount:   f.setEnvAmount(value); break;
            case Drive:       f.setDrive(value); break;
            case Morph:       f.setMorph(value); break;
            case KeyTrack:    f.setKeyTrack(value); break;
            case Attack:      f.setEnvAttack(value); break;
            case Decay:       f.setEnvDecay(value); break;
            case Sustain:     f.setEnvSustain(value); break;
            case Release:     f.setEnvRelease(value); break;
            case EnvVelocity: f.setEnvVelocity(value); break;
            case VelAmount:   f.setVelAmount(value); break;
            case OffsetB:     f.setOffsetB(value); break;
            case ResonanceB:  f.setResonanceB(value); break;
            case DriveB:      f.setDriveB(value); break;
            case Balance:     f.setBalance(value); break;
            case FilterFM:    f.setFmDepth(value); break;
            case TrackB:      f.setTrackB(value); break;
            default: break;
        }
    }

    // The MODE cells offer the modes of the filter type currently chosen.
    void updateModeCell(ParamPages& pages) const
    {
        for (auto& page : pages)
            for (auto& p : page.params)
            {
                const bool a = p.id == base + (int) Mode, b = hasDual() && p.id == id(ModeB);
                if (! a && ! b)
                    continue;
                const auto model = a ? currentModel() : currentModelB();
                p.choices = FilterSlot::modeNames(model);
                p.numChoices = FilterSlot::modeCount(model);
                p.maxVal = (float) (p.numChoices - 1);
            }
    }

    FilterModel currentModel() const { return FilterSlot::modelFromIndex((int) values[Type]); }
    FilterModel currentModelB() const { return FilterSlot::modelFromIndex((int) values[TypeB]); }

    // Stores the (range-checked) value and applies it. forEachFilter(fn) must call fn(VoiceFilter&)
    // for every voice of the engine.
    template <typename ForEachFilter>
    void set(int paramId, float value, const ForEachFilter& forEachFilter)
    {
        if (! owns(paramId) || ! std::isfinite(value))      // (what is not a number is not a setting)
            return;
        const int o = isStageCurve(paramId) ? -1 : offsetOf(paramId);
        stageCurves.set(paramId, value);       // ignores every other id

        switch (o)
        {
            case Type:
            case TypeB:
            {
                const Offset modeOf = o == Type ? Mode : ModeB;
                value = (float) std::clamp((int) std::lround(value), 0, FilterSlot::NUM_MODELS - 1);
                values[(size_t) o] = value;
                const auto model = o == Type ? currentModel() : currentModelB();
                // The stored mode always names a mode the new type has.
                values[modeOf] = std::min(values[modeOf], (float) (FilterSlot::modeCount(model) - 1));
                const int mode = (int) values[modeOf];
                if (o == Type)
                    forEachFilter([model, mode](VoiceFilter& f) { f.setModel(model); f.setMode(mode); });
                else
                    forEachFilter([model, mode](VoiceFilter& f) { f.setModelB(model); f.setModeB(mode); });
                return;
            }
            case Mode:
            {
                value = (float) std::clamp((int) std::lround(value), 0, FilterSlot::modeCount(currentModel()) - 1);
                values[Mode] = value;
                const int mode = (int) value;
                forEachFilter([mode](VoiceFilter& f) { f.setMode(mode); });
                return;
            }
            case ModeB:
            {
                value = (float) std::clamp((int) std::lround(value), 0, FilterSlot::modeCount(currentModelB()) - 1);
                values[ModeB] = value;
                const int mode = (int) value;
                forEachFilter([mode](VoiceFilter& f) { f.setModeB(mode); });
                return;
            }
            case Routing:
            {
                const int r = std::clamp((int) std::lround(value), 0, 2);
                values[Routing] = (float) r;
                forEachFilter([r](VoiceFilter& f) { f.setRouting(static_cast<FilterRouting>(r)); });
                return;
            }
            case Cutoff: case Resonance: case EnvAmount: case KeyTrack: case Drive: case VelAmount: case Morph:
            case EnvVelocity: case OffsetB: case ResonanceB: case DriveB: case Balance: case FilterFM: case TrackB:
            {
                // A continuous cell that every voice's filter takes as it is.
                const float lo = o == Cutoff ? VoiceFilter::MIN_HZ : (o == OffsetB ? -48.0f : (o == EnvAmount || o == KeyTrack ? -1.0f : 0.0f));
                const float hi = o == Cutoff ? VoiceFilter::MAX_HZ : (o == OffsetB ? 48.0f : 1.0f);
                value = std::clamp(value, lo, hi);
                values[(size_t) o] = value;
                forEachFilter([o, value](VoiceFilter& f) { applyTo(f, o, value); });
                return;
            }
            case WheelAmount:
                values[(size_t) o] = std::clamp(value, -1.0f, 1.0f);   // read once per block by the engine
                return;
            case Attack: case Decay: case Release:
                values[(size_t) o] = std::clamp(value, 0.001f, 10.0f);
                break;
            case Sustain:
                values[(size_t) o] = std::clamp(value, 0.0f, 1.0f);
                break;
            case Curve:
                values[(size_t) o] = std::clamp(value, -1.0f, 1.0f);
                break;
            case -1:        // a stage's own curve
                break;
            default:
                return;
        }

        // An envelope value changed.
        Envelope::Params p;
        p.attack = values[Attack];
        p.decay = values[Decay];
        p.sustain = values[Sustain];
        p.release = values[Release];
        stageCurves.applyTo(p, StageCurveParam::FILTER_ENV, values[Curve]);
        forEachFilter([&p](VoiceFilter& f) { f.setEnvParams(p); });
    }

    // Applies every stored value (used once, when the engine's voices exist).
    template <typename ForEachFilter>
    void applyAll(const ForEachFilter& forEachFilter)
    {
        // Type first: the mode is checked against it.
        set(base + (int) Type, values[Type], forEachFilter);
        for (int o = 1; o < (int) Count; ++o)
            set(base + o, values[(size_t) o], forEachFilter);
        if (hasDual())
            for (int o = (int) Count; o < (int) DualCount; ++o)     // (TYPB comes before MODB)
                set(id((Offset) o), values[(size_t) o], forEachFilter);
    }

    // The "every voice's filter" argument for set / applyAll, for a container of voices that each
    // have getFilter().
    template <typename Voices>
    static auto each(Voices& voices)
    {
        return [&voices](auto&& fn) { for (auto& v : voices) fn(v.getFilter()); };
    }

    static const char* name(int offset)
    {
        static const char* const names[DualCount] = {
            "Filter Type", "Filter Mode", "Cutoff", "Resonance", "Filter Env", "Key Track", "Filter Drive",
            "Vel > Cutoff", "F.Env Attack", "F.Env Decay", "F.Env Sustain", "F.Env Release", "F.Env Curve",
            "Filter Morph", "Vel > F.Env", "Wheel > Cutoff",
            "Filter B Type", "Filter B Mode", "Filter B Offset", "Filter B Resonance", "Filter B Drive",
            "Filter Routing", "Filter Balance", "Filter FM", "Filter B Track" };
        return (offset >= 0 && offset < (int) DualCount) ? names[offset] : "";
    }

private:
    // (LfoDestList has no rate flag; on the router's table a cell can be flagged PerSample.)
    static void addDest(LfoDestList& l, const ParamPages& pages, int cellId, bool)
    {
        if (onPages(pages, cellId))
            l.add(pages, cellId);
    }
    static void addDest(mod::DestTable& t, const ParamPages& pages, int cellId, bool perSample)
    {
        if (onPages(pages, cellId))
            t.add(pages, cellId, nullptr, perSample ? (int) mod::PerSample : (int) mod::Control);
    }
    static bool onPages(const ParamPages& pages, int cellId)
    {
        for (auto& page : pages)
            if (page.find(cellId) != nullptr)
                return true;
        return false;
    }

    int base, dualBase;
    std::array<float, DualCount> values {};
    StageCurves stageCurves;
};
