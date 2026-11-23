#pragma once

#include "drum_atlas.h"
#include "resonator_tests.h"
#include "../src/engine/AudioEngine.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/project/FactoryPresets.h"
#include "../src/project/ProjectBridge.h"
#include "../src/project/presets/ResonatorKits.h"
#include "../src/ui/screens/DrumPadScreen.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include <chrono>

// The resonator drum (drum type RSN: the RSN engine's own voice on a kit pad), what the resonator
// got for it in phase U2b (.ralph/spec-unify.md: a membrane, a pitch that drops, wires, the
// partial a bow meets, pairs that beat, partials placed by hand, a brush, the exciter's own
// sound), and the atlas: every sound of the drum types and of the cymbal machine that the phase
// set out to fold away, old against new (tests/drum_atlas.h).
//   NEPTUNE_ATLAS=1          prints the atlas's table
//   NEPTUNE_ATLAS_DIR=<dir>  writes its pairs as WAV
//   NEPTUNE_DRUM_BENCH=1     prints the cost table (meant for the optimised build)
#if JUCE_MAC
// (The system's hook on every allocation: counted for this thread while a test asks.)
extern "C" { extern void (*malloc_logger)(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t); }
namespace reviewalloc
{
    inline std::atomic<int>& count() { static std::atomic<int> n { 0 }; return n; }
    inline pthread_t& watched() { static pthread_t t {}; return t; }
    inline void seen(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
    {
        if (pthread_equal(pthread_self(), watched()))
            count().fetch_add(1);
    }
    template <typename Fn> int during(Fn&& fn)
    {
        watched() = pthread_self();
        count().store(0);
        malloc_logger = seen;
        fn();
        malloc_logger = nullptr;
        return count().load();
    }
}
#endif

class ResonatorDrumTests : public juce::UnitTest
{
public:
    ResonatorDrumTests() : UnitTest("Resonator Drum") {}

    using D = DrumEngine;
    using E = ResonatorEngine;
    using V = ResonatorVoice;
    using RT = ResonatorTests;

    // ---- the resonator by itself ---------------------------------------------------------------------
    struct Body
    {
        float position = 0.0f;
        res::MoreLatent more {};
        res::BodyLatent body {};
        float exciter = 0.0f;
        res::ExciteLatent excite {};
        float pitch = 220.0f, decay = 2.0f;
    };

    static void set(res::Resonator& r, const Body& b, double sr = 48000.0)
    {
        r.prepare(sr);
        r.setBody(b.position, b.body);
        r.setMore(b.more);
        r.setExciter(b.exciter, b.excite);
        r.setPitch(b.pitch);
        r.setDecay(b.decay);
    }

    // Struck once with `energy`, then fed `held` (0: left to ring).
    static std::vector<float> ring(const Body& b, double seconds, float energy = 1.0f, float held = 0.0f, double sr = 48000.0)
    {
        res::Resonator r;
        set(r, b, sr);
        r.strike(energy);
        std::vector<float> out((size_t) (seconds * sr));
        for (auto& s : out) s = r.tick(0.0f, held);
        return out;
    }

    static res::MoreLatent more(int latent, float value)
    {
        res::MoreLatent m {};
        m[(size_t) latent] = value;
        return m;
    }

    // The strongest frequency between lo and hi, Hz (a Goertzel every 2 cents, 0.4 s from `from`).
    static double strongest(const std::vector<float>& x, double lo, double hi, size_t from = 4800, size_t n = 19200)
    {
        double best = lo, most = -1.0;
        for (double f = lo; f <= hi; f *= 1.0011559)
        {
            const double p = fastmeasure::goertzel(x, from, n, f, 48000.0);
            if (p > most) { most = p; best = f; }
        }
        return best;
    }

    static double bandLevel(const std::vector<float>& x, double lo, double hi, size_t from, size_t n)
    {
        const auto p = cymbaltest::spectrum(x, from, n);
        double sum = 0.0;
        for (size_t i = 1; i < p.size(); ++i)
        {
            const double f = (double) i * 48000.0 / (double) n;
            if (f >= lo && f < hi) sum += p[i];
        }
        return std::sqrt(sum);
    }

    static bool bounded(const std::vector<float>& x, float limit)
    {
        for (float s : x)
            if (! std::isfinite(s) || std::abs(s) > limit) return false;
        return true;
    }

    // ---- the kit -----------------------------------------------------------------------------------
    static std::map<int, float> padAs(const char* tag, float level = 0.8f)
    {
        std::map<int, float> p;
        CymbalPresets::resonator(p, 0, tag, level, 0.5f);
        return p;
    }
    static int soundCell(int cell, int pad = 0) { return D::SOUND_BASE + pad * D::SOUND_CELLS + cell; }

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        Rig()
        {
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Drum));
            seq.getPattern().getTrack(0).engineType = EngineType::Drum;
        }
        DrumEngine& drums() { return *dynamic_cast<DrumEngine*>(audio.getEngine(0)); }
        Track& track() { return seq.getPattern().getTrack(0); }
    };

    // (A function of its own, its engines on the heap: the tests around it fill their function's frame.)
    void storedTypeTests()
    {
        const double sr = 48000.0;
        beginTest("One rule for a stored drum type (CymbalPresets::typeNow): each of the eighteen, by every way it reaches a pad - "
                  "the TYPE cell from a file's name, a preset's number or a step lock, and a kit - is the same voice, the same "
                  "sound, and a TYPE cell that holds what it offers");
        {
            // What each stored type must be on the pad.
            struct Want { DrumSynthType stored, voice; const char* sound; int model; };
            const Want wants[] = {
                { DrumSynthType::Tom,         DrumSynthType::Resonator, "TOM",  -1 },
                { DrumSynthType::NoiseSnare,  DrumSynthType::Resonator, "SNAR", -1 },
                { DrumSynthType::FMSnare,     DrumSynthType::Resonator, "FMSN", -1 },
                { DrumSynthType::CymbalSynth, DrumSynthType::Resonator, "RIDE", -1 },
                { DrumSynthType::Rim,         DrumSynthType::Resonator, "RIM",  -1 },
                { DrumSynthType::Clave,       DrumSynthType::Resonator, "CLAV", -1 },
                { DrumSynthType::ClosedHat,   DrumSynthType::OscBank,   nullptr, (int) OscBank::Hat },
                { DrumSynthType::OpenHat,     DrumSynthType::OscBank,   nullptr, (int) OscBank::OpenHat } };
            // The rule itself covers all eighteen: an offered type is itself, the rest are named above.
            for (int t = 0; t < D::NUM_STORED_TYPES; ++t)
            {
                const auto now = CymbalPresets::typeNow((DrumSynthType) t);
                expect((int) now.cell < D::NUM_DRUM_TYPES, juce::String(stored::drumTypeLabels()[t]) + ": the cell holds an offered type");
                if (t < D::NUM_DRUM_TYPES)
                    expect(now.cell == (DrumSynthType) t && now.voice == (DrumSynthType) t && now.sound == nullptr,
                           juce::String(stored::drumTypeLabels()[t]) + " is offered and is itself");
                else
                    expect(now.sound != nullptr || now.cell == DrumSynthType::OscBank, juce::String(stored::drumTypeLabels()[t]) + " became something");
            }

            const int pad = 5, typeId = pad * 8 + D::TYPE;
            const auto check = [&](DrumEngine& e, const Want& w, const juce::String& way)
            {
                const juce::String what = way + ", stored " + stored::drumTypeLabels()[(int) w.stored];
                expect(e.getVoice(pad).getType() == w.voice, what + ": the voice is " + stored::drumTypeLabels()[(int) w.voice]
                       + " (it never plays the retired type's old code)");
                const float cell = e.getParameter(typeId);
                expectEquals((int) cell, (int) w.voice, what + ": the TYPE cell holds what it shows");
                const auto spec = D::voicePage(pad).params[(size_t) D::TYPE];
                expect(cell <= spec.maxVal, what + ": the cell is within its list");
                expectEquals(spec.format(cell), juce::String(stored::drumTypeLabels()[(int) w.voice]), what + ": and reads so");
                if (w.sound != nullptr)
                    expectEquals((int) e.getParameter(soundCell(D::RsnSound, pad)), resonatorSoundIndex(w.sound), what + ": the sound is " + w.sound);
                if (w.model >= 0)
                {
                    expectEquals((int) e.getParameter(soundCell(OscBank::ParModel, pad)), w.model, what + ": the bank's MODL cell is that hat");
                    expectEquals((int) e.getVoice(pad).getBankParam(OscBank::ParModel), w.model, what + ": and the bank plays it");
                }
                // It sounds.
                e.setParameter(pad * 8 + D::LEVEL, 1.0f);
                e.noteOn(pad, 1.0f);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                double sum = 0.0;
                for (int b = 0; b < 40; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i)
                        sum += (double) buf.getSample(0, i) * buf.getSample(0, i) + (double) buf.getSample(1, i) * buf.getSample(1, i);
                }
                expect(std::isfinite(sum) && sum > 1.0e-6, what + ": it sounds");
                expect(e.getVoice(pad).getType() == w.voice, what + ": and is still that voice after the hit");
            };

            for (const auto& w : wants)
            {
                // (a) the TYPE cell given the stored number: what a preset's or an old project's map holds.
                {
                    auto engine = std::make_unique<DrumEngine>();
                    DrumEngine& e = *engine;
                    e.prepareToPlay(sr, 64);
                    e.setParameter(typeId, (float) (int) w.stored);
                    check(e, w, "the cell");
                    // Given again (a sound applied twice): the pad's page is not started over.
                    if (w.sound != nullptr)
                    {
                        e.setParameter(soundCell(D::RsnMorph, pad), 1.5f);
                        e.setParameter(typeId, (float) (int) w.stored);
                        expectWithinAbsoluteError(e.getParameter(soundCell(D::RsnMorph, pad)), 1.5f, 1.0e-6f, "the same stored type again changes nothing");
                    }
                }
                // (b) a format-3 file names the type: the name is its number among the eighteen.
                {
                    const int value = stored::valueOf((int) EngineType::Drum, typeId, stored::drumTypeLabels()[(int) w.stored]);
                    expectEquals(value, (int) w.stored, "the name of a stored type is read as its number");
                    auto held = std::make_unique<Rig>();
                    Rig& rig = *held;
                    rig.drums().prepareToPlay(sr, 64);
                    rig.audio.setTrackParameter(0, typeId, (float) value);
                    check(rig.drums(), w, "a file's name through the track");
                }
                // (c) a preset that holds the number, applied as presets are (PresetManager::applyToEngine).
                {
                    Preset preset;
                    preset.engineType = EngineType::Drum;
                    preset.soundParams[typeId] = (float) (int) w.stored;
                    auto engine = std::make_unique<DrumEngine>();
                    DrumEngine& e = *engine;
                    e.prepareToPlay(sr, 64);
                    PresetManager().applyToEngine(preset, &e);
                    check(e, w, "a preset");
                }
                // (d) a step lock on TYPE (a pattern from a file may hold one: the cell itself offers ten).
                {
                    auto held = std::make_unique<Rig>();
                    Rig& rig = *held;
                    rig.drums().prepareToPlay(sr, 64);
                    const float before = rig.audio.getTrackParameter(0, typeId);
                    auto& locked = rig.track().getStep(2);
                    locked.paramLocks[typeId] = (float) (int) w.stored;
                    rig.audio.applyStepLocks(0, locked);
                    check(rig.drums(), w, "a step lock");
                    rig.audio.applyStepLocks(0, rig.track().getStep(3));
                    expectEquals(rig.audio.getTrackParameter(0, typeId), before, "and the pad's own type is back on the next step");
                }
                // (e) a kit (DrumEngine::loadKit).
                {
                    auto engine = std::make_unique<DrumEngine>();
                    DrumEngine& e = *engine;
                    e.prepareToPlay(sr, 64);
                    DrumKit kit = e.getCurrentKit();
                    kit.slots[(size_t) pad].synthType = w.stored;
                    kit.slots[(size_t) pad].params[0] = 400.0f; kit.slots[(size_t) pad].params[1] = 0.3f; kit.slots[(size_t) pad].params[5] = 0.8f;
                    e.loadKit(kit);
                    check(e, w, "a kit");
                }
            }
        }
    }

    // ---- the atlas ---------------------------------------------------------------------------------
    struct OldDrum { const char* name; DrumSynthType type; float tune, decay, tone, drive, snap, level; double seconds; };

    static std::map<int, float> asNew(const OldDrum& d)
    {
        std::map<int, float> p;
        CymbalPresets::drum(p, 0, d.tune, d.decay, d.tone, d.drive, d.snap, d.level, 0.5f, d.type);
        return p;
    }

    static std::vector<drumatlas::Pair> struckPairs()
    {
        using namespace drumatlas;
        using T = DrumSynthType;
        static const OldDrum drums[] = {
            { "tom_100",        T::Tom, 100.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.75f, 1.5 },
            { "tom_150_short",  T::Tom, 150.0f, 0.25f, 0.5f, 0.0f, 0.6f, 0.7f, 1.5 },
            { "tom_70_long",    T::Tom, 70.0f, 0.4f, 0.5f, 0.0f, 0.5f, 0.7f, 1.5 },
            { "snare_200",      T::NoiseSnare, 200.0f, 0.2f, 0.5f, 0.3f, 0.6f, 0.8f, 1.0 },
            { "snare_190_dry",  T::NoiseSnare, 190.0f, 0.12f, 0.5f, 0.0f, 0.5f, 0.8f, 1.0 },
            { "snare_180_lofi", T::NoiseSnare, 180.0f, 0.15f, 0.4f, 0.5f, 0.7f, 0.8f, 1.0 },
            { "fm_snare_200",   T::FMSnare, 200.0f, 0.2f, 0.5f, 0.3f, 0.6f, 0.8f, 1.0 },
            { "clave_2500",     T::Clave, 2500.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.65f, 1.0 },
            { "clave_2500_tight", T::Clave, 2500.0f, 0.015f, 0.5f, 0.0f, 0.5f, 0.65f, 1.0 },
            { "rim_800",        T::Rim, 800.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.8f, 1.0 },
            { "rim_700_tight",  T::Rim, 700.0f, 0.005f, 0.5f, 0.0f, 0.5f, 0.7f, 1.0 } };
        std::vector<Pair> out;
        for (const auto& d : drums)
            out.push_back(pair(d.name, oldDrum(d.type, d.tune, d.decay, d.tone, d.drive, d.snap, d.level, d.seconds),
                               newDrum(asNew(d), d.seconds)));
        return out;
    }

    static std::vector<drumatlas::Pair> hatPairs()
    {
        using namespace drumatlas;
        struct Hat { const char* name; bool open; float tune, decay, tone, level; double held; };
        static const Hat hats[] = {
            { "hat_closed_8000",   false, 8000.0f, 0.04f, 0.5f, 0.4f, 0.1 },
            { "hat_closed_9500",   false, 9500.0f, 0.025f, 0.5f, 0.55f, 0.1 },
            { "hat_closed_7000_dark", false, 7000.0f, 0.02f, 0.3f, 0.5f, 0.1 },
            { "hat_open_8000_held", true, 8000.0f, 0.5f, 0.5f, 0.4f, 2.0 },
            { "hat_open_8000_tapped", true, 8000.0f, 0.5f, 0.5f, 0.4f, 0.03 },
            { "hat_open_6000_dark", true, 6000.0f, 0.3f, 0.3f, 0.45f, 2.0 } };
        std::vector<Pair> out;
        for (const auto& h : hats)
        {
            std::map<int, float> p;
            CymbalPresets::drum(p, 0, h.tune, h.decay, h.tone, 0.0f, 0.5f, h.level, 0.5f,
                                h.open ? DrumSynthType::OpenHat : DrumSynthType::ClosedHat);
            Played how;
            how.release = h.held;
            out.push_back(pair(h.name, oldHat(h.open, h.tune, h.decay, h.tone, h.level, 1.5, 1.0f, h.held), newDrum(p, 1.5, how)));
        }
        return out;
    }

    // What replaces each of the cymbal machine's ten sounds: a pad of the bank or of the resonator
    // drum (the bodies placed by hand against the machine of 494c51e, which had them; the plain
    // sheets and shells against main's).
    static std::map<int, float> newCymbal(int which, bool handPlaced)
    {
        std::map<int, float> p;
        namespace C = CymbalPresets;
        switch (which)
        {
            case 0:  C::bank(p, 0, OscBank::Metal, 8000.0f, 1.2f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f); break;
            case 1:  C::bank(p, 0, OscBank::Ring, 8000.0f, 0.9f, 0.5f, 0.0f, 0.5f, 0.45f, 0.5f, 0.4f, 0.55f, 0.5f); break;
            case 2:  C::resonator(p, 0, handPlaced ? "RIDE" : "RID2", 0.5f, 0.5f, 0.8f, 0.7f, 0.5f, 0.0f, 0.6f); break;
            case 3:  C::resonator(p, 0, handPlaced ? "RIDE" : "RID2", 0.5f, 0.5f); break;
            case 4:  C::resonator(p, 0, handPlaced ? "CUP" : "CUP2", 0.5f, 0.5f); break;
            case 5:  C::resonator(p, 0, handPlaced ? "CRSH" : "CRS2", 0.5f, 0.5f); break;
            case 6:  C::resonator(p, 0, handPlaced ? "CHNA" : "CHN2", 0.45f, 0.5f, 1.0f, 1.0f, 0.5f, 0.15f); break;
            case 7:  C::resonator(p, 0, handPlaced ? "SPLS" : "SPL2", 0.45f, 0.5f); break;
            case 8:  C::resonator(p, 0, handPlaced ? "GONG" : "GNG2", 0.6f, 0.5f); break;
            default: C::resonator(p, 0, handPlaced ? "TAM" : "TAM2", 0.6f, 0.5f); break;
        }
        return p;
    }

    static std::vector<drumatlas::Pair> cymbalPairs(bool at494)
    {
        using namespace drumatlas;
        std::vector<Pair> out;
        const auto& olds = at494 ? cymbals494() : cymbalsMain();
        for (int i = 0; i < (int) olds.size(); ++i)
        {
            const double seconds = i >= 8 ? 12.0 : 6.0;
            const std::vector<double> once { 0.0 };
            auto oldSound = at494 ? oldCymbal<CymbalMachine494>(olds[(size_t) i], once, seconds)
                                  : oldCymbal<CymbalMachine>(olds[(size_t) i], once, seconds);
            out.push_back(pair(juce::String(at494 ? "cym494_" : "cymMain_") + olds[(size_t) i].name, std::move(oldSound),
                               newDrum(newCymbal(i, at494), seconds)));
        }
        return out;
    }

    void expectNotLost(const drumatlas::Pair& p)
    {
        expect(std::strcmp(p.verdict.word, "LOST") != 0, p.name + ": " + drumatlas::describe(p.verdict));
    }

    // ---- cost --------------------------------------------------------------------------------------
    static double microsecondsPerBlock(EngineBase& e, const std::function<void(int)>& play, int blocks = 600)
    {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        std::vector<double> times;
        for (int b = 0; b < blocks; ++b)
        {
            play(b);
            buf.clear();
            const auto t0 = std::chrono::high_resolution_clock::now();
            e.processBlock(buf, midi);
            times.push_back(std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count());
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

    void runTest() override
    {
        using namespace drumatlas;
        using cymbaltest::rms;
        const double sr = 48000.0;

        beginTest("MEMB: a round head's partials (1, 1.593, 2.136, 2.295, 2.653, 2.917), the first on the note; half-way from a "
                  "string they are half-way; HEAD is the point");
        {
            res::Resonator r;
            Body head;
            head.position = (float) res::Head;
            set(r, head);
            r.strike(1.0f);
            r.tick(0.0f, 0.0f);
            const double want[6] = { 1.0, 1.5933, 2.1355, 2.2954, 2.6531, 2.9173 };
            for (int n = 1; n <= 6; ++n)
                expectWithinAbsoluteError((double) r.partial(n), want[n - 1], 1.0e-3, "partial " + juce::String(n));
            expectWithinAbsoluteError(r.more()[res::Membrane], 1.0f, 1.0e-6f, "HEAD is all membrane");
            // Measured, with DROP out of the way: the first partial is on the note, the second where the series says.
            head.more = more(res::Drop, -1.0f);
            head.body[res::Tilt] = -1.0f;
            const auto x = ring(head, 1.0);
            expect(std::abs(RT::centsOff(x, 4800, 24000, 220.0, sr)) < 1.0, "the first partial is on the note");
            expectWithinAbsoluteError(strongest(x, 300.0, 400.0) / 220.0, 1.5933, 0.004, "the second partial");
            // Half the way from the string (MEMB as an offset on STRG): half the way.
            Body half;
            half.more = more(res::Membrane, 0.5f);
            half.body[res::Stiff] = -1.0f;
            res::Resonator h;
            set(h, half);
            h.strike(1.0f);
            h.tick(0.0f, 0.0f);
            expectWithinAbsoluteError((double) h.partial(2), 0.5 * (2.0 + 1.5933), 2.0e-3);
            // Without it nothing moves: a string is a string.
            Body string;
            string.body[res::Stiff] = -1.0f;
            res::Resonator s;
            set(s, string);
            s.strike(1.0f);
            s.tick(0.0f, 0.0f);
            expectWithinAbsoluteError(s.partial(2), 2.0f, 1.0e-5f);
        }

        beginTest("DROP: the pitch is above the note while the body moves hard and falls back to it; a soft stroke bends "
                  "less; without DROP it does not move");
        {
            Body drum;
            drum.position = (float) res::Head;
            drum.body[res::Tilt] = 1.0f;
            drum.more = more(res::Drop, 0.35f);             // (HEAD's own 0.25 and this: 0.6)
            drum.decay = 1.0f;
            const auto hard = ring(drum, 1.5, 2.0f), soft = ring(drum, 1.5, 0.15f);
            const double early = RT::centsOff(hard, 480, 2400, 220.0, sr, 700.0), late = RT::centsOff(hard, 48000, 19200, 220.0, sr, 700.0);
            const double softEarly = RT::centsOff(soft, 480, 2400, 220.0, sr, 700.0);
            logMessage("DROP 0.6: a hard stroke starts " + juce::String(early, 0) + " cents up and ends " + juce::String(late, 1)
                       + " cents off; a soft one starts " + juce::String(softEarly, 0) + " cents up");
            expect(early > 150.0 && early < 500.0, "a hard stroke starts sharp: " + juce::String(early, 0) + " cents");
            expect(std::abs(late) < 5.0, "and settles on the note: " + juce::String(late, 1) + " cents");
            expect(softEarly < 0.4 * early, "a soft stroke bends less: " + juce::String(softEarly, 0) + " cents");
            drum.more = more(res::Drop, -1.0f);
            const auto still = ring(drum, 0.5, 2.0f);
            expect(std::abs(RT::centsOff(still, 480, 2400, 220.0, sr, 700.0)) < 12.0, "no DROP: no bend");
            expect(bounded(hard, 4.0f) && bounded(soft, 4.0f));
        }

        beginTest("WIRE: a band of noise that rides on the body's motion, from 6 to 18 times the note, and is silent with it");
        {
            Body snare;
            snare.position = (float) res::Head;
            snare.body[res::Tilt] = 1.0f;
            snare.more = more(res::Drop, -1.0f);
            snare.decay = 0.3f;
            const auto dry = ring(snare, 0.6);
            snare.more[res::Wire] = 0.8f;
            const auto wired = ring(snare, 0.6);
            const double band = bandLevel(wired, 1320.0, 3960.0, 0, 8192), bandDry = bandLevel(dry, 1320.0, 3960.0, 0, 8192);
            const double above = bandLevel(wired, 9000.0, 20000.0, 0, 8192);
            logMessage("WIRE 0.8 on a head at 220 Hz: the band 1.3-4 kHz is " + juce::String(cymbaltest::dB(band / std::max(bandDry, 1.0e-9)), 1)
                       + " dB over the head alone; above 9 kHz " + juce::String(cymbaltest::dB(above / band), 1) + " dB under the band");
            expect(band > 10.0 * bandDry, "the wires are heard in their band");
            expect(above < 0.5 * band, "and lie in it, not over the whole top");
            expect(rms(wired, 0.5, 0.6, sr) < 0.02 * rms(wired, 0.0, 0.05, sr), "they stop with the head (a tenth of a second after it)");
            // Nothing moves: nothing rattles.
            res::Resonator r;
            set(r, snare);
            float most = 0.0f;
            for (int i = 0; i < 4800; ++i) most = std::max(most, std::abs(r.tick(0.0f, 0.0f)));
            expect(most == 0.0f, "no stroke, no wires");
        }

        beginTest("MEET and SPRD: a bow on a heavy body meets the partial MEET names (a bar sings on its second: 2.76 times "
                  "the note), and with SPRD many at once; at 0 it is the first, as it was");
        {
            Body bar;
            bar.position = (float) res::Bar;
            bar.exciter = (float) res::Bow;
            bar.decay = 1.5f;
            const auto first = ring(bar, 2.0, 1.0f, 1.0f);
            bar.more = more(res::Meet, 1.0f / 15.0f);       // the second partial
            const auto second = ring(bar, 2.0, 1.0f, 1.0f);
            const double f1 = strongest(first, 150.0, 2500.0, 48000, 38400), f2 = strongest(second, 150.0, 2500.0, 48000, 38400);
            logMessage("a bowed bar at 220 Hz sings at " + juce::String(f1, 1) + " Hz; met at its second partial at " + juce::String(f2, 1) + " Hz");
            expectWithinAbsoluteError(f1 / 220.0, 1.0, 0.02, "MEET 0: the first partial");
            expectWithinAbsoluteError(f2 / 220.0, 2.756, 0.05, "MEET at the second partial: it sings there");
            expect(rms(second, 1.0, 2.0, sr) > 0.01, "and it sounds");
            // A cymbal's body under a spread bow: several partials at once, not one sine.
            Body cymbal;
            cymbal.position = (float) res::Crash;
            cymbal.exciter = (float) res::Bow;
            cymbal.pitch = 287.0f;
            cymbal.decay = 4.0f;
            cymbal.decay = 1.0f;
            auto tail = [](const std::vector<float>& x) { return std::vector<float>(x.begin() + 2 * 48000, x.end()); };
            const auto sine = tail(ring(cymbal, 3.0, 1.0f, 1.0f));      // (the last second: the bow's first bite has rung out)
            cymbal.more = more(res::Meet, 0.45f);
            cymbal.more[res::Spread] = 0.5f;
            const auto many = tail(ring(cymbal, 3.0, 1.0f, 1.0f));
            auto fs = measure(sine), fm = measure(many);
            fs.brightLate = cymbaltest::centroid(sine, 0.2, 48000.0, 16384);
            fm.brightLate = cymbaltest::centroid(many, 0.2, 48000.0, 16384);
            // (Met over a spread of partials the bow does not sing on all of them: it finds one among
            // them and holds it - which one is the body's to say. What is asked here is that it is an
            // upper one, where a bow met at the first partial alone gives the first partial.)
            const double low = strongest(sine, 100.0, 6000.0, 4800, 32768), high = strongest(many, 100.0, 6000.0, 4800, 32768);
            logMessage("a bowed crash at 287 Hz sings at " + juce::String(low, 0) + " Hz met at its first partial, at "
                       + juce::String(high, 0) + " Hz with MEET 0.45 SPRD 0.5 (brightness " + juce::String(fs.brightLate, 0)
                       + " and " + juce::String(fm.brightLate, 0) + " Hz)");
            expectWithinAbsoluteError(low / 287.0, 1.0, 0.03, "met at the first partial: the first partial");
            expect(high > 1.8 * 287.0, "met high up: an upper partial sings");
            expect(fm.brightLate > 2.0 * fs.brightLate && cymbaltest::rms(many, 0.0, 1.0, 48000.0) > 1.0e-3, "and it sounds");
            expect(bounded(many, 4.0f) && bounded(second, 4.0f));
        }

        beginTest("PAIR: every second partial comes to sit beside the one under it and the pair beats, each pair at its own "
                  "rate; without it nothing beats");
        {
            Body plate;
            plate.position = (float) res::Bar;
            plate.pitch = 200.0f;
            plate.decay = 8.0f;
            plate.body[res::Damp] = -1.0f;
            const auto plain = ring(plate, 6.0);
            plate.more = more(res::Pair, 1.0f);
            res::Resonator r;
            set(r, plate);
            r.strike(1.0f);
            r.tick(0.0f, 0.0f);
            const double ratio2 = r.partial(2) / r.partial(1), ratio4 = r.partial(4) / r.partial(3);
            expect(ratio2 > 1.001 && ratio2 < 1.005 && ratio4 > 1.001 && ratio4 < 1.005 && std::abs(ratio2 - ratio4) > 1.0e-4,
                   "the pairs: " + juce::String(ratio2, 5) + " and " + juce::String(ratio4, 5));
            const auto paired = ring(plate, 6.0);
            const double want = 200.0 * (ratio2 - 1.0), got = beatRate(paired, 200.0, 0.2, 5.5), none = beatRate(plain, 200.0, 0.2, 5.5);
            logMessage("PAIR 1 on a bar at 200 Hz: the first pair beats at " + juce::String(got, 2) + " Hz (" + juce::String(want, 2)
                       + " by its two places); without PAIR " + juce::String(none, 2));
            expectWithinAbsoluteError(got, want, 0.12, "the first pair's beat");
            expect(none == 0.0, "no pair, no beat");
        }

        beginTest("HAND: partials placed by hand come back - the gong's lowest pair and the cup's two beat as they did on the "
                  "machine of 494c51e, the tam-tam's cluster is where it was; a table has a level and a width per mode");
        {
            // The tables are the old machine's own numbers.
            res::Resonator r;
            Body gong;
            gong.position = (float) res::Gong;
            gong.pitch = 196.0f;
            set(r, gong);
            r.strike(1.0f);
            r.tick(0.0f, 0.0f);
            expectWithinAbsoluteError((double) r.partial(2), 197.4 / 196.0, 1.0e-4, "the gong's pair");
            expectWithinAbsoluteError((double) r.partial(3), 350.0 * std::pow(20.0, (0.0 + 0.7 * 0.0) / 35.0) / 196.0, 0.3, "(then the band begins)");
            const auto& tam = res::handSets()[res::HandTam];
            expectWithinAbsoluteError(tam.ratio[2], 92.5f / 71.0f, 1.0e-4f, "the tam-tam's second partial");
            expectWithinAbsoluteError(tam.pitch, 71.0f, 1.0e-3f);
            const auto& cup = res::handSets()[res::HandCup];
            expectWithinAbsoluteError(cup.ratio[3] / cup.ratio[2], 938.5f / 931.0f, 1.0e-4f, "the cup's first pair");
            bool levels = false, widths = false;
            for (int n = 2; n < 40; ++n)
            {
                levels = levels || std::abs(cup.level[n] - cup.level[n - 1]) > 0.05f;
                widths = widths || std::abs(cup.width[n] - cup.width[n - 1]) > 0.05f;
            }
            expect(levels && widths, "a level and a width per mode");

            // Heard: the beat of the gong's lowest pair, old machine against new sound.
            const std::vector<double> once { 0.0 };
            const auto oldGong = oldCymbal<CymbalMachine494>(cymbals494()[8], once, 8.0);
            const auto newGong = newDrum(newCymbal(8, true), 8.0);
            const auto mainGong = oldCymbal<CymbalMachine>(cymbalsMain()[8], once, 8.0);
            const double oldBeat = beatRate(oldGong, 196.7, 1.0, 6.5), newBeat = beatRate(newGong, 196.7, 1.0, 6.5),
                         mainBeat = beatRate(mainGong, 196.0, 1.0, 6.5);
            logMessage("the gong's lowest partial beats at " + juce::String(oldBeat, 2) + " Hz on the machine of 494c51e, "
                       + juce::String(mainBeat, 2) + " Hz on main's (0 = not at all), " + juce::String(newBeat, 2) + " Hz on the new GONG");
            expectWithinAbsoluteError(oldBeat, 1.4, 0.15, "the old gong beat at 1.4 Hz");
            expectWithinAbsoluteError(newBeat, oldBeat, 0.15, "the new one does again");
            // ... and the cup's pair at 931 / 938.5 Hz.
            const auto oldCup = oldCymbal<CymbalMachine494>(cymbals494()[4], once, 4.0);
            const auto newCup = newDrum(newCymbal(4, true), 4.0);
            const double oldCupBeat = beatRate(oldCup, 934.7, 0.3, 3.0), newCupBeat = beatRate(newCup, 934.7, 0.3, 3.0);
            logMessage("the cup's pair at 931 / 938.5 Hz beats at " + juce::String(oldCupBeat, 2) + " Hz (old) and "
                       + juce::String(newCupBeat, 2) + " Hz (new)");
            expectWithinAbsoluteError(newCupBeat, 7.5, 0.4, "the cup's pair beats at 7.5 Hz");
            expectWithinAbsoluteError(oldCupBeat, 7.5, 0.4);
        }

        beginTest("BRSH and DIR: a brush is fed for as long as there is energy, dense and soft, and its tap is a burst; DIR is "
                  "the exciter itself beside the body, and at 0 nothing of it");
        {
            const auto& points = res::excitePoints();
            expect(points[res::Brush][res::Couple] == 0.0f && points[res::Brush][res::Grain] > 0.6f
                   && points[res::Brush][res::Hard] < points[res::Scrape][res::Hard] && points[res::Brush][res::Cont] > 0.0f, "the point");
            Body ride;
            ride.position = (float) res::Head;
            ride.exciter = (float) res::Brush;
            ride.decay = 0.3f;
            const auto tap = ring(ride, 1.0, 1.0f, 0.0f), swish = ring(ride, 1.0, 0.0f, 1.0f);
            expect(rms(tap, 0.0, 0.05, sr) > 10.0 * rms(tap, 0.6, 0.7, sr) && rms(tap, 0.0, 0.05, sr) > 1.0e-3, "a tap is a burst");
            expect(rms(swish, 0.6, 0.9, sr) > 0.3 * rms(swish, 0.1, 0.4, sr) && rms(swish, 0.6, 0.9, sr) > 1.0e-3, "a swish lasts as long as the energy");
            expect(cymbaltest::repeat(swish, sr) < 0.9, "and is noise on the body, not a tone alone");
            // DIR.
            ride.more = more(res::Direct, 1.0f);
            const auto direct = ring(ride, 1.0, 0.0f, 1.0f);
            const double top = bandLevel(direct, 3000.0, 12000.0, 9600, 8192) / std::max(bandLevel(swish, 3000.0, 12000.0, 9600, 8192), 1.0e-9);
            logMessage("DIR 1 under a brush on a head: " + juce::String(cymbaltest::dB(top), 1) + " dB more between 3 and 12 kHz");
            expect(top > 2.0, "the brush's own hiss is heard beside the head");
            Body struck;
            const auto a = ring(struck, 0.2);
            struck.more = more(res::Direct, 0.0f);
            const auto b = ring(struck, 0.2);
            expect(a == b, "DIR 0: not a bit of it");
            expect(bounded(direct, 4.0f));
        }

        beginTest("Edges: the second eight latents at both ends, no numbers for them, 22.05 to 192 kHz, every new point under "
                  "every exciter: finite, within the limit, silent again; what has rung out costs nothing");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            for (double rate : { 22050.0, 48000.0, 192000.0 })
                for (int point = res::Head; point < res::NumBodyPoints; ++point)
                    for (int exciter = 0; exciter < res::NumExcitePoints; ++exciter)
                    {
                        res::Resonator r;
                        Body b;
                        b.position = (float) point;
                        b.exciter = (float) exciter;
                        b.decay = 0.2f;
                        set(r, b, rate);
                        r.strike(2.0f);
                        bool ok = true;
                        const int n = (int) (0.25 * rate);
                        for (int i = 0; i < n; ++i)
                        {
                            if (i == n / 4)     r.setMore({ { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f } });
                            if (i == n / 2)     r.setMore({ { -1.0f, nan, inf, -inf, 1.0e30f, nan, -1.0e30f, inf } });
                            const float y = r.tick(i < n / 2 ? 0.5f : 0.0f, i < 3 * n / 4 ? 2.0f : 0.0f);
                            ok = ok && std::isfinite(y) && std::abs(y) <= res::Resonator::OUT_LIMIT;
                        }
                        expect(ok, juce::String(E::bodyName(point)) + " " + E::exciterName(exciter) + " at " + juce::String(rate, 0));
                        float last = 1.0f;
                        for (int i = 0; i < (int) (3.0 * rate); ++i) last = r.tick(0.0f, 0.0f);
                        expect(last == 0.0f && r.modesAwake() == 0, juce::String(E::bodyName(point)) + " " + E::exciterName(exciter)
                               + ": silent and at rest again (" + juce::String(r.modesAwake()) + " modes awake)");
                    }
        }

        beginTest("The kit: TYPE offers ten (TOM SNAR FMSN CSYN folded to resonator drum), a pad's "
                  "sound page is its type's and starts from the type's own when the type changes");
        {
            // [FOLD] 10 types offered (TOM, SNAR, FMSN, CSYN are resonator drum sounds now).
            expectEquals(D::NUM_DRUM_TYPES, 10);
            const char* names[10] = { "FMKK", "KICK", "MKIK", "CLAP", "SHKR", "SUB", "KSYN", "CYMB", "RSN", "BANK" };
            for (int i = 0; i < 10; ++i)
                expectEquals(juce::String(D::typeNames()[i]), juce::String(names[i]));
            expectEquals((int) DrumSynthType::Resonator, 8);
            expectEquals((int) DrumSynthType::OscBank, 9);
            DrumEngine e;
            const int type = 3 * 8 + D::TYPE;
            e.setParameter(type, (float) (int) DrumSynthType::Resonator);
            expect(e.isResonator(3) && e.hasSoundPage(3) && e.soundPage(3).name == "RSN");
            const char* cells[8] = { "SND", "MRPH", "XMPH", "ENRG", "DAMP", "WIRE", "MEET", "DIR" };
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(juce::String(e.soundPage(3).params[(size_t) i].label), juce::String(cells[i]));
                expectEquals(e.soundPage(3).params[(size_t) i].id, D::SOUND_BASE + 3 * 8 + i);
                expectWithinAbsoluteError(e.getParameter(soundCell(i, 3)), 0.0f, 1.0e-6f, "the page starts at the sound as it is");
            }
            expect(e.isRebuildParam(soundCell(D::RsnSound, 3)) && ! e.isRebuildParam(soundCell(D::RsnMorph, 3)), "SND cannot be locked, MRPH can");
            e.setParameter(soundCell(D::RsnMorph, 3), 2.0f);
            e.setParameter(type, (float) (int) DrumSynthType::OscBank);
            expect(e.isBank(3) && e.soundPage(3).name == "BANK");
            expectWithinAbsoluteError(e.getParameter(soundCell(OscBank::ParPing, 3)), 0.5f, 1.0e-6f, "the bank's own PING, not the 2 that was MRPH");
            // [FOLD] CSYN folds to Resonator, with its RSN page.
            e.setParameter(type, (float) (int) DrumSynthType::CymbalSynth);
            expect(e.isResonator(3) && e.soundPage(3).name == "RSN");
            e.setParameter(type, (float) (int) DrumSynthType::KickSynth);
            expect(e.hasSoundPage(3) && e.soundPage(3).name == "KCK2");      // [KICKS] (the kick machine's second page)
            e.setParameter(type, (float) (int) DrumSynthType::Clap);
            expect(! e.hasSoundPage(3));
            // (Held to the eighteen stored types: the last is the open hat, which is the bank's OHAT,
            // and the cell holds what it offers.)
            e.setParameter(type, 99.0f);
            expectEquals((int) e.getParameter(type), (int) DrumSynthType::OscBank, "past the stored types: the last of them, as the cell holds it");
            expectEquals((int) e.getVoice(3).getBankParam(OscBank::ParModel), (int) OscBank::OpenHat, "the open hat");
            // What was a drum type and is not offered: setType makes the sound of it.
            DrumVoice v;
            v.setType(DrumSynthType::OpenHat);
            expect(v.getType() == DrumSynthType::OscBank && (int) v.getBankParam(OscBank::ParModel) == OscBank::OpenHat);
            // SND's list: every sound has a name of four letters at most, none twice.
            std::set<juce::String> tags;
            for (const auto& sound : resonatorSounds())
            {
                expect(juce::String(sound.tag).length() <= 4 && tags.insert(sound.tag).second, sound.tag);
                expect(sound.hz >= 20.0f && sound.hz <= 12000.0f && sound.dec >= 0.001f && sound.dec <= 4.0f, juce::String(sound.tag) + " fits TUNE and DEC");
            }
            expect(tags.size() >= 30);
        }

        beginTest("Every sound of SND on a pad: it sounds, stays within bounds, ends (the voice idle, exact silence), is the "
                  "same at 44.1 and 96 kHz, and no two are one sound");
        {
            const auto& sounds = resonatorSounds();
            std::vector<std::vector<float>> renders;
            for (const auto& sound : sounds)
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                for (const auto& kv : padAs(sound.tag))
                    e.setParameter(kv.first, kv.second);
                e.setSlotAftertouchDest(0, DrumATDest::Off);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::vector<float> x;
                e.noteOn(0, 0.9f);
                if (sound.pressEnergy > 0.0f)
                    e.aftertouch(0, 0.8f);
                // (To 100 dB down, and the partials of a table that outlast its first three times over.)
                const bool table = sound.params.at(E::MorphParam) >= (float) res::Gong;
                const double played = 0.4, total = std::min(30.0, played + 3.0 + 5.5 * (double) sound.dec * (double) sound.decScale);
                bool letGo = false;
                for (int b = 0; b < (int) (total * sr) / 64; ++b)
                {
                    if (! letGo && (double) b * 64.0 / sr >= played)
                    {
                        e.noteOff(0);
                        letGo = true;
                    }
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i) x.push_back(buf.getSample(0, i));
                }
                const double level = rms(x, 0.0, played, sr);
                expect(level > 3.0e-3 && level < 0.7, juce::String(sound.tag) + " sounds: " + juce::String(cymbaltest::dB(level), 1) + " dB");
                expect(bounded(x, 3.0f), juce::String(sound.tag) + " stays within bounds");
                // It ends by itself (looked at for the sounds of under five seconds), and under the hand at once.
                // (A table's upper partials outlast its first many times over: those are left to the hand.)
                if ((double) sound.dec * (double) sound.decScale < 5.0 && ! table)
                    expect(! e.getVoice(0).isActive() && x.back() == 0.0f, juce::String(sound.tag) + " ends by itself: idle and exactly silent after "
                           + juce::String(total, 1) + " s");
                e.noteOn(0, 0.9f);
                e.noteOff(0);
                for (int b = 0; b < (int) (0.2 * sr) / 64; ++b) { buf.clear(); e.processBlock(buf, midi); }
                e.getVoice(0).choke();
                for (int b = 0; b < (int) (1.0 * sr) / 64; ++b) { buf.clear(); e.processBlock(buf, midi); }
                expect(! e.getVoice(0).isActive() && buf.getMagnitude(0, 64) == 0.0f, juce::String(sound.tag) + " ends under the hand: idle and exactly silent");
                renders.push_back(std::vector<float>(x.begin(), x.begin() + (size_t) (played * sr)));
                writeWav(juce::String("sound_") + sound.tag + "_new", x);       // (NEPTUNE_ATLAS_DIR: every sound, to listen to)
                // Other rates: about as loud.
                for (double rate : { 44100.0, 96000.0 })
                {
                    Played how;
                    how.velocity = 0.9f;
                    if (sound.pressEnergy > 0.0f) how.pressure = { { 0.0, 0.8f } };
                    how.release = 0.4;
                    const auto y = newDrum(padAs(sound.tag), 0.4, how, rate);
                    const double other = rms(y, 0.0, 0.4, rate) / 0.70710678;
                    // (A stroke with grain in it - a stick and its burst of noise - is as loud as the noise
                    // falls against the stroke: the same every time at one rate, up to 9 dB apart between two.)
                    expect(other > 0.3 * level && other < 3.3 * level, juce::String(sound.tag) + " at " + juce::String(rate, 0) + ": "
                           + juce::String(cymbaltest::dB(other / level), 1) + " dB against 48 kHz");
                }
            }
            int alike = 0;
            for (size_t a = 0; a < renders.size(); ++a)
                for (size_t b = a + 1; b < renders.size(); ++b)
                    if (fastmeasure::difference(renders[a], renders[b], 1024) < 0.05f)
                    {
                        ++alike;
                        logMessage(juce::String(sounds[a].tag) + " and " + sounds[b].tag + " are alike");
                    }
            expectEquals(alike, 0, "no two sounds are one");
        }

        beginTest("A pad's cells: TUNE is its pitch, DEC its length, TONE, SNAP and DRIV are heard, the seven cells of its RSN "
                  "page are added to the sound's own and each is heard");
        {
            auto render = [&](const std::map<int, float>& set, double seconds = 0.6, const Played& how = {}) { return newDrum(set, seconds, how); };
            auto with = [](std::map<int, float> p, int id, float value) { p[id] = value; return p; };
            // TUNE: the clave's first partial is where TUNE says.
            const auto clave = with(padAs("CLAV"), D::DECAY, 0.3f);
            for (float hz : { 600.0f, 1500.0f })
            {
                const auto x = render(with(clave, D::TUNE, hz), 0.5);
                expect(std::abs(RT::centsOff(x, 0, 9600, (double) hz, sr)) < 6.0, "TUNE " + juce::String(hz, 0) + " Hz");
            }
            // DEC: a gong twice as long (CHAN RING: the body's decay), a tom twice as long (its envelope's).
            for (const char* tag : { "CHNA", "TOM" })
            {
                const float dec = resonatorSounds()[(size_t) resonatorSoundIndex(tag)].dec;
                const auto shortOne = measure(render(with(padAs(tag), D::DECAY, dec * 0.5f), 6.0));
                const auto longOne = measure(render(with(padAs(tag), D::DECAY, dec), 6.0));
                expect(longOne.t60 > 1.6 * shortOne.t60 && longOne.t60 < 2.4 * shortOne.t60, juce::String(tag) + ": DEC doubled, "
                       + juce::String(shortOne.t60, 2) + " s to " + juce::String(longOne.t60, 2) + " s");
            }
            const auto base = render(padAs("SNAR"));
            const struct { const char* name; int id; float value; } cells[] = {
                { "TONE", D::TONE, 0.95f }, { "SNAP", D::SNAP, 0.95f }, { "DRIV", D::DRIVE, 0.8f }, { "LEVL", D::LEVEL, 0.3f },
                { "MRPH", soundCell(D::RsnMorph), 1.0f }, { "XMPH", soundCell(D::RsnExciter), 3.0f }, { "ENRG", soundCell(D::RsnEnergy), -0.5f },
                { "DAMP", soundCell(D::RsnDamp), -0.7f }, { "WIRE", soundCell(D::RsnWire), -0.45f } };
            for (const auto& c : cells)
            {
                const auto x = render(with(padAs("SNAR"), c.id, c.value));
                const float d = fastmeasure::difference(base, x, 1024);
                expect(d > 0.02f && bounded(x, 3.0f), juce::String(c.name) + " is heard: " + juce::String(d, 3));
            }
            // DIR: the brush's own hiss beside the snare (under a strike it is the stick's tick alone).
            expect(fastmeasure::difference(render(padAs("SNRS")), render(with(padAs("SNRS"), soundCell(D::RsnDirect), -0.75f)), 1024) > 0.02f,
                   "DIR is heard under a brush");
            // MEET under a bow (the bowed bar: its page's MEET takes it from the third partial to the sixth).
            Played bowed;
            bowed.pressure = { { 0.0, 0.9f } };
            bowed.release = 1.5;
            const auto third = render(padAs("BARB"), 1.5, bowed), sixth = render(with(padAs("BARB"), soundCell(D::RsnMeet), 0.2f), 1.5, bowed);
            expect(fastmeasure::difference(third, sixth, 1024) > 0.05f, "MEET is heard under a bow");
            // What the pad adds is added: MRPH + 1 on a tom is the gong's place (HEAD to GONG).
            DrumEngine e;
            for (const auto& kv : with(padAs("TOM"), soundCell(D::RsnMorph), 1.0f))
                e.setParameter(kv.first, kv.second);
            e.prepareToPlay(sr, 64);
            e.noteOn(0, 1.0f);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            e.processBlock(buf, midi);
            expectWithinAbsoluteError(e.getVoice(0).getResonator().getCell(V::Morph), (float) res::Gong, 1.0e-5f);
            expectWithinAbsoluteError(e.getVoice(0).getResonator().getResonator().more()[res::Hand], 1.0f, 1.0e-3f, "a table's body");
        }

        beginTest("One sound, two hosts: a preset of the RSN engine on the keys and the same sound on a kit pad at that pitch "
                  "are the same sound");
        {
            for (const char* tag : { "CRSH", "CLAV", "SNAR", "RIDE" })
            {
                const auto& sound = resonatorSounds()[(size_t) resonatorSoundIndex(tag)];
                const int note = 57;
                const float hz = 220.0f;
                // The kit: TUNE at the note, LEVL 1, the stroke at full.
                auto p = padAs(tag, 1.0f);
                p[D::TUNE] = hz;
                Played how;
                how.release = 2.0;
                const auto pad = newDrum(p, 2.0, how);
                // The keys.
                ResonatorEngine e;
                e.setNoteSeededNoise(true);     // ([U4-RSN] the kit's noise: the note's, as the pad's is)
                e.prepareToPlay(sr, 64);
                for (const auto& kv : sound.params)
                    e.setParameter(kv.first, kv.second);
                e.noteOn(note, 1.0f, -1);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::vector<float> keys;
                for (int b = 0; b < (int) (2.0 * sr) / 64; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i) keys.push_back(buf.getSample(0, i));
                }
                // (The kit's pad is louder by a fixed factor - no master level of a half, the host's gain -
                // which is taken out: the one gain that fits best.)
                double cross = 0.0, own = 0.0, sum = 0.0, power = 0.0;
                const size_t n = std::min(pad.size(), keys.size());
                for (size_t i = 0; i < n; ++i)
                {
                    cross += (double) pad[i] * (double) keys[i];
                    own += (double) pad[i] * (double) pad[i];
                }
                const double gain = cross / std::max(own, 1.0e-20);
                for (size_t i = 0; i < n; ++i)
                {
                    const double a = gain * (double) pad[i], b = (double) keys[i];
                    sum += (a - b) * (a - b);
                    power += b * b;
                }
                expect(sum < 0.01 * power, juce::String(tag) + ": the same samples but for a gain (" + juce::String(1.0 / gain, 2) + " x): "
                       + juce::String(cymbaltest::dB(std::sqrt(sum / std::max(power, 1.0e-20))), 1) + " dB apart");
                const auto fp = measure(pad), fk = measure(keys);
                logMessage(juce::String(tag) + " on a pad and on the keys: t60 " + juce::String(fp.t60, 2) + " / " + juce::String(fk.t60, 2)
                           + " s, brightness " + juce::String(fp.bright, 0) + " / " + juce::String(fk.bright, 0) + " Hz, difference "
                           + juce::String(cymbaltest::dB(std::sqrt(sum / std::max(power, 1.0e-20))), 1) + " dB");
                expect(power > 1.0e-6, juce::String(tag) + " sounds on the keys");
                expect(fk.t60 > 0.8 * fp.t60 && fk.t60 < 1.25 * fp.t60, juce::String(tag) + ": as long");
                expect(fk.bright > 0.85 * fp.bright && fk.bright < 1.18 * fp.bright, juce::String(tag) + ": as bright");
            }
        }

        beginTest("Strokes add; a hand on it (choke) ends it in a twentieth of a second; the pad's pressure is the bow's and "
                  "the brush's energy, and the finger on a hat");
        {
            // Two strokes on a gong: the second adds to the first, nothing starts over.
            Played twice;
            twice.strokes = { 0.0, 0.5 };
            const auto two = newDrum(padAs("GONG"), 1.5, twice), one = newDrum(padAs("GONG"), 1.5);
            expect(rms(two, 0.6, 1.4, sr) > 1.15 * rms(one, 0.6, 1.4, sr), "the second stroke adds");
            expect(cymbaltest::largestStep(two, (size_t) (0.5 * sr) - 4, (size_t) (0.5 * sr) + 4) < 0.5, "and nothing is cut at it");
            // Choke.
            DrumEngine e;
            e.prepareToPlay(sr, 64);
            for (const auto& kv : padAs("GONG"))
                e.setParameter(kv.first, kv.second);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            auto run = [&](double seconds)
            {
                float peak = 0.0f;
                for (int b = 0; b < (int) (seconds * sr) / 64; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    peak = std::max(peak, buf.getMagnitude(0, 64));
                }
                return peak;
            };
            e.noteOn(0, 1.0f);
            const float ringing = run(0.3);
            e.getVoice(0).choke();
            run(0.1);
            const float after = run(0.1);
            expect(ringing > 0.05f && after < 0.01f * ringing, "choked: " + juce::String(cymbaltest::dB(after / ringing), 0) + " dB after a tenth of a second");
            e.noteOn(0, 1.0f);
            expect(run(0.3) > 0.5f * ringing, "the next stroke lifts the hand");
            // Pressure: the bowed snare is silent until the pad is pressed, sounds while it is, and stops when it is let up.
            Played press;
            press.velocity = 0.05f;
            press.release = 1.4;
            press.pressure = { { 0.5, 0.9f }, { 1.0, 0.0f } };
            const auto bowed = newDrum(padAs("SNRB"), 2.0, press);
            const double before = rms(bowed, 0.3, 0.5, sr), during = rms(bowed, 0.7, 1.0, sr), afterwards = rms(bowed, 1.7, 2.0, sr);
            logMessage("bowed snare: " + juce::String(cymbaltest::dB(before), 0) + " dB before the pad is pressed, "
                       + juce::String(cymbaltest::dB(during), 0) + " dB under it, " + juce::String(cymbaltest::dB(afterwards), 0) + " dB after");
            expect(during > 0.003 && during > 8.0 * before && during > 8.0 * afterwards, "the pressure is the bow");
            // The finger on the open hat: pressed, it is shorter.
            Played open, half;
            open.release = half.release = 1.5;
            half.pressure = { { 0.0, 1.0f } };
            const auto openHat = measure(newDrum(padAs("HATO"), 2.5, open)), halfHat = measure(newDrum(padAs("HATO"), 2.5, half));
            expect(halfHat.t60 < 0.5 * openHat.t60, "the finger closes the hat: " + juce::String(openHat.t60, 2) + " s to " + juce::String(halfHat.t60, 2) + " s");
        }

        beginTest("The kit's LFOs, slots and strip reach a resonator drum's cells (each drum its own), and no other drum hears them");
        {
            const auto& dests = D::lfoDests();
            const int morph = dests.indexOfLabel("MRPH");
            expect(morph > 0 && dests.indexOfLabel("HARD") > 0 && dests.indexOfLabel("ENRG") > 0 && dests.indexOfLabel("WIRE") > 0);
            auto render = [&](bool lfo, int pad)
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                std::map<int, float> p;
                CymbalPresets::resonator(p, 3, "GONG", 0.8f, 0.5f);
                for (const auto& kv : p)
                    e.setParameter(kv.first, kv.second);
                if (lfo)
                {
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 6.0f);
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.3f);
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) morph);
                }
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::vector<float> x;
                e.noteOn(pad, 1.0f);
                for (int b = 0; b < (int) (1.0 * sr) / 64; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i) x.push_back(buf.getSample(0, i));
                }
                return x;
            };
            expect(fastmeasure::difference(render(false, 3), render(true, 3), 1024) > 0.05f, "an LFO on MRPH moves the gong's body");
            expect(render(false, 8) == render(true, 8), "the kick machine does not hear it");
            expect(bounded(render(true, 3), 3.0f));
        }

        beginTest("A cell is stored with the track and locked on a step; a project keeps the type, the sound and the page; "
                  "DRUMS and PARAM show the page");
        {
            Rig rig;
            const int pad = 9, typeId = pad * 8 + D::TYPE, morphId = soundCell(D::RsnMorph, pad), soundId = soundCell(D::RsnSound, pad);
            rig.audio.setTrackParameter(0, typeId, (float) (int) DrumSynthType::Resonator);
            rig.audio.setTrackParameter(0, soundId, (float) resonatorSoundIndex("CLAV"));
            rig.audio.setTrackParameter(0, morphId, 0.5f);
            expectWithinAbsoluteError(rig.drums().getParameter(morphId), 0.5f, 1.0e-6f, "it reaches the engine through the track");
            auto& locked = rig.track().getStep(2);
            locked.paramLocks[morphId] = 3.0f;              // the clave becomes a bell on this step
            rig.audio.applyStepLocks(0, locked);
            expectWithinAbsoluteError(rig.drums().getParameter(morphId), 3.0f, 1.0e-6f, "the lock is applied");
            rig.audio.applyStepLocks(0, rig.track().getStep(3));
            expectWithinAbsoluteError(rig.drums().getParameter(morphId), 0.5f, 1.0e-6f, "and gone on the next step");

            const auto data = ProjectData::fromJSON(ProjectBridge::capture({ rig.seq, rig.audio }, "RSN DRUM").toJSON());
            Rig back;
            ProjectBridge::apply(data, { back.seq, back.audio });
            expect(back.drums().isResonator(pad));
            expectWithinAbsoluteError(back.audio.getTrackParameter(0, soundId), (float) resonatorSoundIndex("CLAV"), 1.0e-4f);
            expectWithinAbsoluteError(back.audio.getTrackParameter(0, morphId), 0.5f, 1.0e-4f);
            auto renderPad = [&](DrumEngine& e)
            {
                e.prepareToPlay(sr, 64);
                e.noteOn(pad, 1.0f);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::vector<float> out;
                for (int b = 0; b < (int) (0.3 * sr) / 64; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i) out.push_back(buf.getSample(0, i));
                }
                return out;
            };
            const auto before = renderPad(rig.drums()), after = renderPad(back.drums());
            expect(rms(before, 0.0, 0.1, sr) > 0.005 && before == after, "the reloaded pad is the same sound");

            // DRUMS.
            DrumEngine engine;
            DrumPadScreen screen(&engine);
            screen.onActivate();
            screen.selectPad(3);
            expectEquals(screen.getPageCount(), 3, "an ordinary pad: DR, PLFO, PSLT");
            engine.setParameter(3 * 8 + D::TYPE, (float) (int) DrumSynthType::Resonator);
            expectEquals(screen.getPageCount(), 4, "a resonator drum: DR, RSN, PLFO, PSLT");
            expectEquals(screen.getSubPage(), juce::String("P4 RSN"));
            screen.onArrowRight();
            expectEquals(screen.getSubPage(), juce::String("P4 RSN"), "the page's name where the type's stood");
            expectEquals(screen.getPageIndex(), 1);
            const char* shortNames[8] = { "SN", "MR", "XM", "EN", "DP", "WR", "MT", "DR" };
            for (int i = 0; i < 8; ++i)
            {
                expectEquals(screen.getEncoderName(i), juce::String(shortNames[i]));
                const float was = engine.getParameter(soundCell(i, 3));
                const auto readout = screen.getEncoderReadout(i);
                screen.onEncoderChange(i, 1);
                expect(engine.getParameter(soundCell(i, 3)) > was, juce::String(shortNames[i]) + " turns");
                // One click is seen: the readout is another one, and all of it fits the name bar
                // (MRPH and XMPH move by 0.05 of sixteen: under a pixel of the bar, and the old
                // "BODY MORPH 0.05" lost its value at the name bar's edge. The audit called both dead.)
                expect(screen.getEncoderReadout(i) != readout, juce::String(shortNames[i]) + " shows one click: " + screen.getEncoderReadout(i));
                expect(screen.getEncoderReadout(i).length() <= DrumPadScreen::READOUT_ROOM, screen.getEncoderReadout(i));
                screen.onEncoderChange(i, 2);
            }
            expect(screen.getEncoderReadout(0).startsWith("SOUND "), screen.getEncoderReadout(0));
            expect(screen.getEncoderReadout(1).startsWith("MORPH "), screen.getEncoderReadout(1));
            expect(screen.getEncoderReadout(2).startsWith("EXC MRPH "), screen.getEncoderReadout(2));
            // PARAM: DR n, then RSN, then the kit's other pages.
            Rig rig2;
            ParamPageScreen param(rig2.seq, &rig2.audio);
            rig2.drums().setSelectedVoice(5);
            const int plain = param.getPageCount();
            rig2.audio.setTrackParameter(0, 5 * 8 + D::TYPE, (float) (int) DrumSynthType::Resonator);
            expectEquals(param.getPageCount(), plain + 1);
            juce::StringArray names;
            for (int i = 0; i < param.getPageCount(); ++i)
            {
                param.setCurrentPageIndex(i);
                names.add(param.getPageName());
            }
            const int at = names.indexOf("RSN");
            expect(at > 0 && names[at - 1] == "DR 6", names.joinIntoString(" "));
            param.setCurrentPageIndex(at);
            expectEquals(param.getEncoderName(0), juce::String("SND"));
            expectEquals(param.getEncoderName(5), juce::String("WIRE"));
            param.onEncoderChange(5, 5);
            expect(rig2.drums().getParameter(soundCell(D::RsnWire, 5)) > 0.0f, "WIRE reaches the engine");
            expect(rig2.track().soundParams.count(soundCell(D::RsnWire, 5)) == 1, "and is stored with the sound");
            rig2.audio.setTrackParameter(0, 5 * 8 + D::TYPE, (float) (int) DrumSynthType::OscBank);
            param.setCurrentPageIndex(at);
            expectEquals(param.getPageName(), juce::String("BANK"));
            expectEquals(param.getEncoderName(0), juce::String("MODL"));
        }

        beginTest("The ride, measured against what a ride does: a stick's definition, the cup, a dense spread of partials, the top "
                  "dying first, the wash building under repeated strokes and holding");
        {
            // What a real ride does, in general terms (T. Rossing, "Science of Percussion Instruments",
            // and N. Fletcher, T. Rossing, "The Physics of Musical Instruments", the chapter on cymbals,
            // gongs and plates; published spectra of 20-inch rides): the stroke lands within a few
            // milliseconds with most of its energy between about 3 and 6 kHz; several hundred modes
            // lie below 10 kHz, so densely that above 1 kHz single ones are hardly told apart; what
            // lies above about 5 kHz is gone within one to three seconds while the low modes ring
            // for ten seconds and more; a stick pattern builds a wash a few decibels above a single
            // stroke's within the first bar and then holds it; the cup has a few strong, long
            // partials, some in close pairs that beat a few times a second.
            const auto one = newDrum(padAs("RIDE", 0.5f), 8.0);
            const auto f = measure(one);
            const auto resolved = cymbaltest::partials(one, sr, 0.05);
            logMessage("RIDE, one stroke: loudest after " + juce::String(f.attack * 1000.0, 1) + " ms; brightness " + juce::String(f.bright, 0)
                       + " Hz at the stroke, " + juce::String(f.brightLate, 0) + " Hz after 0.3 s; " + juce::String((int) resolved.size())
                       + " partials told apart between 300 Hz and 12 kHz; t60 below 1 kHz " + juce::String(f.bandT60[0], 1) + " s, 1-4 kHz "
                       + juce::String(f.bandT60[1], 1) + " s, 4-10 kHz " + juce::String(f.bandT60[2], 1) + " s, above " + juce::String(f.bandT60[3], 1) + " s");
            expect(cymbaltest::rms(one, 0.0, 0.005, sr) > 0.5 * cymbaltest::rms(one, f.attack, f.attack + 0.005, sr) && f.attack <= 0.02,
                   "the stick lands at once (the first 5 ms are within 6 dB of the loudest)");
            expect(f.bright > 2500.0 && f.bright < 7000.0, "its energy sits in the upper middle");
            expect(resolved.size() >= 20, "a dense spread of partials (48 are placed; a real ride has several hundred)");
            expect(f.bandT60[3] < 0.6 * f.bandT60[0] && f.bandT60[2] <= f.bandT60[1] + 0.3 && f.bandT60[0] > 4.0, "the top dies first, the low partials ring on");
            // The pattern: eighth notes at 120 (a stroke every quarter of a second).
            Played pattern;
            pattern.velocity = 0.8f;
            for (int n = 0; n < 16; ++n) pattern.strokes.push_back(0.25 * (double) (n + 1));
            const auto ridden = newDrum(padAs("RIDE", 0.5f), 5.0, pattern);
            auto wash = [&](int stroke)     // the 60 ms before stroke n (counted from 0), 3-8 kHz
            {
                const size_t from = (size_t) ((0.25 * (double) stroke - 0.065) * sr);
                return bandLevel(ridden, 3000.0, 8000.0, from, 2048);
            };
            writeWav("ride_one_stroke_new", one);
            writeWav("ride_eighth_notes_new", ridden);
            const double first = wash(1), fourth = wash(4), eighth = wash(8), last = wash(16);
            logMessage("RIDE, eighth notes: the wash before the 2nd, 5th, 9th and 17th stroke: 0 / " + juce::String(cymbaltest::dB(fourth / first), 1)
                       + " / " + juce::String(cymbaltest::dB(eighth / first), 1) + " / " + juce::String(cymbaltest::dB(last / first), 1) + " dB");
            expect(eighth > 1.25 * first, "the wash builds under the strokes");
            expect(last < 2.0 * eighth && bounded(ridden, 3.0f), "and holds");
        }

        beginTest("The four kits (project/presets/ResonatorKits.h; registered nowhere yet): every value is a cell of the kit, "
                  "every pad sounds, and each does what its name says");
        {
            const auto kits = resonatorKits();
            expectEquals((int) kits.size(), 4);
            auto load = [&](DrumEngine& e, const Preset& kit)
            {
                std::set<int> cells;
                for (const auto& page : e.getParamPages())
                    for (const auto& spec : page.params) cells.insert(spec.id);
                for (const auto& kv : kit.soundParams)
                {
                    expect(EngineParamRange::contains(kv.first) && (cells.count(kv.first) > 0 || D::isSoundParam(kv.first)),
                           kit.name + ": id " + juce::String(kv.first) + " is a cell");
                    e.setParameter(kv.first, kv.second);
                }
            };
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            auto run = [&](DrumEngine& e, double seconds, std::vector<float>* out = nullptr)
            {
                float peak = 0.0f;
                for (int b = 0; b < (int) (seconds * sr) / 64; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    peak = std::max(peak, buf.getMagnitude(0, 64));
                    if (out != nullptr)
                        for (int i = 0; i < 64; ++i) out->push_back(buf.getSample(0, i) + buf.getSample(1, i));
                }
                return peak;
            };
            for (const auto& kit : kits)
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                e.setTempo(120.0f);
                load(e, kit);
                expect(kit.category == "KIT" && kit.engineType == EngineType::Drum && kit.name.length() <= 16, kit.name);
                for (int padIndex = 0; padIndex < 16; ++padIndex)
                {
                    e.noteOn(padIndex, 0.9f);
                    e.aftertouch(padIndex, 0.8f);
                    const float peak = run(e, 0.3);
                    e.noteOff(padIndex);
                    expect(peak > 0.003f && peak < 3.0f, kit.name + " pad " + juce::String(padIndex + 1) + ": peak " + juce::String(peak, 3));
                    e.allNotesOff();
                }
            }
            // 1. A tom that opens into a gong within the note.
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                load(e, kits[0]);
                std::vector<float> heard;
                e.noteOn(9, 1.0f);
                run(e, 0.005, &heard);
                const float atStroke = e.getVoice(9).getResonator().getResonator().more()[res::Hand];
                run(e, 0.4, &heard);
                const float onTheWay = e.getVoice(9).getResonator().getResonator().more()[res::Hand];
                run(e, 2.5, &heard);
                writeWav("kit_tom_to_gong_new", heard);
                const float settled = e.getVoice(9).getResonator().getResonator().more()[res::Hand];
                logMessage(kits[0].name + ": the body is " + juce::String(atStroke, 2) + " of the way to the gong at the stroke, "
                           + juce::String(onTheWay, 2) + " after 0.4 s, " + juce::String(settled, 2) + " after 3 s");
                // (U4-DRUM: 0.15 was 0.20. On the router the envelope reaches the slot through the
                // router's 4 ms lead and a sub-block late, so 5 ms after the stroke the body is a
                // little further from the tom; "U4 Drum" measures this kit old against new.)
                expect(atStroke < 0.2f && onTheWay > atStroke + 0.1f && onTheWay < 0.95f && settled > 0.97f, "a tom at the stroke, a gong in the end");
                expect(e.getVoice(9).isActive(), "and it rings on as one");
                // The kick machine beside it is not moved by the slot.
                expect(! e.isResonator(8));
            }
            // 2. Brushes: under a held pad the swish swells and fades with LFO 1, twice a second at 120.
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                e.setTempo(120.0f);
                load(e, kits[1]);
                e.setSlotAftertouchDest(12, DrumATDest::Off);
                e.noteOn(12, 0.3f);
                e.aftertouch(12, 0.8f);
                std::vector<float> x;
                run(e, 3.0, &x);
                writeWav("kit_brush_snare_stirred_new", x);
                std::vector<double> level;
                for (double t = 0.5; t + 0.05 <= 3.0; t += 0.05) level.push_back(rms(x, t, t + 0.05, sr));
                double most = 0.0, least = 1.0e9;
                for (double l : level) { most = std::max(most, l); least = std::min(least, l); }
                int swells = 0;
                const double middle = std::sqrt(most * least);
                for (size_t i = 1; i < level.size(); ++i)
                    if (level[i - 1] < middle && level[i] >= middle) ++swells;
                logMessage(kits[1].name + ": the swish under a held pad goes from " + juce::String(cymbaltest::dB(least), 1) + " to "
                           + juce::String(cymbaltest::dB(most), 1) + " dB, " + juce::String(swells) + " swells in 2.5 s at 120");
                expect(most > 2.0 * least && most > 0.003, "the stirring is heard");
                expect(swells >= 4 && swells <= 6, "once a beat");
                e.aftertouch(12, 0.0f);
                e.noteOff(12);
                run(e, 1.5);
                expect(run(e, 0.2) < 0.01f * (float) most, "the brush lifted: silence");
            }
            // 3. The hats of the resonator kit: the closed one cuts the open one, a finger closes it.
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                load(e, kits[2]);
                e.noteOn(15, 0.9f);
                const float open = run(e, 0.15);
                e.noteOn(14, 0.9f);
                run(e, 0.3);
                expect(open > 0.01f && ! e.getVoice(15).isActive(), "the closed hat cuts the open one");
            }
            // 4. The clave is a bar on one step and a bell on another.
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                e.setTempo(120.0f);
                load(e, kits[3]);
                std::set<int> places;
                for (int step = 0; step < 8; ++step)        // (an eighth note at 120: a quarter of a second)
                {
                    e.noteOn(11, 0.9f);
                    run(e, 0.02);
                    places.insert((int) std::lround(e.getVoice(11).getResonator().getCell(V::Morph) * 2.0f));
                    e.noteOff(11);
                    run(e, 0.23);
                }
                juce::String seen;
                for (int place : places) seen << " " << juce::String(0.5 * (double) place, 1);
                logMessage(kits[3].name + ": over a bar the clave's body is at MRPH" + seen);
                expect(places.count(2 * res::Bar) == 1 && places.count(2 * res::Bell) == 1 && places.size() == 3, "a bar, half-way, a bell");
            }
        }

        beginTest("Atlas: old against new. The hats and the bank's cymbals are what they were; claves, the rim shot and eleven "
                  "of the cymbals are reached within the bounds of tests/drum_atlas.h; the rest is listed (NEPTUNE_ATLAS=1)");
        {
            int match = 0, close = 0, lost = 0;
            juce::String lostNames;
            auto count = [&](const Pair& p)
            {
                print(p);
                if (std::strcmp(p.verdict.word, "MATCH") == 0) ++match;
                else if (std::strcmp(p.verdict.word, "CLOSE") == 0) ++close;
                else { ++lost; lostNames << " " << p.name; }
            };
            for (const auto& p : hatPairs())
            {
                count(p);
                expect(std::strcmp(p.verdict.word, "MATCH") == 0, p.name + ": " + describe(p.verdict));
                // (A hat that is held is the same to the last bit; one that is let go differs by when its block ends.)
                if (! p.name.contains("tapped"))
                {
                    double worst = 0.0;
                    for (size_t i = 0; i < std::min(p.oldSound.size(), p.newSound.size()); ++i)
                        worst = std::max(worst, (double) std::abs(p.oldSound[i] - p.newSound[i]));
                    expect(worst < 1.0e-6, p.name + ": the same samples (largest difference " + juce::String(worst, 9) + ")");
                }
            }
            for (const auto& p : struckPairs())
            {
                count(p);
                if (p.name.startsWith("clave") || p.name.startsWith("rim"))
                    expectNotLost(p);
            }
            const auto main = cymbalPairs(false), old = cymbalPairs(true);
            for (size_t i = 0; i < main.size(); ++i)
            {
                count(main[i]);
                if (i == 0 || i == 1)       // METL and RING: the bank
                    expect(std::strcmp(main[i].verdict.word, "MATCH") == 0, main[i].name + ": " + describe(main[i].verdict));
                else if (i == 3 || i == 4 || i == 5 || i == 6 || i == 8)    // ride, ride bell, crash, china, gong
                    expectNotLost(main[i]);
            }
            for (size_t i = 0; i < old.size(); ++i)
            {
                count(old[i]);
                if (i != 2 && i != 9)       // (all but the house ride, whose grain is LOFI, and the tam-tam)
                    expectNotLost(old[i]);
            }
            logMessage("atlas: " + juce::String(match) + " MATCH, " + juce::String(close) + " CLOSE, " + juce::String(lost) + " LOST:" + lostNames);
        }

        beginTest("Cost (NEPTUNE_DRUM_BENCH=1 prints the table; any build: a kit with every pad ringing fits its block)");
        {
            // [FOLD] A kit of sixteen: what was the starting kit's old types (CSYN, TOM, SNAR: now folded
            // to resonator drum sounds), the starting kit as it is, and sixteen long resonator drums.
            auto kit = [&](int which)
            {
                auto e = std::make_unique<DrumEngine>();
                e->prepareToPlay(sr, 512);
                std::map<int, float> p;
                if (which == 0)
                {
                    // [FOLD] These old types fold to the resonator drum when loaded through setParameter.
                    namespace C = CymbalPresets;
                    C::pad(p, 6, 8000.0f, 1.5f, 0.5f, 0.0f, 0.6f, 0.5f, 0.35f, (int) DrumSynthType::CymbalSynth);
                    C::pad(p, 7, 8000.0f, 1.3f, 0.5f, 0.0f, 0.7f, 0.5f, 0.65f, (int) DrumSynthType::CymbalSynth);
                    C::pad(p, 11, 100.0f, 0.3f, 0.5f, 0.0f, 0.5f, 0.75f, 0.4f, (int) DrumSynthType::Tom);
                    C::pad(p, 12, 200.0f, 0.2f, 0.5f, 0.3f, 0.6f, 0.8f, 0.5f, (int) DrumSynthType::NoiseSnare);
                }
                else if (which == 2)
                {
                    static const char* const tags[16] = { "GONG", "TAM", "CRSH", "RIDE", "CUP", "CHNA", "GNGB", "TAM2",
                                                         "CRS2", "RID2", "GNG2", "CUP2", "GONG", "TAM", "CRSH", "RIDE" };
                    for (int i = 0; i < 16; ++i)
                        CymbalPresets::resonator(p, i, tags[i], 0.3f, 0.5f, 1.0f + 0.03f * (float) i);
                }
                for (const auto& kv : p)
                    e->setParameter(kv.first, kv.second);
                return e;
            };
            const char* const names[3] = { "old types folded (CSYN, TOM, SNAR as resonator drums)", "the starting kit as it is",
                                           "sixteen long resonator drums (gongs, tam-tams, crashes, rides)" };
            juce::String table = "microseconds a block of 512 samples at 48 kHz, every pad struck every 250 ms (the median of 600 blocks):";
            double worst = 0.0;
            for (int which = 0; which < 3; ++which)
            {
                auto e = kit(which);
                const double ringing = microsecondsPerBlock(*e, [&](int b)
                {
                    if (b % 24 == 0)
                        for (int padIndex = 0; padIndex < 16; ++padIndex) { e->noteOn(padIndex, 0.9f); e->noteOff(padIndex); }
                });
                e->allNotesOff();
                const double resting = microsecondsPerBlock(*e, [](int) {}, 200);
                table << "\n  " << names[which] << ": " << juce::String(ringing, 0) << " us ringing, " << juce::String(resting, 1) << " us at rest";
                worst = std::max(worst, ringing);
                expect(resting < 0.05 * ringing + 20.0, juce::String(names[which]) + ": at rest it costs nothing (" + juce::String(resting, 1) + " us)");
            }
            {
                // Eight voices of the RSN engine, a bell struck and held, and the same under a bow met high up.
                for (int bowed = 0; bowed < 2; ++bowed)
                {
                    ResonatorEngine e;
                    e.prepareToPlay(sr, 512);
                    e.setParameter(E::MorphParam, (float) res::Bell);
                    e.setParameter(E::DecayParam, 8.0f);
                    if (bowed)
                    {
                        e.setParameter(E::ExciterMorphParam, (float) res::Bow);
                        e.setParameter(E::MoreBase + res::Meet, 0.3f);
                        e.setParameter(E::MoreBase + res::Spread, 0.3f);
                    }
                    const double t = microsecondsPerBlock(e, [&](int b)
                    {
                        if (b % 47 == 0)
                            for (int n = 0; n < 8; ++n) { e.noteOff(48 + 3 * n, -1); e.noteOn(48 + 3 * n, 0.9f, -1); }
                    });
                    table << "\n  RSN, eight voices, a bell " << (bowed ? "under a bow met at MEET 0.3 SPRD 0.3" : "struck") << ": " << juce::String(t, 0) << " us";
                }
            }
            if (std::getenv("NEPTUNE_DRUM_BENCH") != nullptr)
                logMessage(table);
            // (A block of 512 samples is 10,667 us. The bound is for the unoptimised build under a loaded machine.)
            expect(worst < 60000.0, "sixteen pads ringing: " + juce::String(worst, 0) + " us a block");
        }

        reviewTests();
    }

    // ---- added by the review of the phase ------------------------------------------------------
    void reviewTests()
    {
        const double sr = 48000.0;
        const bool say = std::getenv("NEPTUNE_REVIEW") != nullptr;

        beginTest("Review: the long sounds and the tables of SND come to rest by themselves after one stroke (the pad idle), "
                  "within 90 s");
        {
            const auto& sounds = resonatorSounds();
            for (const auto& sound : sounds)
            {
                // (The others are looked at in "Every sound of SND on a pad".)
                if (! ((double) sound.dec * (double) sound.decScale >= 5.0 || sound.params.at(E::MorphParam) >= (float) res::Gong))
                    continue;
                DrumEngine e;
                e.prepareToPlay(sr, 512);
                for (const auto& kv : padAs(sound.tag))
                    e.setParameter(kv.first, kv.second);
                e.setSlotAftertouchDest(0, DrumATDest::Off);
                juce::AudioBuffer<float> buf(2, 512);
                juce::MidiBuffer midi;
                e.noteOn(0, 1.0f);
                e.noteOff(0);
                double idleAt = -1.0, quietAt = -1.0, peak = 0.0;
                const double limit = 90.0;
                for (int b = 0; b < (int) (limit * sr) / 512 && idleAt < 0.0; ++b)
                {
                    buf.clear();
                    e.processBlock(buf, midi);
                    const double m = buf.getMagnitude(0, 512);
                    peak = std::max(peak, m);
                    if (m < peak * 1.0e-5 && quietAt < 0.0) quietAt = (double) b * 512.0 / sr;
                    if (m >= peak * 1.0e-5) quietAt = -1.0;
                    if (! e.getVoice(0).isActive()) idleAt = (double) b * 512.0 / sr;
                }
                if (say)
                    logMessage(juce::String("[review] ") + sound.tag + ": 100 dB down after " + juce::String(quietAt, 1) + " s, idle after "
                               + juce::String(idleAt, 1) + " s (DEC " + juce::String(sound.dec * sound.decScale, 2) + " s)");
                expect(idleAt >= 0.0, juce::String(sound.tag) + " is still awake " + juce::String(limit, 0) + " s after one stroke");
            }
        }

        beginTest("Review: a kit of every type under anything - cells that are no numbers or far out, LFOs on the resonator's "
                  "cells, pressure, bend, the strip, TYPE and SND changed while it rings - at 22.05 to 192 kHz in blocks of 1 to "
                  "4096: finite, bounded, and at rest again");
        {
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            const float wild[] = { nan, inf, -inf, 1.0e30f, -1.0e30f, 0.0f, 1.0f, -1.0f, 13.0f, 0.5f };
            struct Run { double rate; int block; };
            for (const Run run : { Run { 22050.0, 1 }, Run { 48000.0, 7 }, Run { 44100.0, 4096 }, Run { 192000.0, 333 } })
            {
                DrumEngine e;
                e.prepareToPlay(run.rate, run.block);
                uint32_t seed = 0x1234567u + (uint32_t) run.block;
                auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
                for (int padIndex = 0; padIndex < 16; ++padIndex)
                {
                    e.setParameter(padIndex * 8 + D::TYPE, (float) (padIndex % D::NUM_DRUM_TYPES));
                    if (e.isResonator(padIndex))
                        e.setParameter(soundCell(D::RsnSound, padIndex), (float) (next() % resonatorSounds().size()));
                }
                e.setParameter(8 * 8 + D::TYPE, (float) (int) DrumSynthType::Resonator);
                const auto& dests = D::lfoDests();
                e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 9.0f);
                e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 1.0f);
                e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) dests.indexOfLabel("MRPH"));
                e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Rate), 31.0f);
                e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Depth), 1.0f);
                e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Dest), (float) dests.indexOfLabel("HAND"));
                juce::AudioBuffer<float> buf(2, run.block);
                juce::MidiBuffer midi;
                bool ok = true;
                float most = 0.0f;
                const int blocks = std::max(24, (int) (1.5 * run.rate) / run.block);
                for (int b = 0; b < blocks; ++b)
                {
                    const int events = run.block >= 256 ? 12 : (b % std::max(1, 600 / run.block) == 0 ? 3 : 0);
                    for (int k = 0; k < events; ++k)
                    {
                        const int padIndex = (int) (next() % 16), what = (int) (next() % 9);
                        const float v = wild[next() % 10];
                        if (what == 0)      e.noteOn(padIndex, 0.2f + 0.8f * (float) (next() % 100) / 100.0f);
                        else if (what == 1) e.noteOff(padIndex);
                        else if (what == 2) e.aftertouch(padIndex, (float) (next() % 100) / 100.0f);
                        else if (what == 3) e.setParameter(soundCell((int) (next() % 8), padIndex), v);
                        else if (what == 4) e.setParameter(padIndex * 8 + (int) (next() % 6), v == 0.0f ? 20.0f : v * 9000.0f);
                        else if (what == 5) e.pitchBend((float) (next() % 200) / 100.0f - 1.0f);
                        else if (what == 6) e.modWheel((float) (next() % 100) / 100.0f);
                        else if (what == 7) e.setParameter(padIndex * 8 + D::TYPE, (float) (next() % D::NUM_DRUM_TYPES));
                        else                e.getVoice(padIndex).choke();
                    }
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < run.block; ++i)
                        {
                            const float y = buf.getSample(ch, i);
                            ok = ok && std::isfinite(y);
                            most = std::max(most, std::abs(y));
                        }
                }
                expect(ok && most < 40.0f, "at " + juce::String(run.rate, 0) + " in blocks of " + juce::String(run.block) + ": finite, at most "
                       + juce::String(most, 2));
                if (say)
                    logMessage("[review] fuzz at " + juce::String(run.rate, 0) + " / " + juce::String(run.block) + ": peak " + juce::String(most, 2));
                // Everything let go and a hand on every pad: the kit is at rest and exactly silent.
                e.pitchBend(0.0f);
                e.allNotesOff();
                for (int padIndex = 0; padIndex < 16; ++padIndex)
                {
                    e.aftertouch(padIndex, 0.0f);
                    e.getVoice(padIndex).choke();
                }
                for (int b = 0; b < std::max(4, (int) (2.0 * run.rate) / run.block); ++b) { buf.clear(); e.processBlock(buf, midi); }
                int awake = 0;
                for (int padIndex = 0; padIndex < 16; ++padIndex)
                    awake += e.getVoice(padIndex).isActive() ? 1 : 0;
                expect(awake == 0 && buf.getMagnitude(0, run.block) == 0.0f, "at " + juce::String(run.rate, 0) + ": " + juce::String(awake)
                       + " pads still awake two seconds after a hand on every one");
            }
        }

        beginTest("Review: a turned cell of a ringing resonator drum does not click (MRPH across the tables, DAMP, WIRE, DIR, "
                  "MEET; TUNE, DEC, TONE, SNAP)");
        {
            // The largest second difference of the signal (what a step in it or in its slope leaves)
            // while the cell is turned as an encoder turns it, a step every 10 ms, against the
            // same sound with the cell standing at the start, the middle and the end of the turn.
            auto roughness = [&](const char* tag, int cell, float from, float to, bool own, bool turn)
            {
                DrumEngine e;
                e.prepareToPlay(sr, 64);
                for (const auto& kv : padAs(tag))
                    e.setParameter(kv.first, kv.second);
                e.setSlotAftertouchDest(0, DrumATDest::Off);
                const int id = own ? cell : soundCell(cell);
                e.setParameter(id, from);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::vector<float> x;
                e.noteOn(0, 1.0f);
                const int blocks = (int) (0.9 * sr) / 64, start = (int) (0.1 * sr) / 64;
                for (int b = 0; b < blocks; ++b)
                {
                    if (turn && b >= start && b % 8 == 0)
                        e.setParameter(id, from + (to - from) * std::min(1.0f, (float) (b - start) / (float) (blocks - start - 60)));
                    buf.clear();
                    e.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i) x.push_back(buf.getSample(0, i));
                }
                // ... told from what is merely bright by how far it stands out of the 10 ms around it.
                std::vector<double> d(x.size(), 0.0);
                for (size_t i = 2; i < x.size(); ++i)
                    d[i] = (double) x[i] - 2.0 * (double) x[i - 1] + (double) x[i - 2];
                double worst = 0.0;
                const size_t half = 240;
                for (size_t from = (size_t) (0.1 * sr); from + 2 * half < d.size(); from += half)
                {
                    double sum = 0.0, top = 0.0;
                    for (size_t i = from; i < from + 2 * half; ++i) { sum += d[i] * d[i]; top = std::max(top, std::abs(d[i])); }
                    const double around = std::sqrt(sum / (double) (2 * half));
                    if (top > 1.0e-4)
                        worst = std::max(worst, top / std::max(around, 1.0e-9));
                }
                return worst;
            };
            struct Turn { const char* tag; int cell; float from, to; bool own; const char* name; };
            const Turn turns[] = {
                { "GONG", D::RsnMorph, 0.0f, 6.0f, false, "MRPH up through the tables" },
                { "GONG", D::RsnMorph, 0.0f, -7.0f, false, "MRPH down to a string" },
                { "TMGN", D::RsnMorph, 0.0f, 7.0f, false, "a head's MRPH" },
                { "CRSH", D::RsnDamp, -1.0f, 1.0f, false, "DAMP" },
                { "GONG", D::RsnWire, 0.0f, 1.0f, false, "WIRE" },
                { "GNGS", D::RsnDirect, -1.0f, 1.0f, false, "DIR" },
                { "GONG", D::RsnMeet, -1.0f, 1.0f, false, "MEET" },
                { "GONG", D::TUNE, 196.0f, 392.0f, true, "TUNE" },
                { "GONG", D::DECAY, 1.5f, 0.05f, true, "DEC" },
                { "GONG", D::TONE, 0.0f, 1.0f, true, "TONE" },
                { "RIDE", D::SNAP, 0.0f, 1.0f, true, "SNAP" } };
            for (const auto& t : turns)
            {
                const float mid = 0.5f * (t.from + t.to);
                const double still = std::max({ roughness(t.tag, t.cell, t.from, t.from, t.own, false), roughness(t.tag, t.cell, mid, mid, t.own, false),
                                                roughness(t.tag, t.cell, t.to, t.to, t.own, false) });
                const double turned = roughness(t.tag, t.cell, t.from, t.to, t.own, true);
                if (say)
                    logMessage(juce::String("[review] ") + t.tag + " " + t.name + ": stands out " + juce::String(turned, 2) + " times turned, "
                               + juce::String(still, 2) + " standing");
                expect(turned < 1.5 * still + 1.0, juce::String(t.tag) + " " + t.name + " clicks: stands out " + juce::String(turned, 2) + " times turned against "
                       + juce::String(still, 2) + " standing");
            }
        }

        beginTest("Review: a kit given as a DrumKit - a clave and a rim shot in it are the sounds CLAV and RIM of the resonator "
                  "drum, at the kit's pitch; DRIV on a snare adds wires (the sound's own share of it)");
        {
            DrumEngine e;
            e.prepareToPlay(sr, 64);
            DrumKit kit;
            kit.slots[2].synthType = DrumSynthType::Clave;
            kit.slots[2].params[0] = 2400.0f; kit.slots[2].params[1] = 0.3f; kit.slots[2].params[2] = 0.5f; kit.slots[2].params[4] = 0.5f; kit.slots[2].params[5] = 0.7f;
            kit.slots[3].synthType = DrumSynthType::Rim;
            kit.slots[3].params[0] = 750.0f; kit.slots[3].params[1] = 0.3f; kit.slots[3].params[2] = 0.5f; kit.slots[3].params[4] = 0.5f; kit.slots[3].params[5] = 0.7f;
            e.loadKit(kit);
            expect(e.isResonator(2) && e.isResonator(3), "both are resonator drums");
            expectEquals((int) e.getParameter(soundCell(D::RsnSound, 2)), resonatorSoundIndex("CLAV"), "the clave is CLAV");
            expectEquals((int) e.getParameter(soundCell(D::RsnSound, 3)), resonatorSoundIndex("RIM"), "the rim shot is RIM");
            expectWithinAbsoluteError(e.getParameter(2 * 8 + D::TUNE), 2400.0f, 0.5f);
            expectWithinAbsoluteError(e.getParameter(2 * 8 + D::DECAY), CymbalPresets::foldedDecay(DrumSynthType::Clave, 0.3f), 1.0e-5f);

            // The snare's wires with DRIV: the same pad with the sound's share of DRIV taken away is another sound.
            auto snare = [&](bool wires)
            {
                DrumEngine d;
                d.prepareToPlay(sr, 64);
                for (const auto& kv : padAs("SNAR"))
                    d.setParameter(kv.first, kv.second);
                d.setParameter(D::DRIVE, 1.0f);
                const auto& sound = resonatorSounds()[(size_t) resonatorSoundIndex("SNAR")];
                expect(sound.driveWire > 0.0f, "SNAR has a share of DRIV on WIRE");
                if (! wires)
                    d.getVoice(0).setResonatorHost(sound.decScale, sound.pressEnergy, sound.pressDamp, sound.gated, ! sound.rings(), 0.0f, sound.dec);
                juce::AudioBuffer<float> buf(2, 64);
                juce::MidiBuffer midi;
                std::vector<float> x;
                d.noteOn(0, 1.0f);
                for (int b = 0; b < (int) (0.3 * sr) / 64; ++b)
                {
                    buf.clear();
                    d.processBlock(buf, midi);
                    for (int i = 0; i < 64; ++i) x.push_back(buf.getSample(0, i));
                }
                return x;
            };
            const auto with = snare(true), without = snare(false);
            const double top = bandLevel(with, 1200.0, 3600.0, 480, 8192) / std::max(1.0e-12, bandLevel(without, 1200.0, 3600.0, 480, 8192));
            logMessage("SNAR at DRIV 1: the wires' band (6 to 18 times the note) is " + juce::String(cymbaltest::dB(top), 1) + " dB over the same without the sound's share");
            expect(top > 1.25, "DRIV adds wires: " + juce::String(cymbaltest::dB(top), 1) + " dB");
        }

        storedTypeTests();

       #if JUCE_MAC
        beginTest("Review: what a step lock may do to a pad on the audio thread allocates nothing - TYPE to every type and back "
                  "(its sound page starts over), every cell of the sound page, a stroke, a block");
        {
            DrumEngine e;
            e.prepareToPlay(sr, 64);
            juce::AudioBuffer<float> buf(2, 64);
            juce::MidiBuffer midi;
            // (Once before it is counted: what is built at the first use is built.)
            for (int pass = 0; pass < 2; ++pass)
            {
                const int allocations = reviewalloc::during([&]
                {
                    for (int type = 0; type < D::NUM_DRUM_TYPES; ++type)
                    {
                        e.setParameter(5 * 8 + D::TYPE, (float) type);
                        for (int c = 1; c < D::SOUND_CELLS; ++c)        // (not SND: it cannot be locked)
                            e.setParameter(soundCell(c, 5), 0.3f);
                        e.noteOn(5, 0.8f);
                        buf.clear();
                        e.processBlock(buf, midi);
                    }
                });
                if (pass == 1)
                    expectEquals(allocations, 0, "allocations while a pad's TYPE and cells were set and played");
            }
        }
       #endif

        beginTest("Review: cost of the kit's LFOs on the resonator's cells (NEPTUNE_DRUM_BENCH=1 prints it; any build: sixteen "
                  "ringing pads under two LFOs cost less than eight times what they cost without)");
        {
            double took[2] = {};
            for (int lfos = 0; lfos < 2; ++lfos)
            {
                DrumEngine e;
                e.prepareToPlay(sr, 512);
                static const char* const tags[16] = { "GONG", "TAM", "CRSH", "RIDE", "CUP", "CHNA", "GNGB", "TAM2",
                                                     "CRS2", "RID2", "GNG2", "CUP2", "GONG", "TAM", "CRSH", "RIDE" };
                std::map<int, float> p;
                for (int i = 0; i < 16; ++i)
                    CymbalPresets::resonator(p, i, tags[i], 0.3f, 0.5f, 1.0f + 0.03f * (float) i);
                for (const auto& kv : p)
                    e.setParameter(kv.first, kv.second);
                if (lfos)
                {
                    const auto& dests = D::lfoDests();
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Rate), 3.0f);
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Depth), 0.1f);
                    const char* d0 = std::getenv("NEPTUNE_REVIEW_D0"); const char* d1 = std::getenv("NEPTUNE_REVIEW_D1");
                    e.setParameter(EngineLfoSection::id(0, EngineLfoSection::Dest), (float) dests.indexOfLabel(d0 ? d0 : "MRPH"));
                    e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Rate), 5.0f);
                    e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Depth), 0.3f);
                    e.setParameter(EngineLfoSection::id(1, EngineLfoSection::Dest), (float) dests.indexOfLabel(d1 ? d1 : "HARD"));
                }
                took[lfos] = microsecondsPerBlock(e, [&](int b)
                {
                    if (b % 24 == 0)
                        for (int padIndex = 0; padIndex < 16; ++padIndex) { e.noteOn(padIndex, 0.9f); e.noteOff(padIndex); }
                }, 300);
            }
            if (std::getenv("NEPTUNE_DRUM_BENCH") != nullptr)
                logMessage("sixteen long resonator drums: " + juce::String(took[0], 0) + " us a block; with the kit's two LFOs on MRPH and HARD: "
                           + juce::String(took[1], 0) + " us");
            // (They cost what a body that moves costs - about four times a body that stands, unoptimised -
            // and were fifteen times before the cells were looked at every 16 samples instead of every one.)
            expect(took[1] < 60000.0 && took[1] < 8.0 * took[0], "sixteen pads under two LFOs: " + juce::String(took[1], 0) + " us a block against "
                   + juce::String(took[0], 0) + " without");
        }
    }
};

static ResonatorDrumTests resonatorDrumTests;
