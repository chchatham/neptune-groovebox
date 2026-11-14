#pragma once
#include <juce_core/juce_core.h>
#include <memory>
#include <set>
#include <vector>
#include "control_grammar_tests.h"
#include "../src/ui/screens/PresetScreen.h"
#include "../src/engine/UnifiedVoiceEngine.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/project/PresetManager.h"
#include "../src/project/ProjectBridge.h"
#include "../src/arpeggiator/Arpeggiator.h"
#include "../src/controller/CustomScaleManager.h"

// PRESET: the highlighted preset takes over the selected track's sound for listening only; the
// track's own sound returns on every way out of the screen unless LD kept the preset.
namespace presetaudition
{
    // Everything that makes a track's sound, as the engine / mixer / effects hold it.
    struct Sound
    {
        EngineType engine = EngineType::UnifiedVoice, trackType = EngineType::UnifiedVoice;
        ParamMap stored;                    // Track::soundParams
        std::vector<std::pair<int, float>> values;      // every engine id, mixer, effect blocks
        std::vector<float> routes;
    };

    inline std::vector<std::pair<int, float>> engineValues(const EngineBase& e)
    {
        std::vector<std::pair<int, float>> v;
        for (int id = 0; id < EngineParamRange::THIRD_END; ++id)
            if (EngineParamRange::contains(id))
                v.push_back({ id, e.getParameter(id) });
        return v;
    }

    inline Sound soundOf(SequencerCore& seq, AudioEngine& audio, int t)
    {
        Sound s;
        auto& track = seq.getPattern().getTrack(t);
        s.engine = audio.getEngineTypeOf(t);
        s.trackType = track.engineType;
        s.stored = track.soundParams;
        if (auto* e = audio.getEngine(t))
            s.values = engineValues(*e);
        for (int id = MixParam::VOLUME; id < MixParam::SEND_BASE + MixParam::MAX_SENDS; ++id)
            s.values.push_back({ id, audio.getTrackParameter(t, id) });
        for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
            s.values.push_back({ id, audio.getTrackParameter(t, id) });
        for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
        {
            auto r = audio.getPressureRouter(t).effectiveRoute(i);
            s.routes.push_back((float) (int) r.dest);
            s.routes.push_back(r.depth);
            s.routes.push_back(r.bipolar ? 1.0f : 0.0f);
        }
        return s;
    }

    // Empty when the two are the same sound; otherwise the first difference.
    inline juce::String difference(const Sound& a, const Sound& b)
    {
        if (a.engine != b.engine) return "engine type " + juce::String((int) a.engine) + " / " + juce::String((int) b.engine);
        if (a.trackType != b.trackType) return "track engine type";
        if (a.values.size() != b.values.size()) return "value count";
        for (size_t i = 0; i < a.values.size(); ++i)
            if (a.values[i].second != b.values[i].second)
                return "id " + juce::String(a.values[i].first) + ": " + juce::String(a.values[i].second, 6)
                       + " / " + juce::String(b.values[i].second, 6);
        if (a.stored != b.stored) return "stored parameters";
        if (a.routes != b.routes) return "pressure routes";
        return {};
    }

    inline juce::String engineDifference(const std::vector<std::pair<int, float>>& a, const std::vector<std::pair<int, float>>& b)
    {
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (a[i].second != b[i].second)
                return "id " + juce::String(a[i].first) + ": " + juce::String(a[i].second, 6) + " / " + juce::String(b[i].second, 6);
        return {};
    }

    // What a new engine holds after a preset was loaded into it.
    inline std::vector<std::pair<int, float>> freshValues(const Preset& p)
    {
        SequencerCore seq;
        AudioEngine audio;
        audio.setSequencer(&seq);
        audio.addEngine(0, AudioEngine::createEngine(p.engineType));
        audio.applyAllTrackParameters(0, p.soundParams);
        return engineValues(*audio.getEngine(0));
    }

    // The screen alone: track 0 with a sound of its own (not a preset, not the defaults), track 1 FM.
    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        PresetManager mgr;
        std::unique_ptr<PresetScreen> screen;
        juce::int64 now = 1000;

        explicit Rig(EngineType type = EngineType::UnifiedVoice)
        {
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(type));
            seq.getPattern().getTrack(0).engineType = type;
            audio.addEngine(1, AudioEngine::createEngine(EngineType::FM));
            seq.getPattern().getTrack(1).engineType = EngineType::FM;
            makeOwnSound(0);
            screen = std::make_unique<PresetScreen>(seq, &audio, mgr);
            screen->setClock([this] { return now; });
        }

        void set(int t, int id, float v)
        {
            audio.setTrackParameter(t, id, v);
            seq.getPattern().getTrack(t).soundParams[id] = v;
        }

        // The last preset of the track's engine with every page value nudged, a mixer setting, an
        // effect block, a pressure range and a pressure route: nothing a preset would put there.
        void makeOwnSound(int t)
        {
            const auto type = audio.getEngineTypeOf(t);
            auto list = mgr.getFilteredIndices(type);
            if (!list.empty())
                for (auto& [id, v] : mgr.getPreset(list.back()).soundParams)
                    set(t, id, v);
            auto* e = audio.getEngine(t);
            for (auto& page : e->getParamPages())
                for (auto& p : page.params)
                    if (p.isValid() && !e->isRebuildParam(p.id) && p.id != e->getMasterParamId())
                    {
                        const float v = e->getParameter(p.id);
                        const float moved = p.turned(v, 1, TurnStep::Plain);
                        set(t, p.id, moved != v ? moved : p.turned(v, -1, TurnStep::Plain));
                    }
            set(t, MixParam::VOLUME, 0.33f);
            set(t, MixParam::PAN, -0.25f);
            set(t, MixParam::SEND_BASE, 0.4f);
            set(t, TrackFxParam::DIRT_BASE, audio.getTrackParameter(t, TrackFxParam::DIRT_BASE) + 0.25f);
            seq.getPattern().getTrack(t).soundParams[RangeParam::LO_BASE + 10] = 0.2f;
            audio.setPressureRoute(t, 1, { AudioEngine::PressureDest::Volume, 0.4f, false });
        }

        Sound sound(int t = 0) { return soundOf(seq, audio, t); }
        void turn(int knob, int clicks = 1) { screen->onEncoderChange(knob, clicks < 0 ? 128 + clicks : clicks); }
        void confirm(int knob) { for (int i = 0; i < 4; ++i) { turn(knob); now += 150; } }

        // The highlighted preset (the screen's readout names it).
        const Preset* highlighted() const
        {
            const auto name = screen->getEncoderReadout(0).fromFirstOccurrenceOf("PRESET ", false, false);
            for (int i = 0; i < mgr.getPresetCount(); ++i)
                if (mgr.getPreset(i).name == name)
                    return &mgr.getPreset(i);
            return nullptr;
        }
    };

    // The screen inside a ScreenManager, driven by controller messages: TRACKS (0), PRESET (1),
    // SETTINGS (2), ARP (3); Shift + pad 1 opens TRACKS, Shift + pad 5 PRESET.
    struct Nav
    {
        SequencerCore seq;
        AudioEngine audio;
        PresetManager presets;
        DisplayRenderer display;
        ScreenManager mgr;
        PresetScreen* screen = nullptr;
        juce::int64 now = 1000;

        Nav()
        {
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addEngine(1, AudioEngine::createEngine(EngineType::FM));
            seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            seq.getPattern().getTrack(1).engineType = EngineType::FM;
            for (int t = 0; t < 2; ++t)
            {
                audio.setTrackParameter(t, MixParam::VOLUME, 0.3f + 0.1f * (float) t);
                seq.getPattern().getTrack(t).soundParams[MixParam::VOLUME] = 0.3f + 0.1f * (float) t;
            }
            audio.setTrackParameter(0, UnifiedParam::CUTOFF, 1234.0f);
            seq.getPattern().getTrack(0).soundParams[UnifiedParam::CUTOFF] = 1234.0f;

            mgr.setSequencer(&seq);
            mgr.setClock([this] { return now; });
            mgr.setDisplay(&display);
            mgr.setTrackCount(2);
            // As in the app: the picture is drawn straight after a track is selected.
            mgr.setTrackSelectionCallback([this](int t)
            {
                seq.setSelectedTrack(t);
                mgr.requestRedraw();
                mgr.redrawIfNeeded();
            });
            mgr.addScreen(std::make_unique<cgtest::Rec>("TRACKS"));
            auto ps = std::make_unique<PresetScreen>(seq, &audio, presets);
            screen = ps.get();
            screen->setClock([this] { return now; });
            mgr.addScreen(std::move(ps));
            mgr.addScreen(std::make_unique<cgtest::Rec>("SETTINGS"));
            mgr.addScreen(std::make_unique<cgtest::Rec>("ARP"));
            mgr.setShiftPadScreen(0, 0);
            mgr.setShiftPadScreen(4, 1);
            openPreset();
        }

        Sound sound(int t) { return soundOf(seq, audio, t); }
        void press(int cc)   { mgr.controlChangeReceived(1, cc, 127); }
        void release(int cc) { mgr.controlChangeReceived(1, cc, 0); }
        void shift(bool on)  { mgr.controlChangeReceived(7, 63, on ? 127 : 0); }
        void pad(int pad)
        {
            mgr.noteOnReceived(1, cgtest::Rig::padNote(pad), 100);
            mgr.noteOffReceived(1, cgtest::Rig::padNote(pad));
        }
        void shiftPad(int p) { shift(true); pad(p); shift(false); }
        void openPreset() { shiftPad(4); }
        void turn(int encoder, int clicks = 1) { mgr.controlChangeReceived(16, 0x15 + encoder, clicks < 0 ? 128 + clicks : clicks); }
        // Scroll until the track sounds different from `from`.
        bool audition(int t, const Sound& from)
        {
            for (int i = 0; i < screen->getListSize(); ++i)
            {
                turn(0);
                if (difference(sound(t), from).isNotEmpty()) return true;
            }
            return false;
        }
    };

    // ---- What a sound is to the ear: level over time and energy by frequency -------------------
    inline std::vector<float> renderFeatures(EngineBase& e, int note = 57)
    {
        const double sr = 48000.0;
        const int block = 64;
        std::vector<float> mono;
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        auto run = [&](double seconds)
        {
            for (int b = 0; b < (int) (seconds * sr) / block; ++b)
            {
                buf.clear();
                e.processBlock(buf, midi);
                for (int i = 0; i < block; ++i)
                    mono.push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
            }
        };
        e.noteOn(note, 0.8f);
        run(0.6);
        e.noteOff(note);
        run(0.3);
        e.allNotesOff();
        run(0.3);
        mono.resize((size_t) (0.9 * sr));       // (the last stretch only empties the engine)

        std::vector<float> f;
        auto db = [](double power) { return (float) (10.0 * std::log10(power + 1.0e-7)); };
        const size_t win = mono.size() / 12;
        for (size_t w = 0; w < 12; ++w)
        {
            double sum = 0.0;
            for (size_t i = w * win; i < (w + 1) * win; ++i) sum += (double) mono[i] * mono[i];
            f.push_back(db(sum / (double) win));
        }
        // Energy in ten octave-wide bands (one-pole band splits are enough for a comparison).
        double lowPrev = 0.0;
        double total[10] = {};
        double lp[10] = {};
        for (float x : mono)
        {
            double prev = x;
            for (int b = 9; b >= 0; --b)
            {
                const double fc = 31.25 * std::pow(2.0, b);
                const double a = 1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * fc / sr);
                lp[b] += a * (x - lp[b]);
                const double band = prev - lp[b];
                total[b] += band * band;
                prev = lp[b];
            }
            juce::ignoreUnused(lowPrev);
        }
        for (double t : total) f.push_back(db(t / (double) mono.size()));
        return f;
    }

    inline float featureDistance(const std::vector<float>& a, const std::vector<float>& b)
    {
        float worst = 0.0f;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (a[i] > -55.0f || b[i] > -55.0f)
                worst = std::max(worst, std::abs(a[i] - b[i]));
        return worst;
    }

    inline std::unique_ptr<AudioEngine> freshAudio(SequencerCore& seq, const Preset& p)
    {
        auto audio = std::make_unique<AudioEngine>();
        audio->setSequencer(&seq);
        audio->addEngine(0, AudioEngine::createEngine(p.engineType));
        audio->getEngine(0)->prepareToPlay(48000.0, 64);
        audio->applyAllTrackParameters(0, PresetManager::soundOf(p));
        return audio;
    }
}

class PresetAuditionTests : public juce::UnitTest
{
public:
    PresetAuditionTests() : UnitTest("Preset Audition") {}

    void runTest() override
    {
        using namespace presetaudition;

        beginTest("Entering changes nothing; the highlight moving makes the preset the track's sound, AU untouched");
        {
            Rig rig;
            const auto own = rig.sound();
            rig.screen->onActivate();
            expect(rig.screen->isAuditionOn(), "audition is on from the start");
            expectEquals(rig.screen->getEncoderReadout(3), juce::String("AUDITION ON"));
            expect(difference(rig.sound(), own).isEmpty(), "entering the screen leaves the sound alone");
            expect(!rig.screen->isAuditioning());
            expect(!rig.screen->getSubPage().contains("AUD"));

            // Knob 1.
            rig.turn(0);
            expectEquals(rig.screen->getSelectedIndex(), 1);
            expect(rig.screen->isAuditioning());
            expect(rig.screen->getSubPage().contains("AUD"), "the name bar says the sound is only being listened to");
            auto* p = rig.highlighted();
            expect(p != nullptr);
            if (p == nullptr) return;
            expect(engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*p)).isEmpty(),
                   "the track plays the highlighted preset: " + engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*p)));
            for (auto& [id, v] : p->soundParams)
                expectEquals(rig.seq.getPattern().getTrack(0).soundParams[id], v, "and stores its values");
            // The mixer, the effect block and the pressure routes are the track's own.
            expectEquals(rig.audio.getTrackParameter(0, MixParam::VOLUME), 0.33f);
            expectEquals(rig.audio.getTrackParameter(0, TrackFxParam::DIRT_BASE), own.values[own.values.size() - (size_t) (TrackFxParam::END - TrackFxParam::DIRT_BASE)].second);
            expect(rig.sound().routes == own.routes);

            // Up / Down.
            rig.screen->onPadPageNext();
            expectEquals(rig.screen->getSelectedIndex(), 2);
            auto* next = rig.highlighted();
            expect(next != nullptr && next != p);
            if (next != nullptr)
                expect(engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*next)).isEmpty(), "Down auditions");
            rig.screen->onPadPagePrev();
            expect(engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*p)).isEmpty(), "Up auditions");

            // Back to the first row: that preset is heard too (the highlight moved onto it).
            rig.screen->onPadPagePrev();
            expectEquals(rig.screen->getSelectedIndex(), 0);
            if (auto* first = rig.highlighted())
                expect(engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*first)).isEmpty());
            // Against the end of the list nothing moves and nothing is loaded again.
            const auto* engine = rig.audio.getEngine(0);
            rig.screen->onPadPagePrev();
            expect(rig.audio.getEngine(0) == engine);
        }

        beginTest("A category or engine-filter change that moves the highlight auditions too");
        {
            Rig rig;
            const auto own = rig.sound();
            rig.screen->onActivate();
            auto* before = rig.highlighted();
            // CT: step through the categories; each time the highlight lands on another preset it is heard.
            int heard = 0;
            for (int c = 0; c < PresetManager::getCategories().size(); ++c)
            {
                rig.turn(1);
                auto* p = rig.highlighted();
                if (rig.screen->getListSize() == 0)
                {
                    expect(difference(rig.sound(), own).isEmpty(), "an empty list gives the track's own sound back");
                    expect(!rig.screen->isAuditioning());
                    before = nullptr;
                    continue;
                }
                if (p != nullptr && p != before)
                {
                    expect(engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*p)).isEmpty(),
                           "category " + juce::String(c + 1) + ": " + p->name);
                    ++heard;
                }
                before = p;
            }
            expect(heard >= 2);

            // EN: with ANY the list holds other engines' presets; a category whose first preset is
            // another engine's brings that engine in.
            Rig any;
            const auto anyOwn = any.sound();
            any.screen->onActivate();
            any.turn(2);
            expect(any.screen->getSubPage().contains("ANY"));
            bool otherEngine = false;
            for (int i = 0; i < any.screen->getListSize() && !otherEngine; ++i)
            {
                any.turn(0);
                otherEngine = any.audio.getEngineTypeOf(0) != EngineType::UnifiedVoice;
            }
            expect(otherEngine, "a preset of another engine type is heard on its own engine");
            auto* p = any.highlighted();
            if (p != nullptr)
            {
                expect(any.audio.getEngineTypeOf(0) == p->engineType);
                expect(any.seq.getPattern().getTrack(0).engineType == p->engineType);
                expect(engineDifference(engineValues(*any.audio.getEngine(0)), freshValues(*p)).isEmpty());
            }
            // Back to "this track's engine": that is still the engine of the track's own sound.
            any.turn(2, -1);
            for (int i = 0; i < any.screen->getListSize(); ++i)
            {
                any.screen->onPadPageNext();
                expect(any.audio.getEngineTypeOf(0) == EngineType::UnifiedVoice, "THIS lists the own sound's engine");
            }
            any.screen->onDeactivate();
            expect(difference(any.sound(), anyOwn).isEmpty(), difference(any.sound(), anyOwn));
        }

        beginTest("Every engine: an auditioned preset holds the values of a fresh load; the own engine returns");
        {
            PresetManager all;
            std::set<EngineType> types;
            for (int i = 0; i < all.getPresetCount(); ++i)
                types.insert(all.getPreset(i).engineType);
            expect(types.size() >= 5);

            for (auto type : types)
            {
                Rig rig(type);
                const juce::String tag = "engine " + juce::String((int) type) + ": ";
                const auto own = rig.sound();
                const auto* engine = rig.audio.getEngine(0);
                rig.screen->onActivate();
                const int count = rig.screen->getListSize();
                expect(count >= 1, tag + "has presets");

                // Down the whole list and back up: whatever was heard before, each preset sounds
                // as if loaded into a new engine.
                for (int i = 0; i < 2 * count; ++i)
                {
                    if (i < count) rig.screen->onPadPageNext(); else rig.screen->onPadPagePrev();
                    auto* p = rig.highlighted();
                    if (p == nullptr || !rig.screen->isAuditioning()) continue;
                    const auto d = engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*p));
                    expect(d.isEmpty(), tag + p->name + " " + d);
                }
                expect(rig.audio.getEngine(0) != engine, tag + "a preset is heard in an engine of its own");

                rig.screen->onDeactivate();
                expect(rig.audio.getEngine(0) == engine, tag + "the track's own engine itself is back on leaving");
                const auto d = difference(rig.sound(), own);
                expect(d.isEmpty(), tag + "the own sound is back exactly: " + d);
            }
        }

        beginTest("Every engine: after presets of every other engine, leaving restores the sound exactly");
        {
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                               EngineType::WaveSeq, EngineType::Sampler, EngineType::Drum })
            {
                Rig rig(type);
                const juce::String tag = "engine " + juce::String((int) type) + ": ";
                const auto own = rig.sound();
                rig.screen->onActivate();
                rig.turn(2);                                    // ANY
                std::set<EngineType> heard;
                for (int i = 0; i < rig.screen->getListSize(); ++i)
                {
                    rig.screen->onPadPageNext();
                    heard.insert(rig.audio.getEngineTypeOf(0));
                }
                expect(heard.size() >= 5, tag + "other engines were heard");
                expect(rig.audio.getEngineTypeOf(0) != type || rig.screen->isAuditioning());
                rig.screen->onDeactivate();
                expect(!rig.screen->isAuditioning());
                const auto d = difference(rig.sound(), own);
                expect(d.isEmpty(), tag + d);
            }
        }

        beginTest("LD keeps the highlighted preset: LOAD OK, nothing is put back on leaving");
        {
            Rig rig;
            const auto own = rig.sound();
            rig.screen->onActivate();
            rig.turn(0, 2);
            auto* p = rig.highlighted();
            const auto heard = rig.sound();
            const auto* engine = rig.audio.getEngine(0);

            for (int i = 0; i < 3; ++i) { rig.turn(4); rig.now += 150; }
            expect(rig.screen->isAuditioning(), "three clicks are not a load");
            rig.turn(4);
            expectEquals(rig.screen->getSubPage(), juce::String("LOAD OK"));
            expect(!rig.screen->isAuditioning());
            expect(rig.audio.getEngine(0) == engine, "the preset being heard simply stays");
            expect(difference(rig.sound(), heard).isEmpty(), "LD changes nothing audible");
            expect(rig.highlighted() == p, "the kept preset stays highlighted");

            rig.screen->onDeactivate();
            expect(difference(rig.sound(), heard).isEmpty(), "LD keeps: " + difference(rig.sound(), heard));
            expect(difference(rig.sound(), own).isNotEmpty());

            // The kept preset is now the sound that auditioning returns to.
            rig.screen->onActivate();
            rig.screen->onPadPageNext();
            expect(difference(rig.sound(), heard).isNotEmpty());
            rig.screen->onDeactivate();
            expect(difference(rig.sound(), heard).isEmpty(), difference(rig.sound(), heard));

            // LD without scrolling loads the highlighted preset.
            Rig direct;
            direct.screen->onActivate();
            auto* first = direct.highlighted();
            direct.confirm(4);
            expectEquals(direct.screen->getSubPage(), juce::String("LOAD OK"));
            if (first != nullptr)
                expect(engineDifference(engineValues(*direct.audio.getEngine(0)), freshValues(*first)).isEmpty());
            const auto loaded = direct.sound();
            direct.screen->onDeactivate();
            expect(difference(direct.sound(), loaded).isEmpty());

            // LD on a preset of another engine type: the track is that engine from then on.
            Rig other;
            other.screen->onActivate();
            other.turn(2);
            for (int i = 0; i < other.screen->getListSize() && other.audio.getEngineTypeOf(0) == EngineType::UnifiedVoice; ++i)
                other.turn(0);
            const auto type = other.audio.getEngineTypeOf(0);
            expect(type != EngineType::UnifiedVoice);
            auto* kept = other.highlighted();
            other.confirm(4);
            expect(other.highlighted() == kept);
            const auto otherSound = other.sound();
            other.screen->onDeactivate();
            expect(other.audio.getEngineTypeOf(0) == type && other.seq.getPattern().getTrack(0).engineType == type);
            expect(difference(other.sound(), otherSound).isEmpty());

            // Nothing highlighted: LOAD FAIL, and the sound is untouched.
            Rig empty;
            const auto emptyOwn = empty.sound();
            empty.screen->onActivate();
            for (int c = 0; c < PresetManager::getCategories().size() && empty.screen->getListSize() > 0; ++c)
                empty.turn(1);
            expectEquals(empty.screen->getListSize(), 0);
            empty.confirm(4);
            expectEquals(empty.screen->getSubPage(), juce::String("LOAD FAIL"));
            expect(difference(empty.sound(), emptyOwn).isEmpty());
        }

        beginTest("AU is the off switch: off gives the own sound back and scrolling is silent; on, the highlight is heard at once");
        {
            Rig rig;
            const auto own = rig.sound();
            rig.screen->onActivate();
            rig.turn(0);
            expect(difference(rig.sound(), own).isNotEmpty());
            rig.turn(3, -1);
            expectEquals(rig.screen->getEncoderReadout(3), juce::String("AUDITION OFF"));
            expect(rig.screen->getSubPage().contains("AU-"), "the name bar shows that audition is off");
            expect(difference(rig.sound(), own).isEmpty(), difference(rig.sound(), own));
            rig.turn(0);
            rig.screen->onPadPageNext();
            expect(difference(rig.sound(), own).isEmpty(), "AU off: scrolling loads nothing");
            rig.turn(3);
            expect(rig.screen->isAuditioning());
            if (auto* p = rig.highlighted())
                expect(engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*p)).isEmpty());
            rig.turn(3);                                         // already on: nothing happens
            expect(rig.screen->isAuditioning());
            rig.screen->onDeactivate();
            expect(difference(rig.sound(), own).isEmpty());
        }

        beginTest("VL set while auditioning stays; a volume a user preset brings does not");
        {
            Rig rig;
            rig.screen->onActivate();
            // SV stores the track's sound, volume included, as a user preset.
            rig.confirm(5);
            rig.set(0, MixParam::VOLUME, 0.6f);
            rig.screen->onDeactivate();
            rig.screen->onActivate();
            const auto own = rig.sound();
            for (int i = 0; i < 400; ++i) rig.turn(0);          // to the end: the user preset
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, MixParam::VOLUME), 0.33f, 0.001f, "the user preset's volume is heard");
            rig.screen->onPadPagePrev();
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, MixParam::VOLUME), 0.6f, 0.001f,
                                      "the next preset starts from the track's own mixer again");
            rig.screen->onPadPageNext();
            rig.screen->onDeactivate();
            expect(difference(rig.sound(), own).isEmpty(), difference(rig.sound(), own));

            rig.screen->onActivate();
            rig.turn(0, -1);
            const float before = rig.audio.getTrackParameter(0, MixParam::VOLUME);
            rig.turn(7, -5);
            const float set = rig.audio.getTrackParameter(0, MixParam::VOLUME);
            expect(set < before);
            rig.screen->onDeactivate();
            expectEquals(rig.audio.getTrackParameter(0, MixParam::VOLUME), set, "VL is the track's level, not part of the audition");
            expectEquals(rig.seq.getPattern().getTrack(0).soundParams[MixParam::VOLUME], set);
        }

        beginTest("SV reports the saved name; a typed name renames the user preset without touching the audition");
        {
            Rig rig;
            juce::String saved;
            int written = 0;
            rig.screen->onPresetSaved = [&](const juce::String& n) { saved = n; };
            rig.screen->onUserPresetsChanged = [&] { ++written; };
            rig.screen->onActivate();
            rig.confirm(5);
            expectEquals(saved, juce::String("User 1"));
            expect(rig.mgr.findUserPreset("User 1") >= 0);
            expectEquals(written, 1);

            const auto own = rig.sound();
            const bool auditioning = rig.screen->isAuditioning();
            expect(! rig.screen->renameUserPreset("User 1", "   "), "an empty name is refused");
            expect(! rig.screen->renameUserPreset("User 1", "narptune"), "a factory preset's name is refused, whatever the case");
            expect(! rig.screen->renameUserPreset("No Such", "Mine"), "only an existing user preset is renamed");
            expectEquals(written, 1);
            expect(rig.screen->renameUserPreset("User 1", juce::String::fromUTF8("  My b\xc3\xa4ss sound is very long  ")));
            expectEquals(written, 2, "the renamed preset is written to disk");
            expect(rig.mgr.findUserPreset("User 1") < 0);
            const int idx = rig.mgr.findUserPreset("My bss sound is");
            expect(idx >= 0, "printable ASCII, 15 characters, no outer spaces");
            if (idx >= 0)
                expect(! rig.mgr.getPreset(idx).isFactory);
            expect(rig.screen->isAuditioning() == auditioning);
            expect(difference(rig.sound(), own).isEmpty(), "a rename changes no sound");

            // Highlighted and heard: the highlight stays on it under the new name.
            for (int i = 0; i < 400; ++i) rig.turn(0);
            expect(rig.highlighted() != nullptr && rig.highlighted()->name == "My bss sound is");
            const auto heard = rig.sound();
            expect(rig.screen->renameUserPreset("My bss sound is", "Low One"));
            expect(rig.highlighted() != nullptr && rig.highlighted()->name == "Low One");
            expect(difference(rig.sound(), heard).isEmpty(), "the audition goes on unchanged");
            rig.screen->onDeactivate();
            expect(difference(rig.sound(), own).isEmpty());

            rig.screen->onActivate();
            rig.confirm(5);
            expectEquals(saved, juce::String("User 2"), "the next save takes the next free User name");
            expect(! rig.screen->renameUserPreset("User 2", "LOW ONE"), "a name in use is refused");
            rig.screen->onDeactivate();
        }

        beginTest("Narptune is a factory preset of the unified voice");
        {
            PresetManager mgr;
            int found = -1;
            for (int i = 0; i < mgr.getPresetCount(); ++i)
                if (mgr.getPreset(i).name == "Narptune")
                    found = i;
            expect(found >= 0);
            if (found >= 0)
            {
                const auto& p = mgr.getPreset(found);
                expect(p.isFactory && p.engineType == EngineType::UnifiedVoice && p.category == "BASS");
                auto at = [&](int id) { auto it = p.soundParams.find(id); return it != p.soundParams.end() ? it->second : -999.0f; };
                expectWithinAbsoluteError(at(UnifiedParam::CUTOFF), 32.436f, 0.001f);
                expectWithinAbsoluteError(at(UnifiedParam::RESONANCE), 0.9f, 0.0001f);
                expectEquals(at(UnifiedParam::WAVE1), 1.0f);
                expectEquals(at(UnifiedParam::TUNE3), -12.0f);
                expectEquals(at(UnifiedParam::AMP_RELEASE), 10.0f);
                expectEquals(at(UnifiedParam::DETUNE2), 8.0f);
            }
        }

        beginTest("A step lock that is holding a parameter is neither remembered as the sound nor lost");
        {
            Rig rig;
            rig.set(0, UnifiedParam::CUTOFF, 1234.0f);
            Step locked, plain;
            locked.paramLocks[UnifiedParam::CUTOFF] = 500.0f;
            rig.seq.getPattern().getTrack(0).steps[0].paramLocks[UnifiedParam::CUTOFF] = 500.0f;
            rig.audio.applyStepLocks(0, locked);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF), 500.0f, 0.5f);

            rig.screen->onActivate();
            rig.turn(0);
            auto* p = rig.highlighted();
            rig.audio.applyStepLocks(0, plain);                 // the lock ends while the preset is heard
            if (p != nullptr)
                expect(engineDifference(engineValues(*rig.audio.getEngine(0)), freshValues(*p)).isEmpty(),
                       "the parameter goes back to the preset's value, not to the own sound's");
            rig.audio.applyStepLocks(0, locked);
            rig.screen->onDeactivate();
            rig.audio.applyStepLocks(0, plain);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF), 1234.0f, 0.5f,
                                      "after leaving, the lock ends on the own sound's value");
        }

        beginTest("Every way out restores: Shift + pad, Settings, Arp, another screen set directly");
        {
            for (int route = 0; route < 4; ++route)
            {
                Nav nav;
                const juce::String tag = "route " + juce::String(route) + ": ";
                expect(nav.mgr.getActiveScreen() == nav.screen);
                const auto own = nav.sound(0);
                expect(nav.audition(0, own), tag + "scrolling with knob 1 changes the sound");
                // Up / Down beside the pads scroll as well.
                const int index = nav.screen->getSelectedIndex();
                nav.press(cgtest::UP); nav.release(cgtest::UP);
                expectEquals(nav.screen->getSelectedIndex(), index - 1, tag + "Up scrolls");
                expect(nav.screen->isAuditioning());

                if (route == 0)      nav.shiftPad(0);
                else if (route == 1) { nav.press(cgtest::SETTINGS); nav.release(cgtest::SETTINGS); }
                else if (route == 2) { nav.press(cgtest::ARP); nav.release(cgtest::ARP); }
                else                 nav.mgr.setActiveScreen(0);

                expect(nav.mgr.getActiveScreen() != nav.screen, tag + "the screen was left");
                const auto d = difference(nav.sound(0), own);
                expect(d.isEmpty(), tag + d);
                expect(!nav.screen->isAuditioning());

                // Coming back starts clean: nothing is heard until the highlight moves.
                nav.openPreset();
                expect(difference(nav.sound(0), own).isEmpty(), tag + "re-entering loads nothing");
            }
        }

        beginTest("Another track selected while auditioning: the old track gets its sound back, the new one is remembered");
        {
            Nav nav;
            const auto own0 = nav.sound(0), own1 = nav.sound(1);
            expect(nav.audition(0, own0));

            // Shift + Down: track 2.
            nav.shift(true); nav.press(cgtest::DOWN); nav.release(cgtest::DOWN); nav.shift(false);
            expectEquals(nav.seq.getSelectedTrack(), 1);
            expect(difference(nav.sound(0), own0).isEmpty(), "track 1 is itself again at once: " + difference(nav.sound(0), own0));
            expect(difference(nav.sound(1), own1).isEmpty(), "track 2 is untouched until the highlight moves");
            expect(!nav.screen->isAuditioning());

            expect(nav.audition(1, own1), "now track 2 auditions");
            expect(nav.audio.getEngineTypeOf(1) == EngineType::FM, "from its own engine's presets");
            expect(difference(nav.sound(0), own0).isEmpty());

            // Hold Up + top pad 1: back to track 1.
            nav.press(cgtest::UP); nav.pad(0); nav.release(cgtest::UP);
            expectEquals(nav.seq.getSelectedTrack(), 0);
            expect(difference(nav.sound(1), own1).isEmpty(), "track 2 restored: " + difference(nav.sound(1), own1));

            // Keep a preset on track 1, audition on track 2, leave: track 1 keeps, track 2 returns.
            expect(nav.audition(0, own0));
            for (int i = 0; i < 4; ++i) { nav.turn(4); nav.now += 150; }
            const auto kept = nav.sound(0);
            expect(difference(kept, own0).isNotEmpty());
            nav.shift(true); nav.press(cgtest::DOWN); nav.release(cgtest::DOWN); nav.shift(false);
            expect(nav.audition(1, own1));
            nav.shiftPad(0);
            expect(difference(nav.sound(0), kept).isEmpty(), "LD on track 1 stays");
            expect(difference(nav.sound(1), own1).isEmpty(), "track 2 returns");
        }

        beginTest("A project saved while auditioning holds the track's own sound");
        {
            for (int otherEngine = 0; otherEngine < 2; ++otherEngine)
            {
                Rig rig;
                rig.screen->onActivate();
                const auto before = ProjectBridge::capture({ rig.seq, rig.audio }, "P").toJSON();

                if (otherEngine != 0)
                {
                    rig.turn(2);
                    for (int i = 0; i < rig.screen->getListSize() && rig.audio.getEngineTypeOf(0) == EngineType::UnifiedVoice; ++i)
                        rig.turn(0);
                    expect(rig.audio.getEngineTypeOf(0) != EngineType::UnifiedVoice);
                }
                else
                    rig.turn(0, 2);
                expect(rig.screen->isAuditioning());

                auto data = ProjectBridge::capture({ rig.seq, rig.audio }, "P");
                expect(data.toJSON() != before, "the engine holds the auditioned preset");
                rig.screen->keepOriginalInSave(data);
                expect(data.toJSON() == before, "the saved project is the one from before the audition");

                // Loading that file gives the own sound.
                SequencerCore seq2;
                AudioEngine audio2;
                audio2.setSequencer(&seq2);
                ProjectBridge::apply(ProjectData::fromJSON(data.toJSON()), { seq2, audio2 });
                expect(audio2.getEngineTypeOf(0) == EngineType::UnifiedVoice);
                expectEquals(audio2.getTrackParameter(0, MixParam::VOLUME), 0.33f);

                // After LD the preset is the sound and is what gets saved.
                rig.confirm(4);
                auto kept = ProjectBridge::capture({ rig.seq, rig.audio }, "P");
                const auto plain = kept.toJSON();
                rig.screen->keepOriginalInSave(kept);
                expect(kept.toJSON() == plain && plain != before, "nothing is swapped once LD kept the preset");
            }
        }

        beginTest("A project loaded while auditioning is not overwritten by the remembered sound");
        {
            // The project to load: track 1 with a cutoff of 777 Hz.
            Rig source;
            source.set(0, UnifiedParam::CUTOFF, 777.0f);
            const auto project = ProjectData::fromJSON(ProjectBridge::capture({ source.seq, source.audio }, "B").toJSON());

            Rig rig;
            rig.screen->onActivate();
            rig.turn(0, 2);
            expect(rig.screen->isAuditioning());
            ProjectBridge::apply(project, { rig.seq, rig.audio });
            const auto loaded = rig.sound();
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF), 777.0f, 0.5f);

            auto data = ProjectBridge::capture({ rig.seq, rig.audio }, "B");
            const auto plain = data.toJSON();
            rig.screen->keepOriginalInSave(data);
            expect(data.toJSON() == plain, "a save after the load stores the loaded project");

            rig.screen->onDeactivate();
            expect(difference(rig.sound(), loaded).isEmpty(), "leaving keeps the loaded project's sound: " + difference(rig.sound(), loaded));

            // And the screen carries on from the loaded sound.
            rig.screen->onActivate();
            rig.turn(0);
            rig.screen->onDeactivate();
            expect(difference(rig.sound(), loaded).isEmpty());
        }

        beginTest("Pads play the scale chosen on SCALE: its notes, its root lit; chromatic from middle C without one");
        {
            Rig rig;
            std::vector<int> played;
            rig.screen->setNoteCallback([&](int n, int) { played.push_back(n); });
            rig.screen->setNoteOffCallback([](int) {});
            rig.screen->onActivate();
            rig.screen->onPadPress(0, 100); rig.screen->onPadRelease(0);
            rig.screen->onPadPress(5, 100); rig.screen->onPadRelease(5);
            expect(played == (std::vector<int> { 60, 65 }), "no scale given: semitones from middle C");

            CustomScaleManager scales;
            rig.screen->setPadScale([&](int pad) { return scales.getNoteForPad(pad); }, [&] { return scales.getRootNote(); });
            played.clear();
            for (int pad : { 0, 1, 2, 7 }) { rig.screen->onPadPress(pad, 100); rig.screen->onPadRelease(pad); }
            expect(played == (std::vector<int> { 60, 61, 62, 67 }), "the first page is chromatic");

            scales.setActivePage(1);                    // Major
            scales.setRootNote(1, 62);                  // in D
            played.clear();
            for (int pad : { 0, 1, 2, 7 }) { rig.screen->onPadPress(pad, 100); rig.screen->onPadRelease(pad); }
            expect(played == (std::vector<int> { 62, 64, 66, 74 }), "D major: D E F# ... D");

            std::array<Screen::PadColour, 16> colours;
            expect(rig.screen->getPadColours(colours));
            auto lit = [&](int pad) { return colours[(size_t) pad].r == 100; };
            expect(lit(0) && lit(7) && lit(14) && !lit(1) && !lit(6), "the Ds are the lit pads");
            rig.screen->onDeactivate();
        }

        beginTest("SV: a sound shaped on the knobs comes back from its preset exactly, on every engine");
        {
            PresetManager all;
            std::set<EngineType> types;
            for (int i = 0; i < all.getPresetCount(); ++i)
                types.insert(all.getPreset(i).engineType);

            for (auto type : types)
            {
                const juce::String tag = "engine " + juce::String((int) type) + ": ";
                // A track as the app starts it (nothing stored), shaped only by turning knobs on
                // PARAM: every cell of every page, a few clicks, as a player would.
                SequencerCore seq;
                AudioEngine audio;
                PresetManager mgr;
                audio.setSequencer(&seq);
                audio.prepare(48000.0, 64);
                audio.addEngine(0, AudioEngine::createEngine(type));
                seq.getPattern().getTrack(0).engineType = type;
                ParamPageScreen param(seq, &audio);
                PresetScreen preset(seq, &audio, mgr);
                juce::int64 now = 1000;
                preset.setClock([&] { return now; });

                int turned = 0;
                for (int round = 0; round < 2; ++round)
                    for (int pg = 0; pg < param.getPageCount(); ++pg)
                    {
                        param.setCurrentPageIndex(pg);
                        for (int knob = 0; knob < 8; ++knob)
                        {
                            const auto cell = param.getCurrentPage().params[(size_t) knob];
                            if (!cell.isValid() || juce::String(cell.label) == "ARCH") continue;
                            if (audio.getEngine(0)->isRebuildParam(cell.id)) continue;
                            const int clicks = 1 + ((pg + knob + round) % 3);
                            for (int c = 0; c < clicks; ++c)
                                param.onEncoderChange(knob, ((pg + knob) % 2) ? 1 : 127);
                            ++turned;
                        }
                    }
                expect(turned > 20, tag + "knobs were turned");

                const auto live = engineValues(*audio.getEngine(0));
                const auto heardBefore = renderFeatures(*audio.getEngine(0));

                // SV (four clicks), then the saved preset loaded into a new engine.
                preset.onActivate();
                const int before = mgr.getPresetCount();
                for (int i = 0; i < 4; ++i) { preset.onEncoderChange(5, 1); now += 150; }
                expectEquals(mgr.getPresetCount(), before + 1, tag + "saved");
                const auto saved = mgr.getPreset(mgr.getPresetCount() - 1);
                expect(engineDifference(engineValues(*audio.getEngine(0)), live).isEmpty(), tag + "saving changes nothing");

                const auto reloaded = freshValues(saved);
                juce::String diffs;
                int count = 0;
                for (size_t i = 0; i < live.size() && i < reloaded.size(); ++i)
                    if (live[i].second != reloaded[i].second)
                    {
                        if (++count <= 12)
                            diffs << " " << audio.getEngine(0)->getParameterName(live[i].first) << "(" << live[i].first << ") "
                                  << juce::String(live[i].second, 4) << " -> " << juce::String(reloaded[i].second, 4) << ";";
                    }
                expect(count == 0, tag + juce::String(count) + " values differ after reload:" + diffs);

                SequencerCore s2;
                auto fresh = freshAudio(s2, saved);
                auto calm = [](EngineBase* e)
                {
                    if (auto* u = dynamic_cast<UnifiedVoiceEngine*>(e))
                        for (int v = 0; v < UnifiedVoiceEngine::MAX_VOICES; ++v)
                            u->getVoice(v).getAnalogCharacter().enabled = false;
                };
                calm(fresh->getEngine(0));
                calm(audio.getEngine(0));
                const auto target = renderFeatures(*fresh->getEngine(0));
                const float d = featureDistance(renderFeatures(*audio.getEngine(0)), target);
                logMessage(tag + "reloaded sound differs by " + juce::String(d, 1) + " dB, " + juce::String(count) + " values");
                preset.onDeactivate();
            }
        }

        beginTest("SV while a preset is auditioned saves the track's own sound, not the one being listened to");
        {
            Rig rig;
            const auto own = engineValues(*rig.audio.getEngine(0));
            rig.screen->onActivate();
            rig.turn(0);                                        // the highlight moves: a preset is heard
            expect(rig.screen->isAuditioning());
            expect(engineDifference(engineValues(*rig.audio.getEngine(0)), own).isNotEmpty());
            const int before = rig.mgr.getPresetCount();
            rig.confirm(5);                                     // SV
            expectEquals(rig.mgr.getPresetCount(), before + 1);
            const auto saved = rig.mgr.getPreset(rig.mgr.getPresetCount() - 1);
            const auto d = engineDifference(freshValues(saved), own);
            expect(d.isEmpty(), "the saved preset is the own sound: " + d);
            expect(rig.screen->isAuditioning(), "and the audition goes on");
            rig.screen->onDeactivate();
            expect(engineDifference(engineValues(*rig.audio.getEngine(0)), own).isEmpty());
        }

        beginTest("A saved preset carries the track's effects: the insert chain, its settings, the effect blocks, the sends");
        {
            Rig rig;
            rig.audio.addInsertEffect(0, EffectType::DelayEffect);
            rig.audio.addInsertEffect(0, EffectType::WahEffect);
            rig.set(0, AudioEngine::EFFECT_PARAM_BASE + 0, rig.audio.getTrackParameter(0, AudioEngine::EFFECT_PARAM_BASE + 0) * 0.5f + 0.01f);
            rig.set(0, AudioEngine::EFFECT_PARAM_BASE + 4 + 1, 0.77f);
            rig.set(0, AudioEngine::INSERT_WET_BASE + 1, 0.4f);
            rig.set(0, AudioEngine::INSERT_SYNC_BASE + 0, 1.0f);
            rig.set(0, AudioEngine::INSERT_DIV_BASE + 0, 5.0f);
            auto effects = [&](int t)
            {
                juce::String out;
                for (int slot = 0; slot < AudioEngine::MAX_INSERTS_PER_TRACK; ++slot)
                {
                    EffectType type {};
                    if (!rig.audio.getInsertEffectType(t, slot, type)) break;
                    out << (int) type << ":";
                    for (int n = 0; n < 4; ++n)
                        out << juce::String(rig.audio.getTrackParameter(t, AudioEngine::EFFECT_PARAM_BASE + slot * 4 + n), 3) << ",";
                    for (int base : { AudioEngine::INSERT_ON_BASE, AudioEngine::INSERT_WET_BASE, AudioEngine::INSERT_SYNC_BASE, AudioEngine::INSERT_DIV_BASE })
                        out << juce::String(rig.audio.getTrackParameter(t, base + slot), 2) << ",";
                    out << " ";
                }
                for (int id = TrackFxParam::DIRT_BASE; id < TrackFxParam::END; ++id)
                    out << juce::String(rig.audio.getTrackParameter(t, id), 2) << ",";
                out << " send " << juce::String(rig.audio.getTrackParameter(t, MixParam::SEND_BASE), 2);
                return out;
            };
            const auto mine = effects(0);
            expect(mine.startsWith(juce::String((int) EffectType::DelayEffect) + ":"));

            rig.screen->onActivate();
            rig.confirm(5);                                     // SV
            const juce::String savedName = rig.mgr.getPreset(rig.mgr.getPresetCount() - 1).name;
            rig.screen->onDeactivate();

            // The track goes on to other effects; the preset brings its own back when it is heard...
            rig.audio.removeInsertEffect(0, 0);
            rig.set(0, TrackFxParam::DIRT_BASE, 0.0f);
            rig.set(0, MixParam::SEND_BASE, 0.0f);
            const auto changed = effects(0);
            expect(changed != mine);
            rig.screen->onActivate();
            for (int i = 0; i < 400 && (rig.highlighted() == nullptr || rig.highlighted()->name != savedName); ++i)
                rig.turn(0);
            expect(rig.highlighted() != nullptr && rig.highlighted()->name == savedName);
            expectEquals(effects(0), mine, "auditioned: the preset's effects");

            // ...a factory preset has none of its own and plays through the track's...
            rig.turn(0, -1);
            expect(rig.highlighted() != nullptr && rig.highlighted()->isFactory);
            expectEquals(effects(0), changed, "a preset without effects: the track's own");
            rig.turn(0, 1);
            expectEquals(effects(0), mine);

            // ...leaving puts the track's own effects back; LD keeps the preset's.
            rig.screen->onDeactivate();
            expectEquals(effects(0), changed, "left: the track's own effects again");
            rig.screen->onActivate();
            rig.turn(0, -1); rig.turn(0, 1);
            rig.confirm(4);                                     // LD
            rig.screen->onDeactivate();
            expectEquals(effects(0), mine, "loaded: the preset's effects stay");

            // A preset saved on a track without inserts says so: loading it removes them.
            Rig bare;
            bare.screen->onActivate();
            bare.confirm(5);
            const juce::String bareName = bare.mgr.getPreset(bare.mgr.getPresetCount() - 1).name;
            bare.screen->onDeactivate();
            bare.audio.addInsertEffect(0, EffectType::WahEffect);
            bare.screen->onActivate();
            for (int i = 0; i < 400 && (bare.highlighted() == nullptr || bare.highlighted()->name != bareName); ++i)
                bare.turn(0);
            EffectType type {};
            expect(!bare.audio.getInsertEffectType(0, 0, type), "no insert effects in the preset: none on the track");
            bare.screen->onDeactivate();
            expect(bare.audio.getInsertEffectType(0, 0, type), "and the track's own comes back on leaving");
        }

        beginTest("A saved preset carries the track's arpeggiator; a preset without one leaves the track's alone");
        {
            Rig rig;
            Arpeggiator arp;
            rig.screen->setArpProvider([&](int t) { return t == 0 ? &arp : nullptr; });
            arp.setMode(Arpeggiator::Mode::Pedal);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setOctaveRange(3);
            arp.setGate(0.31f);
            arp.setEnabled(true);
            const auto mine = ProjectBridge::captureArp(arp);

            rig.screen->onActivate();
            rig.confirm(5);                                     // SV
            const auto saved = rig.mgr.getPreset(rig.mgr.getPresetCount() - 1);
            expect(saved.arp == mine, "the arp is in the preset");
            expect(Preset::fromJSON(saved.toJSON()).arp == mine, "and in its file");
            rig.screen->onDeactivate();

            // The track's arp moves on; the preset brings its own when heard.
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setOctaveRange(1);
            arp.setEnabled(false);
            const auto changed = ProjectBridge::captureArp(arp);
            rig.screen->onActivate();
            for (int i = 0; i < 400 && (rig.highlighted() == nullptr || rig.highlighted()->name != saved.name); ++i)
                rig.turn(0);
            expect(ProjectBridge::captureArp(arp) == mine, "auditioned: the preset's arp, switched on as it was saved");
            rig.turn(0, -1);                                    // a factory preset: no arp of its own
            expect(ProjectBridge::captureArp(arp) == changed, "a preset without an arp: the track's own");
            rig.turn(0, 1);
            rig.screen->onDeactivate();
            expect(ProjectBridge::captureArp(arp) == changed, "left: the track's own arp again");

            rig.screen->onActivate();
            rig.turn(0, -1); rig.turn(0, 1);
            rig.confirm(4);                                     // LD
            rig.screen->onDeactivate();
            expect(ProjectBridge::captureArp(arp) == mine, "loaded: the preset's arp stays");

            // SV while another preset is auditioned saves the track's own arp.
            arp.setMode(Arpeggiator::Mode::Down);
            const auto own2 = ProjectBridge::captureArp(arp);
            rig.screen->onActivate();
            rig.turn(0, -1);
            expect(rig.screen->isAuditioning());
            rig.confirm(5);
            expect(rig.mgr.getPreset(rig.mgr.getPresetCount() - 1).arp == own2);
            rig.screen->onDeactivate();
        }

        beginTest("Notes held across an audition do not hang");
        {
            Rig rig;
            rig.audio.getEngine(0)->noteOn(60, 0.8f);
            rig.screen->onActivate();
            const auto* engine = rig.audio.getEngine(0);
            rig.turn(0);
            expect(rig.audio.getEngine(0) != engine, "the preset has an engine of its own; the held note ended with the old one");
            rig.audio.getEngine(0)->noteOff(60);            // (the key's release is harmless there)

            // Another engine type: the old engine and its notes are gone with it; the release of
            // the key goes to the new one and is harmless.
            rig.audio.getEngine(0)->noteOn(64, 0.8f);
            rig.turn(2);
            for (int i = 0; i < rig.screen->getListSize() && rig.audio.getEngineTypeOf(0) == EngineType::UnifiedVoice; ++i)
                rig.turn(0);
            expect(rig.audio.getEngine(0) != engine);
            rig.audio.getEngine(0)->noteOff(64);
            rig.audio.getEngine(0)->allNotesOff();

            // Pads held on the screen are released on leaving.
            std::vector<int> off;
            rig.screen->setNoteOffCallback([&](int n) { off.push_back(n); });
            rig.screen->onPadPress(3, 100);
            rig.screen->onDeactivate();
            expectEquals((int) off.size(), 1);
        }
        beginTest("Every engine: an auditioned preset sounds like the preset loaded into a new engine");
        {
            PresetManager all;
            std::set<EngineType> types;
            for (int i = 0; i < all.getPresetCount(); ++i)
                types.insert(all.getPreset(i).engineType);
            int checked = 0;
            for (auto type : types)
            {
                Rig rig(type);
                rig.audio.prepare(48000.0, 64);
                rig.screen->onActivate();
                const int count = rig.screen->getListSize();
                for (int i = 0; i < 2 * count; ++i)
                {
                    if (i < count) rig.screen->onPadPageNext(); else rig.screen->onPadPagePrev();
                    auto* p = rig.highlighted();
                    if (p == nullptr || !rig.screen->isAuditioning()) continue;
                    // Sounds with random phases or noise differ from note to note: several notes of
                    // each, and the auditioned ones must lie as close to the fresh ones as those
                    // lie to each other.
                    SequencerCore s1;
                    auto a = freshAudio(s1, *p);
                    std::vector<std::vector<float>> fresh, heardNotes;
                    for (int n = 0; n < 4; ++n)
                    {
                        fresh.push_back(renderFeatures(*a->getEngine(0)));
                        heardNotes.push_back(renderFeatures(*rig.audio.getEngine(0)));
                    }
                    float natural = 0.0f, d = 1000.0f;
                    for (size_t x = 0; x < fresh.size(); ++x)
                        for (size_t y = 0; y < fresh.size(); ++y)
                        {
                            if (x < y) natural = std::max(natural, featureDistance(fresh[x], fresh[y]));
                            d = std::min(d, featureDistance(heardNotes[x], fresh[y]));
                        }
                    ++checked;
                    expect(d <= std::max(3.0f, natural),
                           "engine " + juce::String((int) type) + " " + p->name + ": auditioned differs by "
                           + juce::String(d, 1) + " dB (notes of a fresh load differ by up to " + juce::String(natural, 1) + ")");
                }
                rig.screen->onDeactivate();
            }
            expect(checked > 50);
        }
    }
};

static PresetAuditionTests presetAuditionTests;
