#pragma once

#include "EngineBase.h"
#include "ArchitecturePreset.h"
#include "VoiceAllocator.h"
#include "UnifiedVoice.h"
#include <array>
#include <vector>

// The parameter ids (namespace UnifiedParam) are in UnifiedVoice.h.
//
// ON THE MODULATION ROUTER (U4-UV; ModRouter.h, "PER ENGINE: unified voice"). One mod::Router for
// the engine (four LFOs, the extra envelopes as ENV2 .. ENV5), one mod::Voice in each voice,
// rendered by sub-blocks (UnifiedVoice::render) and mixed sample by sample in voice order as before.
//   on the router    the LFOs (setLfo with every cell; DPTH and DEST for an LFO without WHL / VEL
//                    / PRES), the sixteen free slots and LSHP (700-772), the STRIP page (158-165)
//                    and the XIN page (166-173) (PageIds: 174-189 are the stored LFO cells), the
//                    pressure route PAN, PM / RING / SYNC (the old XIN cell's three behaviours; a
//                    stored XIN / XDPT becomes the XIN page's first route: setLegacyXin), ETIM / FTIM
//   the voice's own  what "P01 Keyboard" pins bit for bit and the router cannot hold bit for bit:
//                    an LFO with a WHL, VEL or PRES amount (the classic path: LFOBank, its leads,
//                    its destinations; the router has no additive depth, and its control rate is
//                    not the per-sample path P01's DuneDrone plays with the strip up), the pressure
//                    routes VOL CUT FM TIMB WAVE VIB, the strip's built-in vibrato (steps aside for
//                    an LFO's WHL or the STRIP page), bend; and the extra envelopes' DEST (the
//                    router has no fixed slot for it)
//   EVAR             Envelope::setTimeScale (the same product as the scaled copy it replaces, drawn
//                    when the copy was drawn; "U4 Unified" proves it bit for bit against the old class)
class UnifiedVoiceEngine : public EngineBase
{
public:
    static constexpr int MAX_VOICES = 16;

    UnifiedVoiceEngine(int maxVoices = 8)
        : allocator(juce::jlimit(1, MAX_VOICES, maxVoices))
    {
        // The voice pool never changes size, so the audio thread never sees a reallocation; the
        // allocator decides how many of them are in use.
        voices.resize((size_t) MAX_VOICES);
        // The track's custom LFO shape (USR / USRS): every LFO reads the router's eight levels (LSHP).
        // The router's per-note randoms: seeded by the voice's number, so a render repeats (the
        // ANLG page's spread is still the system random's, under P01's pin).
        for (int i = 0; i < MAX_VOICES; ++i)
        {
            auto& v = voices[(size_t) i];
            for (int l = 0; l < LFOBank::MAX_LFOS; ++l)
                v.getLFOBank().getLFO(l).setUserShape(router.shape());
            v.getRouterVoice().seed(0, i);
        }
        routerDests().registerGroups();
        // The six pressure destinations the voice plays on its own path (updatePressure).
        router.setOwnPressure(ownPressureDests(), true);
        UnifiedVoice::sharedTables();        // built here, on the message thread, not at the first note
        installAllocatorCallbacks();

        initialVoiceCount = juce::jlimit(1, MAX_VOICES, maxVoices);
        loadInitPatch();
    }

    EngineType getType() const override { return EngineType::UnifiedVoice; }
    juce::String getName() const override { return "Unified Voice"; }

    void prepareToPlay(double sr, int /*samplesPerBlock*/) override
    {
        sampleRate = sr;
        router.prepare(sr);
        for (auto& v : voices)
            v.setSampleRate(sr);
        // Glide coefficients depend on the sample rate.
        applyParameter(UnifiedParam::GLIDE, flatParams[UnifiedParam::GLIDE]);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        int numSamples = buffer.getNumSamples();

        modWheelNow = smoothWheel(modWheelNow, modWheelValue, numSamples, sampleRate);
        updatePressure();

        const float gain = HEADROOM * unisonGain;       // LEVL is applied by each voice (UnifiedVoice::setLevel)

        // The modulation router, once a block. (The pressure slots aimed at the six destinations
        // the voice plays itself - updatePressure - are left out by the router: setOwnPressure in
        // the constructor. PAN and everything else is the router's.)
        // When an LFO in use has a WHL amount the strip goes to those LFOs and nothing else:
        // the router's strip routes see 0 (the voice's LFO path handles the strip instead).
        const float routerStrip = wheelIsRouted() ? 0.0f : modWheelNow;
        router.beginBlock(audioInput.left, audioInput.right, audioInput.connected() ? audioInput.frames : 0,
                          audioInput.envelope, routerStrip, sampleRate);
        // RING: a slot that brings the track input there takes its amount of the dry mixer away
        // while the track has an input (the ring, (1 - d) + d x; without an input, the voice).
        // And an input that comes or goes under a route that reads it is a swapped sound (the
        // track's declick), as it was under the old XIN cell.
        {
            float dry = 0.0f;
            bool reads = false;
            const auto isInput = [this](int place)
            {
                const int source = router.sources().source(place);
                return source >= mod::Src::Xin && source <= mod::Src::XinBand;
            };
            for (int x = 0; x < mod::Router::NUM_XIN; ++x)
            {
                const int dest = (int) router.get(mod::Router::xinId(x * 4 + mod::Router::XinDest1));
                const float amount = router.get(mod::Router::xinId(x * 4 + mod::Router::XinAmount1));
                reads = reads || (dest != 0 && amount != 0.0f);
                if (dest == UnifiedVoice::RRing)
                    dry += amount;
            }
            for (int k = 0; k < mod::Router::NUM_FREE; ++k)
            {
                const auto& slot = router.slot(k);
                if (! isInput(slot.src) || slot.dest == 0)
                    continue;
                reads = reads || slot.amount != 0.0f;
                if (slot.dest == UnifiedVoice::RRing)
                    dry += slot.amount;
            }
            const bool live = reads && audioInput.connected();
            if (live != inputHeard)
            {
                inputHeard = live;
                if (reads)
                    soundSwapped();
            }
            if (! audioInput.connected())
                dry = 0.0f;
            const bool syncLive = router.routed(UnifiedVoice::RSync);
            for (auto& v : voices)
            {
                v.setRingDry(dry);
                v.setSyncLive(syncLive);
            }
        }
        // XMOD and FFM work through oscillator 2: it runs while the router moves either.
        const bool osc2Routed = router.routed(UnifiedVoice::RXmod) || router.routed(UnifiedVoice::RFilterFm);
        if (osc2Routed != osc2ForRouter)
        {
            osc2ForRouter = osc2Routed;
            applyOscLevels();
        }

        // Sub-blocks (the router's control rate), the voices one after the other inside each (they
        // share the router's per-sample buffers); then the samples are mixed as they always were,
        // voice by voice in order, for each sample.
        for (int offset = 0; offset < numSamples;)
        {
            const int n = router.beginChunk(offset, numSamples);
            for (int v = 0; v < MAX_VOICES; ++v)
            {
                auto& voice = voices[(size_t) v];
                if (! voice.isActive())
                {
                    made[(size_t) v] = 0;
                    voice.idle(router, n);
                    continue;
                }
                made[(size_t) v] = voice.render(router, n, voiceOut[(size_t) v].data(), voicePan[(size_t) v].data());
            }
            for (int i = 0; i < n; ++i)
            {
                // An LFO routed to PAN moves its own voice between the channels (balance law: the
                // centre is unchanged, so a patch without that routing sounds exactly as before).
                float sampleL = 0.0f, sampleR = 0.0f;
                for (int v = 0; v < MAX_VOICES; ++v)
                {
                    if (i >= made[(size_t) v])
                        continue;
                    const float s = voiceOut[(size_t) v][(size_t) i];
                    const float pan = voicePan[(size_t) v][(size_t) i];
                    sampleL += pan > 0.0f ? s * (1.0f - pan) : s;
                    sampleR += pan < 0.0f ? s * (1.0f + pan) : s;
                }

                if (right)
                {
                    left[offset + i] += sampleL * gain;
                    right[offset + i] += sampleR * gain;
                }
                else
                {
                    left[offset + i] += 0.5f * (sampleL + sampleR) * gain;
                }
            }
            offset += n;
        }

        // Voices that are not sounding still keep their free-running LFOs in time, so FREE and
        // HOLD behave like one LFO that never stops.
        for (auto& voice : voices)
            if (!voice.isActive())
                voice.advanceIdleLfos(numSamples);
    }

    void releaseResources() override
    {
        for (auto& v : voices)
            v.reset();
    }

    void noteOn(int note, float velocity, int /*voiceIndex*/) override
    {
        if (isMono())
        {
            monoNoteOn(note, velocity);
            return;
        }
        allocator.noteOn(note, velocity);
    }

    void noteOff(int note, int /*voiceIndex*/) override
    {
        if (isMono())
        {
            monoNoteOff(note);
            return;
        }
        allocator.noteOff(note);
    }

    void allNotesOff() override
    {
        soundCut();
        monoStackCount = 0;
        monoNote = -1;
        monoPressure = 0.0f;
        allocator.allNotesOff();
        for (auto& v : voices)
            v.reset();
    }

    void aftertouch(int note, float pressure) override
    {
        if (isMono())
        {
            if (note == monoNote)
                monoPressure = pressure;
            return;
        }
        allocator.setAftertouch(note, pressure);
    }

    // -1..1 from the pitch strip; the range is the BEND parameter.
    void pitchBend(float amount) override
    {
        bendAmount = juce::jlimit(-1.0f, 1.0f, amount);
        float semis = bendAmount * flatParams[UnifiedParam::BEND_RANGE];
        for (auto& v : voices)
            v.setPitchBend(semis);
    }

    // 0..1 from the mod strip. It goes to every LFO whose WHL amount is set (MOD2 page); while no
    // LFO in use listens to it, it is vibrato (up to half a semitone), so the strip always does
    // something.
    void modWheel(float value) override
    {
        modWheelValue = juce::jlimit(0.0f, 1.0f, value);
    }

    // ---- Per-stage envelope curves (StageCurveParam) --------------------------------------------
    // Envelope slots: 0 amp, 1 filter envelope, 2-5 the extra envelopes 1-4 (MENV). On the amp and
    // filter envelopes they add to the CURV cell.
    static int stageCurveParamFor(ParamId cellId)
    {
        using namespace UnifiedParam;
        switch (cellId)
        {
            case AMP_ATTACK:   return StageCurveParam::id(0, StageCurveParam::Attack);
            case AMP_DECAY:    return StageCurveParam::id(0, StageCurveParam::Decay);
            case AMP_RELEASE:  return StageCurveParam::id(0, StageCurveParam::Release);
            case FENV_ATTACK:  return StageCurveParam::id(1, StageCurveParam::Attack);
            case FENV_DECAY:   return StageCurveParam::id(1, StageCurveParam::Decay);
            case FENV_RELEASE: return StageCurveParam::id(1, StageCurveParam::Release);
            default: break;
        }
        if (cellId >= ModParam::ENV_BASE && cellId < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            const int env = (cellId - ModParam::ENV_BASE) / ModParam::ENV_STRIDE;
            const int cell = (cellId - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
            if (cell == ModParam::ENV_ATK) return StageCurveParam::id(2 + env, StageCurveParam::Attack);
            if (cell == ModParam::ENV_DEC) return StageCurveParam::id(2 + env, StageCurveParam::Decay);
            if (cell == ModParam::ENV_REL) return StageCurveParam::id(2 + env, StageCurveParam::Release);
        }
        return -1;
    }
    int getStageCurveParam(ParamId cellId) const override { return stageCurveParamFor(cellId); }

private:
    StageCurves stageCurves;

    // The curves of extra envelope `env` (0-3), when it is running.
    void applyExtraEnvCurves(int env)
    {
        for (auto& v : voices)
        {
            auto& chain = v.getEnvelopeChain();
            if (env >= chain.getStageCount())
                continue;
            auto p = chain.getEnvelopeMut(env).getParams();
            stageCurves.applyTo(p, 2 + env);
            chain.getEnvelopeMut(env).setParams(p);
        }
    }

    void setStageCurve(ParamId id, float value)
    {
        stageCurves.set(id, value);
        const int slot = StageCurveParam::envelopeOf(id);
        if (slot < 2)                               applyEnvelope(slot == 1);
        else if (slot < 2 + ModParam::MAX_ENVS)     applyExtraEnvCurves(slot - 2);
    }

public:
    void setParameter(ParamId id, float value) override
    {
        if (StageCurveParam::owns(id))
        {
            setStageCurve(id, value);
            return;
        }
        // The router's cells: the free slots, LSHP and the slots shown at their own ids; the STRIP
        // and XIN pages at 158-173 here (174-189 are this engine's stored LFO cells).
        if (mod::Router::ownsSlotBlock(id) || PressureSlotParam::contains(id))
        {
            router.set(id, value);
            return;
        }
        if (const int routerId = routerIdOf(id); routerId >= 0)
        {
            router.set(routerId, value);
            return;
        }
        if (id == UnifiedParam::XIN || id == UnifiedParam::XIN_DEPTH)
        {
            setLegacyXin(id, value);
            return;
        }
        if (!EngineParamRange::contains(id) || id >= UnifiedParam::MAX_ID)
            return;
        if (id == UnifiedParam::MODE_A || id == UnifiedParam::MODE_B)
        {
            auto model = FilterSlot::modelFromIndex(
                (int) flatParams[id == UnifiedParam::MODE_A ? UnifiedParam::MODEL_A : UnifiedParam::MODEL_B]);
            value = (float) juce::jlimit(0, FilterSlot::modeCount(model) - 1, (int) std::lround(value));
        }
        else if (const ParamSpec* spec = findSpec(id))
        {
            value = spec->isDiscrete() ? std::round(spec->clamp(value)) : spec->clamp(value);
            // A selector (waveform, filter type, routing, stack ...) turned under a held note.
            if (spec->kind == ParamKind::Choice && flatParams[(size_t) id] != value)
                soundSwapped();
        }
        if ((id == UnifiedParam::MODE_A || id == UnifiedParam::MODE_B) && flatParams[(size_t) id] != value)
            soundSwapped();
        // The sound of the ARCH that is being left is kept BEFORE flatParams is overwritten
        // (archIndex() is still the old slot here).
        if (id == UnifiedParam::ARCH)
        {
            const int oldSlot = archIndex();
            const int newSlot = juce::jlimit(0, ARCH_SLOTS - 1, (int) std::lround(value));
            if (newSlot != oldSlot)
                snapshotArch(oldSlot);
        }
        flatParams[(size_t) id] = value;
        applyParameter(id, value);
        // The set value of a cell the router can move (it keeps the set values of its destinations).
        router.setBase(id, flatParams[(size_t) id]);

        // Page cells an LFO or the extra envelopes are routed to: the voices move them around
        // their set value, so they are told when it changes (and when an oscillator's kind, and
        // with it the range of its SRC cells, does).
        if (id == UnifiedParam::ARCH || id == UnifiedParam::WAVE1 || id == UnifiedParam::WAVE2 || id == UnifiedParam::WAVE3)
            refreshRoutes();
        else if (isRouted(id))
            for (auto& v : voices)
                v.setParamRouteBase(id, flatParams[(size_t) id]);
    }

    float getParameter(ParamId id) const override
    {
        if (const int routerId = routerIdOf(id); routerId >= 0)
            return router.get(routerId);
        if (id == UnifiedParam::XIN)
            return (float) legacyXin;
        if (id == UnifiedParam::XIN_DEPTH)
            return legacyXinDepth;
        if (id >= 0 && id < UnifiedParam::MAX_ID)
            return flatParams[(size_t) id];
        if (mod::Router::ownsSlotBlock(id) || PressureSlotParam::contains(id))
            return router.get(id);
        return stageCurves.get(id);     // 0 for anything else
    }

    juce::String getParameterName(ParamId id) const override
    {
        if (const ParamSpec* spec = findSpec(id))
            return spec->label;
        if (const int routerId = routerIdOf(id); routerId >= 0)
            return mod::Router::name(routerId);
        if (id == UnifiedParam::XIN)        return "XIN";       // (the old cells, from a stored sound: setLegacyXin)
        if (id == UnifiedParam::XIN_DEPTH)  return "XDPT";
        if (mod::Router::ownsSlotBlock(id))
            return mod::Router::name(id);
        return "Param " + juce::String(id);
    }

    // ---- The modulation router ----------------------------------------------------------------
    // Ids: the STRIP page at 158-165 and the XIN page at 166-173 (the router's own 174-189 are
    // this engine's stored LFO cells WHL / VEL / KTRK / PRES until format 3; PageIds); the free
    // slots 700-763, LSHP 764-771 and 772 at the router's own ids.
    static constexpr int STRIP_IDS = 158, XIN_IDS = 166;
    static int routerIdOf(int id)
    {
        if (id >= STRIP_IDS && id < STRIP_IDS + 8) return id - STRIP_IDS + mod::Router::STRIP_BASE;
        if (id >= XIN_IDS && id < XIN_IDS + 8)     return id - XIN_IDS + mod::Router::XIN_BASE;
        return -1;
    }

    // What can be moved (UnifiedVoice::RouterDest): the LFOs' list as it was, index for index and in
    // the same units, then PM RING SYNC (the track input's three ways in) and ETIM FTIM.
    static const mod::DestTable& routerDests()
    {
        static const mod::DestTable table = []
        {
            using namespace mod;
            DestTable t;
            t.group();
            t.input("PTCH", 0.0f, 1.0f, -8.0f, 8.0f, PerSample | Steps);   // octaves, not squared (12 semitones at depth 1)
            t.input("CUT", 0.0f, 4.0f, -16.0f, 16.0f, PerSample);           // octaves
            t.input("RESO", 0.0f, 0.5f, -1.0f, 1.0f);
            t.input("MIX", 0.0f, 0.5f, -1.0f, 1.0f, PerSample);
            t.input("AMP", 1.0f, 1.0f, 0.0f, 2.0f, PerSample).audioFloor(-2.0f);   // a gain; below 0: ring modulation
            t.input("PW", 0.0f, 0.4f, -1.0f, 1.0f, PerSample);
            t.input("XMOD", 0.0f, 1.0f, -1.0f, 1.0f, PerSample);
            t.input("CUTB", 0.0f, 4.0f, -16.0f, 16.0f, PerSample);
            t.input("PAN", 0.0f, 1.0f, -1.0f, 1.0f, PerSample);
            t.input("LVL1", 1.0f, 1.0f, 0.0f, 2.0f, PerSample);
            t.input("LVL2", 1.0f, 1.0f, 0.0f, 2.0f, PerSample);
            t.input("LVL3", 1.0f, 1.0f, 0.0f, 2.0f, PerSample);
            t.input("NOIS", 1.0f, 1.0f, 0.0f, 2.0f, PerSample);
            t.input("FFM", 0.0f, 1.0f, -1.0f, 1.0f, PerSample);
            t.input("PIT2", 0.0f, 1.0f, -8.0f, 8.0f, PerSample | Steps);   // octaves
            static const char* const rates[4] = { "RAT1", "RAT2", "RAT3", "RAT4" };
            static const char* const depths[4] = { "DPT1", "DPT2", "DPT3", "DPT4" };
            for (int l = 0; l < 4; ++l) t.lfoCell(rates[l], l, false);
            for (int l = 0; l < 4; ++l) t.lfoCell(depths[l], l, true);
            jassert(t.size() == UnifiedVoice::RFirstParam);
            for (int i = 0; i < UnifiedVoice::NUM_PARAM_DESTS; ++i)
            {
                const auto& p = UnifiedVoice::PARAM_DESTS[i];
                if (p.group)
                    t.group();
                const int at = t.size();
                if (at == UnifiedVoice::RSrcA1 || at == UnifiedVoice::RSrcA2 || at == UnifiedVoice::RSrcA3)
                    t.input(p.label, 0.0f, 1.0f, -1.0f, 1.0f, PerSample);      // offsets (UnifiedVoice::movedSrcA)
                else if (at == UnifiedVoice::RSrcB1 || at == UnifiedVoice::RSrcB2 || at == UnifiedVoice::RSrcB3)
                    t.input(p.label, 0.0f, 1.0f, -1.0f, 1.0f);                 // offsets (UnifiedVoice::movedSrcB)
                else
                    t.add(cellSpec(p.id), p.label);
                jassert(t.size() == at + 1);
            }
            jassert(t.size() == UnifiedVoice::RPm);
            t.group();
            t.input("PM", 0.0f, 1.0f, -1.0f, 1.0f, PerSample | mod::Phase);    // (a phase: U4-1 brings it in and lets it go without a step)
            t.input("RING", 1.0f, 1.0f, 0.0f, 2.0f, PerSample).audioFloor(-2.0f);
            t.input("SYNC", 0.0f, 1.0f, -1.0f, 1.0f, PerSample);
            t.group();
            t.envTimes();
            jassert(t.size() == UnifiedVoice::NUM_ROUTER_DESTS);
            // No DestTable strip defaults: the unified voice's STRIP page starts empty (OFF, 0,
            // OFF, 0, OFF, 0, WVIB 0, VRAT 5.5 Hz) so that old projects without ids 158-165 keep
            // the same sound. The architecture presets seed CUT +50 % and WVIB 0.25 when selected.
            // Pad pressure until AT ROUTE says otherwise: the level (the voice's own path: x (1 + pressure)).
            t.pressureRoute(0, UnifiedVoice::RAmp, 1.0f);
            t.finish(4);
            return t;
        }();
        return table;
    }

    // The spec of a page cell the router can move (the extra envelopes' cells are built by the
    // screen: times 1 ms .. 10 s, the depth 0..1).
    static ParamSpec cellSpec(int id)
    {
        if (const ParamSpec* spec = findSpec(id))
            return *spec;
        if (id >= ModParam::ENV_BASE && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            const int cell = (id - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
            if (cell == ModParam::ENV_ATK || cell == ModParam::ENV_DEC || cell == ModParam::ENV_REL)
                return { "ENVT", id, 0.001f, 10.0f, 0.1f, ParamKind::Log, "s" };
            return { "ENVL", id, 0.0f, 1.0f, cell == ModParam::ENV_DEPTH ? 1.0f : 0.7f, ParamKind::Linear };
        }
        jassertfalse;
        return {};
    }

    // What an old pressure route (AftertouchRouter::Destination) is here, as a slot's DEST. The
    // six the voice plays on its own path (LEVL, CUT, XMOD, PW, MIX and VIB: updatePressure, which
    // P01 pins) and PAN, which is the router's.
    int pressureDestOf(AftertouchRouter::Destination d) const override
    {
        using D = AftertouchRouter::Destination;
        switch (d)
        {
            case D::Volume:         return UnifiedVoice::RAmp;
            case D::FilterCutoff:   return UnifiedVoice::RCutoff;
            case D::FMDepth:        return UnifiedVoice::RXmod;
            case D::MacroTimbre:    return UnifiedVoice::RPw;
            case D::WavePosition:   return UnifiedVoice::RMix;
            case D::PanSpread:      return UnifiedVoice::RPan;
            default:                return 0;
        }
    }
    // The destinations of a pressure slot that the voice plays itself.
    static mod::Mask ownPressureDests()
    {
        mod::Mask m;
        for (int d : { (int) UnifiedVoice::RAmp, (int) UnifiedVoice::RCutoff, (int) UnifiedVoice::RXmod,
                       (int) UnifiedVoice::RPw, (int) UnifiedVoice::RMix })
            m.set(d);
        return m;
    }

    const mod::Router& getRouter() const { return router; }
    mod::Router& getRouter() { return router; }
    mod::Router* modRouter() override { return &router; }
    const mod::Router* modRouter() const override { return &router; }
    // What a slot's SRC and VIA offer here (the router's list for four LFOs and ENV2 .. ENV5).
    static int sourceCount() { return mod::SourceTable::standard(ModParam::MAX_LFOS, ModParam::MAX_ENVS).size(); }
    // Where LFO l is aimed, whichever path it runs on (lfoIsClassic), as its DEST cell counts: a
    // cell of an effect on the track (the router's, on either path) is LfoDest::Count + its number.
    UnifiedVoice::LfoDest lfoDestination(int l) const
    {
        if (l < 0 || l >= ModParam::MAX_LFOS)
            return UnifiedVoice::LfoDest::Off;
        const int dest = router.lfo(l).dest, track = router.dests().trackCell();
        if (dest >= track)
            return (UnifiedVoice::LfoDest) ((int) UnifiedVoice::LfoDest::Count + dest - track);
        return classicLfo[(size_t) l] ? voices[0].getLfoDest(l) : (UnifiedVoice::LfoDest) dest;
    }
    bool lfoOnClassicPath(int l) const { return l >= 0 && l < ModParam::MAX_LFOS && classicLfo[(size_t) l]; }

    int getParameterCount() const override { return UnifiedParam::MAX_ID; }
    bool isRebuildParam(ParamId id) const override { return id == UnifiedParam::ARCH; }

    bool supportsModulation() const override { return true; }
    int getMasterParamId() const override { return UnifiedParam::ARCH; }

    // ---- Pages -------------------------------------------------------------------------------

    ParamPages getParamPages() const override
    {
        ParamPages pages = basePages();
        // The third oscillator is a sub in the mono architecture and a full oscillator otherwise.
        const bool subStyle = (currentPresetId == ArchitecturePresetId::MonoVA && archIndex() == 1);
        pages[1].name = subStyle ? "SUB" : "OSC3";
        pages[1].params[2].label = subStyle ? "SUB" : "LVL3";

        // SRC: two cells for each oscillator that plays something other than an analog wave.
        for (auto& page : pages)
            if (page.name == "SRC")
                for (int osc = 0; osc < 3; ++osc)
                {
                    const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                    page.params[(size_t) (osc * 2)] = srcSpec(osc, kind, false);
                    page.params[(size_t) (osc * 2 + 1)] = srcSpec(osc, kind, true);
                }

        // The MODE cells offer the modes of the filter type currently chosen.
        for (auto& page : pages)
            for (auto& p : page.params)
                if (p.id == UnifiedParam::MODE_A || p.id == UnifiedParam::MODE_B)
                {
                    auto model = FilterSlot::modelFromIndex(
                        (int) flatParams[p.id == UnifiedParam::MODE_A ? UnifiedParam::MODEL_A : UnifiedParam::MODEL_B]);
                    p.choices = FilterSlot::modeNames(model);
                    p.numChoices = FilterSlot::modeCount(model);
                    p.maxVal = (float) (p.numChoices - 1);
                }
        // The router's pages: XIN, SLT1 .., LSHP, STRIP (the screen puts the LFO and envelope
        // pages MOD, MOD2 and MENV after them; the LFO cells are this engine's own ids).
        mod::Router::PageIds ids;
        ids.strip = STRIP_IDS;
        ids.xin = XIN_IDS;
        ids.lfoPages = false;
        router.appendPages(pages, ids);
        return pages;
    }

    // The SRC cell of oscillator `osc` (0..2) for WAV choice `kind`; invalid for the analog waves.
    // Labels carry the oscillator's number: 1POS 1TBL, 2IDX 2RAT, 3FRM 3WID, 1TLT 1ODD.
    static ParamSpec srcSpec(int osc, int kind, bool second)
    {
        static const char* const labels[4][2][3] = {
            { { "1POS", "2POS", "3POS" }, { "1TBL", "2TBL", "3TBL" } },
            { { "1IDX", "2IDX", "3IDX" }, { "1RAT", "2RAT", "3RAT" } },
            { { "1FRM", "2FRM", "3FRM" }, { "1WID", "2WID", "3WID" } },
            { { "1TLT", "2TLT", "3TLT" }, { "1ODD", "2ODD", "3ODD" } } };
        // nTBL: the wavetable engine's built-in set, by its names (the first nine, then the newer ones).
        const int numTables = WavetableFactory::builtInCount();
        const int k = kind - (int) UnifiedVoice::OscWaveform::Wavetable;
        if (k < 0 || k > 3 || osc < 0 || osc > 2)
            return {};
        const int id = second ? UnifiedParam::srcB(osc) : UnifiedParam::srcA(osc);
        const char* label = labels[k][second ? 1 : 0][osc];
        const float d = srcDefault(kind, second);
        if (second && k == 0) return { label, id, 0, (float) (numTables - 1), d, ParamKind::Choice, "", WavetableFactory::builtInLabels(), numTables };
        if (second && k == 1) return { label, id, 1, 12, d, ParamKind::Int };
        return { label, id, 0, 1, d, ParamKind::Linear };
    }
    static float srcDefault(int kind, bool second)
    {
        switch ((UnifiedVoice::OscWaveform) kind)
        {
            case UnifiedVoice::OscWaveform::Wavetable: return second ? 0.0f : 0.0f;      // SAW table, first frame
            case UnifiedVoice::OscWaveform::FM:        return second ? 2.0f : 0.3f;
            case UnifiedVoice::OscWaveform::Formant:   return second ? 0.5f : 0.3f;
            case UnifiedVoice::OscWaveform::Harmonic:  return second ? 0.0f : 0.4f;
            default:                                   return 0.0f;
        }
    }

    static const ParamPages& basePages()
    {
        using namespace UnifiedParam;
        static const char* const waves[] = { "SIN", "SAW", "SQR", "TRI", "PLS", "NOIS", "WT", "FM", "FORM", "HARM" };
        static const char* const routes[] = { "SNGL", "SER", "PAR" };
        static const char* const modes[] = { "MONO", "DUO", "POLY", "UNI" };
        static const char* const archs[] = { "INIT", "MONO", "HYBR", "POLY" };
        static const char* const stacks[] = { "OFF", "2", "4", "8" };
        static const char* const glideModes[] = { "ALL", "LEGA" };

        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;

        static const ParamPages pages = {
            { "OSC", "SRC", {{
                { "WAV1", WAVE1, 0, 9, 1, C, "", waves, 10 },
                { "WAV2", WAVE2, 0, 9, 1, C, "", waves, 10 },
                { "MIX",  MIX, 0, 1, 0.5f, L },
                { "PW",   PULSE_WIDTH, 0.05f, 0.95f, 0.5f, L },
                { "TUN2", TUNE2, -24, 24, 0, I, "st" },
                { "DET2", DETUNE2, -50, 50, 8, L, "ct" },
                { "XMOD", CROSS_MOD, 0, 1, 0, L },
                { "DRIV", MIX_DRIVE, 0, 1, 0, L } }} },
            { "OSC3", "SRC", {{
                { "WAV3", WAVE3, 0, 9, 0, C, "", waves, 10 },
                { "TUN3", TUNE3, -24, 24, -12, I, "st" },
                { "LVL3", SUB, 0, 1, 0, L },
                { "NOIS", NOISE, 0, 1, 0, L },
                { "NCOL", NOISE_COLOR, 0, 1, 0, L },
                { "TUNE", TUNE, -24, 24, 0, I, "st" },
                { "FINE", FINE, -100, 100, 0, L, "ct" },
                { "BEND", BEND_RANGE, 0, 24, 2, I, "st" } }} },
            // Filled in getParamPages(): two cells per oscillator that is a wavetable, FM, formant
            // or harmonic oscillator (empty for the analog waves).
            { "SRC", "SRC", {{ {}, {}, {}, {}, {}, {}, {}, {} }} },
            { "VOICE", "SRC", {{
                { "ARCH", ARCH, 0, 3, 0, C, "", archs, 4 },
                { "MODE", VOICE_MODE, 0, 3, 2, C, "", modes, 4 },
                { "VOX",  VOICE_COUNT, 1, MAX_VOICES, 8, I },
                { "LEGA", LEGATO, 0, 1, 0, C, "", ParamChoices::offOn, 2 },
                { "STCK", STACK, 0, 3, 0, C, "", stacks, 4 },
                { "SPRD", SPREAD, 0, 1, 0.1f, L },
                { "GLID", GLIDE, 0, 2, 0, L, "s" },
                { "GLMD", GLIDE_MODE, 0, 1, 0, C, "", glideModes, 2 } }} },
            // Everything that makes the voices imperfect, in one place: slow drift (amount, rate),
            // and what is drawn afresh for every note (pitch, start phase, waveshape, pitch
            // overshoot, cutoff, envelope times).
            { "ANALG", "SRC", {{
                { "DRFT", DRIFT, 0, 1, 0.15f, L },
                { "DRAT", DRIFT_RATE, 0.05f, 5, 0.3f, G, "Hz" },
                { "SLOP", SLOP, 0, 50, 2, L, "ct" },
                { "PHAS", RANDOM_PHASE, 0, 1, 1, C, "", ParamChoices::offOn, 2 },
                { "WSHP", WAVE_VAR, 0, 1, 0.1f, L },
                { "SOFT", SOFT_START, 0, 50, 8, L, "ct" },
                { "FVAR", FILTER_VAR, 0, 1, 0, L },
                { "EVAR", ENV_VAR, 0, 1, 0, L } }} },
            { "FILTR", "FLTR", {{
                { "CUT",  CUTOFF, 20, 20000, 8000, G, "Hz" },
                { "RESO", RESONANCE, 0, 1, 0.2f, L },
                { "ENV",  FILTER_ENV, -1, 1, 0, L },
                { "DRIV", FILTER_DRIVE, 0, 1, 0, L },
                { "TYPE", MODEL_A, 0, FilterSlot::NUM_MODELS - 1, 0, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
                { "MODE", MODE_A, 0, 5, 0, C, "", FilterSlot::modeNames(FilterModel::SVF), 4 },
                { "MORP", MORPH, 0, 1, 0, L },
                { "FFM",  FILTER_FM, 0, 1, 0, L } }} },
            { "FLT B", "FLTR", {{
                { "TYPB", MODEL_B, 0, FilterSlot::NUM_MODELS - 1, FilterSlot::NUM_MODELS - 1, C, "", FilterSlot::modelNames(), FilterSlot::NUM_MODELS },
                { "MODB", MODE_B, 0, 5, 0, C, "", FilterSlot::modeNames(FilterModel::Bypass), 1 },
                { "OFSB", FILTER_B_OFFSET, -48, 48, 0, I, "st" },
                { "RESB", RESONANCE_B, 0, 1, 0.2f, L },
                { "ROUT", ROUTING, 0, 2, 0, C, "", routes, 3 },
                { "BAL",  BALANCE, 0, 1, 0.5f, L },
                { "DRVB", DRIVE_B, 0, 1, 0, L },
                { "TRKB", B_TRACK, 0, 1, 1, L } }} },
            { "F ENV", "FLTR", {{
                { "ATK",  FENV_ATTACK, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  FENV_DECAY, 0.001f, 10, 0.5f, G, "s" },
                { "SUS",  FENV_SUSTAIN, 0, 1, 0, L },
                { "REL",  FENV_RELEASE, 0.001f, 10, 0.3f, G, "s" },
                { "CURV", FENV_CURVE, -1, 1, 0, L },
                { "KEY",  KEYTRACK, -1, 1, 0, L },
                { "VEL",  FILTER_VEL, 0, 1, 0, L },
                { "PMOD", POLY_MOD, -1, 1, 0, L } }} },
            { "AMP", "AMP", {{
                { "ATK",  AMP_ATTACK, 0.001f, 10, 0.01f, G, "s" },
                { "DEC",  AMP_DECAY, 0.001f, 10, 0.3f, G, "s" },
                { "SUS",  AMP_SUSTAIN, 0, 1, 0.7f, L },
                { "REL",  AMP_RELEASE, 0.001f, 10, 0.3f, G, "s" },
                { "CURV", AMP_CURVE, -1, 1, 0, L },
                { "VEL",  VEL_SENS, 0, 1, 0.7f, L },
                { "LEVL", LEVEL, 0, 1, 0.8f, L },
                {} }} },
        };
        return pages;
    }

    VoiceAllocator& getAllocator() { return allocator; }
    UnifiedVoice& getVoice(int index) { return voices[(size_t) juce::jlimit(0, MAX_VOICES - 1, index)]; }

    // ---- Architecture presets ----------------------------------------------------------------

    void applyArchitecturePreset(ArchitecturePresetId presetId)
    {
        const auto& preset = getArchitecturePreset(presetId);
        currentPresetId = presetId;
        flatParams[UnifiedParam::ARCH] = (float) ((int) presetId + 1);

        using namespace UnifiedParam;

        set(VOICE_MODE, (float) (int) preset.voiceMode);
        set(VOICE_COUNT, (float) preset.voiceCount);
        set(LEGATO, preset.legato ? 1.0f : 0.0f);
        set(GLIDE, preset.glideTime);
        set(GLIDE_MODE, preset.legato ? 1.0f : 0.0f);
        set(STACK, preset.unisonEnabled ? (preset.unisonCount >= 4 ? 2.0f : 1.0f) : 0.0f);
        set(SPREAD, preset.unisonEnabled ? preset.unisonSpread : 0.1f);
        set(DRIFT, preset.analogEnabled ? preset.driftAmount : 0.0f);

        set(WAVE1, (float) (int) preset.oscWaveforms[0]);
        set(WAVE2, (float) (int) preset.oscWaveforms[1]);
        set(WAVE3, (float) (int) preset.oscWaveforms[2]);
        const float l1 = preset.oscEnabled[0] ? preset.oscLevels[0] : 0.0f;
        const float l2 = preset.oscEnabled[1] ? preset.oscLevels[1] : 0.0f;
        set(MIX, (l1 + l2) > 0.0f ? l2 / (l1 + l2) : 0.0f);
        set(SUB, preset.oscEnabled[2] ? preset.oscLevels[2] : 0.0f);
        const float semis2 = std::round(preset.oscDetune[1]);
        set(TUNE2, semis2);
        set(DETUNE2, (preset.oscDetune[1] - semis2) * 100.0f);
        set(TUNE3, std::round(preset.oscDetune[2]));
        set(NOISE, preset.noiseEnabled ? preset.noiseLevel : 0.0f);
        set(PULSE_WIDTH, 0.5f);
        set(CROSS_MOD, 0.0f);
        set(MIX_DRIVE, 0.0f);
        set(TUNE, 0.0f);
        set(FINE, 0.0f);

        set(MODEL_A, (float) FilterSlot::indexFromModel(preset.filterModel));
        set(MODEL_B, (float) FilterSlot::indexFromModel(preset.filter2Model));
        set(MODE_A, 0.0f);
        set(MODE_B, 0.0f);
        set(BALANCE, 0.5f);
        set(FILTER_FM, 0.0f);
        set(DRIVE_B, 0.0f);
        set(POLY_MOD, 0.0f);
        set(B_TRACK, 1.0f);
        set(ROUTING, (float) (int) preset.filterRouting);
        set(CUTOFF, preset.filterCutoff);
        set(RESONANCE, juce::jmin(preset.filterResonance, 1.0f));
        set(RESONANCE_B, juce::jmin(preset.filterResonance, 1.0f));
        set(FILTER_DRIVE, preset.filterDrive);
        // Presets give the envelope amount in Hz above the cutoff; the parameter is in octaves.
        float octaves = std::log2(juce::jmax(20.0f, preset.filterCutoff + preset.filterEnvAmount)
                                  / juce::jmax(20.0f, preset.filterCutoff));
        set(FILTER_ENV, octaves / UnifiedVoice::FILTER_ENV_OCTAVES);
        set(KEYTRACK, preset.voiceMode == VoiceMode::Poly ? 0.5f : 0.3f);
        set(FILTER_VEL, 0.0f);
        set(MORPH, 0.0f);
        set(FILTER_B_OFFSET, 0.0f);

        set(AMP_ATTACK, preset.ampEnv.attack);
        set(AMP_DECAY, preset.ampEnv.decay);
        set(AMP_SUSTAIN, preset.ampEnv.sustain);
        set(AMP_RELEASE, preset.ampEnv.release);
        set(FENV_ATTACK, preset.filterEnv.attack);
        set(FENV_DECAY, preset.filterEnv.decay);
        set(FENV_SUSTAIN, preset.filterEnv.sustain);
        set(FENV_RELEASE, preset.filterEnv.release);

        // The architecture's mod strip route (LFO cells on MOD / MOD2).
        for (auto& [id, value] : preset.wheelRoute)
            set(id, value);

        // The STRIP page: CUT +50 %, WVIB 0.25 semitones (every ARCH preset gets these).
        // The DestTable has NO strip defaults (old projects without ids 158-165 must keep their
        // sound); the ARCH preset seeds the page when selected.  While an LFO in use has a WHL
        // amount the strip goes to those LFOs instead (the router sees strip 0 in processBlock);
        // the STRIP defaults are there for when the user clears WHL.
        setParameter(STRIP_IDS + 0, (float) UnifiedVoice::RCutoff);   // DST1 = CUT
        setParameter(STRIP_IDS + 1, 0.5f);                             // AMT1 = +50 %
        setParameter(STRIP_IDS + (mod::Router::STRIP_VIBRATO - mod::Router::STRIP_BASE), 0.25f);
    }

    ArchitecturePresetId getCurrentPresetId() const { return currentPresetId; }

    const ArchitecturePreset& getCurrentPreset() const
    {
        return getArchitecturePreset(currentPresetId);
    }

    // ---- ARCH memory for projects ---------------------------------------------------------------

    // Capture the ARCH memory as a flat list for project storage. The format:
    // [version, numSlots, then per slot: visited, numParams, (id, value) x numParams]
    std::vector<float> captureArchMemory()
    {
        // Snapshot the current slot so the project has the latest state.
        snapshotArch(archIndex());
        std::vector<float> out;
        out.push_back(1.0f);                                 // version
        out.push_back((float) ARCH_SLOTS);
        for (int s = 0; s < ARCH_SLOTS; ++s)
        {
            const auto& snap = archMemory[(size_t) s];
            out.push_back(snap.visited ? 1.0f : 0.0f);
            out.push_back((float) snap.params.size());
            for (auto& [id, val] : snap.params)
            {
                out.push_back((float) id);
                out.push_back(val);
            }
        }
        return out;
    }

    // Restore ARCH memory from a flat list (graceful on empty or old projects).
    void applyArchMemory(const std::vector<float>& v)
    {
        for (auto& s : archMemory) { s.visited = false; s.params.clear(); }
        if (v.size() < 2 || (int) std::lround(v[0]) != 1)
            return;
        const int slots = juce::jlimit(0, ARCH_SLOTS, (int) std::lround(v[1]));
        size_t at = 2;
        for (int s = 0; s < slots && at + 1 < v.size(); ++s)
        {
            const bool visited = (int) std::lround(v[at++]) != 0;
            const int count = (at < v.size()) ? juce::jlimit(0, 2048, (int) std::lround(v[at++])) : 0;
            if (s < ARCH_SLOTS)
            {
                archMemory[(size_t) s].visited = visited;
                archMemory[(size_t) s].params.clear();
            }
            for (int i = 0; i < count && at + 1 < v.size(); ++i)
            {
                const int id = (int) std::lround(v[at]);
                const float val = v[at + 1];
                at += 2;
                if (s < ARCH_SLOTS && visited)
                    archMemory[(size_t) s].params[id] = val;
            }
        }
    }

private:
    // One full-level voice peaks about where a drum hit does (measured on the device: 0.7 left the
    // synth clearly quieter than the kit).
    static constexpr float HEADROOM = 1.0f;
    static constexpr int MONO_STACK_SIZE = 16;

    void set(ParamId id, float value) { setParameter(id, value); }

    int archIndex() const { return (int) flatParams[UnifiedParam::ARCH]; }

    static const ParamSpec* findSpec(int id)
    {
        for (auto& page : basePages())
            if (auto* p = page.find(id))
                return p;
        return nullptr;
    }

    void installAllocatorCallbacks()
    {
        allocator.setNoteOnCallback([this](int voiceIdx, const Voice& v)
        {
            if (voiceIdx >= 0 && voiceIdx < (int) voices.size())
            {
                voices[(size_t) voiceIdx].setNote(v.note, v.velocity);
                voices[(size_t) voiceIdx].noteOn();
            }
        });

        allocator.setNoteOffCallback([this](int voiceIdx, const Voice& /*v*/)
        {
            if (voiceIdx >= 0 && voiceIdx < (int) voices.size())
                voices[(size_t) voiceIdx].noteOff();
        });
    }

    // Every page parameter at its default, with the voice count the engine was built with.
    void loadInitPatch()
    {
        for (auto& page : basePages())
            for (auto& p : page.params)
                if (p.isValid() && p.id != UnifiedParam::ARCH)
                    setParameter(p.id, p.id == UnifiedParam::VOICE_COUNT ? (float) initialVoiceCount : p.defaultVal);

        flatParams[UnifiedParam::ARCH] = 0.0f;

        // Modulation defaults: one LFO, no extra envelopes, nothing routed.
        flatParams[ModParam::LFO_COUNT] = 1.0f;
        flatParams[ModParam::ENV_COUNT] = 0.0f;
        for (int l = 0; l < ModParam::MAX_LFOS; ++l)
        {
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_RATE), 2.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEPTH), 0.3f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_SHAPE), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_TRIG), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_SYNC), 0.0f);
            setParameter(ModParam::lfoParamId(l, ModParam::LFO_DEST), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_DIV), (float) LFO::DEFAULT_DIVISION);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_PHASE), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_DELAY), 0.0f);
            setParameter(ModParam::lfoExtParamId(l, ModParam::LFO_FADE), 0.0f);
            for (int off : { ModParam::LFO_WHEEL, ModParam::LFO_VEL, ModParam::LFO_KTRK, ModParam::LFO_PRES })
                setParameter(ModParam::lfoExt2ParamId(l, off), 0.0f);
        }
        for (int e = 0; e < ModParam::MAX_ENVS; ++e)
        {
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_ATK)] = 0.01f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_DEC)] = 0.1f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_SUS)] = 0.7f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_REL)] = 0.3f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_DEPTH)] = 1.0f;
            flatParams[(size_t) ModParam::envParamId(e, ModParam::ENV_TARGET)] = 3.0f;
            // (The router keeps the set value of every cell it can move, ATK2 .. REL5 among them:
            // set here without setParameter, it would move them around its own default, and hand
            // that default back when a slot lets go.)
            for (int c : { ModParam::ENV_ATK, ModParam::ENV_DEC, ModParam::ENV_SUS, ModParam::ENV_REL, ModParam::ENV_DEPTH })
                router.setBase(ModParam::envParamId(e, c), flatParams[(size_t) ModParam::envParamId(e, c)]);
        }
        applyModParameter(ModParam::ENV_COUNT, 0.0f);
        applyModParameter(ModParam::LFO_COUNT, 1.0f);

        // No mod slot set, two shown, the custom shape a rising staircase; the STRIP and XIN pages
        // at zero (old projects do not store ids 158-165, so initPatch must match what they expect;
        // applyArchitecturePreset seeds the STRIP page with CUT +50 % and WVIB 0.25 for a new
        // sound).
        for (int id = ModSlotParam::BASE; id < ModSlotParam::END; ++id)
            router.set(id, 0.0f);
        for (int i = 0; i < LFO::USER_POINTS; ++i)
            router.set(LfoShapeParam::BASE + i, LFO::defaultUserLevel(i));
        router.set(ModSlotParam::COUNT, (float) mod::Router::DEFAULT_SHOWN);
        for (int id = mod::Router::STRIP_BASE; id < mod::Router::XIN_END; ++id)
            router.set(id, 0.0f);
        router.set(mod::Router::STRIP_VIBRATO_RATE, mod::Router::DEFAULT_VIBRATO_RATE);
        router.set(mod::Router::xinId(mod::Router::XinLowPass), 2000.0f);
        router.set(mod::Router::xinId(mod::Router::XinHighPass), 200.0f);
        legacyXin = 0;
        legacyXinDepth = 0.5f;
    }

    bool isMono() const { return allocator.getMode() == VoiceMode::Mono; }

    // ---- Mono: last-note priority with a held-note stack, optional legato -----------------------

    void monoNoteOn(int note, float velocity)
    {
        const bool wasHeld = monoStackCount > 0 && voices[0].isGateOpen();
        monoRemove(note);
        if (monoStackCount < MONO_STACK_SIZE)
            monoStack[(size_t) monoStackCount++] = note;

        monoVelocity = velocity;
        monoPressure = 0.0f;
        monoNote = note;

        if (legato && wasHeld)
        {
            voices[0].setNoteLegato(note);
        }
        else
        {
            voices[0].setNote(note, velocity);
            voices[0].noteOn();
        }
    }

    void monoNoteOff(int note)
    {
        monoRemove(note);
        if (note != monoNote)
            return;

        if (monoStackCount > 0)
        {
            // Fall back to the most recent key still held.
            monoNote = monoStack[(size_t) (monoStackCount - 1)];
            if (legato)
            {
                voices[0].setNoteLegato(monoNote);
            }
            else
            {
                voices[0].setNote(monoNote, monoVelocity);
                voices[0].noteOn();
            }
        }
        else
        {
            monoNote = -1;
            voices[0].noteOff();
        }
    }

    void monoRemove(int note)
    {
        int w = 0;
        for (int r = 0; r < monoStackCount; ++r)
            if (monoStack[(size_t) r] != note)
                monoStack[(size_t) w++] = monoStack[(size_t) r];
        monoStackCount = w;
    }

    // True while an LFO in use has a WHL amount: the strip then does what those say and nothing else.
    bool wheelIsRouted() const
    {
        const int lfos = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        for (int l = 0; l < lfos; ++l)
            if (flatParams[(size_t) ModParam::lfoExt2ParamId(l, ModParam::LFO_WHEEL)] != 0.0f)
                return true;
        return false;
    }

    // Per-voice pressure through the shared router, plus the mod wheel: to the LFOs' WHL amounts,
    // or as vibrato on every voice while none is set.
    void updatePressure()
    {
        const int count = juce::jmin(allocator.getVoiceCount(), (int) voices.size());
        const float wheelVibrato = wheelIsRouted() || stripPageUsed() ? 0.0f : modWheelNow * 0.5f;
        for (int i = 0; i < (int) voices.size(); ++i)
        {
            float pressure = 0.0f;
            if (isMono())
                pressure = (i == 0) ? monoPressure : 0.0f;
            else if (i < count)
                pressure = allocator.getAftertouch(i);

            // The pressure slots the voice plays itself (Router::pressureIsOwn: LEVL, CUT, XMOD,
            // PW, MIX and VIB), summed exactly as AftertouchRouter::computeModulation summed the
            // routes (P01 pins the arithmetic). Every other slot is the router's.
            float volumeMod = 0.0f, cutoffMod = 0.0f, vibratoMod = 0.0f;
            float timbreMod = 0.0f, wavePosMod = 0.0f, fmDepthMod = 0.0f;
            if (pressure > 0.0f)
            {
                for (int p = 0; p < mod::Router::NUM_PRESSURE; ++p)
                {
                    const auto slot = router.pressureSlot(p);
                    if (! router.pressureIsOwn(p))
                        continue;
                    float mod;
                    if (slot.bipolar)
                        mod = (pressure * 2.0f - 1.0f) * slot.amount;
                    else
                        mod = pressure * slot.amount;

                    switch (slot.dest)
                    {
                        case UnifiedVoice::RAmp:    volumeMod += mod; break;
                        case UnifiedVoice::RCutoff: cutoffMod += mod; break;
                        case UnifiedVoice::RPw:     timbreMod += mod; break;
                        case UnifiedVoice::RMix:    wavePosMod += mod; break;
                        case UnifiedVoice::RXmod:   fmDepthMod += mod; break;
                        default:                    vibratoMod += mod; break;      // (VIB: the only other one that is the voice's)
                    }
                }
            }

            voices[(size_t) i].setPressureMods(volumeMod, cutoffMod, vibratoMod + wheelVibrato,
                                               timbreMod, wavePosMod, fmDepthMod);
            // The LFOs' own WHL / PRES amounts take the raw wheel and this voice's raw pressure (and
            // so does the router's PRES, mod::Voice::tick).
            voices[(size_t) i].setLfoPerformance(modWheelNow, pressure);
        }
    }

    // True while the STRIP page gives the strip something to do (a route with an amount, or WVIB):
    // the built-in vibrato then steps aside, as it does for an LFO's WHL amount.
    bool stripPageUsed() const
    {
        for (int r = 0; r < mod::Router::NUM_STRIP; ++r)
            if (router.get(mod::Router::stripDestId(r)) != 0.0f && router.get(mod::Router::stripAmountId(r)) != 0.0f)
                return true;
        return router.get(mod::Router::STRIP_VIBRATO) > 0.0f;
    }

    void applyOscLevels()
    {
        const float mix = flatParams[UnifiedParam::MIX];
        const float sub = flatParams[UnifiedParam::SUB];
        const float xmod = flatParams[UnifiedParam::CROSS_MOD];
        for (auto& v : voices)
        {
            v.getOsc(0).level = 1.0f - mix;
            v.getOsc(0).enabled = mix < 0.999f;
            v.getOsc(1).level = mix;
            // Still needed as the cross-mod / filter-FM source when it is out of the mix.
            v.getOsc(1).enabled = mix > 0.001f || xmod > 0.0f || flatParams[UnifiedParam::FILTER_FM] > 0.0f
                                  || lfoNeedsOsc2();
            v.getOsc(2).level = sub;
            v.getOsc(2).enabled = sub > 0.001f;
        }
    }

    // An LFO routed to the cross-mod or filter-FM amount needs oscillator 2 running.
    bool lfoNeedsOsc2() const
    {
        const int count = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        for (int l = 0; l < count; ++l)
            if (UnifiedVoice::lfoDestNeedsOsc2(voices[0].getLfoDest(l)))
                return true;
        // (The router's slots and LFOs on XMOD / FFM: osc2ForRouter, as of the last block.)
        return osc2ForRouter || UnifiedVoice::lfoDestNeedsOsc2(voices[0].getEnvDest());
    }

    // ---- The LFOs: the router's, or the classic path -----------------------------------------
    // Every LFO cell goes to the router's LFO of the same number (setLfo: RATE, SHPE, TRIG, SYNC,
    // DIV, PHAS, DLY, FADE, KTRK), and the LFO's DEST and DPTH are the router's LFO slot - unless
    // the LFO has a WHL, VEL or PRES amount. Those add to the depth (depth + amount x source, held
    // to -1..1, in the LFO itself), which the router cannot do: it multiplies an amount by a VIA,
    // so "depth 0 plus the strip" (what every WheelRoute of a preset is, and P01's DuneDrone,
    // pinned bit for bit with the strip up) has no slot. Such an LFO runs on the voice's own LFO
    // bank and its own destinations, exactly as before (the classic path); the router's LFO of
    // that number then has no depth and no destination (a slot may still read it as a source).
    bool lfoIsClassic(int l) const
    {
        const int count = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        if (l < 0 || l >= count)
            return false;
        std::array<bool, ModParam::MAX_LFOS> classic {};
        for (int k = 0; k < count; ++k)
            for (int off : { ModParam::LFO_WHEEL, ModParam::LFO_VEL, ModParam::LFO_PRES })
                if (flatParams[(size_t) ModParam::lfoExt2ParamId(k, off)] != 0.0f)
                    classic[(size_t) k] = true;
        // An LFO aimed at another's rate or depth (RAT1 .. DPT4) acts on that LFO on its own path:
        // the classic path's RATn / DPTn move the voice's LFO bank, the router's move the router's
        // LFOs. One of the two on the classic path takes the other there too, or the move would
        // reach an LFO that is not heard (an LFO with WHL on CUT, another on its RAT1).
        for (bool grew = true; grew;)
        {
            grew = false;
            for (int k = 0; k < count; ++k)
            {
                const int d = (int) std::lround(flatParams[(size_t) ModParam::lfoParamId(k, ModParam::LFO_DEST)]);
                if (d < (int) UnifiedVoice::LfoDest::Lfo1Rate || d > (int) UnifiedVoice::LfoDest::Lfo4Depth)
                    continue;
                const int t = (d - (int) UnifiedVoice::LfoDest::Lfo1Rate) % ModParam::MAX_LFOS;
                if (t < count && classic[(size_t) k] != classic[(size_t) t])
                {
                    classic[(size_t) k] = classic[(size_t) t] = true;
                    grew = true;
                }
            }
        }
        return classic[(size_t) l];
    }

    // Where LFO l's DEST and DPTH go: the voices' classic path or the router's slot.
    void syncLfoRoute(int l)
    {
        const int count = juce::jlimit(1, ModParam::MAX_LFOS, (int) flatParams[ModParam::LFO_COUNT]);
        const bool classic = lfoIsClassic(l);
        const int stored = juce::jlimit(0, UnifiedVoice::NUM_LFO_CELL_DESTS - 1,
                                        (int) std::lround(flatParams[(size_t) ModParam::lfoParamId(l, ModParam::LFO_DEST)]));
        // A cell of an effect on the track (E4) is the router's to move, on either path: the LFO
        // then plays the router's slot (its WHL / VEL / PRES amounts, which only the classic path
        // adds to the depth, do not count on it).
        const int trackDest = stored - (int) UnifiedVoice::LfoDest::Count;
        const int dest = trackDest >= 0 ? 0 : stored;
        const bool onRouter = (! classic || trackDest >= 0) && l < count;
        router.setLfo(l, mod::Router::Depth, onRouter ? flatParams[(size_t) ModParam::lfoParamId(l, ModParam::LFO_DEPTH)] : 0.0f);
        router.setLfo(l, mod::Router::DestCell, onRouter ? (float) (trackDest >= 0 ? router.trackCellIndex(trackDest) : dest) : 0.0f);
        for (auto& v : voices)
            v.setLfoDest(l, classic ? static_cast<UnifiedVoice::LfoDest>(dest) : UnifiedVoice::LfoDest::Off);
        classicLfo[(size_t) l] = classic;
        updateRoute(l);
    }

    // Every LFO's (one LFO's DEST or amount can move another between the paths: lfoIsClassic).
    void syncLfoRoutes()
    {
        for (int l = 0; l < ModParam::MAX_LFOS; ++l)
            syncLfoRoute(l);
    }

    // ---- The old XIN cell (148, XDPT 149) from a stored sound --------------------------------
    // XIN is the XIN page now (the router's: any destination, the input raw or filtered); a stored
    // XIN / XDPT becomes its first route, as the FM engine's did:
    //   PM    the input on PM at XDPT (the same leaky sum, the same depth)
    //   SYNC  the input on SYNC at SYNC_LEVEL / threshold(XDPT): the buffer crosses SYNC_LEVEL where
    //         the input crossed the old threshold
    //   RING  the input on RING at XDPT (a slot that brings the input to RING rings: ringDry)
    // OFF takes the route away again. The cells keep what was stored.
    void setLegacyXin(int id, float value)
    {
        if (id == UnifiedParam::XIN)
            legacyXin = std::isfinite(value) ? juce::jlimit(0, (int) UnifiedVoice::NUM_XIN_MODES - 1, (int) std::lround(juce::jlimit(-1.0f, 100.0f, value))) : 0;
        else
            legacyXinDepth = std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f;
        const float d = legacyXinDepth;
        int dest = 0;
        float amount = d;
        switch (legacyXin)
        {
            case UnifiedVoice::XinPm:   dest = UnifiedVoice::RPm; break;
            case UnifiedVoice::XinRing: dest = UnifiedVoice::RRing; break;
            case UnifiedVoice::XinSync:
                dest = UnifiedVoice::RSync;
                amount = d > 0.0f ? UnifiedVoice::SYNC_LEVEL / UnifiedVoice::xinSyncThreshold(d) : 0.0f;
                break;
            default: amount = 0.0f; break;
        }
        router.set(mod::Router::xinId(mod::Router::XinDest1), (float) dest);
        router.set(mod::Router::xinId(mod::Router::XinFilter1), 0.0f);
        router.set(mod::Router::xinId(mod::Router::XinAmount1), amount);
    }

    // ---- Page cells as LFO / envelope destinations (UnifiedVoice::PARAM_DESTS) ------------------

    bool isRouted(int id) const
    {
        for (int r : routeId)
            if (r == id && id >= 0)
                return true;
        return false;
    }

    // A slot's table cell: full depth moves it as far as it did when the list was nine tables long
    // (UnifiedVoice::TABLE_ROUTE_SPAN). 0 for every other cell: its whole range.
    float tableRouteSpan(int id) const
    {
        for (int osc = 0; osc < 3; ++osc)
            if (id == UnifiedParam::srcB(osc))
            {
                const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                return kind == (int) UnifiedVoice::OscWaveform::Wavetable ? UnifiedVoice::TABLE_ROUTE_SPAN : 0.0f;
            }
        return 0.0f;
    }

    // The range and scale of a cell an LFO can be sent to, from its ParamSpec.
    void destRange(int id, float& lo, float& hi, bool& log) const
    {
        lo = 0.0f; hi = 1.0f; log = false;
        ParamSpec src;
        const ParamSpec* spec = findSpec(id);
        for (int osc = 0; osc < 3 && spec == nullptr; ++osc)
            if (id == UnifiedParam::srcA(osc) || id == UnifiedParam::srcB(osc))
            {
                const int kind = (int) std::lround(flatParams[osc == 0 ? UnifiedParam::WAVE1 : (osc == 1 ? UnifiedParam::WAVE2 : UnifiedParam::WAVE3)]);
                src = srcSpec(osc, kind, id == UnifiedParam::srcB(osc));
                if (! src.isValid())
                    return;             // an analog wave: the cells do nothing
                spec = &src;
            }
        if (spec != nullptr)
        {
            lo = spec->minVal;
            hi = spec->maxVal;
            log = spec->kind == ParamKind::Log;
            return;
        }
        // The extra envelopes' cells are built by the screen: times 1 ms .. 10 s, the rest 0..1.
        if (id >= ModParam::ENV_BASE && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            const int cell = (id - ModParam::ENV_BASE) % ModParam::ENV_STRIDE;
            if (cell == ModParam::ENV_ATK || cell == ModParam::ENV_DEC || cell == ModParam::ENV_REL)
            {
                lo = 0.001f; hi = 10.0f; log = true;
            }
        }
    }

    // An LFO's DEST as the voices' own path has it: OFF for a cell of an effect on the track (the router's).
    int voiceLfoDest(int l) const
    {
        const int stored = (int) std::lround(flatParams[(size_t) ModParam::lfoParamId(l, ModParam::LFO_DEST)]);
        return stored < (int) UnifiedVoice::LfoDest::Count ? stored : 0;
    }

    // Source 0-3 = the LFOs (their DEST), 4 = the extra envelopes: tells every voice which page
    // cell the source moves, and puts a cell it has let go of back to its set value.
    void updateRoute(int source)
    {
        // (An LFO on the router has no page cell of the classic path: the router moves it.)
        const int dest = source < ModParam::MAX_LFOS
            ? (classicLfo[(size_t) source] ? voiceLfoDest(source) : 0)
            : envDestIndex;
        const int id = UnifiedVoice::paramOfDest(dest);
        const int before = routeId[(size_t) source];
        routeId[(size_t) source] = id;
        if (id >= 0)
        {
            float lo, hi; bool log;
            destRange(id, lo, hi, log);
            for (auto& v : voices)
                v.setParamRoute(source, id, flatParams[(size_t) id], lo, hi, log, tableRouteSpan(id));
        }
        else
        {
            for (auto& v : voices)
                v.setParamRoute(source, -1);
        }
        if (before >= 0 && before != id && ! isRouted(before))
            applyParameter(before, flatParams[(size_t) before]);
    }

    void refreshRoutes()
    {
        for (int s = 0; s < UnifiedVoice::NUM_MOD_SOURCES; ++s)
            if (routeId[(size_t) s] >= 0 || s < ModParam::MAX_LFOS)
                updateRoute(s);
    }

    void applyUnison()
    {
        const bool unison = allocator.getMode() == VoiceMode::Unison;
        const int n = allocator.getVoiceCount();
        const float spread = flatParams[UnifiedParam::SPREAD];
        unisonGain = (unison && n > 1) ? 1.0f / std::sqrt((float) n) : 1.0f;
        for (int i = 0; i < (int) voices.size(); ++i)
        {
            float detune = 0.0f;
            if (unison && n > 1 && i < n)
                detune = (((float) i / (float) (n - 1)) * 2.0f - 1.0f) * spread * 0.5f;
            voices[(size_t) i].setUnisonDetune(detune);
        }
    }

    void applyEnvelope(bool filter)
    {
        using namespace UnifiedParam;
        Envelope::Params p;
        p.attack = flatParams[filter ? FENV_ATTACK : AMP_ATTACK];
        p.decay = flatParams[filter ? FENV_DECAY : AMP_DECAY];
        p.sustain = flatParams[filter ? FENV_SUSTAIN : AMP_SUSTAIN];
        p.release = flatParams[filter ? FENV_RELEASE : AMP_RELEASE];
        stageCurves.applyTo(p, filter ? 1 : 0, flatParams[filter ? FENV_CURVE : AMP_CURVE]);
        for (auto& v : voices)
        {
            if (filter) v.setFilterEnvParams(p);
            else        v.setAmpEnvParams(p);
        }
    }

    void applyParameter(ParamId id, float value)
    {
        using namespace UnifiedParam;
        const int iv = (int) std::lround(value);

        switch (id)
        {
            case WAVE1: case WAVE2: case WAVE3:
            {
                int osc = (id == WAVE1) ? 0 : (id == WAVE2 ? 1 : 2);
                const int kind = juce::jlimit(0, UnifiedVoice::NUM_WAVEFORMS - 1, iv);
                auto w = static_cast<UnifiedVoice::OscWaveform>(kind);
                // A different kind of oscillator starts from that kind's own two settings.
                if (voices[0].getOsc(osc).waveform != w && !UnifiedVoice::isAnalog(w))
                {
                    flatParams[(size_t) srcA(osc)] = srcDefault(kind, false);
                    flatParams[(size_t) srcB(osc)] = srcDefault(kind, true);
                }
                for (auto& v : voices)
                {
                    v.getOsc(osc).waveform = w;
                    v.setSrcCell(srcA(osc), flatParams[(size_t) srcA(osc)]);
                    v.setSrcCell(srcB(osc), flatParams[(size_t) srcB(osc)]);
                }
                break;
            }
            // The cells that are simply one value in every voice: UnifiedVoice::setParam (which is
            // also what an LFO routed to the cell calls, for its own voice).
            case SRC_A1: case SRC_A2: case SRC_A3: case SRC_B1: case SRC_B2: case SRC_B3:
                for (auto& v : voices) v.setSrcCell(id, value);     // (the set value: the router moves around it)
                break;
            case TUNE3: case MIX_DRIVE: case NOISE_COLOR: case GLIDE: case DRIFT: case DRIFT_RATE:
            case FILTER_ENV: case FILTER_DRIVE: case MORPH: case RESONANCE_B: case BALANCE: case DRIVE_B:
            case B_TRACK: case KEYTRACK: case FILTER_VEL: case POLY_MOD: case VEL_SENS:
                for (auto& v : voices) v.setParam(id, value);
                break;
            case MIX: case SUB: case CROSS_MOD:
                applyOscLevels();
                if (id == CROSS_MOD)
                    for (auto& v : voices) v.setCrossMod(value);
                break;
            case TUNE: case FINE:
                for (auto& v : voices) v.setTune(flatParams[TUNE] + flatParams[FINE] / 100.0f);
                break;
            case TUNE2: case DETUNE2:
                for (auto& v : voices)
                {
                    v.setParam(TUNE2, flatParams[TUNE2]);
                    v.setParam(DETUNE2, flatParams[DETUNE2]);
                }
                break;
            case NOISE:        for (auto& v : voices) v.setNoiseLevel(value); break;
            case PULSE_WIDTH:  for (auto& v : voices) v.setPulseWidth(value); break;
            case GLIDE_MODE:
                for (auto& v : voices)
                    v.setGlideMode(iv > 0 ? UnifiedVoice::GlideMode::Legato : UnifiedVoice::GlideMode::Always);
                break;
            case BEND_RANGE:   pitchBend(bendAmount); break;

            case CUTOFF:       for (auto& v : voices) v.setFilterBaseCutoff(value); break;
            case RESONANCE:    for (auto& v : voices) v.setFilterResonanceA(value); break;
            case FILTER_B_OFFSET: for (auto& v : voices) v.setFilterBOffset(value); break;
            case FILTER_FM:
                for (auto& v : voices) v.setFilterFM(value);
                applyOscLevels();      // oscillator 2 must run while it modulates the filter
                break;
            case MODEL_A: case MODEL_B:
            {
                const int slot = (id == MODEL_A) ? 0 : 1;
                const auto model = FilterSlot::modelFromIndex(iv);
                // Keep the stored mode inside the new type's range so the cell and the sound agree.
                const int modeId = (slot == 0) ? MODE_A : MODE_B;
                flatParams[(size_t) modeId] = (float) juce::jmin((int) flatParams[(size_t) modeId],
                                                                 FilterSlot::modeCount(model) - 1);
                for (auto& v : voices)
                {
                    v.setFilterModel(slot, model);
                    v.setFilterMode(slot, (int) flatParams[(size_t) modeId]);
                }
                break;
            }
            case MODE_A: case MODE_B:
                for (auto& v : voices) v.setFilterMode(id == MODE_A ? 0 : 1, iv);
                break;
            case ROUTING:
                for (auto& v : voices) v.setFilterRouting(static_cast<FilterRouting>(juce::jlimit(0, 2, iv)));
                break;

            case AMP_ATTACK: case AMP_DECAY: case AMP_SUSTAIN: case AMP_RELEASE: case AMP_CURVE:
                applyEnvelope(false);
                break;
            case FENV_ATTACK: case FENV_DECAY: case FENV_SUSTAIN: case FENV_RELEASE: case FENV_CURVE:
                applyEnvelope(true);
                break;
            case LEVEL:
                outputLevel = value;
                for (auto& v : voices)
                    v.setLevel(outputLevel);
                break;

            case ARCH:
            {
                const int newSlot = juce::jlimit(0, ARCH_SLOTS - 1, iv);
                // The architecture's starting sound (so that every cell has a sensible value),
                // then, where this ARCH was left earlier, the sound it was left with. (The sound
                // being left now was kept in setParameter(), before ARCH itself changed.)
                if (newSlot <= 0)
                    loadInitPatch();
                else
                    applyArchitecturePreset(static_cast<ArchitecturePresetId>(
                        juce::jlimit(0, (int) ArchitecturePresetId::Count - 1, newSlot - 1)));
                restoreArch(newSlot);       // (nothing for an ARCH never left)
                break;
            }
            case VOICE_MODE:
            {
                auto m = static_cast<VoiceMode>(juce::jlimit(0, 3, iv));
                if (m != allocator.getMode())
                {
                    allNotesOff();
                    allocator.setMode(m);
                }
                applyUnison();
                break;
            }
            case VOICE_COUNT:
            {
                int n = juce::jlimit(1, MAX_VOICES, iv);
                if (n != allocator.getVoiceCount())
                {
                    allNotesOff();
                    allocator.setVoiceCount(n);
                }
                applyUnison();
                break;
            }
            case LEGATO: legato = iv > 0; break;
            case STACK:  for (auto& v : voices) v.setWaveStackMode(iv); break;
            case SPREAD:
                for (auto& v : voices) v.setWaveStackDetune(value);
                applyUnison();
                break;
            case SLOP:         for (auto& v : voices) v.getAnalogCharacter().slopCents = value; break;
            case RANDOM_PHASE: for (auto& v : voices) v.getAnalogCharacter().randomPhase = iv > 0; break;
            // A component tolerance over the first part of the cell (0.3 % at the default, as it
            // always was), growing into a difference that is heard from note to note (15 % at the top:
            // up to 0.03 the whole cell could not be heard at all).
            case WAVE_VAR:     for (auto& v : voices) v.getAnalogCharacter().waveshapeVar = value * (0.03f + 0.12f * value * value); break;
            case SOFT_START:   for (auto& v : voices) v.getAnalogCharacter().softStartCents = value; break;
            case FILTER_VAR:   for (auto& v : voices) v.getAnalogCharacter().filterVariance = value; break;
            case ENV_VAR:      for (auto& v : voices) v.getAnalogCharacter().envVariance = value; break;
            default:
                applyModParameter(id, value);
                break;
        }
    }

    void applyModParameter(ParamId id, float value)
    {
        // LFO params: 40 + lfoIdx*6 + offset
        if (id >= ModParam::LFO_BASE
            && id < ModParam::LFO_BASE + ModParam::MAX_LFOS * ModParam::LFO_STRIDE)
        {
            int offset = id - ModParam::LFO_BASE;
            int lfoIdx = offset / ModParam::LFO_STRIDE;
            int param = offset % ModParam::LFO_STRIDE;

            for (auto& v : voices)
            {
                auto& lfo = v.getLFOBank().getLFO(lfoIdx);
                switch (param)
                {
                    case ModParam::LFO_RATE:  lfo.setRate(value); break;
                    case ModParam::LFO_DEPTH: lfo.setDepth(value); break;
                    case ModParam::LFO_SHAPE:
                        lfo.setShape(static_cast<LFO::Shape>(juce::jlimit(0, (int) LFO::Shape::COUNT - 1, (int) std::lround(value))));
                        break;
                    case ModParam::LFO_TRIG:
                        lfo.setTriggerMode(static_cast<LFO::TriggerMode>(
                            juce::jlimit(0, (int) LFO::TriggerMode::COUNT - 1, (int) std::lround(value))));
                        break;
                    case ModParam::LFO_SYNC:
                        lfo.setTempo(tempoBpm);
                        lfo.setTempoSync(value > 0.5f);
                        break;
                    default: break;     // (DEST: syncLfoRoute)
                }
            }
            switch (param)
            {
                case ModParam::LFO_RATE:  router.setLfo(lfoIdx, mod::Router::Rate, value); break;
                case ModParam::LFO_SHAPE: router.setLfo(lfoIdx, mod::Router::Shape, value); break;
                case ModParam::LFO_TRIG:
                    router.setLfo(lfoIdx, mod::Router::Trigger,
                                  (float) juce::jlimit(0, (int) LFO::TriggerMode::COUNT - 1, (int) std::lround(value)));
                    break;
                case ModParam::LFO_SYNC:  router.setLfo(lfoIdx, mod::Router::Sync, value); break;
                default: break;
            }
            if (param == ModParam::LFO_DEST)
                flatParams[(size_t) id] = (float) juce::jlimit(0, UnifiedVoice::NUM_LFO_CELL_DESTS - 1, (int) std::lround(value));
            if (param == ModParam::LFO_DEST || param == ModParam::LFO_DEPTH)
            {
                syncLfoRoutes();        // the classic path or the router's slot; a page cell as the destination
                applyOscLevels();       // oscillator 2 may now be needed (or no longer)
            }
            return;
        }

        // More LFO params: 130 + lfoIdx*4 + offset
        if (id >= ModParam::LFO_EXT_BASE && id < ModParam::LFO_EXT_END)
        {
            int offset = id - ModParam::LFO_EXT_BASE;
            int lfoIdx = offset / ModParam::LFO_EXT_STRIDE;
            int param = offset % ModParam::LFO_EXT_STRIDE;

            for (auto& v : voices)
            {
                auto& lfo = v.getLFOBank().getLFO(lfoIdx);
                switch (param)
                {
                    case ModParam::LFO_DIV:
                        lfo.setTempo(tempoBpm);
                        lfo.setSyncDivision((int) std::lround(value));
                        break;
                    case ModParam::LFO_PHASE: lfo.setStartPhase(juce::jlimit(0.0f, 1.0f, value)); break;
                    case ModParam::LFO_DELAY: lfo.setDelayTime(value); break;
                    case ModParam::LFO_FADE:  lfo.setFadeTime(value); break;
                    default: break;
                }
            }
            switch (param)
            {
                case ModParam::LFO_DIV:   router.setLfo(lfoIdx, mod::Router::Division, value); break;
                case ModParam::LFO_PHASE: router.setLfo(lfoIdx, mod::Router::Phase, value); break;
                case ModParam::LFO_DELAY: router.setLfo(lfoIdx, mod::Router::Delay, value); break;
                case ModParam::LFO_FADE:  router.setLfo(lfoIdx, mod::Router::Fade, value); break;
                default: break;
            }
            return;
        }

        // Extra ENV params: 64 + envIdx*6 + offset
        if (id >= ModParam::ENV_BASE
            && id < ModParam::ENV_BASE + ModParam::MAX_ENVS * ModParam::ENV_STRIDE)
        {
            int offset = id - ModParam::ENV_BASE;
            int envIdx = offset / ModParam::ENV_STRIDE;
            int param = offset % ModParam::ENV_STRIDE;

            for (auto& v : voices)
            {
                auto& chain = v.getEnvelopeChain();
                // Depth and target belong to the whole chain; the stage settings need the stage.
                if (param == ModParam::ENV_DEPTH) { chain.setDepth(value); continue; }
                if (param == ModParam::ENV_TARGET)
                {
                    // 0-3: the envelopes' own targets. Beyond: one of the LFOs' destinations.
                    const int t = juce::jlimit(0, UnifiedVoice::NUM_ENV_DESTS - 1, (int) std::lround(value));
                    chain.setTarget(t < UnifiedVoice::NUM_ENV_TARGETS ? static_cast<EnvelopeChain::Target>(t)
                                                                      : EnvelopeChain::Target::Custom);
                    v.setEnvDest(UnifiedVoice::lfoDestOfEnvDest(t));
                    continue;
                }
                if (envIdx >= chain.getStageCount())
                    continue;
                auto& env = chain.getEnvelopeMut(envIdx);
                auto p = env.getParams();
                switch (param)
                {
                    case ModParam::ENV_ATK:    p.attack = value; env.setParams(p); break;
                    case ModParam::ENV_DEC:    p.decay = value; env.setParams(p); break;
                    case ModParam::ENV_SUS:    p.sustain = value; env.setParams(p); break;
                    case ModParam::ENV_REL:    p.release = value; env.setParams(p); break;
                    default: break;
                }
            }
            if (param == ModParam::ENV_TARGET)
            {
                envDestIndex = (int) voices[0].getEnvDest();
                updateRoute(UnifiedVoice::ENV_SOURCE);
                applyOscLevels();
            }
            return;
        }

        // LFO performance controls: 174 + lfoIdx*4 + offset
        if (id >= ModParam::LFO_EXT2_BASE && id < ModParam::LFO_EXT2_END)
        {
            int offset = id - ModParam::LFO_EXT2_BASE;
            int lfoIdx = offset / ModParam::LFO_EXT2_STRIDE;
            int param = offset % ModParam::LFO_EXT2_STRIDE;
            const float amount = juce::jlimit(-1.0f, 1.0f, value);

            for (auto& v : voices)
            {
                switch (param)
                {
                    case ModParam::LFO_WHEEL: v.setLfoWheelAmount(lfoIdx, amount); break;
                    case ModParam::LFO_VEL:   v.setLfoVelocityAmount(lfoIdx, amount); break;
                    case ModParam::LFO_KTRK:  v.getLFOBank().getLFO(lfoIdx).setKeyTrack(amount); break;
                    case ModParam::LFO_PRES:  v.setLfoPressureAmount(lfoIdx, amount); break;
                    default: break;
                }
            }
            if (param == ModParam::LFO_KTRK)
                router.setLfo(lfoIdx, mod::Router::KeyTrack, amount);
            else
            {
                syncLfoRoutes();        // an amount set or cleared moves the LFO between the paths
                applyOscLevels();
            }
            return;
        }

        // LFO count change
        if (id == ModParam::LFO_COUNT)
        {
            int count = juce::jlimit(1, ModParam::MAX_LFOS, (int)value);
            for (auto& v : voices)
            {
                auto& bank = v.getLFOBank();
                while (bank.getActiveCount() < count)
                    bank.addLFO();
                while (bank.getActiveCount() > count)
                    bank.removeLFO();
            }
            for (int l = 0; l < ModParam::MAX_LFOS; ++l)
                syncLfoRoute(l);
            applyOscLevels();
            return;
        }

        // ENV count change
        if (id == ModParam::ENV_COUNT)
        {
            int count = juce::jlimit(0, ModParam::MAX_ENVS, (int)value);
            for (auto& v : voices)
            {
                auto& chain = v.getEnvelopeChain();
                while (chain.getStageCount() < count)
                {
                    Envelope::Params p;
                    chain.addEnvelope(p);
                }
                while (chain.getStageCount() > count)
                    chain.removeLastEnvelope();
            }
            // New stages take the stored settings.
            for (int e = 0; e < count; ++e)
            {
                for (int p = ModParam::ENV_ATK; p <= ModParam::ENV_TARGET; ++p)
                    applyModParameter(ModParam::envParamId(e, p), flatParams[(size_t) ModParam::envParamId(e, p)]);
                applyExtraEnvCurves(e);
            }
            return;
        }
    }

public:
    void setTempo(float bpm) override
    {
        tempoBpm = bpm;
        router.setTempo(bpm);
        for (auto& v : voices)
            v.getLFOBank().setTempo(bpm);
    }

private:
    double sampleRate = 44100.0;
    float outputLevel = 0.8f;
    float unisonGain = 1.0f;
    float tempoBpm = 120.0f;
    float bendAmount = 0.0f;
    float modWheelValue = 0.0f;
    float modWheelNow = 0.0f;       // smoothed per block
    bool legato = false;
    int initialVoiceCount = 8;
    // The old XIN / XDPT cells as a stored sound last gave them (setLegacyXin)
    int legacyXin = UnifiedVoice::XinOff;
    float legacyXinDepth = 0.5f;

    VoiceAllocator allocator;
    std::vector<UnifiedVoice> voices;
    std::array<float, UnifiedParam::MAX_ID> flatParams {};
    ArchitecturePresetId currentPresetId = ArchitecturePresetId::MonoVA;

    // ---- Architecture memory: each ARCH keeps what was dialled in on it ---------------------------
    // (Owner, 2026-11-14: switching away and back must lose nothing.) Index 0 = INIT, 1-3 = the
    // three presets. An ARCH that was left comes back as it was left instead of as its starting
    // sound. THE one memory: it is stored with the project (captureArchMemory / applyArchMemory),
    // so a session that goes on and a project that is loaded again bring back the same things.
    // (The PARAM screen had a second one, which kept more than this one did; this one keeps
    // what that one kept.)
    static constexpr int ARCH_SLOTS = 1 + (int) ArchitecturePresetId::Count;
    struct ArchSnapshot { bool visited = false; std::map<int, float> params; };
    std::array<ArchSnapshot, ARCH_SLOTS> archMemory {};

    // The sound as it stands into an ARCH's slot: every cell of the engine but ARCH itself - the
    // pages, the LFOs and envelopes, the router's (the slots, LSHP, STRIP, XIN, the pressure
    // slots), the stage curves. (Message thread: ARCH is a rebuild cell, never a step lock.)
    void snapshotArch(int slot)
    {
        if (slot < 0 || slot >= ARCH_SLOTS)
            return;
        auto& snap = archMemory[(size_t) slot];
        snap.visited = true;
        snap.params.clear();
        for (int id = 0; id < EngineParamRange::END; ++id)
            if (EngineParamRange::contains(id) && id != UnifiedParam::ARCH)
                snap.params[id] = getParameter(id);
    }

    // An ARCH's slot back into the sound: every cell that is not what the slot holds. (A few
    // passes: a cell such as an oscillator's kind sets others. A memory from a project saved
    // before the slot held everything holds the pages and the LFOs' cells: the rest is then the
    // architecture's starting sound, as it was when that project was saved.)
    void restoreArch(int slot)
    {
        if (slot < 0 || slot >= ARCH_SLOTS || !archMemory[(size_t) slot].visited)
            return;
        for (int pass = 0; pass < 3; ++pass)
        {
            bool changed = false;
            for (auto& [id, val] : archMemory[(size_t) slot].params)
                if (id != UnifiedParam::ARCH && getParameter(id) != val)
                {
                    setParameter(id, val);
                    changed = true;
                }
            if (! changed)
                break;
        }
    }

    // The page cell each modulation source (LFO 1-4, the extra envelopes) is routed to, -1 = none.
    std::array<int, UnifiedVoice::NUM_MOD_SOURCES> routeId { { -1, -1, -1, -1, -1 } };
    int envDestIndex = 0;       // the extra envelopes' destination as a UnifiedVoice::LfoDest index
    // The modulation router (ModRouter.h): four LFOs, the four extra envelopes as ENV2 .. ENV5
    mod::Router router { routerDests(), ModParam::MAX_LFOS, ModParam::MAX_ENVS };
    std::array<bool, ModParam::MAX_LFOS> classicLfo {};     // the LFO runs the classic path (lfoIsClassic)
    bool osc2ForRouter = false;                             // the router moves XMOD or FFM
    bool inputHeard = false;                                // a route reads the track input, and there is one
    // Each voice's samples of the sub-block and where it sits, before they are mixed
    std::array<std::array<float, mod::Router::MAX_CHUNK>, MAX_VOICES> voiceOut {}, voicePan {};
    std::array<int, MAX_VOICES> made {};

    std::array<int, MONO_STACK_SIZE> monoStack {};
    int monoStackCount = 0;
    int monoNote = -1;
    float monoVelocity = 1.0f;
    float monoPressure = 0.0f;
};
