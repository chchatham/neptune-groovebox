#pragma once

#include "mod_wheel_tests.h"
#include "lfo_destinations_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/EngineLfoSection.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/presets/FusionPresets.h"
#include "../src/project/presets/ResonatorPresets.h"
#include "../src/project/presets/ResonatorKits.h"
#include "../src/project/Preset.h"
#include "../src/project/ProjectBridge.h"
#include "../src/sequencer/SequencerCore.h"
#include <juce_core/juce_core.h>
#include <map>
#include <set>
#include <vector>

// -----------------------------------------------------------------------------
// The STRIP page of the FM, macro, wavetable, wave sequencer, sampler and drum
// engines: three routes from the mod strip (DST1-3 / AMT1-3) and the vibrato it
// brings in (WVIB / VRAT), ids 174-181 (EngineLfoSection).
//
// - The default routes are what each engine's strip did before the page existed.
//   REFERENCE holds what the "Mod Wheel" measurement gave for every engine
//   default and factory preset of these engines at commit 767f21d (fixed routes
//   in the engines); the same measurement must still give those numbers.
// - With every amount at zero the strip does nothing at all.
// - A route re-pointed to any destination of the engine's list is heard with the
//   strip up and not with the strip at rest.
// - The cells are ordinary cells: on a page, set and read back, saved with
//   projects and presets, unique ids inside the engine blocks, LFO pages last.
// -----------------------------------------------------------------------------
class StripRoutesTests : public juce::UnitTest
{
public:
    StripRoutesTests() : UnitTest("Strip Routes") {}

    using S = EngineLfoSection;
    using W = ModWheelTests;
    using D = LfoDestinationsTests;

    static constexpr double SR = 48000.0;
    static constexpr int BLOCK = 64;
    // A route holds its destination at one offset, where the LFO tests swing a square wave both
    // ways: half their threshold.
    static constexpr float MIN_HELD_DIFFERENCE = 0.01f;

    static const std::vector<EngineType>& stripEngines()
    {
        static const std::vector<EngineType> types = { EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                                                       EngineType::WaveSeq, EngineType::Sampler, EngineType::Drum };
        return types;
    }

    // ---- the measurement before the page existed (commit 767f21d, Debug build, 48 kHz) ---------
    // The macro oscillator and drum rows were measured again after the dead controls were fixed (the
    // fourth macro now acts in every model; drums ring out and DRIV works on every type), and the
    // drum rows and the two breaks rendered from the drum voices (100, 170) once more on 2026-11-14,
    // when the hats became six band-passed squares: 7-25 % less bright at rest than the old hats,
    // which were squares at 8 kHz and above folding back from the top of the band.
    struct Ref { const char* name; float diff, centroid0, centroid127, levelDb; };

    static const std::vector<Ref>& reference()
    {
        static const std::vector<Ref> refs = {
            // engine defaults
            { "FM", 0.172f, 208, 252, -0.4f },              { "Macro oscillator", 0.335f, 536, 305, -0.6f },
            { "Wavetable", 0.825f, 196, 604, -4.2f },       { "Wave sequencer", 0.363f, 633, 635, -0.0f },
            { "Sampler", 0.653f, 347, 495, 4.0f },          { "Drums", 0.390f, 57, 141, -0.0f },
            { "Resonator", 0.408f, 354, 474, 1.5f },        // (2026-11-21, U1: the engine rebuilt on dsp/Resonator.h - a struck string of 64 modes;
                                                            //  the strip is ENRG and DAMP - 0.2: the highs ring longer. [U4-RSN] was 0.4 dB:
                                                            //  on the router a note struck with the strip already up takes ENRG as it
                                                            //  stands - the strike is harder - where the old routes glided in after it)
            // factory presets of these engines
            { "FM Bass 2", 0.116f, 199, 212, 0.5f },        { "FM Bell Pad", 0.128f, 421, 408, 0.3f },
            { "Anthem Bell", 0.266f, 221, 391, -0.0f },     // (2026-11-14)
            // (2026-11-14; the strip opens its filter. Level: 1.5 dB until phase U4, when the grains' random
            // numbers became draws of spread::Generator: every grain is somewhere else, and this one
            // number of one render moves with the seed - over eight seeds 1.8 .. 4.4 dB on the old engine,
            // 3.1 .. 5.3 dB on the new, "U4 Sampler" has the six-seed comparison of the sound.)
            { "Abyss Drone", 0.677f, 889, 1680, 4.5f },
            // The macro oscillator's thirty-two (2026-11-14), each model's own strip routes.
            { "Super Saw Lead", 0.255f, 452, 301, -1.0f },
            { "Hollow Sq Bass", 0.165f, 180, 136, -0.8f },
            { "Soft Sine Pad", 0.337f, 303, 181, 1.0f },
            { "Octave Strings", 0.301f, 668, 475, -2.3f },
            { "FM Glass Keys", 0.644f, 1415, 2130, -0.1f },
            { "FM Growl Bass", 0.457f, 342, 265, -1.2f },
            { "Detuned Chime", 0.640f, 1589, 2084, -0.2f },
            { "Feedback Scream", 0.378f, 1167, 1329, 0.2f },
            { "Fold Lead", 0.657f, 749, 1904, 0.4f },
            { "West Coast Bass", 0.382f, 194, 650, -1.3f },
            { "Cheby Organ", 0.480f, 441, 1037, 2.9f },
            { "Fold Sweep Pad", 0.340f, 538, 1124, -1.5f },
            { "Choir Aah", 0.472f, 879, 2323, -7.0f },
            { "Talking Lead", 0.434f, 726, 1792, -9.2f },
            // (U4-MACRO, 2026-11-22: the models' noise - PLCK's burst, DUST's clicks, the whisper and
            // the drum blend - became draws of the voice's generator per note (spread::Model) where
            // its seed was a function of the note. These rows are one note's draw: re-measured. The
            // old numbers in brackets; "U4 Macro" compares the mean of 32 notes, old against new.)
            { "Whisper Wind", 0.259f, 2855, 1231, -2.6f },     // (0.259, 2894, 1157, -2.0)
            { "Vox Bass", 0.347f, 413, 646, -12.3f },          // (0.348, 414, 643, -11.5: the strip brings the whisper in)
            { "Pure Organ", 0.236f, 203, 206, -1.7f },
            { "Stiff Bell", 0.333f, 203, 274, -3.7f },
            { "Clarinet Tone", 0.316f, 197, 210, -1.6f },
            { "Glass Harmonics", 0.526f, 249, 496, -3.1f },
            { "Nylon Guitar", 0.314f, 2825, 6227, -1.2f },     // (0.372, 916, 4996, -4.7: the burst's offset moves the centroid)
            { "Pluck Bass", 0.240f, 5692, 7094, -1.3f },       // (0.259, 4572, 7269, -2.6)
            { "Koto Snap", 0.491f, 3389, 6590, -2.6f },        // (0.541, 1454, 5949, -7.4)
            { "String Drum", 0.230f, 8263, 7453, -1.2f },      // (0.229, 8669, 7974, -1.0)
            { "Vinyl Crackle", 0.171f, 8195, 7694, 4.3f },     // (0.172, 8159, 7674, 4.3)
            { "Pitched Rain", 0.111f, 214, 209, 1.5f },        // (0.113, 226, 226, 1.8)
            { "Hiss Riser", 0.111f, 5116, 4336, 0.7f },        // (0.111, 6262, 5182, 0.3)
            { "Dust Hat", 0.114f, 6841, 6564, 0.9f },          // (0.106, 7713, 7548, 1.2)
            { "Solo Cello", 0.178f, 272, 277, 3.9f },        { "Glass Bow", 0.326f, 516, 486, 5.5f },
            { "Scrape FX", 0.311f, 704, 419, 6.6f },         { "Bowed Bass", 0.182f, 214, 209, 5.1f },
            { "FM Pluck", 0.171f, 216, 289, 0.6f },         { "Electric Piano", 0.119f, 202, 248, -0.0f },
            { "Rhodes", 0.089f, 200, 211, 0.2f },           { "FM Metallic", 0.125f, 288, 292, -0.1f },
            // (U2b: the kits' rows measured again. Their toms and snares are sounds of the resonator
            // drum now, the hats the oscillator bank's: with the strip up DEC lengthens a tom's own
            // ring inside its envelope as well, and DRIV on a snare is more wires. The strip changes
            // the kits a quarter more than it did; where they sit at rest is where they sat.)
            { "808 Kit 2", 0.384f, 55, 147, -0.5f },        { "FM Kit", 0.392f, 94, 214, -0.6f },
            { "Electro Kit", 0.370f, 60, 168, -0.5f },      { "Minimal Kit", 0.396f, 50, 127, -1.0f },
            { "Industrial Kit", 0.365f, 76, 219, -0.7f },   { "Percussion Kit", 0.402f, 54, 170, -1.0f },
            { "Grain Cloud", 0.131f, 4931, 2915, 0.1f },    { "Resonator Pluck", 0.502f, 2398, 5783, -3.0f },  // (U4-MACRO: one draw; was 0.130 4972 2938 0.0 / 0.505 1631 5380 -5.2)
            // Noise Sweep and Wind remade at U6 on the wavetable NOIS table (were saws from convertUnified).
            { "Noise Sweep", 0.281f, 209, 199, -0.5f },     { "Wind", 0.349f, 218, 199, -0.6f },
            { "Digital Choir", 0.553f, 2486, 4057, -2.1f }, { "Frozen Sweep", 0.490f, 2613, 1152, -2.2f },
            { "Glitch Table", 0.239f, 197, 201, 0.0f },     { "Sequenced Bells", 0.609f, 5114, 5513, -0.2f },
            { "Morphing Texture", 0.590f, 613, 2007, 2.0f }, { "Glass Marimba", 0.175f, 207, 280, 0.0f },
            { "Metallic Stab", 0.140f, 234, 244, 0.0f },    { "FM Pad", 0.117f, 207, 227, 1.1f },
            { "Lo-Fi Tape", 0.298f, 407, 532, 0.3f },       { "Granular Pad", 0.686f, 334, 483, 4.6f },
            { "Vinyl Scratch", 0.307f, 476, 568, 1.2f },    { "Lo-Fi Kit", 0.357f, 57, 141, -0.8f },
            // The drum-break presets (measured when they were added, 2026-11-11): the sampler's own routes,
            // drive and sample-rate reduction, on a break that loops in time and on its slices.
            { "Break 100 Loop", 0.395f, 317, 290, 2.5f }, { "Break 100 Slices", 0.531f, 90, 104, 2.0f },
            { "Break 170 Loop", 0.338f, 403, 368, 2.4f }, { "Break 170 Slices", 0.568f, 91, 112, 1.8f },
            { "Break 80 Loop", 0.298f, 683, 432, 5.2f }, { "Break 80 Slices", 0.289f, 604, 303, 4.7f },
            { "Break 90 Loop", 0.255f, 561, 641, 2.3f }, { "Break 90 Slices", 0.268f, 668, 672, 2.3f },
            { "Break 105 Loop", 0.247f, 105, 121, 1.9f }, { "Break 105 Slices", 0.261f, 80, 91, 2.8f },
            { "Break 120 Loop", 0.243f, 187, 178, 3.6f }, { "Break 120 Slices", 0.249f, 112, 90, 3.8f },
            { "Break 157 Loop", 0.223f, 1265, 968, 4.1f }, { "Break 157 Slices", 0.236f, 1662, 1167, 4.2f },
            { "Break 160 Loop", 0.264f, 207, 228, 3.2f }, { "Break 160 Slices", 0.265f, 150, 145, 3.1f },
            { "Minimal Techno Kit", 0.416f, 49, 143, -1.0f },
            // The cymbal machine's two kits (2026-11-14): the strip's routes are the kit's own (DEC, DRIV).
            // Every drum row was measured again on 2026-11-21, when the cymbal machine's bodies moved
            // onto the shared resonator (unification U2): each kit plays its sixteen pads here, two of
            // them (ten of Cymbal Works) that machine. The strip changes the kits a seventh less and
            // leaves them 7-25 % less bright at the top than before: three times the DEC lengthens the
            // new cymbals' wash as it did the old ones', but not the tick of the stroke, which is now a
            // larger part of them (crash, first 1.5 s: x 1.4 in level where the old one gave x 1.6).
            { "Cymbal Works", 0.292f, 59, 242, -0.8f },     { "House Kit", 0.309f, 57, 170, -0.6f },
            // The presets for the wavetable engine's newer tables (measured when they were added,
            // 2026-11-11): the engine's own route, POS +60 and vibrato. The strip walks the vowels
            // (Vowel Choir ends on the dark O / U, so its brightness stays), raises the sync, FM and
            // fold amounts, pulls the organ's remaining stops (the last registration is the bright
            // one, hence 364 -> 2179 Hz), moves the octaves' weight upwards, takes Noise To Tone
            // further towards the tone (darker), and holds Reso Sweep Pad's slow sweep at the open
            // end of its table (its resonance then sits above most of the harmonics: a touch darker).
            { "Vowel Lead", 0.263f, 611, 741, -0.2f },       { "Vowel Choir", 0.229f, 385, 384, 0.1f },
            { "I You We Yeah", 0.215f, 307, 236, 0.5f },     // (2026-11-14; the strip moves where its four words start)
            { "Sync Sweep Lead", 0.349f, 822, 1524, 0.1f },  { "Fold Bass", 0.357f, 390, 596, 0.3f },
            { "FM Table Keys", 0.468f, 471, 1068, -0.2f },   { "Glass Pluck", 0.425f, 1046, 1768, 0.2f },
            { "Struck Bar", 0.343f, 1296, 2270, -1.4f },     { "Drawbar Organ", 0.681f, 364, 2179, -1.7f },
            { "Noise To Tone", 0.358f, 279, 199, -0.2f },    { "Reso Sweep Bass", 0.421f, 236, 279, -0.9f },
            { "Reso Sweep Pad", 0.329f, 667, 625, -1.3f },   { "Walsh Stepper", 0.292f, 630, 908, -0.0f },
            { "Prime Shimmer", 0.399f, 414, 541, -0.1f },    { "Octave Climb", 0.534f, 432, 2111, -1.8f },
            { "Phase Bass", 0.345f, 150, 200, 0.2f },        { "Comb Phase Pad", 0.270f, 533, 927, -0.3f },
            { "Eight Bit Vowels", 0.266f, 1560, 1191, 0.0f }, { "Glassy Bell", 0.362f, 1850, 4603, -1.2f },
            { "Raw Sync Lead", 0.314f, 904, 1632, -0.0f },
            // the macro oscillator's models and the sampler's play modes
            { "model 0", 0.335f, 536, 305, -0.6f },          { "model 1", 0.654f, 2221, 2850, -0.3f },
            { "model 2", 0.406f, 672, 925, 0.5f },          { "model 3", 0.368f, 601, 1089, -4.8f },     // (U4-MACRO draw; was 0.367 601 1027 -4.7)
            { "model 4", 0.373f, 201, 215, -2.6f },         { "model 5", 0.410f, 2170, 6241, -5.3f },     // (U4-MACRO draw; was 0.452 1291 4554 -8.3)
            { "model 6", 0.119f, 7586, 5587, 0.2f },        { "model 7", 0.370f, 797, 545, 10.5f },     // (re-measured 2026-11-14: the bowed string now sounds;
                                                             //  the earlier 23 Hz was its constant offset)
            { "mode 0", 0.653f, 347, 495, 4.0f },           { "mode 1", 0.640f, 275, 420, 2.9f },
            { "mode 2", 0.654f, 322, 463, 3.9f },           { "mode 3", 0.653f, 347, 495, 4.0f },
            { "mode 4", 0.653f, 347, 495, 4.0f },           { "mode 5", 0.653f, 347, 495, 4.0f },
            { "mode 6", 0.653f, 358, 431, 2.9f },
            // The fusion presets (src/project/presets/FusionPresets.h; measured 2026-11-22, when they got
            // their routes). Those of the resonator, DustBody, PluckBdy, MorphBdy and ShimrPad set STRIP
            // routes of their own: the bow's and the breath's energy with vibrato (+5 to +7 dB: the strip
            // is the harder bow), the dust thinned out, the string's damping taken away, the morph
            // carried on to a gong, the sub brought in (158 Hz for 204). The others have their engine's
            // route: the modulation index (FM), the fourth macro (macro), the position (wavetable), the
            // rate (wave sequencer), decay and drive (PointKit: its resonator drums ring longer, +5 dB).
            // Deep Mod is the unified voice's (its STRIP page: CUT), measured by "Fusion Presets".
            { "BowCello", 0.297f, 467, 470, 6.6f },         { "BowGlass", 0.187f, 393, 394, 5.1f },
            { "BrthPipe", 0.231f, 412, 357, 6.6f },         { "ScrapBar", 0.181f, 783, 952, 5.7f },
            { "StepShpe", 0.190f, 789, 789, -0.8f },        { "PluckBdy", 0.336f, 214, 226, -0.2f },
            { "DustBody", 0.653f, 290, 207, 0.4f },         { "ReedBody", 0.262f, 338, 378, -2.4f },
            { "MorphBdy", 0.306f, 218, 221, -0.3f },        { "FMFilter", 0.207f, 244, 265, 0.2f },
            { "SeqMorph", 0.326f, 354, 322, -0.2f },        { "Rnd Time", 0.425f, 541, 933, -3.1f },
            { "FMWaveOp", 0.289f, 1074, 1353, -0.3f },      { "FM Cycle", 0.289f, 298, 354, 0.4f },
            { "DualFilt", 0.185f, 1160, 1158, -0.3f },      { "SerlFilt", 0.123f, 448, 457, -0.3f },
            { "Seeded", 0.355f, 329, 272, -1.8f },          { "SeedTabl", 0.448f, 389, 592, -0.1f },
            { "Mono Wt", 0.438f, 1023, 2093, 3.1f },        { "Wide Pad", 0.272f, 547, 481, -0.2f },
            { "SprngKey", 0.097f, 203, 241, -0.0f },        { "ShimrPad", 0.215f, 204, 158, 3.0f },
            { "ThruFlng", 0.350f, 422, 269, -1.3f },        { "PointKit", 0.360f, 514, 755, 5.3f },
            // The four kits of resonator drums (src/project/presets/ResonatorKits.h; measured 2026-11-22,
            // when they were registered): the kit's own routes, DEC and DRIV.
            { "Tom To Gong Kit", 0.361f, 62, 119, 0.8f },   { "Brush Kit", 0.291f, 100, 237, -0.1f },
            { "Resonator Kit", 0.278f, 531, 566, 4.9f },    { "Clave Bells Kit", 0.461f, 71, 162, -0.4f },
            // U6 resonator presets (default strip = ENRG + DAMP).
            { "String To Bell", 0.471f, 443, 1052, 2.9f },
            { "Shifting Bodies", 0.423f, 2696, 2676, 1.5f },
            { "Strike To Bow", 0.413f, 345, 458, 1.5f },
            { "Tom", 0.023f, 260, 267, -0.0f },
            { "Snare", 0.024f, 775, 783, 0.7f },
            { "Metal Snare", 0.038f, 536, 551, 0.7f },
            { "Clave", 0.018f, 248, 252, 0.0f },
            { "Rim Shot", 0.001f, 402, 402, 0.0f },
            { "Hat Closed", 0.045f, 6876, 6921, 1.6f },
            { "Hat Half Open", 0.044f, 6979, 7019, 1.6f },
            { "Hat Open", 0.084f, 7080, 6879, 2.2f },
            { "Hat Pedal", 0.036f, 6915, 6916, 1.3f },
            { "Ride", 0.208f, 1899, 1886, 1.3f },
            { "Ride Bell", 0.123f, 521, 517, 1.1f },
            { "Crash", 0.243f, 2600, 2281, 0.8f },
            { "China", 0.142f, 952, 995, 1.0f },
            { "Splash", 0.141f, 1236, 1221, 1.2f },
            { "Gong", 0.120f, 232, 236, 1.3f },
            { "Tam-Tam", 0.303f, 504, 580, 5.0f },
            { "Sheet Ride", 0.183f, 3607, 3484, 6.9f },
            { "Shell Bell", 0.458f, 915, 1421, 3.0f },
            { "Sheet Crash", 0.173f, 3842, 3605, 7.0f },
            { "Sheet China", 0.218f, 2953, 3416, 6.8f },
            { "Sheet Splash", 0.074f, 6670, 6681, 4.0f },
            { "Domed Gong", 0.181f, 288, 293, 1.1f },
            { "Flat Tam-Tam", 0.236f, 591, 662, 3.6f },
            { "Beating Gong", 0.235f, 577, 654, 1.6f },
            { "Bowed Snare", 0.033f, 948, 945, 0.9f },
            { "Brushed Snare", 0.064f, 515, 520, 2.3f },
            { "Brushed Ride", 0.213f, 2240, 2241, 2.0f },
            { "Brushed Gong", 0.168f, 297, 374, 1.4f },
            { "Bowed Cymbal", 0.190f, 606, 571, 3.6f },
            { "Bowed Bar", 0.264f, 999, 1029, 4.0f },
            { "Bowed Bell", 0.311f, 492, 496, 3.8f },
            { "Tom Gong", 0.204f, 268, 303, 1.0f },
            { "Bar Clave", 0.035f, 213, 218, 0.0f },
            { "Tom To Gong", 0.037f, 282, 292, 0.1f },
            // (Measured on 2026-11-22 while a resonator standing alone had no pressure route at all:
            // 0.044, 1556 -> 1648, 1.4. A resonator is made with the pad on EXC again, as a track
            // always had it, and a note then begins at its strike instead of coming up through
            // LAG: the brush is a little darker at the start. The other resonator rows do not move.)
            { "Stirred Snare", 0.043f, 1427, 1524, 1.3f },
            { "Brush To Stick", 0.220f, 2118, 2119, 5.6f } };
        return refs;
    }

    static const Ref* refFor(const juce::String& name)
    {
        for (auto& r : reference())
            if (name == r.name)
                return &r;
        return nullptr;
    }

    // How far a sound may be from its reference: the routes are the same amounts through the
    // LFOs' per-sample path (the fixed ones were per block), and the vibrato's phase starts
    // elsewhere, so the renders are close, not the same samples.
    static constexpr float DIFF_TOLERANCE = 0.04f;
    static constexpr float CENTROID_TOLERANCE = 0.06f;      // of the reference, + 8 Hz
    static constexpr float LEVEL_TOLERANCE_DB = 0.8f;

    struct Measured { float diff, centroid0, centroid127, levelDb; };

    static Measured measureStrip(EngineType type, const ParamMap* sound = nullptr)
    {
        // The "Mod Wheel" suite's measurement of the same sound (strip at rest, strip up), taken
        // from that suite when it has run in this process, made here when it has not.
        const auto r = W::compare(type, sound, false);
        return { r.diff, r.at0.centroid, r.at127.centroid, W::dB(r.at127.rms / std::max(r.at0.rms, 1.0e-9f)) };
    }

    void checkAgainstReference(const juce::String& name, const Measured& m)
    {
        const Ref* r = refFor(name);
        expect(r != nullptr, name + ": no reference measurement");
        if (r == nullptr)
        {
            // What to put into REFERENCE once the sound has been looked at.
            logMessage("{ \"" + name + "\", " + juce::String(m.diff, 3) + "f, " + juce::String((int) std::lround(m.centroid0)) + ", "
                       + juce::String((int) std::lround(m.centroid127)) + ", " + juce::String(m.levelDb, 1) + "f },");
            return;
        }
        logMessage(name.paddedRight(' ', 20) + " diff " + juce::String(m.diff, 3) + " (was " + juce::String(r->diff, 3) + ")"
                   + "  centroid " + juce::String((int) m.centroid0) + " -> " + juce::String((int) m.centroid127)
                   + " (was " + juce::String((int) r->centroid0) + " -> " + juce::String((int) r->centroid127) + ")"
                   + "  level " + juce::String(m.levelDb, 1) + " dB (was " + juce::String(r->levelDb, 1) + ")");
        expectWithinAbsoluteError(m.diff, r->diff, DIFF_TOLERANCE, name + ": how much the strip changes the sound");
        expectWithinAbsoluteError(m.centroid0, r->centroid0, 0.02f * r->centroid0 + 2.0f, name + ": brightness at rest");
        expectWithinAbsoluteError(m.centroid127, r->centroid127, CENTROID_TOLERANCE * r->centroid127 + 8.0f,
                                  name + ": brightness with the strip up");
        expectWithinAbsoluteError(m.levelDb, r->levelDb, LEVEL_TOLERANCE_DB, name + ": level change with the strip up");
    }

    // ---- helpers -------------------------------------------------------------------------------

    static const ParamPage* stripPage(const ParamPages& pages)
    {
        for (auto& p : pages)
            if (p.name == "STRIP")
                return &p;
        return nullptr;
    }

    // Every route off and no vibrato (the voice filter's WHL is 0 by default).
    static void clearRoutes(EngineBase& e)
    {
        for (int r = 0; r < S::NUM_STRIP; ++r)
        {
            e.setParameter(S::stripAmountId(r), 0.0f);
            e.setParameter(S::stripDestId(r), 0.0f);
        }
        e.setParameter(S::STRIP_VIBRATO, 0.0f);
    }

    // The strip is given, the smoother arrives (0.2 s of blocks without a note), then the plan plays.
    static std::vector<float> renderWithStrip(D::Plan& plan, float strip)
    {
        plan.engine->setModWheel(strip);
        juce::AudioBuffer<float> buf(2, BLOCK);
        juce::MidiBuffer midi;
        for (int b = 0; b < (int) (0.2 * SR / BLOCK); ++b)
        {
            buf.clear();
            plan.engine->processBlock(buf, midi);
        }
        return D::render(plan);
    }

    // The same with the strip moving: at rest while the smoother settles, pushed up as the plan
    // starts (it rises in about 20 ms). For a phase (D::isPhase), which is heard while it moves.
    static std::vector<float> renderWithStripRising(D::Plan& plan)
    {
        renderWithStrip(plan, 0.0f);    // (only for the settled smoother: its render is not used)
        plan.engine->setModWheel(1.0f);
        return D::render(plan);
    }

    static bool finite(const std::vector<float>& x)
    {
        for (float s : x)
            if (! std::isfinite(s))
                return false;
        return true;
    }

    void runTest() override
    {
        beginTest("The STRIP page: three routes and the vibrato, before the two LFO pages, on the six engines");
        for (auto type : stripEngines())
        {
            const auto name = D::engineName(type);
            auto e = D::makeEngine(type);
            const auto pages = e->getParamPages();
            expect(pages.size() >= 3, name);
            if (pages.size() < 3)
                continue;
            expectEquals(pages[pages.size() - 1].name, juce::String("LFO2"), name + ": LFO2 is the last page");
            expectEquals(pages[pages.size() - 2].name, juce::String("LFO1"), name + ": LFO1 is the last but one");
            const auto& strip = pages[pages.size() - 3];
            expectEquals(strip.name, juce::String("STRIP"), name + ": STRIP is just before them");
            expectEquals(strip.section, pages.back().section, name + ": in the LFO pages' section");

            static const char* const labels[8] = { "DST1", "AMT1", "DST2", "AMT2", "DST3", "AMT3", "WVIB", "VRAT" };
            const auto dests = D::destLabels(*e);
            for (int c = 0; c < 8; ++c)
            {
                const auto& p = strip.params[(size_t) c];
                expectEquals(juce::String(p.label), juce::String(labels[c]), name);
                expectEquals(p.id, S::STRIP_BASE + c, name + ": ids 174-181 in cell order");
                // The engine holds the cell's default, and gives back what it is set to.
                expectWithinAbsoluteError(e->getParameter(p.id), p.defaultVal, 1.0e-6f, name + " " + p.label + ": default");
                expect(e->getParameterName(p.id).isNotEmpty(), name + " " + p.label + ": has a name");
            }
            for (int r = 0; r < S::NUM_STRIP; ++r)
            {
                const auto& dest = strip.params[(size_t) (r * 2)];
                const auto& amount = strip.params[(size_t) (r * 2 + 1)];
                // DEST is the LFO pages' DEST: the same list, the same names, grouped for Func + turn.
                const ParamSpec* lfoDest = pages.back().find(S::id(1, S::Dest));
                expect(lfoDest != nullptr && dest.choices == lfoDest->choices && dest.numChoices == lfoDest->numChoices,
                       name + ": DST" + juce::String(r + 1) + " offers the LFOs' list");
                // (The engine's own, then the cells of the track's effects: E4.)
                expect(dest.kind == ParamKind::Choice && (int) dests.size() + mod::TrackDest::COUNT == dest.numChoices);
                expect(ChoiceGroups::find(dest.choices) != nullptr, name + ": the list has groups");
                expectEquals(dest.turned(0.0f, 1, TurnStep::Coarse), 1.0f, name + ": Func + turn stops at the first destination");
                expect(dest.turned(1.0f, 1, TurnStep::Coarse) > 2.0f, name + ": and then at the next group");
                expectEquals(dest.turned(3.0f, 1, TurnStep::Plain), 4.0f);
                expect(amount.isBipolar() && amount.minVal == -1.0f && amount.maxVal == 1.0f, name + ": AMT is -100..+100");
                expectEquals(amount.format(-1.0f), juce::String("-100"));
                expectEquals(amount.format(1.0f), juce::String("+100"));

                e->setParameter(dest.id, (float) (dest.numChoices - 1));
                expectEquals(e->getParameter(dest.id), (float) (dest.numChoices - 1), name + ": DST reads back");
                e->setParameter(dest.id, 1000.0f);
                expectEquals(e->getParameter(dest.id), (float) (dest.numChoices - 1), name + ": DST is limited to the list");
                e->setParameter(amount.id, -0.37f);
                expectWithinAbsoluteError(e->getParameter(amount.id), -0.37f, 1.0e-6f, name + ": AMT reads back");
                e->setParameter(amount.id, 3.0f);
                expectEquals(e->getParameter(amount.id), 1.0f, name + ": AMT is limited");
                expect(! e->isRebuildParam(dest.id) && ! e->isRebuildParam(amount.id), name + ": the cells can be locked");
            }
            e->setParameter(S::STRIP_VIBRATO, 0.4f);
            expectWithinAbsoluteError(e->getParameter(S::STRIP_VIBRATO), 0.4f, 1.0e-6f);
            e->setParameter(S::STRIP_VIBRATO_RATE, 3.0f);
            expectWithinAbsoluteError(e->getParameter(S::STRIP_VIBRATO_RATE), 3.0f, 1.0e-6f);
        }

        beginTest("The default routes are the ones the engines had built in");
        {
            struct Route { const char* dest; float amount; };
            struct Default { EngineType type; Route routes[3]; float vibrato; };
            const std::vector<Default> defaults = {
                { EngineType::FM,        { { "INDX", 1.0f },    { "OFF", 0.0f },  { "OFF", 0.0f } },  0.25f },
                { EngineType::MacroOsc,  { { "MORP", 1.0f },    { "SAWS", 0.3f }, { "OFF", 0.0f } },  0.25f },
                { EngineType::Wavetable, { { "POS", 0.6f },     { "OFF", 0.0f },  { "OFF", 0.0f } },  0.25f },
                { EngineType::WaveSeq,   { { "RATE", 0.5f },    { "OFF", 0.0f },  { "OFF", 0.0f } },  0.0f },
                { EngineType::Sampler,   { { "DRIV", 0.7f },    { "RATE", 0.3f }, { "SPRY", 0.5f } }, 0.0f },
                { EngineType::Drum,      { { "DEC", 0.7925f },  { "DRIV", 0.5f }, { "OFF", 0.0f } },  0.0f } };
            for (auto& d : defaults)
            {
                auto e = D::makeEngine(d.type);
                const auto labels = D::destLabels(*e);
                const auto name = D::engineName(d.type);
                for (int r = 0; r < 3; ++r)
                {
                    const int dest = (int) std::lround(e->getParameter(S::stripDestId(r)));
                    expectEquals(labels[(size_t) dest], juce::String(d.routes[r].dest), name + " route " + juce::String(r + 1));
                    expectWithinAbsoluteError(e->getParameter(S::stripAmountId(r)), d.routes[r].amount, 1.0e-6f,
                                              name + " amount " + juce::String(r + 1));
                }
                expectWithinAbsoluteError(e->getParameter(S::STRIP_VIBRATO), d.vibrato, 1.0e-6f, name + " WVIB");
                expectWithinAbsoluteError(e->getParameter(S::STRIP_VIBRATO_RATE), 5.5f, 1.0e-6f, name + " VRAT");
            }
            // The macro oscillator's routes are its fourth and first macro in every model.
            for (int model = 0; model < 8; ++model)
            {
                MacroOscEngine e;
                e.setParameter(MacroOscEngine::ModelParam, (float) model);
                const auto pages = e.getParamPages();
                const auto* strip = stripPage(pages);
                expect(strip != nullptr);
                if (strip == nullptr)
                    continue;
                expectEquals(juce::String(strip->params[0].format(e.getParameter(S::stripDestId(0)))),
                             juce::String(pages[0].params[4].label), "model " + juce::String(model) + ": route 1 is the fourth macro");
                expectEquals(juce::String(strip->params[2].format(e.getParameter(S::stripDestId(1)))),
                             juce::String(pages[0].params[1].label), "model " + juce::String(model) + ": route 2 is the first macro");
            }
        }

        beginTest("Defaults: every engine's default sound answers the strip as it did with the fixed routes");
        for (auto& [type, name] : W::soundEngines())
            if (type != EngineType::UnifiedVoice)
                checkAgainstReference(name, measureStrip(type));

        beginTest("Defaults: every factory preset of these engines answers the strip as it did");
        {
            int checked = 0;
            // All factory presets: the original list, plus the fusion and resonator presets.
            auto all = FactoryPresets::getAll();
            for (auto& p : FusionPresets::getAll()) all.push_back(p);
            for (auto& p : resonatorPresets()) all.push_back(p);
            for (auto& p : resonatorKits()) all.push_back(p);
            for (auto& preset : all)
            {
                if (preset.engineType == EngineType::MIDITrack || preset.engineType == EngineType::UnifiedVoice)
                    continue;
                // (A preset may set STRIP cells of its own - the fusion presets do where the engine's
                // default route has nothing to work on: its row is then what its own routes give.)
                if (refFor(preset.name) == nullptr)
                {
                    // Log what the reference row would be, so the builder can add it.
                    auto m = measureStrip(preset.engineType, &preset.soundParams);
                    logMessage("{ \"" + preset.name + "\", " + juce::String(m.diff, 3) + "f, "
                               + juce::String((int) std::lround(m.centroid0)) + ", " + juce::String((int) std::lround(m.centroid127))
                               + ", " + juce::String(m.levelDb, 1) + "f },");
                    expect(false, preset.name + ": no reference measurement");
                    continue;
                }
                checkAgainstReference(preset.name, measureStrip(preset.engineType, &preset.soundParams));
                ++checked;
            }
            expect(checked >= 99, "at least the original 99 factory presets of the six engines, got " + juce::String(checked));
        }

        beginTest("Defaults: the macro oscillator's models and the sampler's play modes");
        {
            for (int model = 0; model < 8; ++model)
            {
                const ParamMap sound { { MacroOscEngine::ModelParam, (float) model } };
                checkAgainstReference("model " + juce::String(model), measureStrip(EngineType::MacroOsc, &sound));
            }
            for (int mode = 0; mode < SamplerEngine::NumPlayModes; ++mode)
            {
                const ParamMap sound { { SamplerEngine::Mode, (float) mode } };
                checkAgainstReference("mode " + juce::String(mode), measureStrip(EngineType::Sampler, &sound));
            }
        }

        beginTest("Every amount at zero (or every destination OFF): the strip does nothing");
        for (auto type : stripEngines())
        {
            const auto name = D::engineName(type);
            // AMT 0, destinations and vibrato as they are by default / DST OFF, amounts as they are.
            for (int variant = 0; variant < 2; ++variant)
            {
                ParamMap sound;
                for (int r = 0; r < S::NUM_STRIP; ++r)
                    sound[variant == 0 ? S::stripAmountId(r) : S::stripDestId(r)] = 0.0f;
                sound[S::STRIP_VIBRATO] = 0.0f;
                const auto rest = W::render(*W::makeEngine(type, &sound), 0.0f, 1.5);
                const auto up = W::render(*W::makeEngine(type, &sound), 1.0f, 1.5);
                expect(W::measure(rest).rms > 1.0e-4f, name + ": silent");
                const float d = W::difference(rest, up);
                expect(d <= W::MAX_REST_DIFFERENCE,
                       name + (variant == 0 ? ", amounts 0" : ", destinations OFF") + ": the strip is still heard (" + juce::String(d, 5) + ")");
                // And the sound at rest is the default sound at rest: the cells themselves change nothing.
                const float same = W::difference(rest, W::render(*W::makeEngine(type), 0.0f, 1.5));
                expect(same <= W::MAX_REST_DIFFERENCE, name + ": at rest the sound differs from the default (" + juce::String(same, 5) + ")");
            }
            // The default routes can be turned down: at half the amounts the strip is still
            // heard, at none it is not. (Not "less and less" for every engine: the wave sequencer
            // at twice the speed is in time with itself again, and closer than at 1.4 times.)
            auto e = D::makeEngine(type);
            juce::String steps;
            for (float scale : { 0.0f, 0.5f, 1.0f })
            {
                ParamMap sound;
                for (int r = 0; r < S::NUM_STRIP; ++r)
                    sound[S::stripAmountId(r)] = e->getParameter(S::stripAmountId(r)) * scale;
                sound[S::STRIP_VIBRATO] = e->getParameter(S::STRIP_VIBRATO) * scale;
                const float d = W::difference(W::render(*W::makeEngine(type, &sound), 0.0f, 1.5),
                                              W::render(*W::makeEngine(type, &sound), 1.0f, 1.5));
                steps << juce::String(d, 3) << " ";
                if (scale == 0.0f)
                    expect(d <= W::MAX_REST_DIFFERENCE, name + ": amounts x 0, the strip is still heard");
                else
                    expect(d >= D::MIN_DIFFERENCE, name + ": amounts x " + juce::String(scale, 1) + ", the strip is hardly heard (" + juce::String(d, 3) + ")");
            }
            logMessage(name.paddedRight(' ', 10) + " strip difference with the default amounts x 0 / 0.5 / 1: " + steps);
        }

        for (auto type : stripEngines())
        {
            beginTest(D::engineName(type) + ": a route to each destination of the list is heard with the strip up, not at rest");
            const auto name = D::engineName(type);
            const auto labels = D::destLabels(*D::makeEngine(type));
            expect(labels.size() > 8, name + ": the long list");
            std::map<juce::String, std::vector<float>> stills;      // per set-up: no route, strip up
            juce::String quiet;
            for (size_t d = 1; d < labels.size(); ++d)
            {
                const auto& label = labels[d];
                // A phase (D::isPhase) held where the strip holds it is no change of the sound: it is
                // heard while the strip moves, against the same strip moving with no route.
                const bool phase = D::isPhase(type, label);
                // The drum kit reads the strip once, at the strike ("Drums: the strip is read at the strike"):
                // a phase it sets is held for the whole hit, which nobody hears. "U4 Drum" hears PM from the
                // track input and from an LFO.
                if (phase && type == EngineType::Drum)
                    continue;
                float heard = 0.0f, atRest = 0.0f;
                bool ok = true;
                for (float amount : { 1.0f, -1.0f })
                {
                    auto plain = D::prepare(type, label, labels);
                    if (stills.find(plain.variant) == stills.end())
                    {
                        clearRoutes(*plain.engine);
                        stills[plain.variant] = renderWithStrip(plain, 1.0f);
                    }
                    const auto& still = stills[plain.variant];
                    if (phase && stills.find(plain.variant + " rising") == stills.end())
                    {
                        auto rising = D::prepare(type, label, labels);
                        clearRoutes(*rising.engine);
                        stills[plain.variant + " rising"] = renderWithStripRising(rising);
                    }
                    const auto& stillMoving = stills[phase ? plain.variant + " rising" : plain.variant];

                    auto resting = D::prepare(type, label, labels);
                    clearRoutes(*resting.engine);
                    resting.engine->setParameter(S::stripDestId(1), (float) d);
                    resting.engine->setParameter(S::stripAmountId(1), amount);
                    atRest = std::max(atRest, W::difference(still, renderWithStrip(resting, 0.0f)));

                    auto up = D::prepare(type, label, labels);
                    clearRoutes(*up.engine);
                    up.engine->setParameter(S::stripDestId(1), (float) d);
                    up.engine->setParameter(S::stripAmountId(1), amount);
                    const auto moved = phase ? renderWithStripRising(up) : renderWithStrip(up, 1.0f);
                    ok = ok && finite(moved);
                    heard = std::max(heard, W::difference(stillMoving, moved));
                    if (heard >= 2.0f * D::MIN_DIFFERENCE)
                        break;
                }
                expect(ok, name + " " + label + ": output is finite");
                expect(heard >= MIN_HELD_DIFFERENCE, name + " " + label + (phase ? ": strip rising" : ": strip up") + ", difference " + juce::String(heard, 4));
                expect(atRest <= W::MAX_REST_DIFFERENCE, name + " " + label + ": heard with the strip at rest (" + juce::String(atRest, 5) + ")");
                if (heard < 3.0f * D::MIN_DIFFERENCE)
                    quiet << label << " " << juce::String(heard, 3) << "  ";
            }
            if (quiet.isNotEmpty())
                logMessage(name + ", smallest differences: " + quiet);
        }

        beginTest("A route is in the destination's own scale: the strip fully up at +100 is an LFO's peak at full depth");
        {
            // Wavetable position: a square LFO at full depth holds POS + 1 for its first half
            // cycle; the strip at AMT +100 holds the same.
            auto viaStrip = D::makeEngine(EngineType::Wavetable);
            clearRoutes(*viaStrip);
            viaStrip->setParameter(S::stripDestId(0), (float) WavetableVoice::LfoPosition);
            viaStrip->setParameter(S::stripAmountId(0), 0.5f);
            auto viaLfo = D::makeEngine(EngineType::Wavetable);
            clearRoutes(*viaLfo);
            D::setLfo(*viaLfo, 0, (int) WavetableVoice::LfoPosition, 0.5f, D::SQR, D::KEY, 0.05f);     // 10 s high
            D::Plan a, b;
            a.engine = viaStrip;
            b.engine = viaLfo;
            const float d = W::difference(renderWithStrip(a, 1.0f), renderWithStrip(b, 0.0f));
            expect(d < 0.01f, "strip +50 on POS v. a square LFO at depth 50: " + juce::String(d, 4));
        }

        beginTest("WVIB: the strip brings in vibrato on every engine, at VRAT");
        for (auto type : stripEngines())
        {
            const auto name = D::engineName(type);
            ParamMap sound;
            for (int r = 0; r < S::NUM_STRIP; ++r)
                sound[S::stripAmountId(r)] = 0.0f;
            sound[S::STRIP_VIBRATO] = 1.0f;
            const auto rest = W::render(*W::makeEngine(type, &sound), 0.0f, 1.5);
            const auto up = W::render(*W::makeEngine(type, &sound), 1.0f, 1.5);
            expect(finite(up), name);
            const float d = W::difference(rest, up);
            expect(d >= D::MIN_DIFFERENCE, name + ": a semitone of vibrato changes the sound by only " + juce::String(d, 4));
            sound[S::STRIP_VIBRATO_RATE] = 11.0f;
            const float faster = W::difference(up, W::render(*W::makeEngine(type, &sound), 1.0f, 1.5));
            expect(faster >= 0.005f, name + ": VRAT is not heard (" + juce::String(faster, 4) + ")");
        }

        beginTest("A cell the strip was moving is back at its set value when the strip comes to rest");
        {
            // FM: operator 2's level is pushed into the voices while a route moves it.
            ParamMap sound { { S::stripDestId(0), 0.0f }, { S::STRIP_VIBRATO, 0.0f } };
            auto probe = D::makeEngine(EngineType::FM);
            const int lvl2 = D::indexOf(D::destLabels(*probe), "LVL2");
            expect(lvl2 > 0);
            sound[S::stripDestId(1)] = (float) lvl2;
            sound[S::stripAmountId(1)] = 1.0f;
            auto dip = [](EngineBase& e, int block) { e.setModWheel(block < (int) (0.3 * SR / BLOCK) ? 1.0f : 0.0f); };
            const auto rest = W::render(*W::makeEngine(EngineType::FM, &sound), 0.0f, 2.0);
            const auto dipped = W::render(*W::makeEngine(EngineType::FM, &sound), 0.0f, 2.0, dip);
            const auto up = W::render(*W::makeEngine(EngineType::FM, &sound), 1.0f, 2.0);
            const int from = (int) (1.0 * SR);
            const std::vector<float> tailRest(rest.begin() + from, rest.end()), tailDipped(dipped.begin() + from, dipped.end()),
                                     tailUp(up.begin() + from, up.end());
            expect(W::difference(tailRest, tailUp) >= D::MIN_DIFFERENCE, "the route is heard");
            expectWithinAbsoluteError(W::measure(tailDipped).centroid, W::measure(tailRest).centroid,
                                      0.02f * W::measure(tailRest).centroid + 1.0f, "brightness did not come back");
            expectWithinAbsoluteError(W::dB(W::measure(tailDipped).rms / W::measure(tailRest).rms), 0.0f, 0.3f, "level did not come back");

            // The same when the route is taken away, or its amount set to zero, with the strip up.
            for (int how = 0; how < 2; ++how)
            {
                auto undo = [how](EngineBase& e, int block)
                {
                    if (block == (int) (0.3 * SR / BLOCK))
                        e.setParameter(how == 0 ? S::stripDestId(1) : S::stripAmountId(1), 0.0f);
                };
                const auto undone = W::render(*W::makeEngine(EngineType::FM, &sound), 1.0f, 2.0, undo);
                const std::vector<float> tail(undone.begin() + from, undone.end());
                expectWithinAbsoluteError(W::measure(tail).centroid, W::measure(tailRest).centroid,
                                          0.02f * W::measure(tailRest).centroid + 1.0f, how == 0 ? "DST to OFF" : "AMT to 0");
            }
        }

        beginTest("The sampler takes back the level only for what the strip adds to DRIV");
        {
            // The default (DRIV +70) rises by a few dB; with the route on PAN nothing is compensated.
            auto level = [](const ParamMap& sound)
            {
                const auto rest = W::render(*W::makeEngine(EngineType::Sampler, &sound), 0.0f, 1.5);
                const auto up = W::render(*W::makeEngine(EngineType::Sampler, &sound), 1.0f, 1.5);
                return W::dB(W::measure(up).rms / std::max(W::measure(rest).rms, 1.0e-9f));
            };
            ParamMap pan;
            for (int r = 0; r < S::NUM_STRIP; ++r)
                pan[S::stripAmountId(r)] = 0.0f;
            pan[S::stripDestId(0)] = (float) SamplerEngine::LfoAmp;
            pan[S::stripAmountId(0)] = 0.0f;
            expectWithinAbsoluteError(level(pan), 0.0f, 0.05f, "no route: the level is the same");
            pan[S::stripAmountId(0)] = 1.0f;
            expectWithinAbsoluteError(level(pan), 6.0f, 0.5f, "AMP +100: twice the level, nothing taken back");
            const float driven = level({});
            expect(driven > 1.0f && driven < 6.0f, "default: " + juce::String(driven, 1) + " dB");
        }

        beginTest("Drums: the strip is read at the strike, at once (no smoothing), on every pad");
        {
            // One hit of the snare right after the strip moved: DEC +79 makes it ring three times as long.
            auto length = [](float strip)
            {
                DrumEngine e;
                e.prepareToPlay(SR, BLOCK);
                e.setParameter(S::stripAmountId(1), 0.0f);      // decay only
                e.setModWheel(strip);
                e.noteOn(12, 0.9f);
                juce::AudioBuffer<float> buf(2, BLOCK);
                juce::MidiBuffer midi;
                int lastLoud = 0;
                for (int b = 0; b < (int) (3.0 * SR / BLOCK); ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    if (buf.getMagnitude(0, BLOCK) > 0.003f)
                        lastLoud = b;
                }
                return (float) lastLoud * BLOCK / (float) SR;
            };
            const float rest = length(0.0f), up = length(1.0f);
            logMessage("snare length: strip at rest " + juce::String(rest, 3) + " s, up " + juce::String(up, 3) + " s");
            expect(rest > 0.02f && up > 2.2f * rest && up < 3.8f * rest, "about three times as long");
        }

        beginTest("Ids: every cell of every page is unique and lies in one of the engine blocks");
        {
            auto check = [this](const juce::String& name, EngineBase& e, bool drums)
            {
                // A parameter may be shown on two pages (FM: an operator's level on FMMIX and on its
                // own page), with the same range; two different cells never share an id, and the
                // STRIP and LFO cells are shown once.
                std::set<int> seen;
                std::map<int, ParamSpec> first;
                for (auto& page : e.getParamPages())
                    for (auto& p : page.params)
                    {
                        if (! p.isValid())
                            continue;
                        if (! seen.insert(p.id).second)
                        {
                            const auto& f = first[p.id];
                            expect(! S::owns(p.id) && f.minVal == p.minVal && f.maxVal == p.maxVal && f.kind == p.kind,
                                   name + ": id " + juce::String(p.id) + " (" + p.label + ", page " + page.name + ") is also " + f.label);
                        }
                        else
                            first[p.id] = p;
                        expect(EngineParamRange::contains(p.id), name + ": id " + juce::String(p.id) + " is outside the engine blocks");
                        expect(! (p.id >= AudioEngine::SEND_TYPE_PARAM_BASE && p.id < EngineParamRange::EXT_BASE),
                               name + ": id " + juce::String(p.id) + " is a send-bus type");
                        expect(! MixParam::isMixParam(p.id) && ! RangeParam::isRangeParam(p.id), name + ": id " + juce::String(p.id));
                        if (! drums)
                            expect(! StageCurveParam::owns(p.id), name + ": id " + juce::String(p.id) + " is a stage curve");
                    }
                return seen;
            };
            for (auto& [type, name] : W::soundEngines())
            {
                auto e = D::makeEngine(type);
                const auto ids = check(name, *e, type == EngineType::Drum);
                const bool hasStrip = type != EngineType::UnifiedVoice;
                for (int id = S::STRIP_BASE; id < S::STRIP_BASE + S::STRIP_COUNT; ++id)
                    expect((ids.count(id) > 0) == hasStrip || type == EngineType::UnifiedVoice, juce::String(name) + ": STRIP id " + juce::String(id));
                // 182-189 are the modulation router's XIN page: on the macro oscillator and FM, the engines
                // on the router so far; free on the others (the wave sequencer's BODY page, once there, is in
                // the engine's own block: the id rule at the top of ModRouter.h).
                const bool uses = type == EngineType::MacroOsc || type == EngineType::FM || type == EngineType::WaveSeq || type == EngineType::Wavetable || type == EngineType::Sampler
                                  || type == EngineType::Drum || type == EngineType::Resonator;    // (the engines on the router)
                if (hasStrip)
                    for (int id = S::STRIP_BASE + S::STRIP_COUNT; id < EngineParamRange::EXT_END; ++id)
                        expect((ids.count(id) > 0) == uses, juce::String(name) + (uses ? ": 182-189 are a page" : ": 182-189 stay free"));
            }
            for (int model = 0; model < 8; ++model)
            {
                MacroOscEngine e;
                e.setParameter(MacroOscEngine::ModelParam, (float) model);
                check("macro model " + juce::String(model), e, false);
            }
            expectEquals((int) S::STRIP_BASE, 174);
            expectEquals((int) S::STRIP_VIBRATO_RATE, 181);
            expect(S::owns(174) && S::owns(181) && ! S::owns(182) && S::ownsLfo(173) && ! S::ownsLfo(174) && S::ownsStrip(174));
        }

        beginTest("The cells are saved with a project and with a preset, and applied through the track");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            std::vector<std::vector<std::pair<int, float>>> expected;
            int t = 0;
            for (auto type : stripEngines())
            {
                audio.addEngine(t, AudioEngine::createEngine(type));
                seq.getPattern().getTrack(t).engineType = type;
                const int dests = (int) D::destLabels(*audio.getEngine(t)).size();
                std::vector<std::pair<int, float>> values;
                for (int r = 0; r < S::NUM_STRIP; ++r)
                {
                    values.push_back({ S::stripDestId(r), (float) (dests - 1 - r - t) });
                    values.push_back({ S::stripAmountId(r), -0.9f + 0.25f * (float) (r + t) });
                }
                values.push_back({ S::STRIP_VIBRATO, 0.1f * (float) (t + 1) });
                values.push_back({ S::STRIP_VIBRATO_RATE, 2.0f + (float) t });
                for (auto& [id, value] : values)
                {
                    audio.setTrackParameter(t, id, value);
                    seq.getPattern().getTrack(t).soundParams[id] = value;
                    expectWithinAbsoluteError(audio.getTrackParameter(t, id), value, 1.0e-5f, "track " + juce::String(t) + " id " + juce::String(id));
                    expectWithinAbsoluteError(audio.getEngine(t)->getParameter(id), value, 1.0e-5f);
                }
                expected.push_back(values);
                ++t;
            }
            // An amount locked on a step (more strip on that step).
            seq.getPattern().getTrack(0).getStep(3).paramLocks[S::stripAmountId(0)] = 0.25f;

            const auto data = ProjectData::fromJSON(ProjectBridge::capture({ seq, audio }, "STRIP").toJSON());
            SequencerCore seq2;
            AudioEngine audio2;
            audio2.setSequencer(&seq2);
            ProjectBridge::apply(data, { seq2, audio2 });
            for (size_t track = 0; track < expected.size(); ++track)
                for (auto& [id, value] : expected[track])
                    expectWithinAbsoluteError(audio2.getTrackParameter((int) track, id), value, 1.0e-4f,
                                              "project: track " + juce::String((int) track) + " id " + juce::String(id));
            auto& locks = seq2.getPattern().getTrack(0).getStep(3).paramLocks;
            expect(locks.count(S::stripAmountId(0)) > 0 && std::abs(locks[S::stripAmountId(0)] - 0.25f) < 1.0e-5f, "the step lock");

            // A step lock is applied and taken back like any other cell's.
            const float base = audio2.getTrackParameter(0, S::stripAmountId(0));
            audio2.applyStepLocks(0, seq2.getPattern().getTrack(0).getStep(3));
            expectWithinAbsoluteError(audio2.getEngine(0)->getParameter(S::stripAmountId(0)), 0.25f, 1.0e-5f, "locked step");
            audio2.applyStepLocks(0, Step());
            expectWithinAbsoluteError(audio2.getEngine(0)->getParameter(S::stripAmountId(0)), base, 1.0e-5f, "back at the base value");

            // A preset.
            for (size_t track = 0; track < expected.size(); ++track)
            {
                Preset preset;
                preset.name = "STRIP";
                preset.engineType = stripEngines()[track];
                if (auto* engine = audio.getEngine((int) track))
                    for (auto& page : engine->getParamPages())
                        for (auto& p : page.params)
                            if (p.isValid())
                                preset.soundParams[p.id] = engine->getParameter(p.id);
                const Preset loaded = Preset::fromJSON(preset.toJSON());
                expect(loaded.engineType == preset.engineType);
                auto fresh = W::makeEngine(loaded.engineType, &loaded.soundParams);
                for (auto& [id, value] : expected[track])
                    expectWithinAbsoluteError(fresh->getParameter(id), value, 1.0e-4f,
                                              "preset: " + D::engineName(preset.engineType) + " id " + juce::String(id));
            }
        }

        beginTest("The strip moves both LFOs through LRAT and LDPT");
        {
            // LFO 1 and LFO 2 both on the level; the strip at LDPT -100 stops both.
            auto make = [](float amount)
            {
                auto e = D::makeEngine(EngineType::Wavetable);
                clearRoutes(*e);
                const auto labels = D::destLabels(*e);
                D::setLfo(*e, 0, D::indexOf(labels, "AMP"), 0.6f, D::SIN, D::FREE, 7.0f);
                D::setLfo(*e, 1, D::indexOf(labels, "PAN"), 0.8f, D::SIN, D::FREE, 5.0f);
                e->setParameter(S::stripDestId(2), (float) D::indexOf(labels, "LDPT"));
                e->setParameter(S::stripAmountId(2), amount);
                return e;
            };
            auto plain = D::makeEngine(EngineType::Wavetable);
            clearRoutes(*plain);
            D::Plan a, b, c;
            a.engine = make(-1.0f);
            b.engine = plain;
            c.engine = make(-1.0f);
            const auto stopped = renderWithStrip(a, 1.0f), noLfos = renderWithStrip(b, 1.0f), running = renderWithStrip(c, 0.0f);
            expect(W::difference(stopped, noLfos) < 0.01f, "strip up: both LFOs are stopped (" + juce::String(W::difference(stopped, noLfos), 4) + ")");
            expect(W::difference(running, noLfos) > 0.05f, "strip at rest: both LFOs run");
        }
    }
};

static StripRoutesTests stripRoutesTests;
