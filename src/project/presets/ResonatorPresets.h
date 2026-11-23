#pragma once

#include "../Preset.h"
#include "../../engine/ResonatorEngine.h"
#include <cmath>
#include <cstring>
#include <vector>

// The resonator's ready sounds.
//
// ONE list, TWO hosts. A sound is a set of the RSN engine's own cells (ResonatorEngine::Param and
// the filter's: the engine's preset format). resonatorSounds() is what a kit pad's SND cell
// offers (drum type RSN, engine/DrumEngine.h: the pad's voice is the engine's own voice, at a
// pitch of its own); resonatorPresets() gives the same sounds as presets of the RSN engine, to be
// played across the keys. A sound made for one is a sound of the other.
//
// What a sound says besides its cells is for the kit alone: the TUNE and DEC the factory kits
// give its pad, how long a second of DEC is, what the pad's pressure does (energy: a bow, a
// brush; damping: a hat closed by the finger), and whether it sounds only while the pad is held.
//
// SND is stored by number: sounds are only ever added at the end.
//
// As presets they are in the category PERC (one of the PRESET screen's tabs; they were "DRUM",
// which is none of them, and so were found under ALL only).
//
// (PresetManager registers the presets beside FactoryPresets::getAll(), whose own count several
// suites pin. The "Resonator" suite loads and plays them.)
struct ResonatorSound
{
    const char* tag;            // what SND shows: four letters
    const char* name;           // as a preset of the RSN engine
    const char* category;
    ParamMap params;            // the engine's cells
    float hz;                   // in a kit: the pad's TUNE ...
    float dec;                  // ... and DEC
    float decScale = 1.0f;      // seconds of DCAY a second of DEC is
    float pressEnergy = 0.0f;   // the pad's pressure as the exciter's energy
    float pressDamp = 0.0f;     // ... and on DAMP (+: the finger damps)
    bool gated = false;         // a hand falls on it when the pad is let go
    float driveWire = 0.0f;     // how much of the pad's DRIV is added to WIRE
    float lofi = 0.0f;         // the level-following crusher (the cymbal machine's LOFI): 0 = off
    float driveLofi = 0.0f;    // how much of the pad's DRIV is added to LOFI

    // CHAN RING: the body is heard until it has died, and DEC is its decay. Without it the
    // envelope cuts the sound to length, and DEC is the envelope's decay.
    bool rings() const
    {
        const auto it = params.find(ResonatorEngine::EnvChainMode);
        return it != params.end() && (int) std::lround(it->second) == 3;
    }
};

namespace rsnsound
{
    using E = ResonatorEngine;
    inline int body(int latent)   { return E::BodyBase + latent; }
    inline int more(int latent)   { return E::MoreBase + latent; }
    inline int excite(int latent) { return E::ExciteBase + latent; }
    inline int filter(int offset) { return E::FilterBase + offset; }

    // What every drum sound starts from: struck, as loud as it is hit, the envelope the energy
    // alone (CHAN RING: on the keys too the body is heard until it has died).
    inline ParamMap drum(float morph, float exciter, std::initializer_list<std::pair<const int, float>> more)
    {
        ParamMap p { { E::MorphParam, morph }, { E::ExciterMorphParam, exciter }, { E::EnergyParam, 0.5f }, { E::VelSenseParam, 1.0f },
                     { E::LevelParam, 0.8f }, { E::AmpAttack, 0.001f }, { E::AmpDecay, 0.05f }, { E::AmpSustain, 0.0f },
                     { E::AmpRelease, 0.05f }, { E::EnvChainMode, 3.0f } };
        for (const auto& kv : more)
            p[kv.first] = kv.second;
        return p;
    }

    // The body latents of a point of the cymbals' region (res::cymbalPoints: sheets and shells
    // whose top is lost fast), as what must be added to PLAT to get it.
    inline void sheet(ParamMap& p, int cymbalPoint)
    {
        const auto& want = res::cymbalPoints()[(size_t) cymbalPoint];
        const auto& plate = res::bodyPoints()[res::Plate];
        p[E::MorphParam] = (float) res::Plate;
        for (int i = 0; i < res::NumBody; ++i)
            p[body(i)] = want[(size_t) i] - plate[(size_t) i];
    }
}

inline const std::vector<ResonatorSound>& resonatorSounds()
{
    static const std::vector<ResonatorSound> all = []
    {
        using namespace rsnsound;
        using F = VoiceFilterSection;
        std::vector<ResonatorSound> s;
        auto add = [&s](const char* tag, const char* name, const char* category, ParamMap params, float hz, float dec,
                        float decScale = 1.0f, float pressEnergy = 0.0f, float pressDamp = 0.0f, bool gated = false)
        {
            ResonatorSound sound { tag, name, category, std::move(params), hz, dec, decScale, pressEnergy, pressDamp, gated };
            sound.params[sound.rings() ? (int) E::DecayParam : (int) E::AmpDecay] = std::fmin(20.0f, dec * decScale);
            s.push_back(std::move(sound));
        };

        // ---- drums cut to length by their envelope (not CHAN RING: DEC is the envelope's decay) ----
        const int decayCurve = StageCurveParam::id(0, StageCurveParam::Decay);
        // TOM: a head (MRPH 6) with hardly anything above its first partial, its pitch falling
        // after the stroke (DROP), a soft stick.
        add("TOM", "Tom", "PERC", drum(6.0f, 0.0f, {
            { E::EnvChainMode, 0.0f }, { E::AmpAttack, 0.005f }, { decayCurve, -0.4f }, { E::DecayParam, 0.8f },
            { body(res::Damp), 0.45f }, { body(res::Tilt), 0.65f }, { more(res::Drop), 0.25f }, { more(res::Direct), 0.5f },
            { more(res::Wire), 0.2f }, { excite(res::Hard), -0.05f },
            { E::EnergyParam, 1.0f }, { E::LevelParam, 0.9f } }), 100.0f, 0.3f, 1.0f);
        // SNAR: the head short and dull, the wires on it (WIRE) as loud as the head and a moment
        // longer; DRIV is more wires.
        add("SNAR", "Snare", "PERC", drum(6.0f, 0.0f, {
            { E::EnvChainMode, 0.0f }, { decayCurve, -0.3f }, { E::DecayParam, 0.35f },
            { body(res::Damp), 0.35f }, { body(res::Tilt), 0.6f }, { more(res::Drop), -0.25f }, { more(res::Wire), 0.45f },
            { excite(res::Hard), -0.3f }, { E::EnergyParam, 0.8f }, { E::LevelParam, 0.65f } }), 200.0f, 0.2f, 0.7f);
        s.back().driveWire = 0.2f;
        // FMSN: the same wires on a head that is half a sheet: clangy, with partials between the head's.
        add("FMSN", "Metal Snare", "PERC", drum(6.0f, 0.0f, {
            { E::EnvChainMode, 0.0f }, { decayCurve, -0.3f }, { E::DecayParam, 0.35f },
            { body(res::Sheet), 0.45f }, { body(res::Damp), 0.2f }, { body(res::Tilt), 0.4f }, { more(res::Drop), -0.25f },
            { more(res::Wire), 0.45f }, { excite(res::Hard), -0.35f }, { E::EnergyParam, 0.8f }, { E::LevelParam, 0.65f } }),
            200.0f, 0.2f, 0.7f);
        s.back().driveWire = 0.2f;
        // CLAV: two partials a fourth apart (a line pulled most of the way to a shell: 1.33), the
        // rest lost at once, and struck in the middle (POS: the third partial is not reached).
        // (Review: against the old clave's two plain partials the rest stood only 3 dB under the
        // first; so it is 11 dB under. The second is 5 dB under the first, where the old two were equal.)
        add("CLAV", "Clave", "PERC", drum(2.0f, 0.0f, {
            { E::EnvChainMode, 0.0f }, { decayCurve, -0.3f }, { E::DecayParam, 0.5f },
            { body(res::Shell), 0.82f }, { body(res::Damp), 0.9f }, { body(res::Tilt), 0.05f }, { body(res::Pos), 0.5f },
            { excite(res::Hard), -0.6f },
            { E::EnergyParam, 1.0f }, { E::LevelParam, 1.0f } }), 2500.0f, 0.029f, 1.0f);
        // RIM: a stick on the rim - a tick of a bar, and what rattles with it (WIRE).
        add("RIM", "Rim Shot", "PERC", drum(4.0f, 0.0f, {
            { E::EnvChainMode, 0.0f }, { decayCurve, -0.5f }, { E::DecayParam, 0.3f },
            { body(res::Damp), 0.6f }, { body(res::Tilt), 0.7f }, { more(res::Wire), 0.32f }, { excite(res::Hard), -0.3f },
            { E::EnergyParam, 1.0f }, { E::LevelParam, 0.5f } }), 800.0f, 0.005f, 1.0f);

        // ---- hats: two small plates (a dense sheet), their sizzle the same noisy contact as the wires ----
        // Open to closed is DAMP, and the pad's pressure is on it: half open by the finger.
        auto hat = [](float damp, float wire, float cont, float hard)
        {
            ParamMap p = drum(3.0f, 0.0f, {
                { body(res::Stiff), 0.55f }, { body(res::Damp), damp - 0.25f }, { body(res::Tilt), -0.2f }, { more(res::Wire), wire },
                { excite(res::Cont), cont }, { excite(res::Grain), 1.0f }, { excite(res::Hard), hard - 0.7f },
                { E::AmpDecay, 0.03f }, { E::LevelParam, 0.6f },
                { filter(F::Type), 0.0f }, { filter(F::Mode), 2.0f }, { filter(F::Cutoff), 3000.0f } });
            return p;
        };
        // (CONT is how long the burst of noise between the plates lasts: a hundredth of a second
        // closed, an eighth open.)
        add("HATC", "Hat Closed", "PERC", hat(0.9f, 0.55f, 0.15f, 0.95f), 420.0f, 0.08f, 1.0f, 0.0f, 0.3f);
        add("HATH", "Hat Half Open", "PERC", hat(0.72f, 0.7f, 0.3f, 0.95f), 420.0f, 0.3f, 1.0f, 0.0f, 0.6f);
        add("HATO", "Hat Open", "PERC", hat(0.55f, 0.6f, 0.5f, 0.95f), 420.0f, 1.2f, 1.0f, 0.0f, 0.9f);
        // The pedal: the plates snap shut on each other - no stick, a soft burst and the sizzle.
        add("HATP", "Hat Pedal", "PERC", hat(0.95f, 0.8f, 0.1f, 0.6f), 420.0f, 0.06f, 1.0f, 0.0f, 0.0f);

        // ---- cymbals and gongs with partials placed by hand (MRPH 7..13) -------------------------
        // A stick's stroke on every partial of the table, and a burst of noise with it that the
        // band of upper partials turns into the wash (CONT: how long that burst is).
        auto metal = [](float morph, float cont, float hard, float energyDecay, float level)
        {
            return drum(morph, 0.0f, { { excite(res::Cont), cont }, { excite(res::Grain), 1.0f }, { excite(res::Hard), hard - 0.7f },
                                       { E::AmpDecay, energyDecay }, { E::LevelParam, level } });
        };
        // (Seconds of DCAY a second of DEC: what the table's first partial rang for, per second of
        // DEC, on the machine these tables come from.)
        add("RIDE", "Ride", "PERC", metal(10.0f, 0.15f, 0.95f, 0.05f, 0.4f), 431.0f, 1.3f, 0.9f);
        add("CUP", "Ride Bell", "PERC", metal(11.0f, 0.1f, 0.9f, 0.05f, 0.33f), 622.0f, 1.0f, 3.75f);
        add("CRSH", "Crash", "PERC", metal(9.0f, 0.2f, 0.98f, 0.05f, 0.3f), 287.0f, 1.5f, 5.0f);
        add("CHNA", "China", "PERC", metal(12.0f, 0.2f, 0.95f, 0.05f, 0.5f), 498.0f, 1.0f, 1.5f);
        add("SPLS", "Splash", "PERC", metal(13.0f, 0.15f, 0.98f, 0.05f, 0.32f), 812.0f, 0.8f, 1.8f);
        add("GONG", "Gong", "PERC", metal(7.0f, 0.1f, 0.3f, 0.05f, 0.5f), 196.0f, 2.0f, 11.25f);
        // (The tam-tam's shimmer comes after the stroke: the energy rises for half a second.)
        {
            ParamMap p = metal(8.0f, 0.7f, 0.45f, 2.5f, 1.0f);
            p[E::AmpAttack] = 0.5f;
            add("TAM", "Tam-Tam", "PERC", std::move(p), 71.0f, 2.0f, 15.0f);
        }

        // ---- the same instruments as plain sheets and shells (the latents alone, no table) -------
        auto plain = [](int point, float cont, float hard, float energyDecay, float level, float hpHz)
        {
            ParamMap p = drum(3.0f, 0.0f, { { excite(res::Cont), cont }, { excite(res::Grain), 1.0f }, { excite(res::Hard), hard - 0.7f },
                                           { E::AmpDecay, energyDecay }, { E::LevelParam, level },
                                           { filter(F::Type), 0.0f }, { filter(F::Mode), 2.0f }, { filter(F::Cutoff), hpHz } });
            sheet(p, point);
            return p;
        };
        add("RID2", "Sheet Ride", "PERC", plain(res::CymRide, 0.55f, 0.75f, 3.0f, 0.8f, 2000.0f), 240.0f, 1.3f, 5.0f);
        // The old cymbal machine's house ride had LOFI 0.35: a level-following crusher that gave
        // it the grain of an early sampled cymbal. DRIV adds more of it.
        s.back().lofi = 0.35f;
        s.back().driveLofi = 0.3f;
        add("CUP2", "Shell Bell", "PERC", plain(res::CymBell, 0.25f, 0.75f, 1.0f, 0.5f, 300.0f), 622.0f, 1.0f, 4.0f);
        add("CRS2", "Sheet Crash", "PERC", plain(res::CymCrash, 0.8f, 0.8f, 3.0f, 0.9f, 1800.0f), 175.0f, 1.5f, 4.7f);
        add("CHN2", "Sheet China", "PERC", plain(res::CymChina, 0.7f, 0.9f, 0.5f, 0.9f, 1400.0f), 190.0f, 1.0f, 1.5f);
        add("SPL2", "Sheet Splash", "PERC", plain(res::CymSplash, 0.7f, 0.8f, 0.2f, 0.7f, 3000.0f), 240.0f, 0.8f, 1.0f);
        add("GNG2", "Domed Gong", "PERC", plain(res::CymGong, 0.15f, 0.25f, 0.5f, 0.8f, 60.0f), 196.0f, 2.0f, 8.0f);
        {
            ParamMap p = plain(res::CymTam, 0.6f, 0.4f, 4.0f, 1.0f, 35.0f);
            p[E::AmpAttack] = 1.0f;
            add("TAM2", "Flat Tam-Tam", "PERC", std::move(p), 71.0f, 2.0f, 12.0f);
        }

        // ---- bowed, brushed, beating ---------------------------------------------------------------
        // A gong with every partial a pair that beats, each pair at its own rate (PAIR).
        {
            ParamMap p = metal(7.0f, 0.3f, 0.45f, 1.0f, 0.8f);
            p[more(res::Pair)] = 1.0f;
            p[more(res::Hand)] = -0.35f;        // (part of the way back to a line: fewer, clearer partials to pair)
            p[body(res::Sheet)] = 0.35f;
            add("GNGB", "Beating Gong", "PERC", std::move(p), 110.0f, 2.0f, 6.0f);
        }
        // A snare under a bow: the pad's pressure is the bow, the wires sing along.
        add("SNRB", "Bowed Snare", "PERC", drum(6.0f, 1.0f, {
            { body(res::Damp), 0.1f }, { body(res::Tilt), 0.0f }, { more(res::Drop), -0.25f }, { more(res::Wire), 0.6f },
            { more(res::Meet), 0.12f }, { more(res::Spread), 0.3f }, { excite(res::Couple), 0.15f }, { E::AmpDecay, 0.4f },
            { E::AmpSustain, 0.0f }, { E::EnergyParam, 0.9f }, { E::LagParam, 0.04f }, { E::LevelParam, 1.0f } }), 200.0f, 0.8f, 1.0f, 1.0f);
        // A snare under brushes: the tap from the velocity, the swish from the pad's pressure,
        // the wires' own hiss beside the head (DIR).
        add("SNRS", "Brushed Snare", "PERC", drum(6.0f, 4.0f, {
            { body(res::Damp), 0.3f }, { body(res::Tilt), 0.1f }, { more(res::Drop), -0.25f }, { more(res::Wire), 0.5f },
            { more(res::Direct), 0.75f }, { E::AmpDecay, 0.12f }, { E::LagParam, 0.03f }, { E::LevelParam, 0.8f } }),
            200.0f, 0.25f, 1.0f, 0.8f);
        // A ride under brushes.
        {
            ParamMap p = drum(10.0f, 4.0f, { { excite(res::Hard), 0.45f }, { more(res::Direct), 0.5f }, { E::AmpDecay, 0.15f },
                                           { E::LagParam, 0.03f }, { E::LevelParam, 0.8f } });
            add("RIDS", "Brushed Ride", "PERC", std::move(p), 431.0f, 1.3f, 0.9f, 0.8f);
        }
        // A gong under brushes.
        add("GNGS", "Brushed Gong", "PERC", drum(7.0f, 4.0f, { { excite(res::Hard), 0.3f }, { more(res::Direct), 0.3f },
            { E::AmpDecay, 0.3f }, { E::LagParam, 0.05f }, { E::LevelParam, 0.8f } }), 196.0f, 2.0f, 4.5f, 0.8f);
        // A cymbal under a bow, met high up and over a spread of partials: it sings on an upper one
        // of them (which, the body says), not on its first as a bow met there alone does.
        add("CYMB", "Bowed Cymbal", "PERC", drum(9.0f, 1.0f, { { more(res::Meet), 0.45f }, { more(res::Spread), 0.5f },
            { E::AmpDecay, 1.0f }, { E::AmpSustain, 0.0f }, { E::LagParam, 0.08f }, { E::LevelParam, 0.8f } }),
            287.0f, 1.5f, 3.4f, 1.0f);
        // A bar under a bow, met at its third partial: a glass tone two octaves and a half up.
        add("BARB", "Bowed Bar", "PERC", drum(4.0f, 1.0f, { { more(res::Meet), 0.134f }, { body(res::Damp), -0.15f },
            { E::AmpDecay, 1.0f }, { E::LagParam, 0.06f }, { E::LevelParam, 0.8f } }), 220.0f, 2.0f, 1.5f, 1.0f);
        // A bell under a bow, met about its fifth partial and its neighbours.
        add("BELB", "Bowed Bell", "PERC", drum(5.0f, 1.0f, { { more(res::Meet), 0.27f }, { more(res::Spread), 0.12f },
            { E::AmpDecay, 1.0f }, { E::LagParam, 0.06f }, { E::LevelParam, 0.8f } }), 220.0f, 2.0f, 2.0f, 1.0f);

        // ---- for the kits that morph (project/presets/ResonatorKits.h) -------------------------------
        // A head that rings like a gong's would (long, not cut by its envelope): what a slot or the
        // pad's MRPH carries from HEAD to GONG. Its envelope (1.2 s) is the time the morph takes.
        add("TMGN", "Tom Gong", "PERC", drum(6.0f, 0.0f, {
            { body(res::Damp), 0.1f }, { body(res::Tilt), 0.3f }, { more(res::Drop), -0.15f }, { excite(res::Hard), -0.3f },
            { E::AmpDecay, 1.2f }, { E::EnergyParam, 0.9f }, { E::LevelParam, 0.8f } }), 110.0f, 1.5f, 2.0f);
        // A clave as what it is, a short bar (cut to length by its envelope): one point of MRPH from a bell.
        add("CLVB", "Bar Clave", "PERC", drum(4.0f, 0.0f, {
            { E::EnvChainMode, 0.0f }, { decayCurve, -0.3f }, { E::DecayParam, 0.8f },
            { body(res::Damp), 0.5f }, { body(res::Tilt), 0.2f }, { excite(res::Hard), -0.3f },
            { E::EnergyParam, 1.0f }, { E::LevelParam, 0.9f } }), 1800.0f, 0.06f, 1.0f);
        return s;
    }();
    return all;
}

// The place of a sound in SND's list (0 if there is none of that tag).
inline int resonatorSoundIndex(const char* tag)
{
    const auto& all = resonatorSounds();
    for (size_t i = 0; i < all.size(); ++i)
        if (std::strcmp(all[i].tag, tag) == 0)
            return (int) i;
    return 0;
}

// A slot's DEST is a place in ResonatorEngine::lfoDests() and its SRC one in the router's list of
// sources: both are looked up by name here, so the presets follow the lists as they grow, and a
// name that is not there is remembered (stored::unknownNames, Preset.h: the preset suites fail on
// it; both lists begin with OFF, which is no name to route by: from 1).
inline std::vector<Preset> resonatorPresets()
{
    using E = ResonatorEngine;
    auto dest = [](const char* label)
    {
        return stored::placeOf(E::lfoDests().labels(), E::lfoDests().visible(), label, "resonator destination", 1);
    };
    auto src = [](const char* label)
    {
        const auto& sources = mod::SourceTable::standard(2);
        return stored::placeOf(sources.labels(), sources.size(), label, "resonator source", 1);
    };
    auto slot = [](int n, int offset) { return ModSlotParam::id(n, offset); };
    auto make = [](const char* name, const char* category, ParamMap params)
    {
        Preset p;
        p.name = name;
        p.category = category;
        p.engineType = EngineType::Resonator;
        p.soundParams = std::move(params);
        p.isFactory = true;
        return p;
    };
    const int filter = E::FilterBase;

    std::vector<Preset> out;

    // The envelope morphs within each note: struck as a string, and as the filter envelope falls
    // (1.5 s) the partials leave the harmonics and settle on a bell's, the hum coming up under them.
    // (FENV, so that the sound's own level stays up while it happens; it does not move the cutoff.)
    out.push_back(make("String To Bell", "KEYS", {
        { E::MorphParam, 5.0f }, { E::ExciterMorphParam, 0.0f }, { E::DecayParam, 6.0f }, { E::EnergyParam, 0.5f },
        { E::ExciteBase + res::Hard, 0.15f },
        { E::AmpAttack, 0.001f }, { E::AmpDecay, 1.0f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 2.5f },
        { filter + VoiceFilterSection::EnvAmount, 0.0f }, { filter + VoiceFilterSection::Attack, 0.001f },
        { filter + VoiceFilterSection::Decay, 1.5f }, { filter + VoiceFilterSection::Sustain, 0.0f },
        { slot(0, ModSlotParam::SRC), src("FENV") }, { slot(0, ModSlotParam::DEST), dest("MRPH") },
        { slot(0, ModSlotParam::AMT), -5.0f / (float) (E::NUM_BODIES - 1) } }));

    // From note to note: every note is another body somewhere between the tube and the bar (RAND on
    // MRPH), and a harder note is struck harder and nearer the middle (VEL on HARD and POS).
    out.push_back(make("Shifting Bodies", "KEYS", {
        { E::MorphParam, 2.5f }, { E::ExciterMorphParam, 0.0f }, { E::DecayParam, 2.5f }, { E::EnergyParam, 0.5f },
        { E::BodyBase + res::Tilt, 0.1f },
        { E::AmpAttack, 0.001f }, { E::AmpDecay, 0.5f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 1.5f },
        { slot(0, ModSlotParam::SRC), src("RAND") }, { slot(0, ModSlotParam::DEST), dest("MRPH") },
        { slot(0, ModSlotParam::AMT), 0.3f * 5.0f / (float) (E::NUM_BODIES - 1) },
        { slot(1, ModSlotParam::SRC), src("VEL") }, { slot(1, ModSlotParam::DEST), dest("HARD") },
        { slot(1, ModSlotParam::AMT), 0.2f } }));

    // The exciter morphs under the pad: a struck string that rings out, and pressure turns the
    // strike into a bow (PRES on XMPH: 0 = STRK, the pad right down = BOW). Pressure is the bow's
    // energy as well (the EXC route every RSN track starts with).
    out.push_back(make("Strike To Bow", "LEAD", {
        { E::MorphParam, 0.0f }, { E::ExciterMorphParam, 0.0f }, { E::DecayParam, 1.5f }, { E::EnergyParam, 0.5f },
        { E::LagParam, 0.03f },
        { E::AmpAttack, 0.001f }, { E::AmpDecay, 0.5f }, { E::AmpSustain, 1.0f }, { E::AmpRelease, 1.0f },
        { slot(0, ModSlotParam::SRC), src("PRES") }, { slot(0, ModSlotParam::DEST), dest("XMPH") },
        { slot(0, ModSlotParam::AMT), 1.0f / (float) (E::NUM_EXCITERS - 1) } }));

    // [U2b] Every sound of the list above as a preset on the keys (the pitch its kit pad would
    // have is the key nearest `hz`; here it follows the key).
    for (const auto& sound : resonatorSounds())
        out.push_back(make(sound.name, sound.category, sound.params));

    // [U2b] And what needs the slots and LFOs of a whole track (in a kit they are the kit's:
    // project/FactoryPresets.h makes the same things there).
    {
        // A tom that turns into a gong within every note: the filter envelope (2 s) carries MRPH
        // from HEAD to GONG and the decay with it.
        ParamMap p = resonatorSounds()[(size_t) resonatorSoundIndex("TOM")].params;
        p[E::DecayParam] = 4.0f;
        p[filter + VoiceFilterSection::EnvAmount] = 0.0f;  p[filter + VoiceFilterSection::Attack] = 2.0f;
        p[filter + VoiceFilterSection::Decay] = 4.0f;      p[filter + VoiceFilterSection::Sustain] = 1.0f;
        p[slot(0, ModSlotParam::SRC)] = src("FENV");  p[slot(0, ModSlotParam::DEST)] = dest("MRPH");
        p[slot(0, ModSlotParam::AMT)] = 1.0f / (float) (E::NUM_BODIES - 1);
        out.push_back(make("Tom To Gong", "PERC", std::move(p)));
    }
    {
        // Brushes on a snare, stirred: LFO 1 on the energy and LFO 2 on the hardness, a quarter
        // turn apart, one turn of the hand a beat (synced); the pad's pressure is how hard.
        ParamMap p = resonatorSounds()[(size_t) resonatorSoundIndex("SNRS")].params;
        const int lfo1 = EngineLfoSection::BASE, lfo2 = EngineLfoSection::BASE + EngineLfoSection::PER_LFO;
        p[lfo1 + EngineLfoSection::Depth] = 0.45f;  p[lfo1 + EngineLfoSection::Sync] = 1.0f;
        p[lfo1 + EngineLfoSection::Dest] = dest("ENRG");
        p[lfo2 + EngineLfoSection::Depth] = 0.3f;   p[lfo2 + EngineLfoSection::Sync] = 1.0f;
        p[lfo2 + EngineLfoSection::Dest] = dest("HARD");
        out.push_back(make("Stirred Snare", "PERC", std::move(p)));
    }
    {
        // A brush that turns into a stick within the note: XMPH falls from BRSH to STRK under the
        // amp envelope's own fall... the other way round in time: it starts as a brush's swish and
        // hardens (the filter envelope rising carries XMPH down to the strike and HARD up).
        ParamMap p = resonatorSounds()[(size_t) resonatorSoundIndex("RIDS")].params;
        p[E::AmpDecay] = 1.5f;  p[E::AmpSustain] = 0.6f;
        p[filter + VoiceFilterSection::EnvAmount] = 0.0f;  p[filter + VoiceFilterSection::Attack] = 0.8f;
        p[filter + VoiceFilterSection::Decay] = 2.0f;      p[filter + VoiceFilterSection::Sustain] = 1.0f;
        p[slot(0, ModSlotParam::SRC)] = src("FENV");  p[slot(0, ModSlotParam::DEST)] = dest("XMPH");
        p[slot(0, ModSlotParam::AMT)] = -0.25f;     // (one point down: BRSH to SCRP, sparser and harder)
        p[slot(1, ModSlotParam::SRC)] = src("FENV");  p[slot(1, ModSlotParam::DEST)] = dest("HARD");
        p[slot(1, ModSlotParam::AMT)] = 0.25f;
        out.push_back(make("Brush To Stick", "PERC", std::move(p)));
    }
    return out;
}
