#pragma once

#include <juce_core/juce_core.h>
#include "project/ProjectBridge.h"
#include "engine/ModStripFollower.h"
#include "engine/UnifiedVoiceEngine.h"
#include "arpeggiator/Arpeggiator.h"
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

// -----------------------------------------------------------------------------
// The pin of .ralph/spec-unify.md, section 0: the two unified-voice sounds of the
// owner's project P01 (track 3, "User 13", the keyboard sound; track 1, "DuneDrone")
// render the same samples, bit for bit, as they did at commit 66a3f06.
//
// tests/fixtures/p01_keyboard.json is P01 cut down, in the project file format: the
// stored sound of tracks 1 and 3 (every parameter, their legacy "arp" block), the
// other six tracks with their engine type and nothing else (the loader gives them
// their defaults), no steps, the arpeggiator lists, ROLL, the two send buses, the
// master chain and level, the pressure routes of every track, the keyboard split.
// Patterns 1-13 are empty, pattern 14 (the active one) carries the sounds. Left
// out: steps, chord pads, scale pages. It is read by ProjectData::fromJSON and put
// into a real SequencerCore + AudioEngine by ProjectBridge::apply, as the app does.
//
// Each section is a fresh instrument, one track played, rendered through the whole
// path (engine, insert, sends, master) with AudioEngine::renderBlock:
//   notes     keys: low / mid / high, each at two velocities, with their releases
//   chord     keys: four notes held, released together
//   pressure  two pads held (AudioEngine::padNoteOn), pressure moving on one of them
//   strip     a key held, the mod strip up and back (ModStripFollower, as the app)
//   bend      two keys held, the pitch strip up, down and back
//
// What makes a render of these sounds differ from run to run: UnifiedVoice::noteOn
// draws from juce::Random::getSystemRandom() (a random start phase per oscillator,
// and SLOP, WVAR, FVAR, EVAR per note), and both sounds have all of them set. So
// there are two families:
//   plain   the five cells that are drawn per note (PHAS, SLOP, WVAR, FVAR, EVAR)
//           set to zero through the track's parameters, everything else as stored
//           (drift and the soft start stay). 48 kHz / 256 and 44.1 kHz / 64.
//   seeded  the sound exactly as stored, the system random put in a known state
//           before the section. 48 kHz / 256. This is what pins the per-note
//           spread; it also pins the ORDER of the draws in noteOn. JUCE asserts
//           when the system random is seeded: in a Debug build each section of
//           this family logs one "JUCE Assertion failure in juce_Random.cpp" line.
//           They are expected.
// A change that fails "seeded" alone changed the per-note spread or the order of
// its draws; one that fails "plain" changed the sound itself.
//
// The stored checksums were made at 66a3f06, in a Debug and in a Release build of
// the tests. Each build type has its own column; on the machine they were made on
// (Apple clang, arm64) the two came out equal. They are never re-made to make a
// failure go away: a failure says the owner's sound changed.
//   NEPTUNE_P01_PRINT=1      prints the table of checksums and what the two sounds use
//   NEPTUNE_P01_FILE=<file>  renders that project file instead of the fixture (read only)
// -----------------------------------------------------------------------------
class P01KeyboardTests : public juce::UnitTest
{
public:
    P01KeyboardTests() : UnitTest("P01 Keyboard") {}

    static constexpr int KEYBOARD_TRACK = 2;    // track 3, "User 13"
    static constexpr int DRONE_TRACK = 0;       // track 1, "DuneDrone"

    struct Pin { const char* name; uint64_t debug, release; };

    // Made at 66a3f06 with this file and the fixture copied into a worktree of that commit.
    // Debug: the build the tests run in. Release: the test binary with -DCMAKE_BUILD_TYPE=Release.
    static const std::vector<Pin>& pins()
    {
        static const std::vector<Pin> table = {
            // P01-PINS-BEGIN  (name, Debug, Release)
            { "plain T3 48k notes",       0x2925b54bd6bb8f79ull, 0x2925b54bd6bb8f79ull },
            { "plain T3 48k chord",       0xdae66758a9048cb0ull, 0xdae66758a9048cb0ull },
            { "plain T3 48k pressure",    0xee98516c8a2a2b21ull, 0xee98516c8a2a2b21ull },
            { "plain T3 48k strip",       0x6f2793f9ba8bea5aull, 0x6f2793f9ba8bea5aull },
            { "plain T3 48k bend",        0x1785161bf5d00fc5ull, 0x1785161bf5d00fc5ull },
            { "plain T1 48k notes",       0x75e34f16735da938ull, 0x75e34f16735da938ull },
            { "plain T1 48k chord",       0xe880c55709d49e1cull, 0xe880c55709d49e1cull },
            { "plain T1 48k pressure",    0xd0f4e3e6862d7858ull, 0xd0f4e3e6862d7858ull },
            { "plain T1 48k strip",       0x26c327f52c488b87ull, 0x26c327f52c488b87ull },
            { "plain T1 48k bend",        0xc1c24034dad7bf86ull, 0xc1c24034dad7bf86ull },
            { "plain T3 44k notes",       0xfb97a2022a79f410ull, 0xfb97a2022a79f410ull },
            { "plain T3 44k chord",       0xd510fb5d10d6cfb2ull, 0xd510fb5d10d6cfb2ull },
            { "plain T3 44k pressure",    0x7e803399d9b89b01ull, 0x7e803399d9b89b01ull },
            { "plain T3 44k strip",       0x732addaeb2080e61ull, 0x732addaeb2080e61ull },
            { "plain T3 44k bend",        0xd470f2c238698d65ull, 0xd470f2c238698d65ull },
            { "plain T1 44k notes",       0xebf996a2e42b8e1full, 0xebf996a2e42b8e1full },
            { "plain T1 44k chord",       0x545a8423837a65beull, 0x545a8423837a65beull },
            { "plain T1 44k pressure",    0xe74c51106f7e304aull, 0xe74c51106f7e304aull },
            { "plain T1 44k strip",       0xaa3e8bb9cd932c63ull, 0xaa3e8bb9cd932c63ull },
            { "plain T1 44k bend",        0x4a072bdbc5526dbcull, 0x4a072bdbc5526dbcull },
            { "seeded T3 48k notes",      0xeb33a81327745299ull, 0xeb33a81327745299ull },
            { "seeded T3 48k chord",      0x6f51e0f64953dee5ull, 0x6f51e0f64953dee5ull },
            { "seeded T3 48k pressure",   0x33eb8508f81b8dbbull, 0x33eb8508f81b8dbbull },
            { "seeded T3 48k strip",      0x5d159faddd2db838ull, 0x5d159faddd2db838ull },
            { "seeded T3 48k bend",       0x93f17631051f72eeull, 0x93f17631051f72eeull },
            { "seeded T1 48k notes",      0x4633750d33225565ull, 0x4633750d33225565ull },
            { "seeded T1 48k chord",      0xcdd2564117a71708ull, 0xcdd2564117a71708ull },
            { "seeded T1 48k pressure",   0x4fd3f13bafbe5b5aull, 0x4fd3f13bafbe5b5aull },
            { "seeded T1 48k strip",      0xe088b573a79573d4ull, 0xe088b573a79573d4ull },
            { "seeded T1 48k bend",       0x4418b008da90932bull, 0x4418b008da90932bull },
            // P01-PINS-END
        };
        return table;
    }

    static juce::String hex(uint64_t v) { return "0x" + juce::String::toHexString((juce::int64) v).paddedLeft('0', 16); }

    static juce::File fixtureFile()
    {
        const auto other = juce::SystemStats::getEnvironmentVariable("NEPTUNE_P01_FILE", {});
        if (other.isNotEmpty())
            return juce::File(other);
        return juce::File(NEPTUNE_TESTS_DIR).getChildFile("fixtures").getChildFile("p01_keyboard.json");
    }

    // ---- The instrument with the project loaded ----------------------------------------------
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        std::array<Arpeggiator, 8> arps;
        KeyboardSetup keyboard;
        ModStripFollower strip;
        std::vector<float> left, right;
        double sampleRate;
        int block;
        bool loaded = false;
        bool arp3WasOn = false;

        Rig(double sr, int blockSize) : left((size_t) blockSize), right((size_t) blockSize), sampleRate(sr), block(blockSize)
        {
            seq.prepareToPlay(sr, blockSize);
            audio.setSequencer(&seq);
            audio.prepare(sr, blockSize);
            for (int t = 0; t < (int) arps.size(); ++t)
                audio.setTrackArpeggiator(t, &arps[(size_t) t]);

            const auto file = fixtureFile();
            if (! file.existsAsFile())
                return;
            const auto data = ProjectData::fromJSON(file.loadFileAsString());
            ProjectBridge::Live live { seq, audio };
            live.arpForTrack = [this](int t) { return t >= 0 && t < (int) arps.size() ? &arps[(size_t) t] : nullptr; };
            live.getKeyboard = [this] { return keyboard; };
            live.setKeyboard = [this](const KeyboardSetup& k) { keyboard = k; };
            ProjectBridge::apply(data, live);
            loaded = true;

            // The notes are played straight to the engine, as a key is with the arpeggiator off.
            // (Track 3's arpeggiator is stored ON with latch; the arpeggiator is not part of the pin.)
            arp3WasOn = arps[(size_t) KEYBOARD_TRACK].isEnabled();
            for (auto& a : arps)
                a.setEnabled(false);
        }

        EngineBase* engine(int track) { return audio.getEngine(track); }

        // The cells whose value is drawn anew at every note-on, at zero.
        void noSpread(int track)
        {
            for (int id : { UnifiedParam::RANDOM_PHASE, UnifiedParam::SLOP, UnifiedParam::WAVE_VAR,
                            UnifiedParam::FILTER_VAR, UnifiedParam::ENV_VAR })
                audio.setTrackParameter(track, id, 0.0f);
        }
    };

    // ---- The script --------------------------------------------------------------------------
    struct Event { double time; std::function<void(Rig&)> act; };
    struct Section { const char* name; double seconds; std::vector<Event> events; };

    static float cc(float x) { return (float) juce::roundToInt(juce::jlimit(0.0f, 1.0f, x) * 127.0f) / 127.0f; }

    // A value moved from `a` to `b` between two times, one message every 5 ms.
    template <typename Send>
    static void ramp(std::vector<Event>& ev, double from, double to, float a, float b, Send send)
    {
        const int steps = juce::jmax(1, (int) std::lround((to - from) / 0.005));
        for (int i = 0; i <= steps; ++i)
        {
            const float x = a + (b - a) * (float) i / (float) steps;
            ev.push_back({ from + (to - from) * (double) i / (double) steps, [send, x](Rig& r) { send(r, x); } });
        }
    }

    static std::vector<Section> script(int track)
    {
        auto keyOn = [track](int note, float vel) { return [=](Rig& r) { if (auto* e = r.engine(track)) e->noteOn(note, vel, -1); }; };
        auto keyOff = [track](int note) { return [=](Rig& r) { if (auto* e = r.engine(track)) e->noteOff(note, -1); }; };
        std::vector<Section> s;

        {
            Section sec { "notes", 5.0, {} };
            const int notes[] = { 36, 60, 84 };
            int slot = 0;
            for (int velocity : { 50, 127 })
                for (int note : notes)
                {
                    const double at = 0.6 * slot++;
                    sec.events.push_back({ at, keyOn(note, (float) velocity / 127.0f) });
                    sec.events.push_back({ at + 0.35, keyOff(note) });
                }
            s.push_back(std::move(sec));
        }
        {
            Section sec { "chord", 2.5, {} };
            for (int note : { 48, 55, 60, 64 })
            {
                sec.events.push_back({ 0.0, keyOn(note, 100.0f / 127.0f) });
                sec.events.push_back({ 1.2, keyOff(note) });
            }
            s.push_back(std::move(sec));
        }
        {
            // Two pads held; the pressure moves on the first one only.
            Section sec { "pressure", 2.2, {} };
            sec.events.push_back({ 0.0, [track](Rig& r) { r.audio.padNoteOn(track, 3, 62, 90.0f / 127.0f); } });
            sec.events.push_back({ 0.0, [track](Rig& r) { r.audio.padNoteOn(track, 5, 50, 90.0f / 127.0f); } });
            auto press = [](Rig& r, float x) { r.audio.padPressure(3, cc(x)); };
            ramp(sec.events, 0.2, 0.8, 0.0f, 1.0f, press);
            ramp(sec.events, 0.9, 1.1, 1.0f, 0.3f, press);
            sec.events.push_back({ 1.3, [](Rig& r) { r.audio.padNoteOff(3); } });
            sec.events.push_back({ 1.4, [](Rig& r) { r.audio.padNoteOff(5); } });
            s.push_back(std::move(sec));
        }
        {
            Section sec { "strip", 2.3, {} };
            sec.events.push_back({ 0.0, keyOn(57, 100.0f / 127.0f) });
            auto move = [track](Rig& r, float x) { r.strip.moved(r.engine(track), track, cc(x)); };
            ramp(sec.events, 0.2, 0.7, 0.0f, 1.0f, move);
            ramp(sec.events, 1.0, 1.2, 1.0f, 0.0f, move);
            sec.events.push_back({ 1.5, keyOff(57) });
            s.push_back(std::move(sec));
        }
        {
            Section sec { "bend", 2.2, {} };
            sec.events.push_back({ 0.0, keyOn(60, 100.0f / 127.0f) });
            sec.events.push_back({ 0.0, keyOn(67, 100.0f / 127.0f) });
            // (the 14-bit value to -1..1 as the app does it)
            auto bend = [track](Rig& r, float x)
            {
                const int value14 = juce::jlimit(0, 16383, 8192 + juce::roundToInt(x * 8191.0f));
                if (auto* e = r.engine(track))
                    e->pitchBend(juce::jlimit(-1.0f, 1.0f, (float) (value14 - 8192) / 8191.0f));
            };
            ramp(sec.events, 0.2, 0.5, 0.0f, 1.0f, bend);
            ramp(sec.events, 0.6, 1.0, 1.0f, -1.0f, bend);
            ramp(sec.events, 1.1, 1.3, -1.0f, 0.0f, bend);
            sec.events.push_back({ 1.4, keyOff(60) });
            sec.events.push_back({ 1.4, keyOff(67) });
            s.push_back(std::move(sec));
        }
        return s;
    }

    // ---- The render --------------------------------------------------------------------------
    struct Result { uint64_t sum = 1469598103934665603ull; float peak = 0.0f; bool finite = true; };

    static void add(Result& res, const float* samples, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const float s = samples[i];
            if (! std::isfinite(s)) res.finite = false;
            else                    res.peak = juce::jmax(res.peak, std::abs(s));
            uint32_t bits;
            std::memcpy(&bits, &s, sizeof(bits));
            for (int b = 0; b < 4; ++b)
            {
                res.sum ^= (bits >> (8 * b)) & 0xffu;
                res.sum *= 1099511628211ull;
            }
        }
    }

    // Every block: the events that are due, then the whole mix; left then right into the sum.
    static Result render(Rig& rig, const Section& section)
    {
        Result res;
        std::vector<Event> events = section.events;
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.time < b.time; });
        const juce::int64 total = (juce::int64) std::llround(section.seconds * rig.sampleRate);
        size_t next = 0;
        for (juce::int64 pos = 0; pos < total; pos += rig.block)
        {
            while (next < events.size() && (juce::int64) std::llround(events[next].time * rig.sampleRate) <= pos)
                events[next++].act(rig);
            rig.audio.renderBlock(rig.left.data(), rig.right.data(), rig.block);
            add(res, rig.left.data(), rig.block);
            add(res, rig.right.data(), rig.block);
        }
        return res;
    }

    // The thread's system random in a known state. (Random::setSeed would do; combineSeed is
    // used so that the state is checked.) Both assert on the system random in a Debug build.
    static bool seedSystemRandom(juce::int64 state)
    {
        auto& rng = juce::Random::getSystemRandom();
        juce::Random copy(rng.getSeed());
        const juce::int64 drawn = copy.nextInt64();
        rng.combineSeed(state ^ drawn ^ copy.getSeed());
        if (rng.getSeed() != state)
            rng.setSeed(state);
        return rng.getSeed() == state;
    }

    struct Pass { const char* family; const char* label; double sampleRate; int block; bool seeded; };

    void check(const juce::String& name, const Result& res, bool print, juce::StringArray& table)
    {
        expect(res.finite, name + ": a sample that is not a number");
        expect(res.peak > 0.001f, name + ": silent (peak " + juce::String(res.peak, 6) + ")");
        expect(res.peak < 4.0f, name + ": peak " + juce::String(res.peak, 3));
        logMessage(name.paddedRight(' ', 28) + hex(res.sum) + "  peak " + juce::String(res.peak, 4));
        table.add("            { \"" + name + "\", " + hex(res.sum) + "ull },");

        const Pin* pin = nullptr;
        for (auto& p : pins())
            if (name == p.name)
                pin = &p;
       #ifdef NDEBUG
        const uint64_t expected = pin != nullptr ? pin->release : 0;
        const char* build = "Release";
       #else
        const uint64_t expected = pin != nullptr ? pin->debug : 0;
        const char* build = "Debug";
       #endif
        if (print && expected == 0)
            return;
        if (expected == 0)
            expect(false, name + ": no stored checksum for a " + build + " build (actual " + hex(res.sum) + ")");
        else if (expected != res.sum)
            expect(false, name + " [" + build + "]: expected " + hex(expected) + ", actual " + hex(res.sum)
                              + "  (the sound of P01 changed: do not store a new value, find the change)");
    }

    // What the two sounds use, for whoever needs to know which code is held in place.
    void describe(Rig& rig)
    {
        for (int track : { KEYBOARD_TRACK, DRONE_TRACK })
        {
            auto* e = rig.engine(track);
            if (e == nullptr) continue;
            std::cout << "--- track " << track + 1 << " (" << e->getName() << ")" << std::endl;
            for (auto& page : e->getParamPages())
            {
                juce::String line = page.name.paddedRight(' ', 6);
                for (auto& p : page.params)
                {
                    if (! p.isValid()) continue;
                    const float v = e->getParameter(p.id);
                    line << p.label << "=";
                    if (p.kind == ParamKind::Choice && p.choices != nullptr && (int) v >= 0 && (int) v < p.numChoices)
                        line << p.choices[(int) v];
                    else
                        line << juce::String(v, 3);
                    line << "  ";
                }
                std::cout << line << std::endl;
            }
            juce::String mix;
            mix << "mixer  VOL=" << rig.audio.getTrackParameter(track, MixParam::VOLUME) << " PAN=" << rig.audio.getTrackParameter(track, MixParam::PAN);
            for (int b = 0; b < rig.audio.getSendCount(); ++b)
                mix << " SEND" << b + 1 << "=" << rig.audio.getTrackParameter(track, MixParam::SEND_BASE + b);
            for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
            {
                EffectType type;
                if (! rig.audio.getInsertEffectType(track, slot, type)) break;
                mix << "  insert " << slot + 1 << ": type " << (int) type << " (" << AudioEngine::effectTypeName(type) << ")";
            }
            const auto& router = rig.audio.getPressureRouter(track);
            for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
            {
                const auto route = router.effectiveRoute(i);
                if (route.dest != AftertouchRouter::Destination::Off)
                    mix << "  pressure -> " << AftertouchRouter::getDestName(route.dest) << " " << route.depth;
            }
            std::cout << mix << std::endl;
        }
        for (int b = 0; b < rig.audio.getSendCount(); ++b)
            std::cout << "send bus " << b + 1 << ": type " << (int) rig.audio.getSendBusType(b)
                      << " (" << AudioEngine::effectTypeName(rig.audio.getSendBusType(b)) << ")" << std::endl;
        juce::String master = "master";
        for (int i = 0; i < AudioEngine::NUM_MASTER_PARAMS; ++i)
            master << " " << rig.audio.getMasterParam(i);
        std::cout << master << "  level " << rig.audio.getMasterLevel() << std::endl;
    }

    void runTest() override
    {
        const bool print = juce::SystemStats::getEnvironmentVariable("NEPTUNE_P01_PRINT", {}).isNotEmpty();
        const bool ownFixture = juce::SystemStats::getEnvironmentVariable("NEPTUNE_P01_FILE", {}).isEmpty();

        beginTest("the fixture loads through the project path");
        {
            expect(fixtureFile().existsAsFile(), "missing " + fixtureFile().getFullPathName());
            if (! ownFixture)
                logMessage("NEPTUNE_P01_FILE: rendering " + fixtureFile().getFullPathName());
            Rig rig(48000.0, 256);
            expect(rig.loaded);
            if (! rig.loaded)
                return;
            expectEquals(rig.seq.getActivePatternIndex(), 13);
            expectEquals(rig.seq.getPattern().getTrackCount(), 8);
            for (int track : { KEYBOARD_TRACK, DRONE_TRACK })
            {
                expect(rig.engine(track) != nullptr && rig.engine(track)->getType() == EngineType::UnifiedVoice);
                expect(dynamic_cast<UnifiedVoiceEngine*>(rig.engine(track)) != nullptr);
            }
            expect(rig.engine(1) != nullptr && rig.engine(1)->getType() == EngineType::Drum);

            // Track 1: one insert, the hall reverb; track 3: none.
            EffectType type {};
            expect(rig.audio.getInsertEffectType(DRONE_TRACK, 0, type) && type == EffectType::HallReverbEffect);
            expect(! rig.audio.getInsertEffectType(DRONE_TRACK, 1, type));
            expect(! rig.audio.getInsertEffectType(KEYBOARD_TRACK, 0, type));
            // Send 1 the reverb, send 2 the delay; both tracks send to them.
            expectEquals(rig.audio.getSendCount() >= 2 ? (int) rig.audio.getSendBusType(0) : -1, (int) EffectType::ReverbEffect);
            expectEquals(rig.audio.getSendCount() >= 2 ? (int) rig.audio.getSendBusType(1) : -1, (int) EffectType::DelayEffect);
            expect(rig.audio.getTrackParameter(KEYBOARD_TRACK, MixParam::SEND_BASE) > 0.1f);
            expect(rig.audio.getTrackParameter(KEYBOARD_TRACK, MixParam::SEND_BASE + 1) > 0.1f);
            expect(rig.audio.getTrackParameter(DRONE_TRACK, MixParam::SEND_BASE + 1) > 0.1f);
            expectWithinAbsoluteError(rig.audio.getMasterLevel(), 0.8f, 1.0e-6f);
            // Pressure to the voice's level on both; the keys' split; the arpeggiator as stored.
            for (int track : { KEYBOARD_TRACK, DRONE_TRACK })
            {
                const auto route = rig.audio.getPressureRouter(track).effectiveRoute(0);
                expect(route.dest == AftertouchRouter::Destination::Volume && route.depth == 1.0f);
            }
            expectEquals(rig.keyboard.split, 2);
            expect(rig.arp3WasOn, "track 3's arpeggiator is stored on");
            // The per-note spread that makes the seeded family necessary.
            for (int track : { KEYBOARD_TRACK, DRONE_TRACK })
            {
                expect(rig.engine(track)->getParameter(UnifiedParam::RANDOM_PHASE) > 0.5f);
                expect(rig.engine(track)->getParameter(UnifiedParam::SLOP) > 0.0f);
                expect(rig.engine(track)->getParameter(UnifiedParam::FILTER_VAR) > 0.0f);
                expect(rig.engine(track)->getParameter(UnifiedParam::ENV_VAR) > 0.0f);
            }
            if (print)
                describe(rig);
        }

        beginTest("the two sounds render the samples they did at 66a3f06");
        {
            if (! ownFixture)
                logMessage("(compared with the fixture's stored checksums)");
            static const Pass passes[] = {
                { "plain",  "48k", 48000.0, 256, false },
                { "plain",  "44k", 44100.0, 64,  false },
                { "seeded", "48k", 48000.0, 256, true  },
            };
            juce::StringArray table;
            const juce::int64 randomBefore = juce::Random::getSystemRandom().getSeed();
            for (auto& pass : passes)
                for (int track : { KEYBOARD_TRACK, DRONE_TRACK })
                {
                    const auto sections = script(track);
                    for (size_t i = 0; i < sections.size(); ++i)
                    {
                        const juce::String name = juce::String(pass.family) + " T" + juce::String(track + 1) + " " + pass.label + " " + sections[i].name;
                        Rig rig(pass.sampleRate, pass.block);
                        if (! rig.loaded) { expect(false, name + ": not loaded"); continue; }
                        rig.seq.setSelectedTrack(track);
                        if (pass.seeded)
                            expect(seedSystemRandom((juce::int64) (0x5eed0000 + 16 * track + (int) i)), name + ": the system random could not be set");
                        else
                            rig.noSpread(track);
                        const Result res = render(rig, sections[i]);
                        expectEquals(rig.audio.getNonFiniteBlocks(), 0, name);
                        check(name, res, print, table);
                    }
                }
            seedSystemRandom(randomBefore);     // (the suites that follow draw as they would have)

            if (print)
            {
               #ifdef NDEBUG
                std::cout << "P01 checksums (Release)" << std::endl;
               #else
                std::cout << "P01 checksums (Debug)" << std::endl;
               #endif
                for (auto& line : table)
                    std::cout << line << std::endl;
            }
        }
    }
};

static P01KeyboardTests p01KeyboardTests;
