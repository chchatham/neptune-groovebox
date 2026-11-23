#pragma once

// The pressure slots (PressureSlotParam, ids 773-784; ModRouter.h: THE PRESSURE SLOTS): where pad
// pressure goes on a track, as cells of the track's sound.
//   - an engine is made with its route (fresh, on a track, rebuilt, set aside for an audition: alike);
//   - every destination a route had before the slots had cells is one entry of a slot's list,
//     where the engine's own map of then puts it, near the top of AT ROUTE's menu, and is heard;
//   - the slots travel with the sound: capture, a preset, a project (by name), a clip / undo;
//     an untouched track is still empty, a track whose route was changed is not;
//   - pressure on a cell of an effect on the track.
class PressureSlotsTests : public juce::UnitTest
{
public:
    PressureSlotsTests() : juce::UnitTest("Pressure Slots", "Neptune") {}

    using D = AftertouchRouter::Destination;
    static int destId(int slot)    { return PressureSlotParam::id(slot, PressureSlotParam::DEST_OFFSET); }
    static int amountId(int slot)  { return PressureSlotParam::id(slot, PressureSlotParam::AMT_OFFSET); }
    static int bipolarId(int slot) { return PressureSlotParam::id(slot, PressureSlotParam::BIPOLAR_OFFSET); }

    static std::vector<float> slotsOf(const EngineBase& e)
    {
        std::vector<float> v;
        for (int id = PressureSlotParam::BASE; id < PressureSlotParam::END; ++id)
            v.push_back(e.getParameter(id));
        return v;
    }

    static const std::vector<EngineType>& routed()
    {
        static const std::vector<EngineType> types = { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc, EngineType::Wavetable,
                                                       EngineType::WaveSeq, EngineType::Sampler, EngineType::Drum, EngineType::Resonator };
        return types;
    }

    struct Rig
    {
        SequencerCore seq;
        AudioEngine audio;
        explicit Rig(EngineType type, int tracks = 1)
        {
            audio.setSequencer(&seq);
            for (int t = 0; t < tracks; ++t)
            {
                audio.addEngine(t, AudioEngine::createEngine(type));
                for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
                    seq.getPattern(p).getTrack(t).engineType = type;
            }
            audio.prepare(48000.0, 256);
        }
        EngineBase& engine(int t = 0) { return *audio.getEngine(t); }
        const mod::DestTable& table(int t = 0) { return engine(t).modRouter()->dests(); }
        void set(int id, float value, int t = 0) { audio.setTrackParameter(t, id, value); }
        float get(int id, int t = 0) { return audio.getTrackParameter(t, id); }

        // A held note, the pad pressed after `after` blocks (0.15 s; a drum's hit: 21 ms): the
        // sound from 0.1 s later for 0.25 s (left + right).
        std::vector<float> pressed(float pressure, int note = 57, int after = 28)
        {
            std::vector<float> l(256), r(256), out;
            engine().allNotesOff();
            for (int b = 0; b < 40; ++b) audio.renderBlock(l.data(), r.data(), 256);
            engine().noteOn(note, 0.7f);
            for (int b = 0; b < after; ++b) audio.renderBlock(l.data(), r.data(), 256);
            engine().aftertouch(note, pressure);
            for (int b = 0; b < 19; ++b) audio.renderBlock(l.data(), r.data(), 256);
            for (int b = 0; b < 47; ++b)
            {
                audio.renderBlock(l.data(), r.data(), 256);
                out.insert(out.end(), l.begin(), l.end());
                out.insert(out.end(), r.begin(), r.end());
            }
            engine().aftertouch(note, 0.0f);
            engine().noteOff(note);
            return out;
        }
    };

    static double apart(const std::vector<float>& a, const std::vector<float>& b)
    {
        double diff = 0.0, sum = 1.0e-12;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            diff += (double) (a[i] - b[i]) * (a[i] - b[i]);
            sum += (double) a[i] * a[i] + (double) b[i] * b[i];
        }
        return std::sqrt(diff / sum);
    }

    void runTest() override
    {
        beginTest("An engine is made with its pressure route: fresh, on a track, rebuilt and set aside for an audition, it holds the same");
        {
            const std::map<EngineType, const char*> first = {
                { EngineType::UnifiedVoice, "AMP" }, { EngineType::FM, "AMP" }, { EngineType::MacroOsc, "AMP" }, { EngineType::Wavetable, "AMP" },
                { EngineType::WaveSeq, "AMP" }, { EngineType::Sampler, "OFF" }, { EngineType::Drum, "DEC" }, { EngineType::Resonator, "EXC" } };
            for (auto type : routed())
            {
                const juce::String name((int) type);
                auto bare = AudioEngine::createEngine(type);
                const auto& t = bare->modRouter()->dests();
                const auto made = slotsOf(*bare);
                expectEquals(juce::String(t.pressureLabels()[(int) made[0]]), juce::String(first.at(type)), "engine " + name);
                expectEquals(made[1], type == EngineType::Sampler ? 0.5f : 1.0f, "engine " + name + ": the first route's depth");
                for (int p = 1; p < 4; ++p)
                {
                    expectEquals(made[(size_t) (p * 3)], 0.0f, "engine " + name + ": the other routes are off");
                    expectEquals(made[(size_t) (p * 3 + 1)], 0.5f);
                    expectEquals(made[(size_t) (p * 3 + 2)], 0.0f);
                }
                // What AudioEngine::defaultPressureRouter says a new track does, in the old words.
                Rig rig(type);
                expect(slotsOf(rig.engine()) == made, "engine " + name + ": on a track");
                const auto said = AudioEngine::defaultPressureRouter(type), read = rig.audio.getPressureRouter(0);
                expect(read.sameRoutesAs(said), "engine " + name + ": the default as routes");
                // Changed, then rebuilt (a sound load starts from the engine as it is made).
                rig.set(destId(0), (float) t.indexOfLabel("PAN"));
                rig.set(amountId(2), -0.3f);
                rig.audio.setEngineType(0, type, true);
                expect(slotsOf(rig.engine()) == made, "engine " + name + ": rebuilt");
                // Set aside for an audition: the stand-in is as it is made, the track's own keeps its routes.
                rig.set(destId(1), (float) t.indexOfLabel("PAN"));
                const auto own = slotsOf(rig.engine());
                auto parked = rig.audio.parkEngine(0, type);
                expect(slotsOf(rig.engine()) == made, "engine " + name + ": the audition's engine");
                expect(slotsOf(*parked) == own, "engine " + name + ": the parked engine keeps its own");
                rig.audio.unparkEngine(0, parked);
                expect(slotsOf(rig.engine()) == own);
                rig.audio.resetPressureRouter(0);
                expect(slotsOf(rig.engine()) == made, "engine " + name + ": reset");
                // (Func + turn on AT ROUTE's DEST goes by group: its menu is among the grouped lists.)
                expect(ChoiceGroups::find(t.pressureMenu()) != nullptr, "engine " + name + ": the menu's groups");
            }
            // (and the grouped lists still have room: a list that finds none is silently ungrouped)
            expect(ChoiceGroups::used().load() < ChoiceGroups::MAX_LISTS,
                   "grouped lists in use: " + juce::String(ChoiceGroups::used().load()) + " of " + juce::String((int) ChoiceGroups::MAX_LISTS));
            logMessage("grouped choice lists in use: " + juce::String(ChoiceGroups::used().load()) + " of " + juce::String((int) ChoiceGroups::MAX_LISTS));
        }

        beginTest("Every destination a route had before the slots is an entry of a slot's list, where the engine's map of then puts it, and is heard");
        {
            // AudioEngine::pressureDestinations: what AT ROUTE offered each engine at 4aadbb2, with
            // the engine's own label for it; and what the engine's PressureMap made of it.
            struct Was { EngineType type; D dest; const char* entry; };
            const Was was[] = {
                { EngineType::UnifiedVoice, D::Volume, "AMP" }, { EngineType::UnifiedVoice, D::FilterCutoff, "CUT" }, { EngineType::UnifiedVoice, D::Vibrato, "VIB" },
                { EngineType::UnifiedVoice, D::MacroTimbre, "PW" }, { EngineType::UnifiedVoice, D::WavePosition, "MIX" }, { EngineType::UnifiedVoice, D::FMDepth, "XMOD" },
                { EngineType::UnifiedVoice, D::PanSpread, "PAN" },
                { EngineType::FM, D::Volume, "AMP" }, { EngineType::FM, D::FilterCutoff, "CUT" }, { EngineType::FM, D::FMDepth, "INDX" },
                { EngineType::FM, D::MacroTimbre, "INDX" }, { EngineType::FM, D::PanSpread, "PAN" }, { EngineType::FM, D::Vibrato, "VIB" },
                { EngineType::MacroOsc, D::Volume, "AMP" }, { EngineType::MacroOsc, D::FilterCutoff, "CUT" }, { EngineType::MacroOsc, D::Vibrato, "VIB" },
                // (the macro oscillator's cells are named by the model: TIMB is SAWS on the first one)
                { EngineType::MacroOsc, D::MacroTimbre, "SAWS" }, { EngineType::MacroOsc, D::FMDepth, "SAWS" }, { EngineType::MacroOsc, D::WavePosition, "MORP" },
                { EngineType::MacroOsc, D::PanSpread, "PAN" }, { EngineType::MacroOsc, D::Excite, "BOW" },
                { EngineType::Wavetable, D::Volume, "AMP" }, { EngineType::Wavetable, D::FilterCutoff, "CUT" }, { EngineType::Wavetable, D::Vibrato, "VIB" },
                { EngineType::Wavetable, D::WavePosition, "POS" }, { EngineType::Wavetable, D::MacroTimbre, "POS" }, { EngineType::Wavetable, D::PanSpread, "PAN" },
                { EngineType::WaveSeq, D::Volume, "AMP" }, { EngineType::WaveSeq, D::FilterCutoff, "CUT" }, { EngineType::WaveSeq, D::PanSpread, "PAN" },
                { EngineType::WaveSeq, D::Vibrato, "VIB" },
                { EngineType::Sampler, D::Volume, "AMP" }, { EngineType::Sampler, D::FilterCutoff, "CUT" }, { EngineType::Sampler, D::PanSpread, "PAN" },
                { EngineType::Sampler, D::MacroTimbre, "DRIV" }, { EngineType::Sampler, D::WavePosition, "GPOS" }, { EngineType::Sampler, D::Vibrato, "VIB" },
                { EngineType::Resonator, D::Excite, "EXC" }, { EngineType::Resonator, D::Volume, "AMP" }, { EngineType::Resonator, D::FilterCutoff, "CUT" },
                { EngineType::Resonator, D::Vibrato, "VIB" }, { EngineType::Resonator, D::PanSpread, "PAN" },
                { EngineType::Drum, D::DrumDecay, "DEC" }, { EngineType::Drum, D::DrumPitch, "PTCH" }, { EngineType::Drum, D::FilterCutoff, "TONE" },
                { EngineType::Drum, D::Volume, "AMP" }, { EngineType::Drum, D::DrumDrive, "DRIV" }, { EngineType::Drum, D::DrumSnap, "SNAP" },
                { EngineType::Drum, D::PanSpread, "PAN" } };
            for (auto& w : was)
            {
                const juce::String name = juce::String((int) w.type) + " " + AftertouchRouter::getDestName(w.dest);
                Rig rig(w.type);
                const auto& t = rig.table();
                // One old route, loaded as a project of then stores it: the engine's own answer.
                rig.engine().setPressureRoute(1, { w.dest, 0.7f, true });
                const int value = (int) rig.get(destId(1));
                expect(value > 0, name + ": has a place in the list");
                expectEquals(juce::String(t.pressureLabels()[value]), juce::String(w.entry), name);
                expectEquals(rig.get(amountId(1)), 0.7f, name + ": the depth");
                expectEquals(rig.get(bipolarId(1)), 1.0f, name + ": bipolar");
                expect(value < t.pressureTrack(), name + ": of the engine, not of the track's effects");
                // ... and back to the same words (where two Destinations are one thing here, the first).
                expect(rig.engine().pressureDestOf(rig.engine().pressureDestAsRoute(value)) == (w.dest == D::Vibrato ? 0 : value)
                       && (w.dest != D::Vibrato || rig.engine().pressureDestAsRoute(value) == D::Vibrato), name + ": read back");
                // One turn on AT ROUTE away: it is in the menu (EXC, BOW and VIB at its top).
                int track = 0;
                AftertouchScreen screen(rig.audio, [&] { return track; });
                const int place = t.pressurePlaceOf(value);
                expectEquals(juce::String(screen.getDestLabels()[place]), juce::String(w.entry), name + ": in AT ROUTE's menu");
                if (juce::String(w.entry) == "EXC" || juce::String(w.entry) == "BOW")
                    expectEquals(place, 1, name + ": first after OFF");
                if (juce::String(w.entry) == "VIB")
                    expect(place <= 2, name + ": at the top of the menu");
            }

            // Heard: the route alone, full depth, the pad half down, against the same note with no
            // route and with the pad at rest. Each render is a new engine with the system random in the same state, so
            // two renders of the same thing are the same samples (checked: else the test says nothing).
            struct Heard { EngineType type; const char* entry; std::function<void(Rig&)> sound; };
            const auto none = [](Rig&) {};
            const Heard heard[] = {
                { EngineType::UnifiedVoice, "AMP", none }, { EngineType::UnifiedVoice, "CUT", none }, { EngineType::UnifiedVoice, "VIB", none },
                { EngineType::UnifiedVoice, "PW", [](Rig& r) { r.set(UnifiedParam::WAVE1, 2.0f); r.set(UnifiedParam::MIX, 0.0f); } },
                { EngineType::UnifiedVoice, "MIX", [](Rig& r) { r.set(UnifiedParam::WAVE2, 0.0f); r.set(UnifiedParam::MIX, 0.2f); } },
                { EngineType::UnifiedVoice, "PAN", none },
                { EngineType::FM, "AMP", none }, { EngineType::FM, "INDX", none }, { EngineType::FM, "VIB", none }, { EngineType::FM, "PAN", none },
                { EngineType::MacroOsc, "AMP", none }, { EngineType::MacroOsc, "SAWS", none }, { EngineType::MacroOsc, "MORP", none },
                { EngineType::MacroOsc, "VIB", none },
                { EngineType::MacroOsc, "BOW", [](Rig& r) { r.set(MacroOscEngine::ModelParam, 7.0f); } },
                { EngineType::Wavetable, "AMP", none }, { EngineType::Wavetable, "POS", none }, { EngineType::Wavetable, "VIB", none },
                { EngineType::WaveSeq, "AMP", none }, { EngineType::WaveSeq, "PAN", none }, { EngineType::WaveSeq, "VIB", none },
                { EngineType::Resonator, "EXC", [](Rig& r) { r.set(ResonatorEngine::ExciterMorphParam, (float) res::Bow); } },
                { EngineType::Resonator, "AMP", none }, { EngineType::Resonator, "VIB", none },
                // (DEC: on a clap, whose decay it is under a held pad; a kick's is its release)
                { EngineType::Drum, "DEC", [](Rig& r) { r.set(DrumEngine::TYPE, (float) (int) DrumSynthType::Clap); r.set(DrumEngine::DECAY, 0.2f); } },
                { EngineType::Drum, "PTCH", none }, { EngineType::Drum, "AMP", none }, { EngineType::Drum, "VIB", none } };
            for (auto& h : heard)
            {
                const juce::String name = juce::String((int) h.type) + " " + h.entry;
                const int note = h.type == EngineType::Drum ? 0 : 57;
                // value < 0: no route at all.
                const auto render = [&](bool routed, float pressure)
                {
                    u4uv::seedSystemRandom(0x5eed77);
                    Rig rig(h.type);
                    h.sound(rig);
                    const auto& t = rig.table();
                    int value = 0;
                    for (int v = 1; v < t.pressureTrack(); ++v)
                        if (juce::String(t.pressureLabels()[v]) == h.entry)
                            value = v;
                    expect(value > 0, name + ": in the list");
                    for (int p = 0; p < 4; ++p)
                        rig.set(destId(p), 0.0f);
                    if (routed)
                    {
                        rig.set(destId(0), (float) value);
                        rig.set(amountId(0), 1.0f);
                    }
                    const auto out = rig.pressed(pressure, note, h.type == EngineType::Drum ? 4 : 28);
                    expect(rig.audio.getNonFiniteBlocks() == 0);
                    return out;
                };
                const auto rest = render(false, 0.5f), again = render(false, 0.5f);
                expect(apart(rest, again) < 1.0e-6, name + ": the same render twice is the same (" + juce::String(apart(rest, again), 6) + ")");
                const auto moved = render(true, 0.5f), up = render(true, 0.0f);
                expect(apart(rest, moved) > 0.01, name + ": heard under the pad (" + juce::String(apart(rest, moved), 4) + ")");
                // (EXC is the one route whose presence is heard at rest: a note of a resonator with
                // the pad on its exciter starts at its strike and not from nothing.)
                if (juce::String(h.entry) != "EXC")
                    expect(apart(up, rest) < 1.0e-6, name + ": the pad at rest, the route does nothing (" + juce::String(apart(up, rest), 6) + ")");
            }
        }

        beginTest("The slots are part of a sound: captured, in a preset and a project by name, in a clip; an untouched track is empty, a changed route is not");
        {
            Rig rig(EngineType::Resonator, 2);
            const auto& t = rig.table();
            const int reso = t.indexOfLabel("RESO"), vib = t.pressureVibrato(), effect = t.pressureTrack() + mod::TrackDest::ofInsert(0, 0);
            EmptyTracks empty;
            expect(empty.isEmpty(rig.seq, &rig.audio, nullptr, 0), "as it is made: empty");
            rig.set(amountId(0), 2.0f);                 // EXC at depth 2 (more than the screen's range: a file's)
            expect(! empty.isEmpty(rig.seq, &rig.audio, nullptr, 0), "a changed route is a changed sound");
            expect(empty.reasonFor(rig.seq, &rig.audio, nullptr, 0) == EmptyTracks::Reason::Sound);
            expect(empty.isEmpty(rig.seq, &rig.audio, nullptr, 1), "the other track is still empty");
            rig.set(destId(1), (float) reso);           // nothing an old route could say
            rig.set(amountId(1), -0.4f);
            rig.set(bipolarId(1), 1.0f);
            rig.set(destId(2), (float) vib);
            rig.set(amountId(2), 0.25f);
            rig.set(destId(3), (float) effect);
            rig.set(amountId(3), 0.6f);
            const auto own = slotsOf(rig.engine());
            expectEquals(own[1], 2.0f, "the amount keeps what a route's depth could be");

            // Captured with the sound (every id, though they are on no page).
            const ProjectBridge::Live live { rig.seq, rig.audio };
            const auto sound = ProjectBridge::captureTrackSound(live, 0, rig.seq.getPattern().getTrack(0).soundParams);
            for (int id = PressureSlotParam::BASE; id < PressureSlotParam::END; ++id)
                expectEquals(sound.count(id) != 0 ? sound.at(id) : -99.0f, rig.engine().getParameter(id), "captured: id " + juce::String(id));

            // A preset: the DEST cells carry their names, and a name wins over a number that moved.
            Preset preset;
            preset.name = "Pressed";
            preset.engineType = EngineType::Resonator;
            preset.soundParams = sound;
            auto stored = preset.toVar();
            int named = 0;
            for (auto& p : *stored.getProperty("params", {}).getArray())
                if (PressureSlotParam::contains((int) p.getProperty("id", -1)) && p.hasProperty("n"))
                {
                    ++named;
                    const int id = (int) p.getProperty("id", -1);
                    expectEquals(p.getProperty("n", {}).toString(), juce::String(t.pressureLabels()[(int) sound.at(id)]));
                    p.getDynamicObject()->setProperty("value", 1.0);       // (as if the table had grown since)
                }
            expectEquals(named, 4, "the four DEST cells are stored by name");
            const auto back = Preset::fromVar(stored);
            for (int id = PressureSlotParam::BASE; id < PressureSlotParam::END; ++id)
                expectEquals(back.soundParams.count(id) != 0 ? back.soundParams.at(id) : -99.0f, sound.at(id), "preset: id " + juce::String(id));

            // A clip / an undo step (GrammarHooks: capture, a cleared sound, the captured one again).
            auto hooks = GrammarHooks::make(rig.seq, rig.audio);
            const auto clip = hooks.captureSound(0);
            hooks.applySound(0, EngineType::Resonator, {});
            expect(slotsOf(rig.engine()) != own, "a cleared sound: the routes a resonator is made with");
            expect(empty.isEmpty(rig.seq, &rig.audio, nullptr, 0));
            hooks.applySound(0, EngineType::Resonator, clip);
            expect(slotsOf(rig.engine()) == own, "pasted / undone: the routes are back");

            // A project, through its file form.
            const auto saved = ProjectData::fromVar(ProjectBridge::capture(live, "SLOTS").toVar());
            SequencerCore seq2;
            AudioEngine audio2;
            ProjectBridge::apply(saved, { seq2, audio2 });
            expect(slotsOf(*audio2.getEngine(0)) == own, "a project brings every slot back");
            expect(slotsOf(*audio2.getEngine(1)) == slotsOf(*AudioEngine::createEngine(EngineType::Resonator)), "and the untouched track's");
            EmptyTracks empty2;
            expect(empty2.isEmpty(seq2, &audio2, nullptr, 1), "which is still empty after a load");
            expect(! empty2.isEmpty(seq2, &audio2, nullptr, 0));
        }

        beginTest("An old project's routes (no slot cells) do what they did: each engine's own map, the depth held to -1 .. 1, a drum kit's one route");
        {
            Rig rig(EngineType::Wavetable);
            rig.audio.addEngine(1, AudioEngine::createEngine(EngineType::Drum));
            rig.audio.addEngine(2, AudioEngine::createEngine(EngineType::FM));
            for (int p = 0; p < SequencerCore::MAX_PATTERNS; ++p)
            {
                rig.seq.getPattern(p).getTrack(1).engineType = EngineType::Drum;
                rig.seq.getPattern(p).getTrack(2).engineType = EngineType::FM;
            }
            auto data = ProjectBridge::capture({ rig.seq, rig.audio }, "OLD");
            for (auto& pattern : data.patterns)
                for (auto& track : pattern.tracks)
                    for (int id = PressureSlotParam::BASE; id < PressureSlotParam::END; ++id)
                        track.soundParams.erase(id);
            data.trackAftertouch.resize(3);
            data.trackAftertouch[0].routes = { { (int) D::MacroTimbre, 0.5f, false },      // not on the wavetable's list then: OFF
                                               { (int) D::WavePosition, 3.0f, true }, { (int) D::Vibrato, -0.25f, false } };
            data.trackAftertouch[1].routes = { { (int) D::DrumSnap, 0.9f, false }, { (int) D::Volume, 1.0f, false } };
            data.trackAftertouch[2].routes = { { (int) D::FMDepth, 1.0f, false }, { (int) D::PanSpread, 1.0f, false } };   // PAN: not offered FM then
            SequencerCore seq2;
            AudioEngine audio2;
            ProjectBridge::apply(ProjectData::fromVar(data.toVar()), { seq2, audio2 });
            const auto slot = [&](int t, int id) { return audio2.getTrackParameter(t, id); };
            const auto& wt = WavetableEngine::lfoDests();
            expectEquals(slot(0, destId(0)), 0.0f, "TIMB was not the wavetable's to route: off, as it loaded then");
            expectEquals(slot(0, destId(1)), (float) wt.indexOfLabel("POS"));
            expectEquals(slot(0, amountId(1)), 1.0f, "a project's depth is -1 .. 1");
            expectEquals(slot(0, bipolarId(1)), 1.0f);
            expectEquals(slot(0, destId(2)), (float) wt.pressureVibrato());
            expectEquals(slot(0, amountId(2)), -0.25f);
            expectEquals(slot(1, destId(0)), (float) DrumEngine::lfoDests().indexOfLabel("SNAP"));
            expectEquals(slot(1, destId(1)), 0.0f, "a kit had one route: a second one in the file never played");
            auto* drums = dynamic_cast<DrumEngine*>(audio2.getEngine(1));
            expect(drums != nullptr && drums->getVoice(7).getAftertouchDest() == DrumVoiceATDest::Snap);
            expectWithinAbsoluteError(drums->getVoice(7).getAftertouchDepth(), 0.9f, 0.001f);
            expectEquals(slot(2, destId(0)), (float) FMEngine::lfoDests().indexOfLabel("INDX"));
            expectEquals(slot(2, destId(1)), 0.0f);
        }

        beginTest("Pressure on a cell of an effect on the track: the pad of the voice that leads moves it, and it comes back when the pad is up");
        {
            Rig rig(EngineType::FM);
            rig.audio.addInsertEffect(0, EffectType::FilterEffectType);
            const auto& t = rig.table();
            const int d = mod::TrackDest::ofInsert(0, 0);
            for (int p = 0; p < 4; ++p)
                rig.set(destId(p), 0.0f);
            rig.set(destId(0), (float) (t.pressureTrack() + d));
            rig.set(amountId(0), -0.8f);
            int track = 0;
            AftertouchScreen screen(rig.audio, [&] { return track; });
            expect(screen.getSlotDestText(0).startsWith("1"), "the display names the effect's cell: " + screen.getSlotDestText(0));
            expect(screen.getSlotDestText(0) == rig.audio.trackDestText(0, d));
            const auto& router = *rig.engine().modRouter();
            std::vector<float> l(256), r(256);
            const auto run = [&](int blocks) { for (int b = 0; b < blocks; ++b) rig.audio.renderBlock(l.data(), r.data(), 256); };
            rig.engine().noteOn(57, 0.8f);
            run(20);
            expect(router.trackDests().test(d));
            expectEquals(router.trackOffset(d), 0.0f, "pad at rest: nothing");
            rig.engine().aftertouch(57, 0.5f);
            run(20);
            expectWithinAbsoluteError(router.trackOffset(d), -0.4f, 0.001f, "half the pad: half the amount of the cell's range");
            // A second note that is not pressed does not take the cell away from the pressed one.
            rig.engine().noteOn(64, 0.8f);
            run(20);
            expectWithinAbsoluteError(router.trackOffset(d), -0.4f, 0.001f);
            rig.engine().aftertouch(57, 0.0f);
            run(20);
            expectEquals(router.trackOffset(d), 0.0f, "pad up: the cell is given back");
            rig.engine().aftertouch(64, 1.0f);
            run(20);
            expectWithinAbsoluteError(router.trackOffset(d), -0.8f, 0.001f, "the other pad: its voice leads now");
            rig.engine().noteOff(64);
            rig.engine().noteOff(57);
            run(400);
            expectEquals(router.trackOffset(d), 0.0f, "the notes over: nothing is left on the cell");
            expect(rig.audio.getNonFiniteBlocks() == 0);
        }

        beginTest("Recorded pressure becomes locks through the slots: a route's old cell, or the cell a slot is aimed at");
        {
            Rig rig(EngineType::MacroOsc);
            const auto& t = rig.table();
            ParamSpec cell;
            // The level: no cell of the table, the LEVL cell of the page, as an old route locked it.
            expect(rig.audio.pressureLockCell(0, D::Volume, 60, cell));
            const int level = cell.id;
            expect(rig.audio.pressureLockCellOf(0, rig.engine().pressureDestOf(D::Volume), 60, cell) && cell.id == level);
            // TIMB: the cell the slot is aimed at, whatever the model calls it (an old route found
            // it only where the model's name for it was TIMB).
            const int timbre = rig.engine().pressureDestOf(D::MacroTimbre);
            expect(rig.audio.pressureLockCellOf(0, timbre, 60, cell) && cell.id == t[timbre].id);
            // RESO: no old route says it; the slot's destination is a page cell all the same.
            const int reso = t.indexOfLabel("RESO");
            const bool hasCell = t[reso].id >= 0;
            expect(rig.audio.pressureLockCellOf(0, reso, 60, cell) == hasCell);
            // VIB, BOW and an effect's cell are no cell of the engine: nothing to lock.
            expect(! rig.audio.pressureLockCellOf(0, t.pressureVibrato(), 60, cell));
            expect(! rig.audio.pressureLockCellOf(0, rig.engine().pressureDestOf(D::Excite), 60, cell));
            expect(! rig.audio.pressureLockCellOf(0, t.pressureTrack(), 60, cell));
            expect(! rig.audio.pressureLockCellOf(0, 0, 60, cell));
        }
    }
};

static PressureSlotsTests pressureSlotsTests;
