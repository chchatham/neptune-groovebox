#pragma once

#include "ParamSpec.h"
#include "EngineLfoSection.h"
#include "../dsp/DSPUtils.h"
#include "../dsp/Resonator.h"
#include <algorithm>
#include <array>
#include <cmath>

// -----------------------------------------------------------------------------
// BODY: a voice's own sound passed through a resonator (dsp/Resonator.h) tuned to
// its note - an FM voice, a wave sequence or a grain cloud as the exciter of its own
// string, plate or bell. A shared section, as VoiceFilterSection is: one VoiceBody in
// every voice, two pages, and the engine side that stores the cells.
//
//   BODY   MIX  MRPH DCAY COUP | DAMP POS  TUNE KTRK
//   BDY2   CONT GRAN REED HARD | RDAM KDCY DIRE WIRE
//
//   MIX   how much of the voice is the body's sound; 0 = off: the voice is not touched
//         and the body is not run (the default)
//   MRPH  what the body is: STRG TUBE COMB PLAT BAR BELL, 0..5 and anything between
//   DCAY  how long it rings (the time to fall by 60 dB)
//   COUP  how the voice's sound meets it. 0: as a force - the body is struck by every
//         sample of it and answers at its partials (a filter that rings). Raised, the
//         voice's level becomes a bow's pressure as well: the body is gripped and sings
//         by itself, for as long and as hard as the voice sounds (the exciter's COUP
//         latent; the grip is at its best round 75, as on RSN's EXC page).
//   DAMP  added to the body's DAMP latent: how much sooner the high partials die
//   POS   added to its POS latent: where it is driven, from an end (every partial) to
//         the middle (the even ones are not reached)
//   TUNE  the body against the voice's note, semitones: a fifth, an octave, a beating unison
//   KTRK  how far the body follows the keys: 100 = on the note, 0 = a fixed body at
//         middle C (+ TUNE) that every note is played through
//
// Page 2 (BDY2): the exciter's character, release behavior, and the body's own sounds.
//   CONT  burst vs continuous: 0 = one impulse at note-on (the resonator rings freely),
//         1 = the voice feeds the body for as long as it sounds (the default, as before)
//   GRAN  grain density: 0 = none (the voice alone excites the body); raised, the
//         resonator generates its own sparse impulses, from 2 a second to dense noise at
//         1 (what DUST needs: the body is excited without the voice making clicks)
//   REED  friction curve: 0 = bow (stick and slip: holds while small, lets go when large),
//         1 = reed (an opening the difference closes). With COUP, what REED is here is
//         what it is on RSN's EXC page. Default 0 (bow, as before)
//   HARD  exciter brightness, offset from the auto value (which scales with COUP): -1 =
//         duller, 0 = as before, 1 = brighter. The two low-passes on the excitation move
//         from the note (at the dullest) to 128x it (at the brightest)
//   RDAM  release damping: how quickly the body dies when the voice stops. 0 = ring on
//         for the full DCAY (as before); 1 = killed at once (like a palm mute, or the
//         Karplus-Strong string's note-off behavior)
//   KDCY  key-dependent decay: how much the decay follows the pitch. 0 = no change (as
//         before); positive = higher notes decay faster (the string's natural behavior:
//         halved per octave at 1); negative = higher notes ring longer
//   DIRE  the exciter's own sound beside the body: the click of a strike, the hiss of
//         grains. At 0 (default) only the body is heard. 0..1
//   WIRE  something lying against the body that rattles with its motion: the snare wires
//         on a head, the sizzle of two plates. At 0 (default) nothing rattles. 0..1
//
// The body also follows what the engine says moves the voice's pitch as a whole (its TUNE,
// FINE and the bend: setPitchOffset), not a pitch LFO nor a sequence's own steps: those move
// the voice against its body.
//
// WHERE IT SITS: last in the voice, after the filter AND after the amp envelope. What comes
// before it is the excitation, and the filter and the envelope are what shape an excitation:
// a short envelope is a pluck, a closed filter a soft mallet, an open one a hard one. And a
// body must be able to ring after the voice has stopped driving it, which no stage in front of
// the amp envelope can. (A body in front of the filter would be one more oscillator colour,
// cut off with the note.) So a voice with a body is alive until its body has rung out:
// isActive() of the voice is "the source sounds or the body rings" (see the recipe).
//
// Level. A sound that sits exactly on the body's partials (the usual case: the voice is tuned
// to its own body) would build up by the body's Q, forty decibels and more. The drive is
// therefore eased as the body fills (1 / (1 + (held / FULL)^2), measured every few samples,
// moved to smoothly): what does not resonate passes as it is, what does settles a little
// above the level it came in at. Beyond that the resonator's own bounds hold (its ceiling on
// what the bank holds and the limit on its output, +-4).
//
// Cost: with MIX at 0 a voice pays one test of a flag a sample and nothing is run; with it up,
// the resonator's 64 modes a sample (fewer on high notes), and its control work when a cell,
// the pitch or the body moves. Numbers: tests/voice_body_tests.h ("Cost").
//
// ---- How to give an engine BODY (done for the wave sequencer: WaveSeqEngine.h) -------------
//  1. Voice: a member `VoiceBody body;`, `VoiceBody& getBody() { return body; }`,
//     body.setSampleRate(sr) in setSampleRate, body.noteOn(note) in the note-on (after the
//     note is known), body.reset() in reset().
//  2. Voice: rename the per-sample function (the one that returns the voice's finished sample,
//     envelope and level applied) to processSource() and wrap it:
//         float process() {
//             if (! body.engaged()) return processSource();              // exactly as before
//             return body.process(active ? processSource() : 0.0f, active);
//         }
//     and let the voice count as sounding while its body rings:
//         bool isActive() const { return active || body.ringing(); }
//     Inside the voice keep using the source's own flag (`active`) for "struck again while
//     sounding" and for the envelopes; only what the engine and the allocator ask (the loop
//     over voices, setVoiceSoundingQuery) reads isActive(). A stereo voice runs one body on
//     the sum (or two bodies: twice the cost) - decide per engine.
//  3. Engine: a member `VoiceBodySection bodySection;` (ids 133-148, free on every engine that
//     uses EngineLfoSection); in setParameter / getParameter, before the filter section's
//     test (the ids are above every FilterBase):
//         if (bodySection.owns(id)) { bodySection.set(id, value, VoiceBodySection::each(voices));
//                                     lfoSection.baseChanged(id, bodySection.get(id), EngineLfoSection::each(voices)); return; }
//     getParameterName: VoiceBodySection::name(id - VoiceBodySection::BASE). (A voice's body and
//     the section start from the same defaults: nothing has to be applied at construction.)
//  4. Pages: VoiceBodySection::appendPages(out) after the filter's pages and before the LFO
//     section's (an engine's last pages are the mod slots, STRIP, LFO1, LFO2).
//  5. Destinations: VoiceBodySection::addLfoDests(list, pages) at the END of the engine's
//     LfoDestList (stored DEST indices of everything before it stay what they were), and in
//     the engine's applyVoiceParam, first:
//         if (VoiceBodySection::ownsId(id)) { VoiceBodySection::applyTo(voice.getBody(), id - VoiceBodySection::BASE, value); return; }
//     That makes every cell a destination of both LFOs, the strip's routes and the mod slots.
//  6. Once a block, with the voice's other pitch offsets: body.setPitchOffset(tune + fine + bend).
//  7. Tests: the page count and the destination count of the engine are pinned in several
//     suites (grep the old numbers); the "Mod Slots" checksum lines of the engine with MIX at 0
//     must not change.
// -----------------------------------------------------------------------------

// One voice's body. No allocation; everything here is safe on the audio thread.
class VoiceBody
{
public:
    // Page 1: BODY (the original eight cells)
    // Page 2: BDY2 (the exciter's character, release behavior, body's own sounds)
    enum Cell { Mix = 0, Morph, Decay, Couple, Damp, Pos, Tune, KeyTrack,
                Continuous, Grain, Reed, Hard, ReleaseDamp, KeyDecay, Direct, Wire,
                NumCells };
    // The cells of the first page, whose indices are stored (a saved DEST) and must not move.
    static constexpr int PAGE1_CELLS = 8;

    static constexpr float FULL = 0.1f;         // what the bank holds (sum of squares) where the drive is eased to a half
    static constexpr float MAKEUP = 2.0f;       // the body's output against the voice's
    static constexpr float BOW = 3.0f;          // the voice's level (its peak follower) to the junction's energy
    static constexpr float SILENT = 1.0e-9f;    // a body that holds less has rung out (-90 dB)
    static constexpr int LOOK = 16;             // samples between two looks at what the body holds

    void setSampleRate(double sr)
    {
        const double rate = sr >= 1.0 ? sr : 44100.0;
        sampleRate = rate;
        core.prepare(rate);
        mixStep = (float) (1.0 / (0.004 * rate));
        easeK = (float) (1.0 - std::exp(-1.0 / (0.002 * rate)));
        followUp = (float) (1.0 - std::exp(-1.0 / (0.001 * rate)));
        followDown = (float) std::exp(-1.0 / (0.060 * rate));
        running = false;
        dirty = true;
    }

    // The voice's note-on. A body that still rings goes on ringing and moves to the new note.
    void noteOn(int note)
    {
        if (note != currentNote)
        {
            currentNote = note;
            dirty = true;
        }
        fresh = on;     // (a body that is off at the note-on comes up from nothing when MIX is raised)
        released = false;   // the source is sounding again
    }

    void reset()
    {
        if (running)
            core.reset();
        running = false;
        follow = 0.0f;
        released = false;
    }

    // True while the body has to be run: MIX is up, or it was and the body has not been put away.
    bool engaged() const { return on || running; }
    // True while the body holds sound (the voice is alive for as long).
    bool ringing() const { return running; }

    // One sample of the voice (engaged() only). sourceActive: the voice's own sound still runs;
    // without it the body rings out and is put away.
    float process(float in, bool sourceActive) noexcept
    {
        const float x = std::abs(in) < 1.0e6f ? in : 0.0f;      // (what is no number, or no sound, is silence)
        if (! running)
        {
            // The body starts empty; a note that begins with it takes MIX as it stands, a MIX
            // raised under a sounding note comes up from nothing.
            core.reset();
            running = true;
            dirty = true;
            pushIn = looks = 0;
            follow = 0.0f;
            ease = easeNow = 1.0f;
            mixNow = fresh ? mix : 0.0f;
            released = false;
        }
        fresh = false;

        // RDAM: when the source stops, the body dies faster.
        if (! sourceActive && ! released)
        {
            released = true;
            if (cell[ReleaseDamp] > 0.0f)
            {
                // Shorten the decay: at RDAM 1 the body dies in about 5 ms (like a palm mute).
                const float rdam = std::clamp(cell[ReleaseDamp], 0.0f, 1.0f);
                const float shortDecay = effectiveDecay() * (1.0f - rdam) + 0.005f * rdam;
                core.setDecay(std::max(0.005f, shortDecay));
            }
        }

        if (pushIn > 0)
            --pushIn;
        if (pushIn == 0 && dirty)
            push();

        // The voice's level, for the junction: up at once, down in 60 ms.
        float energy = 0.0f;
        if (coupled)
        {
            const float a = std::abs(x);
            follow = a > follow ? follow + (a - follow) * followUp : (follow > 1.0e-9f ? follow * followDown : 0.0f);
            energy = std::min(res::Resonator::MAX_ENERGY, follow * BOW);
        }
        easeNow += (ease - easeNow) * easeK;
        const float wet = core.tick(x * easeNow, energy) * MAKEUP;
        if (mixNow != mix)
            mixNow = dsp::towards(mixNow, mix, mixStep);
        const float out = x + mixNow * (wet - x);

        if (++looks >= LOOK)
        {
            looks = 0;
            const float held = core.held();
            const float full = held * (1.0f / FULL);
            ease = held >= 0.0f ? 1.0f / (1.0f + full * full) : 0.0f;
            // Put away: MIX has come down to nothing, or the voice has stopped and the body has rung out.
            if ((! on && mixNow == 0.0f) || (! sourceActive && held < SILENT && follow < 1.0e-4f))
            {
                core.reset();
                running = false;
            }
        }
        return out;
    }

    // --- Set by the engine (the same values go to every voice; an LFO or a slot gives one voice its own) ---
    void set(int c, float v)
    {
        if (c < 0 || c >= NumCells || ! std::isfinite(v) || cell[(size_t) c] == v)
            return;
        cell[(size_t) c] = v;
        if (c == Mix)
        {
            mix = std::clamp(v, 0.0f, 1.0f);
            on = mix > 0.0f;
        }
        else
            dirty = true;
    }
    float get(int c) const { return cell[(size_t) std::clamp(c, 0, (int) NumCells - 1)]; }

    // What moves the voice's pitch as a whole (tune, fine, bend), semitones: the body goes with it.
    void setPitchOffset(float semitones)
    {
        const float s = std::abs(semitones) <= 96.0f ? semitones : 0.0f;
        if (s != pitchOffset)
        {
            pitchOffset = s;
            dirty = true;
        }
    }

    // The body's pitch as the cells stand.
    double frequency() const
    {
        const float semis = 60.0f + cell[KeyTrack] * (float) (currentNote - 60) + cell[Tune] + pitchOffset;
        return 440.0 * std::exp2((double) (std::clamp(semis, -24.0f, 160.0f) - 69.0f) / 12.0);
    }

    // The effective decay as the cells stand (DCAY modified by KDCY and the note).
    float effectiveDecay() const
    {
        const float kdcy = std::clamp(cell[KeyDecay], -1.0f, 1.0f);
        if (kdcy == 0.0f)
            return cell[Decay];
        // KDCY: positive means higher notes decay faster (halved per octave at 1).
        const float octavesAboveC4 = (float) (currentNote - 60) / 12.0f;
        return cell[Decay] * std::exp2(-kdcy * octavesAboveC4);
    }

    res::Resonator& getResonator() { return core; }
    float mixNowValue() const { return mixNow; }
    float driveNow() const { return easeNow; }      // what the voice's sound is driven in with: 1, less as the body fills

private:
    // The cells go to the resonator when one moved, as often as it looks at them (it glides to them).
    void push() noexcept
    {
        dirty = false;
        pushIn = core.controlPeriod();
        res::BodyLatent offset {};
        offset[res::Damp] = cell[Damp];
        offset[res::Pos] = cell[Pos];
        core.setBody(cell[Morph], offset);
        // The exciter: by default the voice feeds the body continuously (CONT 1), no grains of
        // its own (GRAN 0), gripping by COUP with the bow's curve (REED 0); open (HARD auto:
        // the voice's own brightness passes) while it only strikes, a bow's hardness where it
        // grips. The page 2 cells override or add to these defaults.
        const float c = std::clamp(cell[Couple], 0.0f, 1.0f);
        const float cont = std::clamp(cell[Continuous], 0.0f, 1.0f);
        const float gran = std::clamp(cell[Grain], 0.0f, 1.0f);
        const float reed = std::clamp(cell[Reed], 0.0f, 1.0f);
        // HARD: at 0 (default), the auto behavior (1 - 0.4 * coup) as before; the cell adds.
        const float autoHard = 1.0f - 0.4f * c;
        const float hard = std::clamp(autoHard + cell[Hard], 0.0f, 1.0f);
        core.setExciter(res::ExciteLatent { { cont, gran, c, reed, hard } });
        // KDCY: pitch-dependent decay.
        core.setDecay(released ? std::max(0.005f, effectiveDecay() * (1.0f - std::clamp(cell[ReleaseDamp], 0.0f, 1.0f))
                                                  + 0.005f * std::clamp(cell[ReleaseDamp], 0.0f, 1.0f))
                               : effectiveDecay());
        core.setPitch(frequency());
        coupled = c > 0.0f;
        if (! coupled)
            follow = 0.0f;      // (not followed while it only strikes: what was followed must not keep the body from being put away)
        // [U2b] MORE latents: WIRE and DIRECT, as offsets from the body position's own values.
        if (cell[Wire] != 0.0f || cell[Direct] != 0.0f)
        {
            res::MoreLatent moreOff {};
            moreOff[res::Wire] = cell[Wire];
            moreOff[res::Direct] = cell[Direct];
            core.setMore(moreOff);
        }
        else if (moreWasSet)
        {
            core.setMore(res::MoreLatent {});
        }
        moreWasSet = cell[Wire] != 0.0f || cell[Direct] != 0.0f;
    }

    // (What a voice with BODY off reads every sample comes first, beside the voice's own members.)
    bool on = false, running = false, dirty = true, fresh = false, coupled = false, released = false;
    bool moreWasSet = false;
    int pushIn = 0, looks = 0;
    int currentNote = 60;
    float pitchOffset = 0.0f;
    double sampleRate = 44100.0;
    float mix = 0.0f, mixNow = 0.0f, mixStep = 0.005f;
    float ease = 1.0f, easeNow = 1.0f, easeK = 0.01f;
    float follow = 0.0f, followUp = 0.02f, followDown = 0.999f;
    //                                MIX   MRPH  DCAY  COUP  DAMP  POS   TUNE  KTRK  CONT  GRAN  REED  HARD  RDAM  KDCY  DIRE  WIRE
    std::array<float, NumCells> cell { { 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } };
    res::Resonator core;
};

// The engine side: parameter ids (base + cell), the two pages, true values, and applying a
// change to every voice's body.
class VoiceBodySection
{
public:
    static constexpr int BASE = 133;        // 133-148 (158-189 are the router's on every engine: ModRouter.h, the id rule)
    static constexpr int NUM_BODIES = res::NumBodyPoints;

    explicit VoiceBodySection(int baseParamId = BASE) : base(baseParamId)
    {
        // Seed from both pages.
        for (auto& page : makePages(base))
            for (auto& p : page.params)
                values[(size_t) (p.id - base)] = p.defaultVal;
    }

    static constexpr bool ownsId(int paramId, int baseId = BASE) { return paramId >= baseId && paramId < baseId + (int) VoiceBody::NumCells; }
    bool owns(int paramId) const { return ownsId(paramId, base); }
    float get(int paramId) const { return owns(paramId) ? values[(size_t) (paramId - base)] : 0.0f; }

    // The first page (BODY): the original eight cells, unchanged.
    static ParamPage makePage(int baseId = BASE)
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log;
        const int b = baseId;
        ParamPage page;
        page.name = "BODY"; page.section = "BODY";
        page.params = {{
            { "MIX",  b + VoiceBody::Mix, 0, 1, 0, L },
            { "MRPH", b + VoiceBody::Morph, 0, (float) (NUM_BODIES - 1), 0, L, "x", nullptr, 0, 0.05f },
            { "DCAY", b + VoiceBody::Decay, 0.05f, 20, 2.0f, G, "s" },
            { "COUP", b + VoiceBody::Couple, 0, 1, 0, L },
            { "DAMP", b + VoiceBody::Damp, -1, 1, 0, L },
            { "POS",  b + VoiceBody::Pos, -1, 1, 0, L },
            { "TUNE", b + VoiceBody::Tune, -24, 24, 0, L, "st", nullptr, 0, 1.0f },
            { "KTRK", b + VoiceBody::KeyTrack, 0, 1, 1, L } }};
        return page;
    }

    // The second page (BDY2): exciter character, release behavior, body sounds.
    static ParamPage makePage2(int baseId = BASE)
    {
        auto L = ParamKind::Linear;
        const int b = baseId;
        ParamPage page;
        page.name = "BDY2"; page.section = "BODY";
        page.params = {{
            { "CONT", b + VoiceBody::Continuous, 0, 1, 1.0f, L },
            { "GRAN", b + VoiceBody::Grain, 0, 1, 0, L },
            { "REED", b + VoiceBody::Reed, 0, 1, 0, L },
            { "HARD", b + VoiceBody::Hard, -1, 1, 0, L },
            { "RDAM", b + VoiceBody::ReleaseDamp, 0, 1, 0, L },
            { "KDCY", b + VoiceBody::KeyDecay, -1, 1, 0, L },
            { "DIRE", b + VoiceBody::Direct, 0, 1, 0, L },
            { "WIRE", b + VoiceBody::Wire, 0, 1, 0, L } }};
        return page;
    }

    // Both pages as a vector, for seeding defaults.
    static std::vector<ParamPage> makePages(int baseId = BASE)
    {
        return { makePage(baseId), makePage2(baseId) };
    }

    // Append both BODY pages to the engine's page list.
    static void appendPage(ParamPages& pages, int baseId = BASE)
    {
        pages.push_back(makePage(baseId));
        pages.push_back(makePage2(baseId));
    }

    // ---- Modulation destinations (EngineLfoSection: both LFOs, the strip's routes, the mod slots) ----
    // All sixteen cells, under names of their own (MIX, MRPH, TUNE, KTRK ... are taken on one
    // engine or another). Add them at the end of the engine's list. The first eight keep their
    // labels (BMIX .. BKTK) for stored DEST compatibility; the new eight follow.
    static const char* destLabel(int cell)
    {
        static const char* const labels[VoiceBody::NumCells] = {
            "BMIX", "BMRP", "BDCY", "BCUP", "BDMP", "BPOS", "BTUN", "BKTK",
            "BCNT", "BGRN", "BRED", "BHRD", "BRDM", "BKDC", "BDIR", "BWIR" };
        return labels[(size_t) std::clamp(cell, 0, (int) VoiceBody::NumCells - 1)];
    }
    static void addLfoDests(LfoDestList& l, const ParamPages& pages, int baseId = BASE)
    {
        l.group();
        for (int c = 0; c < (int) VoiceBody::NumCells; ++c)
            l.add(pages, baseId + c, destLabel(c));
    }

    // One voice's body takes `value` for the cell at `offset` (what an LFO, a strip route or a
    // mod slot on that cell gives it, sample by sample).
    static void applyTo(VoiceBody& b, int offset, float value) { b.set(offset, value); }

    // Stores the (range-checked) value and gives it to every voice. forEachBody(fn) must call
    // fn(VoiceBody&) for every voice of the engine.
    template <typename ForEachBody>
    void set(int paramId, float value, const ForEachBody& forEachBody)
    {
        if (! owns(paramId) || ! std::isfinite(value))
            return;
        const int o = paramId - base;
        value = spec(o).clamp(value);
        values[(size_t) o] = value;
        forEachBody([o, value](VoiceBody& b) { b.set(o, value); });
    }

    // The "every voice's body" argument for set, for a container of voices that each have getBody().
    template <typename Voices>
    static auto each(Voices& voices)
    {
        return [&voices](auto&& fn) { for (auto& v : voices) fn(v.getBody()); };
    }

    static const char* name(int offset)
    {
        static const char* const names[VoiceBody::NumCells] = {
            "Body Mix", "Body Morph", "Body Decay", "Body Coupling", "Body Damping", "Body Position", "Body Tune", "Body Key Track",
            "Body Continuous", "Body Grain", "Body Reed", "Body Hard", "Body Release Damp", "Body Key Decay", "Body Direct", "Body Wire" };
        return (offset >= 0 && offset < (int) VoiceBody::NumCells) ? names[offset] : "";
    }

private:
    static const ParamSpec& spec(int offset)
    {
        // Build from both pages so that all 16 cells have specs.
        static const std::array<ParamSpec, VoiceBody::NumCells> specs = []
        {
            std::array<ParamSpec, VoiceBody::NumCells> s {};
            auto pages = makePages(0);
            for (auto& page : pages)
                for (auto& p : page.params)
                    if (p.id >= 0 && p.id < (int) VoiceBody::NumCells)
                        s[(size_t) p.id] = p;
            return s;
        }();
        return specs[(size_t) std::clamp(offset, 0, (int) VoiceBody::NumCells - 1)];
    }

    int base;
    std::array<float, VoiceBody::NumCells> values {};
};
