#pragma once

#include "DSPUtils.h"
#include "GrainScheduler.h"
#include "ModeBank.h"
#include "Saturation.h"
#include <algorithm>
#include <array>
#include <cmath>

// -----------------------------------------------------------------------------
// One resonator and one exciter, both computed from latent parameters. No engine
// around it: prepare, set the latents (or a position among the named points), set
// the pitch, strike, then one sample at a time with an outside excitation and an
// energy. (The RSN engine is a thin shell around it; any voice may pass through it
// as a stage, fed by its own sound, in phase U3 / U4 of .ralph/spec-unify.md.)
//
// THE BODY is a bank of ringing modes (dsp/ModeBank.h): mode 0 is the hum, an
// octave under the note; modes 1..63 are the partials, the first always ON the
// note. Eight latents, each 0..1, say what the body is:
//   STIF  stiffness. Partial n of a stiff string is n sqrt(1 + B n^2): B = 0 is the ideal
//         string (harmonic), a little B the stretched partials of a piano string, and where
//         stiffness is everything the square law of a bar. With it the ends go from pinned
//         to free (n -> n + a, a up to 0.514), which lands on the free bar: 1, 2.76, 5.39, 8.89.
//         (B = STIF 10^(7 STIF - 4). A sheet's and a shell's partials take 0.3 % of it.)
//   SHT   from a line to a sheet: the partials go to those of a rectangular plate
//         (m^2 + (n / 1.37)^2: dense, irregular).
//   SHEL  to a shell: a church bell's partials above its strike note (1, 1.183 the minor
//         third, 1.506, 2, 2.514, 2.662, 3.011, 4.166 ...). (Line, sheet and shell share each
//         partial's place: line = what SHT and SHEL leave of 1, and the three are weighed.)
//   ODD   the even partials leave (a tube closed at one end; a comb with a negative loop).
//   DAMP  loss against frequency: a partial at r times the note dies (1 + DAMP^3 (r^2 - 1))
//         times as fast (0: every partial rings as long, a comb filter; 1: wood).
//   TILT  level against frequency: 1 / (1 + 4 TILT^2 (r - 1)).
//   HUM   the level of the hum.
//   POS   where it is touched and driven, from an end (0: every partial alike) to the
//         middle (1: the even ones are not reached): cos((n - 1) POS pi / 2).
// STRG TUBE COMB PLAT BAR BELL are six points in that space (bodyPoints). A position 0..5
// is a point between two neighbours, latent by latent; the offsets are added to it. The
// partials move, the modes keep ringing, and the first partial stays on the note.
//
// THE EXCITER has five latents:
//   CONT  from one burst (0) to fed for as long as there is energy (1).
//   GRAN  what it is made of: one impulse (0), sparse irregular impulses, dense noise (1).
//   COUP  how much it reacts to the body's own motion where it touches: none for a strike
//         or a scrape; with it the exciter is a junction that pulls the contact's motion
//         towards a drive, by a grip that depends on the difference between the two
//         (McIntyre, Schumacher and Woodhouse 1983: bow and reed are the same loop with
//         two curves).
//   REED  that curve, from the bow's (0: it holds while the difference is small and lets
//         go when it grows: stick and slip) to the reed's (1: an opening that the
//         difference closes). With a curve of dsp/Saturation.h chosen (setCurve) the bow's
//         grip is that curve's (output / input)^4 and the reed's opening 1 - curve.
//   HARD  dull to bright: the excitation's low-pass, from the note to 128 times it, and how
//         soon a junction lets go (the bow's hold reaches 1.2 - HARD bow speeds).
// [U3 grains] GRAN's impulses are noise until a recording is given (setGrainSource): then they are
// grains of it (dsp/GrainScheduler.h), from 2 a second to 400, read where setGrains says - the
// body is scraped with fragments of a sample. Everything else about the exciter stays as it is.
// STRK BOW BRTH SCRP BRSH are five points (excitePoints), with a position 0..4.
// [U2b] Eight more latents of the body (MEMB DROP WIRE MEET SPRD PAIR HAND DIR: see `More` below),
// eight more points (HEAD, a drum head, and seven bodies with partials placed by hand: handSets).
//
// Energy drives all of it (the engine gives velocity x envelope, or pad pressure): a strike
// takes it once, a fed exciter follows it, and a junction's drive and grip are zero with it,
// so a bow that is lifted lets the body ring on and die by itself.
//
// Bounded for any input: the junction can only move the contact towards its drive (its grip is
// never negative, and it is solved with the body's own answer in it, so no setting makes it
// ring by itself); what comes from outside is held (softLimit); and what the bank holds is
// measured at control rate and scaled down beyond a ceiling. Partials that have rung out are
// put to rest there too: a body struck once soon runs only its low ones, a silent one none.
// Coefficients are computed at control rate without a transcendental per mode (polynomial
// sine and cosine, rational decays), and only when a latent, the pitch or the decay moved.
// -----------------------------------------------------------------------------
namespace res
{
enum Body { Stiff = 0, Sheet, Shell, Odd, Damp, Tilt, Hum, Pos, NumBody };
enum Excite { Cont = 0, Grain, Couple, Shape, Hard, NumExcite };
// [U2b] Eight more latents of the body (a second array, so that whoever sets the first eight alone
// - a host written before them - gets what the points give and nothing else changes):
//   MEMB  to a membrane: the partials of a round head (1, 1.59, 2.14, 2.30, 2.65, 2.92 ...), a
//         share beside line, sheet and shell. Head against shell is MEMB against SHEL.
//   DROP  the pitch rises with how hard the body moves (as the square of it) and falls back
//         as it dies (a head's tension under the stroke, a gong's glide): up to an octave at
//         the loudest.
//   WIRE  a noisy contact: something lies against the body and rattles with its motion - the
//         snare wires on a head, the sizzle of two plates of a hat. Noise as loud as the body
//         moves, a moment longer than it.
//   MEET  which partial a bow or breath meets on a heavy body (0: the first, as before; 1: the
//         sixteenth), and
//   SPRD  how many around it (0: that one alone; 1: a dozen each side). Both at 0: as it was.
//   PAIR  every second partial comes to sit beside the one under it, a few parts in a thousand
//         above: a pair that beats, each at its own rate (the two forms of one mode of a round
//         body that is not quite round).
//   HAND  to partials placed by hand: a table of places, levels and widths (how soon each dies)
//         per mode - that of the point the position is at (handSets) - a share like MEMB.
//   DIR   the exciter itself beside the body: the stick's click, the brush's or the breath's hiss.
enum More { Membrane = 0, Drop, Wire, Meet, Spread, Pair, Hand, Direct, NumMore };
// (Positions are stored by number: points are only ever added at the end.)
enum BodyPoint { String = 0, Tube, Comb, Plate, Bar, Bell, Head, Gong, Tam, Crash, Ride, Cup, China, Splash, NumBodyPoints };
enum ExcitePoint { Strike = 0, Bow, Breath, Scrape, Brush, NumExcitePoints };

using BodyLatent = std::array<float, NumBody>;
using ExciteLatent = std::array<float, NumExcite>;
using MoreLatent = std::array<float, NumMore>;

inline const std::array<BodyLatent, NumBodyPoints>& bodyPoints()
{
    static const std::array<BodyLatent, NumBodyPoints> p = {{
        //  STIF   SHT   SHEL  ODD   DAMP   TILT   HUM   POS
        { { 0.05f, 0.0f, 0.0f, 0.0f, 0.30f, 0.25f, 0.0f, 0.25f } },    // STRG
        { { 0.00f, 0.0f, 0.0f, 1.0f, 0.35f, 0.45f, 0.0f, 0.00f } },    // TUBE
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } },    // COMB
        { { 0.00f, 1.0f, 0.0f, 0.0f, 0.25f, 0.20f, 0.0f, 0.00f } },    // PLAT
        { { 1.00f, 0.0f, 0.0f, 0.0f, 0.25f, 0.25f, 0.0f, 0.00f } },    // BAR
        { { 0.00f, 0.0f, 1.0f, 0.0f, 0.15f, 0.20f, 0.5f, 0.00f } },    // BELL
        // [U2b] HEAD: a drum head. Then seven bodies whose partials are placed by hand (HAND 1:
        // handSets); what the eight latents add to those is left at nothing.
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.45f, 0.35f, 0.0f, 0.00f } },    // HEAD
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } },    // GONG
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } },    // TAM
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } },    // CRSH
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } },    // RIDE
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } },    // CUP
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } },    // CHNA
        { { 0.00f, 0.0f, 0.0f, 0.0f, 0.00f, 0.00f, 0.0f, 0.00f } } }}; // SPLS
    return p;
}

// [U2b] What the points give of the second eight.
inline const std::array<MoreLatent, NumBodyPoints>& morePoints()
{
    static const std::array<MoreLatent, NumBodyPoints> p = {{
        //  MEMB  DROP   WIRE  MEET  SPRD  PAIR  HAND  DIR
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },    // STRG
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },    // TUBE
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },    // COMB
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },    // PLAT
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },    // BAR
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },    // BELL
        { { 1.0f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },    // HEAD
        { { 0.0f, 0.12f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } },    // GONG
        { { 0.0f, 0.08f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } },    // TAM
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } },    // CRSH
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } },    // RIDE
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } },    // CUP
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } },    // CHNA
        { { 0.0f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } } }}; // SPLS
    return p;
}

inline const std::array<ExciteLatent, NumExcitePoints>& excitePoints()
{
    static const std::array<ExciteLatent, NumExcitePoints> p = {{
        //  CONT  GRAN   COUP   REED  HARD
        { { 0.0f, 0.00f, 0.00f, 0.0f, 0.70f } },    // STRK
        { { 1.0f, 0.00f, 0.75f, 0.0f, 0.60f } },    // BOW
        { { 1.0f, 1.00f, 0.75f, 1.0f, 0.45f } },    // BRTH
        { { 1.0f, 0.45f, 0.00f, 0.5f, 0.70f } },    // SCRP
        // [U2b] BRSH: wires drawn over it - dense fine grain, soft, no grip; fed for as long as
        // there is energy, and a tap is a burst of that grain some tens of milliseconds long.
        { { 0.4f, 0.80f, 0.00f, 0.0f, 0.35f } } }}; // BRSH
    return p;
}

// ---- U2, the cymbal machine (engine/CymbalMachine.h) ---------------------------------------
// The cymbals' region of the body's space: sheets and shells whose top is lost fast (a partial
// 30 times the note dies a hundred or more times sooner, so that noise fed into it comes out as
// a band of hiss, and what is struck rings on in the low partials alone). They are points like
// the six above, kept out of that list because a position among those is stored by number.
enum CymbalPoint { CymRide = 0, CymBell, CymCrash, CymChina, CymSplash, CymGong, CymTam, NumCymbalPoints };

inline const std::array<BodyLatent, NumCymbalPoints>& cymbalPoints()
{
    static const std::array<BodyLatent, NumCymbalPoints> p = {{
        //  STIF   SHT    SHEL   ODD   DAMP   TILT   HUM   POS
        { { 0.55f, 1.00f, 0.00f, 0.00f, 0.75f, 0.00f, 0.00f, 0.00f } },  // RIDE: a heavy sheet
        { { 0.00f, 0.30f, 0.70f, 0.00f, 0.25f, 0.20f, 0.00f, 0.00f } },  // BELL: its cup, more shell than sheet
        { { 0.60f, 1.00f, 0.00f, 0.00f, 0.92f, 0.00f, 0.00f, 0.00f } },  // CRSH: a thin sheet
        { { 0.60f, 1.00f, 0.00f, 0.00f, 0.90f, 0.00f, 0.00f, 0.40f } },  // CHNA: stretched and uneven
        { { 0.55f, 1.00f, 0.00f, 0.00f, 0.85f, 0.00f, 0.00f, 0.00f } },  // SPLS: a small thin sheet
        { { 0.55f, 0.60f, 0.40f, 0.00f, 0.30f, 0.30f, 0.40f, 0.00f } },  // GONG: a domed sheet with a note and a hum
        { { 0.66f, 1.00f, 0.00f, 0.00f, 0.50f, 0.00f, 0.30f, 0.00f } } }}; // TAM: a large flat sheet, its partials spread far up
    return p;
}
// ---- (end of U2) ---------------------------------------------------------------------------

// The latents at a position among the points, plus the offsets, each held to 0..1.
template <size_t Points, size_t N>
std::array<float, N> latentAt(const std::array<std::array<float, N>, Points>& points, float position,
                              const std::array<float, N>& offset)
{
    const float top = (float) (Points - 1);
    const float p = position >= 0.0f ? std::min(position, top) : 0.0f;     // (not-a-number is 0)
    const size_t a = std::min((size_t) p, Points - 2);
    const float t = p - (float) a;
    std::array<float, N> out;
    for (size_t i = 0; i < N; ++i)
    {
        // (At a point: the point itself, to the last bit.)
        const float at = t < 1.0f ? points[a][i] + (points[a + 1][i] - points[a][i]) * t : points[a + 1][i];
        const float v = at + (std::isfinite(offset[i]) ? offset[i] : 0.0f);
        out[i] = std::clamp(v, 0.0f, 1.0f);
    }
    return out;
}

// --- [U3 grains] ---------------------------------------------------------------------
// A recording the exciter's grains are read from: one channel that stays where it is for as long
// as it is in use (a sample of the bank lives for the whole run). gain: what brings it to a level
// of about 1 (1 / its RMS, held).
struct GrainSource
{
    const float* data = nullptr;
    int frames = 0;
    double rate = 44100.0;
    float gain = 1.0f;
};

// Where and how the grains are read.
struct GrainSet
{
    float position = 0.0f;      // 0..1 of the recording: where grains start
    float scan = 1.0f;          // how fast that place moves on, in the recording's own speed (0: it stays; wraps)
    float scatter = 0.0f;       // 0..1: each grain up to +-250 ms from there, and their onsets as irregular
    float size = 0.03f;         // a grain's length, seconds
    float pitch = 0.0f;         // semitones on the recording's own speed
    float pitchJitter = 0.0f;   // 0..1: each grain up to +-12 semitones off (squared, as on the sampler)
    int window = grn::Hann;
};
// --- [U3 grains] end -----------------------------------------------------------------

static constexpr int MODES = dsp::ModeBank::MAX_MODES;
static constexpr float FREE_END = 0.514f;      // what a bar's free ends add to the partial number
static constexpr float WIDE_STIFF = 0.003f;    // how much of STIF a sheet's and a shell's partials take

// What the body's partials are measured against. Entry n is partial n (1 = the note); entry 0 is unused.
struct Series
{
    alignas(32) float sheet[MODES] {}, shell[MODES] {}, number[MODES] {}, even[MODES] {};
    alignas(32) float head[MODES] {}, beat[MODES] {};       // [U2b] a round membrane; how far above its neighbour a paired partial sits
};

inline const Series& series()
{
    static const Series all = []
    {
        Series s;
        // A rectangular plate held at its edges, sides 1.37 : 1, its lowest partials in order.
        std::array<float, 20 * 20> plate {};
        for (int m = 1; m <= 20; ++m)
            for (int n = 1; n <= 20; ++n)
                plate[(size_t) ((m - 1) * 20 + n - 1)] = (float) (m * m) + (float) (n * n) / (1.37f * 1.37f);
        std::sort(plate.begin(), plate.end());
        // A church bell from its strike note up (prime, tierce, quint, nominal ...); beyond the
        // table the partials go on as they end, each step a little wider than the last.
        static const float bell[] = { 1.0f, 1.183f, 1.506f, 2.0f, 2.514f, 2.662f, 3.011f, 4.166f, 5.433f, 6.796f, 8.215f, 9.71f,
                                      11.26f, 12.87f, 14.53f, 16.24f, 18.0f, 19.8f, 21.65f, 23.54f, 25.47f, 27.44f, 29.45f };
        const int known = (int) (sizeof(bell) / sizeof(bell[0]));
        float step = bell[known - 1] - bell[known - 2];
        // [U2b] An ideal round membrane: the zeros of the Bessel functions, in order, over the first.
        static const float membrane[MODES - 1] = {
            1.0000f, 1.5933f, 2.1355f, 2.2954f, 2.6531f, 2.9173f, 3.1555f, 3.5001f, 3.5985f, 3.6475f, 4.0589f, 4.1317f, 4.2304f,
            4.6010f, 4.6101f, 4.8319f, 4.9033f, 5.0836f, 5.1308f, 5.4121f, 5.5404f, 5.5531f, 5.6508f, 5.9765f, 6.0194f, 6.1526f,
            6.1631f, 6.2087f, 6.4827f, 6.5286f, 6.6690f, 6.7462f, 6.8490f, 6.9436f, 7.0707f, 7.1694f, 7.3253f, 7.4024f, 7.4682f,
            7.5145f, 7.6045f, 7.6652f, 7.8592f, 7.8925f, 8.0710f, 8.1314f, 8.1569f, 8.1569f, 8.3143f, 8.4500f, 8.6451f, 8.6522f,
            8.6605f, 8.7678f, 8.7811f, 8.8204f, 8.9992f, 9.1301f, 9.1678f, 9.2200f, 9.2388f, 9.3906f, 9.4643f };
        for (int n = 1; n < MODES; ++n)
        {
            s.head[n] = membrane[n - 1];
            // (PAIR: 1.2 to 4 parts in a thousand, no two pairs alike.)
            const float golden = (float) n * 0.61803399f;
            s.beat[n] = n % 2 == 0 ? 1.0f + 0.0012f * (1.0f + 2.3f * (golden - std::floor(golden))) : 0.0f;
            s.number[n] = (float) (n - 1);
            s.even[n] = n % 2 == 0 ? 1.0f : 0.0f;
            s.sheet[n] = plate[(size_t) (n - 1)] / plate[0];
            if (n <= known)
                s.shell[n] = bell[n - 1];
            else
                s.shell[n] = s.shell[n - 1] + (step += 0.04f);
        }
        return s;
    }();
    return all;
}

// --- [U2b] Partials placed by hand ------------------------------------------------------------
// A table per mode: where it is (over the first), how strongly it is driven and heard, and how
// much sooner it dies than the first (its width as a band). Entry n is partial n (1 = the note).
// The seven here are the cymbal machine's bodies as they were before it moved onto the latents:
// twelve partials placed and levelled by hand - among them the pairs that beat (the gong's
// lowest, two of the cup's) - and thirty-six more spread over a band, strongest in its middle,
// which fed with noise is the wash. Read as HAND comes up, by the body points of the same names;
// another table is one more entry here.
struct HandSet
{
    alignas(32) float ratio[MODES] {}, level[MODES] {}, width[MODES] {};   // level: its root (it acts on the way in and on the way out)
    float pitch = 220.0f;       // the first partial as its maker tuned it, Hz
};

enum HandPoint { HandGong = 0, HandTam, HandCrash, HandRide, HandCup, HandChina, HandSplash, NumHandSets };

inline const std::array<HandSet, NumHandSets>& handSets()
{
    static const std::array<HandSet, NumHandSets> all = []
    {
        struct Def
        {
            int hash;                   // (which face of irregularities: the old machine's model number)
            float ping[12], db[12];
            float pingT, pingLevel, washLo, washHi, washPeak, washSigma, washLevel, trashDb;
        };
        static const Def defs[NumHandSets] = {
            { 7, { 196.0f, 197.4f, 393.0f, 471.0f, 588.0f, 762.0f, 969.0f, 1193.0f, 1466.0f, 1795.0f, 2190.0f, 2660.0f },
              { 0.0f, -5.0f, -4.0f, -8.0f, -9.0f, -11.0f, -13.0f, -15.0f, -17.0f, -19.0f, -21.0f, -23.0f },
              1.5f, 1.0f, 350.0f, 7000.0f, 1500.0f, 1.5f, 0.35f, 3.0f },                                             // GONG
            { 8, { 71.0f, 92.5f, 118.0f, 141.0f, 167.0f, 203.0f, 248.0f, 289.0f, 341.0f, 397.0f, 466.0f, 541.0f },
              { -2.0f, 0.0f, -1.0f, -3.0f, -2.0f, -4.0f, -4.0f, -6.0f, -6.0f, -8.0f, -9.0f, -10.0f },
              2.0f, 0.6f, 300.0f, 10000.0f, 2500.0f, 1.8f, 1.2f, 3.0f },                                             // TAM
            { 4, { 287.0f, 398.0f, 523.0f, 741.0f, 1012.0f, 1367.0f, 1790.0f, 2333.0f, 2954.0f, 3731.0f, 4610.0f, 5720.0f },
              { -3.0f, -2.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f, -9.0f },
              1.0f, 0.15f, 600.0f, 16000.0f, 4800.0f, 1.7f, 1.0f, 3.0f },                                            // CRSH
            { 2, { 431.0f, 692.0f, 1043.0f, 1489.0f, 2078.0f, 2811.0f, 3594.0f, 4467.0f, 5486.0f, 6619.0f, 7906.0f, 9372.0f },
              { -20.0f, -16.0f, -12.0f, -8.0f, -3.0f, -3.0f, 0.0f, -2.0f, -1.0f, -4.0f, -6.0f, -9.0f },
              0.12f, 1.3f, 900.0f, 15000.0f, 5200.0f, 1.3f, 0.6f, 3.0f },                                            // RIDE
            { 3, { 622.0f, 931.0f, 938.5f, 1352.0f, 1836.0f, 2444.0f, 2459.0f, 3087.0f, 3861.0f, 4688.0f, 5637.0f, 6719.0f },
              { -4.0f, 0.0f, -6.0f, -2.0f, -1.0f, -3.0f, -9.0f, -4.0f, -5.0f, -7.0f, -9.0f, -11.0f },
              1.0f, 1.0f, 1500.0f, 14000.0f, 6000.0f, 1.2f, 0.25f, 3.0f },                                           // CUP
            { 5, { 498.0f, 713.0f, 1021.0f, 1260.0f, 1594.0f, 2011.0f, 2530.0f, 2988.0f, 3620.0f, 4387.0f, 5190.0f, 6240.0f },
              { -5.0f, -3.0f, 0.0f, -2.0f, 0.0f, -1.0f, -2.0f, -1.0f, -4.0f, -5.0f, -7.0f, -9.0f },
              0.6f, 0.6f, 700.0f, 13000.0f, 3000.0f, 1.3f, 1.0f, 7.0f },                                             // CHNA
            { 6, { 812.0f, 1190.0f, 1675.0f, 2260.0f, 2985.0f, 3790.0f, 4760.0f, 5810.0f, 7020.0f, 8390.0f, 9900.0f, 11600.0f },
              { -3.0f, -1.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f, -9.0f },
              0.8f, 0.4f, 1800.0f, 17000.0f, 7500.0f, 1.3f, 1.0f, 3.0f } };                                          // SPLS
        // A number in 0..1 that is always the same for the same body and place.
        auto hash01 = [](int model, int i)
        {
            uint32_t h = (uint32_t) (model * 7919 + i * 104729 + 12345);
            h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
            return (float) (h >> 8) * (1.0f / 16777216.0f);
        };
        // How long a partial rings against one in the middle of the band (the old DAMP at its middle).
        auto rings = [](float ratio) { return std::min(2.5f, std::pow(ratio, -0.64f)); };
        static constexpr int PING = 12, WASH = 36;
        std::array<HandSet, NumHandSets> sets;
        for (int k = 0; k < NumHandSets; ++k)
        {
            const Def& d = defs[k];
            struct Mode { float hz, level, seconds; };
            std::array<Mode, PING + WASH> m {};
            float sumSq = 0.0f;
            for (int i = 0; i < PING; ++i)
            {
                m[(size_t) i] = { d.ping[i], std::pow(10.0f, d.db[i] / 20.0f), d.pingT * rings(d.ping[i] / d.washPeak) };
                sumSq += m[(size_t) i].level * m[(size_t) i].level;
            }
            for (int i = 0; i < PING; ++i)
                m[(size_t) i].level *= d.pingLevel / std::sqrt(sumSq);
            const float step = std::pow(d.washHi / d.washLo, 1.0f / (float) (WASH - 1));
            sumSq = 0.0f;
            for (int i = 0; i < WASH; ++i)
            {
                // Irregular spacing and uneven strengths, the same always: the instrument's own face.
                const float f = d.washLo * std::pow(step, (float) i + 0.7f * (hash01(d.hash, i) - 0.5f));
                const float octaves = std::log2(f / d.washPeak) / d.washSigma;
                const float level = std::exp(-0.5f * octaves * octaves)
                                    * std::pow(10.0f, d.trashDb * (hash01(d.hash, i + 100) * 2.0f - 1.0f) / 20.0f);
                m[(size_t) (PING + i)] = { f, level, rings(f / d.washPeak) };
                sumSq += level * level;
            }
            for (int i = 0; i < WASH; ++i)
                m[(size_t) (PING + i)].level *= d.washLevel / std::sqrt(sumSq);
            std::sort(m.begin(), m.end(), [](const Mode& a, const Mode& b) { return a.hz < b.hz; });
            float loudest = 0.0f;
            for (const auto& mode : m)
                loudest = std::max(loudest, mode.level);
            HandSet& set = sets[(size_t) k];
            set.pitch = m[0].hz;
            for (int n = 1; n < MODES; ++n)
            {
                if (n <= PING + WASH)
                {
                    const Mode& mode = m[(size_t) (n - 1)];
                    set.ratio[n] = mode.hz / m[0].hz;
                    set.level[n] = std::sqrt(mode.level / loudest);
                    set.width[n] = std::clamp(m[0].seconds / mode.seconds, 0.05f, 50.0f);
                }
                else        // (what the table does not place is silent, above the rest)
                {
                    set.ratio[n] = set.ratio[n - 1] * 1.03f;
                    set.level[n] = 0.0f;
                    set.width[n] = set.width[n - 1];
                }
            }
        }
        return sets;
    }();
    return all;
}

// Which table a body point reads as HAND comes up: its own, or (the points that have none) the gong's.
inline int handOf(int point)
{
    static constexpr int own[NumBodyPoints] = { HandGong, HandGong, HandGong, HandGong, HandGong, HandGong, HandGong,
                                                HandGong, HandTam, HandCrash, HandRide, HandCup, HandChina, HandSplash };
    return own[std::clamp(point, 0, (int) NumBodyPoints - 1)];
}
// --- [U2b] end ---------------------------------------------------------------------------------

// Bounded and monotonic, slope 1 at 0, +-limit far out; not-a-number gives 0.
inline float softLimit(float x, float limit)
{
    if (! (std::abs(x) < 1.0e6f))
        return x > 0.0f ? limit : (x < 0.0f ? -limit : 0.0f);
    const float r = x / limit;
    return x / std::sqrt(1.0f + r * r);
}

// What is heard: untouched up to +-1 (no sound that fits pays for the limit), then bent to +-limit.
inline float outLimit(float x, float limit)
{
    const float a = std::abs(x);
    if (a <= 1.0f)
        return x;
    if (a != a)
        return 0.0f;        // (not-a-number is silence, never a level)
    return std::copysign(1.0f + softLimit(a - 1.0f, limit - 1.0f), x);
}

// The cosine of an angle up to a quarter turn, from its square.
inline float cosSeries(float a2)
{
    return 1.0f + a2 * (-0.5f + a2 * (1.0f / 24.0f + a2 * (-1.0f / 720.0f + a2 * (1.0f / 40320.0f + a2 * (-1.0f / 3628800.0f)))));
}

// Sine and cosine of 0..pi by their series about the nearer end (seven-figure accuracy; the
// compiler can do eight at a time).
inline void sinCos(float w, float& s, float& c)
{
    const bool far = w > 1.5707964f;
    const float a = far ? 3.1415927f - w : w, a2 = a * a;
    s = a * (1.0f + a2 * (-1.0f / 6.0f + a2 * (1.0f / 120.0f + a2 * (-1.0f / 5040.0f + a2 * (1.0f / 362880.0f)))));
    const float k = cosSeries(a2);
    c = far ? -k : k;
}

// The cosine of `turns` whole turns.
inline float cosTurns(float turns)
{
    const float t = std::abs(turns - std::floor(turns + 0.5f));     // 0 .. 0.5
    const bool far = t > 0.25f;
    const float a = 6.2831855f * (far ? 0.5f - t : t), a2 = a * a;
    const float k = cosSeries(a2);
    return far ? -k : k;
}

class Resonator
{
public:
    static constexpr double MIN_HZ = 4.0;
    static constexpr float MAX_ENERGY = 2.0f;
    static constexpr float RESTS = 1.0e-14f;        // eight partials that hold less than this are put to rest (-140 dB)
    static constexpr float HELD = 64.0f;            // the most the bank may hold (sum of squares)
    static constexpr float OUT_LIMIT = 4.0f;
    static constexpr int PHYSICAL = 0;              // setCurve: the bow's and the reed's own curves

    void prepare(double rate)
    {
        sampleRate = rate >= 1.0 ? rate : 44100.0;
        // The cells are looked at about 3000 times a second, whatever the rate.
        period = std::clamp((int) std::lround(sampleRate / 3000.0), 8, 256);
        glideK = (float) (1.0 - std::exp(-(double) period / (0.0015 * sampleRate)));
        series();       // built here (never on the audio thread)
        invRate = 1.0 / sampleRate;                                             // [U3 grains]
        seenCont = seenGrain = seenHard = seenPitch = seenCouple = -1.0f;
        seenBody.fill(-1.0f);
        seenMore.fill(-1.0f);
        seenHandA = seenHandB = -1;
        motionDecay = (float) std::exp(-1.0 / (0.03 * sampleRate));
        wireDecay = (float) std::exp(-1.0 / (0.04 * sampleRate));
        bodyDirty = exciteDirty = true;
        reset();
    }

    void reset()
    {
        bank.clearState();
        lp1 = lp2 = turbulence = contact = burst = 0.0f;
        motion = wireLp = wireTop = wireTop2 = wireEnv = direct1 = direct2 = 0.0f;     // [U2b]
        bendNow = bendSeen = 1.0f;
        onset = false;
        fresh = true;
        countdown = 0;
        cloud.reset();          // [U3 grains]
        travel = 0.0;
        if (source != wanted)
        {
            source = wanted;
            exciteDirty = true;
        }
    }

    // --- What it is -------------------------------------------------------------------
    // The latents themselves, each held to 0..1 (what is no number is 0) ...
    void setBody(BodyLatent latent)
    {
        for (auto& v : latent) v = v >= 0.0f ? std::min(v, 1.0f) : 0.0f;
        if (latent != bodyTarget) { bodyTarget = latent; bodyDirty = true; }
    }
    void setExciter(ExciteLatent latent)
    {
        for (auto& v : latent) v = v >= 0.0f ? std::min(v, 1.0f) : 0.0f;
        if (latent != exciteTarget) { exciteTarget = latent; exciteDirty = true; }
    }
    // ... or a position among the named points, and what is added to the point there.
    // (A position also gives the second eight latents and the table HAND reads: morePoints, handOf.)
    void setBody(float position, const BodyLatent& offset = {})
    {
        setBody(latentAt(bodyPoints(), position, offset));
        const float p = position >= 0.0f ? std::min(position, (float) (NumBodyPoints - 1)) : 0.0f;
        const int a = std::min((int) p, (int) NumBodyPoints - 2);
        morePoint = latentAt(morePoints(), p, MoreLatent {});
        const int ha = handOf(a), hb = handOf(a + 1);
        const float mix = ha == hb ? 0.0f : p - (float) a;
        if (ha != handA || hb != handB || mix != handMixTarget)
        {
            // (Past a point the pair of tables is another: where the mix stood between the old two
            // is said in the new two, or the body would leap to the far table and glide back.)
            if (ha != handA || hb != handB)
            {
                const int shown = handMixNow >= 0.5f ? handB : handA;
                if (ha == shown)        handMixNow = 0.0f;
                else if (hb == shown)   handMixNow = 1.0f;
            }
            handA = ha;
            handB = hb;
            handMixTarget = mix;
            bodyDirty = true;
        }
        placeMore();
    }
    // [U2b] The second eight latents: what is added to those of the position (to nothing, where
    // the body was given as latents), the sum held to 0..1; what is no number is 0.
    void setMore(MoreLatent offset)
    {
        for (auto& v : offset) v = std::abs(v) <= 1.0f ? v : (v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : 0.0f));
        if (offset != moreOffset) { moreOffset = offset; placeMore(); }
    }
    const MoreLatent& more() const      { return moreNow; }
    void setExciter(float position, const ExciteLatent& offset = {}) { setExciter(latentAt(excitePoints(), position, offset)); }
    // 0 = the bow's and the reed's own curves; n = curve n - 1 of dsp/Saturation.h.
    void setCurve(int c)        { curve = std::clamp(c, 0, (int) sat::Count); }
    // The time the note takes to fall by 60 dB, seconds.
    void setDecay(float seconds)
    {
        const float t = seconds >= 0.01f ? std::min(seconds, 60.0f) : 0.01f;
        if (t != decay) { decay = t; bodyDirty = true; }
    }
    void setPitch(double hz)
    {
        const float f = (float) (hz >= MIN_HZ ? std::min(hz, 0.45 * sampleRate) : MIN_HZ);
        if (f != pitch) { pitch = f; bodyDirty = exciteDirty = true; }
    }

    void seedNoise(uint32_t s)  { seed = s != 0 ? s : 1u; cloud.seed(seed ^ 0x51ed270bu); wireSeed = (seed * 2654435761u) | 1u; }

    // --- [U3 grains] ---------------------------------------------------------------------
    // What GRAN's impulses are read from; nullptr: noise, as without it. A change waits for the
    // grains that sound to end (no grain is cut or handed another recording).
    void setGrainSource(const GrainSource* s)
    {
        wanted = s != nullptr && s->data != nullptr && s->frames >= 2 && s->rate >= 1.0 ? s : nullptr;
    }
    void setGrains(const GrainSet& g)
    {
        GrainSet n;
        n.position = g.position >= 0.0f ? std::min(g.position, 1.0f) : 0.0f;
        n.scan = std::abs(g.scan) <= 4.0f ? g.scan : 0.0f;
        n.scatter = g.scatter >= 0.0f ? std::min(g.scatter, 1.0f) : 0.0f;
        n.size = g.size >= MIN_GRAIN ? std::min(g.size, MAX_GRAIN) : MIN_GRAIN;
        n.pitch = std::abs(g.pitch) <= 48.0f ? g.pitch : 0.0f;
        n.pitchJitter = g.pitchJitter >= 0.0f ? std::min(g.pitchJitter, 1.0f) : 0.0f;
        n.window = std::clamp(g.window, 0, (int) grn::NumWindows - 1);
        if (n.position != grains.position || n.scan != grains.scan || n.scatter != grains.scatter || n.size != grains.size
            || n.pitch != grains.pitch || n.pitchJitter != grains.pitchJitter || n.window != grains.window)
        {
            grains = n;
            exciteDirty = true;
        }
    }
    int grainsSounding() const          { return cloud.sounding(); }
    float grainDensity() const          { return density; }     // grains a second, as GRAN stands
    bool readsGrains() const            { return source != nullptr; }
    const GrainSource* grainSource() const { return source; }  // what the grains that sound are read from
    static constexpr int GRAIN_POOL = 16;
    static constexpr float MIN_GRAIN = 0.002f, MAX_GRAIN = 0.25f;
    static constexpr float GRAIN_SCATTER_SECONDS = 0.25f, MAX_GRAIN_DENSITY = 400.0f;
    // --- [U3 grains] end -----------------------------------------------------------------

    // --- U2 (VOC's mode TUNE): a spectrum laid over the partials -----------------------
    // One gain per mode (entry 0 the hum, entry n partial n; MODES entries, each held to
    // 0..MAX_SPECTRUM, what is no number is 0), multiplied into the weight with which that
    // mode is driven - by the exciter, by what comes from outside - and felt by a junction.
    // The body's own level (what a strike or a steady sound comes out at) is worked out
    // without it: the spectrum is a level, not made up for. Taken at the next look at the
    // cells and glided to, with no sine or cosine worked out again. nullptr: none again
    // (every gain 1). A body that was never given one pays nothing for it.
    static constexpr float MAX_SPECTRUM = 1.0e6f;
    void setSpectrum(const float* gains)
    {
        if (gains == nullptr)
        {
            if (spectral) { spectral = false; bodyDirty = true; }
            return;
        }
        for (int n = 0; n < MODES; ++n)
            spectrum[n] = gains[n] >= 0.0f ? std::min(gains[n], MAX_SPECTRUM) : 0.0f;
        if (! spectral) bodyDirty = true;       // (the first: the body is told again, and kept)
        spectral = spectrumDirty = true;
    }

    const BodyLatent& body() const      { return bodyNow; }
    const ExciteLatent& exciter() const { return exciteNow; }
    float moving() const                { return motion; }      // [U2b] how hard the body moves: its output's peak, 30 ms back
    float pitchBend() const             { return bendNow; }     // ... and what DROP makes of it, as a ratio
    int modesRunning() const            { return bank.running; }
    int modesAwake() const              { return bank.awake; }
    int controlPeriod() const           { return period; }
    float held() const                  { return bank.energy(); }
    // Partial n's frequency over the note's, as the body stands (n = 1: 1; n = 0: the hum).
    float partial(int n) const          { return ratio[std::clamp(n, 0, MODES - 1)]; }

    // [U2b] A hand on it: the burst of a stroke is over (what rings is left to the decay).
    void stopBurst() noexcept { burst = 0.0f; }

    // A new note: the latents are taken as they stand (no glide), the burst is armed. What rings
    // goes on ringing (reset() first for silence).
    void strike(float energy)
    {
        fresh = true;
        onset = true;
        burst = std::isfinite(energy) ? std::clamp(energy, 0.0f, MAX_ENERGY) : 0.0f;
        countdown = 0;
    }

    // One sample. outside: sound that excites the body (a track's input, another voice);
    // energy: 0..2, what feeds the exciter now.
    float tick(float outside, float energy) noexcept
    {
        if (--countdown <= 0)
        {
            countdown = period;
            control();
        }
        const float e = energy >= 0.0f ? std::min(energy, MAX_ENERGY) : 0.0f;
        const float cont = exciteNow[Cont], couple = exciteNow[Couple];
        const float burstPart = (1.0f - cont) * burst, fedPart = cont * e;
        burst *= burstDecay;

        // What it is made of: the impulse of the onset, then impulses at random, up to noise.
        float grain = 0.0f, feed;
        const bool first = onset;
        if (onset)
        {
            onset = false;
            feed = (burstPart + BITE * couple * cont * burst) * strikeGain;      // (a junction's first bite)
        }
        else
        {
            if (source != nullptr)                                  // [U3 grains] fragments of a recording
                grain = sampleGrain();
            else if (grainChance > 0.0f && dsp::xorshiftFloat(seed) < grainChance)
                grain = dsp::xorshiftNoise(seed) * 1.7320508f;
            feed = grain * (burstPart * burstGain + fedPart * fedGain) * (1.0f - couple);
        }
        if (outside != 0.0f)
            feed += softLimit(outside, OUT_LIMIT) * outsideGain;
        lp1 += hardK * (feed - lp1);
        lp2 += hardK * (lp1 - lp2);
        const float force = lp2;

        // The junction: a drive, the contact's motion, and a grip that depends on their difference.
        // What it pulls with is difference x k / (1 + k), k = its firmness x the hold: the body's
        // own answer to the pull is in that 1 + k, so the difference can only shrink, never
        // change sides. (Written as one division: the hold is (grip + REED (open - grip)), the
        // bow's grip 1 / q^4, so k / (1 + k) = n / (q^4 + n).)
        const float drive = burstPart + fedPart;
        if (gripNow <= 0.0f || drive <= 1.0e-5f)
        {
            contact = 0.0f;         // (nothing reacts to it: it is not summed)
            return heard(bank.tick(force) * OUT_GAIN, first, grain, drive);
        }

        turbulence += hardK * (grain - turbulence);
        const float d = drive * (1.0f + TURBULENCE * exciteNow[Grain] * turbulence);
        const float shape = exciteNow[Shape];
        const float bowSpeed = BOW_SPEED * d;
        float target = bowSpeed;
        if (shape > 0.0f)
            target += shape * (REED_CLOSES * REED_SHUT * (1.0f - 1.0f / (1.0f + 2.5f * d + 3.1f * d * d)) - bowSpeed);
        const float firm = gripNow * std::min(1.0f, drive * LIFT);
        const float perStick = 1.0f / (stick * BOW_SPEED * std::max(drive, 0.02f));
        // (A heavy body is met at its first partial alone, or where MEET and SPRD say: see computeExciter.)
        float met = bank.x[1];
        if (meetCount > 0)
        {
            met = 0.0f;
            for (int i = 0; i < meetCount; ++i)
                met += meetW[i] * bank.x[meetLo + i];
        }
        const float difference = target - (contact * contactAll + met * contactOne);
        const float x = difference * perStick;
        float q4, open;
        if (curve == PHYSICAL)
        {
            const float q = std::max(1.0f, std::abs(x) + 0.75f);
            q4 = (q * q) * (q * q);
            open = 1.0f - difference * (1.0f / REED_CLOSES);
        }
        else
        {
            // (The curve's own two sides: a lopsided curve holds one way more than the other.)
            const float sx = std::abs(x) >= 1.0e-3f ? x : 1.0e-3f;
            const float q = std::max(1.0f, std::abs(sx) / std::max(std::abs(sat::shape(curve - 1, sx)), 1.0e-6f));
            q4 = std::min((q * q) * (q * q), 1.0e12f);
            open = 1.0f - sat::shape(curve - 1, std::clamp(difference * (1.0f / REED_CLOSES), -1.0e3f, 1.0e3f));
        }
        const float n = firm * ((1.0f - shape) + shape * std::clamp(open, 0.0f, 2.0f) * q4);
        const float pull = difference * n / (q4 + n);
        if (meetCount > 0)
            for (int i = 0; i < meetCount; ++i)
                bank.x[meetLo + i] += pull * contactOne * meetW[i];
        else
            bank.x[1] += pull * contactOne;
        bank.wake();
        return heard(bank.tick(force + pull * contactAll, &contact) * OUT_GAIN, first, grain, drive);
    }

    void process(const float* outside, const float* energy, float* out, int samples) noexcept
    {
        for (int i = 0; i < samples; ++i)
            out[i] = tick(outside != nullptr ? outside[i] : 0.0f, energy != nullptr ? energy[i] : 0.0f);
    }

private:
    static constexpr float BOW_SPEED = 0.09f;       // the bow's speed at energy 1
    static constexpr float BOW_STICK = 1.2f;        // how far from the bow's speed it still holds, in bow speeds: this less HARD
    static constexpr float REED_CLOSES = 0.12f;     // the pressure difference that shuts the reed
    static constexpr float REED_SHUT = 0.95f;       // the mouth pressure at full energy, in closing pressures
    static constexpr float GRIP_BOW = 6.0f;         // a light body under the bow at COUP 0.75: the difference is left a seventh
    static constexpr float GRIP_REED = 0.4f;        // ... and under the reed
    static constexpr float GENTLE = 3.0f;           // a heavy body is gripped this many times what just makes it sing
    static constexpr float SINGS_BOW = 39.0f;       // ... which is this many times its first partial's loss a sample
    static constexpr float SINGS_REED = 7.0f;
    static constexpr float SEEN = 0.2f;             // how much of a heavy body's motion the junction meets
    static constexpr float BITE = 0.1f;             // the junction's first bite, against a strike
    static constexpr float LIFT = 12.0f;            // below energy 1 / 12 the bow leaves the string by degrees
    static constexpr float TURBULENCE = 0.6f;
    static constexpr float OUT_GAIN = 0.5f;
    static constexpr float FED_GAIN = 1.0f;
    static constexpr float OUTSIDE_GAIN = 1.0f;
    static constexpr float MAX_R = 0.999995f;
    // [U2b]
    static constexpr float DROP_SEMITONES = 12.0f;  // DROP at the top, the body at its loudest
    static constexpr float MOTION_FULL = 2.0f;      // ... which is an output of half
    static constexpr float WIRE_GAIN = 6.0f;        // the wires' noise against the body's own level, WIRE at the top
    static constexpr float WIRE_ABOVE = 6.0f;       // ... and where it lies: from this many times the note to three times that
    static constexpr float DIRECT_GAIN = 2.0f;
    static constexpr float MEET_TOP = 16.0f;        // the partial MEET reaches at 1
    static constexpr int MAX_MEET = 24;             // the most partials a bow meets at once

    // What is heard: the body, the wires on it, the exciter itself.
    float heard(float body, bool first, float grain, float drive) noexcept
    {
        const float mag = std::abs(body);
        motion = mag > motion ? mag : motion * motionDecay;
        if (wireGain <= 0.0f && directGain <= 0.0f && direct1 == 0.0f)
            return outLimit(body, OUT_LIMIT);
        float out = body;
        if (wireGain > 0.0f)
        {
            // A band of noise, from WIRE_ABOVE times the note to three times that, as loud as the
            // body moves, and going on rattling a moment after it (a tenth of a second to nothing).
            wireEnv = mag > wireEnv ? mag : wireEnv * wireDecay;
            const float n = dsp::xorshiftNoise(wireSeed);
            wireLp += wireK * (n - wireLp);
            wireTop += wireTopK * (n - wireLp - wireTop);
            wireTop2 += wireTopK * (wireTop - wireTop2);
            out += wireGain * wireTop2 * wireEnv;
        }
        if (directGain > 0.0f || direct1 != 0.0f)
        {
            // (An impulse comes out about a third as high as it went in; dense grain as loud as one.)
            const float raw = (first ? drive : grain * drive * directDense) * directLift;
            direct1 += hardK * (raw - direct1);
            direct2 += hardK * (direct1 - direct2);
            out += directGain * direct2;
        }
        return outLimit(out, OUT_LIMIT);
    }

    void placeMore() noexcept
    {
        MoreLatent t;
        for (size_t i = 0; i < t.size(); ++i)
            t[i] = std::clamp(morePoint[i] + moreOffset[i], 0.0f, 1.0f);
        if (t != moreTarget) { moreTarget = t; bodyDirty = true; }
    }

    // --- [U3 grains] ---------------------------------------------------------------------
    // One sample of the grains: the clock, a grain when one is due, their sum; the place moves on.
    // (Kept out of line: tick() without a recording stays as small as it was.)
    [[gnu::noinline]] float sampleGrain() noexcept
    {
        const GrainSource& s = *source;
        const double frames = (double) s.frames;
        const grn::Area area { 0.0, frames };
        if (density > 0.0f && cloud.due(density, invRate) && wanted == source)
        {
            double p = (double) grains.position * (frames - 1.0) + travel;
            if (p >= frames) p -= frames;
            cloud.start(grainSpec, p, area, grainStep, sampleRate);
        }
        travel += scanStep;
        if (travel >= frames) travel -= frames;
        else if (travel < 0.0) travel += frames;
        if (! (travel >= 0.0 && travel < frames))
            travel = 0.0;
        if (cloud.sounding() == 0)
            return 0.0f;
        return s.gain * cloud.sumMono([&s](double at) { return grn::readLinear(s.data, s.frames, at); }, grainStep, area);
    }
    // --- [U3 grains] end -----------------------------------------------------------------

    // The latents move towards what was set; what moved is computed again.
    void control() noexcept
    {
        const bool snap = fresh;
        fresh = false;
        if (wanted != source && cloud.sounding() == 0)      // [U3 grains] another recording, once the grains have ended
        {
            source = wanted;
            exciteDirty = true;
        }
        // [U2b] DROP: the pitch follows how hard the body moves.
        if (moreNow[Drop] > 0.0f || moreTarget[Drop] > 0.0f || bendNow != 1.0f)
        {
            // (As the square of the motion: a head's tension rises with the energy it holds.)
            const float moves = std::min(1.0f, motion * MOTION_FULL);
            const float semis = DROP_SEMITONES * moreNow[Drop] * moreNow[Drop] * moves * moves;
            const float want = semis > 1.0e-3f ? std::exp2(semis * (1.0f / 12.0f)) : 1.0f;
            bendNow += (want - bendNow) * 0.2f;
            if (std::abs(want - bendNow) < 2.0e-5f)
                bendNow = want;
            if (bendNow != bendSeen)
            {
                bendSeen = bendNow;
                bodyDirty = true;
            }
        }
        if (bodyDirty)
        {
            bool moves = approach(bodyNow, bodyTarget, snap);
            moves = approach(moreNow, moreTarget, snap) || moves;
            if (snap || std::abs(handMixTarget - handMixNow) < 1.0e-4f)
                handMixNow = handMixTarget;
            else
            {
                handMixNow += (handMixTarget - handMixNow) * glideK;
                moves = true;
            }
            bodyDirty = moves;
            computeBody(snap);
            exciteDirty = true;         // (its gains follow the body's)
        }
        if (exciteDirty)
        {
            exciteDirty = approach(exciteNow, exciteTarget, snap) || bodyDirty;
            computeExciter();
        }
        if (spectrumDirty)      // U2: the spectrum moved and the body did not
            applySpectrum(snap ? 0 : period);
        // What the bank holds is looked at every fourth time: too much is scaled down, and
        // partials that have rung out are put to rest (a body struck once soon runs only its
        // low ones). What is left of the excitation's filters ends as well.
        if ((++looks & 3) == 0)
        {
            const float e = bank.rest(RESTS);
            if (! (e <= HELD))
            {
                if (e < 1.0e30f)
                    bank.scale(std::sqrt(HELD / e));
                else        // (infinity or not-a-number cannot come in; if it ever did, it would not stay)
                {
                    bank.clearState();
                    lp1 = lp2 = turbulence = contact = 0.0f;
                }
            }
            if (std::abs(lp1) + std::abs(lp2) < 1.0e-15f)
                lp1 = lp2 = 0.0f;
            if (std::abs(turbulence) < 1.0e-15f)
                turbulence = 0.0f;
            if (std::abs(direct1) + std::abs(direct2) < 1.0e-15f)
                direct1 = direct2 = 0.0f;
            // (A burst that has died is over: what it still fed - the grain of a brush's tap - would
            // keep every partial of the body awake for nothing.)
            if (burst < 1.0e-7f)
                burst = 0.0f;
            if (motion < 1.0e-12f)
                motion = 0.0f;
            if (wireEnv < 1.0e-12f)
                wireEnv = wireLp = wireTop = wireTop2 = 0.0f;
        }
    }

    template <size_t N>
    bool approach(std::array<float, N>& now, const std::array<float, N>& target, bool snap) const noexcept
    {
        bool moving = false;
        for (size_t i = 0; i < N; ++i)
        {
            if (snap || std::abs(target[i] - now[i]) < 1.0e-4f)
                now[i] = target[i];
            else
            {
                now[i] += (target[i] - now[i]) * glideK;
                moving = true;
            }
        }
        return moving;
    }

    // The partials' places and weights, from the latents alone: worked out again only when a
    // latent moved (a pitch that moves - vibrato, a bend, an LFO - leaves them as they are).
    void computeSeries() noexcept
    {
        const Series& table = series();
        const float stiff = bodyNow[Stiff];
        const float b = stiff * std::pow(10.0f, 7.0f * stiff - 4.0f), bNorm = 1.0f / (1.0f + b);
        const float spacing = 1.0f / (1.0f + FREE_END * b * bNorm);     // the ends come free as stiffness takes over
        const float bWide = WIDE_STIFF * b, wideNorm = 1.0f / (1.0f + bWide);
        // Line, sheet and shell share the partials out between them (so each of the three cells
        // acts wherever the other two stand).
        // [U2b] ... and a round head and a table placed by hand are two more that share.
        const float sheet = bodyNow[Sheet], shell = bodyNow[Shell], memb = moreNow[Membrane], hand = moreNow[Hand];
        const float line = std::max(0.0f, 1.0f - sheet - shell - memb - hand), share = 1.0f / (line + sheet + shell + memb + hand);
        const float lineShare = line * share, sheetShare = sheet * share, shellShare = shell * share;
        const float membShare = memb * share, handShare = hand * share;
        const HandSet& setA = handSets()[(size_t) handA];
        const HandSet& setB = handSets()[(size_t) handB];
        const float mixB = handMixNow, mixA = 1.0f - mixB;
        const float pair = moreNow[Pair];
        const float evens = 1.0f - bodyNow[Odd];
        const float tilt = 4.0f * bodyNow[Tilt] * bodyNow[Tilt];
        const float comb = 0.25f * bodyNow[Pos];        // turns a partial
        // The hum: an octave under the note, as loud as HUM.
        ratio[0] = 0.5f;
        weightIn[0] = bodyNow[Hum];
        weightOut[0] = 1.0f;
        widthOf[0] = 1.0f;
        for (int n = 1; n < MODES; ++n)
        {
            // Stiffness stretches the line's partials as the stiff string's are; a sheet's and a
            // shell's, which have their own already, far less (their STIF: a stretch of the series).
            const float one = 1.0f + table.number[n] * spacing, sh = table.sheet[n], sl = table.shell[n], hd = table.head[n];
            float r = lineShare * one * std::sqrt((1.0f + b * one * one) * bNorm)
                      + sheetShare * sh * std::sqrt((1.0f + bWide * sh * sh) * wideNorm)
                      + shellShare * sl * std::sqrt((1.0f + bWide * sl * sl) * wideNorm);
            // The head, and the table's place, level and width as far as HAND has come. (Without
            // an `if`: a share of nothing adds exactly nothing, and the loop stays one the
            // compiler does four partials at a time.)
            r += membShare * hd * std::sqrt((1.0f + bWide * hd * hd) * wideNorm);
            r += handShare * (mixA * setA.ratio[n] + mixB * setB.ratio[n]);
            const float level = 1.0f + handShare * (mixA * setA.level[n] + mixB * setB.level[n] - 1.0f);
            widthOf[n] = 1.0f + handShare * (mixA * setA.width[n] + mixB * setB.width[n] - 1.0f);
            const float even = 1.0f + table.even[n] * (evens - 1.0f);
            ratio[n] = r;
            weightIn[n] = cosTurns(table.number[n] * comb) * even * level;
            weightOut[n] = even * level / (1.0f + tilt * (r - 1.0f));
        }
        // PAIR: every second partial comes to sit beside the one under it.
        if (pair > 0.0f)
            for (int n = 2; n < MODES; n += 2)
                ratio[n] += pair * (ratio[n - 1] * table.beat[n] - ratio[n]);
        heavy = std::min(1.0f, 4.0f * std::max(std::max(std::max(sheet, shell), std::max(memb, hand)), b / (b + 0.001f)));
    }

    void computeBody(bool snap) noexcept
    {
        if (bodyNow != seenBody || moreNow != seenMore || handA != seenHandA || handB != seenHandB || handMixNow != seenHandMix)
        {
            seenBody = bodyNow;
            seenMore = moreNow;
            seenHandA = handA;
            seenHandB = handB;
            seenHandMix = handMixNow;
            computeSeries();
        }
        const float sr = (float) sampleRate;
        const float w0 = 6.2831855f * std::min(pitch * bendNow, 0.45f * sr) / sr;
        const float loss = bodyNow[Damp] * bodyNow[Damp] * bodyNow[Damp];
        const float rate = 6.908f / (decay * sr);
        const float top = 0.45f * 6.2831855f, fadeSlope = 1.0f / (0.05f * 6.2831855f);

        alignas(32) float tc[MODES], ts[MODES], tin[MODES], tout[MODES], r2[MODES];
        for (int n = 0; n < MODES; ++n)
        {
            const float w = w0 * ratio[n];
            const float fade = std::clamp((top - w) * fadeSlope, 0.0f, 1.0f);       // out before the top
            const float a = rate * std::max(0.25f, 1.0f + loss * (ratio[n] * ratio[n] - 1.0f)) * widthOf[n];
            const float radius = std::min(MAX_R, 1.0f / (1.0f + a + 0.5f * a * a));
            modeLoss[n] = fade > 0.0f ? 1.0f - radius : -1.0f;      // (-1: not there)
            float sn, cs;
            sinCos(std::min(w, top), sn, cs);
            tc[n] = radius * cs;
            ts[n] = radius * sn;
            tin[n] = weightIn[n] * fade;
            tout[n] = weightOut[n] * fade;
            r2[n] = radius * radius;
        }
        // (The partials rise with their number: the first that is out is where the bank ends.)
        int modes = MODES;
        while (modes > 2 && w0 * ratio[modes - 1] >= top)
            --modes;
        // Three sums over the modes, in four lanes each (so that they can be done four at a time).
        float p1[4] {}, p2[4] {}, g[4] {};
        for (int n = 0; n < MODES; n += 4)
            for (int j = 0; j < 4; ++j)
            {
                const float through = tin[n + j] * tout[n + j], t2 = through * through;
                p1[j] += t2;
                p2[j] += t2 / (1.0f - r2[n + j]);
                g[j] += tin[n + j] * tin[n + j];
            }
        const float burstSum = (p1[0] + p1[1]) + (p1[2] + p1[3]), ringSum = (p2[0] + p2[1]) + (p2[2] + p2[3]);
        const float touched = (g[0] + g[1]) + (g[2] + g[3]);
        if (spectral)           // U2: after the sums above, which are the body's own
            keepForSpectrum(tc, ts, tin, tout, modes);
        bank.retarget(tc, ts, tin, tout, modes, snap ? 0 : period);
        // A burst leaves each partial at (in x out): the root of their squares is its level.
        // Something that lasts adds up over a partial's ring time: `overlap` times as much power.
        // (The first partial's weights are 1 until it fades before the top of the range, so both
        // sums are 1 or more there: a note that leaves the range fades, it is not made up for.)
        strikeGain = 1.0f / std::sqrt(std::max(1.0f, burstSum));
        overlap = ringSum / std::max(1.0f, burstSum);
        contactGain = 1.0f / std::sqrt(std::max(1.0f, touched));
        lossOne = 1.0f - std::sqrt(r2[1]);
        firstIn = tin[1] > 0.0f ? 1.0f : 0.0f;        // (out with the rest before the top of the range)
    }

    // --- U2: the spectrum (setSpectrum) -------------------------------------------------
    // What the bank was last told is kept, so that a spectrum that moves under a body that
    // does not costs one multiplication a mode.
    void keepForSpectrum(const float* tc, const float* ts, float* tin, const float* tout, int modes) noexcept
    {
        for (int n = 0; n < MODES; ++n)
        {
            keptC[n] = tc[n];
            keptS[n] = ts[n];
            keptIn[n] = tin[n];
            keptOut[n] = tout[n];
            tin[n] *= spectrum[n];
        }
        keptModes = modes;
        spectrumDirty = false;
    }

    void applySpectrum(int steps) noexcept
    {
        spectrumDirty = false;
        if (! spectral || keptModes <= 0)
            return;
        alignas(32) float tin[MODES];
        for (int n = 0; n < MODES; ++n)
            tin[n] = keptIn[n] * spectrum[n];
        bank.retarget(keptC, keptS, tin, keptOut, keptModes, steps);
    }

    // (Each part is worked out again only when what it is made of has moved: a body that
    // morphs under a steady exciter costs no logarithm here.)
    void computeExciter() noexcept
    {
        const float sr = (float) sampleRate;
        const float cont = exciteNow[Cont], grain = exciteNow[Grain], hard = exciteNow[Hard];
        if (cont != seenCont)
        {
            seenCont = cont;
            burstSamples = std::max(4.0f, (0.002f + 0.5f * cont * cont) * sr);
            burstDecay = 1.0f - 1.0f / burstSamples;
        }
        if (grain != seenGrain)
        {
            // Impulses a sample: none, then 2 a second rising to every sample.
            seenGrain = grain;
            grainChance = std::min(1.0f, std::min(1.0f, grain * 20.0f) * (2.0f / sr) * std::pow(2.0f * sr, grain));
        }
        if (hard != seenHard || pitch != seenPitch)
        {
            seenHard = hard;
            seenPitch = pitch;
            const float cutoff = std::min(0.45f * sr, pitch * std::exp2(7.0f * hard));
            hardK = 1.0f - std::exp(-6.2831855f * cutoff / sr);
        }
        // [U3 grains] What the gains take as the excitation's power a sample: an impulse's chance,
        // or - grains of a recording brought to a level of 1 - how much of the time one sounds
        // times a window's mean square.
        float power = grainChance;
        if (source != nullptr)
        {
            const double speed = source->rate * invRate;
            density = std::min(1.0f, grain * 20.0f) * 2.0f * std::pow(0.5f * MAX_GRAIN_DENSITY, grain);
            grainSpec.size = grains.size;
            grainSpec.density = density;
            grainSpec.jitter = 1.0f;        // (irregular, as the impulses are)
            grainSpec.scatter = grains.scatter;
            grainSpec.scatterFrames = GRAIN_SCATTER_SECONDS * (float) source->rate;
            grainSpec.pitchJitter = grains.pitchJitter * grains.pitchJitter;
            grainSpec.window = grains.window;
            grainStep = speed * (double) std::exp2(grains.pitch / 12.0f);
            scanStep = speed * (double) grains.scan;
            // Grains that line up (nothing scatters them and the place moves on at the speed they
            // are read at) are the recording itself, put together again: they add in level, not in power.
            const float sounding = grains.size * density;
            const bool linedUp = grains.scatter == 0.0f && grains.pitchJitter == 0.0f && scanStep == grainStep;
            power = std::max(0.375f * std::min(1.0f, sounding), linedUp ? 0.25f * std::min(sounding, (float) GRAIN_POOL) : 0.0f);
        }
        burstGain = strikeGain / std::sqrt(1.0f + power * std::min(overlap, 0.5f * burstSamples));
        fedGain = FED_GAIN * strikeGain / std::sqrt(1.0f + power * overlap);
        outsideGain = OUTSIDE_GAIN * strikeGain / std::sqrt(1.0f + overlap);

        // The junction. A light body (a string, a tube) is met where it is touched, at every
        // partial: stick and slip, all its harmonics. A heavy one (stiff, a sheet, a shell) cannot
        // be held like that - its partials are no harmonics, and a junction that met them all
        // would lock on some compromise beside the note: it is met at its first partial alone and
        // sings on it. COUP moves the grip by a factor of ten a quarter (0.75: as the constants say).
        const float couple = exciteNow[Couple];
        const float shape = exciteNow[Shape];
        stick = std::max(0.1f, BOW_STICK - hard);        // (a harder bow lets go sooner: a brighter slip)
        if (couple != seenCouple)
        {
            seenCouple = couple;
            coupleGrip = std::min(1.0f, 10.0f * couple) * std::pow(10.0f, 4.0f * (couple - 0.75f));
        }
        // [U2b] MEET and SPRD: the partials the junction meets on a heavy body, weighed by a bell
        // curve about the one MEET names, as wide as SPRD says; their weights' squares add to 1.
        // Both at 0: the first partial, as it always was.
        meetCount = 0;
        float lossMet = lossOne;
        bool metAny = firstIn > 0.0f;
        if (couple > 0.0f && heavy > 0.0f && (moreNow[Meet] > 0.0f || moreNow[Spread] > 0.0f))
        {
            const float centre = 1.0f + moreNow[Meet] * (MEET_TOP - 1.0f), sigma = 0.3f + 5.0f * moreNow[Spread];
            const int lo = std::clamp((int) std::floor(centre - 2.0f * sigma), 1, MODES - 1);
            const int hi = std::min(std::min(lo + MAX_MEET, MODES), (int) std::ceil(centre + 2.0f * sigma) + 1);
            float sum = 0.0f, fourth = 0.0f, lost = 0.0f;
            for (int n = lo; n < hi; ++n)
            {
                const float d = ((float) n - centre) / sigma;
                const float w = modeLoss[n] >= 0.0f ? std::exp(-d * d) : 0.0f;
                meetW[n - lo] = w;
                sum += w * w;
                fourth += (w * w) * (w * w);
                lost += w * w * std::max(modeLoss[n], 0.0f);
            }
            metAny = sum > 1.0e-12f;
            if (metAny)
            {
                const float norm = 1.0f / std::sqrt(sum);
                for (int n = lo; n < hi; ++n)
                    meetW[n - lo] *= norm;
                meetLo = lo;
                meetCount = hi - lo;
                // What the grip must make up for: a partial met with a share w^2 of the junction
                // needs 1 / w^2 times the grip to sing (K partials met alike: K times one's).
                lossMet = lost * sum / std::max(fourth, 1.0e-20f);
            }
        }
        // [U2b] WIRE and DIR.
        wireGain = WIRE_GAIN * moreNow[Wire] * moreNow[Wire];
        wireK = 1.0f - std::exp(-6.2831855f * std::min(0.1f * sr, WIRE_ABOVE * pitch) / sr);
        wireTopK = 1.0f - std::exp(-6.2831855f * std::min(0.4f * sr, 3.0f * WIRE_ABOVE * pitch) / sr);
        directGain = DIRECT_GAIN * moreNow[Direct] * moreNow[Direct];
        directLift = 1.0f / std::max(hardK, 0.02f);
        directDense = 1.0f / std::sqrt(1.0f + 2.0f * (source != nullptr ? 1.0f : grainChance) / std::max(hardK, 0.02f));

        float grip = 0.0f;
        if (couple > 0.0f)
        {
            const float light = GRIP_BOW + shape * (GRIP_REED - GRIP_BOW);
            const float sings = GENTLE * (SINGS_BOW + shape * (SINGS_REED - SINGS_BOW)) * lossMet / (SEEN * SEEN);
            grip = heavy <= 0.0f ? light : (heavy >= 1.0f ? sings : light * std::pow(sings / light, heavy));
        }
        gripNow = grip * coupleGrip;
        contactAll = (1.0f - heavy) * contactGain;
        contactOne = heavy * (metAny ? 1.0f : 0.0f) * SEEN;
    }

    dsp::ModeBank bank;
    double sampleRate = 44100.0;
    int period = 16, countdown = 0;
    bool bodyDirty = true, exciteDirty = true, fresh = true, onset = false;
    float glideK = 0.2f;

    BodyLatent bodyTarget = bodyPoints()[String], bodyNow = bodyPoints()[String];
    ExciteLatent exciteTarget = excitePoints()[Strike], exciteNow = excitePoints()[Strike];
    int curve = PHYSICAL;
    float pitch = 220.0f, decay = 2.0f;
    BodyLatent seenBody { { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f } };
    alignas(32) float ratio[MODES] {}, weightIn[MODES] {}, weightOut[MODES] {};

    float strikeGain = 1.0f, burstGain = 1.0f, fedGain = 1.0f, outsideGain = 1.0f, overlap = 1.0f, contactGain = 1.0f;
    float burst = 0.0f, burstDecay = 0.99f, grainChance = 0.0f, hardK = 0.5f;
    float seenCont = -1.0f, seenGrain = -1.0f, seenHard = -1.0f, seenPitch = -1.0f, seenCouple = -1.0f;
    float burstSamples = 96.0f, coupleGrip = 0.0f;
    unsigned looks = 0;
    float lossOne = 1.0e-4f, heavy = 0.0f, gripNow = 0.0f, stick = 0.6f, firstIn = 1.0f, contactAll = 1.0f, contactOne = 0.0f;
    float lp1 = 0.0f, lp2 = 0.0f, turbulence = 0.0f, contact = 0.0f;
    uint32_t seed = 0x2545f491u;

    // [U2b] The second eight latents, the tables HAND reads, and what they make.
    MoreLatent morePoint {}, moreOffset {}, moreTarget {}, moreNow {};
    MoreLatent seenMore { { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f } };
    int handA = HandGong, handB = HandGong, seenHandA = -1, seenHandB = -1;
    float handMixTarget = 0.0f, handMixNow = 0.0f, seenHandMix = -1.0f;
    alignas(32) float widthOf[MODES] {}, modeLoss[MODES] {}, meetW[MAX_MEET] {};
    int meetLo = 1, meetCount = 0;
    float motion = 0.0f, motionDecay = 0.999f, bendNow = 1.0f, bendSeen = 1.0f;
    float wireGain = 0.0f, wireK = 0.5f, wireLp = 0.0f, wireTopK = 0.5f, wireTop = 0.0f, wireTop2 = 0.0f;
    float wireEnv = 0.0f, wireDecay = 0.99f;
    float directGain = 0.0f, directLift = 1.0f, directDense = 1.0f, direct1 = 0.0f, direct2 = 0.0f;
    uint32_t wireSeed = 0x6c8e9cf5u;

    // U2: the spectrum over the partials, and what the bank was last told without it.
    bool spectral = false, spectrumDirty = false;
    int keptModes = 0;
    alignas(32) float spectrum[MODES] {}, keptC[MODES] {}, keptS[MODES] {}, keptIn[MODES] {}, keptOut[MODES] {};

    // [U3 grains]
    grn::Cloud<GRAIN_POOL> cloud;
    const GrainSource* wanted = nullptr;    // what was set ...
    const GrainSource* source = nullptr;    // ... and what the grains that sound are read from
    GrainSet grains;
    grn::Spec grainSpec;
    double invRate = 1.0 / 44100.0, grainStep = 1.0, scanStep = 1.0, travel = 0.0;
    float density = 0.0f;
};
}
